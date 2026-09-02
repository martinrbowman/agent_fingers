#include "io/pin_claims.h"
#include "pico/assert.h"

static const pin_owner_t s_table[PIN_CLAIMS_GPIO_COUNT] = {
    [0]  = PIN_DIGITAL_IO, [1] = PIN_DIGITAL_IO, [2] = PIN_DIGITAL_IO, [3] = PIN_DIGITAL_IO,
    [4]  = PIN_DIGITAL_IO, [5] = PIN_DIGITAL_IO, [6] = PIN_DIGITAL_IO, [7] = PIN_DIGITAL_IO,

    [8]  = PIN_SPI_MASTER, [9]  = PIN_SPI_MASTER,
    [10] = PIN_SPI_MASTER, [11] = PIN_SPI_MASTER,

    [12] = PIN_SPI_SLAVE, [13] = PIN_SPI_SLAVE,
    [14] = PIN_SPI_SLAVE, [15] = PIN_SPI_SLAVE,

    [16] = PIN_UART, [17] = PIN_UART,

    [18] = PIN_I2C_SLAVE, [19] = PIN_I2C_SLAVE,

    [20] = PIN_I2C_MASTER, [21] = PIN_I2C_MASTER,

    [22] = PIN_DEBUG,

    [23] = PIN_RESERVED_BOARD, [24] = PIN_RESERVED_BOARD, [25] = PIN_RESERVED_BOARD,

    [26] = PIN_ADC, [27] = PIN_ADC, [28] = PIN_ADC,

    [29] = PIN_RESERVED_BOARD,
};

pin_owner_t pin_owner_get(uint32_t gpio) {
    if (gpio >= PIN_CLAIMS_GPIO_COUNT) {
        return PIN_UNCLAIMED;
    }
    return s_table[gpio];
}

void pin_claims_self_check(void) {
    unsigned counts[PIN_RESERVED_BOARD + 1] = {0};
    for (uint32_t gpio = 0; gpio < PIN_CLAIMS_GPIO_COUNT; gpio++) {
        counts[s_table[gpio]]++;
    }
    hard_assert(counts[PIN_DIGITAL_IO] == 8);
    hard_assert(counts[PIN_SPI_MASTER] == 4);
    hard_assert(counts[PIN_SPI_SLAVE] == 4);
    hard_assert(counts[PIN_UART] == 2);
    hard_assert(counts[PIN_I2C_SLAVE] == 2);
    hard_assert(counts[PIN_I2C_MASTER] == 2);
    hard_assert(counts[PIN_DEBUG] == 1);
    hard_assert(counts[PIN_ADC] == 3);
    hard_assert(counts[PIN_RESERVED_BOARD] == 4);
    hard_assert(counts[PIN_UNCLAIMED] == 0);
}
