#include "ui_answers.h"

#include "app_locale.h"
#include "bookofanswers.h"
#include "camera_service.h"
#include "ui_fonts.h"
#include "voice_service.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#define ANSWERS_TASK_STACK_BYTES         16384
#define ANSWERS_PREVIEW_TASK_STACK_BYTES 12288
#define ANSWERS_PREVIEW_MS               1200U
#define ANSWERS_PREVIEW_FRAME_X          49
#define ANSWERS_PREVIEW_FRAME_Y          55
#define ANSWERS_PREVIEW_FRAME_SIZE       102
#define ANSWERS_CAPTURE_MAX_JPEG_BYTES   (64 * 1024)
#define ANSWERS_PREVIEW_BLOCK_PX         7
#define ANSWERS_MODE_COUNT               3
#define ANSWERS_VOICE_RECORD_MS          2600U

typedef enum {
  ANSWERS_VIEW_MENU = 0,
  ANSWERS_VIEW_CAMERA,
  ANSWERS_VIEW_BUSY,
  ANSWERS_VIEW_RESULT,
} AnswersView;

typedef enum {
  ANSWERS_MODE_CAMERA = 0,
  ANSWERS_MODE_VOICE,
  ANSWERS_MODE_AUTO,
} AnswersMode;

typedef enum {
  ANSWERS_LOCAL_OK = 0,
  ANSWERS_LOCAL_CAMERA_FAIL,
  ANSWERS_LOCAL_MIC_FAIL,
  ANSWERS_LOCAL_TASK_FAIL,
} AnswersLocalResult;

static lv_obj_t *s_screenAnswers = nullptr;
static lv_obj_t *s_titleLabel = nullptr;
static lv_obj_t *s_optionLabels[ANSWERS_MODE_COUNT] = {nullptr, nullptr, nullptr};
static lv_obj_t *s_answerLabel = nullptr;
static lv_obj_t *s_answerSubLabel = nullptr;
static lv_obj_t *s_hintLabel = nullptr;
static lv_obj_t *s_previewFrame = nullptr;
static lv_obj_t *s_previewCanvas = nullptr;
static lv_color_t *s_previewBuf = nullptr;

static AnswersView s_view = ANSWERS_VIEW_MENU;
static AnswersMode s_selectedMode = ANSWERS_MODE_CAMERA;
static AnswersMode s_activeMode = ANSWERS_MODE_AUTO;
static bool s_busy = false;
static TaskHandle_t s_askTask = nullptr;
static TaskHandle_t s_previewTask = nullptr;
static portMUX_TYPE s_resultMux = portMUX_INITIALIZER_UNLOCKED;
static bool s_visible = false;
static bool s_resultReady = false;
static AnswersLocalResult s_resultCode = ANSWERS_LOCAL_TASK_FAIL;
static char s_resultMain[40];
static char s_resultSub[96];
static uint16_t s_resultNumber = 0;
static bool s_previewEnabled = false;
static bool s_previewFrozen = false;
static bool s_previewHasFrame = false;
static uint32_t s_previewSeq = 0;
static uint32_t s_previewAppliedSeq = 0;
static uint8_t s_previewBits[CAMERA_PREVIEW_BYTES];
static char s_currentMain[40];
static char s_currentSub[96];
static char s_currentFooter[40];

static void answersPreviewTask(void *param);
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

static const char *answersModeTitle(AnswersMode mode) {
  switch (mode) {
    case ANSWERS_MODE_CAMERA:
      return "观象问卦";
    case ANSWERS_MODE_VOICE:
      return "开口问天";
    case ANSWERS_MODE_AUTO:
    default:
      return "天机自动";
  }
}

static const char *answersBusyText(AnswersMode mode) {
  switch (mode) {
    case ANSWERS_MODE_CAMERA:
      return "正在观察宇宙表情";
    case ANSWERS_MODE_VOICE:
      return "正在偷听命运碎碎念";
    case ANSWERS_MODE_AUTO:
    default:
      return "天机转圈中";
  }
}

static const char *answersErrorText(AnswersLocalResult result) {
  switch (result) {
    case ANSWERS_LOCAL_CAMERA_FAIL:
      return "相机闭眼了";
    case ANSWERS_LOCAL_MIC_FAIL:
      return "麦克风装睡";
    case ANSWERS_LOCAL_TASK_FAIL:
    default:
      return "天机卡壳";
  }
}

static void formatAnswerFooter(AnswersMode mode, uint16_t number, char *out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return;
  }
  snprintf(out, outLen, "%s  #%03u", answersModeTitle(mode), (unsigned)number);
}

static void copyPickedAnswer(uint32_t seed, AnswersMode mode,
                             char *outMain, size_t mainLen,
                             char *outSub, size_t subLen,
                             char *outFooter, size_t footerLen,
                             uint16_t *outNumber) {
  uint16_t number = 0;
  const BookAnswer *answer = bookofanswers_pick(seed, &number);
  if (outMain != nullptr && mainLen > 0) {
    snprintf(outMain, mainLen, "【%s】", answer->mainText);
  }
  if (outSub != nullptr && subLen > 0) {
    snprintf(outSub, subLen, "%s", answer->subText);
  }
  formatAnswerFooter(mode, number, outFooter, footerLen);
  if (outNumber != nullptr) {
    *outNumber = number;
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
            s_view == ANSWERS_VIEW_CAMERA;
  portEXIT_CRITICAL(&s_resultMux);
  return allowed;
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
    Serial.printf("[Answers] preview buffer alloc failed size=%u\r\n",
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

static void showMenu(void) {
  s_view = ANSWERS_VIEW_MENU;
  setPreviewState(false, false);
  setHidden(s_previewFrame, true);
  setHidden(s_answerLabel, true);
  setHidden(s_answerSubLabel, true);
  for (int i = 0; i < ANSWERS_MODE_COUNT; i++) {
    setHidden(s_optionLabels[i], false);
  }

  lv_label_set_text(s_titleLabel, app_tr(TR_ANSWERBOOK_TITLE));
  for (int i = 0; i < ANSWERS_MODE_COUNT; i++) {
    char line[48];
    snprintf(line, sizeof(line), "%s%s",
             s_selectedMode == (AnswersMode)i ? "> " : "  ",
             answersModeTitle((AnswersMode)i));
    lv_label_set_text(s_optionLabels[i], line);
  }
  lv_label_set_text(s_hintLabel, "A切换  B启动  长按A返回");
  lv_obj_invalidate(s_screenAnswers);
}

static void showCamera(void) {
  ensurePreviewTask();
  s_view = ANSWERS_VIEW_CAMERA;
  setPreviewState(true, false);
  for (int i = 0; i < ANSWERS_MODE_COUNT; i++) {
    setHidden(s_optionLabels[i], true);
  }
  setHidden(s_answerLabel, true);
  setHidden(s_answerSubLabel, true);
  setHidden(s_previewFrame, false);

  lv_label_set_text(s_titleLabel, answersModeTitle(ANSWERS_MODE_CAMERA));
  lv_label_set_text(s_hintLabel, "A/B拍摄，表情不用太庄严");
  lv_obj_invalidate(s_screenAnswers);
}

static void showBusy(AnswersMode mode) {
  s_view = ANSWERS_VIEW_BUSY;
  setPreviewState(mode == ANSWERS_MODE_CAMERA, true);
  for (int i = 0; i < ANSWERS_MODE_COUNT; i++) {
    setHidden(s_optionLabels[i], true);
  }
  setHidden(s_answerLabel, true);
  setHidden(s_answerSubLabel, true);
  setHidden(s_previewFrame, mode != ANSWERS_MODE_CAMERA);

  lv_label_set_text(s_titleLabel, answersModeTitle(mode));
  lv_label_set_text(s_hintLabel, answersBusyText(mode));
  lv_obj_invalidate(s_screenAnswers);
}

static void showResult(const char *mainText, const char *subText, const char *footer) {
  s_view = ANSWERS_VIEW_RESULT;
  setPreviewState(false, false);
  setHidden(s_previewFrame, true);
  for (int i = 0; i < ANSWERS_MODE_COUNT; i++) {
    setHidden(s_optionLabels[i], true);
  }
  setHidden(s_answerLabel, false);
  setHidden(s_answerSubLabel, false);

  snprintf(s_currentMain, sizeof(s_currentMain), "%s",
           mainText != nullptr ? mainText : "【未知】");
  snprintf(s_currentSub, sizeof(s_currentSub), "%s",
           subText != nullptr ? subText : "宇宙刚才走神了");
  snprintf(s_currentFooter, sizeof(s_currentFooter), "%s",
           footer != nullptr ? footer : "");
  lv_label_set_text(s_titleLabel, app_tr(TR_ANSWERBOOK_TITLE));
  lv_label_set_text(s_answerLabel, s_currentMain);
  lv_label_set_text(s_answerSubLabel, s_currentSub);
  lv_label_set_text(s_hintLabel, s_currentFooter);
  lv_obj_invalidate(s_screenAnswers);
}

static void renderCurrentView(void) {
  switch (s_view) {
    case ANSWERS_VIEW_CAMERA:
      showCamera();
      break;
    case ANSWERS_VIEW_BUSY:
      showBusy(s_activeMode);
      break;
    case ANSWERS_VIEW_RESULT:
      showResult(s_currentMain, s_currentSub, s_currentFooter);
      break;
    case ANSWERS_VIEW_MENU:
    default:
      showMenu();
      break;
  }
}

static void answersPreviewTask(void *param) {
  (void)param;

  uint8_t bits[CAMERA_PREVIEW_BYTES];
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(ANSWERS_PREVIEW_MS));

    if (!previewCaptureAllowed()) {
      continue;
    }
    if (!camera_service_lock(30)) {
      continue;
    }

    bool ok = false;
    if (previewCaptureAllowed() &&
        (camera_service_is_ready() || camera_service_init())) {
      camera_fb_t *fb = camera_service_capture();
      if (fb != nullptr) {
        ok = camera_service_frame_to_mosaic_preview100(
            fb, bits, sizeof(bits), ANSWERS_PREVIEW_BLOCK_PX);
        camera_service_release(fb);
      }
    }
    camera_service_unlock();

    if (ok) {
      storePreviewBits(bits, false);
    }
  }
}

static void ensurePreviewTask(void) {
  if (s_previewTask != nullptr) {
    return;
  }
  if (xTaskCreate(answersPreviewTask, "abprev",
                  ANSWERS_PREVIEW_TASK_STACK_BYTES, nullptr, 1,
                  &s_previewTask) != pdPASS) {
    s_previewTask = nullptr;
    Serial.println("[Answers] preview task create failed");
  }
}

static AnswersLocalResult captureImageSeed(uint32_t *outSeed) {
  if (outSeed == nullptr) {
    return ANSWERS_LOCAL_CAMERA_FAIL;
  }
  if (!camera_service_lock(1600)) {
    Serial.println("[Answers] camera busy while capture");
    return ANSWERS_LOCAL_CAMERA_FAIL;
  }

  AnswersLocalResult status = ANSWERS_LOCAL_CAMERA_FAIL;
  camera_fb_t *fb = nullptr;

  if (!camera_service_is_ready() && !camera_service_init()) {
    status = ANSWERS_LOCAL_CAMERA_FAIL;
  } else {
    camera_fb_t *warmup = camera_service_capture();
    if (warmup != nullptr) {
      camera_service_release(warmup);
    }

    fb = camera_service_capture();
    if (fb == nullptr || fb->len == 0 ||
        fb->len > ANSWERS_CAPTURE_MAX_JPEG_BYTES) {
      Serial.printf("[Answers] capture fail fb=%p len=%u\r\n",
                    (void *)fb, fb != nullptr ? (unsigned)fb->len : 0U);
      status = ANSWERS_LOCAL_CAMERA_FAIL;
    } else {
      *outSeed = bookofanswers_seed_from_image_bytes(fb->buf, fb->len);
      status = ANSWERS_LOCAL_OK;
      Serial.printf("[Answers] image seed from JPEG %u bytes\r\n", (unsigned)fb->len);
    }
  }

  if (fb != nullptr) {
    camera_service_release(fb);
  }
  camera_service_pause();
  camera_service_unlock();
  return status;
}

static AnswersLocalResult buildLocalAnswer(AnswersMode mode,
                                           char *outMain, size_t mainLen,
                                           char *outSub, size_t subLen,
                                           char *outFooter, size_t footerLen,
                                           uint16_t *outNumber) {
  uint32_t seed = 0;
  AnswersLocalResult code = ANSWERS_LOCAL_OK;

  if (mode == ANSWERS_MODE_CAMERA) {
    code = captureImageSeed(&seed);
  } else if (mode == ANSWERS_MODE_VOICE) {
    if (!voice_service_capture_local_seed(ANSWERS_VOICE_RECORD_MS, &seed)) {
      code = ANSWERS_LOCAL_MIC_FAIL;
    }
  } else {
    seed = bookofanswers_seed_auto();
  }

  if (code != ANSWERS_LOCAL_OK) {
    snprintf(outMain, mainLen, "【%s】", answersErrorText(code));
    snprintf(outSub, subLen, "本地宇宙刚刚咳了一声");
    formatAnswerFooter(mode, 0, outFooter, footerLen);
    if (outNumber != nullptr) {
      *outNumber = 0;
    }
    return code;
  }

  copyPickedAnswer(seed, mode, outMain, mainLen, outSub, subLen,
                   outFooter, footerLen, outNumber);
  return ANSWERS_LOCAL_OK;
}

static void answersAskTask(void *param) {
  const AnswersMode mode = (AnswersMode)(uintptr_t)param;

  char mainText[40];
  char subText[96];
  char footer[40];
  uint16_t number = 0;
  mainText[0] = '\0';
  subText[0] = '\0';
  footer[0] = '\0';

  AnswersLocalResult code = buildLocalAnswer(mode,
                                             mainText, sizeof(mainText),
                                             subText, sizeof(subText),
                                             footer, sizeof(footer),
                                             &number);

  portENTER_CRITICAL(&s_resultMux);
  s_resultCode = code;
  snprintf(s_resultMain, sizeof(s_resultMain), "%s", mainText);
  snprintf(s_resultSub, sizeof(s_resultSub), "%s", subText);
  s_resultNumber = number;
  snprintf(s_currentFooter, sizeof(s_currentFooter), "%s", footer);
  s_resultReady = true;
  s_busy = false;
  s_askTask = nullptr;
  portEXIT_CRITICAL(&s_resultMux);

  vTaskDelete(nullptr);
}

static bool startLocalAsk(AnswersMode mode) {
  portENTER_CRITICAL(&s_resultMux);
  if (s_busy) {
    portEXIT_CRITICAL(&s_resultMux);
    return false;
  }
  s_busy = true;
  s_resultReady = false;
  portEXIT_CRITICAL(&s_resultMux);

  s_activeMode = mode;
  showBusy(mode);

  if (xTaskCreate(answersAskTask, "answers", ANSWERS_TASK_STACK_BYTES,
                  (void *)(uintptr_t)mode, 1, &s_askTask) != pdPASS) {
    portENTER_CRITICAL(&s_resultMux);
    s_busy = false;
    s_askTask = nullptr;
    portEXIT_CRITICAL(&s_resultMux);
    char footer[40];
    formatAnswerFooter(mode, 0, footer, sizeof(footer));
    showResult("【天机卡壳】", "任务没开起来，稍后再问", footer);
    Serial.println("[Answers] task create failed");
    return true;
  }

  return true;
}

void ui_answers_init(void) {
  s_screenAnswers = lv_obj_create(nullptr);
  lv_obj_set_size(s_screenAnswers, 200, 180);
  lv_obj_set_style_bg_color(s_screenAnswers, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_screenAnswers, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_screenAnswers, LV_OBJ_FLAG_SCROLLABLE);

  s_titleLabel = lv_label_create(s_screenAnswers);
  styleLabel(s_titleLabel, UI_FONT_MD);
  lv_obj_set_style_text_align(s_titleLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(s_titleLabel, LV_ALIGN_TOP_MID, 0, 8);

  for (int i = 0; i < ANSWERS_MODE_COUNT; i++) {
    s_optionLabels[i] = lv_label_create(s_screenAnswers);
    styleLabel(s_optionLabels[i], UI_FONT_MD);
    lv_obj_set_style_text_align(s_optionLabels[i], LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_width(s_optionLabels[i], 150);
    lv_label_set_long_mode(s_optionLabels[i], LV_LABEL_LONG_DOT);
    lv_obj_align(s_optionLabels[i], LV_ALIGN_TOP_MID, 0, 44 + i * 30);
  }

  s_answerLabel = lv_label_create(s_screenAnswers);
  styleLabel(s_answerLabel, UI_FONT_MD);
  lv_obj_set_style_text_align(s_answerLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_width(s_answerLabel, 188);
  lv_label_set_long_mode(s_answerLabel, LV_LABEL_LONG_WRAP);
  lv_obj_align(s_answerLabel, LV_ALIGN_CENTER, 0, -28);

  s_answerSubLabel = lv_label_create(s_screenAnswers);
  styleLabel(s_answerSubLabel, UI_FONT_SM);
  lv_obj_set_style_text_align(s_answerSubLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_width(s_answerSubLabel, 188);
  lv_label_set_long_mode(s_answerSubLabel, LV_LABEL_LONG_WRAP);
  lv_obj_align(s_answerSubLabel, LV_ALIGN_CENTER, 0, 18);

  s_previewFrame = lv_obj_create(s_screenAnswers);
  lv_obj_set_size(s_previewFrame, ANSWERS_PREVIEW_FRAME_SIZE,
                  ANSWERS_PREVIEW_FRAME_SIZE);
  lv_obj_set_pos(s_previewFrame, ANSWERS_PREVIEW_FRAME_X,
                 ANSWERS_PREVIEW_FRAME_Y);
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

  s_hintLabel = lv_label_create(s_screenAnswers);
  styleLabel(s_hintLabel, UI_FONT_SM);
  lv_obj_set_style_text_align(s_hintLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_width(s_hintLabel, 188);
  lv_label_set_long_mode(s_hintLabel, LV_LABEL_LONG_DOT);
  lv_obj_align(s_hintLabel, LV_ALIGN_BOTTOM_MID, 0, -8);

  showMenu();
}

void ui_answers_show(void) {
  portENTER_CRITICAL(&s_resultMux);
  s_visible = true;
  portEXIT_CRITICAL(&s_resultMux);
  if (ui_answers_is_busy()) {
    showBusy(s_activeMode);
  } else {
    showMenu();
  }
  lv_scr_load(s_screenAnswers);
  lv_obj_invalidate(s_screenAnswers);
}

void ui_answers_leave(void) {
  portENTER_CRITICAL(&s_resultMux);
  s_visible = false;
  portEXIT_CRITICAL(&s_resultMux);
  setPreviewState(false, false);
  if (!ui_answers_is_busy()) {
    if (camera_service_lock(300)) {
      camera_service_pause();
      camera_service_unlock();
    }
  }
}

void ui_answers_refresh(void) {
  if (!ui_answers_is_active()) {
    return;
  }
  renderCurrentView();
}

void ui_answers_refresh_locale(void) {
  if (!ui_answers_is_active()) {
    return;
  }
  renderCurrentView();
}

bool ui_answers_is_active(void) {
  return s_screenAnswers != nullptr &&
         lv_scr_act() == s_screenAnswers;
}

bool ui_answers_is_busy(void) {
  portENTER_CRITICAL(&s_resultMux);
  const bool busy = s_busy;
  portEXIT_CRITICAL(&s_resultMux);
  return busy;
}

bool ui_answers_next(UiRefreshMode *outRefreshMode) {
  if (outRefreshMode != nullptr) {
    *outRefreshMode = UI_REFRESH_NONE;
  }
  if (ui_answers_is_busy()) {
    return false;
  }

  if (s_view == ANSWERS_VIEW_MENU) {
    s_selectedMode = (AnswersMode)(((uint8_t)s_selectedMode + 1U) % ANSWERS_MODE_COUNT);
    showMenu();
    if (outRefreshMode != nullptr) {
      *outRefreshMode = UI_REFRESH_FAST;
    }
    return true;
  }
  if (s_view == ANSWERS_VIEW_CAMERA) {
    const bool started = startLocalAsk(ANSWERS_MODE_CAMERA);
    if (started && outRefreshMode != nullptr) {
      *outRefreshMode = UI_REFRESH_NAV;
    }
    return started;
  }
  if (s_view == ANSWERS_VIEW_RESULT) {
    const bool started = startLocalAsk(s_activeMode);
    if (outRefreshMode != nullptr) {
      *outRefreshMode = UI_REFRESH_NAV;
    }
    return started;
  }
  return false;
}

bool ui_answers_confirm(UiRefreshMode *outRefreshMode) {
  if (outRefreshMode != nullptr) {
    *outRefreshMode = UI_REFRESH_NONE;
  }
  if (ui_answers_is_busy()) {
    return false;
  }

  if (s_view == ANSWERS_VIEW_MENU) {
    if (s_selectedMode == ANSWERS_MODE_CAMERA) {
      showCamera();
    } else {
      startLocalAsk(s_selectedMode);
    }
    if (outRefreshMode != nullptr) {
      *outRefreshMode = UI_REFRESH_NAV;
    }
    return true;
  }
  if (s_view == ANSWERS_VIEW_CAMERA) {
    const bool started = startLocalAsk(ANSWERS_MODE_CAMERA);
    if (started && outRefreshMode != nullptr) {
      *outRefreshMode = UI_REFRESH_NAV;
    }
    return started;
  }
  if (s_view == ANSWERS_VIEW_RESULT) {
    showMenu();
    if (outRefreshMode != nullptr) {
      *outRefreshMode = UI_REFRESH_NAV;
    }
    return true;
  }
  return false;
}

bool ui_answers_request(void) {
  return ui_answers_confirm(nullptr);
}

bool ui_answers_service(UiRefreshMode *outRefreshMode) {
  if (outRefreshMode != nullptr) {
    *outRefreshMode = UI_REFRESH_NONE;
  }
  if (s_screenAnswers == nullptr) {
    return false;
  }

  bool done = false;
  AnswersLocalResult code = ANSWERS_LOCAL_TASK_FAIL;
  char mainText[40];
  char subText[96];
  char footer[40];
  mainText[0] = '\0';
  subText[0] = '\0';
  footer[0] = '\0';

  portENTER_CRITICAL(&s_resultMux);
  if (s_resultReady) {
    done = true;
    code = s_resultCode;
    snprintf(mainText, sizeof(mainText), "%s", s_resultMain);
    snprintf(subText, sizeof(subText), "%s", s_resultSub);
    formatAnswerFooter(s_activeMode, s_resultNumber, footer, sizeof(footer));
    s_resultReady = false;
  }
  portEXIT_CRITICAL(&s_resultMux);

  if (done) {
    showResult(mainText, subText, footer);
    if (outRefreshMode != nullptr) {
      *outRefreshMode = UI_REFRESH_NAV;
    }
    Serial.printf("[Answers] pipeline done code=%d\r\n", (int)code);
    return ui_answers_is_active();
  }

  if (ui_answers_is_active() && s_view == ANSWERS_VIEW_CAMERA &&
      !ui_answers_is_busy()) {
    if (!consumePreviewFrame()) {
      return false;
    }
    if (outRefreshMode != nullptr) {
      *outRefreshMode = UI_REFRESH_FAST;
    }
    return ui_answers_is_active();
  }

  return false;
}

lv_obj_t *ui_answers_get_screen(void) {
  return s_screenAnswers;
}
