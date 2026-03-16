// printf.c - liborion formatted output

#include "orion.h"
#include <stdarg.h>

// internal: write a uint to a buffer
static int uint_to_buf(char *out, uint32_t val, uint32_t base, int width, char pad, int uppercase) {

    char tmp[32];
    int  i = 30;
    tmp[31] = '\0';

    const char *dig_lo = "0123456789abcdef";
    const char *dig_up = "0123456789ABCDEF";
    const char *digits = uppercase ? dig_up : dig_lo;

    if (val == 0) {
        tmp[i--] = '0';
    } else {
        while (val > 0) {
            tmp[i--] = digits[val % base];
            val /= base;
        }
    }

    int len = 30 - i;
    int written = 0;

    while (len < width) {
        *out++ = pad;
        written++;
        width--;
    }

    const char *src = &tmp[i + 1];
    while (*src) { *out++ = *src++; written++; }
    return written;
}

// core formatter: write into fixed buffer
static int vformat(char *buf, uint32_t bufsz, const char *fmt, va_list args) {

    uint32_t pos = 0;

    #define EMIT(c) do { if (pos + 1 < bufsz) { buf[pos++] = (c); } } while(0)

    for (const char *p = fmt; *p; p++) {

        if (*p != '%') { EMIT(*p); continue; }
        p++;

        char pad   = ' ';
        int  width = 0;

        if (*p == '0') { pad = '0'; p++; }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        switch (*p) {

            case 'c':
                EMIT((char)va_arg(args, int));
                break;

            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                while (*s) EMIT(*s++);
                break;
            }

            case 'd': {
                int32_t v = va_arg(args, int32_t);
                if (v < 0) { EMIT('-'); v = -v; }
                char tmp[24]; int n;
                n = uint_to_buf(tmp, (uint32_t)v, 10, width, pad, 0);
                for (int i = 0; i < n; i++) EMIT(tmp[i]);
                break;
            }

            case 'u': {
                char tmp[24]; int n;
                n = uint_to_buf(tmp, va_arg(args, uint32_t),
                                10, width, pad, 0);
                for (int i = 0; i < n; i++) EMIT(tmp[i]);
                break;
            }

            case 'x': case 'X': {
                char tmp[24]; int n;
                n = uint_to_buf(tmp, va_arg(args, uint32_t),
                                16, width, pad, (*p == 'X'));
                for (int i = 0; i < n; i++) EMIT(tmp[i]);
                break;
            }

            case 'p': {
                EMIT('0'); EMIT('x');
                char tmp[24]; int n;
                n = uint_to_buf(tmp, va_arg(args, uint32_t),
                                16, 8, '0', 0);
                for (int i = 0; i < n; i++) EMIT(tmp[i]);
                break;
            }

            case '%':
                EMIT('%');
                break;

            default:
                EMIT('%');
                EMIT(*p);
                break;
        }
    }
    buf[pos] = '\0';
    return (int)pos;

    #undef EMIT
}

// write character
int putchar(int c) {
    char ch = (char)c;
    write(STDOUT_FILENO, &ch, 1);
    return c;
}

// write string + newline
int puts(const char *s) {
    int n = (int)strlen(s);
    write(STDOUT_FILENO, s, (uint32_t)n);
    write(STDOUT_FILENO, "\n", 1);
    return n + 1;
}

// formatted output
int printf(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int n = vformat(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n > 0) write(STDOUT_FILENO, buf, (uint32_t)n);
    return n;
}

// formatted output -> buffer
int sprintf(char *buf, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vformat(buf, 0x7FFFFFFF, fmt, args);
    va_end(args);
    return n;
}
