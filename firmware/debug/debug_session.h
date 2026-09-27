#pragma once
#include <stdbool.h>
#include <stdint.h>

// Debug-probe session state shared between the cores (see
// debug_probe_design.md). core1 (CMSIS-DAP, on USB) requests sessions;
// core0 owns the SPI master's pins (GP8-11, plus GP22 in JTAG mode) and
// hands them over. A session always wins: a pending claim aborts an
// in-flight SPI master transfer. Sessions are independent of the MCP
// arm/lease/safe-state machinery.

typedef enum {
    DEBUG_MODE_NONE = 0,
    DEBUG_MODE_SWD  = 1,
    DEBUG_MODE_JTAG = 2,
} debug_mode_t;

typedef enum {
    DEBUG_END_NONE       = 0,
    DEBUG_END_DISCONNECT = 1, // DAP_Disconnect
    DEBUG_END_TIMEOUT    = 2, // no DAP packet for DAP_INACTIVITY_TIMEOUT
    DEBUG_END_USB        = 3, // USB unmounted mid-session
} debug_end_reason_t;

// Payload of a debug-session EVENT (opcode OPCODE_DEBUG_SESSION_EVENT).
typedef struct __attribute__((packed)) {
    uint8_t  active;      // 1 = session started, 0 = ended
    uint8_t  mode;        // debug_mode_t
    uint8_t  end_reason;  // debug_end_reason_t (0 on start)
    uint8_t  reserved;
    uint32_t session_count;
} debug_session_event_t;

void debug_session_init(void);

// -- core1 (DAP) side ---------------------------------------------------------
// Returns false if a previous session hasn't finished handing back yet;
// retry later.
bool debug_session_request_start(debug_mode_t mode);
bool debug_session_granted(void); // pins are the DAP's now
bool debug_session_idle(void);
void debug_session_request_end(debug_end_reason_t reason);

// -- core0 side ---------------------------------------------------------------
// True while a claim is waiting -- long-running SPI master work polls this
// and aborts.
bool debug_session_claim_pending(void);
// True whenever the SPI master's pins aren't the SPI master's.
bool debug_session_pins_busy(void);
// Performs any pending handover. Returns true with *ev filled when a
// session started or ended (caller emits the EVENT). Call every core0
// main-loop iteration.
bool debug_session_core0_task(debug_session_event_t *ev);

typedef struct {
    bool     active;
    uint8_t  mode;
    uint8_t  last_end_reason;
    uint32_t session_count;
} debug_session_status_t;

void debug_session_get_status(debug_session_status_t *out);
