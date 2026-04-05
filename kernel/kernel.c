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
#include "vfs.h"
#include "ramfs.h"
#include "devfs.h"
#include "proc.h"
#include "timer.h"
#include "keyboard.h"
#include "sched.h"
#include "syscall.h"
#include "pci.h"
#include "virtio_net.h"
#include "lwip_orion.h"
#include "netfs.h"
#include "procfs.h"
#include "elf.h"
#include "string.h"

#if defined(__linux__)
    #error "Must be compiled with a cross-compiler"
#elif !defined(__i386__)
    #error "Must be compiled with an x86-elf compiler"
#endif

extern uint8_t kernel_start;
extern uint8_t kernel_end;

static void idle_process(void) {
    while (1) {
        asm volatile ("hlt");
    }
}

static void pulsar_server_process(void) {

    // step 1: get a free TCP slot
    file_t *clone_f = vfs_open("/net/tcp/clone", O_RDONLY);
    if (!clone_f) {
        kprintf("PULSAR-SRV: cannot open /net/tcp/clone\n");
        return;
    }
    char slot_str[16];
    int n = vfs_read(clone_f, slot_str, sizeof(slot_str) - 1);
    vfs_close(clone_f);
    if (n <= 0) { kprintf("PULSAR-SRV: clone read failed\n"); return; }
    slot_str[n] = '\0';
    int slen = n;
    while (slen > 0 && (slot_str[slen-1] == '\n' || slot_str[slen-1] == '\r' || slot_str[slen-1] == ' '))
        slot_str[--slen] = '\0';

    kprintf("PULSAR-SRV: using TCP slot %s\n", slot_str);

    // step 2: announce *!564
    char ctl_path[VFS_PATH_MAX];
    strncpy(ctl_path, "/net/tcp/", VFS_PATH_MAX - 1);
    strncat(ctl_path, slot_str, VFS_PATH_MAX - 1 - strlen(ctl_path));
    strncat(ctl_path, "/ctl",   VFS_PATH_MAX - 1 - strlen(ctl_path));

    file_t *ctl_f = vfs_open(ctl_path, O_WRONLY);
    if (!ctl_f) { kprintf("PULSAR-SRV: cannot open %s\n", ctl_path); return; }

    const char *announce = "announce *!564";
    n = vfs_write(ctl_f, announce, strlen(announce));
    vfs_close(ctl_f);
    if (n < 0) { kprintf("PULSAR-SRV: announce failed\n"); return; }

    kprintf("PULSAR-SRV: listening on port 564\n");

    // step 3: enter serve loop (blocks forever, handles all clients)
    ctl_f = vfs_open(ctl_path, O_WRONLY);
    if (!ctl_f) { kprintf("PULSAR-SRV: cannot reopen ctl\n"); return; }

    const char *serve = "serve";
    vfs_write(ctl_f, serve, strlen(serve));  // blocks indefinitely
    vfs_close(ctl_f);
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

    vfs_init();
    ramfs_init();
    devfs_init();

    proc_init();

    syscall_init();
    idt_install_syscall();

    timer_init(100);        // 10 ms = 100hz
    keyboard_init();

    pci_init();
    virtio_net_init();
    lwip_orion_init();
    timer_register_tick_cb(lwip_orion_poll);

    netfs_init();
    procfs_init();

    pcb_t *idle = proc_create("idle", PROC_PRIO_IDLE);
    kassert(idle != 0);
    proc_init_frame(idle, (uint32_t)idle_process);
    proc_set_ready(idle);
    sched_add(idle);


    pcb_t *srv = proc_create("pulsar-srv", PROC_PRIO_NORMAL);
    kassert(srv != 0);
    proc_init_frame(srv, (uint32_t)pulsar_server_process);
    proc_set_ready(srv);
    sched_add(srv);

    // embed shell ELF (objcopy symbols) into ramfs at /bin/sh
    extern uint8_t _binary_user_sh_sh_elf_start[];
    extern uint8_t _binary_user_sh_sh_elf_end[];
    uint32_t sh_elf_size = (uint32_t)(_binary_user_sh_sh_elf_end - _binary_user_sh_sh_elf_start);

    vfs_mkdir("/bin");
    file_t *sh_file = vfs_open("/bin/sh", O_CREAT | O_WRONLY);
    kassert(sh_file != 0);
    vfs_write(sh_file, _binary_user_sh_sh_elf_start, sh_elf_size);
    vfs_close(sh_file);

    // embed top ELF into ramfs at /bin/top
    extern uint8_t _binary_user_otop_otop_elf_start[];
    extern uint8_t _binary_user_otop_otop_elf_end[];
    uint32_t otop_elf_size = (uint32_t)(_binary_user_otop_otop_elf_end - _binary_user_otop_otop_elf_start);

    file_t *otop_file = vfs_open("/bin/otop", O_CREAT | O_WRONLY);
    kassert(otop_file != 0);
    vfs_write(otop_file, _binary_user_otop_otop_elf_start, otop_elf_size);
    vfs_close(otop_file);

    // launch shell as ring-3 process
    pcb_t *sh = proc_create("orion-sh", PROC_PRIO_NORMAL);
    kassert(sh != 0);

    uint32_t entry = elf_load_into("/bin/sh", (uint32_t)sh->page_directory);
    kassert(entry != 0);

    kassert(proc_setup_user_stack(sh) == 0);
    proc_init_user_frame(sh, entry, USTACK_TOP - 8);
    proc_set_ready(sh);
    sched_add(sh);

    kprintf("OrionOS: Online\n");

    asm volatile ("sti");
    sched_start();
}