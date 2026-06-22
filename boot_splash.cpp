#include "boot_splash.h"

#include "epaper_canvas.h"

#include <Arduino.h>
#include <pgmspace.h>
#include <stdint.h>

#if __has_include("boot_splash_image.h")
#include "boot_splash_image.h"
#define BOOT_SPLASH_HAS_BMP 1
#else
#define BOOT_SPLASH_HAS_BMP 0
#endif

#define BOOT_SPLASH_W 200
#define BOOT_SPLASH_H 200

#if BOOT_SPLASH_HAS_BMP
static uint8_t bmpRead8(size_t offset) {
  if (offset >= BOOT_SPLASH_BMP_LEN) {
    return 0;
  }
  return pgm_read_byte(&BOOT_SPLASH_BMP[offset]);
}

static uint16_t bmpRead16(size_t offset) {
  return (uint16_t)bmpRead8(offset) |
         ((uint16_t)bmpRead8(offset + 1) << 8);
}

static uint32_t bmpRead32(size_t offset) {
  return (uint32_t)bmpRead8(offset) |
         ((uint32_t)bmpRead8(offset + 1) << 8) |
         ((uint32_t)bmpRead8(offset + 2) << 16) |
         ((uint32_t)bmpRead8(offset + 3) << 24);
}

static int32_t bmpReadS32(size_t offset) {
  return (int32_t)bmpRead32(offset);
}

static uint8_t bmpPaletteBrightness(size_t paletteOffset, uint8_t index) {
  const size_t entry = paletteOffset + (size_t)index * 4U;
  const uint16_t b = bmpRead8(entry + 0);
  const uint16_t g = bmpRead8(entry + 1);
  const uint16_t r = bmpRead8(entry + 2);
  return (uint8_t)((r + g + b) / 3U);
}

static bool drawBmpSplash(void) {
  if (BOOT_SPLASH_BMP_LEN < 62U ||
      bmpRead8(0) != 'B' || bmpRead8(1) != 'M') {
    Serial.println("[Splash] invalid BMP header");
    return false;
  }

  const uint32_t pixelOffset = bmpRead32(10);
  const uint32_t dibSize = bmpRead32(14);
  const int32_t width = bmpReadS32(18);
  const int32_t heightSigned = bmpReadS32(22);
  const uint16_t planes = bmpRead16(26);
  const uint16_t bitsPerPixel = bmpRead16(28);
  const uint32_t compression = bmpRead32(30);
  const uint32_t colorsUsed = dibSize >= 40U ? bmpRead32(46) : 0U;

  if (width != BOOT_SPLASH_W ||
      (heightSigned != BOOT_SPLASH_H && heightSigned != -BOOT_SPLASH_H) ||
      planes != 1U || bitsPerPixel != 1U || compression != 0U) {
    Serial.printf("[Splash] BMP must be 200x200 1bpp BI_RGB (w=%ld h=%ld bpp=%u comp=%lu)\n",
                  (long)width, (long)heightSigned,
                  (unsigned)bitsPerPixel, (unsigned long)compression);
    return false;
  }

  const uint32_t height = heightSigned < 0 ? (uint32_t)(-heightSigned) : (uint32_t)heightSigned;
  const uint32_t rowStride = (((uint32_t)width * bitsPerPixel + 31U) / 32U) * 4U;
  if (pixelOffset + rowStride * height > BOOT_SPLASH_BMP_LEN) {
    Serial.println("[Splash] BMP pixel data truncated");
    return false;
  }

  uint8_t blackIndex = 0;
  const uint32_t paletteOffset = 14U + dibSize;
  const uint32_t paletteEntries = colorsUsed != 0U ? colorsUsed : 2U;
  if (paletteEntries >= 2U && paletteOffset + 8U <= pixelOffset) {
    const uint8_t brightness0 = bmpPaletteBrightness(paletteOffset, 0);
    const uint8_t brightness1 = bmpPaletteBrightness(paletteOffset, 1);
    blackIndex = brightness0 <= brightness1 ? 0 : 1;
  }

  epaper_clear_white();
  for (uint16_t y = 0; y < BOOT_SPLASH_H; y++) {
    const uint32_t bmpRow = heightSigned > 0 ? (BOOT_SPLASH_H - 1U - y) : y;
    const uint32_t rowOffset = pixelOffset + bmpRow * rowStride;
    for (uint16_t x = 0; x < BOOT_SPLASH_W; x++) {
      const uint8_t packed = bmpRead8(rowOffset + (x / 8U));
      const uint8_t bit = (packed >> (7U - (x % 8U))) & 0x01U;
      epaper_set_pixel(x, y, bit == blackIndex);
    }
  }
  Serial.println("[Splash] boot BMP rendered");
  return true;
}
#endif

static void drawRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool black) {
  for (uint16_t yy = y; yy < y + h && yy < BOOT_SPLASH_H; yy++) {
    for (uint16_t xx = x; xx < x + w && xx < BOOT_SPLASH_W; xx++) {
      epaper_set_pixel(xx, yy, black);
    }
  }
}

static void drawFrame(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  drawRect(x, y, w, 2, true);
  drawRect(x, y + h - 2, w, 2, true);
  drawRect(x, y, 2, h, true);
  drawRect(x + w - 2, y, 2, h, true);
}

static void drawDefaultSplash(void) {
  epaper_clear_white();
  drawFrame(18, 18, 164, 164);
  drawFrame(24, 24, 152, 152);

  // Minimal block "AINK" mark used only when no BMP asset is compiled in.
  drawRect(48, 70, 8, 56, true);
  drawRect(72, 70, 8, 56, true);
  drawRect(56, 70, 16, 8, true);
  drawRect(56, 94, 16, 8, true);

  drawRect(90, 70, 8, 56, true);

  drawRect(110, 70, 8, 56, true);
  drawRect(118, 78, 8, 8, true);
  drawRect(126, 86, 8, 8, true);
  drawRect(134, 70, 8, 56, true);

  drawRect(154, 70, 8, 56, true);
  drawRect(162, 94, 8, 8, true);
  drawRect(170, 86, 8, 8, true);
  drawRect(170, 78, 8, 8, true);
  drawRect(170, 102, 8, 8, true);
  drawRect(170, 110, 8, 8, true);

  drawRect(58, 146, 84, 2, true);
  Serial.println("[Splash] fallback boot screen rendered");
}

bool boot_splash_draw_to_epaper(void) {
#if BOOT_SPLASH_HAS_BMP
  if (drawBmpSplash()) {
    return true;
  }
#endif
  drawDefaultSplash();
  return false;
}
