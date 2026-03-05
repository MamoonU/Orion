// libk/kprintf.c - Kernel formatted output

#include "kprintf.h"
#include "serial.h"
#include <stdint.h>
#include <stddef.h>

// number printer
static void write_uint_base(uint32_t value, uint32_t base, int width, char pad) {

    char    buf[32];
    int     i = 30;
    buf[31] = '\0';

    const char *digits = "0123456789abcdef";

    if (value == 0) {
        buf[i--] = '0';
    } else {
        while (value > 0) {
            buf[i--] = digits[value % base];
            value   /= base;
        }
    }

    int len = 30 - i;

    while (len < width) {
        serial_putchar(pad);
        len++;
    }

    serial_write(&buf[i + 1]);
}

void kprintf(const char *fmt, ...) {

    va_list args;
    va_start(args, fmt);

    for (const char *p = fmt; *p != '\0'; p++) {

        if (*p != '%') {
            serial_putchar(*p);
            continue;
        }

        p++; // consume '%'

        char pad   = ' ';
        int  width = 0;

        if (*p == '0') {
            pad = '0';
            p++;
        }

        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        switch (*p) {

            case 'c':
                serial_putchar((char)va_arg(args, int));
                break;

            case 's': {
                const char *s = va_arg(args, const char *);
                serial_write(s ? s : "(null)");
                break;
            }

            case 'd': {
                int32_t v = va_arg(args, int32_t);
                if (v < 0) {
                    serial_putchar('-');
                    v = -v;
                }
                write_uint_base((uint32_t)v, 10, width, pad);
                break;
            }

            case 'u':
                write_uint_base(va_arg(args, uint32_t), 10, width, pad);
                break;

            case 'x': {
                uint32_t v = va_arg(args, uint32_t);
                serial_write("0x");
                write_uint_base(v, 16, width, pad);
                break;
            }

            case 'p': {
                uint32_t v = va_arg(args, uint32_t);
                serial_write("0x");
                write_uint_base(v, 16, 8, '0');
                break;
            }

            case '%':
                serial_putchar('%');
                break;

            default:
                serial_putchar('%');
                serial_putchar(*p);
                break;
        }
    }

    va_end(args);
}
