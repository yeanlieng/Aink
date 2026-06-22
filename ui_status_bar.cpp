#include "ui_status_bar.h"

#include "epaper_canvas.h"
#include "ui_fonts.h"

#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>
#include <time.h>

#define STATUS_BAR_H      EPAPER_STATUS_BAR_HEIGHT
#define WIFI_ICON_W       15
#define WIFI_ICON_H       9
#define BATTERY_ICON_W    15
#define BATTERY_ICON_H    8

static lv_obj_t *s_bar = nullptr;
static lv_obj_t *s_wifiCanvas = nullptr;
static lv_obj_t *s_dateLabel = nullptr;
static lv_obj_t *s_weatherCanvas = nullptr;
static lv_obj_t *s_tempLabel = nullptr;
static lv_obj_t *s_batteryCanvas = nullptr;
static lv_obj_t *s_divider = nullptr;

static lv_color_t s_wifiBuf[WIFI_ICON_W * WIFI_ICON_H];
static lv_color_t s_weatherBuf[WEATHER_ICON_SIZE * WEATHER_ICON_SIZE];
static lv_color_t s_batteryBuf[BATTERY_ICON_W * BATTERY_ICON_H];

static const char *weekdayShort(int wday) {
  static const char *names[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  if (wday < 0 || wday > 6) {
    return "---";
  }
  return names[wday];
}

static void styleStatusText(lv_obj_t *label) {
  lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_text_opa(label, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(label, 0, LV_PART_MAIN);
  lv_obj_set_style_text_font(label, UI_FONT_SM, LV_PART_MAIN);
}

static void canvasSet(lv_obj_t *canvas, int x, int y, bool black) {
  lv_canvas_set_px(canvas, x, y, black ? lv_color_black() : lv_color_white());
}

static void clearCanvas(lv_obj_t *canvas, int width, int height) {
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      canvasSet(canvas, x, y, false);
    }
  }
}

static void drawWifiIcon(bool connected) {
  if (s_wifiCanvas == nullptr) {
    return;
  }
  clearCanvas(s_wifiCanvas, WIFI_ICON_W, WIFI_ICON_H);

  static const uint16_t disconnectedIcon[WIFI_ICON_H] = {
      0b010000000001000,
      0b001000000010000,
      0b000100000100000,
      0b000010001000000,
      0b000001010000000,
      0b000000100000000,
      0b000000000000000,
      0b000000000000000,
      0b000000100000000,
  };
  static const uint16_t connectedIcon[WIFI_ICON_H] = {
      0b000111111111000,
      0b001100000011000,
      0b000000000000000,
      0b000001111100000,
      0b000011000110000,
      0b000000000000000,
      0b000000111000000,
      0b000000000000000,
      0b000000010000000,
  };

  const uint16_t *icon = connected ? connectedIcon : disconnectedIcon;
  for (int row = 0; row < WIFI_ICON_H; row++) {
    const uint16_t mask = icon[row];
    for (int col = 0; col < WIFI_ICON_W; col++) {
      if ((mask >> (WIFI_ICON_W - 1 - col)) & 0x01) {
        canvasSet(s_wifiCanvas, col, row, true);
      }
    }
  }
}

static void drawWeatherIcon(WeatherIconKind kind) {
  if (s_weatherCanvas == nullptr) {
    return;
  }
  if ((unsigned)kind >= WEATHER_ICON_COUNT) {
    kind = WEATHER_ICON_CLOUDY;
  }
  clearCanvas(s_weatherCanvas, WEATHER_ICON_SIZE, WEATHER_ICON_SIZE);
  for (int row = 0; row < WEATHER_ICON_SIZE; row++) {
    const uint16_t mask = weather_icon_bitmaps[kind][row];
    for (int col = 0; col < WEATHER_ICON_SIZE; col++) {
      if ((mask >> (WEATHER_ICON_SIZE - 1 - col)) & 0x01) {
        canvasSet(s_weatherCanvas, col, row, true);
      }
    }
  }
}

static void drawBatteryIcon(int percent) {
  if (s_batteryCanvas == nullptr) {
    return;
  }
  if (percent < 0) {
    percent = 100;
  }
  if (percent > 100) {
    percent = 100;
  }
  clearCanvas(s_batteryCanvas, BATTERY_ICON_W, BATTERY_ICON_H);

  for (int dy = 0; dy < BATTERY_ICON_H; dy++) {
    for (int dx = 0; dx < BATTERY_ICON_W - 1; dx++) {
      const bool edge = (dy == 0 || dy == BATTERY_ICON_H - 1 ||
                         dx == 0 || dx == BATTERY_ICON_W - 2);
      if (edge) {
        canvasSet(s_batteryCanvas, dx, dy, true);
      }
    }
  }
  for (int dy = 2; dy < 6; dy++) {
    canvasSet(s_batteryCanvas, BATTERY_ICON_W - 1, dy, true);
  }

  int fillWidth = (percent * 10) / 100;
  if (fillWidth < 1 && percent > 0) {
    fillWidth = 1;
  }
  for (int dy = 2; dy <= 5; dy++) {
    for (int dx = 2; dx < 2 + fillWidth; dx++) {
      canvasSet(s_batteryCanvas, dx, dy, true);
    }
  }
}

void ui_status_bar_init(void) {
  lv_obj_t *top = lv_layer_top();
  s_bar = lv_obj_create(top);
  lv_obj_set_size(s_bar, EPD_1IN54_V2_WIDTH, STATUS_BAR_H);
  lv_obj_set_pos(s_bar, 0, 0);
  lv_obj_set_style_bg_color(s_bar, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_bar, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(s_bar, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_bar, 0, LV_PART_MAIN);
  lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_CLICKABLE);

  s_wifiCanvas = lv_canvas_create(s_bar);
  lv_canvas_set_buffer(s_wifiCanvas, s_wifiBuf, WIFI_ICON_W, WIFI_ICON_H,
                       LV_IMG_CF_TRUE_COLOR);
  lv_obj_set_pos(s_wifiCanvas, 2, 6);

  s_dateLabel = lv_label_create(s_bar);
  styleStatusText(s_dateLabel);
  lv_obj_set_pos(s_dateLabel, 18, 2);
  lv_obj_set_width(s_dateLabel, 96);
  lv_label_set_long_mode(s_dateLabel, LV_LABEL_LONG_CLIP);

  s_weatherCanvas = lv_canvas_create(s_bar);
  lv_canvas_set_buffer(s_weatherCanvas, s_weatherBuf, WEATHER_ICON_SIZE,
                       WEATHER_ICON_SIZE, LV_IMG_CF_TRUE_COLOR);
  lv_obj_set_pos(s_weatherCanvas, 118, 1);

  s_tempLabel = lv_label_create(s_bar);
  styleStatusText(s_tempLabel);
  lv_obj_set_pos(s_tempLabel, 136, 2);
  lv_obj_set_width(s_tempLabel, 32);
  lv_label_set_long_mode(s_tempLabel, LV_LABEL_LONG_CLIP);

  s_batteryCanvas = lv_canvas_create(s_bar);
  lv_canvas_set_buffer(s_batteryCanvas, s_batteryBuf, BATTERY_ICON_W,
                       BATTERY_ICON_H, LV_IMG_CF_TRUE_COLOR);
  lv_obj_set_pos(s_batteryCanvas, EPD_1IN54_V2_WIDTH - BATTERY_ICON_W, 4);

  s_divider = lv_obj_create(s_bar);
  lv_obj_set_size(s_divider, EPD_1IN54_V2_WIDTH, 1);
  lv_obj_set_pos(s_divider, 0, STATUS_BAR_H - 1);
  lv_obj_set_style_bg_color(s_divider, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_divider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_divider, 0, LV_PART_MAIN);
  lv_obj_clear_flag(s_divider, LV_OBJ_FLAG_SCROLLABLE);

  ui_status_bar_update(-1, false, false, WEATHER_ICON_CLOUDY, 0);
}

void ui_status_bar_update(int batteryPercent, bool wifiConnected,
                          bool showWeather, WeatherIconKind weatherIcon,
                          int weatherTempC) {
  if (s_bar == nullptr) {
    return;
  }

  drawWifiIcon(wifiConnected);
  drawBatteryIcon(batteryPercent);

  struct tm timeInfo;
  char dateLine[32];
  if (getLocalTime(&timeInfo, 0)) {
    snprintf(dateLine, sizeof(dateLine), "%s %d/%d %02d:%02d",
             weekdayShort(timeInfo.tm_wday),
             timeInfo.tm_mon + 1,
             timeInfo.tm_mday,
             timeInfo.tm_hour,
             timeInfo.tm_min);
  } else {
    snprintf(dateLine, sizeof(dateLine), "NO TIME");
    showWeather = false;
  }
  lv_label_set_text(s_dateLabel, dateLine);

  if (showWeather) {
    char tempLine[8];
    if (weatherTempC >= -9 && weatherTempC <= 99) {
      snprintf(tempLine, sizeof(tempLine), "%2dC", weatherTempC);
    } else {
      snprintf(tempLine, sizeof(tempLine), "--C");
    }
    drawWeatherIcon(weatherIcon);
    lv_label_set_text(s_tempLabel, tempLine);
    lv_obj_clear_flag(s_weatherCanvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_tempLabel, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_weatherCanvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_tempLabel, LV_OBJ_FLAG_HIDDEN);
  }

  lv_obj_invalidate(s_bar);
}
