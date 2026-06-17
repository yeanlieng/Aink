#include "weather_service.h"

#include "app_locale.h"
#include "settings_api.h"
#include "weather_gzip.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#define WEATHER_FETCH_INTERVAL_MS  (30UL * 60UL * 1000)
#define WEATHER_RETRY_INTERVAL_MS  (5UL * 60UL * 1000)
#define WEATHER_HTTP_TIMEOUT_MS    60000
#define WEATHER_HTTP_CONNECT_MS    30000
#define WEATHER_HTTP_OPTIONAL_MS   20000
#define WEATHER_TASK_POLL_MS       250
#define WEATHER_TASK_STACK_BYTES   12288

static WeatherSnapshot s_snapshot = {};
static unsigned long s_lastFetchMs = 0;
static unsigned long s_lastAttemptMs = 0;
static bool s_freshFetchPending = false;
static TaskHandle_t s_weatherTask = nullptr;
static portMUX_TYPE s_weatherMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_updateRequested = false;
static volatile bool s_forceRequested = false;
static volatile bool s_updateBusy = false;

static int roundTemp(float tempC) {
  return (int)(tempC + (tempC >= 0.0f ? 0.5f : -0.5f));
}

static bool httpsGetBody(const char *url, const char *tag, const char *apiKey, String *outBody,
                         uint32_t timeoutMs);
static bool qweatherGet(const char *host, const char *pathQuery, const char *apiKey, const char *tag,
                        String *outBody, uint32_t timeoutMs);
static const char *qweatherLangCode(void);
static bool parseQWeatherApiCode(const String &body, char *out, size_t outLen);
static void logQWeatherFailure(const char *tag, const String &body);
static int wdayFromIsoDate(const char *isoDate);
static void dayLabelFromIsoDate(const char *isoDate, int index, char *out, size_t outLen);

WeatherIconKind weather_service_qweather_icon_to_icon(int iconCode) {
  if (iconCode == 100 || iconCode == 150) {
    return WEATHER_ICON_SUNNY;
  }
  if (iconCode == 101 || iconCode == 102 || iconCode == 103 || iconCode == 151 || iconCode == 152 ||
      iconCode == 153) {
    return WEATHER_ICON_CLOUDY;
  }
  if (iconCode == 104) {
    return WEATHER_ICON_CLOUDY;
  }
  if (iconCode >= 300 && iconCode <= 399) {
    return WEATHER_ICON_RAIN;
  }
  if (iconCode >= 400 && iconCode <= 499) {
    return WEATHER_ICON_SNOW;
  }
  if (iconCode >= 500 && iconCode <= 515) {
    return WEATHER_ICON_FOG;
  }
  if (iconCode == 302 || iconCode == 303 || iconCode == 304 || iconCode == 900 || iconCode == 901) {
    return WEATHER_ICON_LIGHTNING;
  }
  return WEATHER_ICON_CLOUDY;
}

WeatherIconKind weather_service_wmo_to_icon(int wmo) {
  switch (wmo) {
    case 0:
    case 1:
      return WEATHER_ICON_SUNNY;
    case 2:
    case 3:
      return WEATHER_ICON_CLOUDY;
    case 45:
    case 48:
      return WEATHER_ICON_FOG;
    case 51:
    case 53:
    case 55:
    case 56:
    case 57:
    case 61:
    case 63:
    case 65:
    case 66:
    case 67:
    case 80:
    case 81:
    case 82:
      return WEATHER_ICON_RAIN;
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86:
      return WEATHER_ICON_SNOW;
    case 95:
    case 96:
    case 99:
      return WEATHER_ICON_LIGHTNING;
    default:
      return WEATHER_ICON_CLOUDY;
  }
}

const char *weather_service_wmo_to_label(int wmo) {
  const bool zh = app_locale_get() == APP_LANG_ZH;
  switch (weather_service_wmo_to_icon(wmo)) {
    case WEATHER_ICON_SUNNY:
      return zh ? "晴" : "Clear";
    case WEATHER_ICON_CLOUDY:
      return zh ? "多云" : "Cloudy";
    case WEATHER_ICON_RAIN:
      return zh ? "雨" : "Rain";
    case WEATHER_ICON_SNOW:
      return zh ? "雪" : "Snow";
    case WEATHER_ICON_LIGHTNING:
      return zh ? "雷暴" : "Storm";
    case WEATHER_ICON_FOG:
      return zh ? "雾" : "Fog";
    default:
      return zh ? "多云" : "Cloudy";
  }
}

const char *weather_service_uv_level(int uvIndexTenths) {
  if (uvIndexTenths < 0) {
    return "--";
  }
  const bool zh = app_locale_get() == APP_LANG_ZH;
  const float uv = uvIndexTenths / 10.0f;
  if (uv < 3.0f) {
    return zh ? "弱" : "Low";
  }
  if (uv < 6.0f) {
    return zh ? "中等" : "Mod";
  }
  if (uv < 8.0f) {
    return zh ? "强" : "High";
  }
  if (uv < 11.0f) {
    return zh ? "很强" : "VHi";
  }
  return zh ? "极强" : "Extr";
}

const char *weather_service_aqi_level(int usAqi) {
  if (usAqi < 0) {
    return "--";
  }
  const bool zh = app_locale_get() == APP_LANG_ZH;
  if (usAqi <= 50) {
    return zh ? "优" : "Good";
  }
  if (usAqi <= 100) {
    return zh ? "良" : "Mod";
  }
  if (usAqi <= 150) {
    return zh ? "敏" : "Sens";
  }
  if (usAqi <= 200) {
    return zh ? "中度" : "Unh";
  }
  return zh ? "重度" : "Bad";
}

void weather_service_reset(void) {
  portENTER_CRITICAL(&s_weatherMux);
  memset(&s_snapshot, 0, sizeof(s_snapshot));
  s_snapshot.humidityPct = -1;
  s_snapshot.uvIndexTenths = -1;
  s_snapshot.windSpeedKmh = -1;
  s_snapshot.pressureHpa = -1;
  s_snapshot.usAqi = -1;
  s_snapshot.pm25Tenths = -1;
  s_lastFetchMs = 0;
  s_lastAttemptMs = 0;
  s_freshFetchPending = false;
  s_updateRequested = false;
  s_forceRequested = false;
  portEXIT_CRITICAL(&s_weatherMux);
}

bool weather_service_consume_fresh_fetch(void) {
  portENTER_CRITICAL(&s_weatherMux);
  if (!s_freshFetchPending) {
    portEXIT_CRITICAL(&s_weatherMux);
    return false;
  }
  s_freshFetchPending = false;
  portEXIT_CRITICAL(&s_weatherMux);
  return true;
}

void weather_service_get_snapshot(WeatherSnapshot *out) {
  if (out == nullptr) {
    return;
  }
  portENTER_CRITICAL(&s_weatherMux);
  *out = s_snapshot;
  portEXIT_CRITICAL(&s_weatherMux);
}

static bool parseJsonQuotedStringAfter(const String &body, int sectionIdx, const char *fieldKey,
                                       char *out, size_t outLen);

static bool parseJsonQuotedString(const String &body, const char *fieldKey, char *out,
                                  size_t outLen) {
  return parseJsonQuotedStringAfter(body, 0, fieldKey, out, outLen);
}

static bool parseJsonQuotedStringAfter(const String &body, int sectionIdx, const char *fieldKey,
                                       char *out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return false;
  }
  out[0] = '\0';

  char search[32];
  snprintf(search, sizeof(search), "\"%s\"", fieldKey);
  const int keyIdx = body.indexOf(search, sectionIdx);
  if (keyIdx < 0) {
    return false;
  }

  int pos = body.indexOf(':', keyIdx + (int)strlen(search));
  if (pos < 0) {
    return false;
  }
  pos++;
  while (pos < (int)body.length() && (body.charAt(pos) == ' ' || body.charAt(pos) == '\t')) {
    pos++;
  }
  if (pos >= (int)body.length() || body.charAt(pos) != '"') {
    return false;
  }
  pos++;

  const int end = body.indexOf('"', pos);
  if (end < 0) {
    return false;
  }

  const String value = body.substring(pos, end);
  snprintf(out, outLen, "%s", value.c_str());
  return true;
}

static bool parseJsonFloatAfterKey(const String &body, int sectionIdx, const char *fieldKey,
                                   float *outVal) {
  if (sectionIdx < 0 || outVal == nullptr) {
    return false;
  }

  char search[40];
  snprintf(search, sizeof(search), "\"%s\"", fieldKey);
  const int keyIdx = body.indexOf(search, sectionIdx);
  if (keyIdx < 0) {
    return false;
  }

  int pos = body.indexOf(':', keyIdx + (int)strlen(search));
  if (pos < 0) {
    return false;
  }
  pos++;
  while (pos < (int)body.length() && (body.charAt(pos) == ' ' || body.charAt(pos) == '\t')) {
    pos++;
  }
  if (pos < (int)body.length() && body.charAt(pos) == '"') {
    pos++;
  }

  *outVal = body.substring(pos).toFloat();
  return true;
}

static bool httpsGetBody(const char *url, const char *tag, const char *apiKey, String *outBody,
                         uint32_t timeoutMs) {
  if (url == nullptr || tag == nullptr || outBody == nullptr) {
    return false;
  }
  if (timeoutMs == 0) {
    timeoutMs = WEATHER_HTTP_TIMEOUT_MS;
  }

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

  Serial.printf("[Weather] %s GET heap=%u block=%u dns=%s\r\n", tag,
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                dnsOk ? serverIp.toString().c_str() : "fail");
  Serial.flush();

  WiFi.setSleep(WIFI_PS_NONE);
  yield();

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(15);
  client.setConnectionTimeout(timeoutMs);
  client.setTimeout(timeoutMs);

  HTTPClient http;
  http.setConnectTimeout(WEATHER_HTTP_CONNECT_MS > 65535 ? 65535 : (uint16_t)WEATHER_HTTP_CONNECT_MS);
  http.setTimeout(timeoutMs > 65535 ? 65535 : (uint16_t)timeoutMs);
  http.setReuse(false);
  if (!http.begin(client, url)) {
    Serial.printf("[Weather] %s begin failed\r\n", tag);
    return false;
  }

  http.addHeader("Accept", "application/json");
  http.setAcceptEncoding("gzip");
  if (apiKey != nullptr && apiKey[0] != '\0') {
    http.addHeader("X-QW-Api-Key", apiKey);
  }

  const int code = http.GET();
  *outBody = http.getString();
  http.end();
  client.stop();

  if (code != HTTP_CODE_OK) {
    Serial.printf("[Weather] %s HTTP %d (%s)\r\n", tag, code, http.errorToString(code).c_str());
    logQWeatherFailure(tag, *outBody);
    return false;
  }

  if (!weather_gzip_decompress(outBody)) {
    Serial.printf("[Weather] %s gzip decompress failed\r\n", tag);
    logQWeatherFailure(tag, *outBody);
    return false;
  }
  return true;
}

static bool qweatherGet(const char *host, const char *pathQuery, const char *apiKey, const char *tag,
                        String *outBody, uint32_t timeoutMs) {
  if (host == nullptr || host[0] == '\0' || pathQuery == nullptr || outBody == nullptr) {
    return false;
  }

  char url[384];
  snprintf(url, sizeof(url), "https://%s%s", host, pathQuery);
  return httpsGetBody(url, tag, apiKey, outBody, timeoutMs);
}

static const char *qweatherLangCode(void) {
  return app_locale_get() == APP_LANG_ZH ? "zh-hans" : "en";
}

static bool parseQWeatherApiCode(const String &body, char *out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return false;
  }
  out[0] = '\0';

  if (parseJsonQuotedString(body, "code", out, outLen)) {
    return true;
  }

  const int codeIdx = body.indexOf("\"code\"");
  if (codeIdx < 0) {
    return false;
  }

  int pos = body.indexOf(':', codeIdx + 6);
  if (pos < 0) {
    return false;
  }
  pos++;
  while (pos < (int)body.length() && (body.charAt(pos) == ' ' || body.charAt(pos) == '\t')) {
    pos++;
  }

  size_t w = 0;
  while (pos < (int)body.length() && w + 1 < outLen) {
    const char c = body.charAt(pos);
    if (c < '0' || c > '9') {
      break;
    }
    out[w++] = c;
    pos++;
  }
  out[w] = '\0';
  return w > 0;
}

static void logQWeatherFailure(const char *tag, const String &body) {
  char apiCode[16] = {};
  if (parseQWeatherApiCode(body, apiCode, sizeof(apiCode))) {
    Serial.printf("[Weather] %s QWeather code=%s\r\n", tag, apiCode);
    return;
  }

  const int errIdx = body.indexOf("\"error\"");
  if (errIdx >= 0) {
    float status = 0.0f;
    parseJsonFloatAfterKey(body, errIdx, "status", &status);
    char detail[80] = {};
    parseJsonQuotedStringAfter(body, errIdx, "detail", detail, sizeof(detail));
    Serial.printf("[Weather] %s QWeather v2 status=%.0f detail=%s\r\n", tag, status,
                  detail[0] != '\0' ? detail : "?");
  }

  Serial.print("[Weather] body: ");
  const unsigned n = body.length() < 180 ? (unsigned)body.length() : 180;
  for (unsigned i = 0; i < n; i++) {
    const char c = body.charAt(i);
    Serial.write((c >= 32 && c < 127) ? c : '.');
  }
  Serial.println();
}

static bool qweatherResponseOk(const String &body) {
  if (body.indexOf("\"error\"") >= 0) {
    return false;
  }

  char code[16] = {};
  if (parseQWeatherApiCode(body, code, sizeof(code))) {
    return strcmp(code, "200") == 0;
  }
  return body.indexOf("\"code\":\"200\"") >= 0 || body.indexOf("\"code\": \"200\"") >= 0 ||
         body.indexOf("\"code\":200") >= 0;
}

static bool fetchGeoLocation(float *outLat, float *outLon, char *outLocation, size_t locationLen) {
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin("http://ip-api.com/json/?fields=status,lat,lon,city,countryCode")) {
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[Weather] geo HTTP %d\n", code);
    http.end();
    return false;
  }

  const String body = http.getString();
  http.end();

  if (body.indexOf("\"status\":\"success\"") < 0) {
    Serial.println("[Weather] geo status failed");
    return false;
  }

  const int latIdx = body.indexOf("\"lat\":");
  const int lonIdx = body.indexOf("\"lon\":");
  if (latIdx < 0 || lonIdx < 0) {
    return false;
  }

  *outLat = body.substring(latIdx + 6).toFloat();
  *outLon = body.substring(lonIdx + 6).toFloat();

  char city[20] = {};
  char countryCode[4] = {};
  parseJsonQuotedString(body, "city", city, sizeof(city));
  parseJsonQuotedString(body, "countryCode", countryCode, sizeof(countryCode));

  if (outLocation != nullptr && locationLen > 0) {
    if (city[0] != '\0' && countryCode[0] != '\0') {
      snprintf(outLocation, locationLen, "%s, %s", city, countryCode);
    } else if (city[0] != '\0') {
      snprintf(outLocation, locationLen, "%s", city);
    } else {
      outLocation[0] = '\0';
    }
  }

  Serial.printf("[Weather] geo lat=%.4f lon=%.4f loc=%s\n",
                *outLat, *outLon,
                (outLocation != nullptr && outLocation[0] != '\0') ? outLocation : "?");
  return true;
}

static bool parseJsonIntAfterKey(const String &body, int sectionIdx, const char *fieldKey, int *outVal) {
  float f = 0.0f;
  if (!parseJsonFloatAfterKey(body, sectionIdx, fieldKey, &f)) {
    return false;
  }
  *outVal = (int)(f + 0.5f);
  return true;
}

static int wdayFromIsoDate(const char *isoDate) {
  int year = 0;
  int month = 0;
  int day = 0;
  if (sscanf(isoDate, "%d-%d-%d", &year, &month, &day) != 3) {
    return 0;
  }

  struct tm timeInfo = {};
  timeInfo.tm_year = year - 1900;
  timeInfo.tm_mon = month - 1;
  timeInfo.tm_mday = day;
  timeInfo.tm_isdst = -1;
  mktime(&timeInfo);
  return timeInfo.tm_wday;
}

static void dayLabelFromIsoDate(const char *isoDate, int index, char *out, size_t outLen) {
  (void)index;
  snprintf(out, outLen, "%s", app_tr_weekday(wdayFromIsoDate(isoDate)));
}

static bool parseQWeatherNow(const String &body, WeatherSnapshot *out) {
  const int sectionIdx = body.indexOf("\"now\":");
  if (sectionIdx < 0 || !qweatherResponseOk(body)) {
    return false;
  }

  float tempC = 0.0f;
  float feelsC = 0.0f;
  float humidity = 0.0f;
  float wind = 0.0f;
  float pressure = 0.0f;
  int iconCode = 0;

  if (!parseJsonFloatAfterKey(body, sectionIdx, "temp", &tempC)) {
    return false;
  }

  parseJsonFloatAfterKey(body, sectionIdx, "feelsLike", &feelsC);
  parseJsonFloatAfterKey(body, sectionIdx, "humidity", &humidity);
  parseJsonFloatAfterKey(body, sectionIdx, "windSpeed", &wind);
  parseJsonFloatAfterKey(body, sectionIdx, "pressure", &pressure);
  parseJsonIntAfterKey(body, sectionIdx, "icon", &iconCode);

  char text[16] = {};
  parseJsonQuotedStringAfter(body, sectionIdx, "text", text, sizeof(text));

  out->tempC = roundTemp(tempC);
  out->feelsLikeC = feelsC > 0.0f ? roundTemp(feelsC) : out->tempC;
  out->wmoCode = iconCode;
  out->icon = weather_service_qweather_icon_to_icon(iconCode);
  if (text[0] != '\0') {
    snprintf(out->condition, sizeof(out->condition), "%s", text);
  } else {
    snprintf(out->condition, sizeof(out->condition), "%s", weather_service_wmo_to_label(iconCode));
  }

  if (humidity >= 0.0f && humidity <= 100.0f) {
    out->humidityPct = (int)(humidity + 0.5f);
  }
  if (wind >= 0.0f) {
    out->windSpeedKmh = (int)(wind + 0.5f);
  }
  if (pressure > 800.0f) {
    out->pressureHpa = (int)(pressure + 0.5f);
  }
  return true;
}

static bool parseQWeatherDaily(const String &body, WeatherSnapshot *out) {
  const int dailyIdx = body.indexOf("\"daily\":");
  if (dailyIdx < 0 || !qweatherResponseOk(body)) {
    return false;
  }

  static const int kForecastDayOffset = 1;

  int searchFrom = dailyIdx;
  int dayIndex = 0;
  while (dayIndex < kForecastDayOffset + WEATHER_DAILY_COUNT) {
    const int fxIdx = body.indexOf("\"fxDate\"", searchFrom);
    if (fxIdx < 0) {
      return false;
    }
    int itemIdx = fxIdx;
    while (itemIdx > dailyIdx && body.charAt(itemIdx) != '{') {
      itemIdx--;
    }
    if (body.charAt(itemIdx) != '{') {
      return false;
    }
    searchFrom = fxIdx + 8;

    char fxDate[16] = {};
    char textDay[16] = {};
    float hi = 0.0f;
    float lo = 0.0f;
    int iconCode = 0;

    parseJsonQuotedStringAfter(body, itemIdx, "fxDate", fxDate, sizeof(fxDate));
    parseJsonQuotedStringAfter(body, itemIdx, "textDay", textDay, sizeof(textDay));
    parseJsonFloatAfterKey(body, itemIdx, "tempMax", &hi);
    parseJsonFloatAfterKey(body, itemIdx, "tempMin", &lo);
    parseJsonIntAfterKey(body, itemIdx, "iconDay", &iconCode);

    if (dayIndex == 0) {
      char rise[8] = {};
      char set[8] = {};
      parseJsonQuotedStringAfter(body, itemIdx, "sunrise", rise, sizeof(rise));
      parseJsonQuotedStringAfter(body, itemIdx, "sunset", set, sizeof(set));
      if (rise[0] != '\0') {
        snprintf(out->sunrise, sizeof(out->sunrise), "%s", rise);
      }
      if (set[0] != '\0') {
        snprintf(out->sunset, sizeof(out->sunset), "%s", set);
      }
      out->hiToday = roundTemp(hi);
      out->loToday = roundTemp(lo);

      float uvIndex = 0.0f;
      if (parseJsonFloatAfterKey(body, itemIdx, "uvIndex", &uvIndex) && uvIndex > 0.0f) {
        out->uvIndexTenths = (int)(uvIndex * 10.0f + 0.5f);
      }
    }

    if (dayIndex >= kForecastDayOffset) {
      const int outIdx = dayIndex - kForecastDayOffset;
      out->daily[outIdx].hi = roundTemp(hi);
      out->daily[outIdx].lo = roundTemp(lo);
      out->daily[outIdx].wmoCode = iconCode;
      out->daily[outIdx].icon = weather_service_qweather_icon_to_icon(iconCode);
      if (textDay[0] != '\0') {
        snprintf(out->daily[outIdx].condition, sizeof(out->daily[outIdx].condition), "%s", textDay);
      } else {
        snprintf(out->daily[outIdx].condition, sizeof(out->daily[outIdx].condition), "%s",
                 weather_service_wmo_to_label(iconCode));
      }
      out->daily[outIdx].wday = wdayFromIsoDate(fxDate);
      dayLabelFromIsoDate(fxDate, outIdx, out->daily[outIdx].label, sizeof(out->daily[outIdx].label));
    }

    dayIndex++;
  }

  return true;
}

static bool airQualityResponseOk(const String &body) {
  if (body.length() < 10 || body.indexOf("\"error\"") >= 0) {
    return false;
  }
  return body.indexOf("\"indexes\"") >= 0;
}

static bool parseAirIndexByCode(const String &body, const char *code, int *outAqi) {
  if (outAqi == nullptr || code == nullptr) {
    return false;
  }

  char spaced[32];
  char compact[32];
  snprintf(spaced, sizeof(spaced), "\"code\": \"%s\"", code);
  snprintf(compact, sizeof(compact), "\"code\":\"%s\"", code);

  int idx = body.indexOf(spaced);
  if (idx < 0) {
    idx = body.indexOf(compact);
  }
  if (idx < 0) {
    return false;
  }

  float aqiF = 0.0f;
  if (parseJsonFloatAfterKey(body, idx, "aqi", &aqiF)) {
    *outAqi = (int)(aqiF + 0.5f);
    return *outAqi >= 0;
  }

  char disp[16] = {};
  if (parseJsonQuotedStringAfter(body, idx, "aqiDisplay", disp, sizeof(disp)) && disp[0] != '\0') {
    *outAqi = atoi(disp);
    return *outAqi >= 0;
  }

  return false;
}

static bool parseAirPm25(const String &body, float *outPm) {
  if (outPm == nullptr) {
    return false;
  }

  int searchFrom = 0;
  while (true) {
    const int idx = body.indexOf("\"pm2p5\"", searchFrom);
    if (idx < 0) {
      return false;
    }

    const int codeKeyIdx = body.lastIndexOf("\"code\"", idx);
    if (codeKeyIdx >= 0 && (idx - codeKeyIdx) <= 24) {
      const int concIdx = body.indexOf("\"concentration\"", idx);
      if (concIdx >= 0 && concIdx < idx + 120) {
        return parseJsonFloatAfterKey(body, concIdx, "value", outPm);
      }
    }

    searchFrom = idx + 7;
  }
}

static bool fetchQWeatherAir(const char *host, const char *apiKey, float lat, float lon,
                             WeatherSnapshot *out) {
  char path[160];
  snprintf(path, sizeof(path), "/airquality/v1/current/%.2f/%.2f?lang=%s", lat, lon,
           qweatherLangCode());

  String body;
  if (!qweatherGet(host, path, apiKey, "air", &body, WEATHER_HTTP_OPTIONAL_MS)) {
    Serial.println("[Weather] air request failed");
    return false;
  }

  if (!airQualityResponseOk(body)) {
    logQWeatherFailure("air", body);
    return false;
  }

  static const char *kIndexCodes[] = {"cn-mee", "cn-mee-1h", "us-epa", "qaqi"};
  int aqi = -1;
  for (size_t i = 0; i < sizeof(kIndexCodes) / sizeof(kIndexCodes[0]); i++) {
    if (parseAirIndexByCode(body, kIndexCodes[i], &aqi)) {
      break;
    }
  }

  if (aqi < 0) {
    const int indexesIdx = body.indexOf("\"indexes\"");
    const int itemIdx = indexesIdx >= 0 ? body.indexOf("{", indexesIdx) : -1;
    if (itemIdx >= 0) {
      float aqiF = 0.0f;
      if (parseJsonFloatAfterKey(body, itemIdx, "aqi", &aqiF)) {
        aqi = (int)(aqiF + 0.5f);
      }
    }
  }

  if (aqi < 0) {
    Serial.println("[Weather] air parse failed (no index)");
    logQWeatherFailure("air", body);
    return false;
  }

  float pmF = -1.0f;
  parseAirPm25(body, &pmF);

  out->usAqi = aqi;
  out->pm25Tenths = pmF >= 0.0f ? (int)(pmF * 10.0f + 0.5f) : -1;
  out->aqiValid = true;
  Serial.printf("[Weather] aqi=%d pm2.5=%.1f\n", out->usAqi, pmF);
  return true;
}

static bool fetchQWeatherUv(const char *host, const char *apiKey, const char *locationId,
                            WeatherSnapshot *out) {
  if (out == nullptr || out->uvIndexTenths >= 0) {
    return out != nullptr && out->uvIndexTenths >= 0;
  }

  char path[128];
  snprintf(path, sizeof(path), "/v7/indices/1d?type=5&location=%s", locationId);

  String body;
  if (!qweatherGet(host, path, apiKey, "uv", &body, WEATHER_HTTP_OPTIONAL_MS) ||
      !qweatherResponseOk(body)) {
    Serial.println("[Weather] uv indices fallback unavailable");
    return false;
  }

  const int dailyIdx = body.indexOf("\"daily\":");
  if (dailyIdx < 0) {
    return false;
  }

  const int itemIdx = body.indexOf("{", dailyIdx);
  if (itemIdx < 0) {
    return false;
  }

  float level = 0.0f;
  if (!parseJsonFloatAfterKey(body, itemIdx, "level", &level) || level <= 0.0f) {
    return false;
  }

  // Indices "level" is 1-5 category, not UV index; only used when daily uvIndex missing.
  out->uvIndexTenths = (int)(level * 10.0f + 0.5f);
  Serial.printf("[Weather] uv fallback level=%.0f\n", level);
  return true;
}

static bool resolveQWeatherLocation(const char *host, const char *apiKey, float lon, float lat,
                                  char *locationId, size_t locationIdLen, char *cityName,
                                  size_t cityNameLen) {
  if (locationId == nullptr || locationIdLen == 0) {
    return false;
  }
  locationId[0] = '\0';
  if (cityName != nullptr && cityNameLen > 0) {
    cityName[0] = '\0';
  }

  char path[160];
  snprintf(path, sizeof(path), "/geo/v2/city/lookup?location=%.2f,%.2f&lang=%s", lon, lat,
           qweatherLangCode());

  String body;
  if (!qweatherGet(host, path, apiKey, "geo", &body, 0) || !qweatherResponseOk(body)) {
    logQWeatherFailure("geo", body);
    return false;
  }

  const int locIdx = body.indexOf("\"location\"");
  if (locIdx < 0) {
    return false;
  }

  const int itemIdx = body.indexOf("{", locIdx);
  if (itemIdx < 0) {
    return false;
  }

  char id[16] = {};
  char name[20] = {};
  char adm2[20] = {};
  parseJsonQuotedStringAfter(body, itemIdx, "id", id, sizeof(id));
  parseJsonQuotedStringAfter(body, itemIdx, "name", name, sizeof(name));
  parseJsonQuotedStringAfter(body, itemIdx, "adm2", adm2, sizeof(adm2));

  if (id[0] == '\0') {
    return false;
  }

  snprintf(locationId, locationIdLen, "%s", id);
  if (cityName != nullptr && cityNameLen > 0) {
    // QWeather "name" is often district-level (区); adm2 is prefecture/city (市).
    if (adm2[0] != '\0') {
      snprintf(cityName, cityNameLen, "%s", adm2);
    } else if (name[0] != '\0') {
      snprintf(cityName, cityNameLen, "%s", name);
    }
  }
  Serial.printf("[Weather] geo id=%s name=%s\n", locationId,
                (cityName != nullptr && cityName[0] != '\0') ? cityName : "?");
  return true;
}

static bool fetchWeather(WeatherSnapshot *out) {
  if (!settings_api_has_weather_api()) {
    Serial.println("[Weather] QWeather key/host not configured");
    return false;
  }

  char apiKey[128] = {};
  char apiHost[64] = {};
  settings_api_get_weather_api_key(apiKey, sizeof(apiKey));
  settings_api_get_weather_api_host(apiHost, sizeof(apiHost));

  float lat = 0.0f;
  float lon = 0.0f;
  char location[sizeof(out->location)] = {};
  if (!fetchGeoLocation(&lat, &lon, location, sizeof(location))) {
    return false;
  }

  char locationId[16] = {};
  char qLoc[sizeof(out->location)] = {};
  if (!resolveQWeatherLocation(apiHost, apiKey, lon, lat, locationId, sizeof(locationId), qLoc,
                               sizeof(qLoc))) {
    snprintf(locationId, sizeof(locationId), "%.2f,%.2f", lon, lat);
    Serial.printf("[Weather] geo lookup failed, fallback location=%s\n", locationId);
  }

  char path[128];
  snprintf(path, sizeof(path), "/v7/weather/now?location=%s", locationId);
  {
    String nowBody;
    if (!qweatherGet(apiHost, path, apiKey, "now", &nowBody, 0)) {
      return false;
    }

    if (!qweatherResponseOk(nowBody)) {
      logQWeatherFailure("now", nowBody);
      return false;
    }

    WeatherSnapshot snapshot = {};
    snapshot.humidityPct = -1;
    snapshot.uvIndexTenths = -1;
    snapshot.windSpeedKmh = -1;
    snapshot.pressureHpa = -1;
    snapshot.usAqi = -1;
    snapshot.pm25Tenths = -1;
    if (!parseQWeatherNow(nowBody, &snapshot)) {
      char apiCode[16] = {};
      parseQWeatherApiCode(nowBody, apiCode, sizeof(apiCode));
      Serial.printf("[Weather] parse now failed api=%s hasNow=%d len=%u\n",
                    apiCode[0] != '\0' ? apiCode : "?",
                    nowBody.indexOf("\"now\"") >= 0 ? 1 : 0,
                    (unsigned)nowBody.length());
      logQWeatherFailure("now", nowBody);
      return false;
    }
    snapshot.valid = true;

    delay(100);
    yield();
    fetchQWeatherAir(apiHost, apiKey, lat, lon, &snapshot);
    yield();

    snprintf(path, sizeof(path), "/v7/weather/7d?location=%s", locationId);
    String dailyBody;
    if (!qweatherGet(apiHost, path, apiKey, "7d", &dailyBody, 0) ||
        !parseQWeatherDaily(dailyBody, &snapshot)) {
      Serial.println("[Weather] parse daily failed");
      snapshot.hiToday = snapshot.tempC;
      snapshot.loToday = snapshot.tempC;
      snprintf(snapshot.daily[0].label, sizeof(snapshot.daily[0].label), "---");
      snapshot.daily[0].hi = snapshot.tempC;
      snapshot.daily[0].lo = snapshot.tempC;
      snapshot.daily[0].wmoCode = snapshot.wmoCode;
      snapshot.daily[0].icon = snapshot.icon;
      snprintf(snapshot.daily[0].condition, sizeof(snapshot.daily[0].condition), "%s",
               snapshot.condition);
      snprintf(snapshot.sunrise, sizeof(snapshot.sunrise), "--:--");
      snprintf(snapshot.sunset, sizeof(snapshot.sunset), "--:--");
    }

    fetchQWeatherUv(apiHost, apiKey, locationId, &snapshot);
    yield();

    if (qLoc[0] != '\0') {
      snprintf(snapshot.location, sizeof(snapshot.location), "%s", qLoc);
    } else {
      snprintf(snapshot.location, sizeof(snapshot.location), "%s", location);
    }

    *out = snapshot;
    Serial.printf("[Weather] %dC feel=%d loc=%s hum=%d uv=%.1f wind=%d aqi=%d\n",
                  snapshot.tempC, snapshot.feelsLikeC, snapshot.location,
                  snapshot.humidityPct,
                  snapshot.uvIndexTenths / 10.0f, snapshot.windSpeedKmh,
                  snapshot.aqiValid ? snapshot.usAqi : -1);
    return true;
  }
}

static void weather_service_task(void *param) {
  (void)param;
  for (;;) {
    bool shouldUpdate = false;
    bool force = false;

    portENTER_CRITICAL(&s_weatherMux);
    if (s_updateRequested && !s_updateBusy) {
      shouldUpdate = true;
      force = s_forceRequested;
      s_updateRequested = false;
      s_forceRequested = false;
      s_updateBusy = true;
    }
    portEXIT_CRITICAL(&s_weatherMux);

    if (shouldUpdate) {
      weather_service_update(force);
      portENTER_CRITICAL(&s_weatherMux);
      s_updateBusy = false;
      portEXIT_CRITICAL(&s_weatherMux);
    }

    vTaskDelay(pdMS_TO_TICKS(WEATHER_TASK_POLL_MS));
  }
}

static void weather_service_ensure_task(void) {
  if (s_weatherTask != nullptr) {
    return;
  }

  if (xTaskCreate(weather_service_task, "weather", WEATHER_TASK_STACK_BYTES,
                  nullptr, 1, &s_weatherTask) != pdPASS) {
    Serial.println("[Weather] task create failed");
    s_weatherTask = nullptr;
  }
}

void weather_service_request_update(bool force) {
  weather_service_ensure_task();
  portENTER_CRITICAL(&s_weatherMux);
  s_updateRequested = true;
  if (force) {
    s_forceRequested = true;
  }
  portEXIT_CRITICAL(&s_weatherMux);
}

bool weather_service_is_busy(void) {
  portENTER_CRITICAL(&s_weatherMux);
  const bool busy = s_updateBusy || s_updateRequested;
  portEXIT_CRITICAL(&s_weatherMux);
  return busy;
}

void weather_service_update(bool force) {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  const unsigned long now = millis();
  bool snapshotValid = false;
  unsigned long lastFetchMs = 0;
  unsigned long lastAttemptMs = 0;

  portENTER_CRITICAL(&s_weatherMux);
  snapshotValid = s_snapshot.valid;
  lastFetchMs = s_lastFetchMs;
  lastAttemptMs = s_lastAttemptMs;
  const bool skip =
      (!force && snapshotValid && (now - lastFetchMs) < WEATHER_FETCH_INTERVAL_MS) ||
      (!force && !snapshotValid && lastAttemptMs != 0 &&
       (now - lastAttemptMs) < WEATHER_RETRY_INTERVAL_MS);
  if (!skip) {
    s_lastAttemptMs = now;
  }
  portEXIT_CRITICAL(&s_weatherMux);

  if (skip) {
    return;
  }

  WeatherSnapshot fresh = {};
  if (fetchWeather(&fresh)) {
    portENTER_CRITICAL(&s_weatherMux);
    s_snapshot = fresh;
    s_lastFetchMs = now;
    s_freshFetchPending = true;
    portEXIT_CRITICAL(&s_weatherMux);
    Serial.println("[Weather] fetch complete");
  }
}
