#include "peripherals/adc_scan.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "pico/time.h"

#define ADC_BASE_INPUT 0 // ADC0 = GPIO26, ADC1 = GPIO27, ADC2 = GPIO28
#define ADC_BASE_PIN   26

// RP2350's DMA transfer-count register reserves its top 4 bits for a MODE
// field -- valid count range is 0 to 2^28-1, not the full 32 bits. Plain
// 0xFFFFFFFFu as an "unbounded, run until stopped" sentinel sets those
// MODE bits to garbage instead of 0 -- found during spi_slave.c's hardware
// bring-up (see that file for the full story). Address-wrap tracking here
// means it never produced an observable symptom, but it's still invalid
// per the RP2350 DMA contract; this is the correct sentinel.
#define ADC_CONTINUOUS_TRANSFER_COUNT DMA_CH0_TRANS_COUNT_COUNT_BITS

static int s_dma_chan;
// RP2350 DMA ring wrap masks address bits, so this must be aligned to its
// own size (2^12 = 4096 bytes) — see the same issue and fix in
// io/digital_bank.c's capture buffer.
static uint16_t s_buffer[ADC_SCAN_BUFFER_SAMPLES]
    __attribute__((aligned(ADC_SCAN_BUFFER_SAMPLES * sizeof(uint16_t))));

static bool     s_active;
static uint32_t s_scan_id;
static uint32_t s_rate_hz;
static uint8_t  s_channel_mask;
static uint8_t  s_channel_count;
static uint32_t s_max_samples;
static uint64_t s_start_us;

static uint32_t s_write_lap;
static uint32_t s_last_write_offset;
static uint32_t s_overrun_count;
static bool     s_just_completed;

static uint8_t popcount8(uint8_t v) {
    uint8_t n = 0;
    while (v) {
        n += v & 1;
        v >>= 1;
    }
    return n;
}

void adc_scan_init(void) {
    adc_init();
    for (uint pin = ADC_BASE_PIN; pin < ADC_BASE_PIN + 3; pin++) {
        adc_gpio_init(pin);
    }
    adc_set_round_robin(0); // disabled until a scan is configured
    s_dma_chan = dma_claim_unused_channel(true);
    s_active = false;
    s_scan_id = 0;
}

static uint32_t clamp_rate_hz(uint32_t requested_hz) {
    uint32_t adc_clk_hz = clock_get_hz(clk_adc);
    uint32_t max_hz = adc_clk_hz / 96u; // hardware minimum conversion period
    if (max_hz > ADC_SCAN_MAX_RATE_HZ) {
        max_hz = ADC_SCAN_MAX_RATE_HZ;
    }
    uint32_t min_hz = adc_clk_hz / (96u + 65535u) + 1u;
    uint32_t hz = requested_hz;
    if (hz < min_hz) {
        hz = min_hz;
    }
    if (hz > max_hz) {
        hz = max_hz;
    }
    return hz;
}

static uint32_t write_pos_samples(void) {
    uintptr_t base = (uintptr_t)s_buffer;
    uintptr_t cur = (uintptr_t)dma_hw->ch[s_dma_chan].write_addr;
    return (uint32_t)((cur - base) / sizeof(uint16_t));
}

static void poll_position(void) {
    uint32_t pos = write_pos_samples();
    if (pos < s_last_write_offset) {
        s_write_lap++;
    }
    s_last_write_offset = pos;
}

static uint32_t total_written(void) {
    return s_write_lap * ADC_SCAN_BUFFER_SAMPLES + s_last_write_offset;
}

static void stop_hardware(void) {
    adc_run(false);
    adc_set_round_robin(0);
    dma_channel_abort(s_dma_chan);
    adc_fifo_drain();
}

void adc_scan_config(uint8_t channel_mask, uint32_t rate_hz, uint32_t max_samples,
                      adc_scan_config_result_t *out) {
    channel_mask &= 0x07u; // only bits 0-2 (ADC0-2) are meaningful

    if (s_active) {
        poll_position();
        stop_hardware();
        s_active = false;
    }

    if (channel_mask == 0) {
        // Stop convention: no separate ADC_SCAN_STOP opcode exists.
        if (out) {
            out->scan_id = s_scan_id;
            out->granted_rate_hz = 0;
            out->channel_count = 0;
            out->buffer_capacity_samples = ADC_SCAN_BUFFER_SAMPLES;
            out->vref_millivolts = ADC_SCAN_VREF_MILLIVOLTS;
        }
        return;
    }

    uint32_t granted_hz = clamp_rate_hz(rate_hz);
    uint32_t clamped_max = max_samples;
    if (clamped_max > ADC_SCAN_BUFFER_SAMPLES) {
        clamped_max = ADC_SCAN_BUFFER_SAMPLES;
    }

    s_scan_id++;
    if (s_scan_id == 0) {
        s_scan_id = 1;
    }
    s_channel_mask = channel_mask & 0x07u;
    s_channel_count = popcount8(s_channel_mask);
    s_rate_hz = granted_hz;
    s_max_samples = clamped_max;
    s_start_us = time_us_64();
    s_write_lap = 0;
    s_last_write_offset = 0;
    s_overrun_count = 0;
    s_just_completed = false;

    // Start the round-robin cycle at the lowest selected channel.
    uint input = ADC_BASE_INPUT;
    for (uint i = 0; i < 3; i++) {
        if (s_channel_mask & (1u << i)) {
            input = ADC_BASE_INPUT + i;
            break;
        }
    }
    adc_select_input(input);
    adc_set_round_robin(s_channel_mask);

    adc_fifo_setup(true, true, 1, false, false);

    uint32_t adc_clk_hz = clock_get_hz(clk_adc);
    uint32_t period_cycles = adc_clk_hz / granted_hz;
    if (period_cycles < 96u) {
        period_cycles = 96u;
    }
    adc_set_clkdiv((float)(period_cycles - 1u));

    dma_channel_config dma_cfg = dma_channel_get_default_config(s_dma_chan);
    channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&dma_cfg, false);
    channel_config_set_write_increment(&dma_cfg, true);
    channel_config_set_dreq(&dma_cfg, DREQ_ADC);
    channel_config_set_ring(&dma_cfg, true, 12); // 2^12 = 4096 bytes = 2048 u16 samples

    uint32_t trans_count = (clamped_max > 0) ? clamped_max : ADC_CONTINUOUS_TRANSFER_COUNT;

    dma_channel_configure(s_dma_chan, &dma_cfg,
                           s_buffer, &adc_hw->fifo,
                           trans_count, true);

    adc_run(true);
    s_active = true;

    if (out) {
        out->scan_id = s_scan_id;
        out->granted_rate_hz = granted_hz;
        out->channel_count = s_channel_count;
        out->buffer_capacity_samples = ADC_SCAN_BUFFER_SAMPLES;
        out->vref_millivolts = ADC_SCAN_VREF_MILLIVOLTS;
    }
}

bool adc_scan_read(uint32_t scan_id, uint32_t offset, uint16_t limit,
                    uint16_t *samples_out, uint16_t *returned_out,
                    adc_scan_status_t *status_out) {
    if (scan_id == 0 || scan_id != s_scan_id) {
        return false;
    }
    if (s_active) {
        poll_position();
    }
    uint32_t total = total_written();

    uint32_t oldest_valid = (total > ADC_SCAN_BUFFER_SAMPLES) ? (total - ADC_SCAN_BUFFER_SAMPLES) : 0;
    if (offset < oldest_valid) {
        s_overrun_count += (oldest_valid - offset);
        offset = oldest_valid;
    }

    uint16_t returned = 0;
    if (offset < total) {
        uint32_t available = total - offset;
        uint32_t want = limit;
        if (want > available) {
            want = available;
        }
        if (want > ADC_SCAN_BUFFER_SAMPLES) {
            want = ADC_SCAN_BUFFER_SAMPLES;
        }

        uint32_t ring_start = offset % ADC_SCAN_BUFFER_SAMPLES;
        for (uint32_t i = 0; i < want; i++) {
            uint32_t idx = (ring_start + i) % ADC_SCAN_BUFFER_SAMPLES;
            samples_out[i] = s_buffer[idx] & 0x0FFFu; // 12-bit code
        }
        returned = (uint16_t)want;
    }

    if (returned_out) {
        *returned_out = returned;
    }
    if (status_out) {
        status_out->scan_id = s_scan_id;
        status_out->total_captured = total;
        status_out->overrun_count = s_overrun_count;
        status_out->start_timestamp_us = s_start_us;
        status_out->rate_hz = s_rate_hz;
        status_out->channel_count = s_channel_count;
    }
    return true;
}

bool adc_scan_task(adc_scan_complete_result_t *out) {
    if (s_active) {
        poll_position();

        if (s_max_samples > 0 && !dma_channel_is_busy(s_dma_chan)) {
            stop_hardware();
            s_active = false;
            s_just_completed = true;
        }
    }

    if (s_just_completed) {
        s_just_completed = false;
        if (out) {
            out->scan_id = s_scan_id;
            out->total_captured = total_written();
        }
        return true;
    }
    return false;
}
