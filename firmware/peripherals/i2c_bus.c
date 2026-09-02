#include "peripherals/i2c_bus.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "pico/i2c_slave.h"
#include "pico/time.h"

#define I2C_MASTER_INST     i2c0
#define I2C_MASTER_SDA_PIN  20
#define I2C_MASTER_SCL_PIN  21
#define I2C_MASTER_BAUD     100000u // 100kHz standard mode, fixed for this pass

#define I2C_SLAVE_INST     i2c1
#define I2C_SLAVE_SDA_PIN  18
#define I2C_SLAVE_SCL_PIN  19

#define I2C_XFER_TIMEOUT_MIN_MS 1u
#define I2C_XFER_TIMEOUT_MAX_MS 5000u
#define I2C_XFER_TIMEOUT_DEFAULT_MS 100u

#define I2C_SLAVE_REGMAP_SIZE 256

static uint8_t s_slave_regmap[I2C_SLAVE_REGMAP_SIZE];
static uint8_t s_slave_reg_ptr;
static bool    s_slave_expect_ptr;
static bool    s_slave_enabled;
static volatile uint32_t s_slave_bytes_received;
static volatile uint32_t s_slave_bytes_sent;
static volatile uint32_t s_slave_transaction_count;

void i2c_bus_init(void) {
    i2c_init(I2C_MASTER_INST, I2C_MASTER_BAUD);
    gpio_set_function(I2C_MASTER_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_MASTER_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_MASTER_SDA_PIN);
    gpio_pull_up(I2C_MASTER_SCL_PIN);

    s_slave_enabled = false;
}

static uint32_t clamp_timeout_ms(uint16_t requested_ms) {
    if (requested_ms == 0) {
        return I2C_XFER_TIMEOUT_DEFAULT_MS;
    }
    if (requested_ms < I2C_XFER_TIMEOUT_MIN_MS) {
        return I2C_XFER_TIMEOUT_MIN_MS;
    }
    if (requested_ms > I2C_XFER_TIMEOUT_MAX_MS) {
        return I2C_XFER_TIMEOUT_MAX_MS;
    }
    return requested_ms;
}

// A raw TIMEOUT (not NACK) means the I2C hardware state machine got stuck
// waiting — most likely SDA or SCL held low by a wedged device. NACK is a
// normal protocol response and does not indicate a stuck bus, so recovery
// only triggers on timeout. Bit-bangs up to 9 SCL pulses as plain GPIO to
// let a wedged device finish clocking out and release SDA (the standard
// I2C bus-recovery sequence), then restores i2c0.
static void i2c_master_bus_recovery(void) {
    i2c_deinit(I2C_MASTER_INST);

    gpio_set_function(I2C_MASTER_SCL_PIN, GPIO_FUNC_SIO);
    gpio_set_function(I2C_MASTER_SDA_PIN, GPIO_FUNC_SIO);
    gpio_set_dir(I2C_MASTER_SCL_PIN, GPIO_OUT);
    gpio_set_dir(I2C_MASTER_SDA_PIN, GPIO_IN);
    gpio_pull_up(I2C_MASTER_SDA_PIN);
    gpio_pull_up(I2C_MASTER_SCL_PIN);

    for (int i = 0; i < 9; i++) {
        gpio_put(I2C_MASTER_SCL_PIN, 0);
        sleep_us(5);
        gpio_put(I2C_MASTER_SCL_PIN, 1);
        sleep_us(5);
        if (gpio_get(I2C_MASTER_SDA_PIN)) {
            break;
        }
    }

    i2c_init(I2C_MASTER_INST, I2C_MASTER_BAUD);
    gpio_set_function(I2C_MASTER_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_MASTER_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_MASTER_SDA_PIN);
    gpio_pull_up(I2C_MASTER_SCL_PIN);
}

void i2c_master_xfer(uint8_t address, const uint8_t *write_data, uint16_t write_len,
                      uint16_t read_len, uint16_t timeout_ms,
                      uint8_t *read_data_out, i2c_master_xfer_result_t *out) {
    uint32_t deadline_ms = clamp_timeout_ms(timeout_ms);

    uint8_t write_status = 0;
    uint8_t read_status = 0;
    uint16_t bytes_written = 0;
    uint16_t bytes_read = 0;

    if (write_len > 0) {
        absolute_time_t deadline = make_timeout_time_ms(deadline_ms);
        int rc = i2c_write_blocking_until(I2C_MASTER_INST, address, write_data, write_len,
                                           read_len > 0, deadline);
        if (rc == PICO_ERROR_GENERIC) {
            write_status = 1;
        } else if (rc == PICO_ERROR_TIMEOUT) {
            write_status = 2;
        } else {
            bytes_written = (uint16_t)rc;
        }
    }

    if (read_len > 0 && write_status == 0) {
        absolute_time_t deadline = make_timeout_time_ms(deadline_ms);
        int rc = i2c_read_blocking_until(I2C_MASTER_INST, address, read_data_out, read_len,
                                          false, deadline);
        if (rc == PICO_ERROR_GENERIC) {
            read_status = 1;
        } else if (rc == PICO_ERROR_TIMEOUT) {
            read_status = 2;
        } else {
            bytes_read = (uint16_t)rc;
        }
    }

    if (write_status == 2 || read_status == 2) {
        i2c_master_bus_recovery();
    }

    if (out) {
        out->write_status = write_status;
        out->read_status = read_status;
        out->bytes_written = bytes_written;
        out->bytes_read = bytes_read;
    }
}

static void i2c_slave_event_handler(i2c_inst_t *i2c, i2c_slave_event_t event) {
    switch (event) {
    case I2C_SLAVE_RECEIVE: {
        uint8_t byte = i2c_read_byte_raw(i2c);
        if (s_slave_expect_ptr) {
            s_slave_reg_ptr = byte;
            s_slave_expect_ptr = false;
        } else {
            s_slave_regmap[s_slave_reg_ptr] = byte;
            s_slave_reg_ptr++; // wraps naturally via uint8_t overflow
        }
        s_slave_bytes_received++;
        break;
    }
    case I2C_SLAVE_REQUEST:
        i2c_write_byte_raw(i2c, s_slave_regmap[s_slave_reg_ptr]);
        s_slave_reg_ptr++;
        s_slave_bytes_sent++;
        break;
    case I2C_SLAVE_FINISH:
        s_slave_expect_ptr = true;
        s_slave_transaction_count++;
        break;
    }
}

void i2c_slave_config(bool enabled, uint8_t address, i2c_slave_config_result_t *out) {
    if (s_slave_enabled) {
        // Stops the IRQ before we read the counters below, so there's no
        // race between an in-flight ISR update and this read.
        i2c_slave_deinit(I2C_SLAVE_INST);
        s_slave_enabled = false;
    }

    if (out) {
        out->bytes_received = s_slave_bytes_received;
        out->bytes_sent = s_slave_bytes_sent;
        out->transaction_count = s_slave_transaction_count;
    }

    for (int i = 0; i < I2C_SLAVE_REGMAP_SIZE; i++) {
        s_slave_regmap[i] = 0;
    }
    s_slave_reg_ptr = 0;
    s_slave_expect_ptr = true;
    s_slave_bytes_received = 0;
    s_slave_bytes_sent = 0;
    s_slave_transaction_count = 0;

    if (enabled) {
        gpio_set_function(I2C_SLAVE_SDA_PIN, GPIO_FUNC_I2C);
        gpio_set_function(I2C_SLAVE_SCL_PIN, GPIO_FUNC_I2C);
        gpio_pull_up(I2C_SLAVE_SDA_PIN);
        gpio_pull_up(I2C_SLAVE_SCL_PIN);
        i2c_slave_init(I2C_SLAVE_INST, address & 0x7Fu, i2c_slave_event_handler);
        s_slave_enabled = true;
    }
}
