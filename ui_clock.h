#ifndef UI_CLOCK_H
#define UI_CLOCK_H

#include <lvgl.h>
#include <time.h>

void ui_clock_init(void);
void ui_clock_show(void);
void ui_clock_refresh(void);
void ui_clock_refresh_if_minute(const struct tm *timeinfo);
void ui_clock_on_settings_changed(void);
void ui_clock_refresh_locale(void);
bool ui_clock_is_active(void);

lv_obj_t *ui_clock_get_screen(void);

#endif
