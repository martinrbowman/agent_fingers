#pragma once

// core1 only. Bridges the primary binary protocol's vendor bulk IN/OUT
// endpoints to core0 through the usb_link.h queues; the protocol parser
// itself runs on core0.
void usb_vendor_init(void);
void usb_vendor_task(void);
