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

bool ui_answers_request_capture(void);
bool ui_answers_consume_capture_request(void);
void ui_answers_set_busy(void);
bool ui_answers_run_capture(void);
bool ui_answers_service(UiRefreshMode *outRefreshMode);
bool ui_answers_is_busy(void);

lv_obj_t *ui_answers_get_screen(void);

#endif
