#ifndef UI_STOCK_H
#define UI_STOCK_H

#include <lvgl.h>
#include <stdbool.h>

void ui_stock_init(void);
void ui_stock_show(void);
void ui_stock_refresh(void);
bool ui_stock_is_active(void);

lv_obj_t *ui_stock_get_screen(void);

#endif
