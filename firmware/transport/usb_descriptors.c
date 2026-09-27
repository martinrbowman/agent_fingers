#include "tusb.h"
#include "pico/unique_id.h"

// TinyUSB's common open-hardware development VID/PID. Replace with a
// purchased production VID/PID before shipping — see plan's USB composition
// section ("obtain a production VID/PID rather than shipping a borrowed
// identifier").
#define USBD_VID 0xCafeu
#define USBD_PID 0x4001u

#define USBD_MANUFACTURER "agent_fingers"
#define USBD_PRODUCT       "RP2350 Signal Agent (dev)"

#define USBD_ITF_CDC    0 // consumes interfaces 0 and 1
#define USBD_ITF_VENDOR 2
#define USBD_ITF_DAP    3 // CMSIS-DAP v2 (debug/dap_usb.c driver; keep in sync)
#define USBD_ITF_MAX    4

#define USBD_CDC_EP_CMD 0x81
#define USBD_CDC_EP_OUT 0x02
#define USBD_CDC_EP_IN  0x82
#define USBD_CDC_CMD_MAX_SIZE 8
#define USBD_CDC_IN_OUT_MAX_SIZE 64

#define USBD_VENDOR_EP_OUT 0x03
#define USBD_VENDOR_EP_IN  0x83
#define USBD_DAP_EP_OUT    0x04
#define USBD_DAP_EP_IN     0x84

#define USBD_DESC_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + 2 * TUD_VENDOR_DESC_LEN)
#define USBD_DESC_STR_MAX 32

enum {
    USBD_STR_0 = 0,
    USBD_STR_MANUF,
    USBD_STR_PRODUCT,
    USBD_STR_SERIAL,
    USBD_STR_CDC,
    USBD_STR_VENDOR,
    USBD_STR_DAP,
};

static const tusb_desc_device_t usbd_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210, // 2.1: required for the BOS descriptor (MS OS 2.0)
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USBD_VID,
    .idProduct = USBD_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = USBD_STR_MANUF,
    .iProduct = USBD_STR_PRODUCT,
    .iSerialNumber = USBD_STR_SERIAL,
    .bNumConfigurations = 1,
};

static const uint8_t usbd_desc_cfg[USBD_DESC_LEN] = {
    TUD_CONFIG_DESCRIPTOR(1, USBD_ITF_MAX, USBD_STR_0, USBD_DESC_LEN, 0, 250),
    TUD_CDC_DESCRIPTOR(USBD_ITF_CDC, USBD_STR_CDC, USBD_CDC_EP_CMD,
        USBD_CDC_CMD_MAX_SIZE, USBD_CDC_EP_OUT, USBD_CDC_EP_IN, USBD_CDC_IN_OUT_MAX_SIZE),
    TUD_VENDOR_DESCRIPTOR(USBD_ITF_VENDOR, USBD_STR_VENDOR,
        USBD_VENDOR_EP_OUT, USBD_VENDOR_EP_IN, CFG_TUD_VENDOR_EPSIZE),
    // OpenOCD/pyOCD/probe-rs find a CMSIS-DAP v2 probe by "CMSIS-DAP" in
    // this interface's string -- keep it there.
    TUD_VENDOR_DESCRIPTOR(USBD_ITF_DAP, USBD_STR_DAP,
        USBD_DAP_EP_OUT, USBD_DAP_EP_IN, CFG_TUD_VENDOR_EPSIZE),
};

static char usbd_serial_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

static const char *const usbd_desc_str[] = {
    [USBD_STR_MANUF]   = USBD_MANUFACTURER,
    [USBD_STR_PRODUCT] = USBD_PRODUCT,
    [USBD_STR_SERIAL]  = usbd_serial_str,
    [USBD_STR_CDC]     = "Diagnostic console",
    [USBD_STR_VENDOR]  = "Control/capture channel",
    [USBD_STR_DAP]     = "Agent Fingers CMSIS-DAP v2",
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&usbd_desc_device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return usbd_desc_cfg;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t desc_str[USBD_DESC_STR_MAX];

    if (!usbd_serial_str[0]) {
        pico_get_unique_board_id_string(usbd_serial_str, sizeof(usbd_serial_str));
    }

    uint8_t len;
    if (index == 0) {
        desc_str[1] = 0x0409; // English
        len = 1;
    } else {
        if (index >= sizeof(usbd_desc_str) / sizeof(usbd_desc_str[0])) {
            return NULL;
        }
        const char *str = usbd_desc_str[index];
        for (len = 0; len < USBD_DESC_STR_MAX - 1 && str[len]; ++len) {
            desc_str[1 + len] = str[len];
        }
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc_str;
}

// -- Microsoft OS 2.0 descriptors ----------------------------------------------
// Lets Windows bind WinUSB to both vendor interfaces automatically (no
// Zadig/INF), which the control channel's pyusb host and CMSIS-DAP v2 tools
// (Keil, IAR, MCUXpresso, OpenOCD, pyOCD) need. Windows reads the BOS
// descriptor, then fetches the descriptor set below with a vendor request.
// The CDC interfaces keep their class driver. Not exercised on Windows yet
// (developed on macOS); byte layout checked with pyusb.
//
// Interface GUIDs: CMSIS-DAP v2's standard {CDB3B5AD-...}, and one of our
// own for the control/capture channel.

#define USBD_MS_OS_20_VENDOR_CODE 0x01
#define USBD_MS_OS_20_DESC_LEN    338

// Set header -> configuration subset -> one function subset per vendor
// interface (2: control, 3: CMSIS-DAP), each with a WINUSB compatible ID
// and a DeviceInterfaceGUIDs registry property. Generated (lengths checked)
// rather than hand-assembled.
static const uint8_t usbd_ms_os_20_desc[USBD_MS_OS_20_DESC_LEN] = {
    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x06, 0x52, 0x01, 0x08, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x48, 0x01, 0x08, 0x00, 0x02, 0x00, 0x02, 0x00,
    0xA0, 0x00, 0x14, 0x00, 0x03, 0x00, 0x57, 0x49, 0x4E, 0x55, 0x53, 0x42,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x84, 0x00,
    0x04, 0x00, 0x07, 0x00, 0x2A, 0x00, 0x44, 0x00, 0x65, 0x00, 0x76, 0x00,
    0x69, 0x00, 0x63, 0x00, 0x65, 0x00, 0x49, 0x00, 0x6E, 0x00, 0x74, 0x00,
    0x65, 0x00, 0x72, 0x00, 0x66, 0x00, 0x61, 0x00, 0x63, 0x00, 0x65, 0x00,
    0x47, 0x00, 0x55, 0x00, 0x49, 0x00, 0x44, 0x00, 0x73, 0x00, 0x00, 0x00,
    0x50, 0x00, 0x7B, 0x00, 0x32, 0x00, 0x35, 0x00, 0x34, 0x00, 0x37, 0x00,
    0x44, 0x00, 0x34, 0x00, 0x44, 0x00, 0x41, 0x00, 0x2D, 0x00, 0x39, 0x00,
    0x45, 0x00, 0x45, 0x00, 0x46, 0x00, 0x2D, 0x00, 0x34, 0x00, 0x44, 0x00,
    0x41, 0x00, 0x39, 0x00, 0x2D, 0x00, 0x39, 0x00, 0x32, 0x00, 0x44, 0x00,
    0x36, 0x00, 0x2D, 0x00, 0x42, 0x00, 0x46, 0x00, 0x39, 0x00, 0x41, 0x00,
    0x38, 0x00, 0x32, 0x00, 0x32, 0x00, 0x38, 0x00, 0x42, 0x00, 0x41, 0x00,
    0x30, 0x00, 0x44, 0x00, 0x7D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00,
    0x02, 0x00, 0x03, 0x00, 0xA0, 0x00, 0x14, 0x00, 0x03, 0x00, 0x57, 0x49,
    0x4E, 0x55, 0x53, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x84, 0x00, 0x04, 0x00, 0x07, 0x00, 0x2A, 0x00, 0x44, 0x00,
    0x65, 0x00, 0x76, 0x00, 0x69, 0x00, 0x63, 0x00, 0x65, 0x00, 0x49, 0x00,
    0x6E, 0x00, 0x74, 0x00, 0x65, 0x00, 0x72, 0x00, 0x66, 0x00, 0x61, 0x00,
    0x63, 0x00, 0x65, 0x00, 0x47, 0x00, 0x55, 0x00, 0x49, 0x00, 0x44, 0x00,
    0x73, 0x00, 0x00, 0x00, 0x50, 0x00, 0x7B, 0x00, 0x43, 0x00, 0x44, 0x00,
    0x42, 0x00, 0x33, 0x00, 0x42, 0x00, 0x35, 0x00, 0x41, 0x00, 0x44, 0x00,
    0x2D, 0x00, 0x32, 0x00, 0x39, 0x00, 0x33, 0x00, 0x42, 0x00, 0x2D, 0x00,
    0x34, 0x00, 0x36, 0x00, 0x36, 0x00, 0x33, 0x00, 0x2D, 0x00, 0x41, 0x00,
    0x41, 0x00, 0x33, 0x00, 0x36, 0x00, 0x2D, 0x00, 0x31, 0x00, 0x41, 0x00,
    0x41, 0x00, 0x45, 0x00, 0x34, 0x00, 0x36, 0x00, 0x34, 0x00, 0x36, 0x00,
    0x33, 0x00, 0x37, 0x00, 0x37, 0x00, 0x36, 0x00, 0x7D, 0x00, 0x00, 0x00,
    0x00, 0x00,
};

#define USBD_BOS_TOTAL_LEN (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

static const uint8_t usbd_desc_bos[] = {
    TUD_BOS_DESCRIPTOR(USBD_BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(USBD_MS_OS_20_DESC_LEN, USBD_MS_OS_20_VENDOR_CODE),
};

const uint8_t *tud_descriptor_bos_cb(void) {
    return usbd_desc_bos;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bRequest == USBD_MS_OS_20_VENDOR_CODE &&
        request->wIndex == 7) { // MS_OS_20_DESCRIPTOR_INDEX
        return tud_control_xfer(rhport, request, (void *)(uintptr_t)usbd_ms_os_20_desc,
                                USBD_MS_OS_20_DESC_LEN);
    }
    return false; // stall anything else
}

