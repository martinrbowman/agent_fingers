#include "io/digital_bank.h"
#include "io/safe_state.h"
#include "peripherals/pwm_out.h"
#include "digital_io.pio.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "pico/time.h"
#include "pico/rand.h"
#include <string.h>

#define DIGITAL_BASE_PIN 0
#define DIGITAL_PIN_COUNT 8
#define DIGITAL_PIO pio0

#define LEASE_MIN_MS     100u
#define LEASE_MAX_MS     30000u
#define LEASE_DEFAULT_MS 2000u

// RP2350 A2 erratum E9: a Bank 0 pad with its input buffer enabled, once
// driven high and then released, latches at ~2.2V -- the internal
// pull-down is too weak to pull it back and it reads 1 indefinitely.
// Reproduced on this board: all 8 channels driven high, disarmed, still
// read 0xFF three seconds later. The erratum's workaround is to keep the
// input buffer off except while actually sampling (clearing IE also
// releases a latched pad), so input enables here are owned by this module
// and only turned on for a DIGITAL_READ's mask (briefly) or for the whole
// bank while a capture runs. Note gpio_set_function()/pio_gpio_init()
// silently turn IE back on, so any re-mux must be followed by
// digital_bank_reapply_input_enables().
#define DIGITAL_INPUT_SETTLE_US   2u  // IE on -> first valid sample
#define DIGITAL_RELEASE_SETTLE_US 10u // released pin -> pull-down has discharged it

// RP2350's DMA transfer-count register reserves its top 4 bits for a MODE
// field -- valid count range is 0 to 2^28-1, not the full 32 bits. Plain
// 0xFFFFFFFFu as an "unbounded, run until stopped" sentinel sets those
// MODE bits to garbage instead of 0 -- found during spi_slave.c's hardware
// bring-up (see that file for the full story). Address-wrap tracking here
// means it never produced an observable symptom, but it's still invalid
// per the RP2350 DMA contract; this is the correct sentinel.
#define DIGITAL_CONTINUOUS_TRANSFER_COUNT DMA_CH0_TRANS_COUNT_COUNT_BITS

static uint s_ctrl_sm;
static uint s_sample_sm;

static uint8_t s_pindirs;
static uint8_t s_values;
static uint8_t s_input_enabled; // pad IE bits currently set on GP0-7

static bool     s_armed;
static uint8_t  s_armed_mask;
static uint32_t s_lease_ms;
static absolute_time_t s_lease_expires;

static uint8_t s_challenge[16];

static uint s_capture_sm;
static uint s_capture_offset;
static int  s_capture_dma_chan;
// RP2350 DMA ring wrap works by masking address bits, so the ring region
// must be aligned to its own size (2^12 = 4096 bytes here) — an unaligned
// buffer wraps to the wrong address entirely, corrupting the write pointer.
static uint32_t s_capture_buffer[DIGITAL_CAPTURE_BUFFER_SAMPLES]
    __attribute__((aligned(DIGITAL_CAPTURE_BUFFER_SAMPLES * sizeof(uint32_t))));

static bool     s_capture_active;        // SM+DMA currently running
static uint32_t s_capture_id;            // 0 = never started
static uint32_t s_capture_rate_hz;
static uint32_t s_capture_max_samples;   // 0 = unbounded
static uint64_t s_capture_start_us;

static uint32_t s_capture_write_lap;         // completed full ring laps
static uint32_t s_capture_last_write_offset; // position within current lap
static uint32_t s_capture_overrun_count;
static bool     s_capture_just_completed;

static void rotate_challenge(void) {
    uint32_t *words = (uint32_t *)(void *)s_challenge;
    for (int i = 0; i < 4; i++) {
        words[i] = get_rand_32();
    }
}

static void ctrl_push(uint8_t pindirs, uint8_t values) {
    pio_sm_put_blocking(DIGITAL_PIO, s_ctrl_sm, pindirs);
    pio_sm_put_blocking(DIGITAL_PIO, s_ctrl_sm, values);
}

static void set_input_enables(uint8_t mask) {
    for (uint channel = 0; channel < DIGITAL_PIN_COUNT; channel++) {
        gpio_set_input_enabled(DIGITAL_BASE_PIN + channel, (mask >> channel) & 1u);
    }
    s_input_enabled = mask;
}

// What the input enables should be when no one-shot read is in progress:
// all on while a capture is sampling the bank, all off otherwise (E9).
static uint8_t idle_input_mask(void) {
    return s_capture_active ? 0xFFu : 0x00u;
}

static void force_safe_locked(void) {
    // IE off before letting go of the pins, so a channel that was driving
    // high can't latch at ~2.2V (E9) as it's released. If a capture needs
    // the inputs back, wait for the pull-downs to discharge the released
    // pins first -- re-enabling IE on a pin that's still high would latch.
    set_input_enables(0);
    s_pindirs = 0;
    s_values = 0;
    ctrl_push(0, 0);
    s_armed = false;
    s_armed_mask = 0;
    if (idle_input_mask()) {
        busy_wait_us_32(DIGITAL_RELEASE_SETTLE_US);
        set_input_enables(idle_input_mask());
    }
}

static uint32_t clamp_lease(uint32_t requested_ms) {
    if (requested_ms == 0) {
        return LEASE_DEFAULT_MS;
    }
    if (requested_ms < LEASE_MIN_MS) {
        return LEASE_MIN_MS;
    }
    if (requested_ms > LEASE_MAX_MS) {
        return LEASE_MAX_MS;
    }
    return requested_ms;
}

void digital_bank_init(void) {
    uint ctrl_offset = pio_add_program(DIGITAL_PIO, &digital_io_ctrl_program);
    uint sample_offset = pio_add_program(DIGITAL_PIO, &digital_io_sample_program);

    s_ctrl_sm = pio_claim_unused_sm(DIGITAL_PIO, true);
    s_sample_sm = pio_claim_unused_sm(DIGITAL_PIO, true);

    for (uint pin = DIGITAL_BASE_PIN; pin < DIGITAL_BASE_PIN + DIGITAL_PIN_COUNT; pin++) {
        pio_gpio_init(DIGITAL_PIO, pin);
        // A weak pull-down stays harmless once a channel is armed as output
        // (negligible leakage against the driver) and gives every
        // unconnected/input channel a deterministic idle level instead of
        // floating-noise readings.
        gpio_pull_down(pin);
    }
    // pio_gpio_init() left every input buffer on; E9 needs them off by default.
    set_input_enables(0);
    pio_sm_set_consecutive_pindirs(DIGITAL_PIO, s_ctrl_sm, DIGITAL_BASE_PIN, DIGITAL_PIN_COUNT, false);

    pio_sm_config ctrl_cfg = digital_io_ctrl_program_get_default_config(ctrl_offset);
    sm_config_set_out_pins(&ctrl_cfg, DIGITAL_BASE_PIN, DIGITAL_PIN_COUNT);
    sm_config_set_out_shift(&ctrl_cfg, true, false, 32); // shift right, no autopull
    pio_sm_init(DIGITAL_PIO, s_ctrl_sm, ctrl_offset, &ctrl_cfg);
    pio_sm_set_enabled(DIGITAL_PIO, s_ctrl_sm, true);

    pio_sm_config sample_cfg = digital_io_sample_program_get_default_config(sample_offset);
    sm_config_set_in_pins(&sample_cfg, DIGITAL_BASE_PIN);
    sm_config_set_in_shift(&sample_cfg, false, false, 32); // shift left, no autopush
    pio_sm_init(DIGITAL_PIO, s_sample_sm, sample_offset, &sample_cfg);
    pio_sm_set_enabled(DIGITAL_PIO, s_sample_sm, true);

    uint capture_offset = pio_add_program(DIGITAL_PIO, &digital_io_capture_program);
    s_capture_offset = capture_offset;
    s_capture_sm = pio_claim_unused_sm(DIGITAL_PIO, true);

    pio_sm_config capture_cfg = digital_io_capture_program_get_default_config(capture_offset);
    sm_config_set_in_pins(&capture_cfg, DIGITAL_BASE_PIN);
    sm_config_set_in_shift(&capture_cfg, false, false, 32); // shift left, no autopush
    // This SM never uses its TX FIFO (no `pull` in the program) — join it to
    // RX for 8 words of headroom instead of 4, before `push block` stalls
    // the sample loop waiting on DMA.
    sm_config_set_fifo_join(&capture_cfg, PIO_FIFO_JOIN_RX);
    pio_sm_init(DIGITAL_PIO, s_capture_sm, capture_offset, &capture_cfg);
    // Not enabled here — digital_capture_start() enables it per-capture.

    s_capture_dma_chan = dma_claim_unused_channel(true);
    s_capture_active = false;
    s_capture_id = 0;

    s_pindirs = 0;
    s_values = 0;
    s_armed = false;
    s_armed_mask = 0;
    s_lease_ms = 0;
    rotate_challenge();

    ctrl_push(0, 0);
}

void digital_bank_task(void) {
    if (s_armed && time_reached(s_lease_expires)) {
        safe_state_enter(SAFE_STATE_HOST_LOST);
    }
}

void digital_bank_force_safe(void) {
    force_safe_locked();
}

void digital_bank_reapply_input_enables(void) {
    set_input_enables(s_input_enabled);
}

bool digital_bank_armed(void) {
    return s_armed;
}

uint8_t digital_bank_armed_mask(void) {
    return s_armed_mask;
}

void digital_bank_get_challenge(uint8_t challenge_out[16]) {
    memcpy(challenge_out, s_challenge, 16);
}

bool digital_bank_arm(const uint8_t challenge[16], uint8_t output_mask, uint32_t lease_ms_requested,
                      uint32_t *granted_lease_ms_out) {
    if (memcmp(challenge, s_challenge, 16) != 0) {
        return false;
    }

    s_pindirs = output_mask;
    s_values = 0; // newly armed outputs default low, never undefined
    ctrl_push(s_pindirs, s_values);

    s_armed = true;
    s_armed_mask = output_mask;
    s_lease_ms = clamp_lease(lease_ms_requested);
    s_lease_expires = make_timeout_time_ms(s_lease_ms);

    rotate_challenge(); // this exact challenge can't be replayed again

    if (granted_lease_ms_out) {
        *granted_lease_ms_out = s_lease_ms;
    }
    return true;
}

void digital_bank_renew_lease(void) {
    if (s_armed) {
        s_lease_expires = make_timeout_time_ms(s_lease_ms);
    }
}

bool digital_bank_write_masked(uint8_t mask, uint8_t values) {
    if (!s_armed) {
        return false;
    }
    if (mask & (uint8_t)~s_armed_mask) {
        return false;
    }
    // A PWM-active channel's pad isn't muxed to PIO right now — a write
    // here would update our shadow register but never reach the pin.
    for (uint8_t channel = 0; channel < 8; channel++) {
        if ((mask & (1u << channel)) && pwm_out_channel_active(channel)) {
            return false;
        }
    }
    s_values = (uint8_t)((s_values & ~mask) | (values & mask));
    ctrl_push(s_pindirs, s_values);
    return true;
}

uint8_t digital_bank_read(uint8_t mask) {
    uint8_t idle = idle_input_mask();
    bool need_enable = (mask & (uint8_t)~s_input_enabled) != 0;
    if (need_enable) {
        set_input_enables(idle | mask);
        busy_wait_us_32(DIGITAL_INPUT_SETTLE_US);
    }
    pio_sm_put_blocking(DIGITAL_PIO, s_sample_sm, 0);
    uint32_t word = pio_sm_get_blocking(DIGITAL_PIO, s_sample_sm);
    if (need_enable) {
        set_input_enables(idle);
    }
    return (uint8_t)(word & 0xFFu & mask);
}

static uint32_t capture_clamp_rate_hz(uint32_t requested_hz) {
    uint32_t clk_sys_hz = clock_get_hz(clk_sys);
    // PIO clkdiv is a 16.8 fixed-point value, max just under 65536; below
    // this floor the divider can't slow the SM down any further.
    uint32_t min_hz = (clk_sys_hz / (3u * 65535u)) + 1u;
    uint32_t hz = requested_hz;
    if (hz < min_hz) {
        hz = min_hz;
    }
    if (hz > DIGITAL_CAPTURE_MAX_RATE_HZ) {
        hz = DIGITAL_CAPTURE_MAX_RATE_HZ;
    }
    return hz;
}

static uint32_t capture_write_pos_samples(void) {
    uintptr_t base = (uintptr_t)s_capture_buffer;
    uintptr_t cur = (uintptr_t)dma_hw->ch[s_capture_dma_chan].write_addr;
    return (uint32_t)((cur - base) / sizeof(uint32_t));
}

// Polls the DMA write pointer and folds any ring wraparound into
// s_capture_write_lap. Must be called often enough that no more than one
// full lap can pass between calls — true at this module's rate ceiling
// (DIGITAL_CAPTURE_MAX_RATE_HZ) given the main loop's iteration latency, but
// not a hard real-time guarantee.
static void capture_poll_position(void) {
    uint32_t pos = capture_write_pos_samples();
    if (pos < s_capture_last_write_offset) {
        s_capture_write_lap++;
    }
    s_capture_last_write_offset = pos;
}

static uint32_t capture_total_written(void) {
    return s_capture_write_lap * DIGITAL_CAPTURE_BUFFER_SAMPLES + s_capture_last_write_offset;
}

void digital_capture_start(uint32_t rate_hz, uint32_t max_samples,
                            digital_capture_start_result_t *out) {
    if (s_capture_active) {
        pio_sm_set_enabled(DIGITAL_PIO, s_capture_sm, false);
        dma_channel_abort(s_capture_dma_chan);
        s_capture_active = false;
    }

    uint32_t granted_hz = capture_clamp_rate_hz(rate_hz);
    uint32_t clamped_max = max_samples;
    if (clamped_max > DIGITAL_CAPTURE_BUFFER_SAMPLES) {
        clamped_max = DIGITAL_CAPTURE_BUFFER_SAMPLES;
    }

    s_capture_id++;
    if (s_capture_id == 0) {
        s_capture_id = 1; // 0 is reserved for "no capture yet"
    }
    s_capture_rate_hz = granted_hz;
    s_capture_max_samples = clamped_max;
    s_capture_start_us = time_us_64();
    s_capture_write_lap = 0;
    s_capture_last_write_offset = 0;
    s_capture_overrun_count = 0;
    s_capture_just_completed = false;

    pio_sm_set_enabled(DIGITAL_PIO, s_capture_sm, false);
    pio_sm_clear_fifos(DIGITAL_PIO, s_capture_sm);
    pio_sm_restart(DIGITAL_PIO, s_capture_sm);
    // pio_sm_restart() doesn't reset the PC. A bounded capture's SM keeps
    // sampling after DMA stops until the FIFO fills, then gets disabled
    // stalled on `push` -- without this jump, the next capture's first
    // sample is that leftover push instead of a fresh read (intermittent
    // sample-0 corruption, seen as test_bounded_capture_completes_exactly
    // failing roughly half the time).
    pio_sm_exec(DIGITAL_PIO, s_capture_sm, pio_encode_jmp(s_capture_offset));

    float clkdiv = (float)clock_get_hz(clk_sys) / (3.0f * (float)granted_hz);
    pio_sm_set_clkdiv(DIGITAL_PIO, s_capture_sm, clkdiv);

    dma_channel_config dma_cfg = dma_channel_get_default_config(s_capture_dma_chan);
    channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_cfg, false);
    channel_config_set_write_increment(&dma_cfg, true);
    channel_config_set_dreq(&dma_cfg, pio_get_dreq(DIGITAL_PIO, s_capture_sm, false));
    channel_config_set_ring(&dma_cfg, true, 14); // write ring: 2^14 = 16384 bytes = 4096 samples

    // Bounded capture: DMA itself stops exactly at clamped_max transfers, no
    // polling-based approximate stop needed. Unbounded: an effectively
    // endless count into the wrapping ring, stopped explicitly later.
    uint32_t trans_count = (clamped_max > 0) ? clamped_max : DIGITAL_CONTINUOUS_TRANSFER_COUNT;

    dma_channel_configure(s_capture_dma_chan, &dma_cfg,
                           s_capture_buffer, &DIGITAL_PIO->rxf[s_capture_sm],
                           trans_count, true);

    s_capture_active = true;
    set_input_enables(idle_input_mask());
    busy_wait_us_32(DIGITAL_INPUT_SETTLE_US);
    pio_sm_set_enabled(DIGITAL_PIO, s_capture_sm, true);

    if (out) {
        out->capture_id = s_capture_id;
        out->granted_rate_hz = granted_hz;
        out->buffer_capacity_samples = DIGITAL_CAPTURE_BUFFER_SAMPLES;
    }
}

void digital_capture_stop(digital_capture_stop_result_t *out) {
    if (s_capture_active) {
        capture_poll_position();
        pio_sm_set_enabled(DIGITAL_PIO, s_capture_sm, false);
        dma_channel_abort(s_capture_dma_chan);
        s_capture_active = false;
        set_input_enables(idle_input_mask());
    }
    if (out) {
        out->capture_id = s_capture_id;
        out->total_captured = capture_total_written();
    }
}

bool digital_capture_read(uint32_t capture_id, uint32_t offset, uint16_t limit,
                           uint8_t *samples_out, uint16_t *returned_out,
                           digital_capture_status_t *status_out) {
    if (capture_id == 0 || capture_id != s_capture_id) {
        return false;
    }
    if (s_capture_active) {
        capture_poll_position();
    }
    uint32_t total = capture_total_written();

    // Oldest sample still physically present in the ring.
    uint32_t oldest_valid = (total > DIGITAL_CAPTURE_BUFFER_SAMPLES)
                                 ? (total - DIGITAL_CAPTURE_BUFFER_SAMPLES) : 0;
    if (offset < oldest_valid) {
        s_capture_overrun_count += (oldest_valid - offset);
        offset = oldest_valid;
    }

    uint16_t returned = 0;
    if (offset < total) {
        uint32_t available = total - offset;
        uint32_t want = limit;
        if (want > available) {
            want = available;
        }
        if (want > DIGITAL_CAPTURE_BUFFER_SAMPLES) {
            want = DIGITAL_CAPTURE_BUFFER_SAMPLES;
        }

        uint32_t ring_start = offset % DIGITAL_CAPTURE_BUFFER_SAMPLES;
        for (uint32_t i = 0; i < want; i++) {
            uint32_t idx = (ring_start + i) % DIGITAL_CAPTURE_BUFFER_SAMPLES;
            samples_out[i] = (uint8_t)(s_capture_buffer[idx] & 0xFFu);
        }
        returned = (uint16_t)want;
    }

    if (returned_out) {
        *returned_out = returned;
    }
    if (status_out) {
        status_out->capture_id = s_capture_id;
        status_out->total_captured = total;
        status_out->overrun_count = s_capture_overrun_count;
        status_out->start_timestamp_us = s_capture_start_us;
        status_out->rate_hz = s_capture_rate_hz;
    }
    return true;
}

bool digital_capture_task(digital_capture_stop_result_t *out) {
    if (s_capture_active) {
        capture_poll_position();

        if (s_capture_max_samples > 0 && !dma_channel_is_busy(s_capture_dma_chan)) {
            pio_sm_set_enabled(DIGITAL_PIO, s_capture_sm, false);
            s_capture_active = false;
            set_input_enables(idle_input_mask());
            s_capture_just_completed = true;
        }
    }

    if (s_capture_just_completed) {
        s_capture_just_completed = false;
        if (out) {
            out->capture_id = s_capture_id;
            out->total_captured = capture_total_written();
        }
        return true;
    }
    return false;
}
