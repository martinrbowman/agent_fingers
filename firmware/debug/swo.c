#include "debug/swo.h"
#include "debug/debug_pins.h"
#include "swo_uart_rx.pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"

// PIO1 hosts the SPI master's program too; that SM is stopped while a debug
// session owns GP8-11, and SWO only runs inside a session.
#define SWO_PIO pio1

// Same sentinel as the other continuous DMA rings: RP2350 reserves the top
// 4 bits of TRANS_COUNT for MODE, so "unbounded" is the count field's max.
#define SWO_CONTINUOUS_TRANSFER_COUNT DMA_CH0_TRANS_COUNT_COUNT_BITS

// RP2350 DMA ring wrap masks address bits: the ring must be aligned to its size.
static uint8_t s_buf[SWO_BUFFER_SIZE] __attribute__((aligned(SWO_BUFFER_SIZE)));

static uint  s_sm;
static uint  s_offset;
static int   s_dma;
static bool  s_ready;
static bool  s_active;
static bool  s_overrun;
static float s_clkdiv = 1.0f;
static uint32_t s_read_pos; // absolute index of the next unread byte

void swo_init(void) {
    int sm = pio_claim_unused_sm(SWO_PIO, false);
    if (sm < 0 || !pio_can_add_program(SWO_PIO, &swo_uart_rx_program)) {
        return; // no SWO rather than no probe
    }
    s_sm = (uint)sm;
    s_offset = pio_add_program(SWO_PIO, &swo_uart_rx_program);
    s_dma = dma_claim_unused_channel(false);
    s_ready = s_dma >= 0;
}

uint32_t swo_set_baudrate(uint32_t baud) {
    if (!s_ready || baud == 0) {
        return 0;
    }
    float sys = (float)clock_get_hz(clk_sys);
    float div = sys / (8.0f * (float)baud);
    if (div < 1.0f || div > 65535.0f) {
        return 0;
    }
    // The divider is 16.8 fixed point; report the rate it really gives.
    div = (float)((uint32_t)(div * 256.0f + 0.5f)) / 256.0f;
    s_clkdiv = div;
    if (s_active) {
        pio_sm_set_clkdiv(SWO_PIO, s_sm, s_clkdiv);
    }
    return (uint32_t)(sys / (8.0f * div) + 0.5f);
}

static uint32_t total_written(void) {
    return SWO_CONTINUOUS_TRANSFER_COUNT - dma_channel_hw_addr(s_dma)->transfer_count;
}

bool swo_start(void) {
    if (!s_ready) {
        return false;
    }
    swo_stop();
    gpio_pull_up(DBG_PIN_SWO);               // idle high; E9 doesn't affect pull-ups
    gpio_set_input_enabled(DBG_PIN_SWO, true);

    pio_sm_config c = swo_uart_rx_program_get_default_config(s_offset);
    sm_config_set_in_pins(&c, DBG_PIN_SWO);
    sm_config_set_jmp_pin(&c, DBG_PIN_SWO);
    sm_config_set_in_shift(&c, true, false, 32); // shift right, no autopush
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, s_clkdiv);
    pio_sm_init(SWO_PIO, s_sm, s_offset, &c);

    dma_channel_config d = dma_channel_get_default_config(s_dma);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_8);
    channel_config_set_read_increment(&d, false);
    channel_config_set_write_increment(&d, true);
    channel_config_set_dreq(&d, pio_get_dreq(SWO_PIO, s_sm, false));
    channel_config_set_ring(&d, true, 12); // 2^12 = SWO_BUFFER_SIZE
    // Byte lands in ISR bits 31..24 (shift right): read the top byte lane.
    dma_channel_configure(s_dma, &d, s_buf, (const volatile uint8_t *)&SWO_PIO->rxf[s_sm] + 3,
                          SWO_CONTINUOUS_TRANSFER_COUNT, true);

    s_read_pos = 0;
    s_overrun = false;
    pio_sm_set_enabled(SWO_PIO, s_sm, true);
    s_active = true;
    return true;
}

void swo_stop(void) {
    if (!s_active) {
        return;
    }
    pio_sm_set_enabled(SWO_PIO, s_sm, false);
    dma_channel_abort(s_dma);
    s_active = false;
    gpio_set_input_enabled(DBG_PIN_SWO, false);
    gpio_disable_pulls(DBG_PIN_SWO);
}

bool swo_active(void) {
    return s_active;
}

bool swo_overrun(void) {
    return s_overrun;
}

uint32_t swo_available(void) {
    if (!s_ready) {
        return 0;
    }
    uint32_t total = total_written();
    if (total - s_read_pos > SWO_BUFFER_SIZE) {
        s_overrun = true;
        s_read_pos = total - SWO_BUFFER_SIZE;
    }
    return total - s_read_pos;
}

uint32_t swo_read(uint8_t *out, uint32_t max) {
    uint32_t n = swo_available();
    if (n > max) {
        n = max;
    }
    for (uint32_t i = 0; i < n; i++) {
        out[i] = s_buf[(s_read_pos + i) % SWO_BUFFER_SIZE];
    }
    s_read_pos += n;
    return n;
}
