#pragma once
#include <stdint.h>

typedef enum {
    SAFE_STATE_BOOT = 0,
    SAFE_STATE_WATCHDOG,
    SAFE_STATE_HOST_LOST,
    SAFE_STATE_PROTOCOL_FAULT,
    SAFE_STATE_EXPLICIT_DISARM,
    SAFE_STATE_BROWNOUT,
} safe_state_reason_t;

// Forces every armed output to its documented safe level (currently: the
// digital bank via digital_bank_force_safe()) and records the reason/time
// for GET_STATUS. PWM/SPI outputs will hook in here too once they exist.
void safe_state_enter(safe_state_reason_t reason);

safe_state_reason_t safe_state_last_reason(void);
uint64_t safe_state_last_reason_time_us(void);
