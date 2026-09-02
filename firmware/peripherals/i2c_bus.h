#pragma once
#include <stdbool.h>
#include <stdint.h>

// I2C master (i2c0, GP20 SDA / GP21 SCL) and I2C slave (i2c1, GP18
// SDA / GP19 SCL). Neither is gated by ARM_OUTPUTS's arm/lease — both are
// communication buses, not a static driven output level.

void i2c_bus_init(void);

// -- Master (i2c0) --

typedef struct {
    uint8_t  write_status; // 0=ok/not-attempted, 1=nack, 2=timeout
    uint8_t  read_status;  // 0=ok/not-attempted, 1=nack, 2=timeout
    uint16_t bytes_written;
    uint16_t bytes_read;
} i2c_master_xfer_result_t;

// Combined write-then-read (repeated start, no STOP between phases) at up
// to 100kHz standard mode (fixed for this pass). write_len/read_len bound
// by the caller to fit one protocol frame. timeout_ms clamped to
// [1, 5000]; a write_status of timeout (not nack) triggers an automatic
// GPIO bit-bang bus recovery before returning, leaving the bus clean for
// the next call. read_data_out must be sized for read_len bytes.
void i2c_master_xfer(uint8_t address, const uint8_t *write_data, uint16_t write_len,
                      uint16_t read_len, uint16_t timeout_ms,
                      uint8_t *read_data_out, i2c_master_xfer_result_t *out);

// -- Slave (i2c1) --

typedef struct {
    uint32_t bytes_received;
    uint32_t bytes_sent;
    uint32_t transaction_count;
} i2c_slave_config_result_t;

// Configures (or disables) the slave: a 256-byte register map addressed
// by the first byte of each write phase (auto-incrementing pointer for
// subsequent bytes, matching the common "write pointer, read/write data"
// I2C EEPROM/sensor convention), serviced by IRQ (pico_i2c_slave), not
// polling. Always resets the register map, pointer, and counters — call
// again (even with the same parameters) to fetch-and-reset the counters.
void i2c_slave_config(bool enabled, uint8_t address, i2c_slave_config_result_t *out);
