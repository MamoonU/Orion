// kernel/kernel.c — Kernel entry point
//
// kernel_main() is called by boot.asm after the CPU is in protected mode.
// Its only job is to initialise every subsystem in dependency order, then
// enable interrupts and hand control to the scheduler (future).

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "multiboot.h"
#include "gdt.h"
#include "idt.h"
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

#if defined(__linux__)
    #error "Must be compiled with a cross-compiler"
#elif !defined(__i386__)
    #error "Must be compiled with an x86-elf compiler"
#endif

extern uint8_t kernel_start;
extern uint8_t kernel_end;

void kernel_main(uint32_t multiboot_magic, multiboot_info_t *mbi) {

    // ── Output drivers (no dependencies) ────────────────────────────────
    terminal_initialize();
    serial_init();
    kprintf("UART I/O: Online\n");

    // ── CPU tables ───────────────────────────────────────────────────────
    idt_init();
    gdt_init();

    // ── Sanity check ─────────────────────────────────────────────────────
    kassert(multiboot_magic == MULTIBOOT_MAGIC);

    // ── Memory ───────────────────────────────────────────────────────────
    pmm_init(mbi, (uint32_t)(uintptr_t)&kernel_start,
                  (uint32_t)(uintptr_t)&kernel_end);
    vmm_init();
    kheap_init();

    // ── Processes ────────────────────────────────────────────────────────
    proc_init();

    // ── Hardware drivers ─────────────────────────────────────────────────
    IRQ_init();
    timer_init(100);
    kprintf("Timer: PIT initialized at 100Hz\n");
    keyboard_init();

    // ── Enable interrupts ────────────────────────────────────────────────
    asm volatile ("sti");

    terminal_writestring("Kernel: Online");
}
