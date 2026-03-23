// sys_arch.c - lwIP OS abstraction layer for Orion OS

#include "lwip/opt.h"
#include "lwip/sys.h"
#include "timer.h"
#include <stdint.h>
#include <stddef.h>

// errno
int errno = 0;

// sys_now
u32_t sys_now(void) {
    return timer_get_ticks() * 10u;             // return elapsed # ms since boot
}

// memmove
void *memmove(void *dst, const void *src, size_t n) {
    uint8_t       *d = (uint8_t *)dst;          // destination
    const uint8_t *s = (const uint8_t *)src;    // source
    if (d < s) {
        while (n--) *d++ = *s++;                // forward copy
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;                // backward copy
    }
    return dst;
}