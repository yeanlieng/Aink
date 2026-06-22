#ifndef UI_STATUS_BAR_H
#define UI_STATUS_BAR_H

#include "weather_icons.h"

#include <stdbool.h>

void ui_status_bar_init(void);
void ui_status_bar_update(int batteryPercent, bool wifiConnected,
                          bool showWeather, WeatherIconKind weatherIcon,
                          int weatherTempC);

#endif
