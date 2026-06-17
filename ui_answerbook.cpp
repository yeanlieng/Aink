#include "ui_answerbook.h"

#include "app_locale.h"
#include "bookofanswers.h"
#include "camera_service.h"
#include "oracle_service.h"
#include "ui_fonts.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#define ANSWERBOOK_TASK_STACK_BYTES         16384
#define ANSWERBOOK_PREVIEW_TASK_STACK_BYTES 12288
#define ANSWERBOOK_PREVIEW_MS               3000U
#define ANSWERBOOK_PREVIEW_FRAME_X          49
#define ANSWERBOOK_PREVIEW_FRAME_Y          55
#define ANSWERBOOK_PREVIEW_FRAME_SIZE       102
#define ANSWERBOOK_CAPTURE_MAX_JPEG_BYTES   (64 * 1024)
#define ANSWERBOOK_PREVIEW_BLOCK_PX         10
#define ANSWERBOOK_JPEG_BLOCK_PX            16
#define ANSWERBOOK_JPEG_QUALITY             16

typedef enum {
  ANSWERBOOK_VIEW_MENU = 0,
  ANSWERBOOK_VIEW_CAMERA,
  ANSWERBOOK_VIEW_BUSY,
  ANSWERBOOK_VIEW_RESULT,
} AnswerBookView;

static lv_obj_t *s_screenAnswerbook = nullptr;
static lv_obj_t *s_titleLabel = nullptr;
static lv_obj_t *s_optionLabels[2] = {nullptr, nullptr};
static lv_obj_t *s_answerLabel = nullptr;
static lv_obj_t *s_hintLabel = nullptr;
static lv_obj_t *s_previewFrame = nullptr;
static lv_obj_t *s_previewCanvas = nullptr;
static lv_color_t *s_previewBuf = nullptr;

static AnswerBookView s_view = ANSWERBOOK_VIEW_MENU;
static uint8_t s_selectedOption = 0;
static bool s_busy = false;
static TaskHandle_t s_askTask = nullptr;
static TaskHandle_t s_previewTask = nullptr;
static SemaphoreHandle_t s_cameraMutex = nullptr;
static portMUX_TYPE s_resultMux = portMUX_INITIALIZER_UNLOCKED;
static bool s_visible = false;
static bool s_resultReady = false;
static OracleResult s_resultCode = ORACLE_RESULT_HTTP_FAIL;
static char s_resultText[64];
static bool s_previewEnabled = false;
static bool s_previewFrozen = false;
static bool s_previewHasFrame = false;
static uint32_t s_previewSeq = 0;
static uint32_t s_previewAppliedSeq = 0;
static uint8_t s_previewBits[CAMERA_PREVIEW_BYTES];
static char s_currentAnswer[64];
static char s_currentSource[32];

static void answerbookPreviewTask(void *param);
static void ensurePreviewTask(void);

static void styleLabel(lv_obj_t *label, const lv_font_t *font) {
  lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_text_opa(label, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(label, 0, LV_PART_MAIN);
  if (font != nullptr) {
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  }
}

static void setHidden(lv_obj_t *obj, bool hidden) {
  if (obj == nullptr) {
    return;
  }
  if (hidden) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
  }
}

static void setPreviewState(bool enabled, bool frozen) {
  portENTER_CRITICAL(&s_resultMux);
  s_previewEnabled = enabled;
  s_previewFrozen = frozen;
  portEXIT_CRITICAL(&s_resultMux);
}

static bool previewCaptureAllowed(void) {
  bool allowed = false;
  portENTER_CRITICAL(&s_resultMux);
  allowed = s_visible && s_previewEnabled && !s_previewFrozen && !s_busy &&
            s_view == ANSWERBOOK_VIEW_CAMERA;
  portEXIT_CRITICAL(&s_resultMux);
  return allowed;
}

static bool ensureCameraMutex(void) {
  if (s_cameraMutex != nullptr) {
    return true;
  }
  s_cameraMutex = xSemaphoreCreateMutex();
  if (s_cameraMutex == nullptr) {
    Serial.println("[AnswerBook] camera mutex create failed");
    return false;
  }
  return true;
}

static bool ensurePreviewBuffer(void) {
  if (s_previewBuf != nullptr) {
    return true;
  }

  const size_t bufSize =
      sizeof(lv_color_t) * CAMERA_PREVIEW_WIDTH * CAMERA_PREVIEW_HEIGHT;
  s_previewBuf = static_cast<lv_color_t *>(
      heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (s_previewBuf == nullptr) {
    s_previewBuf = static_cast<lv_color_t *>(
        heap_caps_malloc(bufSize, MALLOC_CAP_8BIT));
  }
  if (s_previewBuf == nullptr) {
    Serial.printf("[AnswerBook] preview buffer alloc failed size=%u\r\n",
                  (unsigned)bufSize);
    return false;
  }
  return true;
}

static void clearPreviewCanvas(void) {
  if (s_previewCanvas == nullptr) {
    return;
  }
  for (int y = 0; y < CAMERA_PREVIEW_HEIGHT; y++) {
    for (int x = 0; x < CAMERA_PREVIEW_WIDTH; x++) {
      lv_canvas_set_px(s_previewCanvas, x, y, lv_color_white());
    }
  }
  lv_obj_invalidate(s_previewCanvas);
}

static void applyPreviewBits(const uint8_t *bits) {
  if (bits == nullptr || s_previewCanvas == nullptr) {
    return;
  }
  for (int y = 0; y < CAMERA_PREVIEW_HEIGHT; y++) {
    for (int x = 0; x < CAMERA_PREVIEW_WIDTH; x++) {
      const int bitIndex = y * CAMERA_PREVIEW_WIDTH + x;
      const bool white =
          (bits[bitIndex / 8] & (0x80 >> (bitIndex % 8))) != 0;
      lv_canvas_set_px(s_previewCanvas, x, y,
                       white ? lv_color_white() : lv_color_black());
    }
  }
  lv_obj_invalidate(s_previewCanvas);
  if (s_previewFrame != nullptr) {
    lv_obj_invalidate(s_previewFrame);
  }
}

static void storePreviewBits(const uint8_t *bits, bool alreadyApplied) {
  if (bits == nullptr) {
    return;
  }
  portENTER_CRITICAL(&s_resultMux);
  memcpy(s_previewBits, bits, CAMERA_PREVIEW_BYTES);
  s_previewHasFrame = true;
  s_previewSeq++;
  if (alreadyApplied) {
    s_previewAppliedSeq = s_previewSeq;
  }
  portEXIT_CRITICAL(&s_resultMux);
}

static bool consumePreviewFrame(void) {
  uint8_t bits[CAMERA_PREVIEW_BYTES];
  bool hasFrame = false;

  portENTER_CRITICAL(&s_resultMux);
  if (s_previewHasFrame && s_previewSeq != s_previewAppliedSeq) {
    memcpy(bits, s_previewBits, CAMERA_PREVIEW_BYTES);
    s_previewAppliedSeq = s_previewSeq;
    hasFrame = true;
  }
  portEXIT_CRITICAL(&s_resultMux);

  if (!hasFrame) {
    return false;
  }
  applyPreviewBits(bits);
  return true;
}

static const char *answerbookErrorText(OracleResult result) {
  switch (result) {
    case ORACLE_RESULT_NO_CAMERA:
      return app_tr(TR_VISION_NO_CAMERA);
    case ORACLE_RESULT_NO_WIFI:
      return app_tr(TR_VISION_NO_WIFI);
    case ORACLE_RESULT_NO_API:
      return app_tr(TR_VISION_NO_API);
    case ORACLE_RESULT_UNSUPPORTED:
      return app_tr(TR_VISION_UNSUPPORTED);
    case ORACLE_RESULT_CAPTURE_FAIL:
      return "读图失败";
    case ORACLE_RESULT_PARSE_FAIL:
      return "解析失败";
    default:
      return app_tr(TR_VISION_FAIL);
  }
}

static void showMenu(void) {
  s_view = ANSWERBOOK_VIEW_MENU;
  setPreviewState(false, false);
  setHidden(s_previewFrame, true);
  setHidden(s_answerLabel, true);
  setHidden(s_optionLabels[0], false);
  setHidden(s_optionLabels[1], false);

  lv_label_set_text(s_titleLabel, app_tr(TR_ANSWERBOOK_TITLE));
  char line0[32];
  char line1[32];
  snprintf(line0, sizeof(line0), "%s拍图问事", s_selectedOption == 0 ? "> " : "  ");
  snprintf(line1, sizeof(line1), "%s随缘启动", s_selectedOption == 1 ? "> " : "  ");
  lv_label_set_text(s_optionLabels[0], line0);
  lv_label_set_text(s_optionLabels[1], line1);
  lv_label_set_text(s_hintLabel, "A next  B confirm");
  lv_obj_invalidate(s_screenAnswerbook);
}

static void showCamera(void) {
  ensurePreviewTask();
  s_view = ANSWERBOOK_VIEW_CAMERA;
  setPreviewState(true, false);
  setHidden(s_optionLabels[0], true);
  setHidden(s_optionLabels[1], true);
  setHidden(s_answerLabel, true);
  setHidden(s_previewFrame, false);

  lv_label_set_text(s_titleLabel, "拍图问事");
  lv_label_set_text(s_hintLabel, "短按A拍摄");
  lv_obj_invalidate(s_screenAnswerbook);
}

static void showBusy(void) {
  s_view = ANSWERBOOK_VIEW_BUSY;
  setPreviewState(true, true);
  setHidden(s_optionLabels[0], true);
  setHidden(s_optionLabels[1], true);
  setHidden(s_answerLabel, true);
  setHidden(s_previewFrame, false);

  lv_label_set_text(s_titleLabel, "拍图问事");
  lv_label_set_text(s_hintLabel, "少女祈福中");
  lv_obj_invalidate(s_screenAnswerbook);
}

static void showResult(const char *answer, const char *source) {
  s_view = ANSWERBOOK_VIEW_RESULT;
  setPreviewState(false, false);
  setHidden(s_previewFrame, true);
  setHidden(s_optionLabels[0], true);
  setHidden(s_optionLabels[1], true);
  setHidden(s_answerLabel, false);

  snprintf(s_currentAnswer, sizeof(s_currentAnswer), "%s",
           answer != nullptr ? answer : "--");
  snprintf(s_currentSource, sizeof(s_currentSource), "%s",
           source != nullptr ? source : "");
  lv_label_set_text(s_titleLabel, app_tr(TR_ANSWERBOOK_TITLE));
  lv_label_set_text(s_answerLabel, s_currentAnswer);
  lv_label_set_text(s_hintLabel, s_currentSource);
  lv_obj_invalidate(s_screenAnswerbook);
}

static void renderCurrentView(void) {
  switch (s_view) {
    case ANSWERBOOK_VIEW_CAMERA:
      showCamera();
      break;
    case ANSWERBOOK_VIEW_BUSY:
      showBusy();
      break;
    case ANSWERBOOK_VIEW_RESULT:
      showResult(s_currentAnswer, s_currentSource);
      break;
    case ANSWERBOOK_VIEW_MENU:
    default:
      showMenu();
      break;
  }
}

static void answerbookPreviewTask(void *param) {
  (void)param;

  uint8_t bits[CAMERA_PREVIEW_BYTES];
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(ANSWERBOOK_PREVIEW_MS));

    if (!previewCaptureAllowed() || s_cameraMutex == nullptr) {
      continue;
    }
    if (xSemaphoreTake(s_cameraMutex, pdMS_TO_TICKS(30)) != pdTRUE) {
      continue;
    }

    bool ok = false;
    if (previewCaptureAllowed() &&
        (camera_service_is_ready() || camera_service_init())) {
      camera_fb_t *fb = camera_service_capture();
      if (fb != nullptr) {
        ok = camera_service_frame_to_mosaic_preview100(
            fb, bits, sizeof(bits), ANSWERBOOK_PREVIEW_BLOCK_PX);
        camera_service_release(fb);
      }
    }
    xSemaphoreGive(s_cameraMutex);

    if (ok) {
      storePreviewBits(bits, false);
    }
  }
}

static void ensurePreviewTask(void) {
  if (s_previewTask != nullptr) {
    return;
  }
  if (!ensureCameraMutex()) {
    return;
  }
  if (xTaskCreate(answerbookPreviewTask, "abprev",
                  ANSWERBOOK_PREVIEW_TASK_STACK_BYTES, nullptr, 1,
                  &s_previewTask) != pdPASS) {
    s_previewTask = nullptr;
    Serial.println("[AnswerBook] preview task create failed");
  }
}

static OracleResult captureMosaicForAi(uint8_t **outJpeg, size_t *outLen) {
  if (outJpeg == nullptr || outLen == nullptr) {
    return ORACLE_RESULT_CAPTURE_FAIL;
  }
  *outJpeg = nullptr;
  *outLen = 0;

  if (!ensureCameraMutex()) {
    return ORACLE_RESULT_CAPTURE_FAIL;
  }
  if (xSemaphoreTake(s_cameraMutex, pdMS_TO_TICKS(1600)) != pdTRUE) {
    Serial.println("[AnswerBook] camera busy while capture");
    return ORACLE_RESULT_CAPTURE_FAIL;
  }

  OracleResult status = ORACLE_RESULT_CAPTURE_FAIL;
  camera_fb_t *fb = nullptr;

  if (!camera_service_is_ready() && !camera_service_init()) {
    status = ORACLE_RESULT_NO_CAMERA;
  } else {
    camera_fb_t *warmup = camera_service_capture();
    if (warmup != nullptr) {
      camera_service_release(warmup);
    }

    fb = camera_service_capture();
    if (fb == nullptr || fb->len == 0 ||
        fb->len > ANSWERBOOK_CAPTURE_MAX_JPEG_BYTES) {
      Serial.printf("[AnswerBook] capture fail fb=%p len=%u\r\n",
                    (void *)fb, fb != nullptr ? (unsigned)fb->len : 0U);
      status = ORACLE_RESULT_CAPTURE_FAIL;
    } else {
      if (!camera_service_frame_to_mosaic_jpeg(
              fb, ANSWERBOOK_JPEG_BLOCK_PX, ANSWERBOOK_JPEG_QUALITY,
              outJpeg, outLen)) {
        Serial.println("[AnswerBook] mosaic jpeg failed");
        status = ORACLE_RESULT_CAPTURE_FAIL;
      } else if (*outLen == 0 || *outLen > ANSWERBOOK_CAPTURE_MAX_JPEG_BYTES) {
        Serial.printf("[AnswerBook] mosaic jpeg invalid len=%u\r\n",
                      (unsigned)*outLen);
        free(*outJpeg);
        *outJpeg = nullptr;
        *outLen = 0;
        status = ORACLE_RESULT_CAPTURE_FAIL;
      } else {
        Serial.printf("[AnswerBook] mosaic JPEG %u bytes\r\n",
                      (unsigned)*outLen);
        status = ORACLE_RESULT_OK;
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

static void answerbookAskTask(void *param) {
  (void)param;

  uint8_t *jpeg = nullptr;
  size_t jpegLen = 0;
  char result[64];
  result[0] = '\0';

  OracleResult code = captureMosaicForAi(&jpeg, &jpegLen);
  if (code == ORACLE_RESULT_OK && jpeg != nullptr && jpegLen > 0) {
    code = oracle_service_ask_jpeg(jpeg, jpegLen, result, sizeof(result));
  }
  free(jpeg);

  portENTER_CRITICAL(&s_resultMux);
  s_resultCode = code;
  snprintf(s_resultText, sizeof(s_resultText), "%s", result);
  s_resultReady = true;
  s_busy = false;
  s_askTask = nullptr;
  portEXIT_CRITICAL(&s_resultMux);

  vTaskDelete(nullptr);
}

static bool startPhotoCapture(void) {
  portENTER_CRITICAL(&s_resultMux);
  if (s_busy) {
    portEXIT_CRITICAL(&s_resultMux);
    return false;
  }
  s_busy = true;
  s_resultReady = false;
  portEXIT_CRITICAL(&s_resultMux);

  showBusy();

  if (xTaskCreate(answerbookAskTask, "oracle", ANSWERBOOK_TASK_STACK_BYTES,
                  nullptr, 1, &s_askTask) != pdPASS) {
    portENTER_CRITICAL(&s_resultMux);
    s_busy = false;
    s_askTask = nullptr;
    portEXIT_CRITICAL(&s_resultMux);
    showResult(app_tr(TR_VISION_FAIL), "拍图问事");
    Serial.println("[AnswerBook] task create failed");
    return true;
  }

  return true;
}

void ui_answerbook_init(void) {
  s_screenAnswerbook = lv_obj_create(nullptr);
  lv_obj_set_size(s_screenAnswerbook, 200, 180);
  lv_obj_set_style_bg_color(s_screenAnswerbook, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_screenAnswerbook, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_screenAnswerbook, LV_OBJ_FLAG_SCROLLABLE);

  s_titleLabel = lv_label_create(s_screenAnswerbook);
  styleLabel(s_titleLabel, UI_FONT_MD);
  lv_obj_set_style_text_align(s_titleLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(s_titleLabel, LV_ALIGN_TOP_MID, 0, 8);

  for (int i = 0; i < 2; i++) {
    s_optionLabels[i] = lv_label_create(s_screenAnswerbook);
    styleLabel(s_optionLabels[i], UI_FONT_MD);
    lv_obj_set_style_text_align(s_optionLabels[i], LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_width(s_optionLabels[i], 150);
    lv_label_set_long_mode(s_optionLabels[i], LV_LABEL_LONG_DOT);
    lv_obj_align(s_optionLabels[i], LV_ALIGN_TOP_MID, 0, 58 + i * 34);
  }

  s_answerLabel = lv_label_create(s_screenAnswerbook);
  styleLabel(s_answerLabel, UI_FONT_MD);
  lv_obj_set_style_text_align(s_answerLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_width(s_answerLabel, 188);
  lv_label_set_long_mode(s_answerLabel, LV_LABEL_LONG_WRAP);
  lv_obj_align(s_answerLabel, LV_ALIGN_CENTER, 0, -4);

  s_previewFrame = lv_obj_create(s_screenAnswerbook);
  lv_obj_set_size(s_previewFrame, ANSWERBOOK_PREVIEW_FRAME_SIZE,
                  ANSWERBOOK_PREVIEW_FRAME_SIZE);
  lv_obj_set_pos(s_previewFrame, ANSWERBOOK_PREVIEW_FRAME_X,
                 ANSWERBOOK_PREVIEW_FRAME_Y);
  lv_obj_set_style_bg_color(s_previewFrame, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_previewFrame, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(s_previewFrame, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_border_width(s_previewFrame, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(s_previewFrame, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_previewFrame, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(s_previewFrame, 0, LV_PART_MAIN);
  lv_obj_clear_flag(s_previewFrame, LV_OBJ_FLAG_SCROLLABLE);

  if (ensurePreviewBuffer()) {
    s_previewCanvas = lv_canvas_create(s_previewFrame);
    lv_canvas_set_buffer(s_previewCanvas, s_previewBuf,
                         CAMERA_PREVIEW_WIDTH, CAMERA_PREVIEW_HEIGHT,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(s_previewCanvas, 1, 1);
    clearPreviewCanvas();
  }

  s_hintLabel = lv_label_create(s_screenAnswerbook);
  styleLabel(s_hintLabel, UI_FONT_SM);
  lv_obj_set_style_text_align(s_hintLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_width(s_hintLabel, 188);
  lv_label_set_long_mode(s_hintLabel, LV_LABEL_LONG_DOT);
  lv_obj_align(s_hintLabel, LV_ALIGN_BOTTOM_MID, 0, -8);

  showMenu();
}

void ui_answerbook_show(void) {
  portENTER_CRITICAL(&s_resultMux);
  s_visible = true;
  portEXIT_CRITICAL(&s_resultMux);
  if (ui_answerbook_is_busy()) {
    showBusy();
  } else {
    showMenu();
  }
  lv_scr_load(s_screenAnswerbook);
  lv_obj_invalidate(s_screenAnswerbook);
}

void ui_answerbook_leave(void) {
  portENTER_CRITICAL(&s_resultMux);
  s_visible = false;
  portEXIT_CRITICAL(&s_resultMux);
  setPreviewState(false, false);
  if (!ui_answerbook_is_busy()) {
    if (s_cameraMutex != nullptr &&
        xSemaphoreTake(s_cameraMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
      camera_service_pause();
      xSemaphoreGive(s_cameraMutex);
    } else if (s_cameraMutex == nullptr) {
      camera_service_pause();
    }
  }
}

void ui_answerbook_refresh(void) {
  if (!ui_answerbook_is_active()) {
    return;
  }
  renderCurrentView();
}

void ui_answerbook_refresh_locale(void) {
  if (!ui_answerbook_is_active()) {
    return;
  }
  renderCurrentView();
}

bool ui_answerbook_is_active(void) {
  return s_screenAnswerbook != nullptr &&
         lv_scr_act() == s_screenAnswerbook;
}

bool ui_answerbook_is_busy(void) {
  portENTER_CRITICAL(&s_resultMux);
  const bool busy = s_busy;
  portEXIT_CRITICAL(&s_resultMux);
  return busy;
}

bool ui_answerbook_next(UiRefreshMode *outRefreshMode) {
  if (outRefreshMode != nullptr) {
    *outRefreshMode = UI_REFRESH_NONE;
  }
  if (ui_answerbook_is_busy()) {
    return false;
  }

  if (s_view == ANSWERBOOK_VIEW_MENU) {
    s_selectedOption = (s_selectedOption + 1U) % 2U;
    showMenu();
    if (outRefreshMode != nullptr) {
      *outRefreshMode = UI_REFRESH_FAST;
    }
    return true;
  }
  if (s_view == ANSWERBOOK_VIEW_CAMERA) {
    const bool started = startPhotoCapture();
    if (started && outRefreshMode != nullptr) {
      *outRefreshMode = UI_REFRESH_NAV;
    }
    return started;
  }
  if (s_view == ANSWERBOOK_VIEW_RESULT) {
    showMenu();
    if (outRefreshMode != nullptr) {
      *outRefreshMode = UI_REFRESH_NAV;
    }
    return true;
  }
  return false;
}

bool ui_answerbook_confirm(UiRefreshMode *outRefreshMode) {
  if (outRefreshMode != nullptr) {
    *outRefreshMode = UI_REFRESH_NONE;
  }
  if (ui_answerbook_is_busy()) {
    return false;
  }

  if (s_view == ANSWERBOOK_VIEW_MENU) {
    if (s_selectedOption == 0) {
      showCamera();
    } else {
      showResult(bookofanswers_random_timed(), app_tr(TR_ANSWERBOOK_LOCAL_SOURCE));
    }
    if (outRefreshMode != nullptr) {
      *outRefreshMode = UI_REFRESH_NAV;
    }
    return true;
  }
  if (s_view == ANSWERBOOK_VIEW_RESULT) {
    showMenu();
    if (outRefreshMode != nullptr) {
      *outRefreshMode = UI_REFRESH_NAV;
    }
    return true;
  }
  return false;
}

bool ui_answerbook_request(void) {
  return ui_answerbook_confirm(nullptr);
}

bool ui_answerbook_service(UiRefreshMode *outRefreshMode) {
  if (outRefreshMode != nullptr) {
    *outRefreshMode = UI_REFRESH_NONE;
  }
  if (s_screenAnswerbook == nullptr) {
    return false;
  }

  bool done = false;
  OracleResult code = ORACLE_RESULT_HTTP_FAIL;
  char result[64];
  result[0] = '\0';

  portENTER_CRITICAL(&s_resultMux);
  if (s_resultReady) {
    done = true;
    code = s_resultCode;
    snprintf(result, sizeof(result), "%s", s_resultText);
    s_resultReady = false;
  }
  portEXIT_CRITICAL(&s_resultMux);

  if (done) {
    if (code == ORACLE_RESULT_OK) {
      showResult(result, app_tr(TR_ANSWERBOOK_AI_SOURCE));
    } else {
      showResult(answerbookErrorText(code), "拍图问事");
    }
    if (outRefreshMode != nullptr) {
      *outRefreshMode = UI_REFRESH_NAV;
    }
    Serial.printf("[AnswerBook] pipeline done code=%d\r\n", (int)code);
    return ui_answerbook_is_active();
  }

  if (ui_answerbook_is_active() && s_view == ANSWERBOOK_VIEW_CAMERA &&
      !ui_answerbook_is_busy()) {
    if (!consumePreviewFrame()) {
      return false;
    }
    if (outRefreshMode != nullptr) {
      *outRefreshMode = UI_REFRESH_FAST;
    }
    return ui_answerbook_is_active();
  }

  return false;
}

lv_obj_t *ui_answerbook_get_screen(void) {
  return s_screenAnswerbook;
}
