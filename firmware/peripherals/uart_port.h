#pragma once
#include <stdbool.h>
#include <stdint.h>

// Hardware UART0 on GP16 (TX) / GP17 (RX). Not gated by the digital bank's
// arm/lease — this is a communication bus, not a static driven output
// level. RX runs continuously via DMA into a ring buffer once configured
// (same ring+overrun shape as digital_bank's capture and adc_scan, minus
// the start/stop/id machinery — this is a plain drain-as-you-go stream, not
// a bounded capture); TX is a one-shot fire-and-forget DMA per UART_WRITE.

void uart_port_init(void);

typedef struct {
    uint32_t granted_baud_rate;
} uart_port_config_result_t;

// (Re)configures the UART, resets the RX ring/read-cursor/error counters,
// and aborts any in-flight TX.
void uart_port_config(uint32_t baud_rate, uint8_t data_bits, uint8_t stop_bits,
                       uint8_t parity, uart_port_config_result_t *out);

// Returns false (nothing sent) if not yet configured, or a previous
// UART_WRITE's DMA transfer is still draining — never blocks or queues.
bool uart_port_write(const uint8_t *data, uint16_t len);

typedef struct {
    uint32_t framing_errors;
    uint32_t parity_errors;
    uint32_t break_errors;
    uint32_t overrun_errors;
    uint32_t ring_overrun_count;
} uart_port_status_t;

// Returns false (nothing written) if not yet configured. Otherwise drains
// up to `limit` bytes from the RX ring into data_out, advances the read
// cursor, and fills status_out with the cumulative counters.
bool uart_port_read(uint16_t limit, uint8_t *data_out, uint16_t *returned_out,
                     uart_port_status_t *status_out);
