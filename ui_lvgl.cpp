#include "ui_lvgl.h"

#include "epaper_canvas.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

/* Must be before lvgl.h so the sketch lv_conf.h is used (not library defaults). */
#define LV_CONF_INCLUDE_SIMPLE
#include "lv_conf.h"
#include <lvgl.h>

#define UI_MAIN_WIDTH   EPD_1IN54_V2_WIDTH
#define UI_MAIN_HEIGHT  EPD_1IN54_V2_HEIGHT
#define UI_DRAW_BUF_PIXELS (UI_MAIN_WIDTH * UI_MAIN_HEIGHT)

static lv_disp_draw_buf_t s_drawBuf;
static lv_color_t *s_buf1 = nullptr;
static lv_disp_drv_t s_dispDrv;
static lv_disp_t *s_display = nullptr;
static uint32_t s_lastTickMs = 0;
static uint32_t s_flushBlackPixels = 0;

static bool lvgl_pixel_is_black(lv_color_t color) {
  /* Snap anti-aliased grays to black — e-paper cannot render gray. */
#if LV_COLOR_DEPTH == 16
  const uint16_t v = color.full;
  const uint8_t r = (uint8_t)(((v >> 11) & 0x1F) * 255 / 31);
  const uint8_t g = (uint8_t)(((v >> 5) & 0x3F) * 255 / 63);
  const uint8_t b = (uint8_t)((v & 0x1F) * 255 / 31);
  return (uint16_t)r + g + b < 765;
#else
  return lv_color_brightness(color) < 250;
#endif
}

static void lvgl_flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
  for (int32_t y = area->y1; y <= area->y2; y++) {
    for (int32_t x = area->x1; x <= area->x2; x++) {
      const bool black = lvgl_pixel_is_black(*color_p);
      epaper_set_pixel((UWORD)x, (UWORD)y, black);
      if (black) {
        s_flushBlackPixels++;
      }
      color_p++;
    }
  }
  lv_disp_flush_ready(disp_drv);
}

void ui_lvgl_init(void) {
  lv_init();
  if (s_buf1 == nullptr) {
    const size_t bufBytes = sizeof(lv_color_t) * UI_DRAW_BUF_PIXELS;
    s_buf1 = static_cast<lv_color_t *>(heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_buf1 == nullptr) {
      s_buf1 = static_cast<lv_color_t *>(heap_caps_malloc(bufBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (s_buf1 == nullptr) {
      Serial.printf("[LVGL] draw buffer alloc failed bytes=%u\n", (unsigned)bufBytes);
      return;
    }
    Serial.printf("[LVGL] draw buffer %u bytes in %s heap=%u psram=%u block=%u\n",
                  (unsigned)bufBytes,
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0 ? "PSRAM/heap" : "heap",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  }
  lv_disp_draw_buf_init(&s_drawBuf, s_buf1, nullptr, UI_DRAW_BUF_PIXELS);
  lv_disp_drv_init(&s_dispDrv);
  s_dispDrv.hor_res = UI_MAIN_WIDTH;
  s_dispDrv.ver_res = UI_MAIN_HEIGHT;
  s_dispDrv.flush_cb = lvgl_flush_cb;
  s_dispDrv.draw_buf = &s_drawBuf;
  s_dispDrv.full_refresh = 1;
  s_display = lv_disp_drv_register(&s_dispDrv);
  s_lastTickMs = millis();
  Serial.printf("[LVGL] init depth=%d disp=%p buf=%u px\n",
                (int)LV_COLOR_DEPTH, (void *)s_display, (unsigned)UI_DRAW_BUF_PIXELS);
}

void ui_lvgl_configure_screen(lv_obj_t *screen) {
  if (screen == nullptr) {
    return;
  }
  lv_obj_set_size(screen, EPD_1IN54_V2_WIDTH, EPD_1IN54_V2_HEIGHT);
  lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_left(screen, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_right(screen, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_top(screen, EPAPER_STATUS_BAR_HEIGHT, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(screen, 0, LV_PART_MAIN);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}

void ui_lvgl_tick(void) {
  const uint32_t now = millis();
  lv_tick_inc(now - s_lastTickMs);
  s_lastTickMs = now;
}

void ui_lvgl_prepare(void) {
  ui_lvgl_tick();
  for (int i = 0; i < 8; i++) {
    lv_timer_handler();
  }
}

void ui_lvgl_refresh(void) {
  if (s_display == nullptr) {
    Serial.println("[LVGL] refresh skipped: no display");
    return;
  }

  ui_lvgl_tick();
  lv_obj_t *scr = lv_scr_act();
  if (scr != nullptr) {
    lv_obj_invalidate(scr);
  }

  s_flushBlackPixels = 0;
  lv_refr_now(s_display);
}

void ui_lvgl_refresh_partial(void) {
  if (s_display == nullptr) {
    return;
  }
  ui_lvgl_tick();
  s_flushBlackPixels = 0;
  lv_refr_now(s_display);
}
