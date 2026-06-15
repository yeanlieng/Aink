#include "ui_clock.h"

#include "clock_format.h"
#include "settings_api.h"
#include "ui_fonts.h"
#include "ui_home.h"

#include <Arduino.h>
#include <stdio.h>
#include <time.h>

static lv_obj_t *s_screenClock = nullptr;
static lv_obj_t *s_timeLabel = nullptr;
static lv_obj_t *s_dateLabel = nullptr;
static int s_lastRenderedMinute = -1;

static void style_time_label(lv_obj_t *label) {
  lv_obj_set_style_text_font(label, UI_FONT_CLOCK, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_text_opa(label, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(label, 0, LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(label, 2, LV_PART_MAIN);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(label, 196);
}

static void set_time_text(const char *timeLine) {
  lv_label_set_text(s_timeLabel, timeLine);
  lv_obj_invalidate(s_timeLabel);
}

static void update_clock_view(const struct tm *timeinfo) {
  if (timeinfo == nullptr) {
    return;
  }

  char timeLine[16];
  clock_format_hm(timeLine, sizeof(timeLine), timeinfo, settings_api_clock_use_24h());
  set_time_text(timeLine);

  if (settings_api_clock_show_date()) {
    char dateLine[24];
    clock_format_date(dateLine, sizeof(dateLine), timeinfo);
    lv_label_set_text(s_dateLabel, dateLine);
    lv_obj_clear_flag(s_dateLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_dateLabel);
  } else {
    lv_obj_add_flag(s_dateLabel, LV_OBJ_FLAG_HIDDEN);
  }
}

void ui_clock_init(void) {
  s_screenClock = lv_obj_create(nullptr);
  lv_obj_set_size(s_screenClock, 200, 180);
  lv_obj_set_style_bg_color(s_screenClock, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_screenClock, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_screenClock, LV_OBJ_FLAG_SCROLLABLE);

  s_timeLabel = lv_label_create(s_screenClock);
  style_time_label(s_timeLabel);
  lv_label_set_text(s_timeLabel, "--:--");
  lv_obj_align(s_timeLabel, LV_ALIGN_CENTER, 0, -8);

  s_dateLabel = lv_label_create(s_screenClock);
  lv_obj_set_style_text_color(s_dateLabel, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_text_opa(s_dateLabel, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(s_dateLabel, 0, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_dateLabel, UI_FONT_MD, LV_PART_MAIN);
  lv_obj_set_style_text_align(s_dateLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_text(s_dateLabel, "");
  lv_obj_align(s_dateLabel, LV_ALIGN_CENTER, 0, 28);
  lv_obj_add_flag(s_dateLabel, LV_OBJ_FLAG_HIDDEN);

  s_lastRenderedMinute = -1;
}

void ui_clock_show(void) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    update_clock_view(&timeinfo);
    s_lastRenderedMinute = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  } else {
    set_time_text("--:--");
    lv_obj_add_flag(s_dateLabel, LV_OBJ_FLAG_HIDDEN);
    s_lastRenderedMinute = -1;
  }
  lv_scr_load(s_screenClock);
  lv_obj_invalidate(s_screenClock);
}

void ui_clock_refresh(void) {
  if (!ui_clock_is_active()) {
    return;
  }

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) {
    return;
  }
  update_clock_view(&timeinfo);
  s_lastRenderedMinute = timeinfo.tm_hour * 60 + timeinfo.tm_min;
}

void ui_clock_refresh_if_minute(const struct tm *timeinfo) {
  if (!ui_clock_is_active() || timeinfo == nullptr) {
    return;
  }

  const int minuteKey = timeinfo->tm_hour * 60 + timeinfo->tm_min;
  if (minuteKey == s_lastRenderedMinute) {
    return;
  }

  update_clock_view(timeinfo);
  s_lastRenderedMinute = minuteKey;
}

void ui_clock_on_settings_changed(void) {
  s_lastRenderedMinute = -1;
  ui_clock_refresh();
  ui_home_refresh_clock();
}

void ui_clock_refresh_locale(void) {
  if (!ui_clock_is_active()) {
    return;
  }
  ui_clock_refresh();
}

bool ui_clock_is_active(void) {
  return s_screenClock != nullptr && lv_scr_act() == s_screenClock;
}

lv_obj_t *ui_clock_get_screen(void) {
  return s_screenClock;
}
