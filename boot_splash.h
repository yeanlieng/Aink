#ifndef BOOT_SPLASH_H
#define BOOT_SPLASH_H

#include <stdbool.h>

// Draw the boot splash into epaper_get_buffer().
// If boot_splash_image.h is present, it must define BOOT_SPLASH_BMP
// as a 200x200 1-bit BMP byte array in PROGMEM.
bool boot_splash_draw_to_epaper(void);

#endif
