#include "ui_weather.h"

#include "weather_icons.h"
#include "weather_metrics_icons.h"
#include "weather_service.h"
#include "app_locale.h"
#include "ui_fonts.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MAIN_ICON_PX       32
#define FORECAST_ICON_PX   18
#define FORECAST_COL_W     64
#define FORECAST_COL_X0    6
#define METRIC_COL_LEFT    6
#define METRIC_COL_RIGHT   100
#define METRIC_ROW_H       20
#define HEADER_DIVIDER_Y   56
#define FORECAST_ICON_Y    60
#define FORECAST_DAY_Y     80
#define FORECAST_TEMP_Y    94
#define FORECAST_DIVIDER_Y 112
#define METRIC_TOP_Y       118

static lv_obj_t *s_screenWeather = nullptr;
static lv_obj_t *s_mainIconCanvas = nullptr;
static lv_obj_t *s_locationLabel = nullptr;
static lv_obj_t *s_dateLabel = nullptr;
static lv_obj_t *s_tempLabel = nullptr;
static lv_obj_t *s_feelsLabel = nullptr;
static lv_obj_t *s_conditionLabel = nullptr;
static lv_obj_t *s_forecastIcons[WEATHER_DAILY_COUNT];
static lv_obj_t *s_forecastDayLabels[WEATHER_DAILY_COUNT];
static lv_obj_t *s_forecastTempLabels[WEATHER_DAILY_COUNT];
static lv_obj_t *s_metricIcons[6];
static lv_obj_t *s_metricLabels[6];

static lv_color_t s_mainIconBuf[MAIN_ICON_PX * MAIN_ICON_PX];
static lv_color_t s_forecastIconBuf[WEATHER_DAILY_COUNT][FORECAST_ICON_PX * FORECAST_ICON_PX];
static lv_color_t s_metricIconBuf[6][METRIC_ICON_SIZE * METRIC_ICON_SIZE];

static void style_text_label(lv_obj_t *label, const lv_font_t *font) {
  lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_text_opa(label, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(label, 0, LV_PART_MAIN);
  if (font != nullptr) {
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  }
}

static void add_divider(lv_obj_t *parent, lv_coord_t y) {
  lv_obj_t *divider = lv_obj_create(parent);
  lv_obj_set_size(divider, 188, 1);
  lv_obj_set_style_bg_color(divider, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(divider, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(divider, 0, LV_PART_MAIN);
  lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(divider, LV_ALIGN_TOP_MID, 0, y);
}

static void canvas_set_weather_icon_scaled(lv_obj_t *canvas, int size, WeatherIconKind kind) {
  if ((unsigned)kind >= WEATHER_ICON_COUNT) {
    kind = WEATHER_ICON_CLOUDY;
  }
  if (size <= 0) {
    return;
  }

  for (int row = 0; row < size; row++) {
    const int srcRow = (row * WEATHER_ICON_SIZE) / size;
    const uint16_t mask = weather_icon_bitmaps[kind][srcRow];
    for (int col = 0; col < size; col++) {
      const int srcCol = (col * WEATHER_ICON_SIZE) / size;
      const bool black = (mask >> (WEATHER_ICON_SIZE - 1 - srcCol)) & 0x01;
      lv_canvas_set_px(canvas, col, row,
                       black ? lv_color_black() : lv_color_white());
    }
  }
}

static void canvas_set_metric_icon(lv_obj_t *canvas, MetricIconKind kind) {
  if ((unsigned)kind >= METRIC_ICON_COUNT) {
    return;
  }

  for (int row = 0; row < METRIC_ICON_SIZE; row++) {
    const uint16_t mask = metric_icon_bitmaps[kind][row];
    for (int col = 0; col < METRIC_ICON_SIZE; col++) {
      const bool black = (mask >> (METRIC_ICON_SIZE - 1 - col)) & 0x01;
      lv_canvas_set_px(canvas, col, row,
                       black ? lv_color_black() : lv_color_white());
    }
  }
}

static void format_date_line(char *out, size_t outLen) {
  struct tm timeInfo = {};
  if (!getLocalTime(&timeInfo)) {
    snprintf(out, outLen, "--/--");
    return;
  }

  snprintf(out, outLen, "%s %d/%d",
           app_tr_weekday(timeInfo.tm_wday),
           timeInfo.tm_mon + 1,
           timeInfo.tm_mday);
}

static void format_uv_line(const WeatherSnapshot *snap, char *out, size_t outLen) {
  if (snap->uvIndexTenths < 0) {
    snprintf(out, outLen, "%s", app_tr(TR_FMT_UV_NA));
    return;
  }
  const int whole = snap->uvIndexTenths / 10;
  const int frac = snap->uvIndexTenths % 10;
  const char *level = weather_service_uv_level(snap->uvIndexTenths);
  if (frac == 0) {
    snprintf(out, outLen, app_tr(TR_FMT_UV_INT), whole, level);
  } else {
    snprintf(out, outLen, app_tr(TR_FMT_UV_FRAC), whole, frac);
    const size_t used = strlen(out);
    if (used + 2 < outLen) {
      snprintf(out + used, outLen - used, " %s", level);
    }
  }
}

static void bind_weather_data(void) {
  WeatherSnapshot snap = {};
  weather_service_get_snapshot(&snap);

  char line[40];
  if (snap.location[0] != '\0') {
    lv_label_set_text(s_locationLabel, snap.location);
  } else {
    lv_label_set_text(s_locationLabel, "--");
  }
  format_date_line(line, sizeof(line));
  lv_label_set_text(s_dateLabel, line);

  if (!snap.valid) {
    lv_label_set_text(s_tempLabel, "--");
    lv_label_set_text(s_feelsLabel, app_tr(TR_NO_DATA));
    lv_label_set_text(s_conditionLabel, app_tr(TR_CHECK_WIFI));
    canvas_set_weather_icon_scaled(s_mainIconCanvas, MAIN_ICON_PX, WEATHER_ICON_CLOUDY);
    for (int i = 0; i < WEATHER_DAILY_COUNT; i++) {
      lv_label_set_text(s_forecastDayLabels[i], "");
      lv_label_set_text(s_forecastTempLabels[i], "");
    }
    for (int i = 0; i < 6; i++) {
      lv_label_set_text(s_metricLabels[i], "");
    }
    return;
  }

  snprintf(line, sizeof(line), "%dC", snap.tempC);
  lv_label_set_text(s_tempLabel, line);
  snprintf(line, sizeof(line), app_tr(TR_FMT_FEELS), snap.feelsLikeC);
  lv_label_set_text(s_feelsLabel, line);
  lv_label_set_text(s_conditionLabel, weather_service_wmo_to_label(snap.wmoCode));
  canvas_set_weather_icon_scaled(s_mainIconCanvas, MAIN_ICON_PX, snap.icon);

  for (int i = 0; i < WEATHER_DAILY_COUNT; i++) {
    const lv_coord_t colX = FORECAST_COL_X0 + (i * FORECAST_COL_W);
    canvas_set_weather_icon_scaled(s_forecastIcons[i], FORECAST_ICON_PX, snap.daily[i].icon);
    lv_label_set_text(s_forecastDayLabels[i], app_tr_weekday(snap.daily[i].wday));
    snprintf(line, sizeof(line), "%d|%d", snap.daily[i].lo, snap.daily[i].hi);
    lv_label_set_text(s_forecastTempLabels[i], line);
    lv_obj_set_pos(s_forecastIcons[i],
                   colX + (FORECAST_COL_W - FORECAST_ICON_PX) / 2,
                   FORECAST_ICON_Y);
    lv_obj_set_pos(s_forecastDayLabels[i], colX + 16, FORECAST_DAY_Y);
    lv_obj_set_pos(s_forecastTempLabels[i], colX + 10, FORECAST_TEMP_Y);
  }

  if (snap.humidityPct >= 0) {
    snprintf(line, sizeof(line), app_tr(TR_FMT_HUM), snap.humidityPct);
  } else {
    snprintf(line, sizeof(line), "%s", app_tr(TR_FMT_HUM_NA));
  }
  lv_label_set_text(s_metricLabels[0], line);

  format_uv_line(&snap, line, sizeof(line));
  lv_label_set_text(s_metricLabels[1], line);

  if (snap.windSpeedKmh >= 0) {
    snprintf(line, sizeof(line), app_tr(TR_FMT_WIND), snap.windSpeedKmh);
  } else {
    snprintf(line, sizeof(line), "%s", app_tr(TR_FMT_WIND_NA));
  }
  lv_label_set_text(s_metricLabels[2], line);

  if (snap.aqiValid) {
    snprintf(line, sizeof(line), app_tr(TR_FMT_AQI), snap.usAqi, weather_service_aqi_level(snap.usAqi));
  } else {
    snprintf(line, sizeof(line), "%s", app_tr(TR_FMT_AQI_NA));
  }
  lv_label_set_text(s_metricLabels[3], line);

  snprintf(line, sizeof(line), "%s-%s", snap.sunrise, snap.sunset);
  lv_label_set_text(s_metricLabels[4], line);

  if (snap.pressureHpa > 0) {
    snprintf(line, sizeof(line), app_tr(TR_FMT_PRESSURE), snap.pressureHpa);
  } else {
    snprintf(line, sizeof(line), "%s", app_tr(TR_FMT_PRESSURE_NA));
  }
  lv_label_set_text(s_metricLabels[5], line);
}

static lv_obj_t *create_metric_row(lv_obj_t *parent, int index, lv_coord_t x, lv_coord_t y,
                                   MetricIconKind iconKind, lv_color_t *iconBuf) {
  s_metricIcons[index] = lv_canvas_create(parent);
  lv_canvas_set_buffer(s_metricIcons[index], iconBuf, METRIC_ICON_SIZE, METRIC_ICON_SIZE,
                       LV_IMG_CF_TRUE_COLOR);
  lv_obj_set_pos(s_metricIcons[index], x, y);
  canvas_set_metric_icon(s_metricIcons[index], iconKind);

  s_metricLabels[index] = lv_label_create(parent);
  style_text_label(s_metricLabels[index], UI_FONT_SM);
  lv_label_set_text(s_metricLabels[index], "");
  const lv_coord_t labelX = x + 14;
  const lv_coord_t labelW = (x < METRIC_COL_RIGHT)
                                ? (METRIC_COL_RIGHT - labelX - 4)
                                : (200 - 6 - labelX);
  lv_obj_set_width(s_metricLabels[index], labelW);
  lv_label_set_long_mode(s_metricLabels[index], LV_LABEL_LONG_CLIP);
  lv_obj_set_pos(s_metricLabels[index], labelX, y - 1);
  return s_metricLabels[index];
}

void ui_weather_init(void) {
  s_screenWeather = lv_obj_create(nullptr);
  lv_obj_set_size(s_screenWeather, 200, 180);
  lv_obj_set_style_bg_color(s_screenWeather, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_screenWeather, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_screenWeather, LV_OBJ_FLAG_SCROLLABLE);

  s_mainIconCanvas = lv_canvas_create(s_screenWeather);
  lv_canvas_set_buffer(s_mainIconCanvas, s_mainIconBuf, MAIN_ICON_PX, MAIN_ICON_PX,
                       LV_IMG_CF_TRUE_COLOR);
  lv_obj_set_pos(s_mainIconCanvas, 6, 8);

  s_locationLabel = lv_label_create(s_screenWeather);
  style_text_label(s_locationLabel, UI_FONT_SM);
  lv_label_set_text(s_locationLabel, "--");
  lv_obj_set_width(s_locationLabel, 96);
  lv_label_set_long_mode(s_locationLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(s_locationLabel, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
  lv_obj_align(s_locationLabel, LV_ALIGN_TOP_RIGHT, -6, 4);

  s_dateLabel = lv_label_create(s_screenWeather);
  style_text_label(s_dateLabel, UI_FONT_SM);
  lv_label_set_text(s_dateLabel, "--/--");
  lv_obj_align(s_dateLabel, LV_ALIGN_TOP_RIGHT, -6, 18);

  s_tempLabel = lv_label_create(s_screenWeather);
  style_text_label(s_tempLabel, UI_FONT_MD);
  lv_obj_set_pos(s_tempLabel, 46, 8);

  s_feelsLabel = lv_label_create(s_screenWeather);
  style_text_label(s_feelsLabel, UI_FONT_SM);
  lv_obj_set_pos(s_feelsLabel, 46, 26);

  s_conditionLabel = lv_label_create(s_screenWeather);
  style_text_label(s_conditionLabel, UI_FONT_SM);
  lv_obj_set_pos(s_conditionLabel, 46, 40);

  add_divider(s_screenWeather, HEADER_DIVIDER_Y);

  for (int i = 0; i < WEATHER_DAILY_COUNT; i++) {
    s_forecastIcons[i] = lv_canvas_create(s_screenWeather);
    lv_canvas_set_buffer(s_forecastIcons[i], s_forecastIconBuf[i],
                         FORECAST_ICON_PX, FORECAST_ICON_PX, LV_IMG_CF_TRUE_COLOR);

    s_forecastDayLabels[i] = lv_label_create(s_screenWeather);
    style_text_label(s_forecastDayLabels[i], UI_FONT_SM);
    lv_label_set_text(s_forecastDayLabels[i], "");

    s_forecastTempLabels[i] = lv_label_create(s_screenWeather);
    style_text_label(s_forecastTempLabels[i], UI_FONT_SM);
    lv_label_set_text(s_forecastTempLabels[i], "");
  }

  add_divider(s_screenWeather, FORECAST_DIVIDER_Y);

  create_metric_row(s_screenWeather, 0, METRIC_COL_LEFT, METRIC_TOP_Y,
                    METRIC_ICON_HUMIDITY, s_metricIconBuf[0]);
  create_metric_row(s_screenWeather, 1, METRIC_COL_RIGHT, METRIC_TOP_Y,
                    METRIC_ICON_UV, s_metricIconBuf[1]);
  create_metric_row(s_screenWeather, 2, METRIC_COL_LEFT, METRIC_TOP_Y + METRIC_ROW_H,
                    METRIC_ICON_WIND, s_metricIconBuf[2]);
  create_metric_row(s_screenWeather, 3, METRIC_COL_RIGHT, METRIC_TOP_Y + METRIC_ROW_H,
                    METRIC_ICON_AQI, s_metricIconBuf[3]);
  create_metric_row(s_screenWeather, 4, METRIC_COL_LEFT, METRIC_TOP_Y + (METRIC_ROW_H * 2),
                    METRIC_ICON_SUNRISE, s_metricIconBuf[4]);
  create_metric_row(s_screenWeather, 5, METRIC_COL_RIGHT, METRIC_TOP_Y + (METRIC_ROW_H * 2),
                    METRIC_ICON_PRESSURE, s_metricIconBuf[5]);

  bind_weather_data();
}

void ui_weather_show(void) {
  bind_weather_data();
  lv_scr_load(s_screenWeather);
  lv_obj_invalidate(s_screenWeather);
}

void ui_weather_refresh(void) {
  if (!ui_weather_is_active()) {
    return;
  }
  bind_weather_data();
  lv_obj_invalidate(s_screenWeather);
}

bool ui_weather_is_active(void) {
  return s_screenWeather != nullptr && lv_scr_act() == s_screenWeather;
}

lv_obj_t *ui_weather_get_screen(void) {
  return s_screenWeather;
}
