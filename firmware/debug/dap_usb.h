#pragma once
#include <stdbool.h>
#include <stdint.h>

// core1 only. USB transport for the CMSIS-DAP v2 interface (interface 3),
// as a TinyUSB application class driver instead of the generic vendor
// class: CMSIS-DAP frames one command/response per USB transfer, and the
// vendor class's streaming FIFO would merge back-to-back responses into
// one packet and add a ZLP after a full 64-byte response -- both break DAP
// framing once more than one packet is in flight (DAP_PACKET_COUNT > 1).

#define DAP_USB_PACKET_SIZE 64u
#define DAP_USB_QUEUE_DEPTH 8u // == advertised DAP packet count

// Oldest queued command, if any (copied out; its slot is freed).
bool dap_usb_receive(uint8_t *buf, uint16_t *len);
// True when the previous response has been sent (one IN transfer in flight
// at most, so responses can never merge).
bool dap_usb_tx_ready(void);
void dap_usb_send(const uint8_t *buf, uint16_t len);
// Drops queued commands (e.g. on USB unmount).
void dap_usb_flush(void);
