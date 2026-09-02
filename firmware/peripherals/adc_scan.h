#pragma once
#include <stdbool.h>
#include <stdint.h>

// Round-robin ADC0-2 (GP26-28) scanning into a ring buffer via FIFO+DMA —
// same shape as io/digital_bank.h's capture mechanism. One scan at a time;
// a new CONFIG replaces whatever's running. Sending channel_mask=0 stops it
// (there is no separate STOP opcode in the plan's ADC opcode table).

#define ADC_SCAN_MAX_RATE_HZ    200000u
#define ADC_SCAN_BUFFER_SAMPLES 2048u
#define ADC_SCAN_VREF_MILLIVOLTS 3300u // nominal 3V3 rail, not independently
                                        // calibrated — see protocol.md

void adc_scan_init(void);

typedef struct {
    uint32_t scan_id;
    uint32_t granted_rate_hz;
    uint8_t  channel_count;
    uint32_t buffer_capacity_samples;
    uint16_t vref_millivolts;
} adc_scan_config_result_t;

// channel_mask bits 0-2 = ADC0-2. channel_mask=0 stops any active scan and
// returns channel_count=0. rate_hz is the TOTAL round-robin conversion
// rate, clamped to what the ADC clock divider can hit, up to
// ADC_SCAN_MAX_RATE_HZ. max_samples is clamped to ADC_SCAN_BUFFER_SAMPLES
// (0 = unbounded) so a bounded scan can never wrap before completing.
void adc_scan_config(uint8_t channel_mask, uint32_t rate_hz, uint32_t max_samples,
                      adc_scan_config_result_t *out);

typedef struct {
    uint32_t scan_id;
    uint32_t total_captured;
    uint32_t overrun_count;
    uint64_t start_timestamp_us;
    uint32_t rate_hz;
    uint8_t  channel_count;
} adc_scan_status_t;

// Returns false (nothing written) if scan_id doesn't match the
// current/last scan. Otherwise fills status_out and copies up to `limit`
// raw 12-bit codes (as u16) starting at absolute sample index `offset`
// into samples_out (caller must size it for `limit` uint16_t values).
bool adc_scan_read(uint32_t scan_id, uint32_t offset, uint16_t limit,
                    uint16_t *samples_out, uint16_t *returned_out,
                    adc_scan_status_t *status_out);

typedef struct {
    uint32_t scan_id;
    uint32_t total_captured;
} adc_scan_complete_result_t;

// Polls for bounded-scan auto-completion and ring overrun. Call every
// main-loop iteration. Returns true exactly once, the iteration a bounded
// scan completes on its own.
bool adc_scan_task(adc_scan_complete_result_t *out);
