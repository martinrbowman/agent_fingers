#pragma once
#include <stdbool.h>
#include <stdint.h>

// core1 only. SWO capture in UART (NRZ) mode on GP10: a PIO receiver into
// a DMA ring, drained by DAP_SWO_Data. Only while an SWD session owns the
// pins (GP10 is TDO in JTAG mode).

#define SWO_BUFFER_SIZE 4096u

void swo_init(void);                 // claims PIO SM + DMA channel; call once at boot
uint32_t swo_set_baudrate(uint32_t baud); // returns the actual rate, 0 if impossible
bool swo_start(void);
void swo_stop(void);
bool swo_active(void);
bool swo_overrun(void);              // sticky until the next start
uint32_t swo_available(void);        // bytes buffered, not yet read
uint32_t swo_read(uint8_t *out, uint32_t max);
