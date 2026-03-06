#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "multiboot.h"
#include "gdt.h"
#include "idt.h"
#include "tss.h"
#include "irq.h"
#include "panic.h"
#include "serial.h"
#include "vga.h"
#include "kprintf.h"
#include "pmm.h"
#include "vmm.h"
#include "kheap.h"
#include "proc.h"
#include "timer.h"
#include "keyboard.h"
#include "sched.h"
#include "syscall.h"

#if defined(__linux__)
    #error "Must be compiled with a cross-compiler"
#elif !defined(__i386__)
    #error "Must be compiled with an x86-elf compiler"
#endif

extern uint8_t kernel_start;
extern uint8_t kernel_end;

static void idle_process(void) {
    while (1)
        asm volatile ("hlt");
}

void kernel_main(uint32_t multiboot_magic, multiboot_info_t *mbi) {

    terminal_init();
    serial_init();

    idt_init();
    IRQ_init();

    gdt_init();
    tss_init();

    kassert(multiboot_magic == MULTIBOOT_MAGIC);

    pmm_init(mbi, (uint32_t)(uintptr_t)&kernel_start, (uint32_t)(uintptr_t)&kernel_end);
    vmm_init();
    kheap_init();

    proc_init();

    syscall_init();
    idt_install_syscall();

    timer_init(100);
    kprintf("Timer: PIT initialized at 100Hz\n");
    keyboard_init();

    // idle process: runs when nothing else is ready
    pcb_t *idle = proc_create("idle", PROC_PRIO_IDLE);
    kassert(idle != 0);
    proc_init_frame(idle, (uint32_t)idle_process);
    proc_set_ready(idle);
    sched_add(idle);

    terminal_writestring("OrionOS: Online");

    asm volatile ("sti");
    sched_start();
    
}
