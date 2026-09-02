#include "transport/usb_vendor.h"
#include "protocol/dispatch.h"
#include "tusb.h"

static void usb_vendor_output(const uint8_t *frame, size_t frame_len) {
    // tud_vendor_write() only copies as much as current FIFO space allows
    // and returns the short count — never assume it took everything.
    size_t sent = 0;
    while (sent < frame_len) {
        uint32_t avail = tud_vendor_write_available();
        if (avail == 0) {
            tud_task();
            continue;
        }
        uint32_t chunk = (uint32_t)(frame_len - sent);
        if (chunk > avail) {
            chunk = avail;
        }
        sent += tud_vendor_write(frame + sent, chunk);
    }
    tud_vendor_write_flush();
}

void usb_vendor_init(void) {
    protocol_dispatch_init(usb_vendor_output);
}

void usb_vendor_task(void) {
    if (tud_vendor_available()) {
        static uint8_t buf[64];
        uint32_t n = tud_vendor_read(buf, sizeof(buf));
        protocol_dispatch_feed(buf, n);
    }
}
