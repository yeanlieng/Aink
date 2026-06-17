#ifndef UI_ANSWERBOOK_H
#define UI_ANSWERBOOK_H

#include <lvgl.h>
#include <stdbool.h>

#include "btn_input.h"
#include "ui_refresh.h"

void ui_answerbook_init(void);
void ui_answerbook_show(void);
void ui_answerbook_leave(void);
void ui_answerbook_refresh(void);
void ui_answerbook_refresh_locale(void);
bool ui_answerbook_is_active(void);
bool ui_answerbook_is_busy(void);

bool ui_answerbook_next(UiRefreshMode *outRefreshMode);
bool ui_answerbook_confirm(UiRefreshMode *outRefreshMode);
bool ui_answerbook_request(void);
bool ui_answerbook_service(UiRefreshMode *outRefreshMode);

lv_obj_t *ui_answerbook_get_screen(void);

#endif
