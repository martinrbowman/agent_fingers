#pragma once
#include <stdint.h>

// Mirrors hardware/pin_claims.yaml. Keep both in sync by hand.
typedef enum {
    PIN_UNCLAIMED = 0,
    PIN_DIGITAL_IO,
    PIN_SPI_MASTER,
    PIN_SPI_SLAVE,
    PIN_UART,
    PIN_I2C_SLAVE,
    PIN_I2C_MASTER,
    PIN_DEBUG,
    PIN_ADC,
    PIN_RESERVED_BOARD,
} pin_owner_t;

#define PIN_CLAIMS_GPIO_COUNT 30

// Returns the declared owner of a GPIO, or PIN_UNCLAIMED if out of range.
// Any module that touches a GPIO directly must check this first.
pin_owner_t pin_owner_get(uint32_t gpio);

// Verifies the static claim table matches the frozen pin plan (channel
// counts per owner). Panics on mismatch — this only catches hand-edit
// mistakes in the table itself, it is not a substitute for the CI check
// against hardware/pin_claims.yaml.
void pin_claims_self_check(void);
