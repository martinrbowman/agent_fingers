#pragma once
#include <stdint.h>

// core1 only. CMSIS-DAP v2 command processor on the second vendor bulk
// interface (see usb_descriptors.c). DAP_Connect claims the pins via
// debug_session (reply deferred until core0 hands them over);
// DAP_Disconnect / 10 s inactivity / USB unmount release them.
// SWD (9.3): Transfer, TransferBlock, TransferConfigure, WriteABORT,
// SWJ_Pins, SWJ_Clock, SWJ_Sequence, SWD_Configure, SWD_Sequence, Delay,
// ResetTarget. JTAG (9.4): JTAG_Sequence, JTAG_Configure, and SWJ_Pins/
// SWJ_Sequence routed to the JTAG pins (TMS on GP11), plus ARM JTAG-DP
// access: Transfer, TransferBlock, WriteABORT and JTAG_IDCODE in JTAG mode
// (chain from JTAG_Configure). SWO (UART/NRZ mode, via DAP_SWO_Data):
// Transport, Mode, Baudrate, Control, Status, Data. Not implemented:
// Manchester SWO, SWO streaming endpoint, SWO_ExtendedStatus,
// atomic/queued commands, UART -- ID_DAP_Invalid.
void dap_init(void);

// Fastest SWD / JTAG clock the bit-bang loops can produce, measured at boot.
uint32_t dap_swd_max_hz(void);
uint32_t dap_jtag_max_hz(void);
void dap_task(void);
