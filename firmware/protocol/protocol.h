#pragma once
#include <stdint.h>

// Wire framing per rp2350_digital_signal_agent_plan.md "Binary framing".
// Little-endian throughout; header is fixed 20 bytes, followed by
// payload_len bytes of payload, followed by a 4-byte CRC32C trailer
// covering header+payload.

#define PROTOCOL_MAGIC       0x5349474Cu
#define PROTOCOL_VERSION     1
#define PROTOCOL_HEADER_SIZE 20
#define PROTOCOL_MAX_PAYLOAD 16384u

// Step-3 bring-up ceiling: the receive/transmit buffers only hold this much
// until the hardened streaming parser (backpressure/credits, step 4) lands.
// Frames within PROTOCOL_MAX_PAYLOAD but over this cap get a
// STATUS_PAYLOAD_TOO_LARGE error rather than being buffered.
#define PROTOCOL_RX_PAYLOAD_CAP 512u

typedef enum {
    FRAME_TYPE_REQUEST  = 1,
    FRAME_TYPE_RESPONSE = 2,
    FRAME_TYPE_EVENT     = 3,
    FRAME_TYPE_ERROR     = 4,
} frame_type_t;

typedef enum {
    FRAME_FLAG_ACK_REQUIRED = 1u << 0,
    FRAME_FLAG_MORE         = 1u << 1,
    FRAME_FLAG_IDEMPOTENT   = 1u << 2,
    FRAME_FLAG_ARMED        = 1u << 3,
} frame_flags_t;

typedef enum {
    STATUS_OK                 = 0,
    STATUS_UNSUPPORTED_OPCODE = 1,
    STATUS_BAD_VERSION        = 2,
    STATUS_BAD_LENGTH         = 3,
    STATUS_BAD_CRC            = 4,
    STATUS_MALFORMED          = 5,
    STATUS_PAYLOAD_TOO_LARGE  = 6,
    STATUS_INTERNAL_ERROR     = 7,
} frame_status_t;

// Full opcode set from the plan. See protocol.md's opcode table for exactly
// which are implemented; unimplemented ones dispatch to
// STATUS_UNSUPPORTED_OPCODE until their owning module exists.
typedef enum {
    OPCODE_GET_INFO               = 1,
    OPCODE_GET_CAPABILITIES       = 2,
    OPCODE_GET_STATUS             = 3,
    OPCODE_SET_SAFE_STATE         = 4,
    OPCODE_DIGITAL_CHANNEL_CONFIG = 5,
    OPCODE_ARM_OUTPUTS            = 6,
    OPCODE_DISARM_OUTPUTS         = 7,
    OPCODE_DIGITAL_READ           = 8,
    OPCODE_DIGITAL_WRITE_MASKED   = 9,
    OPCODE_DIGITAL_CAPTURE_START  = 10,
    OPCODE_DIGITAL_CAPTURE_STOP   = 11,
    OPCODE_DIGITAL_CAPTURE_READ   = 12,
    OPCODE_PWM_CONFIG             = 13,
    OPCODE_ADC_SCAN_CONFIG        = 14,
    OPCODE_ADC_SCAN_READ          = 15,
    OPCODE_I2C_XFER                = 16,
    OPCODE_I2C_SLAVE_CONFIG        = 17,
    OPCODE_UART_CONFIG             = 18,
    OPCODE_UART_WRITE              = 19,
    OPCODE_UART_READ                = 20,
    OPCODE_SPI_XFER                  = 21,
    OPCODE_SPI_SLAVE_CONFIG           = 22,
    OPCODE_PING                       = 23,
    OPCODE_RESET_TO_BOOTSEL           = 24,
} opcode_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t flags;
    uint32_t sequence;
    uint16_t opcode;
    uint16_t status;
    uint32_t payload_len;
} frame_header_t;

_Static_assert(sizeof(frame_header_t) == PROTOCOL_HEADER_SIZE,
               "frame_header_t must match the frozen wire layout");

// GET_INFO response payload.
typedef struct __attribute__((packed)) {
    uint8_t  protocol_version;
    uint8_t  fw_version_major;
    uint8_t  fw_version_minor;
    uint8_t  fw_version_patch;
    uint8_t  board_unique_id[8];
    char     git_hash[16]; // NUL-padded, truncated if longer
} info_response_t;

// GET_CAPABILITIES response payload — static channel counts from the
// frozen Pico 2 pin plan.
typedef struct __attribute__((packed)) {
    uint8_t digital_channel_count;
    uint8_t spi_master_count;
    uint8_t spi_slave_count;
    uint8_t i2c_master_count;
    uint8_t i2c_slave_count;
    uint8_t uart_count;
    uint8_t adc_channel_count;
    uint8_t reserved;
} capabilities_response_t;

// GET_STATUS response payload.
typedef struct __attribute__((packed)) {
    uint64_t uptime_us;
    uint8_t  reset_cause;       // reset_cause_t
    uint8_t  safe_state_reason; // safe_state_reason_t, last recorded
    uint8_t  outputs_armed;     // 0/1
    uint8_t  armed_mask;        // digital bank bits currently armed as output
    // Parser counters since boot — resync/error activity should stay at
    // zero on a healthy link; a climbing count points at a transport or
    // host-side framing bug.
    uint32_t frames_ok;
    uint32_t magic_resyncs;
    uint32_t err_bad_version;
    uint32_t err_bad_length;
    uint32_t err_bad_crc;
    uint32_t err_oversize;
    uint32_t err_stalled_frame;  // partial frame abandoned by an idle sender
    // Fresh each time this is read (rotates on every successful ARM_OUTPUTS,
    // and every time this response is sent). ARM_OUTPUTS must echo the most
    // recently issued value back exactly, or it's rejected as stale/replayed.
    uint8_t  arm_challenge[16];
} status_response_t;

// ARM_OUTPUTS request payload.
typedef struct __attribute__((packed)) {
    uint8_t  challenge[16]; // must match the last value GET_STATUS returned
    uint8_t  output_mask;   // digital bank bits (GP0-7) to claim as output
    uint8_t  reserved[3];
    uint32_t lease_ms;      // 0 = server default; clamped to [100, 30000]
} arm_outputs_request_t;

// ARM_OUTPUTS response payload.
typedef struct __attribute__((packed)) {
    uint32_t granted_lease_ms;
} arm_outputs_response_t;

// DIGITAL_WRITE_MASKED request payload. mask must be a subset of the
// currently armed output mask.
typedef struct __attribute__((packed)) {
    uint8_t mask;
    uint8_t values;
} digital_write_masked_request_t;

// DIGITAL_READ request payload.
typedef struct __attribute__((packed)) {
    uint8_t mask;
} digital_read_request_t;

// DIGITAL_READ response payload.
typedef struct __attribute__((packed)) {
    uint8_t values;
} digital_read_response_t;

// DIGITAL_CAPTURE_START request payload.
typedef struct __attribute__((packed)) {
    uint32_t rate_hz;      // clamped to what the PIO clkdiv can hit at the
                            // current system clock, see digital_bank.c
    uint32_t max_samples;  // 0 = unbounded (runs until STOP); else a bounded
                            // burst, clamped to the ring capacity so it can
                            // never wrap mid-capture
} digital_capture_start_request_t;

// DIGITAL_CAPTURE_START response payload.
typedef struct __attribute__((packed)) {
    uint32_t capture_id;             // nonzero; must be echoed by STOP/READ
    uint32_t granted_rate_hz;        // actual rate after clkdiv quantization
    uint32_t buffer_capacity_samples;
} digital_capture_start_response_t;

// DIGITAL_CAPTURE_STOP request: empty payload, stops whatever capture is
// active (there is only ever one at a time).
//
// DIGITAL_CAPTURE_STOP response payload — also reused, verbatim, as the
// payload of the unsolicited EVENT sent when a bounded capture completes on
// its own (type=EVENT, opcode=OPCODE_DIGITAL_CAPTURE_STOP, sequence=0,
// status=STATUS_OK — see protocol.md's "Events" section for the sequence=0
// convention for frames with no originating request).
typedef struct __attribute__((packed)) {
    uint32_t capture_id;
    uint32_t total_captured; // total samples physically written since START,
                              // NOT clamped to buffer capacity — compare
                              // against buffer_capacity_samples to know how
                              // much is actually still readable
} digital_capture_stop_response_t;

// DIGITAL_CAPTURE_READ request payload. offset/limit are in absolute sample
// units since START (same units as total_captured) — not relative to the
// ring buffer, the wraparound is handled internally.
typedef struct __attribute__((packed)) {
    uint32_t capture_id;
    uint32_t offset;
    uint16_t limit;
    uint16_t reserved;
} digital_capture_read_request_t;

// DIGITAL_CAPTURE_READ response payload, followed by returned_count raw
// sample bytes (each byte is one 8-bit digital-bank word).
typedef struct __attribute__((packed)) {
    uint32_t capture_id;
    uint32_t total_captured;
    uint32_t overrun_count;     // cumulative samples lost to ring overwrite
                                 // before they could be read, since START
    uint64_t start_timestamp_us;
    uint32_t rate_hz;
    uint16_t returned_count;    // may be less than requested: clamped to
                                 // what's actually been written, and/or to
                                 // what's still in the ring (see overrun_count)
    uint16_t reserved;
} digital_capture_read_response_t;

// PWM_CONFIG request payload. `channel` must already be in the digital
// bank's currently-armed output mask (see ARM_OUTPUTS) — PWM deliberately
// reuses that same arm/lease/challenge gate rather than having its own.
// GP0/1, GP2/3, GP4/5, GP6/7 each share one PWM slice's frequency (the
// hardware wrap/clkdiv is per-slice, not per-pin) — configuring a new
// frequency on one half of a pair silently changes the other half's actual
// driven frequency too, even if it isn't touched by this call. Group
// paired channels onto the same frequency, or accept the coupling.
typedef struct __attribute__((packed)) {
    uint8_t  channel;         // 0-7 (GP0-7)
    uint8_t  enabled;         // 0/1
    uint16_t reserved;
    uint32_t frequency_hz;    // clamped to what the PWM clock divider can
                                // hit at the current system clock, up to
                                // PWM_MAX_FREQUENCY_HZ; ignored if enabled=0
    uint16_t duty_permille;   // 0-1000 (0.1% steps); clamped to [0,1000].
                                // 0 and 1000 are glitch-free (hardware
                                // boundary behavior, no special-casing needed)
    uint16_t reserved2;
} pwm_config_request_t;

// PWM_CONFIG response payload.
typedef struct __attribute__((packed)) {
    uint32_t granted_frequency_hz; // 0 if enabled=0 was requested
    uint16_t granted_duty_permille;
    uint16_t reserved;
} pwm_config_response_t;

// ADC_SCAN_CONFIG request payload. Sending channel_mask=0 stops whatever
// scan is active — there is no separate ADC_SCAN_STOP opcode (the plan's
// opcode table only lists CONFIG/READ for ADC).
typedef struct __attribute__((packed)) {
    uint8_t  channel_mask;  // bits 0-2 = ADC0-2 (GP26-28); 0 = stop
    uint8_t  reserved[3];
    uint32_t rate_hz;       // TOTAL round-robin conversion rate, not
                             // per-channel — with N channels selected, each
                             // individual channel is sampled at rate_hz/N.
                             // Clamped to what the ADC clock divider can hit;
                             // see peripherals/adc_scan.c
    uint32_t max_samples;   // 0 = unbounded (until re-CONFIGed with mask=0);
                             // else bounded, clamped to ring capacity
} adc_scan_config_request_t;

// ADC_SCAN_CONFIG response payload.
typedef struct __attribute__((packed)) {
    uint32_t scan_id;              // nonzero; must be echoed by READ
    uint32_t granted_rate_hz;      // actual total rate after quantization
    uint8_t  channel_count;        // popcount(channel_mask); 0 if stopped
    uint8_t  reserved[3];
    uint32_t buffer_capacity_samples;
    // Nominal 3V3 rail voltage, NOT independently calibrated per board —
    // there is no separate precision reference on this board, ADC_VREF is
    // tied straight to the 3V3 rail. volts = raw_code * vref_millivolts /
    // 4096 / 1000. See protocol.md for the honest caveat on accuracy.
    uint16_t vref_millivolts;
    uint16_t reserved2;
} adc_scan_config_response_t;

// Reused, verbatim, as the payload of the unsolicited "ADC block-ready"
// EVENT sent when a bounded scan completes on its own (type=EVENT,
// opcode=OPCODE_ADC_SCAN_READ, sequence=0, status=STATUS_OK) — same
// sequence=0 convention as the digital-capture-complete EVENT.
typedef struct __attribute__((packed)) {
    uint32_t scan_id;
    uint32_t total_captured;
} adc_scan_complete_event_t;

// ADC_SCAN_READ request payload. offset/limit are in absolute raw-sample
// units since START (interleaved across channels in round-robin order),
// same convention as DIGITAL_CAPTURE_READ.
typedef struct __attribute__((packed)) {
    uint32_t scan_id;
    uint32_t offset;
    uint16_t limit;
    uint16_t reserved;
} adc_scan_read_request_t;

// ADC_SCAN_READ response payload, followed by returned_count raw 12-bit ADC
// codes (u16 each, values 0-4095). Sample i (0-based within this response,
// added to the request's offset) belongs to the (i % channel_count)-th set
// bit of channel_mask in ascending channel-number order — that's the fixed
// round-robin hardware cycle order, not re-stated per sample.
typedef struct __attribute__((packed)) {
    uint32_t scan_id;
    uint32_t total_captured;
    uint32_t overrun_count;
    uint64_t start_timestamp_us;
    uint32_t rate_hz;
    uint8_t  channel_count;
    uint8_t  reserved;
    uint16_t returned_count;
} adc_scan_read_response_t;

// UART_CONFIG request payload. Not gated by ARM_OUTPUTS's arm/lease — this
// is a communication bus, not a static driven output level (matches the
// plan's framing of I2C/UART/SPI as buses distinct from the GPIO/PWM
// output safety concern). (Re)configuring resets the RX ring, read cursor,
// and error counters, and aborts any in-flight TX.
typedef struct __attribute__((packed)) {
    uint32_t baud_rate;
    uint8_t  data_bits; // 5-8
    uint8_t  stop_bits; // 1-2
    uint8_t  parity;    // 0=none, 1=even, 2=odd
    uint8_t  reserved;
} uart_config_request_t;

// UART_CONFIG response payload.
typedef struct __attribute__((packed)) {
    uint32_t granted_baud_rate; // actual achieved rate, hardware divider quantized
} uart_config_response_t;

// UART_WRITE request payload: the raw bytes to transmit, the entire
// request payload (no wrapper struct) — a one-shot fire-and-forget DMA
// transfer, not queued against previous writes. If a previous UART_WRITE's
// DMA is still draining, this returns STATUS_MALFORMED rather than
// blocking or queuing — retry after a UART_READ or a short wait. Empty
// response payload on success.

// UART_READ request payload.
typedef struct __attribute__((packed)) {
    uint16_t max_bytes; // clamped to fit one response frame (~502 bytes)
} uart_read_request_t;

// UART_READ response payload, followed by returned_count raw data bytes
// (per-byte error flags already folded into the counters below, not
// carried per byte on the wire).
typedef struct __attribute__((packed)) {
    uint32_t framing_errors;    // cumulative since the last UART_CONFIG
    uint32_t parity_errors;
    uint32_t break_errors;
    uint32_t overrun_errors;     // hardware FIFO overrun (PL011 OE flag)
    uint32_t ring_overrun_count; // our own RX ring overwritten before being
                                  // read — host polled UART_READ too slowly
    uint16_t returned_count;
    uint16_t reserved;
} uart_read_response_t;

// I2C_XFER request payload (master role, i2c0/GP20-21), followed by
// write_len raw bytes to write. A combined write-then-read transaction —
// the read phase uses a repeated start (no STOP between write and read) so
// this covers the common "write register pointer, read data back" pattern
// in one call. Not gated by ARM_OUTPUTS — a bus, not a static output level.
typedef struct __attribute__((packed)) {
    uint8_t  address;    // 7-bit I2C address (0-127)
    uint8_t  reserved;
    uint16_t write_len;
    uint16_t read_len;   // 0 = write-only
    uint16_t timeout_ms; // 0 = default (100ms); clamped to [1, 5000]
} i2c_xfer_request_t;

// I2C_XFER response payload, followed by bytes_read raw bytes (only
// present if read_len > 0 and read_status == 0). If write_status != 0, the
// read phase is skipped entirely (bytes_read will be 0) — a failed write
// leaves the bus/device in an uncertain state, not worth reading from.
// A write_status of TIMEOUT (not NACK) triggers an automatic GPIO bit-bang
// bus recovery (up to 9 SCL pulses) before this response is even sent, so
// the bus is left in a clean state for the next call regardless of outcome.
typedef struct __attribute__((packed)) {
    uint8_t  write_status; // 0=ok/not-attempted, 1=nack, 2=timeout
    uint8_t  read_status;  // 0=ok/not-attempted, 1=nack, 2=timeout
    uint16_t bytes_written;
    uint16_t bytes_read;
} i2c_xfer_response_t;

// I2C_SLAVE_CONFIG request payload (slave role, i2c1/GP18-19). Always
// resets: (re)configuring or disabling wipes the register map, read
// pointer, and counters below.
typedef struct __attribute__((packed)) {
    uint8_t  enabled;
    uint8_t  address; // 7-bit address; meaningful only if enabled=1
    uint16_t reserved;
} i2c_slave_config_request_t;

// I2C_SLAVE_CONFIG response payload. Counters cover activity since the
// *previous* I2C_SLAVE_CONFIG call (or boot), then reset — call again
// (even with identical parameters) to fetch-and-reset a fresh baseline.
// There is no opcode to read the register map's contents remotely; it's
// only reachable by an external master over the physical bus (or via
// I2C_XFER against this device's own i2c0 master, if GP18-21 are
// jumpered together for a self-loopback test).
typedef struct __attribute__((packed)) {
    uint32_t bytes_received;     // since the previous call
    uint32_t bytes_sent;
    uint32_t transaction_count;  // completed transactions (STOP/RESTART) since the previous call
} i2c_slave_config_response_t;

// SPI_XFER request payload (master role, GP8 SCK / GP9 MOSI / GP10 MISO
// / GP11 CS0_n, via PIO not hardware SPI — see the plan's pin-rework
// rationale), followed by len raw TX bytes. Full-duplex, single length for
// both directions — the standard "spi_transfer(tx, rx, len)" convention: if
// you want more RX bytes than you have real TX data for, pad the TX buffer
// with dummy bytes (0x00) yourself to reach the desired len. Not gated by
// ARM_OUTPUTS — a bus, not a static output level.
//
// Only mode 0 (CPOL=0, CPHA=0) is implemented this pass — modes 1-3 return
// STATUS_MALFORMED. This is the most common SPI mode (default state of the
// vast majority of SPI peripherals), chosen deliberately as a smaller,
// verifiable-by-construction subset rather than four modes implemented but
// none of them checked — see protocol.md for current verification status.
typedef struct __attribute__((packed)) {
    uint8_t  mode;    // must be 0 this pass
    uint8_t  reserved;
    uint32_t hz;      // clamped to what the PIO clkdiv can hit, up to SPI_MASTER_MAX_HZ
    uint16_t len;     // bytes to transfer, same count both directions
} spi_xfer_request_t;

// SPI_XFER response payload, followed by len raw RX bytes.
typedef struct __attribute__((packed)) {
    uint32_t granted_hz;
    uint16_t len;
    uint16_t reserved;
} spi_xfer_response_t;

// SPI_SLAVE_CONFIG request payload (slave role, GP12 SCK / GP13 MOSI /
// GP14 MISO / GP15 CS_n). Mode 0 only, hardcoded in the PIO program — no
// mode field needed (unlike SPI_XFER, which takes one because it supports
// selecting an as-yet-unimplemented mode later; the slave program would
// need a second variant entirely, so there's nothing to select yet).
typedef struct __attribute__((packed)) {
    uint8_t enabled;
    uint8_t reserved[3];
} spi_slave_config_request_t;

// SPI_SLAVE_CONFIG response payload. Always resets: (re)configuring or
// disabling wipes the RX ring, read position, and all counters below —
// call again (even with identical parameters) to fetch-and-reset a fresh
// baseline, same convention as I2C_SLAVE_CONFIG.
//
// MISO continuously repeats a fixed 8-byte pattern (0x00..0x07) for as
// long as the master keeps clocking — there is no opcode to set custom
// slave response data (matches the plan's frozen opcode list, and
// mirrors I2C_SLAVE_CONFIG's same limitation). MOSI is captured for
// counting only, not remotely readable.
typedef struct __attribute__((packed)) {
    uint32_t bytes_received;
    uint32_t bytes_sent;        // NOT a count of bytes actually clocked out
                                  // onto the pin — the TX DMA pipeline (4-deep
                                  // FIFO + 1-deep OSR) is eagerly pre-filled
                                  // on enable regardless of real SCK
                                  // activity, so this reads ~5 at idle with
                                  // nothing connected. Only meaningful as a
                                  // rough "more than ~5 bytes consumed"
                                  // signal during sustained real activity.
    uint32_t transaction_count;  // CS falling-edge count since the previous call
} spi_slave_config_response_t;
