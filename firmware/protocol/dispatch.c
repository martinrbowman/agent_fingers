#include "protocol/dispatch.h"
#include "protocol/protocol.h"
#include "protocol/crc32c.h"
#include "app/board.h"
#include "io/safe_state.h"
#include "io/digital_bank.h"
#include "peripherals/adc_scan.h"
#include "peripherals/pwm_out.h"
#include "peripherals/uart_port.h"
#include "peripherals/i2c_bus.h"
#include "peripherals/spi_master.h"
#include "peripherals/spi_slave.h"
#include "build_info.h"
#include "pico/unique_id.h"
#include <string.h>
#include <stdbool.h>

// Streaming frame parser for the vendor-bulk transport (see usb_vendor.c).
// No dynamic allocation; a frame whose declared payload_len exceeds
// PROTOCOL_RX_PAYLOAD_CAP is discarded by byte-counted skip rather than
// buffered. Resyncs on bad magic/version/length/CRC per the plan.
//
// A truncated frame (sender stops mid-payload/skip/CRC and never sends the
// rest) is caught by an idle timeout in protocol_dispatch_poll_events():
// past PROTOCOL_STALLED_FRAME_TIMEOUT_US with no new byte in a non-header
// state, the partial frame is abandoned and the parser resyncs on its own.
// Found by fuzzing: a truncated declared-oversize frame otherwise sits in
// PSTATE_SKIP/PSTATE_CRC consuming whatever real traffic arrives next as if
// it were the missing tail, silently swallowing following requests until
// enough bytes happen to accumulate.
//
// Deliberately not yet implemented, deferred until an opcode that actually
// needs it exists: an idempotent-response cache for duplicate writes.
// DIGITAL_WRITE_MASKED is a level-set (not edge/pulse), so replaying it has
// the same effect as sending it once — not a hazard the cache needs to
// guard against yet. Revisit if a pulse/strobe-style write opcode arrives.

typedef enum {
    PSTATE_HEADER,
    PSTATE_PAYLOAD,
    PSTATE_SKIP,
    PSTATE_CRC,
} parser_state_t;

static protocol_output_fn s_output;

static parser_state_t s_state = PSTATE_HEADER;

static uint8_t  s_header_buf[PROTOCOL_HEADER_SIZE];
static size_t   s_header_have;
static frame_header_t s_hdr;

static uint8_t  s_payload_buf[PROTOCOL_RX_PAYLOAD_CAP];
static uint32_t s_payload_have;
static uint32_t s_skip_remaining;
static bool     s_oversize;

static uint8_t  s_crc_buf[4];
static size_t   s_crc_have;

static uint8_t  s_tx_buf[PROTOCOL_HEADER_SIZE + PROTOCOL_RX_PAYLOAD_CAP + 4];

static uint32_t s_frames_ok;
static uint32_t s_magic_resyncs;
static uint32_t s_err_bad_version;
static uint32_t s_err_bad_length;
static uint32_t s_err_bad_crc;
static uint32_t s_err_oversize;
static uint32_t s_err_stalled_frame;

// Real USB bulk writes deliver header+payload+CRC in one shot; a gap this
// long inside a frame only happens if the sender truncated it.
#define PROTOCOL_STALLED_FRAME_TIMEOUT_US (100 * 1000)
static uint64_t s_last_byte_time_us;

static void reset_to_header_state(void) {
    s_state = PSTATE_HEADER;
    s_header_have = 0;
}

static void emit_frame(uint32_t sequence, uint16_t opcode, uint8_t type,
                        uint16_t status, const uint8_t *payload, uint32_t payload_len) {
    frame_header_t h = {
        .magic = PROTOCOL_MAGIC,
        .version = PROTOCOL_VERSION,
        .type = type,
        .flags = 0,
        .sequence = sequence,
        .opcode = opcode,
        .status = status,
        .payload_len = payload_len,
    };
    memcpy(s_tx_buf, &h, PROTOCOL_HEADER_SIZE);
    if (payload_len) {
        memcpy(s_tx_buf + PROTOCOL_HEADER_SIZE, payload, payload_len);
    }
    uint32_t crc = crc32c(s_tx_buf, PROTOCOL_HEADER_SIZE + payload_len);
    memcpy(s_tx_buf + PROTOCOL_HEADER_SIZE + payload_len, &crc, 4);
    if (s_output) {
        s_output(s_tx_buf, PROTOCOL_HEADER_SIZE + payload_len + 4);
    }
}

static size_t handle_get_info(uint8_t *out) {
    info_response_t r;
    r.protocol_version = PROTOCOL_VERSION;
    r.fw_version_major = FW_VERSION_MAJOR;
    r.fw_version_minor = FW_VERSION_MINOR;
    r.fw_version_patch = FW_VERSION_PATCH;

    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    memcpy(r.board_unique_id, id.id, sizeof(r.board_unique_id));

    memset(r.git_hash, 0, sizeof(r.git_hash));
    strncpy(r.git_hash, FW_GIT_HASH, sizeof(r.git_hash) - 1);

    memcpy(out, &r, sizeof(r));
    return sizeof(r);
}

static size_t handle_get_capabilities(uint8_t *out) {
    capabilities_response_t r = {
        .digital_channel_count = 8,
        .spi_master_count = 1,
        .spi_slave_count = 1,
        .i2c_master_count = 1,
        .i2c_slave_count = 1,
        .uart_count = 1,
        .adc_channel_count = 3,
        .reserved = 0,
    };
    memcpy(out, &r, sizeof(r));
    return sizeof(r);
}

static size_t handle_get_status(uint8_t *out) {
    status_response_t r = {
        .uptime_us = board_time_us(),
        .reset_cause = (uint8_t)board_reset_cause(),
        .safe_state_reason = (uint8_t)safe_state_last_reason(),
        .outputs_armed = digital_bank_armed() ? 1 : 0,
        .armed_mask = digital_bank_armed_mask(),
        .frames_ok = s_frames_ok,
        .magic_resyncs = s_magic_resyncs,
        .err_bad_version = s_err_bad_version,
        .err_bad_length = s_err_bad_length,
        .err_bad_crc = s_err_bad_crc,
        .err_oversize = s_err_oversize,
        .err_stalled_frame = s_err_stalled_frame,
    };
    digital_bank_get_challenge(r.arm_challenge);
    memcpy(out, &r, sizeof(r));
    return sizeof(r);
}

static size_t handle_arm_outputs(const uint8_t *req_payload, uint32_t req_len,
                                  uint8_t *out, uint16_t *status_out) {
    if (req_len != sizeof(arm_outputs_request_t)) {
        *status_out = STATUS_MALFORMED;
        return 0;
    }
    arm_outputs_request_t req;
    memcpy(&req, req_payload, sizeof(req));

    uint32_t granted_lease_ms = 0;
    if (!digital_bank_arm(req.challenge, req.output_mask, req.lease_ms, &granted_lease_ms)) {
        *status_out = STATUS_MALFORMED; // stale/incorrect challenge
        return 0;
    }

    arm_outputs_response_t resp = { .granted_lease_ms = granted_lease_ms };
    memcpy(out, &resp, sizeof(resp));
    return sizeof(resp);
}

static size_t handle_disarm_outputs(void) {
    // safe_state_enter() forces the digital bank safe and records the
    // reason for GET_STATUS — the one path that does both.
    safe_state_enter(SAFE_STATE_EXPLICIT_DISARM);
    return 0;
}

static size_t handle_digital_write_masked(const uint8_t *req_payload, uint32_t req_len,
                                           uint16_t *status_out) {
    if (req_len != sizeof(digital_write_masked_request_t)) {
        *status_out = STATUS_MALFORMED;
        return 0;
    }
    digital_write_masked_request_t req;
    memcpy(&req, req_payload, sizeof(req));

    if (!digital_bank_write_masked(req.mask, req.values)) {
        *status_out = STATUS_MALFORMED; // not armed, mask outside armed_mask,
                                          // or a targeted channel is PWM-active
        return 0;
    }
    return 0;
}

static size_t handle_digital_read(const uint8_t *req_payload, uint32_t req_len,
                                   uint8_t *out, uint16_t *status_out) {
    if (req_len != sizeof(digital_read_request_t)) {
        *status_out = STATUS_MALFORMED;
        return 0;
    }
    digital_read_request_t req;
    memcpy(&req, req_payload, sizeof(req));

    digital_read_response_t resp = { .values = digital_bank_read(req.mask) };
    memcpy(out, &resp, sizeof(resp));
    return sizeof(resp);
}

static size_t handle_spi_slave_config(const uint8_t *req_payload, uint32_t req_len,
                                       uint8_t *out, uint16_t *status_out) {
    if (req_len != sizeof(spi_slave_config_request_t)) {
        *status_out = STATUS_MALFORMED;
        return 0;
    }
    spi_slave_config_request_t req;
    memcpy(&req, req_payload, sizeof(req));

    spi_slave_config_result_t result;
    spi_slave_config(req.enabled != 0, &result);

    spi_slave_config_response_t resp = {
        .bytes_received = result.bytes_received,
        .bytes_sent = result.bytes_sent,
        .transaction_count = result.transaction_count,
    };
    memcpy(out, &resp, sizeof(resp));
    return sizeof(resp);
}

static size_t handle_spi_xfer(const uint8_t *req_payload, uint32_t req_len,
                               uint8_t *out, uint16_t *status_out) {
    if (req_len < sizeof(spi_xfer_request_t)) {
        *status_out = STATUS_MALFORMED;
        return 0;
    }
    spi_xfer_request_t req;
    memcpy(&req, req_payload, sizeof(req));

    if (req_len != sizeof(req) + req.len) {
        *status_out = STATUS_MALFORMED;
        return 0;
    }
    uint16_t header_size = (uint16_t)sizeof(spi_xfer_response_t);
    uint16_t max_len_fit = (uint16_t)(PROTOCOL_RX_PAYLOAD_CAP - header_size);
    if (req.len > max_len_fit) {
        *status_out = STATUS_MALFORMED;
        return 0;
    }

    spi_master_xfer_result_t result;
    if (!spi_master_xfer(req.mode, req.hz, req_payload + sizeof(req), req.len,
                          out + header_size, &result)) {
        *status_out = STATUS_MALFORMED; // unsupported mode (only 0 this pass), or len too large
        return 0;
    }

    spi_xfer_response_t resp = {
        .granted_hz = result.granted_hz,
        .len = req.len,
        .reserved = 0,
    };
    memcpy(out, &resp, sizeof(resp));
    return (size_t)header_size + req.len;
}

static size_t handle_i2c_xfer(const uint8_t *req_payload, uint32_t req_len,
                               uint8_t *out, uint16_t *status_out) {
    if (req_len < sizeof(i2c_xfer_request_t)) {
        *status_out = STATUS_MALFORMED;
        return 0;
    }
    i2c_xfer_request_t req;
    memcpy(&req, req_payload, sizeof(req));

    if (req_len != sizeof(req) + req.write_len) {
        *status_out = STATUS_MALFORMED;
        return 0;
    }
    uint16_t header_size = (uint16_t)sizeof(i2c_xfer_response_t);
    uint16_t max_read_fit = (uint16_t)(PROTOCOL_RX_PAYLOAD_CAP - header_size);
    if (req.read_len > max_read_fit) {
        *status_out = STATUS_MALFORMED;
        return 0;
    }

    i2c_master_xfer_result_t result;
    i2c_master_xfer(req.address, req_payload + sizeof(req), req.write_len,
                     req.read_len, req.timeout_ms, out + header_size, &result);

    i2c_xfer_response_t resp = {
        .write_status = result.write_status,
        .read_status = result.read_status,
        .bytes_written = result.bytes_written,
        .bytes_read = result.bytes_read,
    };
    memcpy(out, &resp, sizeof(resp));
    return (size_t)header_size + result.bytes_read;
}

static size_t handle_i2c_slave_config(const uint8_t *req_payload, uint32_t req_len,
                                       uint8_t *out, uint16_t *status_out) {
    if (req_len != sizeof(i2c_slave_config_request_t)) {
        *status_out = STATUS_MALFORMED;
        return 0;
    }
    i2c_slave_config_request_t req;
    memcpy(&req, req_payload, sizeof(req));

    i2c_slave_config_result_t result;
    i2c_slave_config(req.enabled != 0, req.address, &result);

    i2c_slave_config_response_t resp = {
        .bytes_received = result.bytes_received,
        .bytes_sent = result.bytes_sent,
        .transaction_count = result.transaction_count,
    };
    memcpy(out, &resp, sizeof(resp));
    return sizeof(resp);
}

static size_t handle_uart_config(const uint8_t *req_payload, uint32_t req_len,
                                  uint8_t *out, uint16_t *status_out) {
    if (req_len != sizeof(uart_config_request_t)) {
        *status_out = STATUS_MALFORMED;
        return 0;
    }
    uart_config_request_t req;
    memcpy(&req, req_payload, sizeof(req));

    uart_port_config_result_t result;
    uart_port_config(req.baud_rate, req.data_bits, req.stop_bits, req.parity, &result);

    uart_config_response_t resp = { .granted_baud_rate = result.granted_baud_rate };
    memcpy(out, &resp, sizeof(resp));
    return sizeof(resp);
}

static size_t handle_uart_write(const uint8_t *req_payload, uint32_t req_len,
                                 uint16_t *status_out) {
    if (!uart_port_write(req_payload, (uint16_t)req_len)) {
        *status_out = STATUS_MALFORMED; // not configured, or TX still draining
        return 0;
    }
    return 0;
}

static size_t handle_uart_read(const uint8_t *req_payload, uint32_t req_len,
                                uint8_t *out, uint16_t *status_out) {
    if (req_len != sizeof(uart_read_request_t)) {
        *status_out = STATUS_MALFORMED;
        return 0;
    }
    uart_read_request_t req;
    memcpy(&req, req_payload, sizeof(req));

    uint16_t header_size = (uint16_t)sizeof(uart_read_response_t);
    uint16_t max_bytes_fit = (uint16_t)(PROTOCOL_RX_PAYLOAD_CAP - header_size);
    uint16_t limit = req.max_bytes > max_bytes_fit ? max_bytes_fit : req.max_bytes;

    uint16_t returned = 0;
    uart_port_status_t status;
    if (!uart_port_read(limit, out + header_size, &returned, &status)) {
        *status_out = STATUS_MALFORMED; // not configured
        return 0;
    }

    uart_read_response_t resp = {
        .framing_errors = status.framing_errors,
        .parity_errors = status.parity_errors,
        .break_errors = status.break_errors,
        .overrun_errors = status.overrun_errors,
        .ring_overrun_count = status.ring_overrun_count,
        .returned_count = returned,
        .reserved = 0,
    };
    memcpy(out, &resp, sizeof(resp));
    return (size_t)header_size + returned;
}

static size_t handle_pwm_config(const uint8_t *req_payload, uint32_t req_len,
                                 uint8_t *out, uint16_t *status_out) {
    if (req_len != sizeof(pwm_config_request_t)) {
        *status_out = STATUS_MALFORMED;
        return 0;
    }
    pwm_config_request_t req;
    memcpy(&req, req_payload, sizeof(req));

    pwm_out_config_result_t result;
    if (!pwm_out_config(req.channel, req.enabled != 0, req.frequency_hz, req.duty_permille, &result)) {
        *status_out = STATUS_MALFORMED; // bad channel, or not in armed_mask
        return 0;
    }

    pwm_config_response_t resp = {
        .granted_frequency_hz = result.granted_frequency_hz,
        .granted_duty_permille = result.granted_duty_permille,
        .reserved = 0,
    };
    memcpy(out, &resp, sizeof(resp));
    return sizeof(resp);
}

static size_t handle_adc_scan_config(const uint8_t *req_payload, uint32_t req_len,
                                      uint8_t *out, uint16_t *status_out) {
    if (req_len != sizeof(adc_scan_config_request_t)) {
        *status_out = STATUS_MALFORMED;
        return 0;
    }
    adc_scan_config_request_t req;
    memcpy(&req, req_payload, sizeof(req));

    adc_scan_config_result_t result;
    adc_scan_config(req.channel_mask, req.rate_hz, req.max_samples, &result);

    adc_scan_config_response_t resp = {
        .scan_id = result.scan_id,
        .granted_rate_hz = result.granted_rate_hz,
        .channel_count = result.channel_count,
        .reserved = {0, 0, 0},
        .buffer_capacity_samples = result.buffer_capacity_samples,
        .vref_millivolts = result.vref_millivolts,
        .reserved2 = 0,
    };
    memcpy(out, &resp, sizeof(resp));
    return sizeof(resp);
}

static size_t handle_adc_scan_read(const uint8_t *req_payload, uint32_t req_len,
                                    uint8_t *out, uint16_t *status_out) {
    if (req_len != sizeof(adc_scan_read_request_t)) {
        *status_out = STATUS_MALFORMED;
        return 0;
    }
    adc_scan_read_request_t req;
    memcpy(&req, req_payload, sizeof(req));

    uint16_t header_size = (uint16_t)sizeof(adc_scan_read_response_t);
    uint16_t max_samples_fit = (uint16_t)((PROTOCOL_RX_PAYLOAD_CAP - header_size) / sizeof(uint16_t));
    uint16_t limit = req.limit > max_samples_fit ? max_samples_fit : req.limit;

    uint16_t returned = 0;
    adc_scan_status_t status;
    if (!adc_scan_read(req.scan_id, req.offset, limit, (uint16_t *)(out + header_size), &returned, &status)) {
        *status_out = STATUS_MALFORMED; // stale/unknown scan_id
        return 0;
    }

    adc_scan_read_response_t resp = {
        .scan_id = status.scan_id,
        .total_captured = status.total_captured,
        .overrun_count = status.overrun_count,
        .start_timestamp_us = status.start_timestamp_us,
        .rate_hz = status.rate_hz,
        .channel_count = status.channel_count,
        .reserved = 0,
        .returned_count = returned,
    };
    memcpy(out, &resp, sizeof(resp));
    return (size_t)header_size + (size_t)returned * sizeof(uint16_t);
}

static size_t handle_digital_capture_start(const uint8_t *req_payload, uint32_t req_len,
                                            uint8_t *out, uint16_t *status_out) {
    if (req_len != sizeof(digital_capture_start_request_t)) {
        *status_out = STATUS_MALFORMED;
        return 0;
    }
    digital_capture_start_request_t req;
    memcpy(&req, req_payload, sizeof(req));

    digital_capture_start_result_t result;
    digital_capture_start(req.rate_hz, req.max_samples, &result);

    digital_capture_start_response_t resp = {
        .capture_id = result.capture_id,
        .granted_rate_hz = result.granted_rate_hz,
        .buffer_capacity_samples = result.buffer_capacity_samples,
    };
    memcpy(out, &resp, sizeof(resp));
    return sizeof(resp);
}

static size_t handle_digital_capture_stop(uint8_t *out) {
    digital_capture_stop_result_t result;
    digital_capture_stop(&result);

    digital_capture_stop_response_t resp = {
        .capture_id = result.capture_id,
        .total_captured = result.total_captured,
    };
    memcpy(out, &resp, sizeof(resp));
    return sizeof(resp);
}

static size_t handle_digital_capture_read(const uint8_t *req_payload, uint32_t req_len,
                                           uint8_t *out, uint16_t *status_out) {
    if (req_len != sizeof(digital_capture_read_request_t)) {
        *status_out = STATUS_MALFORMED;
        return 0;
    }
    digital_capture_read_request_t req;
    memcpy(&req, req_payload, sizeof(req));

    uint16_t header_size = (uint16_t)sizeof(digital_capture_read_response_t);
    uint16_t max_samples_fit = (uint16_t)(PROTOCOL_RX_PAYLOAD_CAP - header_size);
    uint16_t limit = req.limit > max_samples_fit ? max_samples_fit : req.limit;

    uint16_t returned = 0;
    digital_capture_status_t status;
    if (!digital_capture_read(req.capture_id, req.offset, limit, out + header_size, &returned, &status)) {
        *status_out = STATUS_MALFORMED; // stale/unknown capture_id
        return 0;
    }

    digital_capture_read_response_t resp = {
        .capture_id = status.capture_id,
        .total_captured = status.total_captured,
        .overrun_count = status.overrun_count,
        .start_timestamp_us = status.start_timestamp_us,
        .rate_hz = status.rate_hz,
        .returned_count = returned,
        .reserved = 0,
    };
    memcpy(out, &resp, sizeof(resp));
    return (size_t)header_size + returned;
}

static size_t handle_ping(const uint8_t *req_payload, uint32_t req_len, uint8_t *out) {
    digital_bank_renew_lease(); // per plan: PING doubles as the lease keepalive
    uint64_t ts = board_time_us();
    memcpy(out, &ts, sizeof(ts));

    uint32_t echo_cap = PROTOCOL_RX_PAYLOAD_CAP - (uint32_t)sizeof(ts);
    uint32_t echo_len = req_len > echo_cap ? echo_cap : req_len;
    if (echo_len) {
        memcpy(out + sizeof(ts), req_payload, echo_len);
    }
    return sizeof(ts) + echo_len;
}

static void dispatch_opcode(void) {
    static uint8_t payload_out[PROTOCOL_RX_PAYLOAD_CAP];
    size_t out_len = 0;
    uint16_t status = STATUS_OK;
    uint8_t type = FRAME_TYPE_RESPONSE;

    switch (s_hdr.opcode) {
    case OPCODE_GET_INFO:
        out_len = handle_get_info(payload_out);
        break;
    case OPCODE_GET_CAPABILITIES:
        out_len = handle_get_capabilities(payload_out);
        break;
    case OPCODE_GET_STATUS:
        out_len = handle_get_status(payload_out);
        break;
    case OPCODE_PING:
        out_len = handle_ping(s_payload_buf, s_payload_have, payload_out);
        break;
    case OPCODE_ARM_OUTPUTS:
        out_len = handle_arm_outputs(s_payload_buf, s_payload_have, payload_out, &status);
        if (status != STATUS_OK) {
            type = FRAME_TYPE_ERROR;
        }
        break;
    case OPCODE_DISARM_OUTPUTS:
        out_len = handle_disarm_outputs();
        break;
    case OPCODE_DIGITAL_WRITE_MASKED:
        out_len = handle_digital_write_masked(s_payload_buf, s_payload_have, &status);
        if (status != STATUS_OK) {
            type = FRAME_TYPE_ERROR;
        }
        break;
    case OPCODE_DIGITAL_READ:
        out_len = handle_digital_read(s_payload_buf, s_payload_have, payload_out, &status);
        if (status != STATUS_OK) {
            type = FRAME_TYPE_ERROR;
        }
        break;
    case OPCODE_PWM_CONFIG:
        out_len = handle_pwm_config(s_payload_buf, s_payload_have, payload_out, &status);
        if (status != STATUS_OK) {
            type = FRAME_TYPE_ERROR;
        }
        break;
    case OPCODE_UART_CONFIG:
        out_len = handle_uart_config(s_payload_buf, s_payload_have, payload_out, &status);
        if (status != STATUS_OK) {
            type = FRAME_TYPE_ERROR;
        }
        break;
    case OPCODE_UART_WRITE:
        out_len = handle_uart_write(s_payload_buf, s_payload_have, &status);
        if (status != STATUS_OK) {
            type = FRAME_TYPE_ERROR;
        }
        break;
    case OPCODE_UART_READ:
        out_len = handle_uart_read(s_payload_buf, s_payload_have, payload_out, &status);
        if (status != STATUS_OK) {
            type = FRAME_TYPE_ERROR;
        }
        break;
    case OPCODE_I2C_XFER:
        out_len = handle_i2c_xfer(s_payload_buf, s_payload_have, payload_out, &status);
        if (status != STATUS_OK) {
            type = FRAME_TYPE_ERROR;
        }
        break;
    case OPCODE_I2C_SLAVE_CONFIG:
        out_len = handle_i2c_slave_config(s_payload_buf, s_payload_have, payload_out, &status);
        if (status != STATUS_OK) {
            type = FRAME_TYPE_ERROR;
        }
        break;
    case OPCODE_SPI_XFER:
        out_len = handle_spi_xfer(s_payload_buf, s_payload_have, payload_out, &status);
        if (status != STATUS_OK) {
            type = FRAME_TYPE_ERROR;
        }
        break;
    case OPCODE_SPI_SLAVE_CONFIG:
        out_len = handle_spi_slave_config(s_payload_buf, s_payload_have, payload_out, &status);
        if (status != STATUS_OK) {
            type = FRAME_TYPE_ERROR;
        }
        break;
    case OPCODE_DIGITAL_CAPTURE_START:
        out_len = handle_digital_capture_start(s_payload_buf, s_payload_have, payload_out, &status);
        if (status != STATUS_OK) {
            type = FRAME_TYPE_ERROR;
        }
        break;
    case OPCODE_DIGITAL_CAPTURE_STOP:
        out_len = handle_digital_capture_stop(payload_out);
        break;
    case OPCODE_DIGITAL_CAPTURE_READ:
        out_len = handle_digital_capture_read(s_payload_buf, s_payload_have, payload_out, &status);
        if (status != STATUS_OK) {
            type = FRAME_TYPE_ERROR;
        }
        break;
    case OPCODE_ADC_SCAN_CONFIG:
        out_len = handle_adc_scan_config(s_payload_buf, s_payload_have, payload_out, &status);
        if (status != STATUS_OK) {
            type = FRAME_TYPE_ERROR;
        }
        break;
    case OPCODE_ADC_SCAN_READ:
        out_len = handle_adc_scan_read(s_payload_buf, s_payload_have, payload_out, &status);
        if (status != STATUS_OK) {
            type = FRAME_TYPE_ERROR;
        }
        break;
    default:
        type = FRAME_TYPE_ERROR;
        status = STATUS_UNSUPPORTED_OPCODE;
        break;
    }

    emit_frame(s_hdr.sequence, s_hdr.opcode, type, status, payload_out, (uint32_t)out_len);
}

static void on_frame_ready(void) {
    if (s_oversize) {
        s_err_oversize++;
        emit_frame(s_hdr.sequence, s_hdr.opcode, FRAME_TYPE_ERROR, STATUS_PAYLOAD_TOO_LARGE, NULL, 0);
        return;
    }

    uint32_t received_crc;
    memcpy(&received_crc, s_crc_buf, 4);

    // crc32c() takes one contiguous buffer; header and payload are stored
    // separately, so join them here before checksumming. Static rather than
    // stack-local since this fires from the main poll loop, not an ISR.
    static uint8_t combined[PROTOCOL_HEADER_SIZE + PROTOCOL_RX_PAYLOAD_CAP];
    memcpy(combined, s_header_buf, PROTOCOL_HEADER_SIZE);
    if (s_payload_have) {
        memcpy(combined + PROTOCOL_HEADER_SIZE, s_payload_buf, s_payload_have);
    }
    uint32_t actual_crc = crc32c(combined, PROTOCOL_HEADER_SIZE + s_payload_have);

    if (actual_crc != received_crc) {
        s_err_bad_crc++;
        emit_frame(s_hdr.sequence, s_hdr.opcode, FRAME_TYPE_ERROR, STATUS_BAD_CRC, NULL, 0);
        return;
    }

    s_frames_ok++;
    dispatch_opcode();
}

static void feed_byte(uint8_t byte) {
    s_last_byte_time_us = board_time_us();
    switch (s_state) {
    case PSTATE_HEADER: {
        s_header_buf[s_header_have++] = byte;
        if (s_header_have < PROTOCOL_HEADER_SIZE) {
            if (s_header_have == 4) {
                uint32_t magic;
                memcpy(&magic, s_header_buf, 4);
                if (magic != PROTOCOL_MAGIC) {
                    // Slide the window by one byte and keep scanning for magic.
                    memmove(s_header_buf, s_header_buf + 1, 3);
                    s_header_have = 3;
                    s_magic_resyncs++;
                }
            }
            return;
        }

        memcpy(&s_hdr, s_header_buf, PROTOCOL_HEADER_SIZE);

        if (s_hdr.version != PROTOCOL_VERSION) {
            s_err_bad_version++;
            emit_frame(s_hdr.sequence, s_hdr.opcode, FRAME_TYPE_ERROR, STATUS_BAD_VERSION, NULL, 0);
            reset_to_header_state();
            return;
        }
        if (s_hdr.payload_len > PROTOCOL_MAX_PAYLOAD) {
            s_err_bad_length++;
            emit_frame(s_hdr.sequence, s_hdr.opcode, FRAME_TYPE_ERROR, STATUS_BAD_LENGTH, NULL, 0);
            reset_to_header_state();
            return;
        }

        s_oversize = s_hdr.payload_len > PROTOCOL_RX_PAYLOAD_CAP;
        if (s_oversize) {
            s_skip_remaining = s_hdr.payload_len;
            s_state = s_skip_remaining ? PSTATE_SKIP : PSTATE_CRC;
        } else {
            s_payload_have = 0;
            s_state = s_hdr.payload_len ? PSTATE_PAYLOAD : PSTATE_CRC;
        }
        s_crc_have = 0;
        return;
    }

    case PSTATE_PAYLOAD:
        s_payload_buf[s_payload_have++] = byte;
        if (s_payload_have == s_hdr.payload_len) {
            s_state = PSTATE_CRC;
        }
        return;

    case PSTATE_SKIP:
        s_skip_remaining--;
        if (s_skip_remaining == 0) {
            s_state = PSTATE_CRC;
        }
        return;

    case PSTATE_CRC:
        s_crc_buf[s_crc_have++] = byte;
        if (s_crc_have == 4) {
            on_frame_ready();
            reset_to_header_state();
        }
        return;
    }
}

void protocol_dispatch_init(protocol_output_fn output) {
    s_output = output;
    reset_to_header_state();
}

void protocol_dispatch_feed(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        feed_byte(data[i]);
    }
}

void protocol_dispatch_poll_events(void) {
    if (s_state != PSTATE_HEADER &&
        board_time_us() - s_last_byte_time_us > PROTOCOL_STALLED_FRAME_TIMEOUT_US) {
        s_err_stalled_frame++;
        reset_to_header_state();
    }

    digital_capture_stop_result_t completed;
    if (digital_capture_task(&completed)) {
        digital_capture_stop_response_t resp = {
            .capture_id = completed.capture_id,
            .total_captured = completed.total_captured,
        };
        // sequence=0: no originating request to correlate this against.
        emit_frame(0, OPCODE_DIGITAL_CAPTURE_STOP, FRAME_TYPE_EVENT, STATUS_OK,
                   (const uint8_t *)&resp, sizeof(resp));
    }

    adc_scan_complete_result_t adc_completed;
    if (adc_scan_task(&adc_completed)) {
        adc_scan_complete_event_t resp = {
            .scan_id = adc_completed.scan_id,
            .total_captured = adc_completed.total_captured,
        };
        emit_frame(0, OPCODE_ADC_SCAN_READ, FRAME_TYPE_EVENT, STATUS_OK,
                   (const uint8_t *)&resp, sizeof(resp));
    }
}
