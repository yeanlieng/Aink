#include "camera_service.h"

#define CAMERA_MODEL_XIAO_ESP32S3
#include "camera_pins.h"

#include "esp_heap_caps.h"
#include "img_converters.h"

#include <Arduino.h>
#include <esp32-hal-ledc.h>
#include <string.h>

static bool s_ready = false;

static void setupLedFlash(int pin) {
#if defined(LED_GPIO_NUM)
  ledcAttach(pin, 5000, 8);
#else
  (void)pin;
#endif
}

static camera_config_t buildDefaultConfig(bool usePsram) {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  /* 240x240 is all we need; UXGA DMA buffers exhaust PSRAM alongside LVGL/WiFi. */
  config.frame_size = FRAMESIZE_240X240;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.jpeg_quality = 12;
  config.fb_count = 1;
  config.fb_location = usePsram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  if (usePsram) {
    config.jpeg_quality = 10;
    /* Snapshot use: one buffer, stop when full — avoids FB-OVF while CPU is busy (HTTP). */
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }

  return config;
}

static void applySensorDefaults(void) {
  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr) {
    return;
  }

  if (sensor->id.PID == OV3660_PID) {
    sensor->set_vflip(sensor, 1);
    sensor->set_brightness(sensor, 1);
    sensor->set_saturation(sensor, -2);
  }

  sensor->set_framesize(sensor, FRAMESIZE_240X240);
}

bool camera_service_init(void) {
  if (s_ready) {
    return true;
  }

  const bool hasPsram = psramFound();
  Serial.printf("[Camera] PSRAM=%s freeHeap=%u freePsram=%u\r\n",
                hasPsram ? "yes" : "no",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getFreePsram());

  camera_config_t config = buildDefaultConfig(hasPsram);
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK && hasPsram) {
    Serial.printf("[Camera] PSRAM init failed 0x%x, retrying in DRAM fb_count=1\r\n", err);
    esp_camera_deinit();
    config = buildDefaultConfig(false);
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    err = esp_camera_init(&config);
  }
  if (err != ESP_OK) {
    Serial.printf("[Camera] init failed 0x%x\r\n", err);
    return false;
  }

  applySensorDefaults();

#if defined(LED_GPIO_NUM)
  setupLedFlash(LED_GPIO_NUM);
#endif

  s_ready = true;
  Serial.println("[Camera] ready (240x240 JPEG)");
  return true;
}

bool camera_service_is_ready(void) {
  return s_ready;
}

void camera_service_pause(void) {
  if (!s_ready) {
    return;
  }
  esp_camera_deinit();
  s_ready = false;
}

camera_fb_t *camera_service_capture(void) {
  if (!s_ready) {
    return nullptr;
  }
  return esp_camera_fb_get();
}

void camera_service_release(camera_fb_t *fb) {
  if (fb != nullptr) {
    esp_camera_fb_return(fb);
  }
}

framesize_t camera_service_get_framesize(void) {
  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr || sensor->status.framesize == 0) {
    return FRAMESIZE_INVALID;
  }
  return static_cast<framesize_t>(sensor->status.framesize);
}

bool camera_service_set_framesize(framesize_t size) {
  if (!s_ready) {
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr) {
    return false;
  }

  return sensor->set_framesize(sensor, size) == 0;
}

bool camera_service_frame_to_rgb888(const camera_fb_t *fb, uint8_t **outRgb, size_t *outSize) {
  if (fb == nullptr || outRgb == nullptr || outSize == nullptr) {
    return false;
  }

  const size_t rgbSize = static_cast<size_t>(fb->width) * fb->height * 3;
  uint8_t *rgb = static_cast<uint8_t *>(heap_caps_malloc(rgbSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (rgb == nullptr) {
    rgb = static_cast<uint8_t *>(malloc(rgbSize));
  }
  if (rgb == nullptr) {
    return false;
  }

  if (!fmt2rgb888(fb->buf, fb->len, fb->format, rgb)) {
    free(rgb);
    return false;
  }

  *outRgb = rgb;
  *outSize = rgbSize;
  return true;
}

bool camera_service_frame_to_mono_preview100(const camera_fb_t *fb, uint8_t *outBits, size_t outBitsLen) {
  if (fb == nullptr || outBits == nullptr || outBitsLen < CAMERA_PREVIEW_BYTES) {
    return false;
  }
  if (fb->width == 0 || fb->height == 0) {
    return false;
  }

  uint8_t *rgb = nullptr;
  size_t rgbSize = 0;
  if (!camera_service_frame_to_rgb888(fb, &rgb, &rgbSize)) {
    return false;
  }

  memset(outBits, 0xFF, CAMERA_PREVIEW_BYTES);

  static const uint8_t kBayer4[4][4] = {
      {0, 8, 2, 10},
      {12, 4, 14, 6},
      {3, 11, 1, 9},
      {15, 7, 13, 5},
  };

  for (int y = 0; y < CAMERA_PREVIEW_HEIGHT; y++) {
    const int srcY = (y * (int)fb->height) / CAMERA_PREVIEW_HEIGHT;
    for (int x = 0; x < CAMERA_PREVIEW_WIDTH; x++) {
      const int srcX = (x * (int)fb->width) / CAMERA_PREVIEW_WIDTH;
      const size_t srcIndex = ((size_t)srcY * fb->width + srcX) * 3;
      if (srcIndex + 2 >= rgbSize) {
        continue;
      }

      const uint8_t r = rgb[srcIndex];
      const uint8_t g = rgb[srcIndex + 1];
      const uint8_t b = rgb[srcIndex + 2];
      const int luma = (77 * r + 150 * g + 29 * b) >> 8;
      const int threshold = 96 + (int)kBayer4[y & 0x03][x & 0x03] * 4;
      const bool black = luma < threshold;
      if (black) {
        const int bitIndex = y * CAMERA_PREVIEW_WIDTH + x;
        outBits[bitIndex / 8] &= (uint8_t)~(0x80 >> (bitIndex % 8));
      }
    }
  }

  free(rgb);
  return true;
}

bool camera_service_frame_to_mosaic_preview100(const camera_fb_t *fb, uint8_t *outBits,
                                               size_t outBitsLen, uint8_t blockPx) {
  if (fb == nullptr || outBits == nullptr || outBitsLen < CAMERA_PREVIEW_BYTES) {
    return false;
  }
  if (fb->width == 0 || fb->height == 0) {
    return false;
  }
  if (blockPx < 2) {
    blockPx = 2;
  }

  uint8_t *rgb = nullptr;
  size_t rgbSize = 0;
  if (!camera_service_frame_to_rgb888(fb, &rgb, &rgbSize)) {
    return false;
  }

  memset(outBits, 0xFF, CAMERA_PREVIEW_BYTES);

  for (int by = 0; by < CAMERA_PREVIEW_HEIGHT; by += blockPx) {
    for (int bx = 0; bx < CAMERA_PREVIEW_WIDTH; bx += blockPx) {
      const int blockH = (by + blockPx <= CAMERA_PREVIEW_HEIGHT)
                             ? blockPx
                             : (CAMERA_PREVIEW_HEIGHT - by);
      const int blockW = (bx + blockPx <= CAMERA_PREVIEW_WIDTH)
                             ? blockPx
                             : (CAMERA_PREVIEW_WIDTH - bx);

      uint32_t lumaSum = 0;
      int samples = 0;
      for (int py = 0; py < blockH; py++) {
        const int previewY = by + py;
        const int srcY = (previewY * (int)fb->height) / CAMERA_PREVIEW_HEIGHT;
        for (int px = 0; px < blockW; px++) {
          const int previewX = bx + px;
          const int srcX = (previewX * (int)fb->width) / CAMERA_PREVIEW_WIDTH;
          const size_t srcIndex = ((size_t)srcY * fb->width + srcX) * 3;
          if (srcIndex + 2 >= rgbSize) {
            continue;
          }
          const uint8_t r = rgb[srcIndex];
          const uint8_t g = rgb[srcIndex + 1];
          const uint8_t b = rgb[srcIndex + 2];
          lumaSum += (77 * r + 150 * g + 29 * b) >> 8;
          samples++;
        }
      }

      const bool black = samples > 0 && (lumaSum / (uint32_t)samples) < 128;
      if (!black) {
        continue;
      }
      for (int py = 0; py < blockH; py++) {
        for (int px = 0; px < blockW; px++) {
          const int bitIndex = (by + py) * CAMERA_PREVIEW_WIDTH + (bx + px);
          outBits[bitIndex / 8] &= (uint8_t)~(0x80 >> (bitIndex % 8));
        }
      }
    }
  }

  free(rgb);
  return true;
}

bool camera_service_frame_to_mosaic_jpeg(const camera_fb_t *fb, uint8_t blockPx,
                                         uint8_t jpegQuality, uint8_t **outJpeg,
                                         size_t *outLen) {
  if (fb == nullptr || outJpeg == nullptr || outLen == nullptr) {
    return false;
  }
  *outJpeg = nullptr;
  *outLen = 0;
  if (fb->width == 0 || fb->height == 0) {
    return false;
  }
  if (blockPx < 2) {
    blockPx = 2;
  }

  uint8_t *rgb = nullptr;
  size_t rgbSize = 0;
  if (!camera_service_frame_to_rgb888(fb, &rgb, &rgbSize)) {
    return false;
  }

  const int w = fb->width;
  const int h = fb->height;
  for (int by = 0; by < h; by += blockPx) {
    for (int bx = 0; bx < w; bx += blockPx) {
      const int blockH = (by + blockPx <= h) ? blockPx : (h - by);
      const int blockW = (bx + blockPx <= w) ? blockPx : (w - bx);

      uint32_t sumR = 0;
      uint32_t sumG = 0;
      uint32_t sumB = 0;
      const int pixelCount = blockW * blockH;
      for (int py = 0; py < blockH; py++) {
        for (int px = 0; px < blockW; px++) {
          const size_t idx = ((size_t)(by + py) * w + (bx + px)) * 3;
          if (idx + 2 >= rgbSize) {
            continue;
          }
          sumR += rgb[idx];
          sumG += rgb[idx + 1];
          sumB += rgb[idx + 2];
        }
      }

      const uint8_t avgR = (uint8_t)(sumR / pixelCount);
      const uint8_t avgG = (uint8_t)(sumG / pixelCount);
      const uint8_t avgB = (uint8_t)(sumB / pixelCount);
      for (int py = 0; py < blockH; py++) {
        for (int px = 0; px < blockW; px++) {
          const size_t idx = ((size_t)(by + py) * w + (bx + px)) * 3;
          if (idx + 2 >= rgbSize) {
            continue;
          }
          rgb[idx] = avgR;
          rgb[idx + 1] = avgG;
          rgb[idx + 2] = avgB;
        }
      }
    }
  }

  uint8_t *jpeg = nullptr;
  size_t jpegLen = 0;
  const bool ok = fmt2jpg(rgb, rgbSize, (uint16_t)w, (uint16_t)h, PIXFORMAT_RGB888,
                          jpegQuality, &jpeg, &jpegLen);
  free(rgb);

  if (!ok || jpeg == nullptr || jpegLen == 0) {
    free(jpeg);
    return false;
  }

  *outJpeg = jpeg;
  *outLen = jpegLen;
  return true;
}
