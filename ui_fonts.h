#ifndef UI_FONTS_H
#define UI_FONTS_H

#include <lvgl.h>

LV_FONT_DECLARE(aink_3500_12);
LV_FONT_DECLARE(aink_clock_40);

#define UI_FONT_SM     (&aink_3500_12)
#define UI_FONT_MD     (&aink_3500_12)
#define UI_FONT_CLOCK  (&aink_clock_40)

#endif
