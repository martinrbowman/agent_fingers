#pragma once
#include <stdbool.h>
#include <stdint.h>

// SPI master via PIO (GP8 SCK / GP9 MOSI / GP10 MISO / GP11 CS0_n) —
// not hardware SPI0, per the plan's pin-rework rationale (pin-placement
// flexibility). Mode 0 (CPOL=0, CPHA=0) only this pass; see protocol.h.
// Not gated by ARM_OUTPUTS — a bus, not a static output level.
//
// Blocking (unlike UART's fire-and-forget): SPI_XFER waits for the whole
// transfer to finish before responding, since the RX data must come back
// in the same response. SPI_MASTER_MIN_HZ is a deliberate *safety* floor
// (not a hardware limit) chosen to bound the worst-case block time to a
// small fraction of the watchdog timeout, not the lowest rate the PIO
// clkdiv could actually reach.

#define SPI_MASTER_MIN_HZ  10000u
#define SPI_MASTER_MAX_HZ  1000000u
#define SPI_MASTER_MAX_LEN 500u // fits one protocol frame either direction

void spi_master_init(void);

typedef struct {
    uint32_t granted_hz;
} spi_master_xfer_result_t;

typedef enum {
    SPI_MASTER_OK = 0,
    SPI_MASTER_INVALID, // mode != 0 (only mode 0 this pass) or len too large
    SPI_MASTER_BUSY,    // pins owned by a debug session, or claimed mid-transfer
} spi_master_status_t;

// rx_data_out must be sized for len bytes. Full-duplex: exactly len bytes
// go out and len bytes come back, in lockstep. A debug session claiming
// the pins aborts a transfer in progress (SPI_MASTER_BUSY; rx data then
// meaningless).
spi_master_status_t spi_master_xfer(uint8_t mode, uint32_t hz, const uint8_t *tx_data, uint16_t len,
                                     uint8_t *rx_data_out, spi_master_xfer_result_t *out);

// Debug-probe handover (core0 only, via debug_session.c): release puts
// GP8-11 high-Z with no pulls and input buffers off; reclaim restores the
// SPI master's pin setup.
void spi_master_release_pins(void);
void spi_master_reclaim_pins(void);
