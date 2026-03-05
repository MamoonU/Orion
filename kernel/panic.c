// kernel/panic.c — unrecoverable kernel error handler

#include "panic.h"
#include "serial.h"

__attribute__((noreturn))
void panic(const char *msg) {
    serial_write("\n====================\n");
    serial_write("KERNEL PANIC\n");
    serial_write(msg);
    serial_write("\nSystem halted.\n");
    serial_write("====================\n");
    asm volatile ("cli");
    for (;;) asm volatile ("hlt");
}
