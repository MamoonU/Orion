#include "irq.h"

extern void isr32();                    // Timer interrupt
extern void isr33();                    // Keyboard interrupt



void pic_remap() {						//Remap PIC to avoid conflicts with CPU exceptions
    outb(0x20, 0x11); // Start initialization of master PIC
    outb(0xA0, 0x11); // Start initialization of slave PIC

}