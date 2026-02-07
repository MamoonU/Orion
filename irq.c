#include "irq.h"

extern void isr32();                    // Timer interrupt
extern void isr33();                    // Keyboard interrupt

uint8_t master_mask = inb(0x21);
uint8_t slave_mask  = inb(0xA1);

static irq_handler_t irq_handlers[16] = {0};  //IRQ handler table

// IRQ handler called from assembly ISR stubs
void irq_install_handler(int irq, irq_handler_t handler) {
    if (irq < 0 || irq > 15) return;
    irq_handlers[irq] = handler;
}

// Uninstall IRQ handler
void irq_uninstall_handler(int irq) {
    if (irq < 0 || irq > 15) return;
    irq_handlers[irq] = 0;
}


void PIC_remap() {						//Remap PIC to avoid conflicts with CPU exceptions

    outb(0x20, 0x11); // Start initialization of master PIC
    outb(0xA0, 0x11); // Start initialization of slave PIC

    outb(0x21, 0x20); // Remap master PIC to 0x20-0x27
    outb(0xA1, 0x28); // Remap slave PIC to 0x28-0x2F

    outb(0x21, 0x04); // Tell master PIC that there is a slave PIC at IRQ2 (0000 0100)
    outb(0xA1, 0x02); // Tell slave PIC its cascade identity (0000 0010)

    outb(0x21, 0x01); // Set master PIC to 8086 mode
    outb(0xA1, 0x01); // Set slave PIC to 8086 mode

    outb(0x21, master_mask); // Restore master PIC mask
    outb(0xA1, slave_mask);  // Restore slave PIC mask
}


void PIC_EOI(uint8_t irq) {

    if (irq >= 8) {
        outb(0xA0, 0x20); // Send EOI to slave PIC
    }
    outb(0x20, 0x20); // Send EOI to master PIC

}