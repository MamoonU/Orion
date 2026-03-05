// drivers/serial.c — UART COM1 serial driver
//
// Used for all kernel debug output. At 115200 baud, COM1 is fast
// enough that serial_write() does not meaningfully stall the kernel.

#include "serial.h"
#include "ioport.h"

#define COM1_PORT 0x3F8

void serial_init(void) {
    outb(COM1_PORT + 1, 0x00);  // disable all interrupts
    outb(COM1_PORT + 3, 0x80);  // enable DLAB (baud rate divisor mode)
    outb(COM1_PORT + 0, 0x01);  // divisor low  byte: 1 → 115200 baud
    outb(COM1_PORT + 1, 0x00);  // divisor high byte
    outb(COM1_PORT + 3, 0x03);  // 8 bits, no parity, one stop bit
    outb(COM1_PORT + 2, 0xC7);  // enable FIFO, clear, 14-byte threshold
    outb(COM1_PORT + 4, 0x0B);  // IRQs enabled, RTS/DSR set
}

static int serial_tx_empty(void) {
    return inb(COM1_PORT + 5) & 0x20;
}

void serial_putchar(char c) {
    while (!serial_tx_empty());
    outb(COM1_PORT, c);
}

void serial_write(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putchar('\r');
        serial_putchar(*s++);
    }
}
