#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include "irq.h"

// PS/2 controller ports
#define KB_DATA_PORT    0x60    // read scancodes & write commands to keyboard
#define KB_STATUS_PORT  0x64    // status register (read) | command register (write)

// keyboard buffer size (power of 2)
#define KB_BUFFER_SIZE  64      // ringbuffer -> index = (index + 1) & (SIZE - 1);

void keyboard_init(void);           

void keyboard_handler(regs_t *r);

char keyboard_getchar(void);

int keyboard_has_char(void);

#endif