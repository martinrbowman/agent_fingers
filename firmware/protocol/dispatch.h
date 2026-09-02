#pragma once
#include <stddef.h>
#include <stdint.h>

// Called with one fully-framed outgoing message (header+payload+crc) to
// transmit as-is. Installed by the transport layer.
typedef void (*protocol_output_fn)(const uint8_t *frame, size_t frame_len);

void protocol_dispatch_init(protocol_output_fn output);

// Feed newly received transport bytes into the frame parser. May produce
// zero or more outgoing frames via the installed output function before
// returning.
void protocol_dispatch_feed(const uint8_t *data, size_t len);

// Call every main-loop iteration. Emits any pending unsolicited EVENT frame
// (currently: digital-capture completion) via the installed output function.
void protocol_dispatch_poll_events(void);
