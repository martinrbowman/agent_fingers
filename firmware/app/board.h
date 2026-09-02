#pragma once
#include <stdint.h>

typedef enum {
    RESET_CAUSE_POWER_ON = 0,
    RESET_CAUSE_WATCHDOG,
} reset_cause_t;

// Runs the pin-claim self-check, records reset cause, enters SAFE_STATE_BOOT,
// and arms the watchdog. Call once at the top of main().
void board_init(void);

// Pets the watchdog. Call every main-loop iteration.
void board_task(void);

uint64_t board_time_us(void);
reset_cause_t board_reset_cause(void);
