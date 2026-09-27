#include "debug/swd.h"
#include "debug/debug_pins.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include "pico/platform.h"
#include "pico/time.h"

#define SWCLK_MASK  (1u << DBG_PIN_SWCLK)
#define SWDIO_MASK  (1u << DBG_PIN_SWDIO)
#define NRESET_MASK (1u << DBG_PIN_SWD_NRESET)

// The bit loop's own cost is measured once at boot (calibrate) rather than
// guessed: a guessed overhead that's too large made every request above
// ~5 MHz run the loop flat out, far faster than asked -- found on
// hardware, JTAG to an ESP32 passed at 5 MHz and failed at every setting
// from 6 MHz up. With the measured figure, the clock never exceeds the
// requested rate (the delay call adds a little on top, erring slow).
// Minimum time between the falling clock edge and sampling the target's
// output, whatever the clock setting. Without it the flat-out loop sampled
// the target output a few ns after the clock fell; found with JTAG on an
// ESP32, applied here too since SWD reads have the same shape.
#define SAMPLE_SETTLE_CYCLES 6u // 40 ns at 150 MHz
#define SWD_MAX_HZ 10000000u
#define SWD_CALIBRATION_BITS 32768u
#define SWD_DEFAULT_HZ 1000000u

static uint32_t s_half_cycles;  // extra cycles per half period
static uint32_t s_loop_half_cycles = 16; // measured by swd_calibrate()
static uint8_t  s_turnaround = 1;
static bool     s_data_phase;
static uint8_t  s_idle_cycles;

static inline void delay_half(void) {
    if (s_half_cycles) {
        busy_wait_at_least_cycles(s_half_cycles);
    }
}

static inline void delay_before_sample(void) {
    busy_wait_at_least_cycles(s_half_cycles > SAMPLE_SETTLE_CYCLES ? s_half_cycles : SAMPLE_SETTLE_CYCLES);
}

static inline void swdio_out(uint32_t bit) {
    if (bit & 1u) {
        sio_hw->gpio_set = SWDIO_MASK;
    } else {
        sio_hw->gpio_clr = SWDIO_MASK;
    }
}

static inline void swdio_drive(bool drive) {
    if (drive) {
        sio_hw->gpio_oe_set = SWDIO_MASK;
    } else {
        sio_hw->gpio_oe_clr = SWDIO_MASK;
    }
}

static inline uint32_t swdio_in(void) {
    return (sio_hw->gpio_in & SWDIO_MASK) ? 1u : 0u;
}

static inline void clock_cycle(void) {
    sio_hw->gpio_clr = SWCLK_MASK;
    delay_half();
    sio_hw->gpio_set = SWCLK_MASK;
    delay_half();
}

static inline void write_bit(uint32_t bit) {
    swdio_out(bit);
    sio_hw->gpio_clr = SWCLK_MASK;
    delay_half();
    sio_hw->gpio_set = SWCLK_MASK;
    delay_half();
}

static inline uint32_t read_bit(void) {
    sio_hw->gpio_clr = SWCLK_MASK;
    delay_before_sample();
    uint32_t bit = swdio_in();
    sio_hw->gpio_set = SWCLK_MASK;
    delay_half();
    return bit;
}

void swd_port_on(void) {
    // core0 left these SIO inputs, no pulls, IE off (spi_master_release_pins).
    gpio_set_input_enabled(DBG_PIN_SWCLK, true); // read back by DAP_SWJ_Pins
    gpio_set_input_enabled(DBG_PIN_SWDIO, true);
    gpio_pull_up(DBG_PIN_SWDIO); // SWD idles high; E9 doesn't affect pull-ups
    sio_hw->gpio_set = SWCLK_MASK | SWDIO_MASK;
    sio_hw->gpio_oe_set = SWCLK_MASK | SWDIO_MASK;

    // nRESET is open-drain: output latch 0, released = input. Target has
    // its own pull-up; ours helps a bare line read correctly.
    gpio_pull_up(DBG_PIN_SWD_NRESET);
    gpio_set_input_enabled(DBG_PIN_SWD_NRESET, true);
    sio_hw->gpio_clr = NRESET_MASK;
    sio_hw->gpio_oe_clr = NRESET_MASK;
}

void swd_port_off(void) {
    const uint pins[] = {DBG_PIN_SWCLK, DBG_PIN_SWDIO, DBG_PIN_SWO, DBG_PIN_SWD_NRESET};
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        // IE off before letting go: a pin released high with IE on can
        // latch at ~2.2V on A2 silicon (E9).
        gpio_set_input_enabled(pins[i], false);
        gpio_disable_pulls(pins[i]);
        sio_hw->gpio_oe_clr = 1u << pins[i];
    }
}

void swd_set_clock(uint32_t hz) {
    if (hz == 0) {
        hz = SWD_DEFAULT_HZ;
    }
    if (hz > SWD_MAX_HZ) {
        hz = SWD_MAX_HZ;
    }
    uint32_t sys = clock_get_hz(clk_sys);
    uint32_t half = (sys + 2u * hz - 1u) / (2u * hz); // round up: never faster than asked
    s_half_cycles = half > s_loop_half_cycles ? half - s_loop_half_cycles : 0;
}

// Pins are still the SPI master's (PIO function) at boot, so these SIO
// writes don't reach them.
uint32_t swd_calibrate(void) {
    static const uint8_t zeros[SWD_CALIBRATION_BITS / 8];
    uint32_t saved = s_half_cycles;
    s_half_cycles = 0;
    uint64_t t0 = time_us_64();
    swd_seq_out(zeros, SWD_CALIBRATION_BITS);
    uint64_t us = time_us_64() - t0;
    s_half_cycles = saved;
    sio_hw->gpio_oe_clr = SWCLK_MASK | SWDIO_MASK;
    uint64_t cycles = us * (clock_get_hz(clk_sys) / 1000000u);
    s_loop_half_cycles = (uint32_t)((cycles + SWD_CALIBRATION_BITS) / (2u * SWD_CALIBRATION_BITS));
    return (uint32_t)(clock_get_hz(clk_sys) / (2u * s_loop_half_cycles)); // fastest possible clock
}

void swd_configure(uint8_t turnaround, bool data_phase) {
    s_turnaround = turnaround;
    s_data_phase = data_phase;
}

void swd_set_idle_cycles(uint8_t idle_cycles) {
    s_idle_cycles = idle_cycles;
}

void swd_seq_out(const uint8_t *data, uint32_t bits) {
    swdio_drive(true);
    for (uint32_t i = 0; i < bits; i++) {
        write_bit((uint32_t)(data[i / 8] >> (i % 8)));
    }
}

void swd_seq_in(uint8_t *data, uint32_t bits) {
    swdio_drive(false);
    for (uint32_t i = 0; i < (bits + 7) / 8; i++) {
        data[i] = 0;
    }
    for (uint32_t i = 0; i < bits; i++) {
        data[i / 8] |= (uint8_t)(read_bit() << (i % 8));
    }
    swdio_drive(true);
}

uint8_t swd_transfer(uint32_t request, uint32_t *data) {
    uint32_t parity = 0;
    uint32_t bit;

    // Request: start, APnDP, RnW, A2, A3, parity, stop, park.
    write_bit(1u);
    for (int i = 0; i < 4; i++) {
        bit = (request >> i) & 1u;
        write_bit(bit);
        parity += bit;
    }
    write_bit(parity);
    write_bit(0u);
    write_bit(1u);

    swdio_drive(false);
    for (uint8_t n = s_turnaround; n; n--) {
        clock_cycle();
    }

    uint32_t ack = read_bit();
    ack |= read_bit() << 1;
    ack |= read_bit() << 2;

    if (ack == SWD_ACK_OK) {
        if (request & 0x2u) {
            uint32_t val = 0;
            parity = 0;
            for (int n = 0; n < 32; n++) {
                bit = read_bit();
                parity += bit;
                val = (val >> 1) | (bit << 31);
            }
            bit = read_bit();
            if ((parity ^ bit) & 1u) {
                ack = SWD_ACK_ERROR;
            }
            if (data) {
                *data = val;
            }
            for (uint8_t n = s_turnaround; n; n--) {
                clock_cycle();
            }
            swdio_drive(true);
        } else {
            for (uint8_t n = s_turnaround; n; n--) {
                clock_cycle();
            }
            swdio_drive(true);
            uint32_t val = data ? *data : 0;
            parity = 0;
            for (int n = 0; n < 32; n++) {
                write_bit(val);
                parity += val & 1u;
                val >>= 1;
            }
            write_bit(parity);
        }
        if (s_idle_cycles) {
            swdio_out(0);
            for (uint8_t n = s_idle_cycles; n; n--) {
                clock_cycle();
            }
        }
        swdio_out(1);
        return (uint8_t)ack;
    }

    if (ack == SWD_ACK_WAIT || ack == SWD_ACK_FAULT) {
        if (s_data_phase && (request & 0x2u)) {
            for (int n = 0; n < 33; n++) {
                clock_cycle(); // dummy read data + parity
            }
        }
        for (uint8_t n = s_turnaround; n; n--) {
            clock_cycle();
        }
        swdio_drive(true);
        if (s_data_phase && !(request & 0x2u)) {
            swdio_out(0);
            for (int n = 0; n < 33; n++) {
                clock_cycle(); // dummy write data + parity
            }
        }
        swdio_out(1);
        return (uint8_t)ack;
    }

    // Protocol error (no/garbled ack): back off a whole data phase.
    for (uint32_t n = s_turnaround + 33u; n; n--) {
        clock_cycle();
    }
    swdio_drive(true);
    swdio_out(1);
    return (uint8_t)ack;
}

uint8_t swd_pins_read(void) {
    uint32_t in = sio_hw->gpio_in;
    return (uint8_t)(((in & SWCLK_MASK) ? 0x01u : 0) |
                     ((in & SWDIO_MASK) ? 0x02u : 0) |
                     ((in & NRESET_MASK) ? 0x80u : 0));
}

void swd_pins_write(uint8_t value, uint8_t select) {
    if (select & 0x01u) {
        if (value & 0x01u) sio_hw->gpio_set = SWCLK_MASK; else sio_hw->gpio_clr = SWCLK_MASK;
    }
    if (select & 0x02u) {
        swdio_drive(true);
        swdio_out(value >> 1);
    }
    if (select & 0x80u) {
        // Open-drain: drive low, or release to the pull-ups.
        if (value & 0x80u) sio_hw->gpio_oe_clr = NRESET_MASK; else sio_hw->gpio_oe_set = NRESET_MASK;
    }
}
