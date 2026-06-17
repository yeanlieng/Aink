#include "ui_answers.h"

#include "app_locale.h"
#include "camera_service.h"
#include "ui_fonts.h"
#include "vision_service.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#define ANSWERS_TASK_STACK_BYTES 16384
#define ANSWERS_WAIT_ANIM_MS    800U
#define ANSWERS_CAPTURE_MAX_JPEG_BYTES (64 * 1024)

static lv_obj_t *s_screenAnswers = nullptr;
static lv_obj_t *s_titleLabel = nullptr;
static lv_obj_t *s_hintLabel = nullptr;
static lv_obj_t *s_bodyLabel = nullptr;
static bool s_captureRequested = false;
static bool s_busy = false;
static bool s_answerOnlyVisible = false;
static TaskHandle_t s_captureTask = nullptr;
static SemaphoreHandle_t s_cameraMutex = nullptr;
static portMUX_TYPE s_captureMux = portMUX_INITIALIZER_UNLOCKED;
static bool s_captureRunning = false;
static bool s_captureDone = false;
static VisionResult s_captureCode = VISION_RESULT_HTTP_FAIL;
static char s_captureText[128];
static uint8_t *s_captureJpeg = nullptr;
static size_t s_captureJpegLen = 0;
static unsigned long s_waitAnimLastMs = 0;
static uint8_t s_waitAnimDots = 1;

static const char *answers_error_text(VisionResult result) {
  switch (result) {
    case VISION_RESULT_NO_CAMERA:
      return app_tr(TR_VISION_NO_CAMERA);
    case VISION_RESULT_CAPTURE_FAIL:
      return app_tr(TR_VISION_NO_CAMERA);
    default:
      return app_tr(TR_VISION_FAIL);
  }
}

static void style_label(lv_obj_t *label, const lv_font_t *font) {
  lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_text_opa(label, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(label, 0, LV_PART_MAIN);
  if (font != nullptr) {
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  }
}

static void copy_text(char *dst, size_t dstLen, const char *src) {
  if (dst == nullptr || dstLen == 0) {
    return;
  }
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }

  size_t i = 0;
  while (i + 1 < dstLen && src[i] != '\0') {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static void build_waiting_text(char *out, size_t outLen, uint8_t dots) {
  if (out == nullptr || outLen == 0) {
    return;
  }
  if (dots < 1) dots = 1;
  if (dots > 3) dots = 3;

  const bool zh = app_locale_get() == APP_LANG_ZH;
  const char *prefix = zh ? "答案浮现中" : "Seeking answer";
  const char *suffix =
      zh ? (dots == 1 ? "。" : (dots == 2 ? "。。" : "。。。"))
         : (dots == 1 ? "." : (dots == 2 ? ".." : "..."));
  snprintf(out, outLen, "%s%s", prefix, suffix);
}

static bool ensure_camera_mutex(void) {
  if (s_cameraMutex != nullptr) {
    return true;
  }

  s_cameraMutex = xSemaphoreCreateMutex();
  if (s_cameraMutex == nullptr) {
    Serial.println("[Answers] camera mutex create failed");
    return false;
  }
  return true;
}

static void apply_final_answer_layout(bool answerOnly) {
  s_answerOnlyVisible = answerOnly;
  if (s_bodyLabel == nullptr) {
    return;
  }

  if (answerOnly) {
    if (s_titleLabel != nullptr) {
      lv_label_set_text(s_titleLabel, "");
    }
    if (s_hintLabel != nullptr) {
      lv_label_set_text(s_hintLabel, "");
    }
    lv_obj_set_style_text_font(s_bodyLabel, UI_FONT_MD, LV_PART_MAIN);
    lv_obj_set_width(s_bodyLabel, 188);
    lv_obj_set_height(s_bodyLabel, 118);
    lv_label_set_long_mode(s_bodyLabel, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_bodyLabel, LV_ALIGN_CENTER, 0, 0);
    return;
  }

  lv_obj_set_style_text_font(s_bodyLabel, UI_FONT_MD, LV_PART_MAIN);
  lv_obj_set_width(s_bodyLabel, 188);
  lv_obj_set_height(s_bodyLabel, 72);
  lv_label_set_long_mode(s_bodyLabel, LV_LABEL_LONG_WRAP);
  lv_obj_align(s_bodyLabel, LV_ALIGN_CENTER, 0, 18);
}

static void set_waiting_text(uint8_t dots) {
  char text[48];
  apply_final_answer_layout(false);
  build_waiting_text(text, sizeof(text), dots);
  lv_label_set_text(s_titleLabel, app_tr(TR_BOOK_TITLE));
  lv_label_set_text(s_hintLabel, "");
  lv_label_set_text(s_bodyLabel, text);
  lv_obj_invalidate(s_titleLabel);
  lv_obj_invalidate(s_hintLabel);
  lv_obj_invalidate(s_bodyLabel);
}

static bool capture_running(void) {
  portENTER_CRITICAL(&s_captureMux);
  const bool running = s_captureRunning;
  portEXIT_CRITICAL(&s_captureMux);
  return running;
}

static uint8_t *alloc_jpeg_copy(size_t len) {
  uint8_t *copy = static_cast<uint8_t *>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (copy == nullptr) {
    copy = static_cast<uint8_t *>(malloc(len));
  }
  return copy;
}

static VisionResult capture_frame_for_answer(uint8_t **outJpeg, size_t *outLen) {
  if (outJpeg == nullptr || outLen == nullptr) {
    return VISION_RESULT_CAPTURE_FAIL;
  }
  *outJpeg = nullptr;
  *outLen = 0;

  if (!ensure_camera_mutex()) {
    return VISION_RESULT_CAPTURE_FAIL;
  }

  if (xSemaphoreTake(s_cameraMutex, pdMS_TO_TICKS(1600)) != pdTRUE) {
    Serial.println("[Answers] camera busy while capture");
    return VISION_RESULT_CAPTURE_FAIL;
  }

  VisionResult status = VISION_RESULT_CAPTURE_FAIL;
  camera_fb_t *fb = nullptr;

  if (!camera_service_is_ready() && !camera_service_init()) {
    status = VISION_RESULT_NO_CAMERA;
  } else {
    camera_fb_t *warmup = camera_service_capture();
    if (warmup != nullptr) {
      camera_service_release(warmup);
    }

    fb = camera_service_capture();
    if (fb == nullptr || fb->len == 0 || fb->len > ANSWERS_CAPTURE_MAX_JPEG_BYTES) {
      Serial.printf("[Answers] capture fail fb=%p len=%u\r\n",
                    (void *)fb, fb != nullptr ? (unsigned)fb->len : 0U);
      status = VISION_RESULT_CAPTURE_FAIL;
    } else {
      uint8_t *copy = alloc_jpeg_copy(fb->len);
      if (copy == nullptr) {
        Serial.printf("[Answers] jpeg copy alloc failed len=%u\r\n", (unsigned)fb->len);
        status = VISION_RESULT_CAPTURE_FAIL;
      } else {
        memcpy(copy, fb->buf, fb->len);
        *outJpeg = copy;
        *outLen = fb->len;
        status = VISION_RESULT_OK;
        Serial.printf("[Answers] JPEG %u bytes captured\r\n", (unsigned)fb->len);
      }
    }
  }

  if (fb != nullptr) {
    camera_service_release(fb);
  }

  camera_service_pause();
  xSemaphoreGive(s_cameraMutex);
  return status;
}

static void answers_capture_task(void *param) {
  (void)param;

  char result[128];
  result[0] = '\0';

  uint8_t *jpeg = nullptr;
  size_t jpegLen = 0;

  portENTER_CRITICAL(&s_captureMux);
  jpeg = s_captureJpeg;
  jpegLen = s_captureJpegLen;
  s_captureJpeg = nullptr;
  s_captureJpegLen = 0;
  portEXIT_CRITICAL(&s_captureMux);

  VisionResult code = VISION_RESULT_CAPTURE_FAIL;
  if (jpeg != nullptr && jpegLen > 0) {
    code = vision_service_book_answer_jpeg(jpeg, jpegLen, result, sizeof(result));
  }
  free(jpeg);

  portENTER_CRITICAL(&s_captureMux);
  s_captureCode = code;
  copy_text(s_captureText, sizeof(s_captureText), result);
  s_captureDone = true;
  s_captureRunning = false;
  s_captureTask = nullptr;
  portEXIT_CRITICAL(&s_captureMux);

  vTaskDelete(nullptr);
}

static void show_idle(void) {
  s_busy = false;
  apply_final_answer_layout(false);
  lv_label_set_text(s_titleLabel, app_tr(TR_BOOK_TITLE));
  lv_label_set_text(s_hintLabel, app_tr(TR_BOOK_HINT));
  lv_label_set_text(s_bodyLabel, "?");
}

void ui_answers_init(void) {
  s_screenAnswers = lv_obj_create(nullptr);
  lv_obj_set_size(s_screenAnswers, 200, 180);
  lv_obj_set_style_bg_color(s_screenAnswers, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_screenAnswers, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_screenAnswers, LV_OBJ_FLAG_SCROLLABLE);

  s_titleLabel = lv_label_create(s_screenAnswers);
  style_label(s_titleLabel, UI_FONT_MD);
  lv_obj_align(s_titleLabel, LV_ALIGN_TOP_MID, 0, 12);

  s_hintLabel = lv_label_create(s_screenAnswers);
  style_label(s_hintLabel, UI_FONT_SM);
  lv_obj_set_width(s_hintLabel, 188);
  lv_label_set_long_mode(s_hintLabel, LV_LABEL_LONG_WRAP);
  lv_obj_align(s_hintLabel, LV_ALIGN_TOP_MID, 0, 36);

  s_bodyLabel = lv_label_create(s_screenAnswers);
  style_label(s_bodyLabel, UI_FONT_MD);
  lv_obj_set_style_text_align(s_bodyLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_width(s_bodyLabel, 188);
  lv_obj_set_height(s_bodyLabel, 72);
  lv_label_set_long_mode(s_bodyLabel, LV_LABEL_LONG_WRAP);
  lv_obj_align(s_bodyLabel, LV_ALIGN_CENTER, 0, 18);

  show_idle();
}

void ui_answers_show(void) {
  if (capture_running()) {
    s_busy = true;
    set_waiting_text(s_waitAnimDots);
  } else {
    show_idle();
  }
  lv_scr_load(s_screenAnswers);
  lv_obj_invalidate(s_screenAnswers);
}

void ui_answers_leave(void) {
  if (!capture_running()) {
    if (s_cameraMutex != nullptr) {
      if (xSemaphoreTake(s_cameraMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
        camera_service_pause();
        xSemaphoreGive(s_cameraMutex);
      }
    } else {
      camera_service_pause();
    }
  }
}

void ui_answers_refresh(void) {
  if (s_screenAnswers == nullptr || lv_scr_act() != s_screenAnswers) {
    return;
  }
  if (s_answerOnlyVisible) {
    lv_obj_invalidate(s_screenAnswers);
    return;
  }
  lv_label_set_text(s_titleLabel, app_tr(TR_BOOK_TITLE));
  if (s_busy) {
    set_waiting_text(s_waitAnimDots);
  } else {
    lv_label_set_text(s_hintLabel, app_tr(TR_BOOK_HINT));
  }
  lv_obj_invalidate(s_screenAnswers);
}

bool ui_answers_is_active(void) {
  return s_screenAnswers != nullptr && lv_scr_act() == s_screenAnswers;
}

bool ui_answers_request_capture(void) {
  if (s_busy || capture_running()) {
    return false;
  }
  s_captureRequested = true;
  return true;
}

bool ui_answers_consume_capture_request(void) {
  if (!s_captureRequested) {
    return false;
  }
  s_captureRequested = false;
  return true;
}

void ui_answers_set_busy(void) {
  s_busy = true;
  s_waitAnimDots = 1;
  s_waitAnimLastMs = millis();
  set_waiting_text(s_waitAnimDots);
  lv_obj_invalidate(s_screenAnswers);
}

bool ui_answers_run_capture(void) {
  if (capture_running()) {
    return false;
  }

  s_busy = true;

  uint8_t *jpeg = nullptr;
  size_t jpegLen = 0;
  const VisionResult prep = capture_frame_for_answer(&jpeg, &jpegLen);
  if (prep != VISION_RESULT_OK) {
    free(jpeg);
    s_busy = false;
    apply_final_answer_layout(false);
    lv_label_set_text(s_titleLabel, app_tr(TR_BOOK_TITLE));
    lv_label_set_text(s_hintLabel, app_tr(TR_BOOK_HINT));
    lv_label_set_text(s_bodyLabel, answers_error_text(prep));
    lv_obj_invalidate(s_screenAnswers);
    return false;
  }

  portENTER_CRITICAL(&s_captureMux);
  s_captureRunning = true;
  s_captureDone = false;
  s_captureCode = VISION_RESULT_HTTP_FAIL;
  s_captureText[0] = '\0';
  s_captureJpeg = jpeg;
  s_captureJpegLen = jpegLen;
  portEXIT_CRITICAL(&s_captureMux);

  if (xTaskCreate(answers_capture_task, "answers", ANSWERS_TASK_STACK_BYTES,
                  nullptr, 1, &s_captureTask) != pdPASS) {
    uint8_t *staleJpeg = nullptr;

    portENTER_CRITICAL(&s_captureMux);
    staleJpeg = s_captureJpeg;
    s_captureJpeg = nullptr;
    s_captureJpegLen = 0;
    s_captureRunning = false;
    s_captureDone = false;
    s_captureTask = nullptr;
    portEXIT_CRITICAL(&s_captureMux);

    free(staleJpeg);
    s_busy = false;
    apply_final_answer_layout(false);
    lv_label_set_text(s_titleLabel, app_tr(TR_BOOK_TITLE));
    lv_label_set_text(s_hintLabel, app_tr(TR_BOOK_HINT));
    lv_label_set_text(s_bodyLabel, app_tr(TR_VISION_FAIL));
    lv_obj_invalidate(s_screenAnswers);
    Serial.println("[Answers] task create failed");
    return false;
  }

  return true;
}

bool ui_answers_service(UiRefreshMode *outRefreshMode) {
  if (outRefreshMode != nullptr) {
    *outRefreshMode = UI_REFRESH_NONE;
  }
  if (s_screenAnswers == nullptr) {
    return false;
  }

  bool done = false;
  VisionResult code = VISION_RESULT_HTTP_FAIL;
  char result[128];
  result[0] = '\0';

  portENTER_CRITICAL(&s_captureMux);
  if (s_captureDone) {
    done = true;
    code = s_captureCode;
    copy_text(result, sizeof(result), s_captureText);
    s_captureDone = false;
  }
  portEXIT_CRITICAL(&s_captureMux);

  if (done) {
    s_busy = false;
    if (code == VISION_RESULT_OK) {
      apply_final_answer_layout(true);
      lv_label_set_text(s_bodyLabel, result);
    } else {
      apply_final_answer_layout(false);
      lv_label_set_text(s_titleLabel, app_tr(TR_BOOK_TITLE));
      lv_label_set_text(s_hintLabel, app_tr(TR_BOOK_HINT));
      lv_label_set_text(s_bodyLabel, answers_error_text(code));
    }
    lv_obj_invalidate(s_titleLabel);
    lv_obj_invalidate(s_hintLabel);
    lv_obj_invalidate(s_bodyLabel);
    lv_obj_invalidate(s_screenAnswers);
    if (outRefreshMode != nullptr) {
      *outRefreshMode = UI_REFRESH_NAV;
    }
    Serial.println("[Answers] capture pipeline done");
    return ui_answers_is_active();
  }

  if (!ui_answers_is_active() || !s_busy) {
    return false;
  }

  const unsigned long now = millis();
  if ((unsigned long)(now - s_waitAnimLastMs) < ANSWERS_WAIT_ANIM_MS) {
    return false;
  }

  s_waitAnimLastMs = now;
  s_waitAnimDots = (s_waitAnimDots % 3U) + 1U;
  set_waiting_text(s_waitAnimDots);
  if (outRefreshMode != nullptr) {
    *outRefreshMode = UI_REFRESH_FAST;
  }
  return true;
}

bool ui_answers_is_busy(void) {
  return s_busy || capture_running();
}

lv_obj_t *ui_answers_get_screen(void) {
  return s_screenAnswers;
}
