#pragma once
#include <stdbool.h>
#include <stdint.h>

// JTAG physical layer, core1 only, bit-banged through SIO (debug_pins.h:
// TCK GP8, TDI GP9, TDO GP10, TMS GP11, nRESET GP22 open-drain). Timing
// follows ARM's CMSIS-DAP reference (JTAG_DP.c): TDI/TMS set while TCK
// is low, TDO sampled just before the rising edge.
//
// ESP32 strapping safety: MTDI (GPIO12) selects the ESP32's flash voltage
// at reset, and high means 1.8 V -- a 3.3 V-flash module then won't boot.
// So TDI is parked low whenever it isn't actively shifting, and held low
// across nRESET release (the latch point). Scans *during* reset shift
// normally -- ARM connect/halt-under-reset needs them.

void jtag_port_on(void);
void jtag_port_off(void);
void jtag_set_clock(uint32_t hz); // never faster than asked; capped by the loop speed
// Measures the bit loop's own cost so jtag_set_clock() is accurate. Call
// once on core1 before any session; returns the fastest achievable clock.
uint32_t jtag_calibrate(void);

// DAP_JTAG_Sequence element: info bits 0-5 TCK cycles (0 = 64), bit 6 TMS
// value, bit 7 capture TDO. tdi supplies (cycles+7)/8 bytes, LSB first;
// tdo receives as many when capturing.
void jtag_sequence(uint8_t info, const uint8_t *tdi, uint8_t *tdo);

// DAP_SWJ_Sequence in JTAG mode: bits clocked out on TMS, LSB first.
void jtag_tms_sequence(const uint8_t *data, uint32_t bits);

// DAP_SWJ_Pins in JTAG mode: bit0 TCK, bit1 TMS, bit2 TDI, bit3 TDO,
// bit7 nRESET (nTRST, bit5, isn't wired).
uint8_t jtag_pins_read(void);
void jtag_pins_write(uint8_t value, uint8_t select);

// -- ARM JTAG-DP access (CMSIS-DAP reference JTAG_DP.c) ------------------------
// Chain description from DAP_JTAG_Configure: device count and each TAP's IR
// length (TDI end first). Transfers address one TAP by index; the others
// are held in BYPASS.
#define JTAG_MAX_DEVICES 8u

bool jtag_configure(uint8_t count, const uint8_t *ir_lengths);
bool jtag_select_device(uint8_t index); // false if index >= configured count
void jtag_set_idle_cycles(uint8_t idle_cycles);

#define JTAG_IR_ABORT  0x08u
#define JTAG_IR_DPACC  0x0Au
#define JTAG_IR_APACC  0x0Bu
#define JTAG_IR_IDCODE 0x0Eu

void jtag_ir(uint32_t ir);
uint32_t jtag_read_idcode(void);
void jtag_write_abort(uint32_t data);
// request bits as DAP_Transfer: 0 APnDP (selects nothing here -- the caller
// has already loaded DPACC/APACC), 1 RnW, 2 A2, 3 A3. Returns an ack in
// CMSIS-DAP encoding (1 OK, 2 WAIT, other = fault/protocol error).
uint8_t jtag_transfer(uint32_t request, uint32_t *data);
