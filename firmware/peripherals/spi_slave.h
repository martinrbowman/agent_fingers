#pragma once
#include <stdbool.h>
#include <stdint.h>

// SPI slave via PIO (GP12 SCK / GP13 MOSI / GP14 MISO / GP15 CS_n), mode 0
// only — same scope as spi_master.h's master. Not gated by ARM_OUTPUTS.
//
// Simplified relative to a full register-map responder: MISO continuously
// repeats a small fixed 8-byte pattern (0x00..0x07) for as long as the
// master keeps clocking. MOSI is captured into a 256-byte ring and drained
// with spi_slave_read() (SPI_SLAVE_READ) -- e.g. to sniff a target's SPI
// bus. CS is not synchronized in the PIO
// program itself — correctness relies on the connected master not
// toggling SCK while this slave is deselected (standard SPI etiquette,
// the same assumption our own PIO SPI master satisfies). MISO is not
// tri-stated when deselected — fine for a single-slave bus, would need
// extension for a shared multi-device bus.
//
// Verified against this board's own PIO master (GP8-11 jumpered to
// GP12-15): byte counts exact, MISO pattern and MOSI data correct, with
// the known no-CS-gating one-bit-shift characteristic (~1 in 8 transfers).

void spi_slave_init(void);

typedef struct {
    uint32_t bytes_received;
    uint32_t bytes_sent;        // NOT a count of bytes actually clocked out —
                                  // it's the TX DMA's read position, which
                                  // advances as soon as the (4-deep FIFO +
                                  // 1-deep OSR =) 5-word pipeline is
                                  // eagerly pre-filled on enable, before any
                                  // real SCK activity. Confirmed on
                                  // hardware: reads exactly 5 at idle,
                                  // stable across repeated polls, zero SCK
                                  // toggling. Only meaningful as a rough
                                  // "has more than ~5 bytes been consumed"
                                  // signal during sustained real activity,
                                  // not as an exact transmitted-byte count.
    uint32_t transaction_count;
} spi_slave_config_result_t;

// Enables or disables the slave. Always resets the RX ring, read
// position, CS-edge counter, and byte counters — call again (even with
// the same parameters, or to disable) to fetch-and-reset a fresh baseline.
void spi_slave_config(bool enabled, spi_slave_config_result_t *out);

#define SPI_SLAVE_RX_RING_BYTES 256u

// Drains up to `limit` MOSI bytes received since the previous read into
// data_out (caller sizes it for `limit`). Returns false if the slave isn't
// enabled. If the host fell more than a ring's worth behind, the oldest
// bytes are gone: reading resumes at the oldest one still present and the
// gap is added to *ring_overrun_out (cumulative since enable).
bool spi_slave_read(uint16_t limit, uint8_t *data_out, uint16_t *returned_out,
                    uint32_t *total_received_out, uint32_t *ring_overrun_out);
