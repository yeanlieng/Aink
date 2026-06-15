#ifndef SETTINGS_API_H
#define SETTINGS_API_H

#include <stddef.h>
#include <stdbool.h>

#include "ai_model_config.h"
#include "app_locale.h"

void settings_api_get_wifi_ssid(char *out, size_t outLen);
void settings_api_get_ip(char *out, size_t outLen);
bool settings_api_is_wifi_connected(void);

AiProvider settings_api_get_provider(void);
void settings_api_set_provider(AiProvider provider);
int settings_api_get_model_index(void);
void settings_api_set_model_index(int modelIndex);
const char *settings_api_get_model_id(void);
const char *settings_api_get_model_label(void);

bool settings_api_has_api_key(void);
void settings_api_set_api_key(const char *apiKey);
void settings_api_clear_api_key(void);
void settings_api_get_api_key(char *out, size_t outLen);

AppLanguage settings_api_get_language(void);
void settings_api_set_language(AppLanguage lang);

void settings_api_request_portal_restart(void);
void settings_api_forget_wifi_and_restart(void);

bool settings_api_consume_force_portal_boot(void);

bool settings_api_has_weather_api(void);
void settings_api_set_weather_api_key(const char *apiKey);
void settings_api_set_weather_api_host(const char *apiHost);
void settings_api_clear_weather_api(void);
void settings_api_get_weather_api_key(char *out, size_t outLen);
void settings_api_get_weather_api_host(char *out, size_t outLen);

void settings_api_get_watchlist(char *out, size_t outLen);
void settings_api_set_watchlist(const char *watchlist);
void settings_api_clear_watchlist(void);
bool settings_api_has_watchlist(void);
int settings_api_watchlist_count(void);

#endif
