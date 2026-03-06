// fd.c - File Descriptor Layer

#include "fd.h"
#include "keyboard.h"
#include "vga.h"
#include "serial.h"
#include "kprintf.h"
#include <stddef.h>

// initialise descriptor table
void fd_table_init(fd_entry_t *table) {

    for (int i = 0; i < FD_MAX; i++) {                                      // clear table
        table[i].type  = FD_NONE;
        table[i].flags = 0;
    }

    table[0].type = FD_STDIN;                                               // install standard streams
    table[1].type = FD_STDOUT;
    table[2].type = FD_STDERR;
}

// close descriptor
void fd_close(fd_entry_t *table, int fd) {

    if (fd < 0 || fd >= FD_MAX) return;
    table[fd].type  = FD_NONE;                                              // clear entry
    table[fd].flags = 0;
}

// used for fork()
void fd_table_clone(const fd_entry_t *src, fd_entry_t *dst) {

    for (int i = 0; i < FD_MAX; i++)                                        // copy entries across
        dst[i] = src[i];
}

// allocate new descriptor
int fd_alloc(fd_entry_t *table, uint8_t type) {

    for (int i = 0; i < FD_MAX; i++) {
        if (table[i].type == FD_NONE) {                                     // search free slot
            table[i].type  = type;                                          // assign
            table[i].flags = 0;
            return i;
        }
    }
    return -1;                                                              // table full
}

// read from descriptor
int fd_read(fd_entry_t *table, int fd, void *buf, uint32_t len) {

    if (fd < 0 || fd >= FD_MAX || !buf || !len) return -1;
    if (table[fd].type == FD_NONE) return -1;

    switch (table[fd].type) {                                               // switch on descriptor type

        case FD_STDIN: {                                                    // read from keyboard
            char *out = (char *)buf;
            uint32_t n = 0;
            while (n < len) {
                if (!keyboard_has_char()) break;                            // non-blocking: return what's available
                out[n++] = keyboard_getchar();
            }
            return (int)n;
        }

        case FD_STDOUT:
        case FD_STDERR:
            return -1;                                  // write-only

        default:
            return -1;
    }
}

// write to descriptor
int fd_write(fd_entry_t *table, int fd, const void *buf, uint32_t len) {

    if (fd < 0 || fd >= FD_MAX || !buf || !len) return -1;
    if (table[fd].type == FD_NONE) return -1;

    const char *src = (const char *)buf;

    switch (table[fd].type) {

        case FD_STDOUT:                                                     // write to VGA
            for (uint32_t i = 0; i < len; i++)
                terminal_putchar(src[i]);
            return (int)len;

        case FD_STDERR:                                                     // write to serial
            for (uint32_t i = 0; i < len; i++)
                serial_putchar(src[i]);
            return (int)len;

        case FD_STDIN:
            return -1;                                  // read-only

        default:
            return -1;
    }
}