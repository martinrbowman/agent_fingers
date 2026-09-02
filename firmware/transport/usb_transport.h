#pragma once
#include <stdbool.h>

// Owns tud_init()/tud_task() and polls the CDC and vendor sub-modules.
void usb_transport_init(void);
void usb_transport_task(void);

// True once the device is configured by the host (address+config set),
// independent of whether any interface has an open application on top.
bool usb_transport_mounted(void);
