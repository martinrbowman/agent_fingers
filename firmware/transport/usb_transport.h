#pragma once
#include <stdbool.h>

// USB runs entirely on core1: usb_transport_init() (called on core0) sets
// up the inter-core queues and launches core1, which owns tud_init()/
// tud_task(), the CDC console, and the vendor bulk bridge. Nothing on
// core0 may call tud_* directly -- see usb_link.h.
void usb_transport_init(void);

// core0 side: feeds bytes core1 received from the host into the protocol
// parser. Call every core0 main-loop iteration.
void usb_transport_task(void);

// True once the device is configured by the host (address+config set),
// independent of whether any interface has an open application on top.
// Safe to call from either core.
bool usb_transport_mounted(void);

// core1: marks core1 alive from inside long-running work (e.g. a DAP
// transfer retrying on SWD WAIT) that keeps it away from its main loop,
// so the core0 supervisor doesn't mistake it for a stall.
void usb_transport_core1_alive(void);
