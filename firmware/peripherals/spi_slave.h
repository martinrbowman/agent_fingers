#pragma once
#include <stdbool.h>
#include <stdint.h>

// SPI slave via PIO (GP12 SCK / GP13 MOSI / GP14 MISO / GP15 CS_n), mode 0
// only — same scope as spi_master.h's master. Not gated by ARM_OUTPUTS.
//
// Deliberately simplified relative to a full register-map responder: there
// is no SPI_SLAVE_READ opcode in the plan's frozen opcode list to expose
// received data remotely (mirroring i2c_bus.h's slave). MISO continuously
// repeats a small fixed 8-byte pattern (0x00..0x07) for as long as the
// master keeps clocking; MOSI is captured into a ring buffer purely
// for counting, not remote inspection. CS is not synchronized in the PIO
// program itself — correctness relies on the connected master not
// toggling SCK while this slave is deselected (standard SPI etiquette,
// the same assumption our own PIO SPI master satisfies). MISO is not
// tri-stated when deselected — fine for a single-slave bus, would need
// extension for a shared multi-device bus.
//
// UNVERIFIED: no master has been wired to this slave yet (self-test
// requires jumpering this slave's pins to the master's, GP8-11 —
// planned, not yet run). Built with the same care as the master, but
// nothing about actual behavior against a real SPI master is confirmed.

void spi_slave_init(void);

// Polls the CS pin for falling edges (transaction-boundary counting) and
// the DMA ring positions. Call every main-loop iteration.
void spi_slave_task(void);

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
