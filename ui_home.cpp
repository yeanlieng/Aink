#include "ui_home.h"

#include "app_locale.h"
#include "weather_icons.h"
#include "weather_service.h"
#include "stock_service.h"
#include "settings_icons.h"
#include "settings_api.h"
#include "ui_fonts.h"

#include <stdio.h>

#define WEATHER_TILE_ICON_PX  32
#define SETTINGS_TILE_ICON_PX 32

static lv_obj_t *s_screenHome = nullptr;
static lv_obj_t *s_screenDetail = nullptr;
static lv_obj_t *s_tiles[4];
static lv_obj_t *s_tileLabels[4];
static lv_obj_t *s_weatherIconCanvas = nullptr;
static lv_obj_t *s_weatherTempLabel = nullptr;
static lv_obj_t *s_visionIconCanvas = nullptr;
static lv_obj_t *s_visionHintLabel = nullptr;
static lv_obj_t *s_settingsIconCanvas = nullptr;
static lv_obj_t *s_settingsStatusLabel = nullptr;
static lv_obj_t *s_stockIconCanvas = nullptr;
static lv_obj_t *s_stockPreviewLabel = nullptr;
static lv_color_t s_stockIconBuf[SETTINGS_TILE_ICON_PX * SETTINGS_TILE_ICON_PX];
static lv_obj_t *s_detailTitle = nullptr;
static lv_obj_t *s_detailBody = nullptr;
static lv_color_t s_weatherIconBuf[WEATHER_TILE_ICON_PX * WEATHER_TILE_ICON_PX];
static lv_color_t s_visionIconBuf[SETTINGS_TILE_ICON_PX * SETTINGS_TILE_ICON_PX];
static lv_color_t s_settingsIconBuf[SETTINGS_TILE_ICON_PX * SETTINGS_TILE_ICON_PX];
static int s_focusIndex = 0;

static AppStrId tile_str_id(int index) {
  static const AppStrId kIds[4] = {
    TR_TILE_WEATHER,
    TR_TILE_APP2,
    TR_TILE_APP3,
    TR_TILE_SETTINGS,
  };
  if (index < 0 || index > 3) {
    return TR_APP;
  }
  return kIds[index];
}

static void style_tile(lv_obj_t *btn, lv_obj_t *label, bool focused) {
  /* 墨水屏不用反色大块填充（慢且发灰）；用白底 + 粗边框表示焦点 */
  lv_obj_set_style_bg_color(btn, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_border_color(btn, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_border_width(btn, focused ? 3 : 1, LV_PART_MAIN);
  lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
}

static void style_tile_text(lv_obj_t *label) {
  lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_text_opa(label, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(label, 0, LV_PART_MAIN);
  lv_obj_set_style_text_font(label, UI_FONT_SM, LV_PART_MAIN);
}

static void canvas_set_weather_icon(lv_obj_t *canvas, WeatherIconKind kind) {
  if ((unsigned)kind >= WEATHER_ICON_COUNT) {
    kind = WEATHER_ICON_CLOUDY;
  }

  for (int row = 0; row < WEATHER_TILE_ICON_PX; row++) {
    const int srcRow = (row * WEATHER_ICON_SIZE) / WEATHER_TILE_ICON_PX;
    const uint16_t mask = weather_icon_bitmaps[kind][srcRow];
    for (int col = 0; col < WEATHER_TILE_ICON_PX; col++) {
      const int srcCol = (col * WEATHER_ICON_SIZE) / WEATHER_TILE_ICON_PX;
      const bool black = (mask >> (WEATHER_ICON_SIZE - 1 - srcCol)) & 0x01;
      lv_canvas_set_px(canvas, col, row,
                       black ? lv_color_black() : lv_color_white());
    }
  }
}

static void canvas_set_bitmap_icon(lv_obj_t *canvas, int size, const uint32_t *bitmap) {
  for (int row = 0; row < size; row++) {
    const uint32_t mask = bitmap[row];
    for (int col = 0; col < size; col++) {
      const bool black = (mask >> (SETTINGS_ICON_SIZE - 1 - col)) & 0x01;
      lv_canvas_set_px(canvas, col, row,
                       black ? lv_color_black() : lv_color_white());
    }
  }
}

static void canvas_set_settings_icon(lv_obj_t *canvas) {
  canvas_set_bitmap_icon(canvas, SETTINGS_TILE_ICON_PX, settings_gear_bitmap);
}

static void canvas_set_stock_icon(lv_obj_t *canvas) {
  canvas_set_bitmap_icon(canvas, SETTINGS_TILE_ICON_PX, stock_chart_bitmap);
}

static void format_stock_change(int changePctX10, char *out, size_t outLen) {
  if (changePctX10 == 0) {
    snprintf(out, outLen, "0%%");
    return;
  }
  const char sign = changePctX10 > 0 ? '+' : '-';
  const int absVal = abs(changePctX10);
  const int whole = absVal / 10;
  const int frac = absVal % 10;
  if (frac == 0) {
    snprintf(out, outLen, "%c%d%%", sign, whole);
  } else {
    snprintf(out, outLen, "%c%d.%d%%", sign, whole, frac);
  }
}

static void bind_stock_tile(void) {
  if (s_stockPreviewLabel == nullptr) {
    return;
  }

  const StockQuote *preview = stock_service_get_tile_preview();
  char line[20];
  if (preview == nullptr) {
    snprintf(line, sizeof(line), "--");
  } else {
    char change[12];
    char label[STOCK_NAME_LEN];
    format_stock_change(preview->changePctX10, change, sizeof(change));
    stock_service_format_display_label(preview, label, sizeof(label));
    snprintf(line, sizeof(line), "%s %s", label, change);
  }
  lv_label_set_text(s_stockPreviewLabel, line);
  if (s_stockIconCanvas != nullptr) {
    canvas_set_stock_icon(s_stockIconCanvas);
  }
}

static void canvas_set_vision_icon(lv_obj_t *canvas) {
  canvas_set_bitmap_icon(canvas, SETTINGS_TILE_ICON_PX, vision_eye_bitmap);
}

static void bind_weather_tile(void) {
  if (s_weatherIconCanvas == nullptr || s_weatherTempLabel == nullptr) {
    return;
  }

  WeatherSnapshot snap = {};
  weather_service_get_snapshot(&snap);

  char tempLine[8];
  if (snap.valid) {
    canvas_set_weather_icon(s_weatherIconCanvas, snap.icon);
    snprintf(tempLine, sizeof(tempLine), "%dC", snap.tempC);
  } else {
    canvas_set_weather_icon(s_weatherIconCanvas, WEATHER_ICON_CLOUDY);
    snprintf(tempLine, sizeof(tempLine), "--");
  }
  lv_label_set_text(s_weatherTempLabel, tempLine);
}

static void bind_vision_tile(void) {
  if (s_visionIconCanvas == nullptr || s_visionHintLabel == nullptr) {
    return;
  }

  canvas_set_vision_icon(s_visionIconCanvas);
  lv_label_set_text(s_visionHintLabel, app_tr(TR_VISION_HINT));
}

static void bind_settings_tile(void) {
  if (s_settingsIconCanvas == nullptr || s_settingsStatusLabel == nullptr) {
    return;
  }

  canvas_set_settings_icon(s_settingsIconCanvas);
  lv_label_set_text(s_settingsStatusLabel,
                     settings_api_is_wifi_connected() ? app_tr(TR_ONLINE) : app_tr(TR_OFFLINE));
}

static void refresh_tile_titles(void) {
  for (int i = 0; i < 4; i++) {
    lv_label_set_text(s_tileLabels[i], app_tr(tile_str_id(i)));
    lv_obj_invalidate(s_tileLabels[i]);
  }
}

static void invalidate_tile(int index) {
  if (index >= 0 && index < 4) {
    lv_obj_invalidate(s_tiles[index]);
  }
}

static void update_focus_styles(void) {
  for (int i = 0; i < 4; i++) {
    style_tile(s_tiles[i], s_tileLabels[i], i == s_focusIndex);
  }
}

void ui_home_next_focus(int *outPrevFocus) {
  const int prev = s_focusIndex;
  s_focusIndex = (s_focusIndex + 1) % 4;
  update_focus_styles();
  invalidate_tile(prev);
  invalidate_tile(s_focusIndex);
  if (outPrevFocus != nullptr) {
    *outPrevFocus = prev;
  }
}

void ui_home_prev_focus(int *outPrevFocus) {
  const int prev = s_focusIndex;
  s_focusIndex = (s_focusIndex + 3) % 4;
  update_focus_styles();
  invalidate_tile(prev);
  invalidate_tile(s_focusIndex);
  if (outPrevFocus != nullptr) {
    *outPrevFocus = prev;
  }
}

void ui_home_init(void) {
  s_screenHome = lv_obj_create(nullptr);
  lv_obj_set_size(s_screenHome, 200, 180);
  lv_obj_set_style_bg_color(s_screenHome, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_screenHome, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_screenHome, LV_OBJ_FLAG_SCROLLABLE);

  for (int i = 0; i < 4; i++) {
    s_tiles[i] = lv_btn_create(s_screenHome);
    lv_obj_set_size(s_tiles[i], 98, 88);
    lv_obj_set_pos(s_tiles[i], (i % 2) * 100, (i / 2) * 90);
    lv_obj_clear_flag(s_tiles[i], LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_shadow_width(s_tiles[i], 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(s_tiles[i], 0, LV_PART_MAIN);

    if (i == 0) {
      s_weatherIconCanvas = lv_canvas_create(s_tiles[i]);
      lv_canvas_set_buffer(s_weatherIconCanvas, s_weatherIconBuf,
                           WEATHER_TILE_ICON_PX, WEATHER_TILE_ICON_PX,
                           LV_IMG_CF_TRUE_COLOR);
      lv_obj_align(s_weatherIconCanvas, LV_ALIGN_TOP_MID, 0, 6);

      s_weatherTempLabel = lv_label_create(s_tiles[i]);
      style_tile_text(s_weatherTempLabel);
      lv_label_set_text(s_weatherTempLabel, "--");
      lv_obj_align(s_weatherTempLabel, LV_ALIGN_TOP_MID, 0, 40);

      s_tileLabels[i] = lv_label_create(s_tiles[i]);
      lv_label_set_text(s_tileLabels[i], app_tr(tile_str_id(i)));
      style_tile_text(s_tileLabels[i]);
      lv_obj_align(s_tileLabels[i], LV_ALIGN_TOP_MID, 0, 58);
      bind_weather_tile();
    } else if (i == 1) {
      s_visionIconCanvas = lv_canvas_create(s_tiles[i]);
      lv_canvas_set_buffer(s_visionIconCanvas, s_visionIconBuf,
                           SETTINGS_TILE_ICON_PX, SETTINGS_TILE_ICON_PX,
                           LV_IMG_CF_TRUE_COLOR);
      lv_obj_align(s_visionIconCanvas, LV_ALIGN_TOP_MID, 0, 6);

      s_visionHintLabel = lv_label_create(s_tiles[i]);
      style_tile_text(s_visionHintLabel);
      lv_label_set_text(s_visionHintLabel, app_tr(TR_VISION_HINT));
      lv_obj_align(s_visionHintLabel, LV_ALIGN_TOP_MID, 0, 40);

      s_tileLabels[i] = lv_label_create(s_tiles[i]);
      lv_label_set_text(s_tileLabels[i], app_tr(tile_str_id(i)));
      style_tile_text(s_tileLabels[i]);
      lv_obj_align(s_tileLabels[i], LV_ALIGN_TOP_MID, 0, 58);
      bind_vision_tile();
    } else if (i == 2) {
      s_stockIconCanvas = lv_canvas_create(s_tiles[i]);
      lv_canvas_set_buffer(s_stockIconCanvas, s_stockIconBuf,
                           SETTINGS_TILE_ICON_PX, SETTINGS_TILE_ICON_PX,
                           LV_IMG_CF_TRUE_COLOR);
      lv_obj_align(s_stockIconCanvas, LV_ALIGN_TOP_MID, 0, 6);

      s_stockPreviewLabel = lv_label_create(s_tiles[i]);
      style_tile_text(s_stockPreviewLabel);
      lv_label_set_text(s_stockPreviewLabel, "--");
      lv_obj_align(s_stockPreviewLabel, LV_ALIGN_TOP_MID, 0, 40);

      s_tileLabels[i] = lv_label_create(s_tiles[i]);
      lv_label_set_text(s_tileLabels[i], app_tr(tile_str_id(i)));
      style_tile_text(s_tileLabels[i]);
      lv_obj_align(s_tileLabels[i], LV_ALIGN_TOP_MID, 0, 58);
      bind_stock_tile();
    } else if (i == 3) {
      s_settingsIconCanvas = lv_canvas_create(s_tiles[i]);
      lv_canvas_set_buffer(s_settingsIconCanvas, s_settingsIconBuf,
                           SETTINGS_TILE_ICON_PX, SETTINGS_TILE_ICON_PX,
                           LV_IMG_CF_TRUE_COLOR);
      lv_obj_align(s_settingsIconCanvas, LV_ALIGN_TOP_MID, 0, 6);

      s_settingsStatusLabel = lv_label_create(s_tiles[i]);
      style_tile_text(s_settingsStatusLabel);
      lv_label_set_text(s_settingsStatusLabel, "--");
      lv_obj_align(s_settingsStatusLabel, LV_ALIGN_TOP_MID, 0, 40);

      s_tileLabels[i] = lv_label_create(s_tiles[i]);
      lv_label_set_text(s_tileLabels[i], app_tr(tile_str_id(i)));
      style_tile_text(s_tileLabels[i]);
      lv_obj_align(s_tileLabels[i], LV_ALIGN_TOP_MID, 0, 58);
      bind_settings_tile();
    }
  }

  s_screenDetail = lv_obj_create(nullptr);
  lv_obj_set_size(s_screenDetail, 200, 180);
  lv_obj_set_style_bg_color(s_screenDetail, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_screenDetail, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_screenDetail, LV_OBJ_FLAG_SCROLLABLE);

  s_detailTitle = lv_label_create(s_screenDetail);
  lv_label_set_text(s_detailTitle, app_tr(TR_DETAIL));
  lv_obj_set_style_text_opa(s_detailTitle, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(s_detailTitle, 0, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_detailTitle, UI_FONT_MD, LV_PART_MAIN);
  lv_obj_align(s_detailTitle, LV_ALIGN_TOP_MID, 0, 24);

  s_detailBody = lv_label_create(s_screenDetail);
  lv_label_set_text(s_detailBody, app_tr(TR_COMING_SOON));
  lv_obj_set_style_text_opa(s_detailBody, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(s_detailBody, 0, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_detailBody, UI_FONT_SM, LV_PART_MAIN);
  lv_obj_set_width(s_detailBody, 180);
  lv_label_set_long_mode(s_detailBody, LV_LABEL_LONG_WRAP);
  lv_obj_align(s_detailBody, LV_ALIGN_CENTER, 0, 10);

  s_focusIndex = 0;
  update_focus_styles();
}

void ui_home_show(void) {
  bind_weather_tile();
  bind_vision_tile();
  bind_stock_tile();
  bind_settings_tile();
  lv_scr_load(s_screenHome);
  lv_obj_invalidate(s_screenHome);
}

void ui_home_refresh_weather(void) {
  if (s_screenHome == nullptr || lv_scr_act() != s_screenHome) {
    return;
  }
  bind_weather_tile();
  invalidate_tile(0);
}

void ui_home_refresh_stocks(void) {
  if (s_screenHome == nullptr || lv_scr_act() != s_screenHome) {
    return;
  }
  bind_stock_tile();
  invalidate_tile(2);
}

int ui_home_get_focus(void) {
  return s_focusIndex;
}

const char *ui_home_focus_title(void) {
  if (s_focusIndex < 0 || s_focusIndex > 3) {
    return app_tr(TR_APP);
  }
  return app_tr(tile_str_id(s_focusIndex));
}

void ui_home_refresh_locale(void) {
  if (s_screenHome == nullptr) {
    return;
  }
  refresh_tile_titles();
  bind_vision_tile();
  bind_stock_tile();
  bind_settings_tile();
  lv_label_set_text(s_detailTitle, app_tr(TR_DETAIL));
  lv_label_set_text(s_detailBody, app_tr(TR_COMING_SOON));
  lv_obj_invalidate(s_screenHome);
  lv_obj_invalidate(s_screenDetail);
}

void ui_detail_show(const char *title, const char *body) {
  lv_label_set_text(s_detailTitle, title);
  lv_label_set_text(s_detailBody, body);
  lv_scr_load(s_screenDetail);
  lv_obj_invalidate(s_screenDetail);
}

lv_obj_t *ui_home_get_screen(void) {
  return s_screenHome;
}

lv_obj_t *ui_detail_get_screen(void) {
  return s_screenDetail;
}
