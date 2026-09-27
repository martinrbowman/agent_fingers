#include "debug/dap.h"
#include "debug/debug_session.h"
#include "debug/swd.h"
#include "debug/jtag.h"
#include "debug/dap_usb.h"
#include "debug/swo.h"
#include "transport/usb_transport.h"
#include "build_info.h"
#include "pico/time.h"
#include "pico/unique_id.h"
#include "tusb.h"
#include <string.h>

#define DAP_PACKET_SIZE  DAP_USB_PACKET_SIZE
// Packets the host may have in flight. With 1, every command cost a full
// USB round trip (~19 KiB/s ESP32 JTAG reads). dap_usb.c queues this many
// and sends exactly one response per USB transfer.
#define DAP_PACKET_COUNT DAP_USB_QUEUE_DEPTH
#define DAP_INACTIVITY_TIMEOUT_US 10000000ull

#define ID_DAP_INFO        0x00u
#define ID_DAP_HOST_STATUS 0x01u
#define ID_DAP_CONNECT     0x02u
#define ID_DAP_DISCONNECT  0x03u
#define ID_DAP_TRANSFER_CONFIGURE 0x04u
#define ID_DAP_TRANSFER           0x05u
#define ID_DAP_TRANSFER_BLOCK     0x06u
#define ID_DAP_TRANSFER_ABORT     0x07u
#define ID_DAP_WRITE_ABORT        0x08u
#define ID_DAP_DELAY              0x09u
#define ID_DAP_RESET_TARGET       0x0Au
#define ID_DAP_SWJ_PINS           0x10u
#define ID_DAP_SWJ_CLOCK          0x11u
#define ID_DAP_SWJ_SEQUENCE       0x12u
#define ID_DAP_SWD_CONFIGURE      0x13u
#define ID_DAP_JTAG_SEQUENCE      0x14u
#define ID_DAP_JTAG_CONFIGURE     0x15u
#define ID_DAP_JTAG_IDCODE        0x16u
#define ID_DAP_SWO_TRANSPORT      0x17u
#define ID_DAP_SWO_MODE           0x18u
#define ID_DAP_SWO_BAUDRATE       0x19u
#define ID_DAP_SWO_CONTROL        0x1Au
#define ID_DAP_SWO_STATUS         0x1Bu
#define ID_DAP_SWO_DATA           0x1Cu
#define ID_DAP_SWD_SEQUENCE       0x1Du
#define ID_DAP_INVALID     0xFFu

// DAP_Transfer request bits / response bits.
#define XFER_APnDP       0x01u
#define XFER_RnW         0x02u
#define XFER_MATCH_VALUE 0x10u
#define XFER_MATCH_MASK  0x20u
#define XFER_MISMATCH    0x10u
#define DP_RDBUFF        0x0Cu
#define SWJ_PINS_MAX_WAIT_US 3000000u // CMSIS-DAP spec limit

#define DAP_OK    0x00u
#define DAP_ERROR 0xFFu

#define DAP_PORT_DEFAULT 0u
#define DAP_PORT_SWD     1u
#define DAP_PORT_JTAG    2u

// DAP_Info capabilities byte: bit0 SWD, bit1 JTAG. Advertised only once
// the sequences exist (9.3/9.4), so tools fail clearly instead of
// half-working in the meantime.
#define DAP_CAPABILITIES 0x07u // SWD | JTAG | SWO (UART mode)

// One CMSIS-DAP command per USB packet (see dap_usb.c).
typedef struct {
    uint8_t  data[DAP_PACKET_SIZE];
    uint16_t len;
} dap_packet_t;

static bool     s_connect_pending;   // DAP_Connect waiting for the pins
static bool     s_connect_requested; // debug_session_request_start() accepted
static uint8_t  s_connect_port;
static uint64_t s_last_activity_us;
// Which PHY currently owns the pins (set once core0 has granted them).
typedef enum { PORT_OFF, PORT_SWD, PORT_JTAG } port_t;
static port_t   s_port;

static uint32_t s_swd_max_hz;  // fastest bit-bang clock, measured at boot
static uint32_t s_jtag_max_hz;
static uint16_t s_wait_retry = 100;
static uint8_t  s_swo_transport; // 0 none, 1 DAP_SWO_Data
static uint8_t  s_swo_mode;      // 0 off, 1 UART
static uint16_t s_match_retry;
static uint32_t s_match_mask;

void dap_init(void) {
    swo_init();
    s_swd_max_hz = swd_calibrate();
    s_jtag_max_hz = jtag_calibrate();
    swd_set_clock(0); // defaults
    jtag_set_clock(0);
    s_connect_pending = false;
    s_connect_requested = false;
}

static void send(const uint8_t *resp, uint32_t len) {
    dap_usb_send(resp, (uint16_t)len); // caller checked dap_usb_tx_ready()
}

static bool swd_ready(void) {
    return s_port == PORT_SWD;
}

static bool jtag_ready(void) {
    return s_port == PORT_JTAG;
}

// Pins back to high-Z first, then let core0 take them back.
static void end_session(debug_end_reason_t reason) {
    swo_stop();
    if (s_port == PORT_SWD) {
        swd_port_off();
    } else if (s_port == PORT_JTAG) {
        jtag_port_off();
    }
    s_port = PORT_OFF;
    debug_session_request_end(reason);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// One SWD transfer, retried while the target answers WAIT.
static uint8_t xfer(uint32_t request, uint32_t *data) {
    uint32_t retry = s_wait_retry;
    uint8_t ack;
    do {
        ack = swd_transfer(request, data);
        usb_transport_core1_alive();
    } while (ack == SWD_ACK_WAIT && retry--);
    return ack;
}

static uint32_t info_string(uint8_t *out, const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1; // CMSIS-DAP strings include the NUL
    if (n > DAP_PACKET_SIZE - 2) {
        n = DAP_PACKET_SIZE - 2;
    }
    memcpy(out, s, n);
    return n;
}

static void handle_info(const uint8_t *req, uint16_t len) {
    uint8_t resp[DAP_PACKET_SIZE] = {ID_DAP_INFO, 0};
    uint8_t *data = &resp[2];
    uint32_t n = 0;
    uint8_t id = len > 1 ? req[1] : 0;
    switch (id) {
    case 0x01: n = info_string(data, "agent_fingers"); break;
    case 0x02: n = info_string(data, "Agent Fingers CMSIS-DAP"); break;
    case 0x03: {
        char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        pico_get_unique_board_id_string(serial, sizeof(serial));
        n = info_string(data, serial);
        break;
    }
    case 0x04: n = info_string(data, "2.1.1"); break;
    case 0x09: {
        char fw[16];
        snprintf(fw, sizeof(fw), "%d.%d.%d", FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH);
        n = info_string(data, fw);
        break;
    }
    case 0xF0: data[0] = DAP_CAPABILITIES; n = 1; break;
    case 0xFD: wr32(data, SWO_BUFFER_SIZE); n = 4; break;
    case 0xFE: data[0] = DAP_PACKET_COUNT; n = 1; break;
    case 0xFF:
        data[0] = (uint8_t)(DAP_PACKET_SIZE & 0xFFu);
        data[1] = (uint8_t)(DAP_PACKET_SIZE >> 8);
        n = 2;
        break;
    default: n = 0; break; // unknown/empty IDs answer with length 0
    }
    resp[1] = (uint8_t)n;
    send(resp, 2 + n);
}

static void handle_connect(const uint8_t *req, uint16_t len) {
    uint8_t port = len > 1 ? req[1] : DAP_PORT_DEFAULT;
    if (port == DAP_PORT_DEFAULT) {
        port = DAP_PORT_SWD;
    }
    if (port != DAP_PORT_SWD && port != DAP_PORT_JTAG) {
        uint8_t resp[2] = {ID_DAP_CONNECT, 0}; // 0 = failed
        send(resp, 2);
        return;
    }
    if (debug_session_granted()) {
        uint8_t resp[2] = {ID_DAP_CONNECT, s_connect_port};
        send(resp, 2);
        return;
    }
    // The reply waits for core0 to hand the pins over (which may first
    // abort an SPI master transfer) -- see dap_task().
    s_connect_port = port;
    s_connect_pending = true;
    s_connect_requested = false;
}

// DAP_Transfer, following the CMSIS-DAP reference (DAP.c,
// DAP_SWD_Transfer): AP reads are posted, so each AP read returns the
// previous one's data and the last is collected from DP RDBUFF; a final
// write is confirmed by reading RDBUFF.
static void handle_transfer(const uint8_t *req, uint16_t len) {
    uint8_t resp[DAP_PACKET_SIZE] = {ID_DAP_TRANSFER, 0, 0};
    uint8_t *out = &resp[3];
    const uint8_t *out_end = resp + sizeof(resp);
    const uint8_t *p = req + 1;
    const uint8_t *end = req + len;

    uint32_t response_count = 0;
    uint8_t response_value = 0;
    bool post_read = false;
    bool check_write = false;
    uint32_t data = 0;

    if (end - p < 2 || !swd_ready()) {
        send(resp, 3); // count 0, value 0: nothing done
        return;
    }
    p++; // DAP index (single-DAP probe)
    uint32_t request_count = *p++;

    for (; request_count; request_count--) {
        if (p >= end) {
            break;
        }
        uint8_t rq = *p++;
        if (rq & XFER_RnW) {
            if (post_read) {
                if ((rq & (XFER_APnDP | XFER_MATCH_VALUE)) == XFER_APnDP) {
                    response_value = xfer(rq, &data); // previous data, post next
                } else {
                    response_value = xfer(DP_RDBUFF | XFER_RnW, &data);
                    post_read = false;
                }
                if (response_value != SWD_ACK_OK) {
                    break;
                }
                if (out + 4 > out_end) {
                    response_value = SWD_ACK_ERROR;
                    break;
                }
                wr32(out, data);
                out += 4;
            }
            if (rq & XFER_MATCH_VALUE) {
                if (end - p < 4) {
                    break;
                }
                uint32_t match = rd32(p);
                p += 4;
                uint32_t match_retry = s_match_retry;
                if (rq & XFER_APnDP) {
                    response_value = xfer(rq, NULL);
                    if (response_value != SWD_ACK_OK) {
                        break;
                    }
                }
                do {
                    response_value = xfer(rq, &data);
                } while (response_value == SWD_ACK_OK && (data & s_match_mask) != match && match_retry--);
                if (response_value != SWD_ACK_OK) {
                    break;
                }
                if ((data & s_match_mask) != match) {
                    response_value |= XFER_MISMATCH;
                    break;
                }
            } else if (rq & XFER_APnDP) {
                if (!post_read) {
                    response_value = xfer(rq, NULL);
                    if (response_value != SWD_ACK_OK) {
                        break;
                    }
                    post_read = true;
                }
            } else {
                response_value = xfer(rq, &data);
                if (response_value != SWD_ACK_OK) {
                    break;
                }
                if (out + 4 > out_end) {
                    response_value = SWD_ACK_ERROR;
                    break;
                }
                wr32(out, data);
                out += 4;
            }
            check_write = false;
        } else {
            if (post_read) {
                response_value = xfer(DP_RDBUFF | XFER_RnW, &data);
                if (response_value != SWD_ACK_OK) {
                    break;
                }
                if (out + 4 > out_end) {
                    response_value = SWD_ACK_ERROR;
                    break;
                }
                wr32(out, data);
                out += 4;
                post_read = false;
            }
            if (end - p < 4) {
                break;
            }
            data = rd32(p);
            p += 4;
            if (rq & XFER_MATCH_MASK) {
                s_match_mask = data;
                response_value = SWD_ACK_OK;
            } else {
                response_value = xfer(rq, &data);
                if (response_value != SWD_ACK_OK) {
                    break;
                }
                check_write = true;
            }
        }
        response_count++;
    }

    if (response_value == SWD_ACK_OK) {
        if (post_read) {
            response_value = xfer(DP_RDBUFF | XFER_RnW, &data);
            if (response_value == SWD_ACK_OK && out + 4 <= out_end) {
                wr32(out, data);
                out += 4;
            }
        } else if (check_write) {
            response_value = xfer(DP_RDBUFF | XFER_RnW, NULL);
        }
    }

    resp[1] = (uint8_t)response_count;
    resp[2] = response_value;
    send(resp, (uint32_t)(out - resp));
}

static void handle_transfer_block(const uint8_t *req, uint16_t len) {
    uint8_t resp[DAP_PACKET_SIZE] = {ID_DAP_TRANSFER_BLOCK, 0, 0, 0};
    uint8_t *out = &resp[4];
    const uint8_t *out_end = resp + sizeof(resp);
    const uint8_t *p = req + 1;
    const uint8_t *end = req + len;
    uint32_t response_count = 0;
    uint8_t response_value = 0;
    uint32_t data;

    if (end - p < 4 || !swd_ready()) {
        send(resp, 4);
        return;
    }
    p++; // DAP index
    uint32_t count = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    p += 2;
    uint8_t rq = *p++;

    if (count == 0) {
        goto done;
    }
    if (rq & XFER_RnW) {
        if (rq & XFER_APnDP) {
            response_value = xfer(rq, NULL); // post the first AP read
            if (response_value != SWD_ACK_OK) {
                goto done;
            }
        }
        while (count--) {
            uint32_t r = (count == 0 && (rq & XFER_APnDP)) ? (DP_RDBUFF | XFER_RnW) : rq;
            response_value = xfer(r, &data);
            if (response_value != SWD_ACK_OK) {
                goto done;
            }
            if (out + 4 > out_end) {
                response_value = SWD_ACK_ERROR;
                goto done;
            }
            wr32(out, data);
            out += 4;
            response_count++;
        }
    } else {
        while (count--) {
            if (end - p < 4) {
                goto done;
            }
            data = rd32(p);
            p += 4;
            response_value = xfer(rq, &data);
            if (response_value != SWD_ACK_OK) {
                goto done;
            }
            response_count++;
        }
        response_value = xfer(DP_RDBUFF | XFER_RnW, NULL); // confirm last write
    }
done:
    resp[1] = (uint8_t)response_count;
    resp[2] = (uint8_t)(response_count >> 8);
    resp[3] = response_value;
    send(resp, (uint32_t)(out - resp));
}


// -- JTAG-DP transfers (ARM CMSIS-DAP reference DAP_JTAG_Transfer) ----------
// Every DPACC/APACC scan returns the previous scan's result, so reads are
// posted and the last one is collected from DP RDBUFF, like SWD's AP reads
// -- but here DP reads are posted too, and the IR (DPACC/APACC) is only
// reloaded when the target register bank changes.

static uint8_t jxfer(uint32_t request, uint32_t *data) {
    uint32_t retry = s_wait_retry;
    uint8_t ack;
    do {
        ack = jtag_transfer(request, data);
        usb_transport_core1_alive();
    } while (ack == SWD_ACK_WAIT && retry--);
    return ack;
}

static uint32_t s_jtag_ir; // IR currently loaded in the selected TAP (0 = unknown)

static void jtag_ir_select(uint32_t ir) {
    if (s_jtag_ir != ir) {
        s_jtag_ir = ir;
        jtag_ir(ir);
    }
}

static void handle_transfer_jtag(const uint8_t *req, uint16_t len) {
    uint8_t resp[DAP_PACKET_SIZE] = {ID_DAP_TRANSFER, 0, 0};
    uint8_t *out = &resp[3];
    const uint8_t *out_end = resp + sizeof(resp);
    const uint8_t *p = req + 1;
    const uint8_t *end = req + len;
    uint32_t response_count = 0;
    uint8_t response_value = 0;
    bool post_read = false;
    uint32_t data = 0;

    if (end - p < 2 || !jtag_select_device(p[0])) {
        send(resp, 3);
        return;
    }
    s_jtag_ir = 0; // don't trust a previous packet's IR (other device/tool may have moved it)
    p++;
    uint32_t request_count = *p++;

    for (; request_count; request_count--) {
        if (p >= end) {
            break;
        }
        uint8_t rq = *p++;
        uint32_t rq_ir = (rq & XFER_APnDP) ? JTAG_IR_APACC : JTAG_IR_DPACC;
        if (rq & XFER_RnW) {
            if (post_read) {
                if (s_jtag_ir == rq_ir && !(rq & XFER_MATCH_VALUE)) {
                    response_value = jxfer(rq, &data); // previous data, post next
                } else {
                    jtag_ir_select(JTAG_IR_DPACC);
                    response_value = jxfer(DP_RDBUFF | XFER_RnW, &data);
                    post_read = false;
                }
                if (response_value != SWD_ACK_OK) {
                    break;
                }
                if (out + 4 > out_end) {
                    response_value = SWD_ACK_ERROR;
                    break;
                }
                wr32(out, data);
                out += 4;
            }
            if (rq & XFER_MATCH_VALUE) {
                if (end - p < 4) {
                    break;
                }
                uint32_t match = rd32(p);
                p += 4;
                uint32_t match_retry = s_match_retry;
                jtag_ir_select(rq_ir);
                response_value = jxfer(rq, NULL); // post
                if (response_value != SWD_ACK_OK) {
                    break;
                }
                do {
                    response_value = jxfer(rq, &data);
                } while (response_value == SWD_ACK_OK && (data & s_match_mask) != match && match_retry--);
                if (response_value != SWD_ACK_OK) {
                    break;
                }
                if ((data & s_match_mask) != match) {
                    response_value |= XFER_MISMATCH;
                    break;
                }
            } else if (!post_read) {
                jtag_ir_select(rq_ir);
                response_value = jxfer(rq, NULL); // post
                if (response_value != SWD_ACK_OK) {
                    break;
                }
                post_read = true;
            }
        } else {
            if (post_read) {
                jtag_ir_select(JTAG_IR_DPACC);
                response_value = jxfer(DP_RDBUFF | XFER_RnW, &data);
                if (response_value != SWD_ACK_OK) {
                    break;
                }
                if (out + 4 > out_end) {
                    response_value = SWD_ACK_ERROR;
                    break;
                }
                wr32(out, data);
                out += 4;
                post_read = false;
            }
            if (end - p < 4) {
                break;
            }
            data = rd32(p);
            p += 4;
            if (rq & XFER_MATCH_MASK) {
                s_match_mask = data;
                response_value = SWD_ACK_OK;
            } else {
                jtag_ir_select(rq_ir);
                response_value = jxfer(rq, &data);
                if (response_value != SWD_ACK_OK) {
                    break;
                }
            }
        }
        response_count++;
    }

    if (response_value == SWD_ACK_OK) {
        jtag_ir_select(JTAG_IR_DPACC);
        if (post_read) {
            response_value = jxfer(DP_RDBUFF | XFER_RnW, &data);
            if (response_value == SWD_ACK_OK && out + 4 <= out_end) {
                wr32(out, data);
                out += 4;
            }
        } else {
            response_value = jxfer(DP_RDBUFF | XFER_RnW, NULL); // check last write
        }
    }

    resp[1] = (uint8_t)response_count;
    resp[2] = response_value;
    send(resp, (uint32_t)(out - resp));
}

static void handle_transfer_block_jtag(const uint8_t *req, uint16_t len) {
    uint8_t resp[DAP_PACKET_SIZE] = {ID_DAP_TRANSFER_BLOCK, 0, 0, 0};
    uint8_t *out = &resp[4];
    const uint8_t *out_end = resp + sizeof(resp);
    const uint8_t *p = req + 1;
    const uint8_t *end = req + len;
    uint32_t response_count = 0;
    uint8_t response_value = 0;
    uint32_t data;

    if (end - p < 4 || !jtag_select_device(p[0])) {
        send(resp, 4);
        return;
    }
    s_jtag_ir = 0;
    p++;
    uint32_t count = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    p += 2;
    uint8_t rq = *p++;
    if (count == 0) {
        goto done;
    }
    jtag_ir_select((rq & XFER_APnDP) ? JTAG_IR_APACC : JTAG_IR_DPACC);
    if (rq & XFER_RnW) {
        response_value = jxfer(rq, NULL); // post
        if (response_value != SWD_ACK_OK) {
            goto done;
        }
        while (count--) {
            uint32_t r = rq;
            if (count == 0) {
                jtag_ir_select(JTAG_IR_DPACC);
                r = DP_RDBUFF | XFER_RnW; // collect the last posted read
            }
            response_value = jxfer(r, &data);
            if (response_value != SWD_ACK_OK) {
                goto done;
            }
            if (out + 4 > out_end) {
                response_value = SWD_ACK_ERROR;
                goto done;
            }
            wr32(out, data);
            out += 4;
            response_count++;
        }
    } else {
        while (count--) {
            if (end - p < 4) {
                goto done;
            }
            data = rd32(p);
            p += 4;
            response_value = jxfer(rq, &data);
            if (response_value != SWD_ACK_OK) {
                goto done;
            }
            response_count++;
        }
        jtag_ir_select(JTAG_IR_DPACC);
        response_value = jxfer(DP_RDBUFF | XFER_RnW, NULL); // check last write
    }
done:
    resp[1] = (uint8_t)response_count;
    resp[2] = (uint8_t)(response_count >> 8);
    resp[3] = response_value;
    send(resp, (uint32_t)(out - resp));
}

static void handle_swd_sequence(const uint8_t *req, uint16_t len) {
    uint8_t resp[DAP_PACKET_SIZE] = {ID_DAP_SWD_SEQUENCE, DAP_OK};
    uint8_t *out = &resp[2];
    const uint8_t *p = req + 1;
    const uint8_t *end = req + len;
    if (!swd_ready() || p >= end) {
        resp[1] = DAP_ERROR;
        send(resp, 2);
        return;
    }
    uint32_t seqs = *p++;
    while (seqs--) {
        if (p >= end) {
            resp[1] = DAP_ERROR;
            break;
        }
        uint8_t info = *p++;
        uint32_t bits = info & 0x3Fu;
        if (bits == 0) {
            bits = 64;
        }
        uint32_t bytes = (bits + 7) / 8;
        if (info & 0x80u) {
            if (out + bytes > resp + sizeof(resp)) {
                resp[1] = DAP_ERROR;
                break;
            }
            swd_seq_in(out, bits);
            out += bytes;
        } else {
            if ((uint32_t)(end - p) < bytes) {
                resp[1] = DAP_ERROR;
                break;
            }
            swd_seq_out(p, bits);
            p += bytes;
        }
    }
    send(resp, (uint32_t)(out - resp));
}

static uint8_t pins_read(void) {
    return jtag_ready() ? jtag_pins_read() : swd_pins_read();
}

static void handle_swj_pins(const uint8_t *req, uint16_t len) {
    uint8_t resp[2] = {ID_DAP_SWJ_PINS, 0};
    if (len >= 7 && s_port != PORT_OFF) {
        uint8_t value = req[1];
        uint8_t select = req[2];
        uint32_t wait_us = rd32(&req[3]);
        if (jtag_ready()) {
            jtag_pins_write(value, select);
        } else {
            swd_pins_write(value, select);
        }
        if (wait_us) {
            if (wait_us > SWJ_PINS_MAX_WAIT_US) {
                wait_us = SWJ_PINS_MAX_WAIT_US;
            }
            absolute_time_t deadline = make_timeout_time_us(wait_us);
            while (((pins_read() ^ value) & select) && !time_reached(deadline)) {
                usb_transport_core1_alive();
            }
        }
        resp[1] = pins_read();
    }
    send(resp, 2);
}

// DAP_JTAG_Sequence: [count, {info, tdi bytes}...] -> [OK, captured tdo...].
static void handle_jtag_sequence(const uint8_t *req, uint16_t len) {
    uint8_t resp[DAP_PACKET_SIZE] = {ID_DAP_JTAG_SEQUENCE, DAP_OK};
    uint8_t *out = &resp[2];
    const uint8_t *p = req + 1;
    const uint8_t *end = req + len;
    if (!jtag_ready() || p >= end) {
        resp[1] = DAP_ERROR;
        send(resp, 2);
        return;
    }
    uint32_t seqs = *p++;
    while (seqs--) {
        if (p >= end) {
            resp[1] = DAP_ERROR;
            break;
        }
        uint8_t info = *p++;
        uint32_t bits = info & 0x3Fu;
        uint32_t bytes = ((bits ? bits : 64u) + 7) / 8;
        if ((uint32_t)(end - p) < bytes ||
            ((info & 0x80u) && out + bytes > resp + sizeof(resp))) {
            resp[1] = DAP_ERROR;
            break;
        }
        jtag_sequence(info, p, out);
        p += bytes;
        if (info & 0x80u) {
            out += bytes;
        }
        usb_transport_core1_alive();
    }
    send(resp, (uint32_t)(out - resp));
}

static void process(const dap_packet_t *p) {
    if (p->len == 0) {
        return;
    }
    switch (p->data[0]) {
    case ID_DAP_INFO:
        handle_info(p->data, p->len);
        break;
    case ID_DAP_HOST_STATUS: {
        uint8_t resp[2] = {ID_DAP_HOST_STATUS, DAP_OK};
        send(resp, 2);
        break;
    }
    case ID_DAP_CONNECT:
        handle_connect(p->data, p->len);
        break;
    case ID_DAP_DISCONNECT: {
        end_session(DEBUG_END_DISCONNECT);
        uint8_t resp[2] = {ID_DAP_DISCONNECT, DAP_OK};
        send(resp, 2);
        break;
    }
    case ID_DAP_TRANSFER_CONFIGURE: {
        uint8_t resp[2] = {ID_DAP_TRANSFER_CONFIGURE, DAP_ERROR};
        if (p->len >= 6) {
            swd_set_idle_cycles(p->data[1]);
            jtag_set_idle_cycles(p->data[1]);
            s_wait_retry = (uint16_t)(p->data[2] | (p->data[3] << 8));
            s_match_retry = (uint16_t)(p->data[4] | (p->data[5] << 8));
            resp[1] = DAP_OK;
        }
        send(resp, 2);
        break;
    }
    case ID_DAP_TRANSFER:
        if (jtag_ready()) {
            handle_transfer_jtag(p->data, p->len);
        } else {
            handle_transfer(p->data, p->len);
        }
        break;
    case ID_DAP_TRANSFER_BLOCK:
        if (jtag_ready()) {
            handle_transfer_block_jtag(p->data, p->len);
        } else {
            handle_transfer_block(p->data, p->len);
        }
        break;
    case ID_DAP_JTAG_SEQUENCE:
        handle_jtag_sequence(p->data, p->len);
        break;
    case ID_DAP_JTAG_CONFIGURE: {
        // Device count + each TAP's IR length, for JTAG_IDCODE/JTAG-DP transfers.
        uint8_t resp[2] = {ID_DAP_JTAG_CONFIGURE, DAP_ERROR};
        if (p->len >= 2 && p->len >= 2u + p->data[1] && jtag_configure(p->data[1], &p->data[2])) {
            resp[1] = DAP_OK;
        }
        send(resp, 2);
        break;
    }
    case ID_DAP_JTAG_IDCODE: {
        uint8_t resp[6] = {ID_DAP_JTAG_IDCODE, DAP_ERROR, 0, 0, 0, 0};
        if (jtag_ready() && p->len >= 2 && jtag_select_device(p->data[1])) {
            jtag_ir(JTAG_IR_IDCODE);
            s_jtag_ir = JTAG_IR_IDCODE;
            wr32(&resp[2], jtag_read_idcode());
            resp[1] = DAP_OK;
        }
        send(resp, 6);
        break;
    }
    case ID_DAP_TRANSFER_ABORT:
        break; // transfers run to completion within one packet; no reply by spec
    case ID_DAP_WRITE_ABORT: {
        uint8_t resp[2] = {ID_DAP_WRITE_ABORT, DAP_ERROR};
        if (p->len >= 6 && swd_ready()) {
            uint32_t data = rd32(&p->data[2]);
            if (swd_transfer(0x0u, &data) == SWD_ACK_OK) { // DP ABORT write
                resp[1] = DAP_OK;
            }
        } else if (p->len >= 6 && jtag_ready() && jtag_select_device(p->data[1])) {
            jtag_ir(JTAG_IR_ABORT);
            s_jtag_ir = JTAG_IR_ABORT;
            jtag_write_abort(rd32(&p->data[2]));
            resp[1] = DAP_OK;
        }
        send(resp, 2);
        break;
    }
    case ID_DAP_DELAY: {
        if (p->len >= 3) {
            busy_wait_us_32((uint32_t)(p->data[1] | (p->data[2] << 8)));
        }
        uint8_t resp[2] = {ID_DAP_DELAY, DAP_OK};
        send(resp, 2);
        break;
    }
    case ID_DAP_RESET_TARGET: {
        uint8_t resp[3] = {ID_DAP_RESET_TARGET, DAP_OK, 0}; // no device-specific sequence
        send(resp, 3);
        break;
    }
    case ID_DAP_SWJ_PINS:
        handle_swj_pins(p->data, p->len);
        break;
    case ID_DAP_SWJ_CLOCK: {
        uint8_t resp[2] = {ID_DAP_SWJ_CLOCK, DAP_ERROR};
        if (p->len >= 5 && rd32(&p->data[1]) != 0) {
            swd_set_clock(rd32(&p->data[1]));
            jtag_set_clock(rd32(&p->data[1]));
            resp[1] = DAP_OK;
        }
        send(resp, 2);
        break;
    }
    case ID_DAP_SWJ_SEQUENCE: {
        uint8_t resp[2] = {ID_DAP_SWJ_SEQUENCE, DAP_ERROR};
        if (p->len >= 2 && s_port != PORT_OFF) {
            uint32_t bits = p->data[1] ? p->data[1] : 256u;
            if ((uint32_t)(p->len - 2) >= (bits + 7) / 8) {
                if (jtag_ready()) {
                    jtag_tms_sequence(&p->data[2], bits); // TMS is GP11 in JTAG mode
                } else {
                    swd_seq_out(&p->data[2], bits);
                }
                resp[1] = DAP_OK;
            }
        }
        send(resp, 2);
        break;
    }
    case ID_DAP_SWD_CONFIGURE: {
        uint8_t resp[2] = {ID_DAP_SWD_CONFIGURE, DAP_ERROR};
        if (p->len >= 2) {
            swd_configure((uint8_t)((p->data[1] & 0x3u) + 1u), (p->data[1] & 0x4u) != 0);
            resp[1] = DAP_OK;
        }
        send(resp, 2);
        break;
    }
    case ID_DAP_SWD_SEQUENCE:
        handle_swd_sequence(p->data, p->len);
        break;
    // SWO, UART (NRZ) mode, collected with DAP_SWO_Data (no streaming EP).
    case ID_DAP_SWO_TRANSPORT: {
        uint8_t resp[2] = {ID_DAP_SWO_TRANSPORT, DAP_ERROR};
        if (p->len >= 2 && p->data[1] <= 1 && !swo_active()) {
            s_swo_transport = p->data[1];
            resp[1] = DAP_OK;
        }
        send(resp, 2);
        break;
    }
    case ID_DAP_SWO_MODE: {
        uint8_t resp[2] = {ID_DAP_SWO_MODE, DAP_ERROR};
        if (p->len >= 2 && p->data[1] <= 1 && !swo_active()) { // Manchester (2) not supported
            s_swo_mode = p->data[1];
            resp[1] = DAP_OK;
        }
        send(resp, 2);
        break;
    }
    case ID_DAP_SWO_BAUDRATE: {
        uint8_t resp[5] = {ID_DAP_SWO_BAUDRATE, 0, 0, 0, 0};
        if (p->len >= 5) {
            wr32(&resp[1], swo_set_baudrate(rd32(&p->data[1])));
        }
        send(resp, 5);
        break;
    }
    case ID_DAP_SWO_CONTROL: {
        uint8_t resp[2] = {ID_DAP_SWO_CONTROL, DAP_ERROR};
        if (p->len >= 2) {
            if (p->data[1] == 0) {
                swo_stop();
                resp[1] = DAP_OK;
            } else if (p->data[1] == 1 && s_swo_mode == 1 && s_swo_transport == 1 &&
                       swd_ready() && swo_start()) {
                resp[1] = DAP_OK;
            }
        }
        send(resp, 2);
        break;
    }
    case ID_DAP_SWO_STATUS: {
        uint8_t resp[6] = {ID_DAP_SWO_STATUS, 0};
        uint32_t avail = swo_available();
        resp[1] = (uint8_t)((swo_active() ? 0x01u : 0) | (swo_overrun() ? 0x80u : 0));
        wr32(&resp[2], avail);
        send(resp, 6);
        break;
    }
    case ID_DAP_SWO_DATA: {
        uint8_t resp[DAP_PACKET_SIZE] = {ID_DAP_SWO_DATA, 0, 0, 0};
        uint32_t max = p->len >= 3 ? (uint32_t)(p->data[1] | (p->data[2] << 8)) : 0;
        if (max > DAP_PACKET_SIZE - 4) {
            max = DAP_PACKET_SIZE - 4;
        }
        uint32_t n = swo_read(&resp[4], max);
        resp[1] = (uint8_t)((swo_active() ? 0x01u : 0) | (swo_overrun() ? 0x80u : 0));
        resp[2] = (uint8_t)n;
        resp[3] = (uint8_t)(n >> 8);
        send(resp, 4 + n);
        break;
    }
    default: {
        uint8_t resp[1] = {ID_DAP_INVALID};
        send(resp, 1);
        break;
    }
    }
}

void dap_task(void) {
    uint64_t now = time_us_64();

    if (!tud_mounted()) {
        end_session(DEBUG_END_USB);
        s_connect_pending = false;
        dap_usb_flush();
        return;
    }

    if (s_connect_pending) {
        if (!s_connect_requested) {
            s_connect_requested = debug_session_request_start(
                s_connect_port == DAP_PORT_JTAG ? DEBUG_MODE_JTAG : DEBUG_MODE_SWD);
        } else if (debug_session_granted() && dap_usb_tx_ready()) {
            if (s_connect_port == DAP_PORT_SWD) {
                swd_port_on();
                s_port = PORT_SWD;
            } else {
                jtag_port_on();
                s_port = PORT_JTAG;
            }
            uint8_t resp[2] = {ID_DAP_CONNECT, s_connect_port};
            send(resp, 2);
            s_connect_pending = false;
            s_last_activity_us = now;
        }
        return; // commands after DAP_Connect wait in dap_usb's queue
    }

    if (debug_session_granted() && now - s_last_activity_us > DAP_INACTIVITY_TIMEOUT_US) {
        end_session(DEBUG_END_TIMEOUT);
    }

    // Next command only once the previous response is on its way, so each
    // response is its own USB transfer.
    static dap_packet_t pkt;
    if (dap_usb_tx_ready() && dap_usb_receive(pkt.data, &pkt.len)) {
        s_last_activity_us = now;
        process(&pkt);
    }
}

uint32_t dap_swd_max_hz(void) {
    return s_swd_max_hz;
}

uint32_t dap_jtag_max_hz(void) {
    return s_jtag_max_hz;
}
