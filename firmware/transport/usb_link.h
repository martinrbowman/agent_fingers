#pragma once
#include <stdint.h>
#include "pico/util/queue.h"

// Inter-core byte pipes for the vendor control channel. TinyUSB runs on
// core1 (see usb_transport.c) and isn't thread-safe, so core0 never calls
// tud_*; it exchanges raw protocol bytes with core1 through these queues
// instead. Chunks, not whole frames: the protocol parser on core0 is a
// byte-stream parser already, and the host reader reassembles on its side.

#define USB_LINK_CHUNK_BYTES 64u

typedef struct {
    uint16_t len;
    uint8_t  data[USB_LINK_CHUNK_BYTES];
} usb_link_chunk_t;

// Host -> device bytes, filled by core1, drained by core0.
extern queue_t g_usb_link_rx;
// Device -> host bytes, filled by core0, drained by core1.
extern queue_t g_usb_link_tx;
