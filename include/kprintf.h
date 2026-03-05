#ifndef KPRINTF_H
#define KPRINTF_H

#include <stdarg.h>

// kernel printf - outputs to COM1 serial (UART).
// Supports: %d  %u  %x  %08x  %s  %c  %%
// %x  : hex with 0x prefix, variable width
// %08x: hex with 0x prefix, zero-padded to 8 digits (use for addresses)
void kprintf(const char *fmt, ...);

#endif
