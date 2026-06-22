#include "epaper_canvas.h"

#include <Arduino.h>
#include <string.h>

#include "EPD_1in54_V2.h"

static UBYTE s_blackImage[5000];
static bool s_epaperPartialReady = false;
static bool s_portalMirror = false;

enum EpaperUploadState {
  EPAPER_UPLOAD_IDLE = 0,
  EPAPER_UPLOAD_FULL_WAIT,
  EPAPER_UPLOAD_PARTIAL_WAIT,
};

static EpaperUploadState s_uploadState = EPAPER_UPLOAD_IDLE;
static bool s_uploadMirrored = false;

static const UWORD kRowBytes = (EPD_1IN54_V2_WIDTH + 7) / 8;

static void mapToBuffer(UWORD lx, UWORD ly, UWORD *px, UWORD *py) {
#if EPAPER_ROTATE_CW90
  *px = ly;
  *py = lx;
  if (s_portalMirror) {
    *py = EPD_1IN54_V2_WIDTH - lx - 1;
  }
#else
  *px = lx;
  *py = ly;
  if (s_portalMirror) {
    *px = EPD_1IN54_V2_WIDTH - lx - 1;
  }
#endif
}

UBYTE *epaper_get_buffer(void) {
  return s_blackImage;
}

void epaper_set_portal_mirror(bool enabled) {
  s_portalMirror = enabled;
}

void epaper_set_pixel(UWORD lx, UWORD ly, bool black) {
  UWORD x;
  UWORD y;
  mapToBuffer(lx, ly, &x, &y);
  if (x >= EPD_1IN54_V2_WIDTH || y >= EPD_1IN54_V2_HEIGHT) {
    return;
  }
  const UWORD index = (x / 8) + y * kRowBytes;
  const UBYTE mask = 0x80 >> (x % 8);
  if (black) {
    s_blackImage[index] &= ~mask;
  } else {
    s_blackImage[index] |= mask;
  }
}

void epaper_clear_white(void) {
  memset(s_blackImage, 0xFF, sizeof(s_blackImage));
}

void epaper_clear_main_area(void) {
  for (UWORD ly = EPAPER_STATUS_BAR_HEIGHT; ly < EPD_1IN54_V2_HEIGHT; ly++) {
    for (UWORD lx = 0; lx < EPD_1IN54_V2_WIDTH; lx++) {
      epaper_set_pixel(lx, ly, false);
    }
  }
}

bool epaper_is_partial_ready(void) {
  return s_epaperPartialReady;
}

void epaper_mark_partial_ready(void) {
  s_epaperPartialReady = true;
}

static void mirrorLogicalXInPlace(void) {
  for (UWORD top = 0, bottom = EPD_1IN54_V2_HEIGHT - 1; top < bottom; top++, bottom--) {
    for (UWORD col = 0; col < kRowBytes; col++) {
      const UWORD topIndex = top * kRowBytes + col;
      const UWORD bottomIndex = bottom * kRowBytes + col;
      const UBYTE tmp = s_blackImage[topIndex];
      s_blackImage[topIndex] = s_blackImage[bottomIndex];
      s_blackImage[bottomIndex] = tmp;
    }
  }
}

void epaper_display_base_image_async(void) {
  mirrorLogicalXInPlace();
  EPD_1IN54_V2_DisplayPartBaseImageAsync(s_blackImage);
  mirrorLogicalXInPlace();
}

void epaper_upload(bool fullRefresh) {
  epaper_upload_mode(fullRefresh, false);
}

void epaper_upload_mode(bool fullInit, bool fastPartial) {
  if (!epaper_upload_mode_async(fullInit, fastPartial)) {
    return;
  }
  while (!epaper_poll_upload()) {
    delay(1);
  }
}

bool epaper_upload_mode_async(bool fullInit, bool fastPartial) {
  if (s_uploadState != EPAPER_UPLOAD_IDLE) {
    return false;
  }

  if (fullInit || !s_epaperPartialReady) {
    Serial.println("[EPD] upload full init...");
    EPD_1IN54_V2_Init();
    epaper_display_base_image_async();
    s_uploadState = EPAPER_UPLOAD_FULL_WAIT;
    return true;
  }

  (void)fastPartial;
  mirrorLogicalXInPlace();
  s_uploadMirrored = true;
  EPD_1IN54_V2_DisplayPartAsync(s_blackImage);
  s_uploadState = EPAPER_UPLOAD_PARTIAL_WAIT;
  return true;
}

bool epaper_poll_upload(void) {
  if (s_uploadState == EPAPER_UPLOAD_IDLE) {
    return true;
  }

  if (EPD_1IN54_V2_PollBusyWait()) {
    return false;
  }

  if (s_uploadState == EPAPER_UPLOAD_FULL_WAIT) {
    EPD_1IN54_V2_Init_Partial();
    s_epaperPartialReady = true;
  } else if (s_uploadState == EPAPER_UPLOAD_PARTIAL_WAIT) {
    EPD_1IN54_V2_LoadPartOldImage(s_blackImage);
    if (s_uploadMirrored) {
      mirrorLogicalXInPlace();
      s_uploadMirrored = false;
    }
  }

  s_uploadState = EPAPER_UPLOAD_IDLE;
  return true;
}

bool epaper_upload_active(void) {
  return s_uploadState != EPAPER_UPLOAD_IDLE;
}
