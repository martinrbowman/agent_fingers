#include "transport/usb_cdc.h"
#include "build_info.h"
#include "debug/dap.h"
#include "tusb.h"
#include <stdbool.h>
#include <stdio.h>

static bool s_was_connected;

void usb_cdc_init(void) {
    s_was_connected = false;
}

void usb_cdc_task(void) {
    bool connected = tud_cdc_connected();
    if (connected && !s_was_connected) {
        char banner[160];
        int n = snprintf(banner, sizeof(banner),
                          "agent_fingers %d.%d.%d (%s)\r\n"
                          "debug probe: SWD max %lu kHz, JTAG max %lu kHz (bit-bang, measured)\r\n",
                          FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH, FW_GIT_HASH,
                          (unsigned long)(dap_swd_max_hz() / 1000u),
                          (unsigned long)(dap_jtag_max_hz() / 1000u));
        if (n > 0) {
            tud_cdc_write(banner, (uint32_t)n);
            tud_cdc_write_flush();
        }
    }
    s_was_connected = connected;

    // Diagnostic console is output-only for now; discard anything a
    // terminal sends so it never blocks the CDC RX FIFO.
    if (tud_cdc_available()) {
        uint8_t discard[64];
        tud_cdc_read(discard, sizeof(discard));
    }
}
