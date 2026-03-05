#ifndef GDT_H
#define GDT_H

#include <stdint.h>

struct gdt_entry {          // descriptor format described in 8 bytes

    uint16_t limit_low;         // segment limit (limit = 0xFFFFF = 4GB)
    uint16_t base_low;          // 0-15 bits
    uint8_t  base_mid;          // 16-23 bits

    uint8_t  access;            // access byte
    // |      7      |   6   |   5    |      4      |   3   |          2           |     1      |    0     |
    // | present bit | privilege ring | system data | excec | direction/conforming | read/write | accessed |

    uint8_t  granularity;       // 7-4 = flags, 3-0 = limit bits 16-19
    // |        7        |       6        |        5        |    4     |  3  |  2  |  1  |  0  |
    // | 4KB granularity | 32-bit segment | 64-bit (unused) | free bit |   limit bits 16-19    |

    uint8_t  base_high;         // 24-31 bits

    // base = base_low | base_mid<<16 | base_high<<24

} __attribute__((packed));

struct gdt_ptr {

    uint16_t limit;         // size of gtd in bytes (-1)
    uint32_t base;          // first gdt entry address

} __attribute__((packed));

void gdt_init(void);

#endif