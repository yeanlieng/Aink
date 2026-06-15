/**
 * LVGL config for 200x180 main UI on 1-bit e-paper (via 8-bit flush).
 * Copy or symlink is NOT required if ui_lvgl.cpp defines LV_CONF_INCLUDE_SIMPLE
 * and includes this file before lvgl.h.
 */
#if 1

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* 8-bit is reliable for flush_cb; e-paper driver converts to 1-bit pixels. */
#define LV_COLOR_DEPTH     8
#define LV_COLOR_16_SWAP   0

#define LV_USE_GPU         0
#define LV_DISP_DEF_REFR_PERIOD 1000
#define LV_DPI_DEF         130

#define LV_MEM_CUSTOM      0
#define LV_MEM_SIZE        (48U * 1024U)

#define LV_USE_LOG         0

struct _lv_font_t;
typedef struct _lv_font_t lv_font_t;
extern const lv_font_t aink_3500_12;
extern const lv_font_t aink_3500_14;
extern const lv_font_t aink_clock_40;

#define LV_FONT_MONTSERRAT_12  0
#define LV_FONT_MONTSERRAT_14  0
#define LV_FONT_DEFAULT          &aink_3500_14

#define LV_USE_BTN         1
#define LV_USE_LABEL       1
#define LV_USE_THEME_DEFAULT 1

#define LV_TICK_CUSTOM     0

#endif /* LV_CONF_H */

#endif
