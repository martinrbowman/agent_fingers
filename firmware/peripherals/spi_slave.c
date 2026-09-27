#include "peripherals/spi_slave.h"
#include "spi_slave.pio.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"

#define SPI_SLAVE_PIO      pio2 // PIO0 = digital bank, PIO1 = SPI master
#define SPI_SLAVE_SCK_PIN  12
#define SPI_SLAVE_MOSI_PIN 13
#define SPI_SLAVE_MISO_PIN 14
#define SPI_SLAVE_CS_PIN   15

#define SPI_SLAVE_RX_BUFFER_SAMPLES SPI_SLAVE_RX_RING_BYTES // ring, 1 byte each
#define SPI_SLAVE_RX_RING_SIZE_BITS 8u   // 2^8 = 256

#define SPI_SLAVE_TX_PATTERN_SAMPLES 8u
#define SPI_SLAVE_TX_RING_SIZE_BITS  3u  // 2^3 = 8

// RP2350's DMA transfer-count register reserves its top 4 bits for a MODE
// field -- valid count range is 0 to 2^28-1 (DMA_CH0_TRANS_COUNT_COUNT_BITS),
// not the full 32 bits. Passing plain 0xFFFFFFFFu as an "unbounded, run
// until stopped" sentinel sets those MODE bits to garbage instead of 0, a
// real bug found during hardware bring-up (not just cosmetic -- it also
// pinned the count register at a constant, non-decrementing 0xFFFFFFFF on
// readback, exactly the sentinel value, elsewhere used to diagnose this).
// This is the correct "as many transfers as this field can hold" sentinel.
#define SPI_SLAVE_CONTINUOUS_TRANSFER_COUNT DMA_CH0_TRANS_COUNT_COUNT_BITS

static uint s_sm;
static int  s_tx_dma_chan;
static int  s_rx_dma_chan;

// RP2350 DMA ring wrap masks address bits, so these must be aligned to
// their own size — same issue and fix as digital_bank's/adc_scan's/
// uart_port's ring buffers.
static uint8_t s_rx_buffer[SPI_SLAVE_RX_BUFFER_SAMPLES]
    __attribute__((aligned(SPI_SLAVE_RX_BUFFER_SAMPLES)));
// Pre-reversed 0x00..0x07, matching spi_master.c's TX convention: autopull
// shift_right reads starting from bit 0, so a byte written as-is would go
// out LSB-first on the wire. Reversing each value here first means MISO
// actually presents 0x00..0x07 in true MSB-first order to a real master --
// found missing during hardware bring-up: this array used to hold the raw
// 0..7 values, which happened to work against this project's own PIO
// master only because *its* RX reversal had also (wrongly) been removed
// at the same time. Confirmed by a master self-loop test (GP9<->GP10)
// coming back bit-reversed once that RX reversal was restored; see
// spi_master.c's reverse_bits8() comment for the full story.
static const uint8_t s_tx_pattern[SPI_SLAVE_TX_PATTERN_SAMPLES]
    __attribute__((aligned(SPI_SLAVE_TX_PATTERN_SAMPLES))) =
        {0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0}; // reverse_bits8(0..7)

static bool s_enabled;

// CS falling edges, counted by a GPIO IRQ rather than main-loop polling:
// this board's own SPI master runs on the same core and blocks for the
// whole transfer, so CS goes low and back high between two polls and a
// polled counter never saw a single transaction (found on hardware:
// transaction_count stayed 0 with CS wired and continuity confirmed).
static volatile uint32_t s_transaction_count;

static void cs_irq_handler(void) {
    if (gpio_get_irq_event_mask(SPI_SLAVE_CS_PIN) & GPIO_IRQ_EDGE_FALL) {
        gpio_acknowledge_irq(SPI_SLAVE_CS_PIN, GPIO_IRQ_EDGE_FALL);
        s_transaction_count++;
    }
}

static void set_cs_irq(bool enabled) {
    if (enabled) {
        // The raw edge latch fills regardless of the enable bit -- drop
        // anything stale from while the slave was off.
        gpio_acknowledge_irq(SPI_SLAVE_CS_PIN, GPIO_IRQ_EDGE_FALL);
    }
    gpio_set_irq_enabled(SPI_SLAVE_CS_PIN, GPIO_IRQ_EDGE_FALL, enabled);
}

static void set_listen_inputs(bool enabled) {
    gpio_set_input_enabled(SPI_SLAVE_SCK_PIN, enabled);
    gpio_set_input_enabled(SPI_SLAVE_MOSI_PIN, enabled);
}


static uint32_t s_rx_read_pos; // absolute byte index of the next unread byte
static uint32_t s_rx_overrun_count;

void spi_slave_init(void) {
    uint offset = pio_add_program(SPI_SLAVE_PIO, &spi_slave_mode0_program);
    s_sm = pio_claim_unused_sm(SPI_SLAVE_PIO, true);

    pio_gpio_init(SPI_SLAVE_PIO, SPI_SLAVE_SCK_PIN);
    pio_gpio_init(SPI_SLAVE_PIO, SPI_SLAVE_MOSI_PIN);
    pio_gpio_init(SPI_SLAVE_PIO, SPI_SLAVE_MISO_PIN);
    pio_sm_set_consecutive_pindirs(SPI_SLAVE_PIO, s_sm, SPI_SLAVE_MOSI_PIN, 1, false);
    pio_sm_set_consecutive_pindirs(SPI_SLAVE_PIO, s_sm, SPI_SLAVE_MISO_PIN, 1, true);

    pio_sm_config c = spi_slave_mode0_program_get_default_config(offset);
    sm_config_set_in_pins(&c, SPI_SLAVE_MOSI_PIN);
    sm_config_set_out_pins(&c, SPI_SLAVE_MISO_PIN, 1);
    sm_config_set_in_shift(&c, true, true, 8);  // shift right (LSB-first), autopush
    sm_config_set_out_shift(&c, true, true, 8); // shift right (LSB-first), autopull
    pio_sm_init(SPI_SLAVE_PIO, s_sm, offset, &c);
    // Not enabled here — spi_slave_config() enables/disables per call.

    gpio_init(SPI_SLAVE_CS_PIN);
    gpio_set_dir(SPI_SLAVE_CS_PIN, GPIO_IN);
    gpio_pull_up(SPI_SLAVE_CS_PIN); // idle deasserted if unconnected

    // SCK idles low in Mode 0 (matching the master's own idle
    // convention) — without a pull, a floating/unconnected SCK generates
    // noise that the wait-for-edge PIO loop above would react to as if it
    // were real clock activity. Same class of issue as the digital bank's
    // floating-input pull-down and UART RX's pull-up. Confirmed effective:
    // bytes_received stays at exactly 0 at idle with this pull in place
    // (was the RX-side symptom of the same root cause before this fix).
    // MOSI gets a pull-down too, purely for a deterministic idle sample
    // value, not because SPI specifies one.
    //
    // Separately, and NOT fixed by this pull (different root cause): TX's
    // bytes_sent reads ~5 at idle regardless, because the DMA pipeline
    // (4-deep FIFO + 1-deep OSR) is eagerly pre-filled on enable
    // independent of real SCK activity — see spi_slave_config_result_t.
    gpio_pull_down(SPI_SLAVE_SCK_PIN);
    gpio_pull_down(SPI_SLAVE_MOSI_PIN);
    // RP2350 A2 erratum E9: an input pad with IE on, driven high and then
    // left floating, latches at ~2.2V that the pull-down can't recover --
    // an unplugged master could leave SCK/MOSI there. Keep both input
    // buffers off while the slave is disabled; spi_slave_config() turns
    // them on only for as long as the slave is listening. (CS has a
    // pull-up, which E9 doesn't affect.)
    set_listen_inputs(false);

    s_tx_dma_chan = dma_claim_unused_channel(true);
    s_rx_dma_chan = dma_claim_unused_channel(true);
    s_enabled = false;

    gpio_add_raw_irq_handler(SPI_SLAVE_CS_PIN, cs_irq_handler);
    irq_set_enabled(IO_IRQ_BANK0, true);
}

static void stop_hardware(void) {
    pio_sm_set_enabled(SPI_SLAVE_PIO, s_sm, false);
    dma_channel_abort(s_tx_dma_chan);
    dma_channel_abort(s_rx_dma_chan);
    set_listen_inputs(false);
    set_cs_irq(false);
}

// The DMA channel's own transfer_count register live-decrements from its
// trigger value (SPI_SLAVE_CONTINUOUS_TRANSFER_COUNT, effectively
// unbounded) as transfers happen, so elapsed-byte-count = trigger_value -
// current_value, exact and
// unambiguous regardless of how many bytes have moved or when this is
// read. Deliberately NOT tracked via the write/read address vs. a
// remembered previous offset (a "did it wrap?" scheme) -- found by
// hardware bring-up: a transfer whose length is an exact multiple of the
// 256-byte RX ring lands the write address back at exactly the same spot
// it started, indistinguishable from zero bytes received when only
// polled once per enable/disable cycle (which is all a blocking,
// single-threaded SPI_XFER on the far end allows).
static uint32_t rx_bytes_transferred(void) {
    return SPI_SLAVE_CONTINUOUS_TRANSFER_COUNT - dma_channel_hw_addr(s_rx_dma_chan)->transfer_count;
}

static uint32_t tx_bytes_transferred(void) {
    return SPI_SLAVE_CONTINUOUS_TRANSFER_COUNT - dma_channel_hw_addr(s_tx_dma_chan)->transfer_count;
}

void spi_slave_config(bool enabled, spi_slave_config_result_t *out) {
    uint32_t bytes_received = 0;
    uint32_t bytes_sent = 0;
    if (s_enabled) {
        // Read the live counts before stop_hardware() aborts the DMA
        // channels out from under them.
        bytes_received = rx_bytes_transferred();
        bytes_sent = tx_bytes_transferred();
        stop_hardware();
        s_enabled = false;
    }

    if (out) {
        out->bytes_received = bytes_received;
        out->bytes_sent = bytes_sent;
        out->transaction_count = s_transaction_count;
    }

    s_transaction_count = 0; // CS IRQ already off: stop_hardware() or never enabled
    s_rx_read_pos = 0;
    s_rx_overrun_count = 0;

    if (!enabled) {
        return;
    }

    set_listen_inputs(true);
    pio_sm_clear_fifos(SPI_SLAVE_PIO, s_sm);
    pio_sm_restart(SPI_SLAVE_PIO, s_sm);

    dma_channel_config rx_cfg = dma_channel_get_default_config(s_rx_dma_chan);
    channel_config_set_transfer_data_size(&rx_cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&rx_cfg, false);
    channel_config_set_write_increment(&rx_cfg, true);
    channel_config_set_dreq(&rx_cfg, pio_get_dreq(SPI_SLAVE_PIO, s_sm, false));
    channel_config_set_ring(&rx_cfg, true, SPI_SLAVE_RX_RING_SIZE_BITS); // write ring
    // Autopush with shift_right and an 8-bit threshold left-justifies each
    // received byte in the 32-bit ISR (new bits enter at bit 31 and shift
    // toward bit 0) -- the opposite of autopull's shift_right, which reads
    // starting from bit 0. A plain 8-bit DMA read from the FIFO's base
    // address would silently grab the wrong (always-zero) byte lane; +3
    // reads the top byte, where the real data actually lands. Doesn't
    // affect bytes_received (a count, correct either way) but does affect
    // the actual captured values in s_rx_buffer -- see spi_master.c's RX
    // DMA config for the hardware bring-up story behind this.
    dma_channel_configure(s_rx_dma_chan, &rx_cfg, s_rx_buffer,
                           (const volatile uint8_t *)&SPI_SLAVE_PIO->rxf[s_sm] + 3,
                           SPI_SLAVE_CONTINUOUS_TRANSFER_COUNT, true);

    dma_channel_config tx_cfg = dma_channel_get_default_config(s_tx_dma_chan);
    channel_config_set_transfer_data_size(&tx_cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&tx_cfg, true);
    channel_config_set_write_increment(&tx_cfg, false);
    channel_config_set_dreq(&tx_cfg, pio_get_dreq(SPI_SLAVE_PIO, s_sm, true));
    channel_config_set_ring(&tx_cfg, false, SPI_SLAVE_TX_RING_SIZE_BITS); // read ring, repeats the pattern forever
    dma_channel_configure(s_tx_dma_chan, &tx_cfg, &SPI_SLAVE_PIO->txf[s_sm], s_tx_pattern,
                           SPI_SLAVE_CONTINUOUS_TRANSFER_COUNT, true);

    pio_sm_set_enabled(SPI_SLAVE_PIO, s_sm, true);
    set_cs_irq(true);
    s_enabled = true;
}

// PIO shifts right (LSB-first into the ISR) while the wire is MSB-first, so
// each captured byte is bit-reversed relative to what the master sent --
// same convention and fix as spi_master.c's RX path.
static uint8_t reverse_bits8(uint8_t b) {
    b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
    b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
    b = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
    return b;
}

bool spi_slave_read(uint16_t limit, uint8_t *data_out, uint16_t *returned_out,
                    uint32_t *total_received_out, uint32_t *ring_overrun_out) {
    if (!s_enabled) {
        return false;
    }

    // The DMA transfer count is exact regardless of ring laps (see
    // rx_bytes_transferred()), so no wrap tracking is needed here.
    uint32_t total = rx_bytes_transferred();

    uint32_t oldest_valid = (total > SPI_SLAVE_RX_BUFFER_SAMPLES)
                                 ? (total - SPI_SLAVE_RX_BUFFER_SAMPLES) : 0;
    if (s_rx_read_pos < oldest_valid) {
        s_rx_overrun_count += oldest_valid - s_rx_read_pos;
        s_rx_read_pos = oldest_valid;
    }

    uint16_t returned = 0;
    if (s_rx_read_pos < total) {
        uint32_t want = total - s_rx_read_pos;
        if (want > limit) {
            want = limit;
        }
        for (uint32_t i = 0; i < want; i++) {
            uint32_t idx = (s_rx_read_pos + i) % SPI_SLAVE_RX_BUFFER_SAMPLES;
            data_out[i] = reverse_bits8(s_rx_buffer[idx]);
        }
        returned = (uint16_t)want;
        s_rx_read_pos += want;
    }

    if (returned_out) {
        *returned_out = returned;
    }
    if (total_received_out) {
        *total_received_out = total;
    }
    if (ring_overrun_out) {
        *ring_overrun_out = s_rx_overrun_count;
    }
    return true;
}
