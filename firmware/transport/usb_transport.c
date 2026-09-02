#include "transport/usb_transport.h"
#include "transport/usb_cdc.h"
#include "transport/usb_vendor.h"
#include "tusb.h"

void usb_transport_init(void) {
    tud_init(0);
    usb_cdc_init();
    usb_vendor_init();
}

void usb_transport_task(void) {
    tud_task();
    usb_cdc_task();
    usb_vendor_task();
}

bool usb_transport_mounted(void) {
    return tud_mounted();
}
