#ifndef IDT_H
#define IDT_H

#include <stdint.h>

                                    // IDT entry structure (32-bit protected mode)
struct idt_entry {
    uint16_t base_low;                  // + compute ISR address
    uint16_t selector;                    // Mode segment selector
    uint8_t  zero;              // Must be zero
    uint8_t  flags;          // 0 ring protection, 32-bit interrupt gate, present
    uint16_t base_high;                 // + compute ISR address
} __attribute__((packed));

struct idt_ptr {                //idt pointer structure
    uint16_t limit;
    uint32_t base;              //base address in the idt array
} __attribute__((packed));

                                // Called from kernel_main
void idt_init(void);

#endif


