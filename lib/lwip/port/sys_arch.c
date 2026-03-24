// sys_arch.c - lwIP OS abstraction layer for Orion OS

#include "lwip/opt.h"
#include "lwip/sys.h"
#include "timer.h"
#include <stdint.h>
#include <stddef.h>

unsigned int lwip_port_rand(void);

// errno
int errno = 0;

// sys_now
u32_t sys_now(void) {
    return timer_get_ticks() * 10u;             // return elapsed # ms since boot
}

// LWIP_RAND() implementation - simple LCG PRNG
// Not cryptographic: sufficient for DNS transaction ID randomisation
unsigned int lwip_port_rand(void) {
    static u32_t seed = 0;
    if (!seed) seed = timer_get_ticks() ^ 0xDEADBEEFu;
    seed = seed * 1664525u + 1013904223u;       // Knuth LCG
    return seed;
}
