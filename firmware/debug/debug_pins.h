#pragma once

// Debug-probe pin map (debug_probe_design.md). The SPI master's pins,
// handed over by debug_session.c for the duration of a session.
#define DBG_PIN_SWCLK       8
#define DBG_PIN_SWDIO       9
#define DBG_PIN_SWO         10
#define DBG_PIN_SWD_NRESET  11

#define DBG_PIN_TCK         8
#define DBG_PIN_TDI         9
#define DBG_PIN_TDO         10
#define DBG_PIN_TMS         11
#define DBG_PIN_JTAG_NRESET 22
