#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include "irq.h"

// PIT ports

// I/O port addresses
#define PIT_CHANNEL0 0x40       // system timer (channel 0)
#define PIT_CMD 0x43            // command register

// Data ports
#define PIT_BASE_FREQ 1193182   // oscillator port (frequency in Hz)

void timer_init(uint32_t hz);           // initialise PIT and install IRQ0 handler

uint32_t timer_get_ticks(void);         // return tick counter since initialised

void timer_handler(regs_t *r);               // IRQ0 handler

#endif