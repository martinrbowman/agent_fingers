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

// Returns false (nothing transferred) if mode != 0 (only mode 0 is
// implemented this pass) or len > SPI_MASTER_MAX_LEN. rx_data_out must be
// sized for len bytes. Full-duplex: exactly len bytes go out and len bytes
// come back, in lockstep.
bool spi_master_xfer(uint8_t mode, uint32_t hz, const uint8_t *tx_data, uint16_t len,
                      uint8_t *rx_data_out, spi_master_xfer_result_t *out);
