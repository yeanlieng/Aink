#include "ui_nav.h"

#include "app_locale.h"
#include "ui_answers.h"
#include "ui_home.h"
#include "ui_settings.h"
#include "ui_stock.h"
#include "ui_vision.h"
#include "ui_voice.h"
#include "ui_weather.h"
#include "ui_clock.h"
#include "voice_service.h"

#include <Arduino.h>
#include <stdio.h>

static bool s_onHome = true;

void ui_nav_init(void) {
  s_onHome = true;
  ui_home_show();
}

bool ui_nav_is_home(void) {
  return s_onHome;
}

bool ui_nav_is_weather(void) {
  return !s_onHome && ui_weather_is_active();
}

bool ui_nav_is_settings(void) {
  return !s_onHome && ui_settings_is_active();
}

bool ui_nav_is_vision(void) {
  return !s_onHome && ui_vision_is_active();
}

bool ui_nav_is_answers(void) {
  return !s_onHome && ui_answers_is_active();
}

bool ui_nav_is_stock(void) {
  return !s_onHome && ui_stock_is_active();
}

bool ui_nav_is_clock(void) {
  return !s_onHome && ui_clock_is_active();
}

static void go_home(UiRefreshMode *outRefreshMode) {
  if (ui_vision_is_active()) {
    ui_vision_leave();
  }
  if (ui_answers_is_active()) {
    ui_answers_leave();
  }
  ui_home_show();
  s_onHome = true;
  if (outRefreshMode != nullptr) {
    *outRefreshMode = UI_REFRESH_NAV;
  }
}

static bool open_voice_interaction(UiRefreshMode *outRefreshMode) {
  if (ui_vision_is_active()) {
    ui_vision_leave();
  }
  if (ui_answers_is_active()) {
    ui_answers_leave();
  }
  if (!voice_service_toggle_recording()) {
    Serial.println("[Voice] toggle ignored");
  }
  ui_voice_show();
  s_onHome = false;
  if (outRefreshMode != nullptr) {
    *outRefreshMode = UI_REFRESH_NAV;
  }
  return true;
}

static void open_focused_tile(void) {
  const int focus = ui_home_get_focus();
  if (focus == 0) {
    ui_clock_show();
  } else if (focus == 1) {
    ui_weather_show();
  } else if (focus == 2) {
    ui_vision_show();
  } else if (focus == 3) {
    ui_answers_show();
  } else if (focus == 4) {
    ui_stock_show();
  } else if (focus == 5) {
    ui_settings_show();
  } else {
    ui_detail_show(ui_home_focus_title(), app_tr(TR_COMING_SOON));
  }
  s_onHome = false;
}

bool ui_nav_handle(BtnAction action, UiRefreshMode *outRefreshMode) {
  if (outRefreshMode != nullptr) {
    *outRefreshMode = UI_REFRESH_NONE;
  }
  if (action == BTN_ACTION_NONE) {
    return false;
  }

  if (action == BTN_ACTION_NEXT && voice_service_state() == VOICE_STATE_SPEAKING) {
    if (voice_service_interrupt_speaker()) {
      ui_voice_show();
      s_onHome = false;
      if (outRefreshMode != nullptr) {
        *outRefreshMode = UI_REFRESH_NAV;
      }
      return true;
    }
  }

  if (s_onHome) {
    switch (action) {
      case BTN_ACTION_NEXT:
        ui_home_next_focus(nullptr);
        if (outRefreshMode != nullptr) {
          *outRefreshMode = UI_REFRESH_FAST;
        }
        return true;
      case BTN_ACTION_PREV:
        ui_home_prev_focus(nullptr);
        if (outRefreshMode != nullptr) {
          *outRefreshMode = UI_REFRESH_FAST;
        }
        return true;
      case BTN_ACTION_CONFIRM:
        open_focused_tile();
        if (outRefreshMode != nullptr) {
          *outRefreshMode = UI_REFRESH_NAV;
        }
        return true;
      case BTN_ACTION_BACK:
        if (ui_home_get_focus() != 0) {
          ui_home_reset_focus();
          if (outRefreshMode != nullptr) {
            *outRefreshMode = UI_REFRESH_FAST;
          }
          return true;
        }
        return false;
      case BTN_ACTION_VOICE_TOGGLE:
        return open_voice_interaction(outRefreshMode);
      default:
        return false;
    }
  }

  if (ui_voice_is_active()) {
    switch (action) {
      case BTN_ACTION_BACK:
        go_home(outRefreshMode);
        return true;
      case BTN_ACTION_VOICE_TOGGLE:
        return open_voice_interaction(outRefreshMode);
      case BTN_ACTION_NEXT:
        if (voice_service_interrupt_speaker()) {
          ui_voice_refresh();
          if (outRefreshMode != nullptr) {
            *outRefreshMode = UI_REFRESH_NAV;
          }
          return true;
        }
        return false;
      default:
        return false;
    }
  }

  if (ui_settings_is_active()) {
    switch (action) {
      case BTN_ACTION_NEXT:
        ui_settings_next_row();
        if (outRefreshMode != nullptr) {
          *outRefreshMode = UI_REFRESH_FAST;
        }
        return true;
      case BTN_ACTION_PREV:
        ui_settings_prev_row();
        if (outRefreshMode != nullptr) {
          *outRefreshMode = UI_REFRESH_FAST;
        }
        return true;
      case BTN_ACTION_CONFIRM: {
        const SettingsActivateResult act = ui_settings_activate();
        if (act == SETTINGS_ACT_RESTART) {
          return true;
        }
        if (outRefreshMode != nullptr) {
          *outRefreshMode = UI_REFRESH_NAV;
        }
        return true;
      }
      case BTN_ACTION_BACK:
        if (ui_settings_back()) {
          if (outRefreshMode != nullptr) {
            *outRefreshMode = UI_REFRESH_NAV;
          }
          return true;
        }
        go_home(outRefreshMode);
        return true;
      case BTN_ACTION_VOICE_TOGGLE:
        return open_voice_interaction(outRefreshMode);
      default:
        return false;
    }
  }

  if (ui_vision_is_active()) {
    switch (action) {
      case BTN_ACTION_NEXT:
        if (ui_answers_is_busy()) {
          return false;
        }
        if (ui_vision_request_capture()) {
          ui_vision_set_busy();
          if (outRefreshMode != nullptr) {
            *outRefreshMode = UI_REFRESH_NAV;
          }
          return true;
        }
        return false;
      case BTN_ACTION_BACK:
        go_home(outRefreshMode);
        return true;
      case BTN_ACTION_VOICE_TOGGLE:
        if (ui_answers_is_busy()) {
          return false;
        }
        return open_voice_interaction(outRefreshMode);
      default:
        return false;
    }
  }

  if (ui_answers_is_active()) {
    switch (action) {
      case BTN_ACTION_NEXT:
      {
        if (ui_vision_is_busy()) {
          return false;
        }
        UiRefreshMode answerMode = UI_REFRESH_NONE;
        if (ui_answers_next(&answerMode)) {
          if (outRefreshMode != nullptr) {
            *outRefreshMode = answerMode;
          }
          return true;
        }
        return false;
      }
      case BTN_ACTION_CONFIRM:
      {
        UiRefreshMode answerMode = UI_REFRESH_NONE;
        if (ui_answers_confirm(&answerMode)) {
          if (outRefreshMode != nullptr) {
            *outRefreshMode = answerMode;
          }
          return true;
        }
        return false;
      }
      case BTN_ACTION_BACK:
        go_home(outRefreshMode);
        return true;
      case BTN_ACTION_VOICE_TOGGLE:
        return open_voice_interaction(outRefreshMode);
      default:
        return false;
    }
  }

  if (ui_clock_is_active()) {
    switch (action) {
      case BTN_ACTION_BACK:
        go_home(outRefreshMode);
        return true;
      case BTN_ACTION_VOICE_TOGGLE:
        return open_voice_interaction(outRefreshMode);
      default:
        return false;
    }
  }

  switch (action) {
    case BTN_ACTION_BACK:
      go_home(outRefreshMode);
      return true;
    case BTN_ACTION_VOICE_TOGGLE:
      return open_voice_interaction(outRefreshMode);
    default:
      return false;
  }
}
