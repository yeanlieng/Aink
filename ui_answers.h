#ifndef UI_ANSWERS_H
#define UI_ANSWERS_H

#include <lvgl.h>
#include <stdbool.h>

#include "ui_refresh.h"

void ui_answers_init(void);
void ui_answers_show(void);
void ui_answers_leave(void);
void ui_answers_refresh(void);
bool ui_answers_is_active(void);
bool ui_answers_is_busy(void);

bool ui_answers_next(UiRefreshMode *outRefreshMode);
bool ui_answers_confirm(UiRefreshMode *outRefreshMode);
bool ui_answers_request(void);
bool ui_answers_service(UiRefreshMode *outRefreshMode);

lv_obj_t *ui_answers_get_screen(void);

#endif
