#ifndef UI_HOME_H
#define UI_HOME_H

#include <lvgl.h>

void ui_home_init(void);
void ui_home_show(void);
void ui_home_refresh_weather(void);
void ui_home_refresh_clock(void);
void ui_home_refresh_stocks(void);
void ui_home_refresh_answerbook(void);
void ui_home_refresh_locale(void);
void ui_home_next_focus(int *outPrevFocus);
void ui_home_prev_focus(int *outPrevFocus);
int ui_home_get_focus(void);
const char *ui_home_focus_title(void);

void ui_detail_show(const char *title, const char *body);

lv_obj_t *ui_home_get_screen(void);
lv_obj_t *ui_detail_get_screen(void);

#endif
