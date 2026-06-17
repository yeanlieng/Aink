#include "camera_service.h"

#define CAMERA_MODEL_XIAO_ESP32S3
#include "camera_pins.h"

#include "esp_heap_caps.h"
#include "img_converters.h"

#include <Arduino.h>
#include <esp32-hal-ledc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

static bool s_ready = false;
static CameraServiceMode s_mode = CAMERA_SERVICE_MODE_OFF;
static StaticSemaphore_t s_cameraMutexStorage;
static SemaphoreHandle_t s_cameraMutex = nullptr;
static portMUX_TYPE s_cameraMutexMux = portMUX_INITIALIZER_UNLOCKED;

static bool ensureCameraMutex(void) {
  if (s_cameraMutex != nullptr) {
    return true;
  }

  portENTER_CRITICAL(&s_cameraMutexMux);
  if (s_cameraMutex == nullptr) {
    s_cameraMutex = xSemaphoreCreateMutexStatic(&s_cameraMutexStorage);
  }
  portEXIT_CRITICAL(&s_cameraMutexMux);
  return s_cameraMutex != nullptr;
}

static void setupLedFlash(int pin) {
#if defined(LED_GPIO_NUM)
  ledcAttach(pin, 5000, 8);
#else
  (void)pin;
#endif
}

static camera_config_t buildConfig(CameraServiceMode mode, bool usePsram) {
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
  config.frame_size = mode == CAMERA_SERVICE_MODE_PREVIEW ? FRAMESIZE_128X128 : FRAMESIZE_240X240;
  config.pixel_format = mode == CAMERA_SERVICE_MODE_PREVIEW ? PIXFORMAT_GRAYSCALE : PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.jpeg_quality = 24;
  config.fb_count = 1;
  config.fb_location = mode == CAMERA_SERVICE_MODE_PREVIEW
                           ? CAMERA_FB_IN_DRAM
                           : (usePsram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM);

  if (mode == CAMERA_SERVICE_MODE_PHOTO && usePsram) {
    config.jpeg_quality = 24;
    /* Snapshot use: one buffer, stop when full — avoids FB-OVF while CPU is busy (HTTP). */
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }

  return config;
}

static void applySensorDefaults(framesize_t frameSize) {
  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr) {
    return;
  }

  if (sensor->id.PID == OV3660_PID) {
    sensor->set_vflip(sensor, 1);
    sensor->set_brightness(sensor, 1);
    sensor->set_saturation(sensor, -2);
  }

  sensor->set_framesize(sensor, frameSize);
}

static bool startCameraMode(CameraServiceMode mode) {
  if (s_ready && s_mode == mode) {
    return true;
  }
  if (s_ready) {
    esp_camera_deinit();
    s_ready = false;
    s_mode = CAMERA_SERVICE_MODE_OFF;
  }

  const bool hasPsram = psramFound();
  const bool preview = mode == CAMERA_SERVICE_MODE_PREVIEW;
  Serial.printf("[Camera] mode=%s PSRAM=%s heap=%u psram=%u block=%u\r\n",
                preview ? "preview-gray" : "photo-jpeg",
                hasPsram ? "yes" : "no",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

  camera_config_t config = buildConfig(mode, hasPsram);
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK && mode == CAMERA_SERVICE_MODE_PHOTO && hasPsram) {
    Serial.printf("[Camera] photo PSRAM init failed 0x%x, retrying in DRAM fb_count=1\r\n", err);
    esp_camera_deinit();
    config = buildConfig(mode, false);
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    err = esp_camera_init(&config);
  }
  if (err != ESP_OK && mode == CAMERA_SERVICE_MODE_PREVIEW) {
    Serial.printf("[Camera] preview 128x128 gray init failed 0x%x, retrying 96x96\r\n", err);
    esp_camera_deinit();
    config = buildConfig(mode, false);
    config.frame_size = FRAMESIZE_96X96;
    err = esp_camera_init(&config);
  }
  if (err != ESP_OK) {
    Serial.printf("[Camera] %s init failed 0x%x\r\n",
                  preview ? "preview" : "photo", err);
    return false;
  }

  applySensorDefaults(config.frame_size);

#if defined(LED_GPIO_NUM)
  setupLedFlash(LED_GPIO_NUM);
#endif

  s_ready = true;
  s_mode = mode;
  Serial.printf("[Camera] ready (%s %s)\r\n",
                preview ? (config.frame_size == FRAMESIZE_96X96 ? "96x96" : "128x128") : "240x240",
                preview ? "GRAY" : "JPEG");
  return true;
}

bool camera_service_init(void) {
  return camera_service_start_photo();
}

bool camera_service_start_preview(void) {
  return startCameraMode(CAMERA_SERVICE_MODE_PREVIEW);
}

bool camera_service_start_photo(void) {
  return startCameraMode(CAMERA_SERVICE_MODE_PHOTO);
}

bool camera_service_is_ready(void) {
  return s_ready;
}

CameraServiceMode camera_service_mode(void) {
  return s_mode;
}

bool camera_service_lock(uint32_t timeoutMs) {
  if (!ensureCameraMutex()) {
    return false;
  }

  const TickType_t ticks = timeoutMs == 0 ? 0 : pdMS_TO_TICKS(timeoutMs);
  return xSemaphoreTake(s_cameraMutex, ticks) == pdTRUE;
}

void camera_service_unlock(void) {
  if (s_cameraMutex != nullptr) {
    xSemaphoreGive(s_cameraMutex);
  }
}

void camera_service_pause(void) {
  if (!s_ready) {
    return;
  }
  esp_camera_deinit();
  s_ready = false;
  s_mode = CAMERA_SERVICE_MODE_OFF;
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

  memset(outBits, 0xFF, CAMERA_PREVIEW_BYTES);

  static const uint8_t kBayer4[4][4] = {
      {0, 8, 2, 10},
      {12, 4, 14, 6},
      {3, 11, 1, 9},
      {15, 7, 13, 5},
  };

  if ((fb->format == PIXFORMAT_GRAYSCALE || fb->format == PIXFORMAT_RAW8) &&
      fb->len >= fb->width * fb->height) {
    for (int y = 0; y < CAMERA_PREVIEW_HEIGHT; y++) {
      const int srcY = (y * (int)fb->height) / CAMERA_PREVIEW_HEIGHT;
      for (int x = 0; x < CAMERA_PREVIEW_WIDTH; x++) {
        const int srcX = (x * (int)fb->width) / CAMERA_PREVIEW_WIDTH;
        const size_t srcIndex = (size_t)srcY * fb->width + srcX;
        const int luma = fb->buf[srcIndex];
        const int threshold = 96 + (int)kBayer4[y & 0x03][x & 0x03] * 4;
        if (luma < threshold) {
          const int bitIndex = y * CAMERA_PREVIEW_WIDTH + x;
          outBits[bitIndex / 8] &= (uint8_t)~(0x80 >> (bitIndex % 8));
        }
      }
    }
    return true;
  }

  uint8_t *rgb = nullptr;
  size_t rgbSize = 0;
  if (!camera_service_frame_to_rgb888(fb, &rgb, &rgbSize)) {
    return false;
  }

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
