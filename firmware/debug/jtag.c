#include "debug/jtag.h"
#include "debug/debug_pins.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include "pico/platform.h"
#include "pico/time.h"

#define TCK_MASK    (1u << DBG_PIN_TCK)
#define TDI_MASK    (1u << DBG_PIN_TDI)
#define TDO_MASK    (1u << DBG_PIN_TDO)
#define TMS_MASK    (1u << DBG_PIN_TMS)
#define NRESET_MASK (1u << DBG_PIN_JTAG_NRESET)

// The bit loop's own cost is measured once at boot (calibrate) rather than
// guessed: a guessed overhead that's too large made every request above
// ~5 MHz run the loop flat out, far faster than asked -- found on
// hardware, JTAG to an ESP32 passed at 5 MHz and failed at every setting
// from 6 MHz up. With the measured figure, the clock never exceeds the
// requested rate (the delay call adds a little on top, erring slow).
// Minimum time between the falling clock edge and sampling the target's
// output, whatever the clock setting. Without it the flat-out loop sampled
// TDO a few ns after TCK fell -- before an ESP32 had driven the next bit
// through jumper wires (failed at 6.25 MHz, fine at 5 MHz).
#define SAMPLE_SETTLE_CYCLES 6u // 40 ns at 150 MHz
#define JTAG_MAX_HZ 10000000u
#define JTAG_CALIBRATION_BITS 32768u
#define JTAG_DEFAULT_HZ 1000000u

static uint32_t s_half_cycles;
static uint32_t s_loop_half_cycles = 16; // measured by jtag_calibrate()

static inline void delay_half(void) {
    if (s_half_cycles) {
        busy_wait_at_least_cycles(s_half_cycles);
    }
}

static inline void delay_before_sample(void) {
    busy_wait_at_least_cycles(s_half_cycles > SAMPLE_SETTLE_CYCLES ? s_half_cycles : SAMPLE_SETTLE_CYCLES);
}

// ESP32 strap timing: MTDI is latched as EN rises (setup 0 ms, hold 1 ms
// per the ESP32 datasheet), so TDI must be low at nRESET release and for a
// ms after -- but *not* for the whole time reset is held: OpenOCD's
// connect/halt-under-reset on ARM parts scans JTAG during reset, and
// forcing TDI low then broke every scan (STM32F103: "Invalid ACK (0)").
#define STRAP_HOLD_US 2000u

static inline void tdi_out(uint32_t bit) {
    if (bit & 1u) {
        sio_hw->gpio_set = TDI_MASK;
    } else {
        sio_hw->gpio_clr = TDI_MASK;
    }
}

static inline uint32_t cycle(void) {
    sio_hw->gpio_clr = TCK_MASK;
    delay_before_sample();
    uint32_t tdo = (sio_hw->gpio_in & TDO_MASK) ? 1u : 0u;
    sio_hw->gpio_set = TCK_MASK;
    delay_half();
    return tdo;
}

void jtag_port_on(void) {
    // core0 left GP8-11/GP22 as SIO inputs, no pulls, IE off. TDI gets no
    // pull and idles low (ESP32 MTDI strap); TMS idles high.
    gpio_set_input_enabled(DBG_PIN_TCK, true);
    gpio_set_input_enabled(DBG_PIN_TMS, true);
    gpio_set_input_enabled(DBG_PIN_TDI, true);
    gpio_set_input_enabled(DBG_PIN_TDO, true);
    gpio_pull_up(DBG_PIN_TDO); // E9 doesn't affect pull-ups
    sio_hw->gpio_set = TCK_MASK | TMS_MASK;
    sio_hw->gpio_clr = TDI_MASK;
    sio_hw->gpio_oe_set = TCK_MASK | TMS_MASK | TDI_MASK;

    // nRESET (e.g. ESP32 EN) is open-drain: latch 0, released = input.
    gpio_init(DBG_PIN_JTAG_NRESET);
    gpio_pull_up(DBG_PIN_JTAG_NRESET);
    gpio_set_input_enabled(DBG_PIN_JTAG_NRESET, true);
    sio_hw->gpio_clr = NRESET_MASK;
    sio_hw->gpio_oe_clr = NRESET_MASK;
}

void jtag_port_off(void) {
    const uint pins[] = {DBG_PIN_TCK, DBG_PIN_TDI, DBG_PIN_TDO, DBG_PIN_TMS, DBG_PIN_JTAG_NRESET};
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        gpio_set_input_enabled(pins[i], false); // E9: IE off before release
        gpio_disable_pulls(pins[i]);
        sio_hw->gpio_oe_clr = 1u << pins[i];
    }
}

void jtag_set_clock(uint32_t hz) {
    if (hz == 0) {
        hz = JTAG_DEFAULT_HZ;
    }
    if (hz > JTAG_MAX_HZ) {
        hz = JTAG_MAX_HZ;
    }
    uint32_t sys = clock_get_hz(clk_sys);
    uint32_t half = (sys + 2u * hz - 1u) / (2u * hz); // round up: never faster than asked
    s_half_cycles = half > s_loop_half_cycles ? half - s_loop_half_cycles : 0;
}

// Pins are still the SPI master's (PIO function) at boot, so these SIO
// writes don't reach them. Times the capture path (the slower one).
uint32_t jtag_calibrate(void) {
    static const uint8_t ones[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t tdo[8];
    uint32_t saved = s_half_cycles;
    s_half_cycles = 0;
    uint64_t t0 = time_us_64();
    for (uint32_t i = 0; i < JTAG_CALIBRATION_BITS / 64; i++) {
        jtag_sequence(0x80u, ones, tdo); // 64 cycles, TMS 0, capture
    }
    uint64_t us = time_us_64() - t0;
    s_half_cycles = saved;
    uint64_t cycles = us * (clock_get_hz(clk_sys) / 1000000u);
    s_loop_half_cycles = (uint32_t)((cycles + JTAG_CALIBRATION_BITS) / (2u * JTAG_CALIBRATION_BITS));
    return (uint32_t)(clock_get_hz(clk_sys) / (2u * s_loop_half_cycles));
}

void jtag_sequence(uint8_t info, const uint8_t *tdi, uint8_t *tdo) {
    uint32_t n = info & 0x3Fu;
    if (n == 0) {
        n = 64;
    }
    if (info & 0x40u) {
        sio_hw->gpio_set = TMS_MASK;
    } else {
        sio_hw->gpio_clr = TMS_MASK;
    }
    bool capture = (info & 0x80u) != 0;
    while (n) {
        uint32_t k = n < 8 ? n : 8;
        uint32_t in = *tdi++;
        uint32_t out = 0;
        for (uint32_t i = 0; i < k; i++) {
            tdi_out(in >> i);
            out |= cycle() << i;
        }
        if (capture) {
            *tdo++ = (uint8_t)out;
        }
        n -= k;
    }
    sio_hw->gpio_clr = TDI_MASK; // park low: ESP32 MTDI strap
}

void jtag_tms_sequence(const uint8_t *data, uint32_t bits) {
    for (uint32_t i = 0; i < bits; i++) {
        if ((data[i / 8] >> (i % 8)) & 1u) {
            sio_hw->gpio_set = TMS_MASK;
        } else {
            sio_hw->gpio_clr = TMS_MASK;
        }
        cycle();
    }
}

uint8_t jtag_pins_read(void) {
    uint32_t in = sio_hw->gpio_in;
    return (uint8_t)(((in & TCK_MASK) ? 0x01u : 0) |
                     ((in & TMS_MASK) ? 0x02u : 0) |
                     ((in & TDI_MASK) ? 0x04u : 0) |
                     ((in & TDO_MASK) ? 0x08u : 0) |
                     ((in & NRESET_MASK) ? 0x80u : 0));
}

void jtag_pins_write(uint8_t value, uint8_t select) {
    if (select & 0x80u) {
        if (value & 0x80u) {
            if (sio_hw->gpio_oe & NRESET_MASK) {
                // Releasing reset: TDI low across the ESP32 strap latch.
                // DAP commands run one at a time, so no scan can land in
                // the hold window.
                sio_hw->gpio_clr = TDI_MASK;
                sio_hw->gpio_oe_clr = NRESET_MASK;
                busy_wait_us_32(STRAP_HOLD_US);
            }
        } else {
            sio_hw->gpio_oe_set = NRESET_MASK; // drive low
        }
    }
    if (select & 0x01u) {
        if (value & 0x01u) sio_hw->gpio_set = TCK_MASK; else sio_hw->gpio_clr = TCK_MASK;
    }
    if (select & 0x02u) {
        if (value & 0x02u) sio_hw->gpio_set = TMS_MASK; else sio_hw->gpio_clr = TMS_MASK;
    }
    if (select & 0x04u) {
        tdi_out(value >> 2);
    }
    if (select & 0x80u) {
        sio_hw->gpio_clr = TDI_MASK; // never leave TDI parked high
    }
}

// -- ARM JTAG-DP access --------------------------------------------------------
// Follows ARM's CMSIS-DAP reference (JTAG_DP.c) state by state, except that
// TDI parks low instead of high after each operation (ESP32 MTDI strap).
// TDI is still driven high while shifting BYPASS bits, as JTAG requires.

static uint8_t s_dev_count;
static uint8_t s_dev_index;
static uint8_t s_ir_length[JTAG_MAX_DEVICES];
static uint16_t s_ir_before[JTAG_MAX_DEVICES];
static uint16_t s_ir_after[JTAG_MAX_DEVICES];
static uint8_t s_idle_cycles;

static inline void tms(uint32_t v) {
    if (v) sio_hw->gpio_set = TMS_MASK; else sio_hw->gpio_clr = TMS_MASK;
}

static inline void clk(void) {
    (void)cycle();
}

static inline void tdi_cycle(uint32_t bit) {
    tdi_out(bit);
    (void)cycle();
}

static inline uint32_t tdio_cycle(uint32_t bit) {
    tdi_out(bit);
    return cycle();
}

static inline void park_tdi(void) {
    sio_hw->gpio_clr = TDI_MASK;
}

bool jtag_configure(uint8_t count, const uint8_t *ir_lengths) {
    if (count > JTAG_MAX_DEVICES) {
        return false;
    }
    uint16_t total = 0;
    for (uint8_t i = 0; i < count; i++) {
        s_ir_length[i] = ir_lengths[i];
        s_ir_before[i] = total;
        total += ir_lengths[i];
    }
    for (uint8_t i = 0; i < count; i++) {
        s_ir_after[i] = (uint16_t)(total - s_ir_before[i] - s_ir_length[i]);
    }
    s_dev_count = count;
    s_dev_index = 0;
    return true;
}

bool jtag_select_device(uint8_t index) {
    if (index >= s_dev_count) {
        return false;
    }
    s_dev_index = index;
    return true;
}

void jtag_set_idle_cycles(uint8_t idle_cycles) {
    s_idle_cycles = idle_cycles;
}

void jtag_ir(uint32_t ir) {
    tms(1); clk();              // Select-DR-Scan
    clk();                      // Select-IR-Scan
    tms(0); clk();              // Capture-IR
    clk();                      // Shift-IR
    tdi_out(1);
    for (uint32_t n = s_ir_before[s_dev_index]; n; n--) {
        clk();                  // bypass before
    }
    for (uint32_t n = s_ir_length[s_dev_index] - 1u; n; n--) {
        tdi_cycle(ir);
        ir >>= 1;
    }
    uint32_t n = s_ir_after[s_dev_index];
    if (n) {
        tdi_cycle(ir);          // last bit
        tdi_out(1);
        for (--n; n; n--) {
            clk();              // bypass after
        }
        tms(1); clk();          // bypass & Exit1-IR
    } else {
        tms(1); tdi_cycle(ir);  // last bit & Exit1-IR
    }
    clk();                      // Update-IR
    tms(0); clk();              // Run-Test/Idle
    park_tdi();
}

uint32_t jtag_read_idcode(void) {
    tms(1); clk();              // Select-DR-Scan
    tms(0); clk();              // Capture-DR
    clk();                      // Shift-DR
    for (uint32_t n = s_dev_index; n; n--) {
        clk();                  // bypass before
    }
    uint32_t val = 0;
    for (uint32_t n = 31; n; n--) {
        val |= cycle() << 31;
        val >>= 1;
    }
    tms(1);
    val |= cycle() << 31;       // D31 & Exit1-DR
    clk();                      // Update-DR
    tms(0); clk();              // Run-Test/Idle
    park_tdi();
    return val;
}

void jtag_write_abort(uint32_t data) {
    tms(1); clk();              // Select-DR-Scan
    tms(0); clk();              // Capture-DR
    clk();                      // Shift-DR
    for (uint32_t n = s_dev_index; n; n--) {
        clk();
    }
    tdi_out(0);
    clk(); clk(); clk();        // RnW=0, A2=0, A3=0
    for (uint32_t n = 31; n; n--) {
        tdi_cycle(data);
        data >>= 1;
    }
    uint32_t n = (uint32_t)(s_dev_count - s_dev_index - 1u);
    if (n) {
        tdi_cycle(data);
        for (--n; n; n--) {
            clk();
        }
        tms(1); clk();
    } else {
        tms(1); tdi_cycle(data);
    }
    clk();                      // Update-DR
    tms(0); clk();              // Run-Test/Idle
    park_tdi();
}

uint8_t jtag_transfer(uint32_t request, uint32_t *data) {
    uint32_t ack;
    tms(1); clk();              // Select-DR-Scan
    tms(0); clk();              // Capture-DR
    clk();                      // Shift-DR
    for (uint32_t n = s_dev_index; n; n--) {
        clk();                  // bypass before
    }
    // JTAG-DP ACK comes back LSB first as {WAIT, OK/FAULT, 0}; reorder into
    // CMSIS-DAP's encoding (1 = OK, 2 = WAIT).
    ack  = tdio_cycle(request >> 1) << 1; // RnW   / ACK.0
    ack |= tdio_cycle(request >> 2) << 0; // A2    / ACK.1
    ack |= tdio_cycle(request >> 3) << 2; // A3    / ACK.2

    if (ack != 0x1u) {
        tms(1); clk();          // Exit1-DR
    } else if (request & 0x2u) {
        uint32_t val = 0;
        for (uint32_t n = 31; n; n--) {
            val |= cycle() << 31;
            val >>= 1;
        }
        uint32_t n = (uint32_t)(s_dev_count - s_dev_index - 1u);
        if (n) {
            val |= cycle() << 31;
            for (--n; n; n--) {
                clk();
            }
            tms(1); clk();      // bypass & Exit1-DR
        } else {
            tms(1);
            val |= cycle() << 31; // D31 & Exit1-DR
        }
        if (data) {
            *data = val;
        }
    } else {
        uint32_t val = data ? *data : 0;
        for (uint32_t n = 31; n; n--) {
            tdi_cycle(val);
            val >>= 1;
        }
        uint32_t n = (uint32_t)(s_dev_count - s_dev_index - 1u);
        if (n) {
            tdi_cycle(val);
            for (--n; n; n--) {
                clk();
            }
            tms(1); clk();
        } else {
            tms(1); tdi_cycle(val); // D31 & Exit1-DR
        }
    }
    clk();                      // Update-DR
    tms(0); clk();              // Run-Test/Idle
    park_tdi();
    for (uint32_t n = s_idle_cycles; n; n--) {
        clk();
    }
    return (uint8_t)ack;
}

