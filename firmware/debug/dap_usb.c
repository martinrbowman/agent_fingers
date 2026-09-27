#include "debug/dap_usb.h"
#include "device/usbd_pvt.h"
#include "tusb.h"
#include <string.h>

#define DAP_USB_INTERFACE 3u // must match usb_descriptors.c (USBD_ITF_DAP)

typedef struct {
    uint8_t  data[DAP_USB_PACKET_SIZE];
    uint16_t len;
} dap_usb_packet_t;

static uint8_t s_rhport;
static uint8_t s_ep_out;
static uint8_t s_ep_in;
static bool    s_opened;

static dap_usb_packet_t s_queue[DAP_USB_QUEUE_DEPTH];
static uint8_t s_head;
static uint8_t s_count;
static bool    s_rx_armed;
static bool    s_tx_busy;

// USB DMA buffers.
CFG_TUSB_MEM_ALIGN static uint8_t s_rx_buf[DAP_USB_PACKET_SIZE];
CFG_TUSB_MEM_ALIGN static uint8_t s_tx_buf[DAP_USB_PACKET_SIZE];

static void arm_rx(void) {
    // Only take another packet from the host when there's a slot for it:
    // the host's OUT transfers NAK meanwhile instead of commands being lost.
    if (s_opened && !s_rx_armed && s_count < DAP_USB_QUEUE_DEPTH) {
        s_rx_armed = usbd_edpt_xfer(s_rhport, s_ep_out, s_rx_buf, DAP_USB_PACKET_SIZE);
    }
}

static void driver_init(void) {
    s_opened = false;
}

static bool driver_deinit(void) {
    return true;
}

static void driver_reset(uint8_t rhport) {
    (void)rhport;
    s_opened = false;
    s_rx_armed = false;
    s_tx_busy = false;
    s_head = 0;
    s_count = 0;
}

static uint16_t driver_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len) {
    if (itf->bInterfaceNumber != DAP_USB_INTERFACE || itf->bNumEndpoints != 2) {
        return 0; // not ours -- the generic vendor class takes interface 2
    }
    uint16_t len = (uint16_t)(sizeof(tusb_desc_interface_t) + 2 * sizeof(tusb_desc_endpoint_t));
    if (max_len < len) {
        return 0;
    }
    uint8_t const *p_desc = tu_desc_next(itf);
    if (!usbd_open_edpt_pair(rhport, p_desc, 2, TUSB_XFER_BULK, &s_ep_out, &s_ep_in)) {
        return 0;
    }
    s_rhport = rhport;
    s_opened = true;
    s_rx_armed = false;
    s_tx_busy = false;
    arm_rx();
    return len;
}

static bool driver_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    (void)rhport;
    (void)stage;
    (void)request;
    return false;
}

static bool driver_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
    (void)rhport;
    (void)result;
    if (ep_addr == s_ep_out) {
        s_rx_armed = false;
        if (s_count < DAP_USB_QUEUE_DEPTH) {
            dap_usb_packet_t *p = &s_queue[(s_head + s_count) % DAP_USB_QUEUE_DEPTH];
            p->len = (uint16_t)(xferred_bytes > DAP_USB_PACKET_SIZE ? DAP_USB_PACKET_SIZE : xferred_bytes);
            memcpy(p->data, s_rx_buf, p->len);
            s_count++;
        }
        arm_rx();
    } else if (ep_addr == s_ep_in) {
        s_tx_busy = false;
    }
    return true;
}

static const usbd_class_driver_t s_driver = {
    .name = "CMSIS-DAP",
    .init = driver_init,
    .deinit = driver_deinit,
    .reset = driver_reset,
    .open = driver_open,
    .control_xfer_cb = driver_control_xfer_cb,
    .xfer_cb = driver_xfer_cb,
    .sof = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &s_driver;
}

bool dap_usb_receive(uint8_t *buf, uint16_t *len) {
    if (s_count == 0) {
        return false;
    }
    dap_usb_packet_t *p = &s_queue[s_head];
    memcpy(buf, p->data, p->len);
    *len = p->len;
    s_head = (uint8_t)((s_head + 1) % DAP_USB_QUEUE_DEPTH);
    s_count--;
    arm_rx();
    return true;
}

bool dap_usb_tx_ready(void) {
    return s_opened && !s_tx_busy;
}

void dap_usb_send(const uint8_t *buf, uint16_t len) {
    if (!s_opened || len == 0) {
        return;
    }
    if (len > DAP_USB_PACKET_SIZE) {
        len = DAP_USB_PACKET_SIZE;
    }
    memcpy(s_tx_buf, buf, len);
    // Exact-length transfer: a full 64-byte response goes as one packet with
    // no ZLP, as CMSIS-DAP hosts expect.
    s_tx_busy = usbd_edpt_xfer(s_rhport, s_ep_in, s_tx_buf, len);
}

void dap_usb_flush(void) {
    s_head = 0;
    s_count = 0;
    arm_rx();
}
