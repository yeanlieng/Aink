#include "vision_service.h"

#include "ai_model_config.h"
#include "app_locale.h"
#include "bookofanswers.h"
#include "camera_service.h"
#include "settings_api.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include "img_converters.h"
#include <mbedtls/base64.h>
#include <stdio.h>
#include <string.h>

#define VISION_HTTP_TIMEOUT_MS  60000
#define VISION_BOOK_HTTP_TIMEOUT_MS 30000
#define VISION_MAX_JPEG_BYTES   (64 * 1024)
#define VISION_MAX_TOKENS       1024
#define VISION_OUTPUT_MAX_CHARS 40
#define VISION_BOOK_MAX_TOKENS  256
#define VISION_BOOK_OUTPUT_MAX_CHARS 20
#define VISION_CAMERA_FRAME_WIDTH  240
#define VISION_CAMERA_FRAME_HEIGHT 240
#define VISION_BOOK_OBFUSCATE_BLOCK_PX 8

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

static const char *describe_system_prompt(void) {
  if (app_locale_get() == APP_LANG_ZH) {
    return "你是墨水屏诗人。根据照片写一句中文，不超过40字，凝练有诗意。"
           "只输出这一句正文，不要思考过程、不要解释、不要引号、不要标题。";
  }
  return "You are a poet for e-paper. Write one concise poetic sentence about the photo. "
         "At most 40 words. Output only the final sentence, no reasoning or explanation.";
}

static const char *describe_user_prompt(void) {
  return app_locale_get() == APP_LANG_ZH ? "请直接输出一句描述。" : "Reply with one sentence only.";
}

static const char *book_system_prompt(void) {
  return "你是一个中文短答生成器。根据这张经过隐私混淆的照片，输出一句像签语一样的中文答案。"
         "只输出答案本身，6到14个汉字，不要英文、不要书名、不要应用名、不要标题、不要解释、不要引号。"
         "禁止输出思考过程、推理链或分析，直接给出最终答案。";
}

static const char *book_user_prompt(void) {
  return "直接输出一句简体中文短答，不要思考过程，不要解释，不要标题、书名、应用名或英文。";
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
                         char *jsonBody, uint32_t timeoutMs,
                         String *outResponse, int *outHttpCode) {
  if (url == nullptr || jsonBody == nullptr || outResponse == nullptr || outHttpCode == nullptr) {
    return false;
  }
  if (timeoutMs == 0) {
    timeoutMs = VISION_HTTP_TIMEOUT_MS;
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
  Serial.printf("[Vision] HTTP POST %u bytes to %s (heap=%u psram=%u block=%u dma=%u dns=%s)\r\n",
                (unsigned)bodyLen, url,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                dnsOk ? serverIp.toString().c_str() : "fail");
  if (ESP.getFreePsram() == 0) {
    Serial.println("[Vision] warn: PSRAM=0, enable OPI PSRAM in board settings");
  }
  Serial.flush();

  WiFi.setSleep(WIFI_PS_NONE);
  yield();

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout((int)timeoutMs);
  client.setHandshakeTimeout(timeoutMs < 30000U ? 15 : 30);

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("[Vision] HTTP begin failed");
    *outHttpCode = -1;
    *outResponse = "begin failed";
    return false;
  }

  http.setConnectTimeout((int)(timeoutMs < 30000U ? timeoutMs : 30000U));
  http.setTimeout((int)timeoutMs);
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
    Serial.printf("[Vision] POST fail heap=%u psram=%u block=%u dma=%u err=%s\r\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                  outResponse->c_str());
    Serial.printf("[Vision] TLS lastError=%d %s\r\n", sslCode, sslError);
  }
  http.end();
  return *outHttpCode > 0;
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
    if (c == '\"') {
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
        if (!appendChar(out, outLen, &w, next)) {
          break;
        }
        i++;
        continue;
      }
      if (next == 'b' || next == 'f') {
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

static bool looksLikeReasoningChain(const char *text) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  if (strstr(text, "用户现在") != nullptr || strstr(text, "首先看") != nullptr ||
      strstr(text, "用户查询") != nullptr || strstr(text, "首先，") != nullptr ||
      strstr(text, "首先,") != nullptr) {
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

static bool extractLastCornerQuoted(const String &text, char *out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return false;
  }
  out[0] = '\0';

  int cornerClose = -1;
  for (int i = text.length() - 3; i >= 0; i--) {
    if ((uint8_t)text.charAt(i) == 0xE3 && (uint8_t)text.charAt(i + 1) == 0x80 &&
        (uint8_t)text.charAt(i + 2) == 0x8D) {
      cornerClose = i;
      break;
    }
  }
  if (cornerClose < 3) {
    return false;
  }

  int cornerOpen = -1;
  for (int i = cornerClose - 3; i >= 0; i--) {
    if ((uint8_t)text.charAt(i) == 0xE3 && (uint8_t)text.charAt(i + 1) == 0x80 &&
        (uint8_t)text.charAt(i + 2) == 0x8C) {
      cornerOpen = i;
      break;
    }
  }
  if (cornerOpen < 0 || cornerOpen + 3 >= cornerClose) {
    return false;
  }

  const String quoted = text.substring(cornerOpen + 3, cornerClose);
  if (quoted.length() == 0 || quoted.length() >= (int)outLen) {
    return false;
  }
  quoted.toCharArray(out, outLen);
  return out[0] != '\0';
}

static bool extractLastQuotedAnswer(const String &text, char *out, size_t outLen) {
  if (extractLastCornerQuoted(text, out, outLen)) {
    return true;
  }
  return extractLastAsciiQuoted(text, out, outLen);
}

static void trimVisionOutput(char *text);

static size_t utf8CharLen(unsigned char c) {
  if (c >= 0xF0) {
    return 4;
  }
  if (c >= 0xE0) {
    return 3;
  }
  if (c >= 0xC0) {
    return 2;
  }
  return 1;
}

static bool isCjkLeadByte(unsigned char c) {
  return c >= 0xE0 && c <= 0xEF;
}

static bool extractBestCjkPhrase(const char *text, char *out, size_t outLen, size_t minChars,
                                 size_t maxChars) {
  if (text == nullptr || out == nullptr || outLen == 0) {
    return false;
  }
  out[0] = '\0';

  const char *bestStart = nullptr;
  size_t bestLen = 0;

  for (const char *p = text; *p != '\0';) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (!isCjkLeadByte(c)) {
      p += utf8CharLen(c);
      continue;
    }

    const char *start = p;
    size_t chars = 0;
    while (*p != '\0') {
      const unsigned char ch = static_cast<unsigned char>(*p);
      if (isCjkLeadByte(ch)) {
        const size_t step = utf8CharLen(ch);
        if (p[step - 1] == '\0' && step > 1) {
          break;
        }
        p += step;
        chars++;
        continue;
      }
      break;
    }

    const size_t byteLen = static_cast<size_t>(p - start);
    if (chars >= minChars && chars <= maxChars && byteLen > 0 && byteLen < outLen) {
      bestStart = start;
      bestLen = byteLen;
    }
  }

  if (bestStart == nullptr || bestLen == 0) {
    return false;
  }

  memcpy(out, bestStart, bestLen);
  out[bestLen] = '\0';
  trimVisionOutput(out);
  if (looksLikeReasoningChain(out)) {
    out[0] = '\0';
    return false;
  }
  return out[0] != '\0';
}

static bool extractAnswerFromText(const char *text, char *out, size_t outLen, size_t minChars,
                                  size_t maxChars) {
  if (text == nullptr || text[0] == '\0' || out == nullptr || outLen == 0) {
    return false;
  }
  if (extractLastQuotedAnswer(String(text), out, outLen)) {
    return true;
  }
  const char *finalMark = strstr(text, "最终");
  const char *source = finalMark != nullptr ? finalMark : text;
  if (extractLastQuotedAnswer(String(source), out, outLen)) {
    return true;
  }
  return extractBestCjkPhrase(source, out, outLen, minChars, maxChars);
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

static bool containsAsciiIgnoreCase(const char *text, const char *needle) {
  if (text == nullptr || needle == nullptr || needle[0] == '\0') {
    return false;
  }

  for (const char *p = text; *p != '\0'; p++) {
    const char *a = p;
    const char *b = needle;
    while (*a != '\0' && *b != '\0') {
      char ca = *a;
      char cb = *b;
      if (ca >= 'A' && ca <= 'Z') {
        ca = (char)(ca - 'A' + 'a');
      }
      if (cb >= 'A' && cb <= 'Z') {
        cb = (char)(cb - 'A' + 'a');
      }
      if (ca != cb) {
        break;
      }
      a++;
      b++;
    }
    if (*b == '\0') {
      return true;
    }
  }
  return false;
}

static bool bookAnswerLooksLikeTitle(const char *text) {
  if (text == nullptr || text[0] == '\0') {
    return true;
  }
  if (containsAsciiIgnoreCase(text, "book of answers") ||
      containsAsciiIgnoreCase(text, "the book of answers") ||
      strstr(text, "答案之书") != nullptr ||
      strstr(text, "答案书") != nullptr) {
    return true;
  }

  bool hasChinese = false;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p != '\0'; p++) {
    if (*p >= 0xE0) {
      hasChinese = true;
      break;
    }
  }
  return !hasChinese;
}

static void normalizeVisionOutput(const char *content, const char *reasoning,
                                  size_t maxChars, char *out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return;
  }
  out[0] = '\0';

  char candidate[384];
  candidate[0] = '\0';

  if (content != nullptr && content[0] != '\0' && !looksLikeReasoningChain(content)) {
    snprintf(candidate, sizeof(candidate), "%s", content);
  } else if (content != nullptr && content[0] != '\0') {
    if (!extractAnswerFromText(content, candidate, sizeof(candidate), 4, maxChars)) {
      snprintf(candidate, sizeof(candidate), "%s", content);
    }
  } else if (reasoning != nullptr && reasoning[0] != '\0') {
    if (!extractAnswerFromText(reasoning, candidate, sizeof(candidate), 4, maxChars)) {
      return;
    }
    Serial.println("[Vision] parse used reasoning fallback");
  }

  trimVisionOutput(candidate);
  truncateVisionOutput(candidate, maxChars);
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

static bool parseProviderText(const String &body, const char *sectionKey,
                              size_t maxChars, char *out, size_t outLen) {
  const int idx = body.indexOf(sectionKey);
  const String tail = idx >= 0 ? body.substring(idx) : body;

  if (sectionKey != nullptr && strstr(sectionKey, "choices") != nullptr) {
    char content[384];
    content[0] = '\0';
    parseVisionMessageFields(body, content, sizeof(content));

    if (content[0] != '\0' && !looksLikeReasoningChain(content)) {
      normalizeVisionOutput(content, nullptr, maxChars, out, outLen);
      if (out[0] != '\0') {
        return true;
      }
    }

    char *reasoning = static_cast<char *>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (reasoning == nullptr) {
      reasoning = static_cast<char *>(malloc(4096));
    }
    bool hasReasoning = false;
    if (reasoning != nullptr) {
      reasoning[0] = '\0';
      const int choicesIdx = body.indexOf("\"choices\"");
      const String choicesTail = choicesIdx >= 0 ? body.substring(choicesIdx) : body;
      hasReasoning = parseJsonStringField(choicesTail, "reasoning_content", reasoning, 4096);
      if (hasReasoning) {
        normalizeVisionOutput(content, reasoning, maxChars, out, outLen);
        if (out[0] != '\0') {
          free(reasoning);
          return true;
        }
      }
    }

    char quoted[128];
    if (content[0] != '\0' && extractLastQuotedAnswer(String(content), quoted, sizeof(quoted))) {
      trimVisionOutput(quoted);
      truncateVisionOutput(quoted, maxChars);
      snprintf(out, outLen, "%s", quoted);
      if (reasoning != nullptr) {
        free(reasoning);
      }
      return out[0] != '\0';
    }

    if (content[0] != '\0' &&
        extractAnswerFromText(content, out, outLen, 4, maxChars)) {
      trimVisionOutput(out);
      truncateVisionOutput(out, maxChars);
      if (reasoning != nullptr) {
        free(reasoning);
      }
      return out[0] != '\0';
    }

    if (hasReasoning && extractAnswerFromText(reasoning, out, outLen, 4, maxChars)) {
      Serial.println("[Vision] parse used reasoning fallback");
      trimVisionOutput(out);
      truncateVisionOutput(out, maxChars);
      if (reasoning != nullptr) {
        free(reasoning);
      }
      return out[0] != '\0';
    }

    if (content[0] != '\0') {
      normalizeVisionOutput(content, nullptr, maxChars, out, outLen);
      if (reasoning != nullptr) {
        free(reasoning);
      }
      return out[0] != '\0';
    }
    if (reasoning != nullptr) {
      free(reasoning);
    }
    return false;
  }

  char raw[384];
  if (parseJsonStringField(tail, "text", raw, sizeof(raw)) ||
      parseJsonStringField(tail, "content", raw, sizeof(raw))) {
    normalizeVisionOutput(raw, nullptr, maxChars, out, outLen);
    return out[0] != '\0';
  }
  return false;
}

static bool buildOpenAiCompatibleBody(const char *model, const char *systemText, const char *userText,
                                      const char *base64Jpeg, const char *maxTokensKey,
                                      int maxTokens, bool disableThinking, char **outBody) {
  const size_t bodyCap = strlen(model) + strlen(systemText) + strlen(userText) + strlen(base64Jpeg) + 1100;
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
  snprintf(tokenBuf, sizeof(tokenBuf), "%d", maxTokens);
  if (!appendStr(body, bodyCap, &pos, "{\"model\":\"") ||
      !appendStr(body, bodyCap, &pos, model) ||
      !appendStr(body, bodyCap, &pos, "\",\"") ||
      !appendStr(body, bodyCap, &pos, maxTokensKey) ||
      !appendStr(body, bodyCap, &pos, "\":") ||
      !appendStr(body, bodyCap, &pos, tokenBuf) ||
      (disableThinking && !appendStr(body, bodyCap, &pos, ",\"thinking\":{\"type\":\"disabled\"}")) ||
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
                            int maxTokens, char **outBody) {
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
  snprintf(tokenBuf, sizeof(tokenBuf), "%d", maxTokens);
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
                                            const char *model, const char *base64Jpeg,
                                            const char *systemText, const char *userText,
                                            int maxTokens, size_t maxChars, uint32_t timeoutMs,
                                            char *outText, size_t outLen) {
  char bearerAuth[160];
  snprintf(bearerAuth, sizeof(bearerAuth), "Bearer %s", apiKey);
  const char *authHeader = "Authorization";
  const char *authValue = bearerAuth;

  const char *maxTokensKey =
      provider == AI_PROVIDER_MIMO ? "max_completion_tokens" : "max_tokens";

  char *body = nullptr;
  const bool disableThinking = provider == AI_PROVIDER_MIMO;
  if (!buildOpenAiCompatibleBody(model, systemText, userText, base64Jpeg,
                                 maxTokensKey, maxTokens, disableThinking, &body)) {
    return VISION_RESULT_HTTP_FAIL;
  }

  String response;
  int httpCode = 0;
  const bool posted = httpPostJson(url, authHeader, authValue, nullptr, nullptr,
                                   body, timeoutMs, &response, &httpCode);
  free(body);

  if (!posted || httpCode < 200 || httpCode >= 300) {
    Serial.printf("[Vision] HTTP %d %s\r\n", httpCode, response.c_str());
    if (httpCode == 401 || httpCode == 403) {
      return VISION_RESULT_NO_API;
    }
    return VISION_RESULT_HTTP_FAIL;
  }

  if (!parseProviderText(response, "\"choices\"", maxChars, outText, outLen)) {
    Serial.printf("[Vision] parse fail %s\r\n", response.c_str());
    return VISION_RESULT_PARSE_FAIL;
  }
  return VISION_RESULT_OK;
}

static VisionResult requestGemini(const char *apiKey, const char *model, const char *base64Jpeg,
                                  const char *systemText, const char *userText,
                                  int maxTokens, size_t maxChars, uint32_t timeoutMs,
                                  char *outText, size_t outLen) {
  char url[192];
  snprintf(url, sizeof(url),
           "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent?key=%s",
           model, apiKey);

  char *body = nullptr;
  if (!buildGeminiBody(systemText, userText, base64Jpeg, maxTokens, &body)) {
    return VISION_RESULT_HTTP_FAIL;
  }

  String response;
  int httpCode = 0;
  const bool posted = httpPostJson(url, nullptr, nullptr, nullptr, nullptr,
                                   body, timeoutMs, &response, &httpCode);
  free(body);

  if (!posted || httpCode < 200 || httpCode >= 300) {
    Serial.printf("[Vision] Gemini HTTP %d %s\r\n", httpCode, response.c_str());
    return VISION_RESULT_HTTP_FAIL;
  }

  if (!parseProviderText(response, "\"candidates\"", maxChars, outText, outLen)) {
    Serial.printf("[Vision] Gemini parse fail %s\r\n", response.c_str());
    return VISION_RESULT_PARSE_FAIL;
  }
  return VISION_RESULT_OK;
}

static VisionResult requestMimoCompatible(const char *apiKey, const char *model,
                                          const char *base64Jpeg,
                                          const char *systemText, const char *userText,
                                          int maxTokens, size_t maxChars, uint32_t timeoutMs,
                                          char *outText, size_t outLen) {
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
                                         model, base64Jpeg, systemText, userText,
                                         maxTokens, maxChars, timeoutMs, outText, outLen);
    if (lastResult == VISION_RESULT_OK) {
      return VISION_RESULT_OK;
    }
    if (lastResult != VISION_RESULT_HTTP_FAIL && lastResult != VISION_RESULT_NO_API) {
      return lastResult;
    }
  }
  return lastResult;
}

static VisionResult loadConfiguredVisionProvider(AiProvider *outProvider, char *apiKey,
                                                 size_t apiKeyLen, const char **outModel) {
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

  int modelIndex = settings_api_get_model_index();
  if (!ai_provider_model_supports_vision(provider, modelIndex)) {
    const char *selectedModel = ai_provider_model_id(provider, modelIndex);
    bool foundVisionModel = false;
    const int count = ai_provider_model_count(provider);
    for (int i = 0; i < count; i++) {
      if (ai_provider_model_supports_vision(provider, i)) {
        modelIndex = i;
        foundVisionModel = true;
        break;
      }
    }
    if (!foundVisionModel) {
      Serial.printf("[Vision] model unsupported for vision: %s\r\n", selectedModel);
      return VISION_RESULT_UNSUPPORTED;
    }
    Serial.printf("[Vision] model %s has no vision, using %s\r\n",
                  selectedModel, ai_provider_model_id(provider, modelIndex));
  }

  if (apiKey != nullptr && apiKeyLen > 0) {
    settings_api_get_api_key(apiKey, apiKeyLen);
  }
  if (outProvider != nullptr) {
    *outProvider = provider;
  }
  if (outModel != nullptr) {
    *outModel = ai_provider_model_id(provider, modelIndex);
  }
  return VISION_RESULT_OK;
}

static VisionResult requestConfiguredVision(AiProvider provider, const char *apiKey,
                                            const char *model, const char *base64Jpeg,
                                            const char *systemText, const char *userText,
                                            int maxTokens, size_t maxChars, uint32_t timeoutMs,
                                            char *outText, size_t outLen) {
  if (provider == AI_PROVIDER_GEMINI) {
    return requestGemini(apiKey, model, base64Jpeg, systemText, userText,
                         maxTokens, maxChars, timeoutMs, outText, outLen);
  }
  if (provider == AI_PROVIDER_MIMO) {
    return requestMimoCompatible(apiKey, model, base64Jpeg, systemText, userText,
                                 maxTokens, maxChars, timeoutMs, outText, outLen);
  }

  const char *url = ai_provider_chat_completions_url(provider);
  if (url == nullptr || url[0] == '\0') {
    return VISION_RESULT_UNSUPPORTED;
  }
  return requestOpenAiCompatible(provider, url, apiKey, model, base64Jpeg,
                                 systemText, userText, maxTokens, maxChars, timeoutMs,
                                 outText, outLen);
}

static void chooseBookFallbackAnswer(char *outText, size_t outLen) {
  if (outText == nullptr || outLen == 0) {
    return;
  }
  snprintf(outText, outLen, "%s", bookofanswers_random_timed());
}

static uint8_t clampByte(int value) {
  if (value < 0) {
    return 0;
  }
  if (value > 255) {
    return 255;
  }
  return (uint8_t)value;
}

static uint32_t mixObfuscationSeed(uint32_t seed, uint32_t value) {
  seed ^= value + 0x9E3779B9u + (seed << 6) + (seed >> 2);
  return seed;
}

static bool isJpegSofMarker(uint8_t marker) {
  return marker == 0xC0 || marker == 0xC1 || marker == 0xC2 || marker == 0xC3 ||
         marker == 0xC5 || marker == 0xC6 || marker == 0xC7 ||
         marker == 0xC9 || marker == 0xCA || marker == 0xCB ||
         marker == 0xCD || marker == 0xCE || marker == 0xCF;
}

static bool readJpegDimensions(const uint8_t *jpeg, size_t jpegLen,
                               uint16_t *outWidth, uint16_t *outHeight) {
  if (jpeg == nullptr || jpegLen < 4 || outWidth == nullptr || outHeight == nullptr) {
    return false;
  }
  if (jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
    return false;
  }

  size_t pos = 2;
  while (pos + 4 < jpegLen) {
    while (pos < jpegLen && jpeg[pos] != 0xFF) {
      pos++;
    }
    while (pos < jpegLen && jpeg[pos] == 0xFF) {
      pos++;
    }
    if (pos >= jpegLen) {
      break;
    }

    const uint8_t marker = jpeg[pos++];
    if (marker == 0xD9 || marker == 0xDA) {
      break;
    }
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      continue;
    }
    if (pos + 2 > jpegLen) {
      return false;
    }

    const uint16_t segLen = ((uint16_t)jpeg[pos] << 8) | jpeg[pos + 1];
    if (segLen < 2 || pos + segLen > jpegLen) {
      return false;
    }

    if (isJpegSofMarker(marker)) {
      if (segLen < 7) {
        return false;
      }
      *outHeight = ((uint16_t)jpeg[pos + 3] << 8) | jpeg[pos + 4];
      *outWidth = ((uint16_t)jpeg[pos + 5] << 8) | jpeg[pos + 6];
      return *outWidth > 0 && *outHeight > 0;
    }

    pos += segLen;
  }
  return false;
}

static bool obfuscateJpegForBook(const uint8_t *jpeg, size_t jpegLen,
                                 uint8_t **outJpeg, size_t *outLen) {
  if (jpeg == nullptr || outJpeg == nullptr || outLen == nullptr) {
    return false;
  }
  *outJpeg = nullptr;
  *outLen = 0;

  uint16_t jpegWidth = 0;
  uint16_t jpegHeight = 0;
  if (!readJpegDimensions(jpeg, jpegLen, &jpegWidth, &jpegHeight) ||
      jpegWidth > VISION_CAMERA_FRAME_WIDTH ||
      jpegHeight > VISION_CAMERA_FRAME_HEIGHT) {
    Serial.printf("[Vision] book invalid jpeg dimensions %ux%u\r\n",
                  (unsigned)jpegWidth, (unsigned)jpegHeight);
    return false;
  }

  const int frameW = (int)jpegWidth;
  const int frameH = (int)jpegHeight;
  const size_t rgbSize = (size_t)frameW * (size_t)frameH * 3U;
  uint8_t *rgb = static_cast<uint8_t *>(heap_caps_malloc(rgbSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (rgb == nullptr) {
    Serial.printf("[Vision] book obfuscation PSRAM alloc failed size=%u\r\n", (unsigned)rgbSize);
    return false;
  }

  if (!fmt2rgb888(jpeg, jpegLen, PIXFORMAT_JPEG, rgb)) {
    Serial.println("[Vision] book obfuscation jpeg decode failed");
    free(rgb);
    return false;
  }

  uint32_t seed = mixObfuscationSeed(0xA17B00A5u, (uint32_t)jpegLen);
  for (size_t i = 0; i < jpegLen; i += 257U) {
    seed = mixObfuscationSeed(seed, jpeg[i]);
  }

  for (int by = 0; by < frameH; by += VISION_BOOK_OBFUSCATE_BLOCK_PX) {
    for (int bx = 0; bx < frameW; bx += VISION_BOOK_OBFUSCATE_BLOCK_PX) {
      uint32_t rSum = 0;
      uint32_t gSum = 0;
      uint32_t bSum = 0;
      uint32_t count = 0;
      const int yEnd = min(by + VISION_BOOK_OBFUSCATE_BLOCK_PX, frameH);
      const int xEnd = min(bx + VISION_BOOK_OBFUSCATE_BLOCK_PX, frameW);

      for (int y = by; y < yEnd; y++) {
        for (int x = bx; x < xEnd; x++) {
          const size_t idx = ((size_t)y * frameW + (size_t)x) * 3U;
          rSum += rgb[idx];
          gSum += rgb[idx + 1];
          bSum += rgb[idx + 2];
          count++;
        }
      }

      if (count == 0) {
        continue;
      }

      seed = mixObfuscationSeed(seed, ((uint32_t)bx << 16) | (uint32_t)by);
      const int jitter = (int)(seed & 0x1FU) - 16;
      const int avgR = (int)(rSum / count);
      const int avgG = (int)(gSum / count);
      const int avgB = (int)(bSum / count);
      int luma = (77 * avgR + 150 * avgG + 29 * avgB) >> 8;
      luma = (luma & 0xE0) + jitter;
      const uint8_t shade = clampByte(luma);

      for (int y = by; y < yEnd; y++) {
        for (int x = bx; x < xEnd; x++) {
          const size_t idx = ((size_t)y * frameW + (size_t)x) * 3U;
          rgb[idx] = shade;
          rgb[idx + 1] = shade;
          rgb[idx + 2] = shade;
        }
      }
    }
  }

  uint8_t *encoded = nullptr;
  size_t encodedLen = 0;
  const bool ok = fmt2jpg(rgb, rgbSize,
                          frameW, frameH, PIXFORMAT_RGB888, 35, &encoded, &encodedLen);
  free(rgb);

  if (!ok || encoded == nullptr || encodedLen == 0 || encodedLen > VISION_MAX_JPEG_BYTES) {
    free(encoded);
    Serial.printf("[Vision] book obfuscation encode failed len=%u\r\n", (unsigned)encodedLen);
    return false;
  }

  *outJpeg = encoded;
  *outLen = encodedLen;
  Serial.printf("[Vision] book obfuscated JPEG %u -> %u bytes\r\n",
                (unsigned)jpegLen, (unsigned)encodedLen);
  return true;
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

  char apiKey[129];
  const char *model = nullptr;
  AiProvider provider = AI_PROVIDER_OPENAI;
  VisionResult ready = loadConfiguredVisionProvider(&provider, apiKey, sizeof(apiKey), &model);
  if (ready != VISION_RESULT_OK) {
    return ready;
  }

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
  result = requestConfiguredVision(provider, apiKey, model, base64,
                                   describe_system_prompt(), describe_user_prompt(),
                                   VISION_MAX_TOKENS, VISION_OUTPUT_MAX_CHARS,
                                   VISION_HTTP_TIMEOUT_MS,
                                   outText, outLen);
  free(base64);

  if (result == VISION_RESULT_OK) {
    Serial.printf("[Vision] %s\r\n", outText);
  }

  return result;
}

VisionResult vision_service_book_answer_jpeg(const uint8_t *jpeg, size_t jpegLen,
                                             char *outText, size_t outLen) {
  if (outText == nullptr || outLen == 0) {
    return VISION_RESULT_HTTP_FAIL;
  }
  outText[0] = '\0';

  if (jpeg == nullptr || jpegLen == 0 || jpegLen > VISION_MAX_JPEG_BYTES) {
    Serial.printf("[Vision] book invalid jpeg len=%u, using local answer\r\n", (unsigned)jpegLen);
    chooseBookFallbackAnswer(outText, outLen);
    return VISION_RESULT_LOCAL_FALLBACK;
  }

  char apiKey[129];
  const char *model = nullptr;
  AiProvider provider = AI_PROVIDER_OPENAI;
  VisionResult ready = loadConfiguredVisionProvider(&provider, apiKey, sizeof(apiKey), &model);
  if (ready != VISION_RESULT_OK) {
    Serial.printf("[Vision] book provider unavailable (%d)\r\n", (int)ready);
    return ready;
  }

  uint8_t *obfuscated = nullptr;
  size_t obfuscatedLen = 0;
  if (!obfuscateJpegForBook(jpeg, jpegLen, &obfuscated, &obfuscatedLen)) {
    Serial.println("[Vision] book obfuscation failed, using local answer");
    chooseBookFallbackAnswer(outText, outLen);
    return VISION_RESULT_LOCAL_FALLBACK;
  }

  char *base64 = nullptr;
  size_t base64Len = 0;
  if (!encodeBase64(obfuscated, obfuscatedLen, &base64, &base64Len)) {
    free(obfuscated);
    Serial.println("[Vision] book base64 encode failed, using local answer");
    chooseBookFallbackAnswer(outText, outLen);
    return VISION_RESULT_LOCAL_FALLBACK;
  }
  free(obfuscated);

  Serial.printf("[Vision] book b64 %u bytes, provider=%s model=%s, posting...\r\n",
                (unsigned)base64Len, ai_provider_name(provider), model);
  Serial.flush();

  const VisionResult result = requestConfiguredVision(provider, apiKey, model, base64,
                                                     book_system_prompt(), book_user_prompt(),
                                                     VISION_BOOK_MAX_TOKENS,
                                                     VISION_BOOK_OUTPUT_MAX_CHARS,
                                                     VISION_BOOK_HTTP_TIMEOUT_MS,
                                                     outText, outLen);
  free(base64);

  if (result != VISION_RESULT_OK || outText[0] == '\0') {
    Serial.printf("[Vision] book request failed (%d), using local answer\r\n", (int)result);
    chooseBookFallbackAnswer(outText, outLen);
    return VISION_RESULT_LOCAL_FALLBACK;
  }

  trimVisionOutput(outText);
  if (bookAnswerLooksLikeTitle(outText)) {
    Serial.printf("[Vision] book title-like answer '%s', using local answer\r\n", outText);
    chooseBookFallbackAnswer(outText, outLen);
    return VISION_RESULT_LOCAL_FALLBACK;
  }

  truncateVisionOutput(outText, VISION_BOOK_OUTPUT_MAX_CHARS);
  Serial.printf("[Vision] book answer %s\r\n", outText);
  return VISION_RESULT_OK;
}

VisionResult vision_service_describe_camera(char *outText, size_t outLen) {
  if (outText == nullptr || outLen == 0) {
    return VISION_RESULT_HTTP_FAIL;
  }
  outText[0] = '\0';

  Serial.println("[Vision] describe start");
  Serial.flush();

  char apiKey[129];
  const char *model = nullptr;
  AiProvider provider = AI_PROVIDER_OPENAI;
  VisionResult ready = loadConfiguredVisionProvider(&provider, apiKey, sizeof(apiKey), &model);
  if (ready != VISION_RESULT_OK) {
    return ready;
  }

  if (!camera_service_lock(1600)) {
    Serial.println("[Vision] camera busy while capture");
    return VISION_RESULT_CAPTURE_FAIL;
  }

  Serial.println("[Vision] capturing frame...");
  Serial.flush();

  VisionResult captureStatus = VISION_RESULT_CAPTURE_FAIL;
  camera_fb_t *fb = nullptr;
  uint8_t *jpegCopy = nullptr;
  size_t jpegLen = 0;

  if (!camera_service_is_ready() && !camera_service_init()) {
    Serial.println("[Vision] no camera");
    captureStatus = VISION_RESULT_NO_CAMERA;
  } else {
    camera_fb_t *warmup = camera_service_capture();
    if (warmup != nullptr) {
      camera_service_release(warmup);
    }

    fb = camera_service_capture();
    if (fb == nullptr || fb->len == 0 || fb->len > VISION_MAX_JPEG_BYTES) {
      Serial.printf("[Vision] capture fail fb=%p len=%u\r\n",
                    (void *)fb, fb != nullptr ? (unsigned)fb->len : 0U);
      captureStatus = VISION_RESULT_CAPTURE_FAIL;
    } else {
      jpegCopy = static_cast<uint8_t *>(heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (jpegCopy == nullptr) {
        jpegCopy = static_cast<uint8_t *>(malloc(fb->len));
      }
      if (jpegCopy == nullptr) {
        Serial.printf("[Vision] jpeg copy alloc failed len=%u\r\n", (unsigned)fb->len);
        captureStatus = VISION_RESULT_CAPTURE_FAIL;
      } else {
        memcpy(jpegCopy, fb->buf, fb->len);
        jpegLen = fb->len;
        captureStatus = VISION_RESULT_OK;
      }
    }
  }

  if (fb != nullptr) {
    camera_service_release(fb);
  }
  camera_service_pause();
  camera_service_unlock();

  if (captureStatus != VISION_RESULT_OK) {
    free(jpegCopy);
    return captureStatus;
  }

  Serial.printf("[Vision] JPEG %u bytes, encoding b64...\r\n", (unsigned)jpegLen);
  Serial.flush();

  char *base64 = nullptr;
  size_t base64Len = 0;
  if (!encodeBase64(jpegCopy, jpegLen, &base64, &base64Len)) {
    free(jpegCopy);
    Serial.println("[Vision] base64 encode failed");
    return VISION_RESULT_CAPTURE_FAIL;
  }
  free(jpegCopy);

  Serial.printf("[Vision] b64 %u bytes, provider=%s model=%s, posting...\r\n",
                (unsigned)base64Len, ai_provider_name(provider), model);
  Serial.flush();

  VisionResult result = VISION_RESULT_HTTP_FAIL;
  result = requestConfiguredVision(provider, apiKey, model, base64,
                                   describe_system_prompt(), describe_user_prompt(),
                                   VISION_MAX_TOKENS, VISION_OUTPUT_MAX_CHARS,
                                   VISION_HTTP_TIMEOUT_MS,
                                   outText, outLen);
  free(base64);

  if (result == VISION_RESULT_OK) {
    Serial.printf("[Vision] %s\r\n", outText);
  }

  return result;
}
