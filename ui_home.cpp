#include "ui_home.h"

#include "app_locale.h"
#include "clock_format.h"
#include "weather_icons.h"
#include "weather_service.h"
#include "stock_service.h"
#include "settings_icons.h"
#include "settings_api.h"
#include "ui_fonts.h"

#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define TILE_ICON_PX         32
#define HOME_SLOTS_PER_PAGE  4
#define HOME_LOGICAL_COUNT   6

static lv_obj_t *s_screenHome = nullptr;
static lv_obj_t *s_screenDetail = nullptr;
static lv_obj_t *s_tiles[HOME_SLOTS_PER_PAGE];
static lv_obj_t *s_tileLabels[HOME_SLOTS_PER_PAGE];
static lv_obj_t *s_iconCanvas[HOME_SLOTS_PER_PAGE];
static lv_obj_t *s_subLabels[HOME_SLOTS_PER_PAGE];
static lv_obj_t *s_detailTitle = nullptr;
static lv_obj_t *s_detailBody = nullptr;
static lv_color_t s_iconBuf[HOME_SLOTS_PER_PAGE][TILE_ICON_PX * TILE_ICON_PX];
static int s_logicalFocus = 0;
static int s_homePage = 0;

static AppStrId logical_tile_str_id(int logicalIndex) {
  static const AppStrId kIds[HOME_LOGICAL_COUNT] = {
    TR_TILE_CLOCK,
    TR_TILE_WEATHER,
    TR_TILE_APP2,
    TR_TILE_APP3,
    TR_TILE_SETTINGS,
    TR_TILE_ANSWERBOOK,
  };
  if (logicalIndex < 0 || logicalIndex >= HOME_LOGICAL_COUNT) {
    return TR_APP;
  }
  return kIds[logicalIndex];
}

static void style_tile(lv_obj_t *btn, lv_obj_t *label, bool focused) {
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

  for (int row = 0; row < TILE_ICON_PX; row++) {
    const int srcRow = (row * WEATHER_ICON_SIZE) / TILE_ICON_PX;
    const uint16_t mask = weather_icon_bitmaps[kind][srcRow];
    for (int col = 0; col < TILE_ICON_PX; col++) {
      const int srcCol = (col * WEATHER_ICON_SIZE) / TILE_ICON_PX;
      const bool black = (mask >> (WEATHER_ICON_SIZE - 1 - srcCol)) & 0x01;
      lv_canvas_set_px(canvas, col, row,
                       black ? lv_color_black() : lv_color_white());
    }
  }
}

static void canvas_set_bitmap_icon(lv_obj_t *canvas, const uint32_t *bitmap) {
  for (int row = 0; row < TILE_ICON_PX; row++) {
    const uint32_t mask = bitmap[row];
    for (int col = 0; col < TILE_ICON_PX; col++) {
      const bool black = (mask >> (SETTINGS_ICON_SIZE - 1 - col)) & 0x01;
      lv_canvas_set_px(canvas, col, row,
                       black ? lv_color_black() : lv_color_white());
    }
  }
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

static void bind_clock_slot(int slot) {
  canvas_set_bitmap_icon(s_iconCanvas[slot], clock_face_bitmap);

  struct tm timeinfo;
  char timeLine[16] = "--:--";
  if (getLocalTime(&timeinfo, 0)) {
    clock_format_hm(timeLine, sizeof(timeLine), &timeinfo, settings_api_clock_use_24h());
  }
  lv_label_set_text(s_subLabels[slot], timeLine);
}

static void bind_weather_slot(int slot) {
  WeatherSnapshot snap = {};
  weather_service_get_snapshot(&snap);

  char tempLine[8];
  if (snap.valid) {
    canvas_set_weather_icon(s_iconCanvas[slot], snap.icon);
    snprintf(tempLine, sizeof(tempLine), "%dC", snap.tempC);
  } else {
    canvas_set_weather_icon(s_iconCanvas[slot], WEATHER_ICON_CLOUDY);
    snprintf(tempLine, sizeof(tempLine), "--");
  }
  lv_label_set_text(s_subLabels[slot], tempLine);
}

static void bind_vision_slot(int slot) {
  canvas_set_bitmap_icon(s_iconCanvas[slot], vision_eye_bitmap);
  lv_label_set_text(s_subLabels[slot], app_tr(TR_VISION_HINT));
}

static void bind_stock_slot(int slot) {
  canvas_set_bitmap_icon(s_iconCanvas[slot], stock_chart_bitmap);

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
  lv_label_set_text(s_subLabels[slot], line);
}

static void bind_settings_slot(int slot) {
  canvas_set_bitmap_icon(s_iconCanvas[slot], settings_gear_bitmap);
  lv_label_set_text(s_subLabels[slot],
                     settings_api_is_wifi_connected() ? app_tr(TR_ONLINE) : app_tr(TR_OFFLINE));
}

static void bind_answerbook_slot(int slot) {
  canvas_set_bitmap_icon(s_iconCanvas[slot], answerbook_bitmap);
  lv_label_set_text(s_subLabels[slot], app_tr(TR_ANSWERBOOK_HINT));
}

static void bind_slot_content(int slot, int logicalIndex) {
  if (logicalIndex < 0 || logicalIndex >= HOME_LOGICAL_COUNT) {
    lv_obj_add_flag(s_tiles[slot], LV_OBJ_FLAG_HIDDEN);
    return;
  }

  lv_obj_clear_flag(s_tiles[slot], LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(s_tileLabels[slot], app_tr(logical_tile_str_id(logicalIndex)));

  switch (logicalIndex) {
    case 0:
      bind_clock_slot(slot);
      break;
    case 1:
      bind_weather_slot(slot);
      break;
    case 2:
      bind_vision_slot(slot);
      break;
    case 3:
      bind_stock_slot(slot);
      break;
    case 4:
      bind_settings_slot(slot);
      break;
    case 5:
      bind_answerbook_slot(slot);
      break;
    default:
      break;
  }
}

static void sync_home_page(void) {
  const int page = s_logicalFocus / HOME_SLOTS_PER_PAGE;
  if (page != s_homePage) {
    s_homePage = page;
  }

  for (int slot = 0; slot < HOME_SLOTS_PER_PAGE; slot++) {
    const int logicalIndex = s_homePage * HOME_SLOTS_PER_PAGE + slot;
    bind_slot_content(slot, logicalIndex);
    style_tile(s_tiles[slot], s_tileLabels[slot], logicalIndex == s_logicalFocus);
  }
}

static void invalidate_focus_tile(void) {
  const int slot = s_logicalFocus % HOME_SLOTS_PER_PAGE;
  if (s_logicalFocus / HOME_SLOTS_PER_PAGE == s_homePage) {
    lv_obj_invalidate(s_tiles[slot]);
  }
}

void ui_home_next_focus(int *outPrevFocus) {
  const int prev = s_logicalFocus;
  s_logicalFocus = (s_logicalFocus + 1) % HOME_LOGICAL_COUNT;
  sync_home_page();
  if (outPrevFocus != nullptr) {
    *outPrevFocus = prev;
  }
  invalidate_focus_tile();
  if (prev / HOME_SLOTS_PER_PAGE == s_homePage) {
    lv_obj_invalidate(s_tiles[prev % HOME_SLOTS_PER_PAGE]);
  }
}

void ui_home_prev_focus(int *outPrevFocus) {
  const int prev = s_logicalFocus;
  s_logicalFocus = (s_logicalFocus + HOME_LOGICAL_COUNT - 1) % HOME_LOGICAL_COUNT;
  sync_home_page();
  if (outPrevFocus != nullptr) {
    *outPrevFocus = prev;
  }
  invalidate_focus_tile();
  if (prev / HOME_SLOTS_PER_PAGE == s_homePage) {
    lv_obj_invalidate(s_tiles[prev % HOME_SLOTS_PER_PAGE]);
  }
}

void ui_home_init(void) {
  s_screenHome = lv_obj_create(nullptr);
  lv_obj_set_size(s_screenHome, 200, 180);
  lv_obj_set_style_bg_color(s_screenHome, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_screenHome, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_screenHome, LV_OBJ_FLAG_SCROLLABLE);

  for (int i = 0; i < HOME_SLOTS_PER_PAGE; i++) {
    s_tiles[i] = lv_btn_create(s_screenHome);
    lv_obj_set_size(s_tiles[i], 98, 88);
    lv_obj_set_pos(s_tiles[i], (i % 2) * 100, (i / 2) * 90);
    lv_obj_clear_flag(s_tiles[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_shadow_width(s_tiles[i], 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(s_tiles[i], 0, LV_PART_MAIN);

    s_iconCanvas[i] = lv_canvas_create(s_tiles[i]);
    lv_canvas_set_buffer(s_iconCanvas[i], s_iconBuf[i], TILE_ICON_PX, TILE_ICON_PX,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(s_iconCanvas[i], LV_ALIGN_TOP_MID, 0, 6);

    s_subLabels[i] = lv_label_create(s_tiles[i]);
    style_tile_text(s_subLabels[i]);
    lv_label_set_text(s_subLabels[i], "--");
    lv_obj_align(s_subLabels[i], LV_ALIGN_TOP_MID, 0, 40);

    s_tileLabels[i] = lv_label_create(s_tiles[i]);
    style_tile_text(s_tileLabels[i]);
    lv_label_set_text(s_tileLabels[i], app_tr(TR_APP));
    lv_obj_align(s_tileLabels[i], LV_ALIGN_TOP_MID, 0, 58);
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

  s_logicalFocus = 0;
  s_homePage = 0;
  sync_home_page();
}

void ui_home_show(void) {
  sync_home_page();
  lv_scr_load(s_screenHome);
  lv_obj_invalidate(s_screenHome);
}

void ui_home_refresh_weather(void) {
  if (s_screenHome == nullptr || lv_scr_act() != s_screenHome) {
    return;
  }

  for (int slot = 0; slot < HOME_SLOTS_PER_PAGE; slot++) {
    const int logicalIndex = s_homePage * HOME_SLOTS_PER_PAGE + slot;
    if (logicalIndex == 1) {
      bind_weather_slot(slot);
      lv_obj_invalidate(s_tiles[slot]);
    }
  }
}

void ui_home_refresh_clock(void) {
  if (s_screenHome == nullptr || lv_scr_act() != s_screenHome) {
    return;
  }

  for (int slot = 0; slot < HOME_SLOTS_PER_PAGE; slot++) {
    const int logicalIndex = s_homePage * HOME_SLOTS_PER_PAGE + slot;
    if (logicalIndex == 0) {
      bind_clock_slot(slot);
      lv_obj_invalidate(s_tiles[slot]);
    }
  }
}

void ui_home_refresh_stocks(void) {
  if (s_screenHome == nullptr || lv_scr_act() != s_screenHome) {
    return;
  }

  for (int slot = 0; slot < HOME_SLOTS_PER_PAGE; slot++) {
    const int logicalIndex = s_homePage * HOME_SLOTS_PER_PAGE + slot;
    if (logicalIndex == 3) {
      bind_stock_slot(slot);
      lv_obj_invalidate(s_tiles[slot]);
    }
  }
}

void ui_home_refresh_answerbook(void) {
  if (s_screenHome == nullptr || lv_scr_act() != s_screenHome) {
    return;
  }

  for (int slot = 0; slot < HOME_SLOTS_PER_PAGE; slot++) {
    const int logicalIndex = s_homePage * HOME_SLOTS_PER_PAGE + slot;
    if (logicalIndex == 5) {
      bind_answerbook_slot(slot);
      lv_obj_invalidate(s_tiles[slot]);
    }
  }
}

int ui_home_get_focus(void) {
  return s_logicalFocus;
}

const char *ui_home_focus_title(void) {
  if (s_logicalFocus < 0 || s_logicalFocus >= HOME_LOGICAL_COUNT) {
    return app_tr(TR_APP);
  }
  return app_tr(logical_tile_str_id(s_logicalFocus));
}

void ui_home_refresh_locale(void) {
  if (s_screenHome == nullptr) {
    return;
  }
  sync_home_page();
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
