#include "settings_api.h"

#include "app_locale.h"

#include <Preferences.h>
#include <WiFi.h>

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#define PREFS_NAMESPACE        "epaper"
#define PREFS_KEY_SSID         "ssid"
#define PREFS_KEY_PASS         "pass"
#define PREFS_KEY_FORCE_PORTAL "force_portal"
#define PREFS_KEY_LANGUAGE     "lang"
#define PREFS_KEY_AI_PROVIDER  "ai_provider"
#define PREFS_KEY_AI_MODEL_IDX "ai_model"
#define PREFS_KEY_AI_API_KEY        "ai_api_key"
#define PREFS_KEY_WEATHER_API_KEY   "wx_api_key"
#define PREFS_KEY_WEATHER_API_HOST  "wx_api_host"
#define PREFS_KEY_STOCK_WATCHLIST   "stk_list"
#define PREFS_API_KEY_MAX           128
#define PREFS_WEATHER_HOST_MAX      64
#define PREFS_WATCHLIST_MAX         128
#define PREFS_WATCHLIST_DEFAULT     "sh600519,AAPL"

static void clamp_model_index_for_provider(AiProvider provider, int *modelIndex) {
  const int count = ai_provider_model_count(provider);
  if (count <= 0) {
    *modelIndex = 0;
    return;
  }
  if (*modelIndex < 0 || *modelIndex >= count) {
    *modelIndex = 0;
  }
}

void settings_api_get_wifi_ssid(char *out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    snprintf(out, outLen, "%s", WiFi.SSID().c_str());
    return;
  }

  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true);
  const String stored = prefs.getString(PREFS_KEY_SSID, "");
  prefs.end();
  snprintf(out, outLen, "%s", stored.c_str());
}

void settings_api_get_ip(char *out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    snprintf(out, outLen, "--");
    return;
  }

  snprintf(out, outLen, "%s", WiFi.localIP().toString().c_str());
}

bool settings_api_is_wifi_connected(void) {
  return WiFi.status() == WL_CONNECTED;
}

AiProvider settings_api_get_provider(void) {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true);
  const uint8_t stored = prefs.getUChar(PREFS_KEY_AI_PROVIDER, (uint8_t)AI_PROVIDER_OPENAI);
  prefs.end();
  if (stored >= (uint8_t)AI_PROVIDER_COUNT) {
    return AI_PROVIDER_OPENAI;
  }
  return (AiProvider)stored;
}

void settings_api_set_provider(AiProvider provider) {
  if ((unsigned)provider >= AI_PROVIDER_COUNT) {
    return;
  }

  int modelIndex = settings_api_get_model_index();
  clamp_model_index_for_provider(provider, &modelIndex);

  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putUChar(PREFS_KEY_AI_PROVIDER, (uint8_t)provider);
  prefs.putUChar(PREFS_KEY_AI_MODEL_IDX, (uint8_t)modelIndex);
  prefs.end();
}

int settings_api_get_model_index(void) {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true);
  int modelIndex = (int)prefs.getUChar(PREFS_KEY_AI_MODEL_IDX, 0);
  prefs.end();
  clamp_model_index_for_provider(settings_api_get_provider(), &modelIndex);
  return modelIndex;
}

void settings_api_set_model_index(int modelIndex) {
  AiProvider provider = settings_api_get_provider();
  clamp_model_index_for_provider(provider, &modelIndex);

  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putUChar(PREFS_KEY_AI_MODEL_IDX, (uint8_t)modelIndex);
  prefs.end();
}

const char *settings_api_get_model_id(void) {
  const AiProvider provider = settings_api_get_provider();
  const int modelIndex = settings_api_get_model_index();
  return ai_provider_model_id(provider, modelIndex);
}

const char *settings_api_get_model_label(void) {
  const AiProvider provider = settings_api_get_provider();
  const int modelIndex = settings_api_get_model_index();
  return ai_provider_model_label(provider, modelIndex);
}

bool settings_api_has_api_key(void) {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true);
  const String key = prefs.getString(PREFS_KEY_AI_API_KEY, "");
  prefs.end();
  return key.length() > 0;
}

void settings_api_set_api_key(const char *apiKey) {
  if (apiKey == nullptr) {
    return;
  }

  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putString(PREFS_KEY_AI_API_KEY, apiKey);
  prefs.end();
  Serial.println("[Settings] AI API key saved");
}

void settings_api_clear_api_key(void) {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.remove(PREFS_KEY_AI_API_KEY);
  prefs.end();
  Serial.println("[Settings] AI API key cleared");
}

void settings_api_get_api_key(char *out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return;
  }

  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true);
  const String key = prefs.getString(PREFS_KEY_AI_API_KEY, "");
  prefs.end();
  snprintf(out, outLen, "%s", key.c_str());
}

AppLanguage settings_api_get_language(void) {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true);
  const uint8_t stored = prefs.getUChar(PREFS_KEY_LANGUAGE, (uint8_t)APP_LANG_EN);
  prefs.end();
  return stored == (uint8_t)APP_LANG_ZH ? APP_LANG_ZH : APP_LANG_EN;
}

void settings_api_set_language(AppLanguage lang) {
  const uint8_t value = (lang == APP_LANG_ZH) ? (uint8_t)APP_LANG_ZH : (uint8_t)APP_LANG_EN;
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putUChar(PREFS_KEY_LANGUAGE, value);
  prefs.end();
}

static void write_force_portal_flag(void) {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putBool(PREFS_KEY_FORCE_PORTAL, true);
  prefs.end();
}

void settings_api_request_portal_restart(void) {
  Serial.println("[Settings] restart -> WiFi portal");
  write_force_portal_flag();
  delay(200);
  ESP.restart();
}

void settings_api_forget_wifi_and_restart(void) {
  Serial.println("[Settings] forget WiFi and restart");
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.remove(PREFS_KEY_SSID);
  prefs.remove(PREFS_KEY_PASS);
  prefs.putBool(PREFS_KEY_FORCE_PORTAL, true);
  prefs.end();
  delay(200);
  ESP.restart();
}

static void normalize_weather_host(char *host, size_t hostLen) {
  if (host == nullptr || hostLen == 0 || host[0] == '\0') {
    return;
  }

  size_t len = strlen(host);
  if (len >= 8 && strncmp(host, "https://", 8) == 0) {
    memmove(host, host + 8, len - 7);
    len -= 8;
  } else if (len >= 7 && strncmp(host, "http://", 7) == 0) {
    memmove(host, host + 7, len - 6);
    len -= 7;
  }

  if (len > 0 && host[len - 1] == '/') {
    host[len - 1] = '\0';
  }
}

bool settings_api_has_weather_api(void) {
  char key[PREFS_API_KEY_MAX] = {};
  char host[PREFS_WEATHER_HOST_MAX] = {};
  settings_api_get_weather_api_key(key, sizeof(key));
  settings_api_get_weather_api_host(host, sizeof(host));
  return key[0] != '\0' && host[0] != '\0';
}

void settings_api_set_weather_api_key(const char *apiKey) {
  if (apiKey == nullptr) {
    return;
  }

  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putString(PREFS_KEY_WEATHER_API_KEY, apiKey);
  prefs.end();
  Serial.println("[Settings] weather API key saved");
}

void settings_api_set_weather_api_host(const char *apiHost) {
  if (apiHost == nullptr) {
    return;
  }

  char host[PREFS_WEATHER_HOST_MAX] = {};
  snprintf(host, sizeof(host), "%s", apiHost);
  normalize_weather_host(host, sizeof(host));

  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putString(PREFS_KEY_WEATHER_API_HOST, host);
  prefs.end();
  Serial.printf("[Settings] weather API host saved: %s\n", host);
}

void settings_api_clear_weather_api(void) {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.remove(PREFS_KEY_WEATHER_API_KEY);
  prefs.remove(PREFS_KEY_WEATHER_API_HOST);
  prefs.end();
  Serial.println("[Settings] weather API cleared");
}

void settings_api_get_weather_api_key(char *out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return;
  }

  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true);
  const String key = prefs.getString(PREFS_KEY_WEATHER_API_KEY, "");
  prefs.end();
  snprintf(out, outLen, "%s", key.c_str());
}

void settings_api_get_weather_api_host(char *out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return;
  }

  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true);
  const String host = prefs.getString(PREFS_KEY_WEATHER_API_HOST, "");
  prefs.end();
  snprintf(out, outLen, "%s", host.c_str());
}

void settings_api_get_watchlist(char *out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return;
  }

  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true);
  const String list = prefs.getString(PREFS_KEY_STOCK_WATCHLIST, PREFS_WATCHLIST_DEFAULT);
  prefs.end();
  snprintf(out, outLen, "%s", list.c_str());
}

void settings_api_set_watchlist(const char *watchlist) {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  if (watchlist == nullptr || watchlist[0] == '\0') {
    prefs.putString(PREFS_KEY_STOCK_WATCHLIST, PREFS_WATCHLIST_DEFAULT);
  } else {
    prefs.putString(PREFS_KEY_STOCK_WATCHLIST, watchlist);
  }
  prefs.end();
}

void settings_api_clear_watchlist(void) {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.remove(PREFS_KEY_STOCK_WATCHLIST);
  prefs.end();
}

bool settings_api_has_watchlist(void) {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true);
  const bool hasKey = prefs.isKey(PREFS_KEY_STOCK_WATCHLIST);
  const String list = prefs.getString(PREFS_KEY_STOCK_WATCHLIST, "");
  prefs.end();
  return hasKey && list.length() > 0;
}

int settings_api_watchlist_count(void) {
  char list[PREFS_WATCHLIST_MAX];
  settings_api_get_watchlist(list, sizeof(list));

  int count = 0;
  char *savePtr = nullptr;
  char *token = strtok_r(list, ",", &savePtr);
  while (token != nullptr) {
    while (*token == ' ' || *token == '\t') {
      token++;
    }
    if (*token != '\0') {
      count++;
    }
    token = strtok_r(nullptr, ",", &savePtr);
  }
  return count;
}

bool settings_api_consume_force_portal_boot(void) {
  Preferences prefs;
  prefs.begin(PREFS_NAMESPACE, true);
  const bool forcePortal = prefs.getBool(PREFS_KEY_FORCE_PORTAL, false);
  prefs.end();
  if (!forcePortal) {
    return false;
  }

  prefs.begin(PREFS_NAMESPACE, false);
  prefs.remove(PREFS_KEY_FORCE_PORTAL);
  prefs.end();
  Serial.println("[Settings] force portal boot flag consumed");
  return true;
}
