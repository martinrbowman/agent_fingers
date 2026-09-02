#pragma once

// Primary binary protocol transport (vendor bulk IN/OUT). Feeds received
// bytes into protocol_dispatch_feed() and transmits its responses.
void usb_vendor_init(void);
void usb_vendor_task(void);
