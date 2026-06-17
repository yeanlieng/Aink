#ifndef UI_NAV_H
#define UI_NAV_H

#include "btn_input.h"
#include "ui_refresh.h"

void ui_nav_init(void);
bool ui_nav_handle(BtnAction action, UiRefreshMode *outRefreshMode);
bool ui_nav_is_home(void);
bool ui_nav_is_weather(void);
bool ui_nav_is_settings(void);
bool ui_nav_is_vision(void);
bool ui_nav_is_answers(void);
bool ui_nav_is_stock(void);
bool ui_nav_is_clock(void);

#endif
