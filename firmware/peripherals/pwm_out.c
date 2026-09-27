#include "peripherals/pwm_out.h"
#include "io/digital_bank.h"
#include "hardware/pwm.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

#define PWM_BASE_PIN     0
#define PWM_CHANNEL_COUNT 8
#define PWM_SLICE_COUNT   4 // GP0/1, GP2/3, GP4/5, GP6/7

static bool     s_active[PWM_CHANNEL_COUNT];      // per A/B channel
static bool     s_slice_configured[PWM_SLICE_COUNT];
static uint32_t s_slice_freq_hz[PWM_SLICE_COUNT]; // last frequency applied
static uint16_t s_slice_wrap[PWM_SLICE_COUNT];
static float    s_slice_clkdiv[PWM_SLICE_COUNT];

void pwm_out_init(void) {
    for (int i = 0; i < PWM_CHANNEL_COUNT; i++) {
        s_active[i] = false;
    }
    for (int i = 0; i < PWM_SLICE_COUNT; i++) {
        s_slice_configured[i] = false;
    }
}

static void release_to_pio(uint pin) {
    // Hands the pad back to the digital bank's PIO ctrl SM. The SM's own
    // pindirs/value registers were never touched while this pin was
    // diverted to PWM, so this alone restores correct digital-bank state.
    pio_gpio_init(pio0, pin);
    digital_bank_reapply_input_enables(); // pio_gpio_init() turned IE back on (E9)
}

static uint32_t clamp_frequency_hz(uint32_t requested_hz) {
    uint32_t clk_sys_hz = clock_get_hz(clk_sys);
    // clkdiv is 8 integer + 4 fractional bits, integer part >= 1; wrap is 16 bit.
    uint32_t min_hz = clk_sys_hz / (255u * 65536u) + 1u;
    uint32_t hz = requested_hz;
    if (hz < min_hz) {
        hz = min_hz;
    }
    if (hz > PWM_MAX_FREQUENCY_HZ) {
        hz = PWM_MAX_FREQUENCY_HZ;
    }
    return hz;
}

static void compute_wrap_clkdiv(uint32_t clk_sys_hz, uint32_t freq_hz,
                                 uint16_t *wrap_out, float *clkdiv_out) {
    uint32_t div_int = 1;
    uint32_t wrap;
    for (;;) {
        wrap = clk_sys_hz / (div_int * freq_hz);
        if (wrap <= 65536u || div_int >= 255u) {
            break;
        }
        div_int++;
    }
    if (wrap < 1u) {
        wrap = 1u;
    }
    if (wrap > 65536u) {
        wrap = 65536u;
    }
    *wrap_out = (uint16_t)(wrap - 1u); // TOP register holds count-1
    *clkdiv_out = (float)div_int;
}

bool pwm_out_config(uint8_t channel, bool enabled, uint32_t frequency_hz,
                     uint16_t duty_permille, pwm_out_config_result_t *out) {
    if (channel >= PWM_CHANNEL_COUNT) {
        return false;
    }
    if (!(digital_bank_armed_mask() & (1u << channel))) {
        return false;
    }
    if (duty_permille > 1000u) {
        duty_permille = 1000u;
    }

    uint pin = PWM_BASE_PIN + channel;
    uint slice = pwm_gpio_to_slice_num(pin);
    uint chan = pwm_gpio_to_channel(pin);

    if (!enabled) {
        if (s_active[channel]) {
            release_to_pio(pin);
            s_active[channel] = false;

            uint8_t sibling = channel ^ 1u;
            bool sibling_active = (sibling < PWM_CHANNEL_COUNT) && s_active[sibling];
            if (!sibling_active) {
                pwm_set_enabled(slice, false);
            }
        }
        if (out) {
            out->granted_frequency_hz = 0;
            out->granted_duty_permille = 0;
        }
        return true;
    }

    uint32_t granted_hz = clamp_frequency_hz(frequency_hz);
    uint32_t clk_sys_hz = clock_get_hz(clk_sys);

    bool slice_freq_matches = s_slice_configured[slice] && s_slice_freq_hz[slice] == granted_hz;

    uint16_t wrap;
    float clkdiv;
    if (!slice_freq_matches) {
        compute_wrap_clkdiv(clk_sys_hz, granted_hz, &wrap, &clkdiv);
        s_slice_wrap[slice] = wrap;
        s_slice_clkdiv[slice] = clkdiv;
        s_slice_freq_hz[slice] = granted_hz;
        s_slice_configured[slice] = true;
    } else {
        wrap = s_slice_wrap[slice];
        clkdiv = s_slice_clkdiv[slice];
    }

    uint32_t level = ((uint32_t)duty_permille * ((uint32_t)wrap + 1u)) / 1000u;

    gpio_set_function(pin, GPIO_FUNC_PWM);
    digital_bank_reapply_input_enables(); // gpio_set_function() turned IE back on (E9)

    if (!slice_freq_matches) {
        // Frequency change on a shared slice: brief disable is unavoidable
        // and affects the sibling channel too if it's active — documented
        // in protocol.h, not hidden.
        pwm_set_enabled(slice, false);
        pwm_set_wrap(slice, wrap);
        pwm_set_clkdiv(slice, clkdiv);
        pwm_set_chan_level(slice, chan, (uint16_t)level);
        pwm_set_enabled(slice, true);
    } else {
        // Duty-only change against an unchanged, already-running slice —
        // the CC register is double-buffered in hardware, glitch-free.
        pwm_set_chan_level(slice, chan, (uint16_t)level);
    }

    s_active[channel] = true;

    if (out) {
        out->granted_frequency_hz = granted_hz;
        out->granted_duty_permille = duty_permille;
    }
    return true;
}

bool pwm_out_channel_active(uint8_t channel) {
    return channel < PWM_CHANNEL_COUNT && s_active[channel];
}

void pwm_out_force_safe(void) {
    for (uint8_t channel = 0; channel < PWM_CHANNEL_COUNT; channel++) {
        if (s_active[channel]) {
            release_to_pio(PWM_BASE_PIN + channel);
            s_active[channel] = false;
        }
    }
    for (uint slice = 0; slice < PWM_SLICE_COUNT; slice++) {
        pwm_set_enabled(slice, false);
        s_slice_configured[slice] = false;
    }
}
