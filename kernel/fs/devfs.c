// devfs.c - Device Node Registration

#include "devfs.h"
#include "ramfs.h"
#include "vfs.h"
#include "keyboard.h"
#include "vga.h"
#include "serial.h"
#include "kprintf.h"
#include "kheap.h"
#include "proc.h"
#include "sched.h"

// stdin: keyboard read-only
static int kbd_read(vnode_t *v, void *buf, uint32_t len, uint32_t offset) {

    (void)v; (void)offset;                              // ignore vnode & offset
    char    *out = (char *)buf;                         // output buffer
    uint32_t n   = 0;                                   // character counter

    // block until at least one character is available
    while (!keyboard_has_char()) {

        pcb_t *self = sched_current();
        if (!self) return 0;

        keyboard_set_waiter((uint16_t)self->pid);       // register waiter before disabling interrupts
        asm volatile("cli");

        if (keyboard_has_char()) {
            keyboard_set_waiter(0xFFFF);
            asm volatile("sti");
            break;
        }

        self->state = PROC_BLOCKED;
        sched_remove(self);

        keyboard_set_waiter((uint16_t)self->pid);       // store PID so the IRQ handler can wake
        sched_yield();
    }
    while (n < len && keyboard_has_char())              // read from kb buffer
        out[n++] = keyboard_getchar();
    return (int)n;
}

// stdin vnode operations table
static vfs_ops_t stdin_ops = {
    .read = kbd_read,
};

// stdout: VGA terminal write-only
static int vga_write_op(vnode_t *v, const void *buf, uint32_t len, uint32_t offset) {

    (void)v; (void)offset;
    const char *src = (const char *)buf;                // cast buffer -> char

    for (uint32_t i = 0; i < len; i++)                  // output loop
        terminal_putchar(src[i]);
    return (int)len;
}

//stdout operations table
static vfs_ops_t stdout_ops = {
    .write = vga_write_op,
};

// stderr: serial port write-only
static int serial_write_op(vnode_t *v, const void *buf, uint32_t len, uint32_t offset) {

    (void)v; (void)offset;
    const char *src = (const char *)buf;                // cast buffer -> char

    for (uint32_t i = 0; i < len; i++)                  // // output loop
        serial_putchar(src[i]);
    return (int)len;
}

//stderr operations table
static vfs_ops_t stderr_ops = {
    .write = serial_write_op,
};

// vnode storage: pointers to device vnodes
static vnode_t *g_stdin  = 0;
static vnode_t *g_stdout = 0;
static vnode_t *g_stderr = 0;

// accessor functions: kernel components recieve device vnodes
vnode_t *devfs_stdin_vnode (void) { return g_stdin;  }
vnode_t *devfs_stdout_vnode(void) { return g_stdout; }
vnode_t *devfs_stderr_vnode(void) { return g_stderr; }

// initialise devfs
void devfs_init(void) {

    kprintf("DEVFS: Initialising\n");

    if (vfs_mkdir("/dev") < 0) {                                            // create /dev directory inside ramfs
        kprintf("DEVFS: FATAL — could not create /dev\n");
        return;
    }

    g_stdin  = vnode_alloc(VNODE_DEV, &stdin_ops,  0);                      // allocate the three device vnodes
    g_stdout = vnode_alloc(VNODE_DEV, &stdout_ops, 0);
    g_stderr = vnode_alloc(VNODE_DEV, &stderr_ops, 0);

    if (!g_stdin || !g_stdout || !g_stderr) {                               // OOM protection
        kprintf("DEVFS: FATAL — OOM allocating device vnodes\n");
        return;
    }

    ramfs_register_dev("/dev/stdin",  g_stdin);                             // insert into /dev
    ramfs_register_dev("/dev/stdout", g_stdout);
    ramfs_register_dev("/dev/stderr", g_stderr);

    kprintf("DEVFS: /dev/stdin, /dev/stdout, /dev/stderr registered\n");
}