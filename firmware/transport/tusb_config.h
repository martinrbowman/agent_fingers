#pragma once

#define CFG_TUSB_RHPORT0_MODE  (OPT_MODE_DEVICE)
#define CFG_TUD_ENDPOINT0_SIZE 64

// CDC: human diagnostic console only (boot banner). Vendor bulk: primary
// binary protocol transport. Both are separate TinyUSB interfaces; the
// framed protocol runs on vendor bulk exclusively — see usb_vendor.c.
#define CFG_TUD_CDC 1
#define CFG_TUD_VENDOR 1 // control/capture channel; CMSIS-DAP v2 has its own driver (debug/dap_usb.c)

#define CFG_TUD_CDC_RX_BUFSIZE 64
#define CFG_TUD_CDC_TX_BUFSIZE 256
#define CFG_TUD_CDC_EP_BUFSIZE 64

// Does NOT need to hold a whole protocol frame -- usb_vendor_output()
// writes in a loop, waiting on tud_task() whenever the FIFO's full, so
// frames much larger than this (up to header+PROTOCOL_RX_PAYLOAD_CAP+crc,
// currently 8216 bytes) still send correctly, just via more chunks. This
// just needs to comfortably beat the default (64, nowhere near enough)
// to keep the write loop from stalling on every single USB packet.
#define CFG_TUD_VENDOR_RX_BUFSIZE 2048
#define CFG_TUD_VENDOR_TX_BUFSIZE 2048
