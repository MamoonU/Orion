#include "idt.h"

// External ASM functions from isr.asm
extern void isr0();                    // Divide by zero
extern void isr6();                     // Invalid opcode
extern void isr8();                     // Double fault
extern void isr13();                    // General protection fault
extern void isr14();                    // Page fault

static struct idt_entry idt[256];           // from idt.h
static struct idt_ptr idtp;

// Provided by kernel.c
extern void serial_write(const char *s);
extern void panic(const char *msg);

// Load IDT (assembly)
static inline void lidt(struct idt_ptr *idtp) {         //load idt pointer
    asm volatile ("lidt (%0)" : : "r"(idtp));
}

static void idt_set_gate(uint8_t num, uint32_t base) {          //set idt gate
    idt[num].base_low  = base & 0xFFFF;
    idt[num].selector  = 0x08;        // Kernel code segment
    idt[num].zero      = 0;
    idt[num].flags     = 0x8E;        // Present, ring 0, 32-bit interrupt gate
    idt[num].base_high = (base >> 16) & 0xFFFF;
}

void idt_init(void) {                       //initialize idt
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;

    // Clear IDT
    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, 0);
    }

    // CPU exceptions needed
    idt_set_gate(0,  (uint32_t)isr0);               // Divide by zero
    idt_set_gate(6,  (uint32_t)isr6);        // Invalid opcode
    idt_set_gate(8,  (uint32_t)isr8);    // Double fault
    idt_set_gate(13, (uint32_t)isr13);          // General protection fault
    idt_set_gate(14, (uint32_t)isr14);      // Page fault

    lidt(&idtp);
    serial_write("IDT: Loaded\n");
}

// C exception handler
void isr_handler(uint32_t int_no, uint32_t err_code) {
    serial_write("\n=== CPU EXCEPTION ===\n");

    switch (int_no) {
        case 0:  serial_write("Divide by Zero\n"); break;
        case 6:  serial_write("Invalid Opcode\n"); break;
        case 8:  serial_write("Double Fault\n"); break;
        case 13: serial_write("General Protection Fault\n"); break;
        case 14:
            serial_write("Page Fault\n");
            uint32_t fault_addr;
            asm volatile ("mov %%cr2, %0" : "=r"(fault_addr));      //cr2 = page fault address
            serial_write("Fault address captured\n");
            break;
        default:
            serial_write("Unknown CPU Exception\n");
            break;
    }

    panic("Unhandled CPU exception");
}
