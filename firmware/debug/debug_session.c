#include "debug/debug_session.h"
#include "peripherals/spi_master.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

#define DEBUG_JTAG_RESET_PIN 22

// Transitions are split by core: core1 moves IDLE->CLAIM_REQUESTED and
// {CLAIM_REQUESTED,ACTIVE}->RELEASE_REQUESTED; core0 moves
// CLAIM_REQUESTED->ACTIVE and RELEASE_REQUESTED->IDLE. The one overlap
// (core1 ending a session while core0 is granting it) is why every
// transition goes through a hardware spinlock.
typedef enum {
    DS_IDLE,
    DS_CLAIM_REQUESTED,
    DS_ACTIVE,
    DS_RELEASE_REQUESTED,
} ds_state_t;

static spin_lock_t *s_lock;
static volatile ds_state_t         s_state;
static volatile debug_mode_t       s_mode;
static volatile debug_end_reason_t s_requested_end;

// core0-only bookkeeping.
static bool               s_pins_taken;   // SPI master pins handed away
static bool               s_started;      // "started" EVENT emitted
static debug_mode_t       s_active_mode;
static debug_end_reason_t s_last_end_reason;
static uint32_t           s_session_count;

void debug_session_init(void) {
    s_lock = spin_lock_init(spin_lock_claim_unused(true));
    s_state = DS_IDLE;
    s_mode = DEBUG_MODE_NONE;
    s_requested_end = DEBUG_END_NONE;
    s_pins_taken = false;
    s_started = false;
    s_active_mode = DEBUG_MODE_NONE;
    s_last_end_reason = DEBUG_END_NONE;
    s_session_count = 0;
}

bool debug_session_request_start(debug_mode_t mode) {
    uint32_t save = spin_lock_blocking(s_lock);
    bool ok = (s_state == DS_IDLE);
    if (ok) {
        s_mode = mode;
        s_state = DS_CLAIM_REQUESTED;
    }
    spin_unlock(s_lock, save);
    return ok;
}

bool debug_session_granted(void) {
    return s_state == DS_ACTIVE;
}

bool debug_session_idle(void) {
    return s_state == DS_IDLE;
}

void debug_session_request_end(debug_end_reason_t reason) {
    uint32_t save = spin_lock_blocking(s_lock);
    if (s_state == DS_CLAIM_REQUESTED || s_state == DS_ACTIVE) {
        s_requested_end = reason;
        s_state = DS_RELEASE_REQUESTED;
    }
    spin_unlock(s_lock, save);
}

bool debug_session_claim_pending(void) {
    return s_state == DS_CLAIM_REQUESTED;
}

bool debug_session_pins_busy(void) {
    return s_state != DS_IDLE;
}

// High-Z, no pull (a pull-up on GP9/TDI would hold an ESP32's MTDI strap
// high through a target reset), input buffer off (E9). The DAP drives
// them from here on.
static void release_jtag_reset_pin(void) {
    gpio_init(DEBUG_JTAG_RESET_PIN);
    gpio_disable_pulls(DEBUG_JTAG_RESET_PIN);
    gpio_set_input_enabled(DEBUG_JTAG_RESET_PIN, false);
}

bool debug_session_core0_task(debug_session_event_t *ev) {
    ds_state_t state = s_state;

    if (state == DS_CLAIM_REQUESTED) {
        if (!s_pins_taken) {
            spi_master_release_pins();
            s_active_mode = s_mode;
            if (s_active_mode == DEBUG_MODE_JTAG) {
                release_jtag_reset_pin();
            }
            s_pins_taken = true;
        }
        uint32_t save = spin_lock_blocking(s_lock);
        bool granted = (s_state == DS_CLAIM_REQUESTED);
        if (granted) {
            s_state = DS_ACTIVE;
        }
        spin_unlock(s_lock, save);
        if (!granted) {
            return false; // ended before it began; handled next call
        }
        s_session_count++;
        s_started = true;
        ev->active = 1;
        ev->mode = (uint8_t)s_active_mode;
        ev->end_reason = DEBUG_END_NONE;
        ev->reserved = 0;
        ev->session_count = s_session_count;
        return true;
    }

    if (state == DS_RELEASE_REQUESTED) {
        if (s_pins_taken) {
            if (s_active_mode == DEBUG_MODE_JTAG) {
                gpio_deinit(DEBUG_JTAG_RESET_PIN);
            }
            spi_master_reclaim_pins();
            s_pins_taken = false;
        }
        uint32_t save = spin_lock_blocking(s_lock);
        debug_end_reason_t reason = s_requested_end;
        s_state = DS_IDLE;
        spin_unlock(s_lock, save);

        s_last_end_reason = reason;
        if (!s_started) {
            return false;
        }
        s_started = false;
        ev->active = 0;
        ev->mode = (uint8_t)s_active_mode;
        ev->end_reason = (uint8_t)reason;
        ev->reserved = 0;
        ev->session_count = s_session_count;
        return true;
    }
    return false;
}

void debug_session_get_status(debug_session_status_t *out) {
    out->active = s_started;
    out->mode = s_started ? (uint8_t)s_active_mode : DEBUG_MODE_NONE;
    out->last_end_reason = (uint8_t)s_last_end_reason;
    out->session_count = s_session_count;
}
