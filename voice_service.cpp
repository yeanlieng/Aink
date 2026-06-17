#include "voice_service.h"

#include "ai_model_config.h"
#include "app_locale.h"
#include "settings_api.h"

#include <Arduino.h>
#include <ESP_I2S.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define VOICE_MIC_PDM_CLK_PIN       42
#define VOICE_MIC_PDM_DATA_PIN      41
#define VOICE_SAMPLE_RATE           16000U
#define VOICE_SAMPLE_BYTES          2U
#define VOICE_MAX_RECORD_SECONDS    20U
#define VOICE_MAX_PCM_BYTES         (VOICE_SAMPLE_RATE * VOICE_SAMPLE_BYTES * VOICE_MAX_RECORD_SECONDS)
#define VOICE_WAV_HEADER_BYTES      44U
#define VOICE_HTTP_TIMEOUT_MS       60000
#define VOICE_TASK_STACK_BYTES      12288
#define VOICE_RECORD_TASK_STACK_BYTES 4096
#define VOICE_MIMO_ASR_MODEL        "mimo-v2.5-asr"
#define VOICE_MIMO_FALLBACK_MODEL   "mimo-v2.5"
#define VOICE_RECORDING_DIAGNOSTIC_ONLY 0
#define VOICE_SERIAL_WAV_DUMP       0
#define VOICE_CAPTURE_READ_BYTES    1024U
#define VOICE_AUDIO_PREROLL_DISCARD_MS 200U
#define VOICE_AUDIO_TARGET_RMS      4500.0
#define VOICE_AUDIO_MAX_GAIN        10.0
#define VOICE_AUDIO_CLIP_LIMIT      30000.0

static const char *kVoiceMimoUrls[] = {
    "https://api.xiaomimimo.com/v1/chat/completions",
    "https://token-plan-cn.xiaomimimo.com/v1/chat/completions",
    "https://token-plan-sgp.xiaomimimo.com/v1/chat/completions",
    "https://token-plan-ams.xiaomimimo.com/v1/chat/completions",
};

static I2SClass s_i2s;
static bool s_i2sReady = false;
static uint8_t *s_pcm = nullptr;
static size_t s_pcmLen = 0;
static uint8_t *s_jobWav = nullptr;
static size_t s_jobWavLen = 0;
static TaskHandle_t s_task = nullptr;
static TaskHandle_t s_recordTask = nullptr;
static volatile bool s_recordStopRequested = false;
static bool s_localSeedCaptureActive = false;
static portMUX_TYPE s_voiceMux = portMUX_INITIALIZER_UNLOCKED;
static VoiceState s_state = VOICE_STATE_IDLE;
static bool s_dirty = false;
static char s_transcript[256];
static char s_result[384];
static char s_error[96];
static unsigned long s_recordStartedMs = 0;
static unsigned long s_speakingStartedMs = 0;
static unsigned long s_speakingDurationMs = 0;

static void voice_pipeline_task(void *param);
static void voice_recording_task(void *param);

static bool isZeroIp(const IPAddress &ip) {
  return ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0;
}

static void copyText(char *dst, size_t dstLen, const char *src) {
  if (dst == nullptr || dstLen == 0) {
    return;
  }
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  size_t i = 0;
  while (i + 1 < dstLen && src[i] != '\0') {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static void setState(VoiceState state) {
  portENTER_CRITICAL(&s_voiceMux);
  s_state = state;
  s_dirty = true;
  portEXIT_CRITICAL(&s_voiceMux);
}

static void setError(const char *message) {
  portENTER_CRITICAL(&s_voiceMux);
  copyText(s_error, sizeof(s_error), message);
  s_state = VOICE_STATE_ERROR;
  s_dirty = true;
  portEXIT_CRITICAL(&s_voiceMux);
  Serial.printf("[Voice] error: %s\r\n", message != nullptr ? message : "");
}

static bool appendChar(char *buf, size_t bufLen, size_t *pos, char c) {
  if (*pos + 1 >= bufLen) {
    return false;
  }
  buf[*pos] = c;
  (*pos)++;
  buf[*pos] = '\0';
  return true;
}

static bool appendStr(char *buf, size_t bufLen, size_t *pos, const char *text) {
  if (text == nullptr) {
    return true;
  }
  while (*text != '\0') {
    if (!appendChar(buf, bufLen, pos, *text)) {
      return false;
    }
    text++;
  }
  return true;
}

static int jsonHexNibble(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

static bool parseJsonHex4(const String &body, int offset, uint16_t *out) {
  if (out == nullptr || offset + 3 >= body.length()) {
    return false;
  }
  uint16_t value = 0;
  for (int i = 0; i < 4; i++) {
    const int nibble = jsonHexNibble(body.charAt(offset + i));
    if (nibble < 0) {
      return false;
    }
    value = (uint16_t)((value << 4) | (uint16_t)nibble);
  }
  *out = value;
  return true;
}

static bool appendUtf8Codepoint(char *buf, size_t bufLen, size_t *pos, uint32_t cp) {
  if (cp <= 0x7F) {
    return appendChar(buf, bufLen, pos, (char)cp);
  }
  if (cp <= 0x7FF) {
    return appendChar(buf, bufLen, pos, (char)(0xC0 | (cp >> 6))) &&
           appendChar(buf, bufLen, pos, (char)(0x80 | (cp & 0x3F)));
  }
  if (cp >= 0xD800 && cp <= 0xDFFF) {
    return appendChar(buf, bufLen, pos, '?');
  }
  if (cp <= 0xFFFF) {
    return appendChar(buf, bufLen, pos, (char)(0xE0 | (cp >> 12))) &&
           appendChar(buf, bufLen, pos, (char)(0x80 | ((cp >> 6) & 0x3F))) &&
           appendChar(buf, bufLen, pos, (char)(0x80 | (cp & 0x3F)));
  }
  if (cp <= 0x10FFFF) {
    return appendChar(buf, bufLen, pos, (char)(0xF0 | (cp >> 18))) &&
           appendChar(buf, bufLen, pos, (char)(0x80 | ((cp >> 12) & 0x3F))) &&
           appendChar(buf, bufLen, pos, (char)(0x80 | ((cp >> 6) & 0x3F))) &&
           appendChar(buf, bufLen, pos, (char)(0x80 | (cp & 0x3F)));
  }
  return appendChar(buf, bufLen, pos, '?');
}

static bool appendJsonEscaped(char *buf, size_t bufLen, size_t *pos, const char *text) {
  if (text == nullptr) {
    return true;
  }
  while (*text != '\0') {
    const char c = *text;
    if (c == '\\' || c == '"') {
      if (!appendChar(buf, bufLen, pos, '\\')) {
        return false;
      }
    }
    if (c == '\n') {
      if (!appendStr(buf, bufLen, pos, "\\n")) {
        return false;
      }
    } else if (!appendChar(buf, bufLen, pos, c)) {
      return false;
    }
    text++;
  }
  return true;
}

static bool parseJsonStringField(const String &body, const char *fieldKey, char *out, size_t outLen) {
  if (out == nullptr || outLen == 0 || fieldKey == nullptr) {
    return false;
  }
  out[0] = '\0';

  char search[32];
  snprintf(search, sizeof(search), "\"%s\"", fieldKey);
  int idx = body.indexOf(search);
  if (idx < 0) {
    return false;
  }
  idx += (int)strlen(search);
  while (idx < body.length()) {
    const char c = body.charAt(idx);
    if (c == ':') {
      idx++;
      break;
    }
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') {
      return false;
    }
    idx++;
  }
  while (idx < body.length()) {
    const char c = body.charAt(idx);
    if (c == '"') {
      idx++;
      break;
    }
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') {
      return false;
    }
    idx++;
  }

  size_t w = 0;
  for (int i = idx; i < body.length() && w + 1 < outLen; i++) {
    const char c = body.charAt(i);
    if (c == '"' && body.charAt(i - 1) != '\\') {
      break;
    }
    if (c == '\\' && i + 1 < body.length()) {
      const char next = body.charAt(i + 1);
      if (next == 'n') {
        if (!appendChar(out, outLen, &w, '\n')) {
          break;
        }
        i++;
        continue;
      }
      if (next == 'r') {
        if (!appendChar(out, outLen, &w, '\r')) {
          break;
        }
        i++;
        continue;
      }
      if (next == 't') {
        if (!appendChar(out, outLen, &w, '\t')) {
          break;
        }
        i++;
        continue;
      }
      if (next == 'b') {
        i++;
        continue;
      }
      if (next == 'f') {
        i++;
        continue;
      }
      if (next == '\\' || next == '"') {
        if (!appendChar(out, outLen, &w, next)) {
          break;
        }
        i++;
        continue;
      }
      if (next == 'u') {
        uint16_t first = 0;
        if (parseJsonHex4(body, i + 2, &first)) {
          uint32_t cp = first;
          int consumed = 5;
          if (first >= 0xD800 && first <= 0xDBFF &&
              i + 11 < body.length() &&
              body.charAt(i + 6) == '\\' &&
              body.charAt(i + 7) == 'u') {
            uint16_t second = 0;
            if (parseJsonHex4(body, i + 8, &second) &&
                second >= 0xDC00 && second <= 0xDFFF) {
              cp = 0x10000UL + (((uint32_t)first - 0xD800UL) << 10) +
                   ((uint32_t)second - 0xDC00UL);
              consumed = 11;
            }
          }
          if (!appendUtf8Codepoint(out, outLen, &w, cp)) {
            break;
          }
          i += consumed;
          continue;
        }
      }
    }
    if (!appendChar(out, outLen, &w, c)) {
      break;
    }
  }
  out[w] = '\0';
  return w > 0;
}

static void trimText(char *text) {
  if (text == nullptr) {
    return;
  }
  size_t start = 0;
  while (text[start] == ' ' || text[start] == '\r' || text[start] == '\n' || text[start] == '\t') {
    start++;
  }
  if (start > 0) {
    memmove(text, text + start, strlen(text + start) + 1);
  }
  size_t end = strlen(text);
  while (end > 0) {
    const char c = text[end - 1];
    if (c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '"' || c == '\'') {
      end--;
    } else {
      break;
    }
  }
  text[end] = '\0';
}

static bool httpPostJson(const char *url, const char *apiKey, char *jsonBody, String *outResponse,
                         int *outHttpCode) {
  if (url == nullptr || apiKey == nullptr || jsonBody == nullptr || outResponse == nullptr ||
      outHttpCode == nullptr) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(VOICE_HTTP_TIMEOUT_MS);
  client.setHandshakeTimeout(30);

  char host[96];
  host[0] = '\0';
  const char *hostStart = strstr(url, "://");
  hostStart = hostStart != nullptr ? hostStart + 3 : url;
  const char *hostEnd = strchr(hostStart, '/');
  const size_t hostLen = hostEnd != nullptr ? (size_t)(hostEnd - hostStart) : strlen(hostStart);
  if (hostLen > 0 && hostLen < sizeof(host)) {
    memcpy(host, hostStart, hostLen);
    host[hostLen] = '\0';
    IPAddress ip;
    if (WiFi.hostByName(host, ip) && !isZeroIp(ip)) {
      Serial.printf("[Voice] HTTPS host %s -> %s\r\n", host, ip.toString().c_str());
    } else {
      Serial.printf("[Voice] HTTPS host %s DNS failed\r\n", host);
      *outHttpCode = -1;
      *outResponse = "DNS failed";
      return false;
    }

    Serial.printf("[Voice] TLS heap before connect heap=%u maxAlloc=%u internal=%u\r\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  }

  HTTPClient http;
  if (!http.begin(client, url)) {
    *outHttpCode = -1;
    *outResponse = "begin failed";
    return false;
  }

  char bearer[160];
  snprintf(bearer, sizeof(bearer), "Bearer %s", apiKey);
  http.setConnectTimeout(30000);
  http.setTimeout(VOICE_HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");
  http.addHeader("api-key", apiKey);
  http.addHeader("Authorization", bearer);

  *outHttpCode = http.POST(reinterpret_cast<uint8_t *>(jsonBody), strlen(jsonBody));
  if (*outHttpCode > 0) {
    *outResponse = http.getString();
  } else {
    *outResponse = http.errorToString(*outHttpCode);
    char tlsError[96];
    const int tlsCode = client.lastError(tlsError, sizeof(tlsError));
    Serial.printf("[Voice] TLS lastError=%d %s\r\n", tlsCode, tlsError);
  }
  http.end();
  return *outHttpCode > 0;
}

static bool httpPostMimoJson(const char *apiKey, char *jsonBody, String *outResponse, int *outHttpCode) {
  if (outResponse != nullptr) {
    *outResponse = "";
  }
  if (outHttpCode != nullptr) {
    *outHttpCode = 0;
  }

  String response;
  int httpCode = 0;
  for (size_t i = 0; i < sizeof(kVoiceMimoUrls) / sizeof(kVoiceMimoUrls[0]); i++) {
    Serial.printf("[Voice] MiMo endpoint %u/%u\r\n",
                  (unsigned)(i + 1),
                  (unsigned)(sizeof(kVoiceMimoUrls) / sizeof(kVoiceMimoUrls[0])));
    Serial.flush();
    const bool posted = httpPostJson(kVoiceMimoUrls[i], apiKey, jsonBody, &response, &httpCode);
    if (posted && httpCode >= 200 && httpCode < 300) {
      if (outResponse != nullptr) {
        *outResponse = response;
      }
      if (outHttpCode != nullptr) {
        *outHttpCode = httpCode;
      }
      return true;
    }
    Serial.printf("[Voice] MiMo endpoint failed HTTP %d %s\r\n", httpCode, response.c_str());
    if (httpCode == 401 || httpCode == 403 || httpCode == 400 || httpCode == 422) {
      break;
    }
  }

  if (outResponse != nullptr) {
    *outResponse = response;
  }
  if (outHttpCode != nullptr) {
    *outHttpCode = httpCode;
  }
  return false;
}

static bool encodeBase64(const uint8_t *data, size_t dataLen, char **outB64, size_t *outLen) {
  if (data == nullptr || outB64 == nullptr || outLen == nullptr) {
    Serial.println("[Voice] b64 invalid args");
    return false;
  }

  size_t needed = 0;
  const int probe = mbedtls_base64_encode(nullptr, 0, &needed, data, dataLen);
  if (probe != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    Serial.printf("[Voice] b64 size probe failed code=%d len=%u needed=%u\r\n",
                  probe, (unsigned)dataLen, (unsigned)needed);
    return false;
  }

  char *buffer = static_cast<char *>(heap_caps_malloc(needed + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer == nullptr) {
    buffer = static_cast<char *>(malloc(needed + 1));
  }
  if (buffer == nullptr) {
    Serial.printf("[Voice] b64 alloc failed needed=%u heap=%u psram=%u\r\n",
                  (unsigned)needed, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    return false;
  }

  size_t written = 0;
  const int encoded = mbedtls_base64_encode(reinterpret_cast<unsigned char *>(buffer), needed + 1, &written,
                                           data, dataLen);
  if (encoded != 0) {
    Serial.printf("[Voice] b64 encode failed code=%d needed=%u written=%u\r\n",
                  encoded, (unsigned)needed, (unsigned)written);
    free(buffer);
    return false;
  }
  buffer[written] = '\0';
  *outB64 = buffer;
  *outLen = written;
  return true;
}

static void writeLe16(uint8_t *dst, uint16_t value) {
  dst[0] = (uint8_t)(value & 0xFF);
  dst[1] = (uint8_t)((value >> 8) & 0xFF);
}

static void writeLe32(uint8_t *dst, uint32_t value) {
  dst[0] = (uint8_t)(value & 0xFF);
  dst[1] = (uint8_t)((value >> 8) & 0xFF);
  dst[2] = (uint8_t)((value >> 16) & 0xFF);
  dst[3] = (uint8_t)((value >> 24) & 0xFF);
}

static void writeLe16Sample(uint8_t *pcm, int16_t sample) {
  pcm[0] = (uint8_t)(sample & 0xFF);
  pcm[1] = (uint8_t)(((uint16_t)sample >> 8) & 0xFF);
}

static void writeWavHeader(uint8_t *wav, uint32_t pcmBytes) {
  memcpy(wav + 0, "RIFF", 4);
  writeLe32(wav + 4, pcmBytes + VOICE_WAV_HEADER_BYTES - 8);
  memcpy(wav + 8, "WAVEfmt ", 8);
  writeLe32(wav + 16, 16);
  writeLe16(wav + 20, 1);
  writeLe16(wav + 22, 1);
  writeLe32(wav + 24, VOICE_SAMPLE_RATE);
  writeLe32(wav + 28, VOICE_SAMPLE_RATE * VOICE_SAMPLE_BYTES);
  writeLe16(wav + 32, VOICE_SAMPLE_BYTES);
  writeLe16(wav + 34, 16);
  memcpy(wav + 36, "data", 4);
  writeLe32(wav + 40, pcmBytes);
}

static int16_t readLe16Sample(const uint8_t *pcm) {
  return (int16_t)((uint16_t)pcm[0] | ((uint16_t)pcm[1] << 8));
}

static void logAudioStats(const uint8_t *pcm, size_t pcmLen) {
  if (pcm == nullptr || pcmLen < VOICE_SAMPLE_BYTES) {
    Serial.println("[Voice] audio stats: empty");
    return;
  }

  const size_t sampleCount = pcmLen / VOICE_SAMPLE_BYTES;
  int16_t minSample = 32767;
  int16_t maxSample = -32768;
  uint32_t clipped = 0;
  uint32_t nearZero = 0;
  double sumSquares = 0.0;
  double sumAbs = 0.0;

  for (size_t i = 0; i < sampleCount; i++) {
    const int16_t sample = readLe16Sample(pcm + i * VOICE_SAMPLE_BYTES);
    if (sample < minSample) {
      minSample = sample;
    }
    if (sample > maxSample) {
      maxSample = sample;
    }
    const int absSample = sample < 0 ? -sample : sample;
    if (absSample <= 8) {
      nearZero++;
    }
    if (absSample >= 32000) {
      clipped++;
    }
    sumAbs += (double)absSample;
    sumSquares += (double)sample * (double)sample;
  }

  const double meanAbs = sumAbs / (double)sampleCount;
  const double rms = sqrt(sumSquares / (double)sampleCount);
  Serial.printf("[Voice] audio stats: samples=%u seconds=%.2f min=%d max=%d meanAbs=%.1f rms=%.1f nearZero=%u clipped=%u\r\n",
                (unsigned)sampleCount,
                (double)sampleCount / (double)VOICE_SAMPLE_RATE,
                (int)minSample,
                (int)maxSample,
                meanAbs,
                rms,
                (unsigned)nearZero,
                (unsigned)clipped);
}

static void normalizeCapturedPcm(uint8_t *pcm, size_t pcmLen) {
  if (pcm == nullptr || pcmLen < VOICE_SAMPLE_BYTES) {
    return;
  }

  const size_t sampleCount = pcmLen / VOICE_SAMPLE_BYTES;
  double sum = 0.0;
  for (size_t i = 0; i < sampleCount; i++) {
    sum += (double)readLe16Sample(pcm + i * VOICE_SAMPLE_BYTES);
  }
  const double mean = sum / (double)sampleCount;

  double sumSquares = 0.0;
  for (size_t i = 0; i < sampleCount; i++) {
    const double centered = (double)readLe16Sample(pcm + i * VOICE_SAMPLE_BYTES) - mean;
    sumSquares += centered * centered;
  }
  const double rms = sqrt(sumSquares / (double)sampleCount);
  if (rms < 20.0) {
    Serial.printf("[Voice] audio normalize skipped mean=%.1f rms=%.1f\r\n", mean, rms);
    return;
  }

  double gain = VOICE_AUDIO_TARGET_RMS / rms;
  if (gain > VOICE_AUDIO_MAX_GAIN) {
    gain = VOICE_AUDIO_MAX_GAIN;
  }
  if (gain < 1.0) {
    gain = 1.0;
  }

  uint32_t clipped = 0;
  for (size_t i = 0; i < sampleCount; i++) {
    double value = ((double)readLe16Sample(pcm + i * VOICE_SAMPLE_BYTES) - mean) * gain;
    if (value > VOICE_AUDIO_CLIP_LIMIT) {
      value = VOICE_AUDIO_CLIP_LIMIT;
      clipped++;
    } else if (value < -VOICE_AUDIO_CLIP_LIMIT) {
      value = -VOICE_AUDIO_CLIP_LIMIT;
      clipped++;
    }
    writeLe16Sample(pcm + i * VOICE_SAMPLE_BYTES, (int16_t)(value >= 0.0 ? value + 0.5 : value - 0.5));
  }

  Serial.printf("[Voice] audio normalize mean=%.1f rms=%.1f gain=%.2f clipped=%u\r\n",
                mean, rms, gain, (unsigned)clipped);
}

static void dumpWavBase64ForDiagnostics(const uint8_t *wav, size_t wavLen) {
#if VOICE_SERIAL_WAV_DUMP
  char *base64 = nullptr;
  size_t base64Len = 0;
  if (!encodeBase64(wav, wavLen, &base64, &base64Len)) {
    Serial.println("[Voice] WAV dump b64 failed");
    return;
  }

  Serial.printf("[VoiceWav] BEGIN len=%u b64=%u sampleRate=%u channels=1 bits=16\r\n",
                (unsigned)wavLen, (unsigned)base64Len, (unsigned)VOICE_SAMPLE_RATE);
  const size_t chunk = 96;
  for (size_t i = 0; i < base64Len; i += chunk) {
    const size_t remain = base64Len - i;
    const size_t n = remain < chunk ? remain : chunk;
    Serial.write(reinterpret_cast<const uint8_t *>(base64 + i), n);
    Serial.print("\r\n");
    delay(2);
  }
  Serial.println("[VoiceWav] END");
  free(base64);
#else
  (void)wav;
  (void)wavLen;
#endif
}

static bool ensureI2S(void) {
  if (s_i2sReady) {
    return true;
  }
  s_i2s.setPinsPdmRx(VOICE_MIC_PDM_CLK_PIN, VOICE_MIC_PDM_DATA_PIN);
  s_i2s.setTimeout(100);
  if (!s_i2s.begin(I2S_MODE_PDM_RX, VOICE_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_MONO)) {
    Serial.println("[Voice] I2S microphone init failed");
    return false;
  }
  s_i2sReady = true;
  Serial.println("[Voice] I2S microphone ready");
  return true;
}

static bool beginRecording(void) {
  if (!ensureI2S()) {
    setError("Microphone init failed");
    return false;
  }
  if (s_pcm == nullptr) {
    s_pcm = static_cast<uint8_t *>(heap_caps_malloc(VOICE_MAX_PCM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_pcm == nullptr) {
      s_pcm = static_cast<uint8_t *>(malloc(VOICE_MAX_PCM_BYTES));
    }
    if (s_pcm == nullptr) {
      setError("Audio buffer alloc failed");
      return false;
    }
  }

  s_pcmLen = 0;
  s_recordStopRequested = false;
  s_recordStartedMs = millis();
  portENTER_CRITICAL(&s_voiceMux);
  s_transcript[0] = '\0';
  s_result[0] = '\0';
  s_error[0] = '\0';
  s_state = VOICE_STATE_RECORDING;
  s_dirty = true;
  portEXIT_CRITICAL(&s_voiceMux);
  if (xTaskCreate(voice_recording_task, "voice_rec", VOICE_RECORD_TASK_STACK_BYTES,
                  nullptr, 2, &s_recordTask) != pdPASS) {
    s_recordTask = nullptr;
    setError("Audio recorder task failed");
    return false;
  }
  Serial.println("[Voice] recording started");
  return true;
}

static bool buildWavJob(void) {
  if (s_pcmLen < VOICE_SAMPLE_BYTES) {
    setError("Recording is empty");
    return false;
  }
  if (s_jobWav != nullptr || s_task != nullptr) {
    setError("Voice pipeline busy");
    return false;
  }

  s_jobWavLen = VOICE_WAV_HEADER_BYTES + s_pcmLen;
  s_jobWav = static_cast<uint8_t *>(heap_caps_malloc(s_jobWavLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (s_jobWav == nullptr) {
    s_jobWav = static_cast<uint8_t *>(malloc(s_jobWavLen));
  }
  if (s_jobWav == nullptr) {
    setError("WAV buffer alloc failed");
    return false;
  }
  writeWavHeader(s_jobWav, (uint32_t)s_pcmLen);
  memcpy(s_jobWav + VOICE_WAV_HEADER_BYTES, s_pcm, s_pcmLen);
  return true;
}

static bool stopRecordingAndStartPipeline(void) {
  if (s_recordTask != nullptr) {
    s_recordStopRequested = true;
    const unsigned long stopStartMs = millis();
    while (s_recordTask != nullptr && (unsigned long)(millis() - stopStartMs) < 1000UL) {
      delay(5);
    }
    if (s_recordTask != nullptr) {
      setError("Audio recorder stop failed");
      return false;
    }
  }

  Serial.printf("[Voice] recording stopped, pcm=%u bytes\r\n", (unsigned)s_pcmLen);
  Serial.println("[Voice] raw audio stats:");
  logAudioStats(s_pcm, s_pcmLen);
  normalizeCapturedPcm(s_pcm, s_pcmLen);
  Serial.println("[Voice] normalized audio stats:");
  logAudioStats(s_pcm, s_pcmLen);

#if VOICE_RECORDING_DIAGNOSTIC_ONLY
  if (!buildWavJob()) {
    return false;
  }
  dumpWavBase64ForDiagnostics(s_jobWav, s_jobWavLen);
  free(s_jobWav);
  s_jobWav = nullptr;
  s_jobWavLen = 0;
  free(s_pcm);
  s_pcm = nullptr;
  s_pcmLen = 0;
  portENTER_CRITICAL(&s_voiceMux);
  copyText(s_result, sizeof(s_result), "Recording diagnostic dumped to serial");
  s_state = VOICE_STATE_DONE;
  s_dirty = true;
  portEXIT_CRITICAL(&s_voiceMux);
  return true;
#endif

  if (!settings_api_has_api_key()) {
    setError("AI API key missing");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    setError("WiFi disconnected");
    return false;
  }
  if (!buildWavJob()) {
    return false;
  }

  free(s_pcm);
  s_pcm = nullptr;
  s_pcmLen = 0;

  setState(VOICE_STATE_THINKING);
  if (xTaskCreate(voice_pipeline_task, "voice", VOICE_TASK_STACK_BYTES, nullptr, 1, &s_task) != pdPASS) {
    free(s_jobWav);
    s_jobWav = nullptr;
    s_jobWavLen = 0;
    s_task = nullptr;
    setError("Voice task create failed");
    return false;
  }
  return true;
}

static void serviceRecordingSamples(void) {
  if (s_state != VOICE_STATE_RECORDING) {
    return;
  }

  if (s_recordStopRequested && s_recordTask == nullptr) {
    stopRecordingAndStartPipeline();
    return;
  }

  if (s_pcmLen + VOICE_SAMPLE_BYTES > VOICE_MAX_PCM_BYTES && s_recordTask == nullptr) {
    stopRecordingAndStartPipeline();
  }
}

static void voice_recording_task(void *param) {
  (void)param;

  uint8_t chunk[VOICE_CAPTURE_READ_BYTES];
  unsigned lastWholeSecond = 0;
  size_t prerollBytesRemaining =
      (VOICE_SAMPLE_RATE * VOICE_SAMPLE_BYTES * VOICE_AUDIO_PREROLL_DISCARD_MS) / 1000U;
  prerollBytesRemaining -= prerollBytesRemaining % VOICE_SAMPLE_BYTES;
  Serial.println("[Voice] recorder task started");

  while (!s_recordStopRequested) {
    VoiceState state;
    portENTER_CRITICAL(&s_voiceMux);
    state = s_state;
    portEXIT_CRITICAL(&s_voiceMux);
    if (state != VOICE_STATE_RECORDING || s_pcm == nullptr) {
      break;
    }

    size_t remaining = 0;
    if (s_pcmLen < VOICE_MAX_PCM_BYTES) {
      remaining = VOICE_MAX_PCM_BYTES - s_pcmLen;
    }
    if (remaining < VOICE_SAMPLE_BYTES) {
      s_recordStopRequested = true;
      break;
    }

    size_t bytesToRead = VOICE_CAPTURE_READ_BYTES;
    if (bytesToRead > remaining) {
      bytesToRead = remaining;
    }
    bytesToRead -= bytesToRead % VOICE_SAMPLE_BYTES;
    if (bytesToRead == 0) {
      s_recordStopRequested = true;
      break;
    }

    size_t bytesRead = s_i2s.readBytes(reinterpret_cast<char *>(chunk), bytesToRead);
    bytesRead -= bytesRead % VOICE_SAMPLE_BYTES;
    if (bytesRead == 0) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    if (bytesRead > remaining) {
      bytesRead = remaining - (remaining % VOICE_SAMPLE_BYTES);
    }

    size_t copyOffset = 0;
    if (prerollBytesRemaining > 0) {
      if (bytesRead <= prerollBytesRemaining) {
        prerollBytesRemaining -= bytesRead;
        continue;
      }
      copyOffset = prerollBytesRemaining;
      bytesRead -= prerollBytesRemaining;
      prerollBytesRemaining = 0;
      Serial.printf("[Voice] discarded first %ums of microphone preroll\r\n",
                    (unsigned)VOICE_AUDIO_PREROLL_DISCARD_MS);
    }

    memcpy(s_pcm + s_pcmLen, chunk + copyOffset, bytesRead);
    s_pcmLen += bytesRead;

    const unsigned wholeSecond = (unsigned)(s_pcmLen / (VOICE_SAMPLE_RATE * VOICE_SAMPLE_BYTES));
    if (wholeSecond != lastWholeSecond) {
      lastWholeSecond = wholeSecond;
      portENTER_CRITICAL(&s_voiceMux);
      s_dirty = true;
      portEXIT_CRITICAL(&s_voiceMux);
    }
  }

  portENTER_CRITICAL(&s_voiceMux);
  s_recordTask = nullptr;
  s_dirty = true;
  portEXIT_CRITICAL(&s_voiceMux);
  Serial.println("[Voice] recorder task stopped");
  vTaskDelete(nullptr);
}

static bool transcribeMimo(uint8_t *wav, size_t wavLen, char *out, size_t outLen) {
  Serial.printf("[Voice] MiMo ASR prepare wav=%u heap=%u psram=%u\r\n",
                (unsigned)wavLen, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
  Serial.printf("[Voice] MiMo ASR heap detail maxAlloc=%u internal=%u minFree=%u\r\n",
                (unsigned)ESP.getMaxAllocHeap(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)ESP.getMinFreeHeap());
  Serial.flush();

  char apiKey[129];
  settings_api_get_api_key(apiKey, sizeof(apiKey));

  char *base64 = nullptr;
  size_t base64Len = 0;
  if (!encodeBase64(wav, wavLen, &base64, &base64Len)) {
    free(wav);
    Serial.println("[Voice] MiMo ASR b64 failed");
    return false;
  }
  free(wav);
  wav = nullptr;
  Serial.printf("[Voice] MiMo ASR b64=%u\r\n", (unsigned)base64Len);

  const size_t bodyCap = base64Len + 640;
  char *body = static_cast<char *>(heap_caps_malloc(bodyCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (body == nullptr) {
    body = static_cast<char *>(malloc(bodyCap));
  }
  if (body == nullptr) {
    free(base64);
    Serial.printf("[Voice] MiMo ASR body alloc failed cap=%u heap=%u psram=%u\r\n",
                  (unsigned)bodyCap, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    return false;
  }

  size_t pos = 0;
  body[0] = '\0';
  const bool built =
      appendStr(body, bodyCap, &pos, "{\"model\":\"" VOICE_MIMO_ASR_MODEL "\",\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"data:audio/wav;base64,") &&
      appendStr(body, bodyCap, &pos, base64) &&
      appendStr(body, bodyCap, &pos, "\"}}]}],\"asr_options\":{\"language\":\"zh\"}}");
  free(base64);
  if (!built) {
    Serial.printf("[Voice] MiMo ASR body build failed pos=%u cap=%u\r\n",
                  (unsigned)pos, (unsigned)bodyCap);
    free(body);
    return false;
  }
  Serial.printf("[Voice] MiMo ASR POST body=%u heap=%u psram=%u\r\n",
                (unsigned)strlen(body), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
  Serial.flush();

  int httpCode = 0;
  String response;
  const bool posted = httpPostMimoJson(apiKey, body, &response, &httpCode);
  free(body);

  if (!posted) {
    Serial.printf("[Voice] MiMo ASR HTTP %d %s\r\n", httpCode, response.c_str());
    return false;
  }
  const int choicesIdx = response.indexOf("\"choices\"");
  const String tail = choicesIdx >= 0 ? response.substring(choicesIdx) : response;
  if (!parseJsonStringField(tail, "content", out, outLen)) {
    Serial.printf("[Voice] MiMo ASR parse fail %s\r\n", response.c_str());
    return false;
  }
  trimText(out);
  return out[0] != '\0';
}

static bool buildChatBody(AiProvider provider, const char *model, const char *transcript, char **outBody) {
  const char *systemText =
      app_locale_get() == APP_LANG_ZH
          ? "你是一个小型墨水屏语音助手。用简短自然的中文回答，最多40字，不要带任何表情。"
          : "You are a compact e-paper voice assistant. Answer naturally in at most 40 words, no emojis.";
  const char *prefix =
      app_locale_get() == APP_LANG_ZH
          ? "用户语音转写如下，请直接回答："
          : "The user's speech transcript is below. Answer directly:";

  const size_t bodyCap = strlen(model) + strlen(systemText) + strlen(prefix) + strlen(transcript) + 512;
  char *body = static_cast<char *>(heap_caps_malloc(bodyCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (body == nullptr) {
    body = static_cast<char *>(malloc(bodyCap));
  }
  if (body == nullptr) {
    return false;
  }

  size_t pos = 0;
  body[0] = '\0';
  if (!appendStr(body, bodyCap, &pos, "{\"model\":\"") ||
      !appendStr(body, bodyCap, &pos, model) ||
      !appendStr(body, bodyCap, &pos,
                 provider == AI_PROVIDER_MIMO
                     ? "\",\"max_completion_tokens\":256,\"messages\":[{\"role\":\"system\",\"content\":\""
                     : "\",\"max_tokens\":256,\"messages\":[{\"role\":\"system\",\"content\":\"") ||
      !appendJsonEscaped(body, bodyCap, &pos, systemText) ||
      !appendStr(body, bodyCap, &pos, "\"},{\"role\":\"user\",\"content\":\"") ||
      !appendJsonEscaped(body, bodyCap, &pos, prefix) ||
      !appendStr(body, bodyCap, &pos, "\\n") ||
      !appendJsonEscaped(body, bodyCap, &pos, transcript) ||
      !appendStr(body, bodyCap, &pos, "\"}]}")) {
    free(body);
    return false;
  }

  *outBody = body;
  return true;
}

static bool requestChatCompletion(const char *transcript, char *out, size_t outLen) {
  const AiProvider provider = AI_PROVIDER_MIMO;
  const char *model = settings_api_get_provider() == AI_PROVIDER_MIMO
                          ? settings_api_get_model_id()
                          : VOICE_MIMO_FALLBACK_MODEL;
  if (model == nullptr || model[0] == '\0') {
    model = VOICE_MIMO_FALLBACK_MODEL;
  }

  char apiKey[129];
  settings_api_get_api_key(apiKey, sizeof(apiKey));

  char *body = nullptr;
  if (!buildChatBody(provider, model, transcript, &body)) {
    return false;
  }

  String response;
  int httpCode = 0;
  const bool posted = httpPostMimoJson(apiKey, body, &response, &httpCode);
  free(body);

  if (!posted) {
    Serial.printf("[Voice] MiMo LLM HTTP %d %s\r\n", httpCode, response.c_str());
    return false;
  }

  const int choicesIdx = response.indexOf("\"choices\"");
  const String tail = choicesIdx >= 0 ? response.substring(choicesIdx) : response;
  if (!parseJsonStringField(tail, "content", out, outLen)) {
    Serial.printf("[Voice] MiMo LLM parse fail %s\r\n", response.c_str());
    return false;
  }
  trimText(out);
  return out[0] != '\0';
}

static unsigned long estimateSpeakingMs(const char *text) {
  const size_t len = text != nullptr ? strlen(text) : 0;
  unsigned long ms = (unsigned long)len * 90UL;
  if (ms < 1500UL) {
    ms = 1500UL;
  }
  if (ms > 12000UL) {
    ms = 12000UL;
  }
  return ms;
}

static void voice_pipeline_task(void *param) {
  (void)param;

  uint8_t *wav = nullptr;
  size_t wavLen = 0;
  portENTER_CRITICAL(&s_voiceMux);
  wav = s_jobWav;
  wavLen = s_jobWavLen;
  s_jobWav = nullptr;
  s_jobWavLen = 0;
  portEXIT_CRITICAL(&s_voiceMux);

  char transcript[sizeof(s_transcript)];
  char result[sizeof(s_result)];
  transcript[0] = '\0';
  result[0] = '\0';

  bool ok = false;
  if (wav != nullptr && wavLen > VOICE_WAV_HEADER_BYTES) {
    ok = transcribeMimo(wav, wavLen, transcript, sizeof(transcript));
    wav = nullptr;
  }
  free(wav);

  if (!ok) {
    portENTER_CRITICAL(&s_voiceMux);
    s_task = nullptr;
    portEXIT_CRITICAL(&s_voiceMux);
    setError("MiMo speech recognition failed");
    vTaskDelete(nullptr);
    return;
  }

  portENTER_CRITICAL(&s_voiceMux);
  copyText(s_transcript, sizeof(s_transcript), transcript);
  s_dirty = true;
  portEXIT_CRITICAL(&s_voiceMux);
  Serial.printf("[Voice] transcript: %s\r\n", transcript);

  ok = requestChatCompletion(transcript, result, sizeof(result));
  if (!ok) {
    portENTER_CRITICAL(&s_voiceMux);
    s_task = nullptr;
    portEXIT_CRITICAL(&s_voiceMux);
    setError("LLM request failed");
    vTaskDelete(nullptr);
    return;
  }

  portENTER_CRITICAL(&s_voiceMux);
  copyText(s_result, sizeof(s_result), result);
  s_speakingStartedMs = millis();
  s_speakingDurationMs = estimateSpeakingMs(result);
  s_state = VOICE_STATE_SPEAKING;
  s_dirty = true;
  s_task = nullptr;
  portEXIT_CRITICAL(&s_voiceMux);
  Serial.printf("[Voice] result: %s\r\n", result);
  Serial.println("[Voice] speaker playback placeholder active");
  vTaskDelete(nullptr);
}

void voice_service_init(void) {
  s_state = VOICE_STATE_IDLE;
  s_dirty = false;
  s_transcript[0] = '\0';
  s_result[0] = '\0';
  s_error[0] = '\0';
}

bool voice_service_toggle_recording(void) {
  VoiceState state;
  portENTER_CRITICAL(&s_voiceMux);
  state = s_state;
  portEXIT_CRITICAL(&s_voiceMux);

  if (state == VOICE_STATE_RECORDING) {
    return stopRecordingAndStartPipeline();
  }
  if (state == VOICE_STATE_THINKING) {
    return false;
  }
  if (state == VOICE_STATE_SPEAKING) {
    (void)voice_service_interrupt_speaker();
  }
  return beginRecording();
}

bool voice_service_interrupt_speaker(void) {
  bool interrupted = false;
  portENTER_CRITICAL(&s_voiceMux);
  if (s_state == VOICE_STATE_SPEAKING) {
    s_state = VOICE_STATE_DONE;
    s_dirty = true;
    interrupted = true;
  }
  portEXIT_CRITICAL(&s_voiceMux);
  if (interrupted) {
    Serial.println("[Voice] speaker interrupted");
  }
  return interrupted;
}

bool voice_service_service(void) {
  serviceRecordingSamples();

  VoiceState state;
  bool dirty;
  unsigned long started;
  unsigned long duration;
  portENTER_CRITICAL(&s_voiceMux);
  state = s_state;
  dirty = s_dirty;
  started = s_speakingStartedMs;
  duration = s_speakingDurationMs;
  s_dirty = false;
  portEXIT_CRITICAL(&s_voiceMux);

  if (state == VOICE_STATE_SPEAKING && duration > 0 &&
      (unsigned long)(millis() - started) >= duration) {
    setState(VOICE_STATE_DONE);
    dirty = true;
  }
  return dirty;
}

bool voice_service_is_busy(void) {
  VoiceState state;
  TaskHandle_t task;
  TaskHandle_t recordTask;
  bool localSeedCaptureActive;
  portENTER_CRITICAL(&s_voiceMux);
  state = s_state;
  task = s_task;
  recordTask = s_recordTask;
  localSeedCaptureActive = s_localSeedCaptureActive;
  portEXIT_CRITICAL(&s_voiceMux);
  return state == VOICE_STATE_RECORDING || state == VOICE_STATE_THINKING ||
         state == VOICE_STATE_SPEAKING || task != nullptr || recordTask != nullptr ||
         localSeedCaptureActive;
}

VoiceState voice_service_state(void) {
  VoiceState state;
  portENTER_CRITICAL(&s_voiceMux);
  state = s_state;
  portEXIT_CRITICAL(&s_voiceMux);
  return state;
}

bool voice_service_capture_local_seed(uint32_t recordMs, uint32_t *outSeed) {
  if (outSeed == nullptr) {
    return false;
  }
  if (recordMs < 500U) {
    recordMs = 500U;
  }
  if (recordMs > 4000U) {
    recordMs = 4000U;
  }

  bool canCapture = false;
  portENTER_CRITICAL(&s_voiceMux);
  canCapture = !s_localSeedCaptureActive &&
               (s_state == VOICE_STATE_IDLE || s_state == VOICE_STATE_DONE || s_state == VOICE_STATE_ERROR) &&
               s_task == nullptr && s_recordTask == nullptr;
  if (canCapture) {
    s_localSeedCaptureActive = true;
  }
  portEXIT_CRITICAL(&s_voiceMux);

  if (!canCapture) {
    Serial.println("[Voice] local seed capture skipped: busy");
    return false;
  }

  bool ok = false;
  uint32_t seed = esp_random() ^ (uint32_t)millis();
  do {
    if (!ensureI2S()) {
      Serial.println("[Voice] local seed microphone init failed");
      break;
    }

    uint64_t energy[8] = {};
    uint32_t samplesPerSegment = ((VOICE_SAMPLE_RATE * recordMs) / 1000U) / 8U;
    if (samplesPerSegment == 0) {
      samplesPerSegment = 1;
    }

    uint32_t zcr = 0;
    uint32_t sampleIndex = 0;
    uint32_t discardedBytes =
        (VOICE_SAMPLE_RATE * VOICE_SAMPLE_BYTES * VOICE_AUDIO_PREROLL_DISCARD_MS) / 1000U;
    discardedBytes -= discardedBytes % VOICE_SAMPLE_BYTES;
    const uint32_t targetSamples = (VOICE_SAMPLE_RATE * recordMs) / 1000U;
    const unsigned long startMs = millis();
    const unsigned long timeoutMs = recordMs + 900U;
    int16_t previous = 0;
    bool havePrevious = false;
    uint8_t chunk[VOICE_CAPTURE_READ_BYTES];

    while (sampleIndex < targetSamples && (unsigned long)(millis() - startMs) < timeoutMs) {
      size_t bytesRead = s_i2s.readBytes(reinterpret_cast<char *>(chunk), sizeof(chunk));
      bytesRead -= bytesRead % VOICE_SAMPLE_BYTES;
      if (bytesRead == 0) {
        delay(1);
        continue;
      }

      size_t offset = 0;
      if (discardedBytes > 0) {
        if (bytesRead <= discardedBytes) {
          discardedBytes -= (uint32_t)bytesRead;
          continue;
        }
        offset = discardedBytes;
        bytesRead -= discardedBytes;
        discardedBytes = 0;
      }

      for (size_t i = offset; i + 1 < offset + bytesRead && sampleIndex < targetSamples; i += VOICE_SAMPLE_BYTES) {
        const int16_t sample = readLe16Sample(chunk + i);
        if (havePrevious && ((sample ^ previous) & 0x8000)) {
          zcr++;
        }
        previous = sample;
        havePrevious = true;

        uint32_t segment = sampleIndex / samplesPerSegment;
        if (segment > 7U) {
          segment = 7U;
        }
        const int32_t centered = (int32_t)sample;
        energy[segment] += (uint64_t)(centered * centered);
        sampleIndex++;
      }
    }

    if (sampleIndex < VOICE_SAMPLE_RATE / 4U) {
      Serial.printf("[Voice] local seed too few samples=%u\r\n", (unsigned)sampleIndex);
      break;
    }

    uint32_t energyHash = 0x51ED5EEDu;
    for (int i = 0; i < 8; i++) {
      energyHash = energyHash * 37U + (uint32_t)(energy[i] >> 18);
    }
    seed ^= zcr * 16777619UL;
    seed ^= energyHash;
    seed ^= sampleIndex * 2654435761UL;
    seed ^= (uint32_t)micros();
    ok = true;
    Serial.printf("[Voice] local seed samples=%u zcr=%u energy=%u\r\n",
                  (unsigned)sampleIndex, (unsigned)zcr, (unsigned)energyHash);
  } while (false);

  portENTER_CRITICAL(&s_voiceMux);
  s_localSeedCaptureActive = false;
  portEXIT_CRITICAL(&s_voiceMux);

  *outSeed = seed;
  return ok;
}

void voice_service_status_text(char *out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return;
  }

  VoiceState state;
  char transcript[sizeof(s_transcript)];
  char result[sizeof(s_result)];
  char error[sizeof(s_error)];
  size_t pcmLen;
  portENTER_CRITICAL(&s_voiceMux);
  state = s_state;
  copyText(transcript, sizeof(transcript), s_transcript);
  copyText(result, sizeof(result), s_result);
  copyText(error, sizeof(error), s_error);
  pcmLen = s_pcmLen;
  portEXIT_CRITICAL(&s_voiceMux);

  const bool zh = app_locale_get() == APP_LANG_ZH;
  switch (state) {
    case VOICE_STATE_RECORDING: {
      const unsigned seconds = (unsigned)(pcmLen / (VOICE_SAMPLE_RATE * VOICE_SAMPLE_BYTES));
      snprintf(out, outLen, zh ? "录音中...\n再次双击 B 结束\n%us / %us" :
                                 "Recording...\nDouble-click B to stop\n%us / %us",
               seconds, (unsigned)VOICE_MAX_RECORD_SECONDS);
      break;
    }
    case VOICE_STATE_THINKING:
      snprintf(out, outLen, zh ? "正在转写并请求 LLM..." : "Transcribing and asking the LLM...");
      break;
    case VOICE_STATE_SPEAKING:
      snprintf(out, outLen, zh ? "朗读中，按 A 可打断\n%s" : "Speaking; press A to interrupt\n%s",
               result[0] != '\0' ? result : "-");
      break;
    case VOICE_STATE_DONE:
      snprintf(out, outLen, "%s", result[0] != '\0' ? result : (zh ? "完成" : "Done"));
      break;
    case VOICE_STATE_ERROR:
      snprintf(out, outLen, "%s", error[0] != '\0' ? error : (zh ? "语音失败" : "Voice failed"));
      break;
    case VOICE_STATE_IDLE:
    default:
      snprintf(out, outLen, zh ? "双击 B 开始语音录制" : "Double-click B to start voice recording");
      break;
  }
}
