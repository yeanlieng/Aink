#include "btn_input.h"

#include "DEV_Config.h"

#include <Arduino.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BTN_DEBOUNCE_MS   25U
#define BTN_DOUBLE_MS     400U
#define BTN_LONG_MS       700U
#define BTN_EDGE_QUEUE_SIZE    32U
#define BTN_ACTION_QUEUE_SIZE  16U

struct BtnEdgeEvent {
  uint8_t id;
  uint8_t rawDown;
  uint32_t timeMs;
};

struct ButtonTracker {
  bool stableDown;
  bool lastRawDown;
  uint32_t lastChangeMs;
  uint32_t pressStartMs;
  bool longFired;
  uint8_t clickCount;
  uint32_t lastReleaseMs;
};

static ButtonTracker s_btnA;
static ButtonTracker s_btnB;
static volatile BtnEdgeEvent s_edgeQueue[BTN_EDGE_QUEUE_SIZE];
static volatile uint8_t s_edgeHead = 0;
static volatile uint8_t s_edgeTail = 0;
static BtnAction s_actionQueue[BTN_ACTION_QUEUE_SIZE];
static uint8_t s_actionHead = 0;
static uint8_t s_actionTail = 0;

static void print_serial_help(void) {
  Serial.println("[BtnSim] Serial keys (115200, line ending Any):");
  Serial.println("  n = A click (next)   p = A double (prev)   b = A long (back)");
  Serial.println("  c = B click (confirm)   v = B double (voice)   h = this help");
}

static uint32_t btn_now_ms(void) {
  return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static uint32_t IRAM_ATTR btn_now_ms_from_isr(void) {
  return (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS);
}

static uint8_t IRAM_ATTR nextEdgeIndex(uint8_t index) {
  index++;
  if (index >= BTN_EDGE_QUEUE_SIZE) {
    index = 0;
  }
  return index;
}

static uint8_t nextActionIndex(uint8_t index) {
  index++;
  if (index >= BTN_ACTION_QUEUE_SIZE) {
    index = 0;
  }
  return index;
}

static bool IRAM_ATTR readPinDown(int pin) {
  return gpio_get_level((gpio_num_t)pin) == 0;
}

static bool readRawDown(BtnId id) {
  return readPinDown((id == BTN_ID_A) ? BTN_A_PIN : BTN_B_PIN);
}

static void IRAM_ATTR pushEdgeFromIsr(uint8_t id, int pin) {
  const uint8_t head = s_edgeHead;
  uint8_t next = nextEdgeIndex(head);
  if (next == s_edgeTail) {
    s_edgeTail = nextEdgeIndex(s_edgeTail);
  }

  s_edgeQueue[head].id = id;
  s_edgeQueue[head].rawDown = readPinDown(pin) ? 1U : 0U;
  s_edgeQueue[head].timeMs = btn_now_ms_from_isr();
  s_edgeHead = next;
}

static void IRAM_ATTR handleButtonAInterrupt(void) {
  pushEdgeFromIsr((uint8_t)BTN_ID_A, BTN_A_PIN);
}

static void IRAM_ATTR handleButtonBInterrupt(void) {
  pushEdgeFromIsr((uint8_t)BTN_ID_B, BTN_B_PIN);
}

static bool elapsedMs(uint32_t now, uint32_t since, uint32_t interval) {
  return (uint32_t)(now - since) >= interval;
}

static BtnAction actionFor(BtnId id, uint8_t clickCount, bool isLong) {
  if (isLong) {
    if (id == BTN_ID_A) {
      return BTN_ACTION_BACK;
    }
    return BTN_ACTION_NONE;
  }

  if (clickCount >= 2) {
    return (id == BTN_ID_A) ? BTN_ACTION_PREV : BTN_ACTION_VOICE_TOGGLE;
  }
  if (clickCount == 1) {
    return (id == BTN_ID_A) ? BTN_ACTION_NEXT : BTN_ACTION_CONFIRM;
  }
  return BTN_ACTION_NONE;
}

static bool queueAction(BtnAction action) {
  if (action == BTN_ACTION_NONE) {
    return false;
  }

  const uint8_t next = nextActionIndex(s_actionHead);
  if (next == s_actionTail) {
    return false;
  }

  s_actionQueue[s_actionHead] = action;
  s_actionHead = next;
  return true;
}

static void finalizeClicks(ButtonTracker *btn, BtnId id) {
  if (btn->clickCount == 0) {
    return;
  }
  queueAction(actionFor(id, btn->clickCount, false));
  btn->clickCount = 0;
}

static void serviceButton(ButtonTracker *btn, BtnId id, uint32_t now) {
  if (btn->lastRawDown != btn->stableDown &&
      elapsedMs(now, btn->lastChangeMs, BTN_DEBOUNCE_MS)) {
    const uint32_t stableAt = btn->lastChangeMs + BTN_DEBOUNCE_MS;
    btn->stableDown = btn->lastRawDown;
    if (btn->stableDown) {
      btn->pressStartMs = stableAt;
      btn->longFired = false;
    } else {
      if (btn->longFired) {
        btn->clickCount = 0;
      } else {
        if (btn->clickCount < 2) {
          btn->clickCount++;
        }
        btn->lastReleaseMs = stableAt;
      }
    }
  }

  if (btn->stableDown && !btn->longFired &&
      elapsedMs(now, btn->pressStartMs, BTN_LONG_MS)) {
    btn->longFired = true;
    btn->clickCount = 0;
    queueAction(actionFor(id, 0, true));
  }

  if (!btn->stableDown && btn->clickCount > 0 &&
      elapsedMs(now, btn->lastReleaseMs, BTN_DOUBLE_MS)) {
    finalizeClicks(btn, id);
  }
}

static void serviceButtons(uint32_t now) {
  serviceButton(&s_btnA, BTN_ID_A, now);
  serviceButton(&s_btnB, BTN_ID_B, now);
}

static ButtonTracker *trackerFor(BtnId id) {
  return (id == BTN_ID_A) ? &s_btnA : &s_btnB;
}

static void applyEdgeEvent(const BtnEdgeEvent *event) {
  const BtnId id = (event->id == (uint8_t)BTN_ID_A) ? BTN_ID_A : BTN_ID_B;
  ButtonTracker *btn = trackerFor(id);
  const bool rawDown = event->rawDown != 0;

  serviceButtons(event->timeMs);
  if (rawDown != btn->lastRawDown) {
    btn->lastRawDown = rawDown;
    btn->lastChangeMs = event->timeMs;
  }
}

static bool popEdgeEvent(BtnEdgeEvent *event) {
  noInterrupts();
  if (s_edgeTail == s_edgeHead) {
    interrupts();
    return false;
  }

  const uint8_t tail = s_edgeTail;
  event->id = s_edgeQueue[tail].id;
  event->rawDown = s_edgeQueue[tail].rawDown;
  event->timeMs = s_edgeQueue[tail].timeMs;
  s_edgeTail = nextEdgeIndex(tail);
  interrupts();
  return true;
}

static void resetButton(ButtonTracker *btn, bool rawDown, uint32_t now) {
  btn->stableDown = rawDown;
  btn->lastRawDown = rawDown;
  btn->lastChangeMs = now;
  btn->pressStartMs = rawDown ? now : 0;
  btn->longFired = false;
  btn->clickCount = 0;
  btn->lastReleaseMs = 0;
}

#if BTN_SERIAL_SIM
static void queue_serial_action(BtnAction action) {
  if (queueAction(action)) {
    Serial.printf("[BtnSim] queued action=%d\n", (int)action);
  } else {
    Serial.println("[BtnSim] action queue full");
  }
}

static BtnAction action_from_serial_char(char ch) {
  switch (ch) {
    case 'n':
    case 'N':
      return BTN_ACTION_NEXT;
    case 'p':
    case 'P':
      return BTN_ACTION_PREV;
    case 'b':
    case 'B':
      return BTN_ACTION_BACK;
    case 'c':
    case 'C':
      return BTN_ACTION_CONFIRM;
    case 'v':
    case 'V':
      return BTN_ACTION_VOICE_TOGGLE;
    default:
      return BTN_ACTION_NONE;
  }
}
#endif

void btn_input_init(void) {
  detachInterrupt(digitalPinToInterrupt(BTN_A_PIN));
  detachInterrupt(digitalPinToInterrupt(BTN_B_PIN));

  pinMode(BTN_A_PIN, INPUT_PULLUP);
  pinMode(BTN_B_PIN, INPUT_PULLUP);

  noInterrupts();
  s_edgeHead = 0;
  s_edgeTail = 0;
  interrupts();

  s_actionHead = 0;
  s_actionTail = 0;

  const uint32_t now = btn_now_ms();
  resetButton(&s_btnA, readRawDown(BTN_ID_A), now);
  resetButton(&s_btnB, readRawDown(BTN_ID_B), now);

  attachInterrupt(digitalPinToInterrupt(BTN_A_PIN), handleButtonAInterrupt, CHANGE);
  attachInterrupt(digitalPinToInterrupt(BTN_B_PIN), handleButtonBInterrupt, CHANGE);

#if BTN_SERIAL_SIM
  print_serial_help();
#endif
}

void btn_input_update(void) {
  BtnEdgeEvent event;
  while (popEdgeEvent(&event)) {
    applyEdgeEvent(&event);
  }
  serviceButtons(btn_now_ms());
}

void btn_input_serial_poll(void) {
#if BTN_SERIAL_SIM
  while (Serial.available() > 0) {
    const char ch = (char)Serial.read();
    if (ch == 'h' || ch == 'H' || ch == '?') {
      print_serial_help();
      continue;
    }
    const BtnAction action = action_from_serial_char(ch);
    if (action != BTN_ACTION_NONE) {
      queue_serial_action(action);
    }
  }
#else
  (void)0;
#endif
}

bool btn_input_consume(BtnAction *outAction) {
  if (outAction == nullptr || s_actionTail == s_actionHead) {
    return false;
  }

  *outAction = s_actionQueue[s_actionTail];
  s_actionTail = nextActionIndex(s_actionTail);
  return true;
}
