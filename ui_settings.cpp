#include "ui_settings.h"

#include "ai_model_config.h"
#include "app_locale.h"
#include "settings_api.h"
#include "ui_fonts.h"
#include "weather_service.h"
#include "stock_service.h"

#include <stdio.h>
#include <string.h>

#define SETTINGS_MAX_ROWS  7
#define SETTINGS_ROW_Y0    34
#define SETTINGS_ROW_STEP  20

enum SettingsPage {
  SETTINGS_PAGE_ROOT = 0,
  SETTINGS_PAGE_WIFI,
  SETTINGS_PAGE_MODEL,
  SETTINGS_PAGE_MODEL_PROVIDER,
  SETTINGS_PAGE_MODEL_PRESET,
  SETTINGS_PAGE_STOCKS,
  SETTINGS_PAGE_DISPLAY,
  SETTINGS_PAGE_ABOUT,
};

static lv_obj_t *s_screenSettings = nullptr;
static lv_obj_t *s_titleLabel = nullptr;
static lv_obj_t *s_rowLabels[SETTINGS_MAX_ROWS];
static SettingsPage s_page = SETTINGS_PAGE_ROOT;
static int s_focusRow = 0;
static int s_rowCount = 0;

static void style_label(lv_obj_t *label) {
  lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_text_opa(label, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(label, 0, LV_PART_MAIN);
  lv_obj_set_style_text_font(label, UI_FONT_SM, LV_PART_MAIN);
}

static SettingsPage settings_page_parent(SettingsPage page) {
  switch (page) {
    case SETTINGS_PAGE_MODEL_PROVIDER:
    case SETTINGS_PAGE_MODEL_PRESET:
      return SETTINGS_PAGE_MODEL;
    case SETTINGS_PAGE_WIFI:
    case SETTINGS_PAGE_MODEL:
    case SETTINGS_PAGE_STOCKS:
    case SETTINGS_PAGE_DISPLAY:
    case SETTINGS_PAGE_ABOUT:
      return SETTINGS_PAGE_ROOT;
    default:
      return SETTINGS_PAGE_ROOT;
  }
}

static int page_row_count(SettingsPage page) {
  switch (page) {
    case SETTINGS_PAGE_ROOT:
      return 5;
    case SETTINGS_PAGE_WIFI:
      return 7;
    case SETTINGS_PAGE_STOCKS:
      return 3;
    case SETTINGS_PAGE_MODEL:
      return 5;
    case SETTINGS_PAGE_MODEL_PROVIDER:
      return AI_PROVIDER_COUNT;
    case SETTINGS_PAGE_MODEL_PRESET:
      return ai_provider_model_count(settings_api_get_provider());
    case SETTINGS_PAGE_DISPLAY:
      return 3;
    case SETTINGS_PAGE_ABOUT:
      return 2;
    default:
      return 0;
  }
}

static const char *page_title(SettingsPage page) {
  switch (page) {
    case SETTINGS_PAGE_ROOT:
      return app_tr(TR_SETTINGS);
    case SETTINGS_PAGE_WIFI:
      return app_tr(TR_WIFI);
    case SETTINGS_PAGE_MODEL:
      return app_tr(TR_MODEL);
    case SETTINGS_PAGE_MODEL_PROVIDER:
      return app_tr(TR_PROVIDER_LABEL);
    case SETTINGS_PAGE_MODEL_PRESET:
      return app_tr(TR_MODEL_LABEL);
    case SETTINGS_PAGE_STOCKS:
      return app_tr(TR_STOCK_TITLE);
    case SETTINGS_PAGE_DISPLAY:
      return app_tr(TR_DISPLAY);
    case SETTINGS_PAGE_ABOUT:
      return app_tr(TR_ABOUT);
    default:
      return app_tr(TR_SETTINGS);
  }
}

static const char *language_value_label(void) {
  return app_locale_get() == APP_LANG_EN ? app_tr(TR_LANG_ENGLISH) : app_tr(TR_LANG_CHINESE);
}

static bool page_row_is_action(SettingsPage page, int row) {
  if (page == SETTINGS_PAGE_WIFI) {
    return row >= 3 && row <= 6;
  }
  if (page == SETTINGS_PAGE_MODEL) {
    return row == 3 || row == 4;
  }
  if (page == SETTINGS_PAGE_STOCKS) {
    return row >= 1;
  }
  if (page == SETTINGS_PAGE_DISPLAY && row == 2) {
    return true;
  }
  return false;
}

static bool page_row_is_category(SettingsPage page, int row) {
  if (page == SETTINGS_PAGE_ROOT) {
    return true;
  }
  if (page == SETTINGS_PAGE_MODEL && (row == 0 || row == 1)) {
    return true;
  }
  return false;
}

static void build_row_text(SettingsPage page, int row, char *out, size_t outLen) {
  char wifiSsid[24] = {};
  char ipLine[20] = {};

  switch (page) {
    case SETTINGS_PAGE_ROOT:
      switch (row) {
        case 0:
          snprintf(out, outLen, "%s", app_tr(TR_WIFI));
          break;
        case 1:
          snprintf(out, outLen, "%s", app_tr(TR_MODEL));
          break;
        case 2:
          snprintf(out, outLen, "%s", app_tr(TR_STOCK_TITLE));
          break;
        case 3:
          snprintf(out, outLen, "%s", app_tr(TR_DISPLAY));
          break;
        case 4:
          snprintf(out, outLen, "%s", app_tr(TR_ABOUT));
          break;
        default:
          snprintf(out, outLen, "");
          break;
      }
      break;
    case SETTINGS_PAGE_WIFI:
      switch (row) {
        case 0:
          settings_api_get_wifi_ssid(wifiSsid, sizeof(wifiSsid));
          if (wifiSsid[0] == '\0') {
            snprintf(out, outLen, "%s: --", app_tr(TR_NETWORK));
          } else {
            snprintf(out, outLen, "%s: %s", app_tr(TR_NETWORK), wifiSsid);
          }
          break;
        case 1:
          settings_api_get_ip(ipLine, sizeof(ipLine));
          snprintf(out, outLen, "IP: %s", ipLine);
          break;
        case 2:
          snprintf(out, outLen, "%s",
                   settings_api_has_weather_api() ? app_tr(TR_WEATHER_API_OK)
                                                  : app_tr(TR_WEATHER_API_MISSING));
          break;
        case 3:
          snprintf(out, outLen, "%s", app_tr(TR_CONFIGURE_WEATHER_API));
          break;
        case 4:
          snprintf(out, outLen, "%s", app_tr(TR_CLEAR_WEATHER_API));
          break;
        case 5:
          snprintf(out, outLen, "%s", app_tr(TR_RECONFIGURE_WIFI));
          break;
        case 6:
          snprintf(out, outLen, "%s", app_tr(TR_FORGET_WIFI));
          break;
        default:
          snprintf(out, outLen, "");
          break;
      }
      break;
    case SETTINGS_PAGE_STOCKS:
      switch (row) {
        case 0:
          snprintf(out, outLen, "%s",
                   settings_api_has_watchlist() ? app_tr(TR_STOCK_CONFIGURED)
                                                : app_tr(TR_STOCK_DEFAULT));
          break;
        case 1:
          snprintf(out, outLen, "%s", app_tr(TR_CONFIGURE_STOCK));
          break;
        case 2:
          snprintf(out, outLen, "%s", app_tr(TR_CLEAR_STOCK));
          break;
        default:
          snprintf(out, outLen, "");
          break;
      }
      break;
    case SETTINGS_PAGE_MODEL:
      switch (row) {
        case 0:
          snprintf(out, outLen, "%s: %s", app_tr(TR_PROVIDER_LABEL),
                   ai_provider_name(settings_api_get_provider()));
          break;
        case 1:
          snprintf(out, outLen, "%s: %s", app_tr(TR_MODEL_LABEL),
                   settings_api_get_model_label());
          break;
        case 2:
          snprintf(out, outLen, "%s",
                   settings_api_has_api_key() ? app_tr(TR_API_OK) : app_tr(TR_API_MISSING));
          break;
        case 3:
          snprintf(out, outLen, "%s", app_tr(TR_CONFIGURE_API));
          break;
        case 4:
          snprintf(out, outLen, "%s", app_tr(TR_CLEAR_API));
          break;
        default:
          snprintf(out, outLen, "");
          break;
      }
      break;
    case SETTINGS_PAGE_MODEL_PROVIDER:
      if (row >= 0 && row < AI_PROVIDER_COUNT) {
        snprintf(out, outLen, "%s", ai_provider_name((AiProvider)row));
      } else {
        snprintf(out, outLen, "");
      }
      break;
    case SETTINGS_PAGE_MODEL_PRESET: {
      const AiProvider provider = settings_api_get_provider();
      if (row >= 0 && row < ai_provider_model_count(provider)) {
        snprintf(out, outLen, "%s", ai_provider_model_label(provider, row));
      } else {
        snprintf(out, outLen, "");
      }
      break;
    }
    case SETTINGS_PAGE_DISPLAY:
      switch (row) {
        case 0:
          snprintf(out, outLen, "%s", app_tr(TR_PANEL));
          break;
        case 1:
          snprintf(out, outLen, "%s", app_tr(TR_REFRESH_AUTO));
          break;
        case 2:
          snprintf(out, outLen, "%s %s", app_tr(TR_LANGUAGE), language_value_label());
          break;
        default:
          snprintf(out, outLen, "");
          break;
      }
      break;
    case SETTINGS_PAGE_ABOUT:
      if (row == 0) {
        snprintf(out, outLen, "Aink");
      } else {
        snprintf(out, outLen, "%s", app_tr(TR_VERSION));
      }
      break;
    default:
      snprintf(out, outLen, "");
      break;
  }
}

static void sync_focus_for_page(SettingsPage page) {
  if (page == SETTINGS_PAGE_MODEL_PROVIDER) {
    s_focusRow = (int)settings_api_get_provider();
  } else if (page == SETTINGS_PAGE_MODEL_PRESET) {
    s_focusRow = settings_api_get_model_index();
  } else {
    s_focusRow = 0;
  }
}

static void enter_page(SettingsPage page) {
  s_page = page;
  s_rowCount = page_row_count(page);
  sync_focus_for_page(page);
  if (s_rowCount > 0 && s_focusRow >= s_rowCount) {
    s_focusRow = 0;
  }
}

static void update_menu_view(void) {
  char line[44];
  char prefixed[48];

  lv_label_set_text(s_titleLabel, page_title(s_page));

  for (int i = 0; i < SETTINGS_MAX_ROWS; i++) {
    if (i < s_rowCount) {
      build_row_text(s_page, i, line, sizeof(line));
      if (i == s_focusRow) {
        if (page_row_is_category(s_page, i)) {
          snprintf(prefixed, sizeof(prefixed), "> %s >", line);
        } else {
          snprintf(prefixed, sizeof(prefixed), "> %s", line);
        }
        lv_label_set_text(s_rowLabels[i], prefixed);
      } else if (page_row_is_category(s_page, i)) {
        snprintf(prefixed, sizeof(prefixed), "%s >", line);
        lv_label_set_text(s_rowLabels[i], prefixed);
      } else {
        lv_label_set_text(s_rowLabels[i], line);
      }
      lv_obj_clear_flag(s_rowLabels[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_invalidate(s_rowLabels[i]);
    } else {
      lv_obj_add_flag(s_rowLabels[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
  lv_obj_invalidate(s_screenSettings);
}

static void reset_to_root(void) {
  enter_page(SETTINGS_PAGE_ROOT);
  update_menu_view();
}

void ui_settings_init(void) {
  s_screenSettings = lv_obj_create(nullptr);
  lv_obj_set_size(s_screenSettings, 200, 180);
  lv_obj_set_style_bg_color(s_screenSettings, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_screenSettings, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_screenSettings, LV_OBJ_FLAG_SCROLLABLE);

  s_titleLabel = lv_label_create(s_screenSettings);
  style_label(s_titleLabel);
  lv_label_set_text(s_titleLabel, app_tr(TR_SETTINGS));
  lv_obj_align(s_titleLabel, LV_ALIGN_TOP_MID, 0, 8);

  lv_obj_t *divider = lv_obj_create(s_screenSettings);
  lv_obj_set_size(divider, 188, 1);
  lv_obj_set_style_bg_color(divider, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(divider, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(divider, 0, LV_PART_MAIN);
  lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(divider, LV_ALIGN_TOP_MID, 0, 26);

  for (int i = 0; i < SETTINGS_MAX_ROWS; i++) {
    s_rowLabels[i] = lv_label_create(s_screenSettings);
    style_label(s_rowLabels[i]);
    lv_obj_set_width(s_rowLabels[i], 188);
    lv_label_set_long_mode(s_rowLabels[i], LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_rowLabels[i], 6, SETTINGS_ROW_Y0 + (i * SETTINGS_ROW_STEP));
  }

  reset_to_root();
}

void ui_settings_show(void) {
  reset_to_root();
  lv_scr_load(s_screenSettings);
  lv_obj_invalidate(s_screenSettings);
}

void ui_settings_refresh(void) {
  if (!ui_settings_is_active()) {
    return;
  }
  s_rowCount = page_row_count(s_page);
  update_menu_view();
}

void ui_settings_next_row(void) {
  if (s_rowCount <= 0) {
    return;
  }
  s_focusRow = (s_focusRow + 1) % s_rowCount;
  update_menu_view();
}

void ui_settings_prev_row(void) {
  if (s_rowCount <= 0) {
    return;
  }
  s_focusRow = (s_focusRow + s_rowCount - 1) % s_rowCount;
  update_menu_view();
}

bool ui_settings_back(void) {
  if (s_page == SETTINGS_PAGE_ROOT) {
    return false;
  }
  enter_page(settings_page_parent(s_page));
  lv_scr_load(s_screenSettings);
  update_menu_view();
  return true;
}

SettingsActivateResult ui_settings_activate(void) {
  if (s_page == SETTINGS_PAGE_ROOT) {
    switch (s_focusRow) {
      case 0:
        enter_page(SETTINGS_PAGE_WIFI);
        break;
      case 1:
        enter_page(SETTINGS_PAGE_MODEL);
        break;
      case 2:
        enter_page(SETTINGS_PAGE_STOCKS);
        break;
      case 3:
        enter_page(SETTINGS_PAGE_DISPLAY);
        break;
      case 4:
        enter_page(SETTINGS_PAGE_ABOUT);
        break;
      default:
        return SETTINGS_ACT_NONE;
    }
    update_menu_view();
    return SETTINGS_ACT_NONE;
  }

  if (s_page == SETTINGS_PAGE_MODEL) {
    switch (s_focusRow) {
      case 0:
        enter_page(SETTINGS_PAGE_MODEL_PROVIDER);
        update_menu_view();
        return SETTINGS_ACT_NONE;
      case 1:
        enter_page(SETTINGS_PAGE_MODEL_PRESET);
        update_menu_view();
        return SETTINGS_ACT_NONE;
      case 3:
        settings_api_request_portal_restart();
        return SETTINGS_ACT_RESTART;
      case 4:
        if (settings_api_has_api_key()) {
          settings_api_clear_api_key();
          update_menu_view();
        }
        return SETTINGS_ACT_NONE;
      default:
        return SETTINGS_ACT_NONE;
    }
  }

  if (s_page == SETTINGS_PAGE_MODEL_PROVIDER) {
    if (s_focusRow >= 0 && s_focusRow < AI_PROVIDER_COUNT) {
      settings_api_set_provider((AiProvider)s_focusRow);
      enter_page(SETTINGS_PAGE_MODEL);
      update_menu_view();
    }
    return SETTINGS_ACT_NONE;
  }

  if (s_page == SETTINGS_PAGE_MODEL_PRESET) {
    const AiProvider provider = settings_api_get_provider();
    if (s_focusRow >= 0 && s_focusRow < ai_provider_model_count(provider)) {
      settings_api_set_model_index(s_focusRow);
      enter_page(SETTINGS_PAGE_MODEL);
      update_menu_view();
    }
    return SETTINGS_ACT_NONE;
  }

  if (s_page == SETTINGS_PAGE_DISPLAY && s_focusRow == 2) {
    app_locale_toggle();
    return SETTINGS_ACT_LOCALE;
  }

  if (!page_row_is_action(s_page, s_focusRow)) {
    return SETTINGS_ACT_NONE;
  }

  if (s_page == SETTINGS_PAGE_STOCKS) {
    if (s_focusRow == 1) {
      settings_api_request_portal_restart();
      return SETTINGS_ACT_RESTART;
    }
    if (s_focusRow == 2) {
      settings_api_clear_watchlist();
      stock_service_invalidate_name_cache();
      stock_service_reset();
      update_menu_view();
      return SETTINGS_ACT_NONE;
    }
  }

  if (s_page == SETTINGS_PAGE_WIFI) {
    if (s_focusRow == 3) {
      settings_api_request_portal_restart();
      return SETTINGS_ACT_RESTART;
    }
    if (s_focusRow == 4) {
      if (settings_api_has_weather_api()) {
        settings_api_clear_weather_api();
        weather_service_reset();
        update_menu_view();
      }
      return SETTINGS_ACT_NONE;
    }
    if (s_focusRow == 5) {
      settings_api_request_portal_restart();
      return SETTINGS_ACT_RESTART;
    }
    if (s_focusRow == 6) {
      settings_api_forget_wifi_and_restart();
      return SETTINGS_ACT_RESTART;
    }
  }
  return SETTINGS_ACT_NONE;
}

bool ui_settings_is_active(void) {
  return s_screenSettings != nullptr && lv_scr_act() == s_screenSettings;
}

lv_obj_t *ui_settings_get_screen(void) {
  return s_screenSettings;
}
