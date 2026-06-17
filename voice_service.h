#ifndef VOICE_SERVICE_H
#define VOICE_SERVICE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  VOICE_STATE_IDLE = 0,
  VOICE_STATE_RECORDING,
  VOICE_STATE_THINKING,
  VOICE_STATE_SPEAKING,
  VOICE_STATE_DONE,
  VOICE_STATE_ERROR,
} VoiceState;

void voice_service_init(void);
bool voice_service_toggle_recording(void);
bool voice_service_interrupt_speaker(void);
bool voice_service_service(void);
bool voice_service_is_busy(void);
VoiceState voice_service_state(void);
void voice_service_status_text(char *out, size_t outLen);
bool voice_service_capture_local_seed(uint32_t recordMs, uint32_t *outSeed);

#endif
