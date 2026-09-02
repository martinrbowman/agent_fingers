#include "app/board.h"
#include "io/pin_claims.h"
#include "pico/time.h"
#include "hardware/watchdog.h"

// The watchdog now protects an armed digital bank too (digital_bank_task()
// forces safe on lease expiry well before this fires), so this stays a
// generous "catch a hung firmware" backstop, petted every loop iteration.
#define BOARD_WATCHDOG_TIMEOUT_MS 4000

static reset_cause_t s_reset_cause;

void board_init(void) {
    s_reset_cause = watchdog_caused_reboot() ? RESET_CAUSE_WATCHDOG : RESET_CAUSE_POWER_ON;

    pin_claims_self_check();
    // safe_state_enter(SAFE_STATE_BOOT) happens in main(), after
    // digital_bank_init() — it calls digital_bank_force_safe(), which needs
    // the PIO state machines already claimed and running.

    watchdog_enable(BOARD_WATCHDOG_TIMEOUT_MS, true);
}

void board_task(void) {
    watchdog_update();
}

uint64_t board_time_us(void) {
    return time_us_64();
}

reset_cause_t board_reset_cause(void) {
    return s_reset_cause;
}
