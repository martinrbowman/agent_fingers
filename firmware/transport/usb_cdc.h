#pragma once

// Human diagnostic console only — prints a boot banner when a terminal
// opens the port. The framed binary protocol runs on vendor bulk
// (usb_vendor.h), not here.
void usb_cdc_init(void);
void usb_cdc_task(void);
