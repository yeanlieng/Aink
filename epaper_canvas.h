#ifndef EPAPER_CANVAS_H
#define EPAPER_CANVAS_H

#include "DEV_Config.h"
#include "EPD_1in54_V2.h"

#define EPAPER_ROTATE_CW90  1
#define EPAPER_STATUS_BAR_HEIGHT  20
#define EPAPER_MAIN_HEIGHT  (EPD_1IN54_V2_HEIGHT - EPAPER_STATUS_BAR_HEIGHT)

UBYTE *epaper_get_buffer(void);
void epaper_set_portal_mirror(bool enabled);
void epaper_set_pixel(UWORD lx, UWORD ly, bool black);
void epaper_clear_white(void);
void epaper_clear_main_area(void);
bool epaper_is_partial_ready(void);
void epaper_mark_partial_ready(void);
void epaper_upload(bool fullRefresh);
void epaper_upload_mode(bool fullInit, bool fastPartial);
bool epaper_upload_mode_async(bool fullInit, bool fastPartial);
bool epaper_poll_upload(void);
bool epaper_upload_active(void);

#endif
