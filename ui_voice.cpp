#include "ui_voice.h"

#include "app_locale.h"
#include "ui_fonts.h"
#include "ui_lvgl.h"
#include "voice_service.h"

#include <Arduino.h>

static lv_obj_t *s_screenVoice = nullptr;
static lv_obj_t *s_titleLabel = nullptr;
static lv_obj_t *s_bodyLabel = nullptr;

static void style_label(lv_obj_t *label, const lv_font_t *font) {
  lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_text_opa(label, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(label, 0, LV_PART_MAIN);
  if (font != nullptr) {
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  }
}

static const char *voice_title(void) {
  return app_locale_get() == APP_LANG_ZH ? "语音助手" : "Voice";
}

void ui_voice_init(void) {
  s_screenVoice = lv_obj_create(nullptr);
  ui_lvgl_configure_screen(s_screenVoice);
  lv_obj_set_style_bg_color(s_screenVoice, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_screenVoice, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_screenVoice, LV_OBJ_FLAG_SCROLLABLE);

  s_titleLabel = lv_label_create(s_screenVoice);
  style_label(s_titleLabel, UI_FONT_MD);
  lv_obj_align(s_titleLabel, LV_ALIGN_TOP_MID, 0, 12);

  s_bodyLabel = lv_label_create(s_screenVoice);
  style_label(s_bodyLabel, UI_FONT_SM);
  lv_obj_set_width(s_bodyLabel, 180);
  lv_obj_set_height(s_bodyLabel, 122);
  lv_label_set_long_mode(s_bodyLabel, LV_LABEL_LONG_WRAP);
  lv_obj_align(s_bodyLabel, LV_ALIGN_TOP_MID, 0, 44);

  ui_voice_refresh();
}

void ui_voice_show(void) {
  ui_voice_refresh();
  lv_scr_load(s_screenVoice);
  lv_obj_invalidate(s_screenVoice);
}

void ui_voice_refresh(void) {
  if (s_screenVoice == nullptr) {
    return;
  }
  char text[384];
  voice_service_status_text(text, sizeof(text));
  lv_label_set_text(s_titleLabel, voice_title());
  lv_label_set_text(s_bodyLabel, text);
  lv_obj_invalidate(s_screenVoice);
}

bool ui_voice_is_active(void) {
  return s_screenVoice != nullptr && lv_scr_act() == s_screenVoice;
}

bool ui_voice_service(UiRefreshMode *outRefreshMode) {
  if (outRefreshMode != nullptr) {
    *outRefreshMode = UI_REFRESH_NONE;
  }
  const bool changed = voice_service_service();
  if (!changed || !ui_voice_is_active()) {
    return false;
  }
  ui_voice_refresh();
  if (outRefreshMode != nullptr) {
    *outRefreshMode = UI_REFRESH_NAV;
  }
  return true;
}

lv_obj_t *ui_voice_get_screen(void) {
  return s_screenVoice;
}
