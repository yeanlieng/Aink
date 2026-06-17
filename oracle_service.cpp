#include "oracle_service.h"

#include "ai_model_config.h"
#include "bookofanswers.h"
#include "camera_service.h"
#include "settings_api.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <stdio.h>
#include <string.h>

#define ORACLE_HTTP_TIMEOUT_MS    60000
#define ORACLE_MAX_JPEG_BYTES     (64 * 1024)
#define ORACLE_MAX_TOKENS         256
#define ORACLE_OUTPUT_MAX_CHARS   20

/*----------------------------------------------------------------------------
 * String builder helpers
 *----------------------------------------------------------------------------*/
static void oracleTrimText(char *text);
static void oracleTruncateText(char *text, size_t maxChars);

static bool oracleAppendChar(char *buf, size_t bufLen, size_t *pos, char c) {
  if (*pos + 1 >= bufLen) {
    return false;
  }
  buf[*pos] = c;
  (*pos)++;
  buf[*pos] = '\0';
  return true;
}

static bool oracleAppendStr(char *buf, size_t bufLen, size_t *pos, const char *text) {
  if (text == nullptr) {
    return true;
  }
  while (*text != '\0') {
    if (!oracleAppendChar(buf, bufLen, pos, *text)) {
      return false;
    }
    text++;
  }
  return true;
}

static bool oracleAppendJsonEscaped(char *buf, size_t bufLen, size_t *pos,
                                    const char *text) {
  if (text == nullptr) {
    return true;
  }
  while (*text != '\0') {
    const char c = *text;
    if (c == '\\' || c == '\"') {
      if (!oracleAppendChar(buf, bufLen, pos, '\\')) {
        return false;
      }
    }
    if (!oracleAppendChar(buf, bufLen, pos, c)) {
      return false;
    }
    text++;
  }
  return true;
}

/*----------------------------------------------------------------------------
 * Base64 encode (mbedtls)
 *----------------------------------------------------------------------------*/
static bool oracleEncodeBase64(const uint8_t *data, size_t dataLen,
                               char **outB64, size_t *outLen) {
  if (data == nullptr || outB64 == nullptr || outLen == nullptr) {
    return false;
  }

  size_t needed = 0;
  if (mbedtls_base64_encode(nullptr, 0, &needed, data, dataLen) !=
      MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    return false;
  }

  char *buffer = static_cast<char *>(
      heap_caps_malloc(needed + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer == nullptr) {
    buffer = static_cast<char *>(malloc(needed + 1));
  }
  if (buffer == nullptr) {
    return false;
  }

  size_t written = 0;
  if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(buffer),
                            needed + 1, &written, data, dataLen) != 0) {
    free(buffer);
    return false;
  }
  buffer[written] = '\0';
  *outB64 = buffer;
  *outLen = written;
  return true;
}

/*----------------------------------------------------------------------------
 * HTTP POST JSON
 *----------------------------------------------------------------------------*/
static bool oracleHttpPostJson(const char *url, const char *authHeader,
                               const char *authValue, char *jsonBody,
                               String *outResponse, int *outHttpCode) {
  if (url == nullptr || jsonBody == nullptr || outResponse == nullptr ||
      outHttpCode == nullptr) {
    return false;
  }

  const size_t bodyLen = strlen(jsonBody);
  IPAddress serverIp;
  bool dnsOk = false;
  const char *hostStart = strstr(url, "://");
  if (hostStart != nullptr) {
    hostStart += 3;
    const char *hostEnd = strchr(hostStart, '/');
    const size_t hostLen =
        hostEnd != nullptr ? (size_t)(hostEnd - hostStart) : strlen(hostStart);
    if (hostLen > 0 && hostLen < 96) {
      char host[96];
      memcpy(host, hostStart, hostLen);
      host[hostLen] = '\0';
      dnsOk = WiFi.hostByName(host, serverIp);
    }
  }
  Serial.printf("[Oracle] HTTP POST %u bytes to %s (heap=%u psram=%u dns=%s)\r\n",
                (unsigned)bodyLen, url,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
                dnsOk ? serverIp.toString().c_str() : "fail");
  Serial.flush();

  WiFi.setSleep(WIFI_PS_NONE);
  yield();

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(ORACLE_HTTP_TIMEOUT_MS);
  client.setHandshakeTimeout(30);

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("[Oracle] HTTP begin failed");
    *outHttpCode = -1;
    *outResponse = "begin failed";
    return false;
  }

  http.setConnectTimeout(30000);
  http.setTimeout(ORACLE_HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");
  if (authHeader != nullptr && authValue != nullptr && authValue[0] != '\0') {
    http.addHeader(authHeader, authValue);
  }

  *outHttpCode = http.POST(reinterpret_cast<uint8_t *>(jsonBody), bodyLen);
  if (*outHttpCode > 0) {
    *outResponse = http.getString();
  } else {
    *outResponse = http.errorToString(*outHttpCode);
    char sslError[120];
    const int sslCode = client.lastError(sslError, sizeof(sslError));
    Serial.printf("[Oracle] POST fail heap=%u block=%u err=%s\r\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                  outResponse->c_str());
    Serial.printf("[Oracle] TLS lastError=%d %s\r\n", sslCode, sslError);
  }
  http.end();
  return *outHttpCode > 0;
}

/*----------------------------------------------------------------------------
 * JSON parsing — extract "content" field from choices/candidates
 *----------------------------------------------------------------------------*/
static bool oracleParseJsonString(const String &body, const char *fieldKey,
                                  char *out, size_t outLen) {
  if (fieldKey == nullptr || out == nullptr || outLen == 0) {
    return false;
  }

  char search[32];
  snprintf(search, sizeof(search), "\"%s\"", fieldKey);
  int idx = body.indexOf(search);
  if (idx < 0) {
    return false;
  }

  idx += (int)strlen(search);
  while (idx < body.length()) {
    const char c = body.charAt(idx);
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      idx++;
      continue;
    }
    break;
  }
  if (idx >= body.length() || body.charAt(idx) != ':') {
    return false;
  }
  idx++;
  while (idx < body.length()) {
    const char c = body.charAt(idx);
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      idx++;
      continue;
    }
    break;
  }
  if (idx >= body.length() || body.charAt(idx) != '\"') {
    return false;
  }
  idx++;

  size_t w = 0;
  for (int i = idx; i < body.length() && w + 1 < outLen; i++) {
    const char c = body.charAt(i);
    if (c == '\"' && i > 0 && body.charAt(i - 1) != '\\') {
      break;
    }
    if (c == '\\' && i + 1 < body.length()) {
      const char next = body.charAt(i + 1);
      if (next == 'n') { out[w++] = '\n'; i++; continue; }
      if (next == 'r') { out[w++] = '\r'; i++; continue; }
      if (next == 't') { out[w++] = '\t'; i++; continue; }
      if (next == '\\' || next == '\"') { out[w++] = next; i++; continue; }
    }
    out[w++] = c;
  }
  out[w] = '\0';
  return w > 0;
}

static bool oracleLooksLikeReasoningChain(const char *text) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  if (strstr(text, "用户现在") != nullptr ||
      strstr(text, "首先") != nullptr ||
      strstr(text, "图像") != nullptr ||
      strstr(text, "照片") != nullptr) {
    return strlen(text) > 48U;
  }
  if (strstr(text, "不对") != nullptr && strlen(text) > 48U) {
    return true;
  }
  return false;
}

static bool oracleExtractLastAsciiQuoted(const String &text, char *out,
                                         size_t outLen) {
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

static void oracleSkipUtf8Prefix(char **cursor, const char *prefix) {
  if (cursor == nullptr || *cursor == nullptr || prefix == nullptr) {
    return;
  }
  const size_t len = strlen(prefix);
  if (len > 0 && strncmp(*cursor, prefix, len) == 0) {
    *cursor += len;
  }
}

static bool oracleExtractFinalClause(const char *text, char *out,
                                     size_t outLen) {
  if (text == nullptr || out == nullptr || outLen == 0) {
    return false;
  }
  out[0] = '\0';

  const char *start = strstr(text, "最终答案");
  if (start != nullptr) {
    start += strlen("最终答案");
  } else {
    start = strstr(text, "答案");
    if (start != nullptr) {
      start += strlen("答案");
    } else {
      start = strstr(text, "最终");
      if (start != nullptr) {
        start += strlen("最终");
      } else {
        start = text;
      }
    }
  }

  char *cursor = const_cast<char *>(start);
  while (*cursor == ' ' || *cursor == '\n' || *cursor == '\r' ||
         *cursor == '\t' || *cursor == ':' || *cursor == '-' ||
         *cursor == '\'' || *cursor == '\"') {
    cursor++;
  }
  bool advanced = true;
  while (advanced) {
    char *before = cursor;
    while (*cursor == ' ' || *cursor == '\n' || *cursor == '\r' ||
           *cursor == '\t' || *cursor == ':' || *cursor == '-' ||
           *cursor == '\'' || *cursor == '\"') {
      cursor++;
    }
    oracleSkipUtf8Prefix(&cursor, "是");
    oracleSkipUtf8Prefix(&cursor, "：");
    oracleSkipUtf8Prefix(&cursor, "「");
    oracleSkipUtf8Prefix(&cursor, "『");
    oracleSkipUtf8Prefix(&cursor, "“");
    advanced = cursor != before;
  }

  size_t w = 0;
  while (*cursor != '\0' && w + 1 < outLen) {
    if (*cursor == '\n' || *cursor == '\r' || *cursor == '\"' ||
        *cursor == '\'' || *cursor == ';') {
      break;
    }
    if (strncmp(cursor, "。", strlen("。")) == 0 ||
        strncmp(cursor, "；", strlen("；")) == 0 ||
        strncmp(cursor, "」", strlen("」")) == 0 ||
        strncmp(cursor, "』", strlen("』")) == 0 ||
        strncmp(cursor, "”", strlen("”")) == 0) {
      break;
    }
    out[w++] = *cursor++;
  }
  out[w] = '\0';
  oracleTrimText(out);
  oracleTruncateText(out, ORACLE_OUTPUT_MAX_CHARS);
  return out[0] != '\0';
}

static void oracleNormalizeOutput(const char *content, const char *reasoning,
                                  char *out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return;
  }
  out[0] = '\0';

  char candidate[384];
  candidate[0] = '\0';

  if (content != nullptr && content[0] != '\0' &&
      !oracleLooksLikeReasoningChain(content)) {
    snprintf(candidate, sizeof(candidate), "%s", content);
  } else if (content != nullptr && content[0] != '\0') {
    if (!oracleExtractLastAsciiQuoted(String(content), candidate,
                                      sizeof(candidate)) &&
        !oracleExtractFinalClause(content, candidate, sizeof(candidate))) {
      snprintf(candidate, sizeof(candidate), "%s", content);
    }
  } else if (reasoning != nullptr && reasoning[0] != '\0') {
    if (!oracleExtractLastAsciiQuoted(String(reasoning), candidate,
                                      sizeof(candidate))) {
      oracleExtractFinalClause(reasoning, candidate, sizeof(candidate));
    }
  }

  oracleTrimText(candidate);
  oracleTruncateText(candidate, ORACLE_OUTPUT_MAX_CHARS);
  snprintf(out, outLen, "%s", candidate);
}

static bool oracleExtractContent(const String &body, char *out, size_t outLen) {
  if (out != nullptr && outLen > 0) {
    out[0] = '\0';
  }
  // Try OpenAI format: "choices" → "content"
  int idx = body.indexOf("\"choices\"");
  if (idx >= 0) {
    const String tail = body.substring(idx);
    char content[384];
    content[0] = '\0';
    oracleParseJsonString(tail, "content", content, sizeof(content));
    if (content[0] != '\0' && !oracleLooksLikeReasoningChain(content)) {
      oracleNormalizeOutput(content, nullptr, out, outLen);
      if (out[0] != '\0') {
        return true;
      }
    }

    char *reasoning = static_cast<char *>(
        heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (reasoning == nullptr) {
      reasoning = static_cast<char *>(malloc(4096));
    }
    if (reasoning != nullptr) {
      reasoning[0] = '\0';
      if (oracleParseJsonString(tail, "reasoning_content", reasoning, 4096)) {
        oracleNormalizeOutput(content, reasoning, out, outLen);
      }
      free(reasoning);
      if (out[0] != '\0') {
        return true;
      }
    }

    if (content[0] != '\0') {
      oracleNormalizeOutput(content, nullptr, out, outLen);
      return out[0] != '\0';
    }
  }

  // Try Gemini format: "candidates" → "text"
  idx = body.indexOf("\"candidates\"");
  if (idx >= 0) {
    const String tail = body.substring(idx);
    char text[384];
    text[0] = '\0';
    if (oracleParseJsonString(tail, "text", text, sizeof(text)) ||
        oracleParseJsonString(tail, "content", text, sizeof(text))) {
      oracleNormalizeOutput(text, nullptr, out, outLen);
      return out[0] != '\0';
    }
  }
  return false;
}

/*----------------------------------------------------------------------------
 * Text trimming and UTF-8 safe truncation
 *----------------------------------------------------------------------------*/
static void oracleTrimText(char *text) {
  if (text == nullptr) {
    return;
  }

  size_t start = 0;
  while (text[start] == ' ' || text[start] == '\n' ||
         text[start] == '\r' || text[start] == '\t') {
    start++;
  }
  if (start > 0) {
    memmove(text, text + start, strlen(text + start) + 1);
  }

  const size_t len = strlen(text);
  size_t end = len;
  while (end > 0) {
    const char c = text[end - 1];
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t' ||
        c == '\"' || c == '\'') {
      end--;
      continue;
    }
    break;
  }
  text[end] = '\0';

  // Remove leading quote after trim (may expose nested quote)
  if (text[0] == '\"' || text[0] == '\'') {
    memmove(text, text + 1, strlen(text));
    oracleTrimText(text);
  }
}

static void oracleTruncateText(char *text, size_t maxChars) {
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

/*----------------------------------------------------------------------------
 * Fixed oracle prompts (Chinese-only, locale-independent)
 *----------------------------------------------------------------------------*/
static const char *oracleSystemPrompt(void) {
  return "基于图像模仿‘答案之书’，输出 ≤20 个中文字符";
}

static const char *oracleUserPrompt(void) {
  return "基于图像模仿‘答案之书’，输出 ≤20 个中文字符";
}

/*----------------------------------------------------------------------------
 * JSON body builders (OpenAI-compatible / Gemini)
 *----------------------------------------------------------------------------*/
static bool oracleBuildOpenAiBody(const char *model, const char *systemText,
                                  const char *userText, const char *base64Jpeg,
                                  const char *maxTokensKey, char **outBody) {
  const size_t bodyCap = strlen(model) + strlen(systemText) + strlen(userText) +
                         strlen(base64Jpeg) + 1024;
  char *body = static_cast<char *>(
      heap_caps_malloc(bodyCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (body == nullptr) {
    body = static_cast<char *>(malloc(bodyCap));
  }
  if (body == nullptr) {
    return false;
  }

  size_t pos = 0;
  body[0] = '\0';
  char tokenBuf[12];
  snprintf(tokenBuf, sizeof(tokenBuf), "%d", ORACLE_MAX_TOKENS);
  if (!oracleAppendStr(body, bodyCap, &pos, "{\"model\":\"") ||
      !oracleAppendStr(body, bodyCap, &pos, model) ||
      !oracleAppendStr(body, bodyCap, &pos, "\",\"") ||
      !oracleAppendStr(body, bodyCap, &pos, maxTokensKey) ||
      !oracleAppendStr(body, bodyCap, &pos, "\":") ||
      !oracleAppendStr(body, bodyCap, &pos, tokenBuf) ||
      !oracleAppendStr(body, bodyCap, &pos,
                       ",\"messages\":[{\"role\":\"system\",\"content\":\"") ||
      !oracleAppendJsonEscaped(body, bodyCap, &pos, systemText) ||
      !oracleAppendStr(body, bodyCap, &pos,
                       "\"},{\"role\":\"user\",\"content\":[{\"type\":"
                       "\"image_url\",\"image_url\":{\"url\":\"data:image/"
                       "jpeg;base64,") ||
      !oracleAppendStr(body, bodyCap, &pos, base64Jpeg) ||
      !oracleAppendStr(body, bodyCap, &pos,
                       "\"}},{\"type\":\"text\",\"text\":\"") ||
      !oracleAppendJsonEscaped(body, bodyCap, &pos, userText) ||
      !oracleAppendStr(body, bodyCap, &pos, "\"}]}]}")) {
    free(body);
    return false;
  }

  *outBody = body;
  return true;
}

static bool oracleBuildGeminiBody(const char *systemText, const char *userText,
                                  const char *base64Jpeg, char **outBody) {
  char combined[384];
  snprintf(combined, sizeof(combined), "%s\n\n%s", systemText, userText);

  const size_t bodyCap = strlen(combined) + strlen(base64Jpeg) + 256;
  char *body = static_cast<char *>(
      heap_caps_malloc(bodyCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (body == nullptr) {
    body = static_cast<char *>(malloc(bodyCap));
  }
  if (body == nullptr) {
    return false;
  }

  size_t pos = 0;
  body[0] = '\0';
  char tokenBuf[12];
  snprintf(tokenBuf, sizeof(tokenBuf), "%d", ORACLE_MAX_TOKENS);
  if (!oracleAppendStr(body, bodyCap, &pos,
                       "{\"contents\":[{\"parts\":[{\"text\":\"") ||
      !oracleAppendJsonEscaped(body, bodyCap, &pos, combined) ||
      !oracleAppendStr(body, bodyCap, &pos,
                       "\"},{\"inline_data\":{\"mime_type\":\"image/"
                       "jpeg\",\"data\":\"") ||
      !oracleAppendStr(body, bodyCap, &pos, base64Jpeg) ||
      !oracleAppendStr(body, bodyCap, &pos,
                       "\"}}]}],\"generationConfig\":{\"maxOutputTokens\":") ||
      !oracleAppendStr(body, bodyCap, &pos, tokenBuf) ||
      !oracleAppendStr(body, bodyCap, &pos, "}}")) {
    free(body);
    return false;
  }

  *outBody = body;
  return true;
}

/*----------------------------------------------------------------------------
 * Provider request functions
 *----------------------------------------------------------------------------*/
static OracleResult oracleRequestOpenAi(AiProvider provider, const char *url,
                                        const char *apiKey, const char *model,
                                        const char *base64Jpeg, char *outText,
                                        size_t outLen) {
  char bearerAuth[160];
  snprintf(bearerAuth, sizeof(bearerAuth), "Bearer %s", apiKey);

  const char *maxTokensKey =
      provider == AI_PROVIDER_MIMO ? "max_completion_tokens" : "max_tokens";

  char *body = nullptr;
  if (!oracleBuildOpenAiBody(model, oracleSystemPrompt(), oracleUserPrompt(),
                             base64Jpeg, maxTokensKey, &body)) {
    return ORACLE_RESULT_HTTP_FAIL;
  }

  String response;
  int httpCode = 0;
  const bool posted = oracleHttpPostJson(url, "Authorization", bearerAuth,
                                         body, &response, &httpCode);
  free(body);

  if (!posted || httpCode < 200 || httpCode >= 300) {
    Serial.printf("[Oracle] HTTP %d %s\r\n", httpCode, response.c_str());
    if (httpCode == 401 || httpCode == 403) {
      return ORACLE_RESULT_NO_API;
    }
    return ORACLE_RESULT_HTTP_FAIL;
  }

  if (!oracleExtractContent(response, outText, outLen)) {
    Serial.printf("[Oracle] parse fail %s\r\n", response.c_str());
    return ORACLE_RESULT_PARSE_FAIL;
  }
  oracleTrimText(outText);
  oracleTruncateText(outText, ORACLE_OUTPUT_MAX_CHARS);
  return ORACLE_RESULT_OK;
}

static OracleResult oracleRequestGemini(const char *apiKey, const char *model,
                                        const char *base64Jpeg, char *outText,
                                        size_t outLen) {
  char url[192];
  snprintf(url, sizeof(url),
           "https://generativelanguage.googleapis.com/v1beta/models/%s"
           ":generateContent?key=%s",
           model, apiKey);

  char *body = nullptr;
  if (!oracleBuildGeminiBody(oracleSystemPrompt(), oracleUserPrompt(),
                             base64Jpeg, &body)) {
    return ORACLE_RESULT_HTTP_FAIL;
  }

  String response;
  int httpCode = 0;
  const bool posted =
      oracleHttpPostJson(url, nullptr, nullptr, body, &response, &httpCode);
  free(body);

  if (!posted || httpCode < 200 || httpCode >= 300) {
    Serial.printf("[Oracle] Gemini HTTP %d %s\r\n", httpCode, response.c_str());
    if (httpCode == 401 || httpCode == 403) {
      return ORACLE_RESULT_NO_API;
    }
    return ORACLE_RESULT_HTTP_FAIL;
  }

  if (!oracleExtractContent(response, outText, outLen)) {
    Serial.printf("[Oracle] Gemini parse fail %s\r\n", response.c_str());
    return ORACLE_RESULT_PARSE_FAIL;
  }
  oracleTrimText(outText);
  oracleTruncateText(outText, ORACLE_OUTPUT_MAX_CHARS);
  return ORACLE_RESULT_OK;
}

static OracleResult oracleRequestMimo(const char *apiKey, const char *model,
                                      const char *base64Jpeg, char *outText,
                                      size_t outLen) {
  static const char *kMimoUrls[] = {
      "https://api.xiaomimimo.com/v1/chat/completions",
      "https://token-plan-cn.xiaomimimo.com/v1/chat/completions",
      "https://token-plan-sgp.xiaomimimo.com/v1/chat/completions",
      "https://token-plan-ams.xiaomimimo.com/v1/chat/completions",
  };

  OracleResult lastResult = ORACLE_RESULT_HTTP_FAIL;
  for (size_t i = 0; i < sizeof(kMimoUrls) / sizeof(kMimoUrls[0]); i++) {
    Serial.printf("[Oracle] MiMo endpoint %u/%u\r\n",
                  (unsigned)(i + 1),
                  (unsigned)(sizeof(kMimoUrls) / sizeof(kMimoUrls[0])));
    lastResult = oracleRequestOpenAi(AI_PROVIDER_MIMO, kMimoUrls[i], apiKey,
                                     model, base64Jpeg, outText, outLen);
    if (lastResult == ORACLE_RESULT_OK) {
      return ORACLE_RESULT_OK;
    }
    if (lastResult != ORACLE_RESULT_HTTP_FAIL &&
        lastResult != ORACLE_RESULT_NO_API) {
      return lastResult;
    }
  }
  return lastResult;
}

OracleResult oracle_service_ask_jpeg(const uint8_t *jpeg, size_t jpegLen,
                                     char *outText, size_t outLen) {
  if (outText == nullptr || outLen == 0) {
    return ORACLE_RESULT_HTTP_FAIL;
  }
  outText[0] = '\0';

  if (jpeg == nullptr || jpegLen == 0 || jpegLen > ORACLE_MAX_JPEG_BYTES) {
    Serial.printf("[Oracle] invalid jpeg len=%u\r\n", (unsigned)jpegLen);
    return ORACLE_RESULT_CAPTURE_FAIL;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Oracle] no wifi");
    return ORACLE_RESULT_NO_WIFI;
  }
  if (!settings_api_has_api_key()) {
    Serial.println("[Oracle] no api key");
    return ORACLE_RESULT_NO_API;
  }

  const AiProvider provider = settings_api_get_provider();
  if (!ai_provider_supports_vision(provider)) {
    Serial.printf("[Oracle] provider unsupported: %s\r\n",
                  ai_provider_name(provider));
    return ORACLE_RESULT_UNSUPPORTED;
  }

  const int modelIndex = settings_api_get_model_index();
  if (!ai_provider_model_supports_vision(provider, modelIndex)) {
    Serial.printf("[Oracle] model unsupported for vision: %s\r\n",
                  settings_api_get_model_id());
    return ORACLE_RESULT_UNSUPPORTED;
  }

  char apiKey[129];
  settings_api_get_api_key(apiKey, sizeof(apiKey));
  const char *model = settings_api_get_model_id();

  Serial.printf("[Oracle] mosaic JPEG %u bytes, encoding b64...\r\n",
                (unsigned)jpegLen);
  Serial.flush();

  char *base64 = nullptr;
  size_t base64Len = 0;
  if (!oracleEncodeBase64(jpeg, jpegLen, &base64, &base64Len)) {
    Serial.println("[Oracle] base64 encode fail");
    return ORACLE_RESULT_CAPTURE_FAIL;
  }

  Serial.printf("[Oracle] b64 %u bytes, provider=%s model=%s, posting...\r\n",
                (unsigned)base64Len, ai_provider_name(provider), model);
  Serial.flush();

  OracleResult result = ORACLE_RESULT_HTTP_FAIL;
  if (provider == AI_PROVIDER_GEMINI) {
    result = oracleRequestGemini(apiKey, model, base64, outText, outLen);
  } else if (provider == AI_PROVIDER_MIMO) {
    result = oracleRequestMimo(apiKey, model, base64, outText, outLen);
  } else {
    const char *url = ai_provider_chat_completions_url(provider);
    if (url == nullptr || url[0] == '\0') {
      result = ORACLE_RESULT_UNSUPPORTED;
    } else {
      result = oracleRequestOpenAi(provider, url, apiKey, model, base64,
                                   outText, outLen);
    }
  }
  free(base64);

  if (result == ORACLE_RESULT_OK) {
    Serial.printf("[Oracle] result: %s\r\n", outText);
  }
  return result;
}

/*----------------------------------------------------------------------------
 * Public API
 *----------------------------------------------------------------------------*/
OracleResult oracle_service_ask(char *outText, size_t outLen) {
  if (outText == nullptr || outLen == 0) {
    return ORACLE_RESULT_HTTP_FAIL;
  }
  outText[0] = '\0';

#define FALLBACK_LOCAL()                                                \
  do {                                                                  \
    snprintf(outText, outLen, "%s", bookofanswers_random());           \
    return ORACLE_RESULT_LOCAL_FALLBACK;                                \
  } while (0)

  // Prerequisite checks
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Oracle] no wifi, using local fallback");
    FALLBACK_LOCAL();
  }
  if (!settings_api_has_api_key()) {
    Serial.println("[Oracle] no api key, using local fallback");
    FALLBACK_LOCAL();
  }

  const AiProvider provider = settings_api_get_provider();
  if (!ai_provider_supports_vision(provider)) {
    Serial.printf("[Oracle] provider unsupported: %s\r\n",
                  ai_provider_name(provider));
    FALLBACK_LOCAL();
  }

  const int modelIndex = settings_api_get_model_index();
  if (!ai_provider_model_supports_vision(provider, modelIndex)) {
    Serial.printf("[Oracle] model unsupported for vision: %s\r\n",
                  settings_api_get_model_id());
    FALLBACK_LOCAL();
  }

  char apiKey[129];
  settings_api_get_api_key(apiKey, sizeof(apiKey));
  const char *model = settings_api_get_model_id();

  if (!camera_service_is_ready() && !camera_service_init()) {
    Serial.println("[Oracle] no camera, using local fallback");
    FALLBACK_LOCAL();
  }

  // Capture frame
  Serial.println("[Oracle] capturing frame...");
  Serial.flush();

  camera_fb_t *warmup = camera_service_capture();
  if (warmup != nullptr) {
    camera_service_release(warmup);
  }

  camera_fb_t *fb = camera_service_capture();
  if (fb == nullptr || fb->len == 0 || fb->len > ORACLE_MAX_JPEG_BYTES) {
    Serial.printf("[Oracle] capture fail fb=%p len=%u\r\n",
                  (void *)fb, fb != nullptr ? (unsigned)fb->len : 0U);
    camera_service_release(fb);
    camera_service_pause();
    FALLBACK_LOCAL();
  }

  Serial.printf("[Oracle] JPEG %u bytes, encoding b64...\r\n",
                (unsigned)fb->len);
  Serial.flush();

  // Base64 encode the captured JPEG directly. Keeping the device-side image
  // path identical to AI Vision avoids an extra decode/re-encode failure point.
  char *base64 = nullptr;
  size_t base64Len = 0;
  if (!oracleEncodeBase64(fb->buf, fb->len, &base64, &base64Len)) {
    camera_service_release(fb);
    camera_service_pause();
    Serial.println("[Oracle] base64 encode fail, using local fallback");
    FALLBACK_LOCAL();
  }
  camera_service_release(fb);

  Serial.printf("[Oracle] b64 %u bytes, provider=%s model=%s, posting...\r\n",
                (unsigned)base64Len, ai_provider_name(provider), model);
  Serial.flush();

  // Pause camera during HTTPS
  camera_service_pause();

  // Dispatch to provider
  OracleResult result = ORACLE_RESULT_HTTP_FAIL;
  if (provider == AI_PROVIDER_GEMINI) {
    result = oracleRequestGemini(apiKey, model, base64, outText, outLen);
  } else if (provider == AI_PROVIDER_MIMO) {
    result = oracleRequestMimo(apiKey, model, base64, outText, outLen);
  } else {
    const char *url = ai_provider_chat_completions_url(provider);
    if (url == nullptr || url[0] == '\0') {
      result = ORACLE_RESULT_UNSUPPORTED;
    } else {
      result = oracleRequestOpenAi(provider, url, apiKey, model, base64,
                                   outText, outLen);
    }
  }
  free(base64);

  if (result != ORACLE_RESULT_OK) {
    Serial.println("[Oracle] online failed, using local fallback");
    FALLBACK_LOCAL();
  }

  Serial.printf("[Oracle] result: %s\r\n", outText);
  return ORACLE_RESULT_OK;

#undef FALLBACK_LOCAL
}
