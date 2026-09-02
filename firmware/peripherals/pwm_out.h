#pragma once
#include <stdbool.h>
#include <stdint.h>

// PWM output on any GP0-7 digital-bank channel. Reuses the digital bank's
// existing arm/lease/challenge gate entirely — no separate PWM claim
// mechanism: a channel must already be in ARM_OUTPUTS's armed output mask
// before PWM_CONFIG can drive it. Losing that lease (expiry, disarm, any
// safe_state_enter() trigger) disables any active PWM on it and returns
// the pin to plain digital-bank ownership, same as any other armed output.

#define PWM_MAX_FREQUENCY_HZ 100000u // provisional software ceiling, not
                                       // measured against a reference

void pwm_out_init(void);

// Disables every active PWM channel and returns its pin to PIO/digital-bank
// ownership. Must run BEFORE digital_bank_force_safe() in any force-safe
// sequence — see safe_state.c. Calling this after the digital bank's
// pindirs are already reset would leave a PWM-driven pin actively driving
// despite "safe state," since the PIO's output-enable isn't even connected
// to a pad whose function is muxed to PWM.
void pwm_out_force_safe(void);

typedef struct {
    uint32_t granted_frequency_hz; // 0 if disabled
    uint16_t granted_duty_permille;
} pwm_out_config_result_t;

// Returns false (no state change) if channel > 7 or channel is not
// currently in the digital bank's armed output mask.
bool pwm_out_config(uint8_t channel, bool enabled, uint32_t frequency_hz,
                     uint16_t duty_permille, pwm_out_config_result_t *out);

// True if this channel's pin is currently function-muxed to PWM. While
// true, DIGITAL_WRITE_MASKED must not touch this channel — the PIO ctrl
// SM's write would reach an internal shadow register but never the
// physical pad, since the pad's function isn't PIO right now. See
// digital_bank_write_masked()'s use of this.
bool pwm_out_channel_active(uint8_t channel);
