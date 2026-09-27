#include "transport/usb_transport.h"
#include "transport/usb_cdc.h"
#include "transport/usb_link.h"
#include "transport/usb_vendor.h"
#include "protocol/dispatch.h"
#include "app/board.h"
#include "debug/dap.h"
#include "debug/debug_session.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "hardware/watchdog.h"
#include "tusb.h"
#include <string.h>

// Depths sized so a whole maximum-size response (PROTOCOL_RX_PAYLOAD_CAP-
// class frames, ~8.2KB = ~129 chunks) fits in the TX queue without core0
// blocking on core1, and the RX side holds a full inbound frame of the
// same size. ~15KB of SRAM total.
#define USB_LINK_TX_DEPTH 160u
#define USB_LINK_RX_DEPTH 130u

// Bounds how long one core0 main-loop iteration spends parsing inbound
// bytes, so a host streaming requests can't starve the other tasks.
#define USB_LINK_RX_CHUNKS_PER_TASK 8u

queue_t g_usb_link_rx;
queue_t g_usb_link_tx;

// core1 liveness. core1's loop never blocks, so a heartbeat that stops
// moving means core1 is gone -- and with it all of USB. Found on hardware:
// OpenOCD's `program ... reset` resets the two cores one after the other,
// so core0 can launch core1 and then have OpenOCD's reset of core1 land
// afterwards, parking it back in the bootrom launch wait. USB then stays
// dead until a power cycle. core0 answers a stalled heartbeat with a full
// chip reboot, which relaunches core1 cleanly.
//
// Caveat: halting core1 alone in a debugger for longer than this trips
// it too. Halt both cores (OpenOCD's default) when debugging.
#define CORE1_STALL_TIMEOUT_US 1000000u

static volatile bool     s_mounted;
static volatile uint32_t s_core1_heartbeat;
static uint32_t          s_core1_heartbeat_seen;
static absolute_time_t   s_core1_deadline;

// core0: protocol_dispatch's output function. Blocks only while the TX
// queue is full, i.e. only while core1 is actively draining it to the host
// (core1 discards queued TX when unmounted, so this can't wedge forever).
static void link_output(const uint8_t *frame, size_t frame_len) {
    while (frame_len) {
        usb_link_chunk_t chunk;
        chunk.len = (uint16_t)(frame_len < USB_LINK_CHUNK_BYTES ? frame_len : USB_LINK_CHUNK_BYTES);
        memcpy(chunk.data, frame, chunk.len);
        queue_add_blocking(&g_usb_link_tx, &chunk);
        frame += chunk.len;
        frame_len -= chunk.len;
    }
}

static void core1_main(void) {
    // tud_init() enables the USB IRQ on the calling core, which is what
    // pins all of TinyUSB -- IRQ, tud_task, class drivers -- to core1.
    tud_init(0);
    usb_cdc_init();
    usb_vendor_init();
    dap_init();
    for (;;) {
        tud_task();
        usb_cdc_task();
        usb_vendor_task();
        dap_task();
        s_mounted = tud_mounted();
        s_core1_heartbeat++;
    }
}

void usb_transport_init(void) {
    queue_init(&g_usb_link_rx, sizeof(usb_link_chunk_t), USB_LINK_RX_DEPTH);
    queue_init(&g_usb_link_tx, sizeof(usb_link_chunk_t), USB_LINK_TX_DEPTH);
    protocol_dispatch_init(link_output);
    debug_session_init(); // before core1 can request a session
    s_core1_heartbeat = 0;
    s_core1_heartbeat_seen = 0;
    s_core1_deadline = make_timeout_time_us(CORE1_STALL_TIMEOUT_US);
    multicore_launch_core1(core1_main);
}

static void supervise_core1(void) {
    uint32_t beat = s_core1_heartbeat;
    if (beat != s_core1_heartbeat_seen) {
        s_core1_heartbeat_seen = beat;
        s_core1_deadline = make_timeout_time_us(CORE1_STALL_TIMEOUT_US);
    } else if (time_reached(s_core1_deadline)) {
        board_note_core1_stall();
        watchdog_reboot(0, 0, 0);
        for (;;) {
        }
    }
}

void usb_transport_task(void) {
    supervise_core1();

    usb_link_chunk_t chunk;
    for (unsigned i = 0; i < USB_LINK_RX_CHUNKS_PER_TASK; i++) {
        if (!queue_try_remove(&g_usb_link_rx, &chunk)) {
            break;
        }
        protocol_dispatch_feed(chunk.data, chunk.len);
    }
}

void usb_transport_core1_alive(void) {
    s_core1_heartbeat++;
}

bool usb_transport_mounted(void) {
    return s_mounted;
}
