#ifndef WEATHER_SERVICE_H
#define WEATHER_SERVICE_H

#include "weather_icons.h"

#define WEATHER_DAILY_COUNT 3

typedef struct {
  bool valid;
  int tempC;
  int feelsLikeC;
  int wmoCode;
  WeatherIconKind icon;
  char condition[16];
  char location[28];
  int hiToday;
  int loToday;
  int humidityPct;
  int uvIndexTenths;
  int windSpeedKmh;
  int pressureHpa;
  char sunrise[6];
  char sunset[6];
  bool aqiValid;
  int usAqi;
  int pm25Tenths;
  struct {
    char label[8];
    int wday;
    int hi;
    int lo;
    int wmoCode;
    WeatherIconKind icon;
    char condition[16];
  } daily[WEATHER_DAILY_COUNT];
} WeatherSnapshot;

void weather_service_reset(void);
void weather_service_update(bool force);
void weather_service_request_update(bool force);
bool weather_service_is_busy(void);
bool weather_service_consume_fresh_fetch(void);
void weather_service_get_snapshot(WeatherSnapshot *out);

WeatherIconKind weather_service_wmo_to_icon(int wmo);
WeatherIconKind weather_service_qweather_icon_to_icon(int iconCode);
const char *weather_service_wmo_to_label(int wmo);
const char *weather_service_uv_level(int uvIndexTenths);
const char *weather_service_aqi_level(int usAqi);

#endif
