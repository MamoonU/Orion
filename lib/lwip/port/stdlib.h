// stdlib.h - minimal shim for lwIP port

#ifndef LWIP_PORT_STDLIB_H
#define LWIP_PORT_STDLIB_H

#include <stddef.h>

// ascii -> integer : already implemented in lib/libk/string.c
static inline int atoi(const char *s) {
    int n = 0, sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return sign * n;
}

#endif