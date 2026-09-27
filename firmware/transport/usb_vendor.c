#include "transport/usb_vendor.h"
#include "transport/usb_link.h"
#include "tusb.h"

// Partially written TX chunk carried across task calls:
// tud_vendor_write() only copies as much as current FIFO space allows and
// returns the short count -- never assume it took everything (the original
// cause of frames over 64 bytes being silently truncated during step 5).
static usb_link_chunk_t s_tx;
static uint16_t s_tx_offset;
static bool     s_tx_pending;

void usb_vendor_init(void) {
    s_tx_pending = false;
}

static void pump_rx(void) {
    // Only pull from TinyUSB while core0 has room. Leaving bytes in the
    // endpoint FIFO makes the host's OUT transfers NAK -- real
    // back-pressure instead of dropped bytes when core0 is busy.
    while (tud_vendor_available() && !queue_is_full(&g_usb_link_rx)) {
        usb_link_chunk_t chunk;
        chunk.len = (uint16_t)tud_vendor_read(chunk.data, sizeof(chunk.data));
        if (chunk.len == 0) {
            break;
        }
        queue_try_add(&g_usb_link_rx, &chunk);
    }
}

static void pump_tx(void) {
    if (!tud_mounted()) {
        // No host to send to: discard, so core0's blocking output can't
        // wedge on a full queue (e.g. a capture-complete EVENT emitted
        // while unplugged).
        s_tx_pending = false;
        usb_link_chunk_t discard;
        while (queue_try_remove(&g_usb_link_tx, &discard)) {
        }
        return;
    }

    bool wrote = false;
    for (;;) {
        if (!s_tx_pending) {
            if (!queue_try_remove(&g_usb_link_tx, &s_tx)) {
                break;
            }
            s_tx_offset = 0;
            s_tx_pending = true;
        }
        uint32_t avail = tud_vendor_write_available();
        if (avail == 0) {
            break;
        }
        uint32_t chunk = (uint32_t)(s_tx.len - s_tx_offset);
        if (chunk > avail) {
            chunk = avail;
        }
        s_tx_offset += (uint16_t)tud_vendor_write(s_tx.data + s_tx_offset, chunk);
        wrote = true;
        if (s_tx_offset >= s_tx.len) {
            s_tx_pending = false;
        }
    }
    if (wrote) {
        tud_vendor_write_flush();
    }
}

void usb_vendor_task(void) {
    pump_rx();
    pump_tx();
}
