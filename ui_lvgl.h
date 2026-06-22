#ifndef UI_LVGL_H
#define UI_LVGL_H

typedef struct _lv_obj_t lv_obj_t;

void ui_lvgl_init(void);
void ui_lvgl_tick(void);
/** Run LVGL timers/layout before the first refresh after init. */
void ui_lvgl_prepare(void);
void ui_lvgl_refresh(void);
void ui_lvgl_refresh_partial(void);
void ui_lvgl_configure_screen(lv_obj_t *screen);

#endif
