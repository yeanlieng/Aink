#ifndef BTN_INPUT_H
#define BTN_INPUT_H

#include <stdint.h>
#include <stdbool.h>

/* 无物理按键时在 Serial Monitor 发单字符模拟；接好 D6/D7 后改为 0 */
#ifndef BTN_SERIAL_SIM
#define BTN_SERIAL_SIM  0
#endif

enum BtnId {
  BTN_ID_A = 0,
  BTN_ID_B = 1,
};

enum BtnAction {
  BTN_ACTION_NONE = 0,
  BTN_ACTION_NEXT,          // A click
  BTN_ACTION_PREV,          // A double click
  BTN_ACTION_BACK,          // A long press
  BTN_ACTION_CONFIRM,       // B click
  BTN_ACTION_VOICE_TOGGLE,  // B double click
};

void btn_input_init(void);
void btn_input_update(void);
void btn_input_serial_poll(void);
bool btn_input_consume(BtnAction *outAction);

#endif
