// kernel/arch/x86/tss.c - Task State Segment initialisation

// wires into the system:
//   1. tss_init()         - called from kernel_main, after gdt_init()
//   2. gdt_install_tss()  - writes a TSS descriptor into GDT[5]
//   3. tss_flush()        - executes "ltr 0x28" to load the Task Register
//   4. tss_set_esp0()     - called by the scheduler on every context switch
//                           to keep TSS.esp0 pointing at the new process's
//                           kernel stack top

#include "tss.h"
#include "gdt.h"
#include "kprintf.h"

extern void tss_flush(void);        // gdt.asm

static tss_entry_t tss;             // one global TSS instance (all zeroed at boot)

void tss_init(void) {

    kprintf("TSS: Initialising\n");

    for (uint32_t i = 0; i < sizeof(tss_entry_t); i++) ((uint8_t *)&tss)[i] = 0;    // zero every field - unused hardware fields must be 0

    tss.ss0  = 0x10;                                                                // kernel data segment selector
    tss.esp0 = 0;                                                                   // filled in by tss_set_esp0() before first process runs

    tss.iomap_base = (uint16_t)sizeof(tss_entry_t);                                 // point iomap_base past end of TSS -> deny all I/O from ring-3

    gdt_install_tss((uint32_t)&tss, (uint32_t)(sizeof(tss_entry_t) - 1));           // install TSS descriptor into GDT[5] and re-flush GDTR

    tss_flush();                                                                    // load the task register so the CPU knows which GDT entry describes the TSS

    kprintf("TSS: Loaded (selector=0x28, base=0x%p, size=%u)\n", (uint32_t)&tss, (uint32_t)sizeof(tss_entry_t));
}

// Update TSS.esp0 - called by the scheduler on every context switch
void tss_set_esp0(uint32_t esp0) {
    tss.esp0 = esp0;
}