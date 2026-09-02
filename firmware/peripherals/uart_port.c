#include "peripherals/uart_port.h"
#include "hardware/uart.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"

#define UART_INST    uart0
#define UART_TX_PIN  16
#define UART_RX_PIN  17

#define RX_BUFFER_SAMPLES 512u // 2 bytes each (data + PL011 error flags) = 1024-byte ring
#define RX_RING_SIZE_BITS 10u  // 2^10 = 1024 bytes

// RP2350's DMA transfer-count register reserves its top 4 bits for a MODE
// field -- valid count range is 0 to 2^28-1, not the full 32 bits. Plain
// 0xFFFFFFFFu as an "unbounded, run until stopped" sentinel sets those
// MODE bits to garbage instead of 0 -- found during spi_slave.c's hardware
// bring-up (see that file for the full story). Address-wrap tracking here
// means it never produced an observable symptom, but it's still invalid
// per the RP2350 DMA contract; this is the correct sentinel.
#define UART_CONTINUOUS_TRANSFER_COUNT DMA_CH0_TRANS_COUNT_COUNT_BITS

static int s_rx_dma_chan;
static int s_tx_dma_chan;

// RP2350 DMA ring wrap masks address bits, so this must be aligned to its
// own size — same issue as digital_bank's and adc_scan's capture buffers.
static uint16_t s_rx_buffer[RX_BUFFER_SAMPLES]
    __attribute__((aligned(RX_BUFFER_SAMPLES * sizeof(uint16_t))));
static uint8_t s_tx_buffer[512];

static bool     s_configured;
static uint32_t s_rx_write_lap;
static uint32_t s_rx_last_write_offset;
static uint32_t s_rx_read_pos;
static uint32_t s_ring_overrun_count;
static uint32_t s_framing_errors;
static uint32_t s_parity_errors;
static uint32_t s_break_errors;
static uint32_t s_overrun_errors;

void uart_port_init(void) {
    s_rx_dma_chan = dma_claim_unused_channel(true);
    s_tx_dma_chan = dma_claim_unused_channel(true);
    s_configured = false;
}

static uint32_t rx_write_pos_samples(void) {
    uintptr_t base = (uintptr_t)s_rx_buffer;
    uintptr_t cur = (uintptr_t)dma_hw->ch[s_rx_dma_chan].write_addr;
    return (uint32_t)((cur - base) / sizeof(uint16_t));
}

static void rx_poll_position(void) {
    uint32_t pos = rx_write_pos_samples();
    if (pos < s_rx_last_write_offset) {
        s_rx_write_lap++;
    }
    s_rx_last_write_offset = pos;
}

static uint32_t rx_total_written(void) {
    return s_rx_write_lap * RX_BUFFER_SAMPLES + s_rx_last_write_offset;
}

void uart_port_config(uint32_t baud_rate, uint8_t data_bits, uint8_t stop_bits,
                       uint8_t parity, uart_port_config_result_t *out) {
    if (s_configured) {
        dma_channel_abort(s_rx_dma_chan);
        dma_channel_abort(s_tx_dma_chan);
        uart_deinit(UART_INST);
    }

    if (data_bits < 5 || data_bits > 8) {
        data_bits = 8;
    }
    if (stop_bits < 1 || stop_bits > 2) {
        stop_bits = 1;
    }
    uart_parity_t parity_enum = UART_PARITY_NONE;
    if (parity == 1) {
        parity_enum = UART_PARITY_EVEN;
    } else if (parity == 2) {
        parity_enum = UART_PARITY_ODD;
    }

    uint32_t granted = uart_init(UART_INST, baud_rate);
    uart_set_format(UART_INST, data_bits, stop_bits, parity_enum);
    uart_set_fifo_enabled(UART_INST, true);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    // UART idles high, not low — an unconnected RX floats and reads as
    // spurious framing/break errors without a pull-up (same class of issue
    // as the digital bank's floating-input pull-down, opposite polarity
    // because UART's idle convention is the opposite of a default-low bus).
    gpio_pull_up(UART_RX_PIN);

    s_rx_write_lap = 0;
    s_rx_last_write_offset = 0;
    s_rx_read_pos = 0;
    s_ring_overrun_count = 0;
    s_framing_errors = 0;
    s_parity_errors = 0;
    s_break_errors = 0;
    s_overrun_errors = 0;

    dma_channel_config rx_cfg = dma_channel_get_default_config(s_rx_dma_chan);
    channel_config_set_transfer_data_size(&rx_cfg, DMA_SIZE_16); // data + PL011 FE/PE/BE/OE flags
    channel_config_set_read_increment(&rx_cfg, false);
    channel_config_set_write_increment(&rx_cfg, true);
    channel_config_set_dreq(&rx_cfg, uart_get_dreq(UART_INST, false));
    channel_config_set_ring(&rx_cfg, true, RX_RING_SIZE_BITS);
    dma_channel_configure(s_rx_dma_chan, &rx_cfg,
                           s_rx_buffer, &uart_get_hw(UART_INST)->dr,
                           UART_CONTINUOUS_TRANSFER_COUNT, true);

    s_configured = true;

    if (out) {
        out->granted_baud_rate = granted;
    }
}

bool uart_port_write(const uint8_t *data, uint16_t len) {
    if (!s_configured) {
        return false;
    }
    if (dma_channel_is_busy(s_tx_dma_chan)) {
        return false;
    }
    if (len > sizeof(s_tx_buffer)) {
        len = (uint16_t)sizeof(s_tx_buffer);
    }

    for (uint16_t i = 0; i < len; i++) {
        s_tx_buffer[i] = data[i];
    }

    dma_channel_config tx_cfg = dma_channel_get_default_config(s_tx_dma_chan);
    channel_config_set_transfer_data_size(&tx_cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&tx_cfg, true);
    channel_config_set_write_increment(&tx_cfg, false);
    channel_config_set_dreq(&tx_cfg, uart_get_dreq(UART_INST, true));
    dma_channel_configure(s_tx_dma_chan, &tx_cfg,
                           &uart_get_hw(UART_INST)->dr, s_tx_buffer,
                           len, true);
    return true;
}

bool uart_port_read(uint16_t limit, uint8_t *data_out, uint16_t *returned_out,
                     uart_port_status_t *status_out) {
    if (!s_configured) {
        return false;
    }

    rx_poll_position();
    uint32_t total = rx_total_written();

    uint32_t oldest_valid = (total > RX_BUFFER_SAMPLES) ? (total - RX_BUFFER_SAMPLES) : 0;
    if (s_rx_read_pos < oldest_valid) {
        s_ring_overrun_count += (oldest_valid - s_rx_read_pos);
        s_rx_read_pos = oldest_valid;
    }

    uint16_t returned = 0;
    if (s_rx_read_pos < total) {
        uint32_t available = total - s_rx_read_pos;
        uint32_t want = limit;
        if (want > available) {
            want = available;
        }
        if (want > RX_BUFFER_SAMPLES) {
            want = RX_BUFFER_SAMPLES;
        }

        uint32_t ring_start = s_rx_read_pos % RX_BUFFER_SAMPLES;
        for (uint32_t i = 0; i < want; i++) {
            uint32_t idx = (ring_start + i) % RX_BUFFER_SAMPLES;
            uint16_t raw = s_rx_buffer[idx];
            if (raw & (1u << 8)) {
                s_framing_errors++;
            }
            if (raw & (1u << 9)) {
                s_parity_errors++;
            }
            if (raw & (1u << 10)) {
                s_break_errors++;
            }
            if (raw & (1u << 11)) {
                s_overrun_errors++;
            }
            data_out[i] = (uint8_t)(raw & 0xFFu);
        }
        returned = (uint16_t)want;
        s_rx_read_pos += want;
    }

    if (returned_out) {
        *returned_out = returned;
    }
    if (status_out) {
        status_out->framing_errors = s_framing_errors;
        status_out->parity_errors = s_parity_errors;
        status_out->break_errors = s_break_errors;
        status_out->overrun_errors = s_overrun_errors;
        status_out->ring_overrun_count = s_ring_overrun_count;
    }
    return true;
}
