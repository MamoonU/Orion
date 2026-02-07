#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

typedef struct regs {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
} regs_t;

typedef void (*irq_handler_t)(regs_t *);

void irq_install_handler(int irq, irq_handler_t handler);
void irq_uninstall_handler(int irq);

void irq_handler(regs_t *r);
void pic_remap(void);

#endif