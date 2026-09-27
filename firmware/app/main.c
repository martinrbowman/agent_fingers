#include "app/board.h"
#include "io/digital_bank.h"
#include "io/safe_state.h"
#include "peripherals/adc_scan.h"
#include "peripherals/pwm_out.h"
#include "peripherals/uart_port.h"
#include "peripherals/i2c_bus.h"
#include "peripherals/spi_master.h"
#include "peripherals/spi_slave.h"
#include "protocol/dispatch.h"
#include "transport/usb_transport.h"
#include "pico/stdlib.h"
#include "pico/time.h"

static void blink_task(void) {
    static absolute_time_t next;
    static bool led_on;

    uint32_t interval_ms = usb_transport_mounted() ? 1000 : 200;
    if (time_reached(next)) {
        led_on = !led_on;
        gpio_put(PICO_DEFAULT_LED_PIN, led_on);
        next = make_timeout_time_ms(interval_ms);
    }
}

int main(void) {
    board_init();
    digital_bank_init();
    adc_scan_init();
    pwm_out_init();
    uart_port_init();
    i2c_bus_init();
    spi_master_init();
    spi_slave_init();
    safe_state_enter(SAFE_STATE_BOOT);
    usb_transport_init();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    while (true) {
        usb_transport_task();
        board_task();
        digital_bank_task();
        protocol_dispatch_poll_events();
        blink_task();
    }
}
