#include "io/safe_state.h"
#include "io/digital_bank.h"
#include "peripherals/pwm_out.h"
#include "pico/time.h"

static safe_state_reason_t s_last_reason = SAFE_STATE_BOOT;
static uint64_t s_last_reason_time_us = 0;

void safe_state_enter(safe_state_reason_t reason) {
    s_last_reason = reason;
    s_last_reason_time_us = time_us_64();
    // PWM must release its pins back to PIO before the digital bank resets
    // pindirs — otherwise a pin still function-muxed to PWM keeps actively
    // driving even after "safe state," since the PIO's output-enable isn't
    // connected to a pad currently owned by the PWM peripheral.
    pwm_out_force_safe();
    digital_bank_force_safe();
}

safe_state_reason_t safe_state_last_reason(void) {
    return s_last_reason;
}

uint64_t safe_state_last_reason_time_us(void) {
    return s_last_reason_time_us;
}
