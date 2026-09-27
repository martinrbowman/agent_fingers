#pragma once
#include <stdbool.h>
#include <stdint.h>

// Bidirectional 8-pin digital I/O bank on GP0-7 (see hardware/pin_claims.yaml).
// Boot default and safe-state target: all channels input/high-Z.
//
// Output is gated by an arm lease (plan's "Safety policy"): ARM_OUTPUTS must
// echo the challenge last returned by GET_STATUS, claims a subset of the 8
// channels as output, and grants a finite lease renewed by PING. Letting the
// lease expire, an explicit DISARM_OUTPUTS, or any safe_state_enter() call
// forces the whole bank back to input/high-Z.

void digital_bank_init(void);

// Checks lease expiry. Call every main-loop iteration.
void digital_bank_task(void);

// Forces pindirs=0 (input/high-Z), values=0, and clears the arm. Called by
// safe_state_enter() — this is what actually makes that hook do something.
void digital_bank_force_safe(void);

// Rewrites this module's current pad input-enable state onto GP0-7. The
// bank keeps input buffers off except while sampling (RP2350 A2 erratum
// E9, see digital_bank.c); gpio_set_function()/pio_gpio_init() turn them
// back on, so anything that re-muxes a bank pin must call this afterwards.
void digital_bank_reapply_input_enables(void);

bool digital_bank_armed(void);
uint8_t digital_bank_armed_mask(void);

// Fills challenge_out[16] with the challenge that a matching ARM_OUTPUTS
// call must echo back. Rotates only on a successful arm, so repeated
// GET_STATUS polls without arming see a stable value.
void digital_bank_get_challenge(uint8_t challenge_out[16]);

// Returns false (no state change) if challenge doesn't match, without
// revealing which part was wrong.
bool digital_bank_arm(const uint8_t challenge[16], uint8_t output_mask, uint32_t lease_ms_requested,
                      uint32_t *granted_lease_ms_out);

// Extends the current lease from now by its originally granted duration.
// No-op if not armed.
void digital_bank_renew_lease(void);

// Returns false (no state change) if mask includes any bit outside the
// currently armed output mask.
bool digital_bank_write_masked(uint8_t mask, uint8_t values);

// Samples all 8 pins now, returns the value with non-mask bits cleared.
// Input buffers for the mask are turned on just for this sample (E9).
uint8_t digital_bank_read(uint8_t mask);

// Continuous periodic capture — a third PIO state machine, independent of
// (and usable concurrently with) digital_bank_read()/write_masked() above.
// One capture at a time; a new START replaces whatever's running.

#define DIGITAL_CAPTURE_MAX_RATE_HZ    200000u
#define DIGITAL_CAPTURE_BUFFER_SAMPLES 4096u

typedef struct {
    uint32_t capture_id;
    uint32_t granted_rate_hz;
    uint32_t buffer_capacity_samples;
} digital_capture_start_result_t;

// rate_hz is clamped to what the current system clock's PIO clkdiv can
// reach (see digital_bank.c) up to DIGITAL_CAPTURE_MAX_RATE_HZ. max_samples
// is clamped to DIGITAL_CAPTURE_BUFFER_SAMPLES (0 = unbounded/continuous) so
// a bounded capture can never wrap before it completes.
void digital_capture_start(uint32_t rate_hz, uint32_t max_samples,
                            digital_capture_start_result_t *out);

typedef struct {
    uint32_t capture_id;
    uint32_t total_captured;
} digital_capture_stop_result_t;

void digital_capture_stop(digital_capture_stop_result_t *out);

typedef struct {
    uint32_t capture_id;
    uint32_t total_captured;
    uint32_t overrun_count;
    uint64_t start_timestamp_us;
    uint32_t rate_hz;
} digital_capture_status_t;

// Returns false (nothing written) if capture_id doesn't match the
// current/last capture. Otherwise fills status_out, copies up to `limit`
// sample bytes starting at absolute sample index `offset` into samples_out
// (caller must size it for `limit` bytes), and sets *returned_out to how
// many bytes were actually copied — which can be less than `limit` if the
// capture hasn't produced that many samples yet, or some were overwritten
// (see status_out->overrun_count).
bool digital_capture_read(uint32_t capture_id, uint32_t offset, uint16_t limit,
                           uint8_t *samples_out, uint16_t *returned_out,
                           digital_capture_status_t *status_out);

// Polls for bounded-capture auto-completion and for ring overrun on an
// unbounded capture. Call every main-loop iteration. Returns true exactly
// once, the iteration a bounded capture completes on its own — the caller
// should emit the capture-complete EVENT with *out filled in.
bool digital_capture_task(digital_capture_stop_result_t *out);
