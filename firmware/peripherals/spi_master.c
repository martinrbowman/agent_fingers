#include "peripherals/spi_master.h"
#include "spi_master.pio.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"

#define SPI_MASTER_PIO      pio1 // PIO0 is fully claimed by the digital bank
#define SPI_MASTER_SCK_PIN  8
#define SPI_MASTER_MOSI_PIN 9
#define SPI_MASTER_MISO_PIN 10
#define SPI_MASTER_CS_PIN   11

static uint s_sm;
static int  s_tx_dma_chan;
static int  s_rx_dma_chan;
static uint8_t s_tx_buffer[SPI_MASTER_MAX_LEN];
static uint8_t s_rx_buffer[SPI_MASTER_MAX_LEN];

// The PIO program shifts LSB-first internally, on both directions. On
// TX, autopull's shift_right reads the OSR starting from bit 0, so a byte
// written to the FIFO as-is would go out LSB-first on the wire; reversing
// it first turns that into true MSB-first framing, what virtually every
// real SPI device expects (first bit out = that device's MSB).
//
// RX needs the *same* reversal, for a different reason: autopush's
// shift_right left-justifies each received byte in the 32-bit ISR (new
// bits enter at bit 31, existing content shifts toward bit 0), so once
// read from the right FIFO byte lane (+3 offset, see below) the raw byte
// has its *chronologically first received bit* sitting in bit 0, not
// bit 7. For a true MSB-first sender, the first bit received is that
// sender's MSB -- so without reversing, the reconstructed byte comes out
// bit-reversed relative to what was actually sent.
//
// A hardware bring-up pass briefly removed this RX reversal because it
// made the master<->slave round-trip pass -- but that was two bugs
// canceling out: spi_slave.c's fixed MISO pattern wasn't (at the time)
// pre-reversed for its own TX, so skipping the master's RX reversal
// happened to match it. Confirmed wrong by a true MOSI<->MISO self-loop
// test (GP9<->GP10): received data came back exactly bit-reversed from
// what was sent. Both sides are now fixed to genuinely use MSB-first
// wire framing throughout: this reversal restored here, and
// spi_slave.c's pattern pre-reversed to match.
static uint8_t reverse_bits8(uint8_t v) {
    v = (uint8_t)(((v & 0xF0u) >> 4) | ((v & 0x0Fu) << 4));
    v = (uint8_t)(((v & 0xCCu) >> 2) | ((v & 0x33u) << 2));
    v = (uint8_t)(((v & 0xAAu) >> 1) | ((v & 0x55u) << 1));
    return v;
}

void spi_master_init(void) {
    uint offset = pio_add_program(SPI_MASTER_PIO, &spi_master_mode0_program);
    s_sm = pio_claim_unused_sm(SPI_MASTER_PIO, true);

    pio_gpio_init(SPI_MASTER_PIO, SPI_MASTER_SCK_PIN);
    pio_gpio_init(SPI_MASTER_PIO, SPI_MASTER_MOSI_PIN);
    pio_gpio_init(SPI_MASTER_PIO, SPI_MASTER_MISO_PIN);
    pio_sm_set_consecutive_pindirs(SPI_MASTER_PIO, s_sm, SPI_MASTER_SCK_PIN, 1, true);
    pio_sm_set_consecutive_pindirs(SPI_MASTER_PIO, s_sm, SPI_MASTER_MOSI_PIN, 1, true);
    pio_sm_set_consecutive_pindirs(SPI_MASTER_PIO, s_sm, SPI_MASTER_MISO_PIN, 1, false);

    pio_sm_config c = spi_master_mode0_program_get_default_config(offset);
    sm_config_set_out_pins(&c, SPI_MASTER_MOSI_PIN, 1);
    sm_config_set_in_pins(&c, SPI_MASTER_MISO_PIN);
    sm_config_set_sideset_pins(&c, SPI_MASTER_SCK_PIN);
    sm_config_set_out_shift(&c, true, true, 8); // shift right (LSB-first), autopull
    sm_config_set_in_shift(&c, true, true, 8);  // shift right (LSB-first), autopush
    pio_sm_init(SPI_MASTER_PIO, s_sm, offset, &c);
    pio_sm_set_enabled(SPI_MASTER_PIO, s_sm, true);

    gpio_init(SPI_MASTER_CS_PIN);
    gpio_set_dir(SPI_MASTER_CS_PIN, GPIO_OUT);
    gpio_put(SPI_MASTER_CS_PIN, 1); // idle deasserted (active low)

    s_tx_dma_chan = dma_claim_unused_channel(true);
    s_rx_dma_chan = dma_claim_unused_channel(true);
}

static uint32_t clamp_hz(uint32_t requested_hz) {
    uint32_t hz = requested_hz;
    if (hz < SPI_MASTER_MIN_HZ) {
        hz = SPI_MASTER_MIN_HZ;
    }
    if (hz > SPI_MASTER_MAX_HZ) {
        hz = SPI_MASTER_MAX_HZ;
    }
    return hz;
}

bool spi_master_xfer(uint8_t mode, uint32_t hz, const uint8_t *tx_data, uint16_t len,
                      uint8_t *rx_data_out, spi_master_xfer_result_t *out) {
    if (mode != 0) {
        return false;
    }
    if (len > SPI_MASTER_MAX_LEN) {
        return false;
    }

    uint32_t granted_hz = clamp_hz(hz);

    if (len == 0) {
        if (out) {
            out->granted_hz = granted_hz;
        }
        return true;
    }

    uint32_t clk_sys_hz = clock_get_hz(clk_sys);
    // 2 instructions per bit, each with a [1] delay = 2 SM cycles each,
    // so 4 SM cycles per bit.
    float clkdiv = (float)clk_sys_hz / (4.0f * (float)granted_hz);

    pio_sm_set_enabled(SPI_MASTER_PIO, s_sm, false);
    pio_sm_clear_fifos(SPI_MASTER_PIO, s_sm);
    pio_sm_restart(SPI_MASTER_PIO, s_sm);
    pio_sm_set_clkdiv(SPI_MASTER_PIO, s_sm, clkdiv);

    for (uint16_t i = 0; i < len; i++) {
        s_tx_buffer[i] = reverse_bits8(tx_data[i]);
    }

    dma_channel_config tx_cfg = dma_channel_get_default_config(s_tx_dma_chan);
    channel_config_set_transfer_data_size(&tx_cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&tx_cfg, true);
    channel_config_set_write_increment(&tx_cfg, false);
    channel_config_set_dreq(&tx_cfg, pio_get_dreq(SPI_MASTER_PIO, s_sm, true));

    dma_channel_config rx_cfg = dma_channel_get_default_config(s_rx_dma_chan);
    channel_config_set_transfer_data_size(&rx_cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&rx_cfg, false);
    channel_config_set_write_increment(&rx_cfg, true);
    channel_config_set_dreq(&rx_cfg, pio_get_dreq(SPI_MASTER_PIO, s_sm, false));

    gpio_put(SPI_MASTER_CS_PIN, 0);
    pio_sm_set_enabled(SPI_MASTER_PIO, s_sm, true);

    // Autopush with shift_right and an 8-bit threshold left-justifies each
    // received byte in the 32-bit ISR (new bits enter at bit 31 and shift
    // toward bit 0) -- the opposite of autopull's shift_right, which reads
    // starting from bit 0. So a plain 8-bit DMA read from the FIFO's base
    // address silently grabs the wrong (always-zero) byte lane; +3 reads
    // the top byte, where the real data actually lands. Found by hardware
    // bring-up: DMA never hung and byte *counts* were always right, but
    // every received value came back as exactly 0x00.
    dma_channel_configure(s_rx_dma_chan, &rx_cfg, s_rx_buffer,
                           (const volatile uint8_t *)&SPI_MASTER_PIO->rxf[s_sm] + 3, len, true);
    dma_channel_configure(s_tx_dma_chan, &tx_cfg, &SPI_MASTER_PIO->txf[s_sm], s_tx_buffer, len, true);

    dma_channel_wait_for_finish_blocking(s_rx_dma_chan);

    gpio_put(SPI_MASTER_CS_PIN, 1);
    pio_sm_set_enabled(SPI_MASTER_PIO, s_sm, false);

    for (uint16_t i = 0; i < len; i++) {
        rx_data_out[i] = reverse_bits8(s_rx_buffer[i]);
    }

    if (out) {
        out->granted_hz = granted_hz;
    }
    return true;
}
