#include "weather_service.h"

#include "app_locale.h"
#include "settings_api.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <time.h>

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#define WEATHER_FETCH_INTERVAL_MS  (30UL * 60UL * 1000)
#define WEATHER_RETRY_INTERVAL_MS  (5UL * 60UL * 1000)
#define WEATHER_HTTP_TIMEOUT_MS    60000
#define WEATHER_HTTP_CONNECT_MS    30000

static WeatherSnapshot s_snapshot = {};
static unsigned long s_lastFetchMs = 0;
static unsigned long s_lastAttemptMs = 0;

static int roundTemp(float tempC) {
  return (int)(tempC + (tempC >= 0.0f ? 0.5f : -0.5f));
}

static bool httpsGetBody(const char *url, const char *tag, String *outBody);
static bool qweatherGet(const char *host, const char *pathQuery, const char *tag, String *outBody);
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
    return zh ? "低" : "Low";
  }
  if (uv < 6.0f) {
    return zh ? "中" : "Mod";
  }
  if (uv < 8.0f) {
    return zh ? "高" : "High";
  }
  return zh ? "极" : "Extreme";
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
  memset(&s_snapshot, 0, sizeof(s_snapshot));
  s_snapshot.humidityPct = -1;
  s_snapshot.uvIndexTenths = -1;
  s_snapshot.windSpeedKmh = -1;
  s_snapshot.pressureHpa = -1;
  s_snapshot.usAqi = -1;
  s_snapshot.pm25Tenths = -1;
  s_lastFetchMs = 0;
  s_lastAttemptMs = 0;
}

void weather_service_get_snapshot(WeatherSnapshot *out) {
  if (out == nullptr) {
    return;
  }
  *out = s_snapshot;
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

static bool httpsGetBody(const char *url, const char *tag, String *outBody) {
  if (url == nullptr || tag == nullptr || outBody == nullptr) {
    return false;
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
  client.setTimeout(WEATHER_HTTP_TIMEOUT_MS);

  HTTPClient http;
  http.setConnectTimeout(WEATHER_HTTP_CONNECT_MS);
  http.setTimeout(WEATHER_HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    Serial.printf("[Weather] %s begin failed\r\n", tag);
    return false;
  }

  http.addHeader("Accept-Encoding", "identity");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[Weather] %s HTTP %d (%s)\r\n", tag, code, http.errorToString(code).c_str());
    http.end();
    return false;
  }

  *outBody = http.getString();
  http.end();
  return true;
}

static bool qweatherGet(const char *host, const char *pathQuery, const char *tag, String *outBody) {
  if (host == nullptr || host[0] == '\0' || pathQuery == nullptr || outBody == nullptr) {
    return false;
  }

  char url[384];
  snprintf(url, sizeof(url), "https://%s%s", host, pathQuery);
  return httpsGetBody(url, tag, outBody);
}

static bool qweatherResponseOk(const String &body) {
  char code[16] = {};
  if (parseJsonQuotedString(body, "code", code, sizeof(code))) {
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

  int searchFrom = dailyIdx;
  for (int i = 0; i < WEATHER_DAILY_COUNT; i++) {
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

    if (i == 0) {
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
    }

    out->daily[i].hi = roundTemp(hi);
    out->daily[i].lo = roundTemp(lo);
    out->daily[i].wmoCode = iconCode;
    out->daily[i].icon = weather_service_qweather_icon_to_icon(iconCode);
    if (textDay[0] != '\0') {
      snprintf(out->daily[i].condition, sizeof(out->daily[i].condition), "%s", textDay);
    } else {
      snprintf(out->daily[i].condition, sizeof(out->daily[i].condition), "%s",
               weather_service_wmo_to_label(iconCode));
    }
    out->daily[i].wday = wdayFromIsoDate(fxDate);
    dayLabelFromIsoDate(fxDate, i, out->daily[i].label, sizeof(out->daily[i].label));
  }

  out->hiToday = out->daily[0].hi;
  out->loToday = out->daily[0].lo;
  return true;
}

static bool fetchQWeatherAir(const char *host, const char *apiKey, float lon, float lat,
                             WeatherSnapshot *out) {
  char path[192];
  snprintf(path, sizeof(path),
           "/v7/air/now?location=%.4f,%.4f&key=%s",
           lon, lat, apiKey);

  String body;
  if (!qweatherGet(host, path, "air", &body) || !qweatherResponseOk(body)) {
    return false;
  }

  const int sectionIdx = body.indexOf("\"now\":");
  if (sectionIdx < 0) {
    return false;
  }

  float aqiF = -1.0f;
  float pmF = -1.0f;
  if (!parseJsonFloatAfterKey(body, sectionIdx, "aqi", &aqiF)) {
    return false;
  }
  parseJsonFloatAfterKey(body, sectionIdx, "pm2p5", &pmF);

  out->usAqi = (int)(aqiF + 0.5f);
  out->pm25Tenths = pmF >= 0.0f ? (int)(pmF * 10.0f + 0.5f) : -1;
  out->aqiValid = out->usAqi >= 0;
  Serial.printf("[Weather] aqi=%d pm2.5=%.1f\n", out->usAqi, pmF);
  return out->aqiValid;
}

static bool fetchQWeatherUv(const char *host, const char *apiKey, float lon, float lat,
                              WeatherSnapshot *out) {
  char path[192];
  snprintf(path, sizeof(path),
           "/v7/indices/1d?type=5&location=%.4f,%.4f&key=%s",
           lon, lat, apiKey);

  String body;
  if (!qweatherGet(host, path, "uv", &body) || !qweatherResponseOk(body)) {
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

  out->uvIndexTenths = (int)(level * 10.0f + 0.5f);
  return true;
}

static bool fetchQWeatherCityName(const char *host, const char *apiKey, float lon, float lat,
                                  char *outLocation, size_t locationLen) {
  const char *lang = app_locale_get() == APP_LANG_ZH ? "zh" : "en";
  char path[192];
  snprintf(path, sizeof(path),
           "/geo/v2/city/lookup?location=%.4f,%.4f&key=%s&lang=%s",
           lon, lat, apiKey, lang);

  String body;
  if (!qweatherGet(host, path, "geo", &body) || !qweatherResponseOk(body)) {
    return false;
  }

  const int locIdx = body.indexOf("\"location\":");
  if (locIdx < 0) {
    return false;
  }

  const int itemIdx = body.indexOf("{", locIdx);
  if (itemIdx < 0) {
    return false;
  }

  char name[20] = {};
  char adm2[20] = {};
  parseJsonQuotedStringAfter(body, itemIdx, "name", name, sizeof(name));
  parseJsonQuotedStringAfter(body, itemIdx, "adm2", adm2, sizeof(adm2));

  if (name[0] != '\0') {
    snprintf(outLocation, locationLen, "%s", name);
    return true;
  }
  if (adm2[0] != '\0') {
    snprintf(outLocation, locationLen, "%s", adm2);
    return true;
  }
  return false;
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

  const char *lang = app_locale_get() == APP_LANG_ZH ? "zh" : "en";
  char path[256];

  snprintf(path, sizeof(path),
           "/v7/weather/now?location=%.4f,%.4f&key=%s&lang=%s",
           lon, lat, apiKey, lang);
  String nowBody;
  if (!qweatherGet(apiHost, path, "now", &nowBody)) {
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
    parseJsonQuotedString(nowBody, "code", apiCode, sizeof(apiCode));
    Serial.printf("[Weather] parse now failed api=%s hasNow=%d len=%u\n",
                  apiCode[0] != '\0' ? apiCode : "?",
                  nowBody.indexOf("\"now\"") >= 0 ? 1 : 0,
                  (unsigned)nowBody.length());
    return false;
  }
  snapshot.valid = true;

  snprintf(path, sizeof(path),
           "/v7/weather/3d?location=%.4f,%.4f&key=%s&lang=%s",
           lon, lat, apiKey, lang);
  String dailyBody;
  if (!qweatherGet(apiHost, path, "3d", &dailyBody) || !parseQWeatherDaily(dailyBody, &snapshot)) {
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

  fetchQWeatherAir(apiHost, apiKey, lon, lat, &snapshot);
  fetchQWeatherUv(apiHost, apiKey, lon, lat, &snapshot);

  char qLoc[sizeof(snapshot.location)] = {};
  if (fetchQWeatherCityName(apiHost, apiKey, lon, lat, qLoc, sizeof(qLoc))) {
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

void weather_service_update(bool force) {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  const unsigned long now = millis();
  if (!force && s_snapshot.valid &&
      (now - s_lastFetchMs) < WEATHER_FETCH_INTERVAL_MS) {
    return;
  }
  if (!force && !s_snapshot.valid && s_lastAttemptMs != 0 &&
      (now - s_lastAttemptMs) < WEATHER_RETRY_INTERVAL_MS) {
    return;
  }

  s_lastAttemptMs = now;
  WeatherSnapshot fresh = {};
  if (fetchWeather(&fresh)) {
    s_snapshot = fresh;
    s_lastFetchMs = now;
  }
}
