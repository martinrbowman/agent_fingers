#pragma once

#define CFG_TUSB_RHPORT0_MODE  (OPT_MODE_DEVICE)
#define CFG_TUD_ENDPOINT0_SIZE 64

// CDC: human diagnostic console only (boot banner). Vendor bulk: primary
// binary protocol transport. Both are separate TinyUSB interfaces; the
// framed protocol runs on vendor bulk exclusively — see usb_vendor.c.
#define CFG_TUD_CDC 1
#define CFG_TUD_VENDOR 1

#define CFG_TUD_CDC_RX_BUFSIZE 64
#define CFG_TUD_CDC_TX_BUFSIZE 64
#define CFG_TUD_CDC_EP_BUFSIZE 64

// Must comfortably hold one full protocol frame (header + payload cap + crc
// = 20+512+4 = 536 bytes) — tud_vendor_write() silently truncates to
// whatever FIFO space is available if this is smaller than the frame, and
// the default (64) is nowhere near big enough.
#define CFG_TUD_VENDOR_RX_BUFSIZE 576
#define CFG_TUD_VENDOR_TX_BUFSIZE 576
