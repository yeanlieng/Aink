#include "vision_service.h"

#include "ai_model_config.h"
#include "app_locale.h"
#include "camera_service.h"
#include "settings_api.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <stdio.h>
#include <string.h>

#define VISION_HTTP_TIMEOUT_MS  60000
#define VISION_MAX_JPEG_BYTES   (64 * 1024)
#define VISION_MAX_TOKENS       1024
#define VISION_OUTPUT_MAX_CHARS 40

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

static bool appendJsonEscaped(char *buf, size_t bufLen, size_t *pos, const char *text) {
  if (text == nullptr) {
    return true;
  }
  while (*text != '\0') {
    const char c = *text;
    if (c == '\\' || c == '\"') {
      if (!appendChar(buf, bufLen, pos, '\\')) {
        return false;
      }
    }
    if (!appendChar(buf, bufLen, pos, c)) {
      return false;
    }
    text++;
  }
  return true;
}

static const char *system_prompt(void) {
  if (app_locale_get() == APP_LANG_ZH) {
    return "你是墨水屏诗人。根据照片写一句中文，不超过40字，凝练有诗意。"
           "只输出这一句正文，不要思考过程、不要解释、不要引号、不要标题。";
  }
  return "You are a poet for e-paper. Write one concise poetic sentence about the photo. "
         "At most 40 words. Output only the final sentence, no reasoning or explanation.";
}

static const char *user_prompt(void) {
  return app_locale_get() == APP_LANG_ZH ? "请直接输出一句描述。" : "Reply with one sentence only.";
}

static bool encodeBase64(const uint8_t *data, size_t dataLen, char **outB64, size_t *outLen) {
  if (data == nullptr || outB64 == nullptr || outLen == nullptr) {
    return false;
  }

  size_t needed = 0;
  if (mbedtls_base64_encode(nullptr, 0, &needed, data, dataLen) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    return false;
  }

  char *buffer = static_cast<char *>(heap_caps_malloc(needed + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer == nullptr) {
    buffer = static_cast<char *>(malloc(needed + 1));
  }
  if (buffer == nullptr) {
    return false;
  }

  size_t written = 0;
  if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(buffer), needed + 1, &written, data,
                            dataLen) != 0) {
    free(buffer);
    return false;
  }
  buffer[written] = '\0';
  *outB64 = buffer;
  *outLen = written;
  return true;
}

static bool httpPostJson(const char *url, const char *authHeader, const char *authValue,
                         const char *extraHeaderName, const char *extraHeaderValue,
                         char *jsonBody, String *outResponse, int *outHttpCode) {
  if (url == nullptr || jsonBody == nullptr || outResponse == nullptr || outHttpCode == nullptr) {
    return false;
  }

  const size_t bodyLen = strlen(jsonBody);
  IPAddress serverIp;
  bool dnsOk = false;
  const char *hostStart = strstr(url, "://");
  if (hostStart != nullptr) {
    hostStart += 3;
    const char *hostEnd = strchr(hostStart, '/');
    const size_t hostLen = hostEnd != nullptr ? (size_t)(hostEnd - hostStart) : strlen(hostStart);
    if (hostLen > 0 && hostLen < 96) {
      char host[96];
      memcpy(host, hostStart, hostLen);
      host[hostLen] = '\0';
      dnsOk = WiFi.hostByName(host, serverIp);
    }
  }
  Serial.printf("[Vision] HTTP POST %u bytes to %s (heap=%u psram=%u block=%u dns=%s)\r\n",
                (unsigned)bodyLen, url,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                dnsOk ? serverIp.toString().c_str() : "fail");
  if (ESP.getFreePsram() == 0) {
    Serial.println("[Vision] warn: PSRAM=0, enable OPI PSRAM in board settings");
  }
  Serial.flush();

  WiFi.setSleep(WIFI_PS_NONE);
  yield();

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(VISION_HTTP_TIMEOUT_MS);
  client.setHandshakeTimeout(30);

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("[Vision] HTTP begin failed");
    *outHttpCode = -1;
    *outResponse = "begin failed";
    return false;
  }

  http.setConnectTimeout(30000);
  http.setTimeout(VISION_HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");
  if (authHeader != nullptr && authValue != nullptr && authValue[0] != '\0') {
    http.addHeader(authHeader, authValue);
  }
  if (extraHeaderName != nullptr && extraHeaderValue != nullptr && extraHeaderValue[0] != '\0') {
    http.addHeader(extraHeaderName, extraHeaderValue);
  }

  *outHttpCode = http.POST(reinterpret_cast<uint8_t *>(jsonBody), bodyLen);
  if (*outHttpCode > 0) {
    *outResponse = http.getString();
  } else {
    *outResponse = http.errorToString(*outHttpCode);
    char sslError[120];
    const int sslCode = client.lastError(sslError, sizeof(sslError));
    Serial.printf("[Vision] POST fail heap=%u block=%u err=%s\r\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                  outResponse->c_str());
    Serial.printf("[Vision] TLS lastError=%d %s\r\n", sslCode, sslError);
  }
  http.end();
  return *outHttpCode > 0;
}

static bool parseJsonStringField(const String &body, const char *fieldKey, char *out, size_t outLen) {
  char search[32];
  snprintf(search, sizeof(search), "\"%s\":\"", fieldKey);
  int idx = body.indexOf(search);
  if (idx < 0) {
    return false;
  }

  idx += (int)strlen(search);
  size_t w = 0;
  for (int i = idx; i < body.length() && w + 1 < outLen; i++) {
    const char c = body.charAt(i);
    if (c == '\"' && body.charAt(i - 1) != '\\') {
      break;
    }
    if (c == '\\' && i + 1 < body.length()) {
      const char next = body.charAt(i + 1);
      if (next == 'n') {
        out[w++] = '\n';
        i++;
        continue;
      }
      if (next == 'r') {
        out[w++] = '\r';
        i++;
        continue;
      }
      if (next == 't') {
        out[w++] = '\t';
        i++;
        continue;
      }
      if (next == '\\' || next == '\"') {
        out[w++] = next;
        i++;
        continue;
      }
    }
    out[w++] = c;
  }
  out[w] = '\0';
  return w > 0;
}

static bool looksLikeReasoningChain(const char *text) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  if (strstr(text, "用户现在") != nullptr || strstr(text, "首先看") != nullptr) {
    return true;
  }
  if (strstr(text, "不对") != nullptr && strlen(text) > 48U) {
    return true;
  }
  return false;
}

static bool extractLastAsciiQuoted(const String &text, char *out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return false;
  }
  out[0] = '\0';

  const int closeIdx = text.lastIndexOf('\"');
  if (closeIdx <= 0) {
    return false;
  }
  const int openIdx = text.lastIndexOf('\"', closeIdx - 1);
  if (openIdx < 0 || openIdx >= closeIdx - 1) {
    return false;
  }

  const String quoted = text.substring(openIdx + 1, closeIdx);
  if (quoted.length() == 0 || quoted.length() >= (int)outLen) {
    return false;
  }
  quoted.toCharArray(out, outLen);
  return out[0] != '\0';
}

static void trimVisionOutput(char *text) {
  if (text == nullptr) {
    return;
  }

  size_t start = 0;
  while (text[start] == ' ' || text[start] == '\n' || text[start] == '\r' || text[start] == '\t') {
    start++;
  }
  if (start > 0) {
    memmove(text, text + start, strlen(text + start) + 1);
  }

  const size_t len = strlen(text);
  size_t end = len;
  while (end > 0) {
    const char c = text[end - 1];
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\"' || c == '\'') {
      end--;
      continue;
    }
    break;
  }
  text[end] = '\0';

  if (text[0] == '\"' || text[0] == '\'') {
    memmove(text, text + 1, strlen(text));
    trimVisionOutput(text);
  }
}

static void truncateVisionOutput(char *text, size_t maxChars) {
  if (text == nullptr || maxChars == 0) {
    return;
  }

  size_t i = 0;
  size_t chars = 0;
  while (text[i] != '\0' && chars < maxChars) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    size_t step = 1;
    if (c >= 0xF0) {
      step = 4;
    } else if (c >= 0xE0) {
      step = 3;
    } else if (c >= 0xC0) {
      step = 2;
    }
    if (text[i + step - 1] == '\0' && step > 1) {
      break;
    }
    i += step;
    chars++;
  }
  text[i] = '\0';
}

static void normalizeVisionOutput(const char *content, const char *reasoning, char *out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return;
  }
  out[0] = '\0';

  char candidate[384];
  candidate[0] = '\0';

  if (content != nullptr && content[0] != '\0' && !looksLikeReasoningChain(content)) {
    snprintf(candidate, sizeof(candidate), "%s", content);
  } else if (content != nullptr && content[0] != '\0') {
    if (!extractLastAsciiQuoted(String(content), candidate, sizeof(candidate))) {
      snprintf(candidate, sizeof(candidate), "%s", content);
    }
  } else if (reasoning != nullptr && reasoning[0] != '\0') {
    if (!extractLastAsciiQuoted(String(reasoning), candidate, sizeof(candidate))) {
      const char *finalMark = strstr(reasoning, "最终");
      const char *source = finalMark != nullptr ? finalMark : reasoning;
      if (!extractLastAsciiQuoted(String(source), candidate, sizeof(candidate))) {
        return;
      }
    }
  }

  trimVisionOutput(candidate);
  truncateVisionOutput(candidate, VISION_OUTPUT_MAX_CHARS);
  snprintf(out, outLen, "%s", candidate);
}

static bool parseVisionMessageFields(const String &body, char *contentOut, size_t contentLen) {
  if (contentOut != nullptr && contentLen > 0) {
    contentOut[0] = '\0';
  }

  const int idx = body.indexOf("\"choices\"");
  const String tail = idx >= 0 ? body.substring(idx) : body;
  return contentOut != nullptr && parseJsonStringField(tail, "content", contentOut, contentLen);
}

static bool parseProviderText(const String &body, const char *sectionKey, char *out, size_t outLen) {
  const int idx = body.indexOf(sectionKey);
  const String tail = idx >= 0 ? body.substring(idx) : body;

  if (sectionKey != nullptr && strstr(sectionKey, "choices") != nullptr) {
    char content[384];
    content[0] = '\0';
    parseVisionMessageFields(body, content, sizeof(content));

    if (content[0] != '\0' && !looksLikeReasoningChain(content)) {
      normalizeVisionOutput(content, nullptr, out, outLen);
      if (out[0] != '\0') {
        return true;
      }
    }

    char *reasoning = static_cast<char *>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (reasoning == nullptr) {
      reasoning = static_cast<char *>(malloc(4096));
    }
    if (reasoning != nullptr) {
      reasoning[0] = '\0';
      const int idx = body.indexOf("\"choices\"");
      const String tail = idx >= 0 ? body.substring(idx) : body;
      if (parseJsonStringField(tail, "reasoning_content", reasoning, 4096)) {
        normalizeVisionOutput(content, reasoning, out, outLen);
      }
      free(reasoning);
      if (out[0] != '\0') {
        return true;
      }
    }

    char quoted[128];
    if (content[0] != '\0' && extractLastAsciiQuoted(String(content), quoted, sizeof(quoted))) {
      trimVisionOutput(quoted);
      truncateVisionOutput(quoted, VISION_OUTPUT_MAX_CHARS);
      snprintf(out, outLen, "%s", quoted);
      return out[0] != '\0';
    }

    if (content[0] != '\0') {
      normalizeVisionOutput(content, nullptr, out, outLen);
      return out[0] != '\0';
    }
    return false;
  }

  char raw[384];
  if (parseJsonStringField(tail, "text", raw, sizeof(raw)) ||
      parseJsonStringField(tail, "content", raw, sizeof(raw))) {
    normalizeVisionOutput(raw, nullptr, out, outLen);
    return out[0] != '\0';
  }
  return false;
}

static bool buildOpenAiCompatibleBody(const char *model, const char *systemText, const char *userText,
                                      const char *base64Jpeg, const char *maxTokensKey, char **outBody) {
  const size_t bodyCap = strlen(model) + strlen(systemText) + strlen(userText) + strlen(base64Jpeg) + 1024;
  char *body = static_cast<char *>(heap_caps_malloc(bodyCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (body == nullptr) {
    body = static_cast<char *>(malloc(bodyCap));
  }
  if (body == nullptr) {
    return false;
  }

  size_t pos = 0;
  body[0] = '\0';
  char tokenBuf[12];
  snprintf(tokenBuf, sizeof(tokenBuf), "%d", VISION_MAX_TOKENS);
  if (!appendStr(body, bodyCap, &pos, "{\"model\":\"") ||
      !appendStr(body, bodyCap, &pos, model) ||
      !appendStr(body, bodyCap, &pos, "\",\"") ||
      !appendStr(body, bodyCap, &pos, maxTokensKey) ||
      !appendStr(body, bodyCap, &pos, "\":") ||
      !appendStr(body, bodyCap, &pos, tokenBuf) ||
      !appendStr(body, bodyCap, &pos, ",\"messages\":[{\"role\":\"system\",\"content\":\"") ||
      !appendJsonEscaped(body, bodyCap, &pos, systemText) ||
      !appendStr(body, bodyCap, &pos, "\"},{\"role\":\"user\",\"content\":[{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,") ||
      !appendStr(body, bodyCap, &pos, base64Jpeg) ||
      !appendStr(body, bodyCap, &pos, "\"}},{\"type\":\"text\",\"text\":\"") ||
      !appendJsonEscaped(body, bodyCap, &pos, userText) ||
      !appendStr(body, bodyCap, &pos, "\"}]}]}")) {
    free(body);
    return false;
  }

  *outBody = body;
  return true;
}

static bool buildGeminiBody(const char *systemText, const char *userText, const char *base64Jpeg,
                            char **outBody) {
  char combined[384];
  snprintf(combined, sizeof(combined), "%s\n\n%s", systemText, userText);

  const size_t bodyCap = strlen(combined) + strlen(base64Jpeg) + 256;
  char *body = static_cast<char *>(heap_caps_malloc(bodyCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (body == nullptr) {
    body = static_cast<char *>(malloc(bodyCap));
  }
  if (body == nullptr) {
    return false;
  }

  size_t pos = 0;
  body[0] = '\0';
  char tokenBuf[12];
  snprintf(tokenBuf, sizeof(tokenBuf), "%d", VISION_MAX_TOKENS);
  if (!appendStr(body, bodyCap, &pos, "{\"contents\":[{\"parts\":[{\"text\":\"") ||
      !appendJsonEscaped(body, bodyCap, &pos, combined) ||
      !appendStr(body, bodyCap, &pos, "\"},{\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\"") ||
      !appendStr(body, bodyCap, &pos, base64Jpeg) ||
      !appendStr(body, bodyCap, &pos, "\"}}]}],\"generationConfig\":{\"maxOutputTokens\":") ||
      !appendStr(body, bodyCap, &pos, tokenBuf) ||
      !appendStr(body, bodyCap, &pos, "}}")) {
    free(body);
    return false;
  }

  *outBody = body;
  return true;
}

static VisionResult requestOpenAiCompatible(AiProvider provider, const char *url, const char *apiKey,
                                            const char *model, const char *base64Jpeg, char *outText,
                                            size_t outLen) {
  char bearerAuth[160];
  snprintf(bearerAuth, sizeof(bearerAuth), "Bearer %s", apiKey);
  const char *authHeader = "Authorization";
  const char *authValue = bearerAuth;

  const char *maxTokensKey =
      provider == AI_PROVIDER_MIMO ? "max_completion_tokens" : "max_tokens";

  char *body = nullptr;
  if (!buildOpenAiCompatibleBody(model, system_prompt(), user_prompt(), base64Jpeg, maxTokensKey, &body)) {
    return VISION_RESULT_HTTP_FAIL;
  }

  String response;
  int httpCode = 0;
  const bool posted = httpPostJson(url, authHeader, authValue, nullptr, nullptr, body, &response, &httpCode);
  free(body);

  if (!posted || httpCode < 200 || httpCode >= 300) {
    Serial.printf("[Vision] HTTP %d %s\r\n", httpCode, response.c_str());
    if (httpCode == 401 || httpCode == 403) {
      return VISION_RESULT_NO_API;
    }
    return VISION_RESULT_HTTP_FAIL;
  }

  if (!parseProviderText(response, "\"choices\"", outText, outLen)) {
    Serial.printf("[Vision] parse fail %s\r\n", response.c_str());
    return VISION_RESULT_PARSE_FAIL;
  }
  return VISION_RESULT_OK;
}

static VisionResult requestGemini(const char *apiKey, const char *model, char *base64Jpeg,
                                  char *outText, size_t outLen) {
  char url[192];
  snprintf(url, sizeof(url),
           "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent?key=%s",
           model, apiKey);

  char *body = nullptr;
  if (!buildGeminiBody(system_prompt(), user_prompt(), base64Jpeg, &body)) {
    free(base64Jpeg);
    return VISION_RESULT_HTTP_FAIL;
  }
  free(base64Jpeg);

  String response;
  int httpCode = 0;
  const bool posted = httpPostJson(url, nullptr, nullptr, nullptr, nullptr, body, &response, &httpCode);
  free(body);

  if (!posted || httpCode < 200 || httpCode >= 300) {
    Serial.printf("[Vision] Gemini HTTP %d %s\r\n", httpCode, response.c_str());
    return VISION_RESULT_HTTP_FAIL;
  }

  if (!parseProviderText(response, "\"candidates\"", outText, outLen)) {
    Serial.printf("[Vision] Gemini parse fail %s\r\n", response.c_str());
    return VISION_RESULT_PARSE_FAIL;
  }
  return VISION_RESULT_OK;
}

static VisionResult requestMimoCompatible(const char *apiKey, const char *model,
                                          const char *base64Jpeg, char *outText,
                                          size_t outLen) {
  static const char *kMimoUrls[] = {
      "https://api.xiaomimimo.com/v1/chat/completions",
      "https://token-plan-cn.xiaomimimo.com/v1/chat/completions",
      "https://token-plan-sgp.xiaomimimo.com/v1/chat/completions",
      "https://token-plan-ams.xiaomimimo.com/v1/chat/completions",
  };

  VisionResult lastResult = VISION_RESULT_HTTP_FAIL;
  for (size_t i = 0; i < sizeof(kMimoUrls) / sizeof(kMimoUrls[0]); i++) {
    Serial.printf("[Vision] MiMo endpoint %u/%u\r\n",
                  (unsigned)(i + 1),
                  (unsigned)(sizeof(kMimoUrls) / sizeof(kMimoUrls[0])));
    lastResult = requestOpenAiCompatible(AI_PROVIDER_MIMO, kMimoUrls[i], apiKey,
                                         model, base64Jpeg, outText, outLen);
    if (lastResult == VISION_RESULT_OK) {
      return VISION_RESULT_OK;
    }
    if (lastResult != VISION_RESULT_HTTP_FAIL && lastResult != VISION_RESULT_NO_API) {
      return lastResult;
    }
  }
  return lastResult;
}

VisionResult vision_service_describe_jpeg(const uint8_t *jpeg, size_t jpegLen, char *outText, size_t outLen) {
  if (outText == nullptr || outLen == 0) {
    return VISION_RESULT_HTTP_FAIL;
  }
  outText[0] = '\0';

  if (jpeg == nullptr || jpegLen == 0 || jpegLen > VISION_MAX_JPEG_BYTES) {
    Serial.printf("[Vision] invalid jpeg len=%u\r\n", (unsigned)jpegLen);
    return VISION_RESULT_CAPTURE_FAIL;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Vision] no wifi");
    return VISION_RESULT_NO_WIFI;
  }
  if (!settings_api_has_api_key()) {
    Serial.println("[Vision] no api key");
    return VISION_RESULT_NO_API;
  }

  const AiProvider provider = settings_api_get_provider();
  if (!ai_provider_supports_vision(provider)) {
    Serial.printf("[Vision] provider unsupported: %s\r\n", ai_provider_name(provider));
    return VISION_RESULT_UNSUPPORTED;
  }

  const int modelIndex = settings_api_get_model_index();
  if (!ai_provider_model_supports_vision(provider, modelIndex)) {
    Serial.printf("[Vision] model unsupported for vision: %s\r\n", settings_api_get_model_id());
    return VISION_RESULT_UNSUPPORTED;
  }

  char apiKey[129];
  settings_api_get_api_key(apiKey, sizeof(apiKey));
  const char *model = settings_api_get_model_id();

  Serial.printf("[Vision] JPEG %u bytes, encoding b64...\r\n", (unsigned)jpegLen);
  Serial.flush();

  char *base64 = nullptr;
  size_t base64Len = 0;
  if (!encodeBase64(jpeg, jpegLen, &base64, &base64Len)) {
    Serial.println("[Vision] base64 encode failed");
    return VISION_RESULT_CAPTURE_FAIL;
  }

  Serial.printf("[Vision] b64 %u bytes, provider=%s model=%s, posting...\r\n",
                (unsigned)base64Len, ai_provider_name(provider), model);
  Serial.flush();

  VisionResult result = VISION_RESULT_HTTP_FAIL;
  if (provider == AI_PROVIDER_GEMINI) {
    result = requestGemini(apiKey, model, base64, outText, outLen);
    base64 = nullptr;
  } else if (provider == AI_PROVIDER_MIMO) {
    result = requestMimoCompatible(apiKey, model, base64, outText, outLen);
  } else {
    const char *url = ai_provider_chat_completions_url(provider);
    if (url == nullptr || url[0] == '\0') {
      free(base64);
      return VISION_RESULT_UNSUPPORTED;
    }
    result = requestOpenAiCompatible(provider, url, apiKey, model, base64, outText, outLen);
  }
  free(base64);

  if (result == VISION_RESULT_OK) {
    Serial.printf("[Vision] %s\r\n", outText);
  }

  return result;
}

VisionResult vision_service_describe_camera(char *outText, size_t outLen) {
  if (outText == nullptr || outLen == 0) {
    return VISION_RESULT_HTTP_FAIL;
  }
  outText[0] = '\0';

  Serial.println("[Vision] describe start");
  Serial.flush();

  if (!camera_service_is_ready() && !camera_service_init()) {
    Serial.println("[Vision] no camera");
    return VISION_RESULT_NO_CAMERA;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Vision] no wifi");
    return VISION_RESULT_NO_WIFI;
  }
  if (!settings_api_has_api_key()) {
    Serial.println("[Vision] no api key");
    return VISION_RESULT_NO_API;
  }

  const AiProvider provider = settings_api_get_provider();
  if (!ai_provider_supports_vision(provider)) {
    Serial.printf("[Vision] provider unsupported: %s\r\n", ai_provider_name(provider));
    return VISION_RESULT_UNSUPPORTED;
  }

  const int modelIndex = settings_api_get_model_index();
  if (!ai_provider_model_supports_vision(provider, modelIndex)) {
    Serial.printf("[Vision] model unsupported for vision: %s\r\n", settings_api_get_model_id());
    return VISION_RESULT_UNSUPPORTED;
  }

  char apiKey[129];
  settings_api_get_api_key(apiKey, sizeof(apiKey));
  const char *model = settings_api_get_model_id();

  Serial.println("[Vision] capturing frame...");
  Serial.flush();

  camera_fb_t *warmup = camera_service_capture();
  if (warmup != nullptr) {
    camera_service_release(warmup);
  }

  camera_fb_t *fb = camera_service_capture();
  if (fb == nullptr || fb->len == 0 || fb->len > VISION_MAX_JPEG_BYTES) {
    Serial.printf("[Vision] capture fail fb=%p len=%u\r\n",
                  (void *)fb, fb != nullptr ? (unsigned)fb->len : 0U);
    camera_service_release(fb);
    return VISION_RESULT_CAPTURE_FAIL;
  }

  Serial.printf("[Vision] JPEG %u bytes, encoding b64...\r\n", (unsigned)fb->len);
  Serial.flush();

  char *base64 = nullptr;
  size_t base64Len = 0;
  const size_t jpegLen = fb->len;
  if (!encodeBase64(fb->buf, fb->len, &base64, &base64Len)) {
    Serial.println("[Vision] base64 encode failed");
    camera_service_release(fb);
    return VISION_RESULT_CAPTURE_FAIL;
  }
  camera_service_release(fb);

  /* Camera keeps streaming after init; pause during HTTPS or FB-OVF floods serial. */
  camera_service_pause();

  Serial.printf("[Vision] b64 %u bytes, provider=%s model=%s, posting...\r\n",
                (unsigned)base64Len, ai_provider_name(provider), model);
  Serial.flush();

  VisionResult result = VISION_RESULT_HTTP_FAIL;
  if (provider == AI_PROVIDER_GEMINI) {
    result = requestGemini(apiKey, model, base64, outText, outLen);
    base64 = nullptr;
  } else if (provider == AI_PROVIDER_MIMO) {
    result = requestMimoCompatible(apiKey, model, base64, outText, outLen);
  } else {
    const char *url = ai_provider_chat_completions_url(provider);
    if (url == nullptr || url[0] == '\0') {
      free(base64);
      return VISION_RESULT_UNSUPPORTED;
    }
    result = requestOpenAiCompatible(provider, url, apiKey, model, base64, outText, outLen);
  }
  free(base64);

  if (result == VISION_RESULT_OK) {
    Serial.printf("[Vision] %s\r\n", outText);
  }

  return result;
}
