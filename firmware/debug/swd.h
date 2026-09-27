#pragma once
#include <stdbool.h>
#include <stdint.h>

// SWD physical layer, core1 only, bit-banged through SIO (debug_pins.h).
// Timing and turnaround handling follow ARM's CMSIS-DAP reference
// (SW_DP.c): host drives SWDIO while SWCLK is low and the target samples
// on the rising edge; host samples SWDIO just before the rising edge.

// SWD transfer acknowledges, as CMSIS-DAP reports them.
#define SWD_ACK_OK    0x1u
#define SWD_ACK_WAIT  0x2u
#define SWD_ACK_FAULT 0x4u
#define SWD_ACK_ERROR 0x8u // parity error (our own), CMSIS-DAP "protocol error"

void swd_port_on(void);   // SWCLK/SWDIO driven high, nRESET released
void swd_port_off(void);  // all probe pins back to high-Z, no pulls, IE off

void swd_set_clock(uint32_t hz); // never faster than asked; capped by the loop speed
// Measures the bit loop's own cost so swd_set_clock() is accurate. Call
// once on core1 before any session; returns the fastest achievable clock.
uint32_t swd_calibrate(void);
void swd_configure(uint8_t turnaround, bool data_phase);
void swd_set_idle_cycles(uint8_t idle_cycles);

// Line sequence on SWDIO, LSB first (DAP_SWJ_Sequence / SWD_Sequence output).
void swd_seq_out(const uint8_t *data, uint32_t bits);
// Capture `bits` bits from SWDIO with SWDIO released (SWD_Sequence input).
void swd_seq_in(uint8_t *data, uint32_t bits);

// One SWD transfer. request bits: 0 APnDP, 1 RnW, 2 A2, 3 A3. For writes
// *data is sent; for reads *data receives the value (data may be NULL).
// Returns an SWD_ACK_* value (or the raw 3-bit ack on protocol error).
uint8_t swd_transfer(uint32_t request, uint32_t *data);

// DAP_SWJ_Pins in SWD mode: bit0 SWCLK, bit1 SWDIO, bit7 nRESET.
uint8_t swd_pins_read(void);
void swd_pins_write(uint8_t value, uint8_t select);
