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
#include "timer.h"

// /dev/stdin: keyboard read-only
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

// /dev/stdout: VGA terminal write-only
static int vga_write_op(vnode_t *v, const void *buf, uint32_t len, uint32_t offset) {

    (void)v;
    (void)offset;

    const char *src = (const char *)buf;
    uint32_t i = 0;
    while (i < len) {

        if (src[i] == '\033' && i + 1 < len && src[i+1] == '[') {

            uint32_t j = i + 2;
            while (j < len && src[j] >= 0x20 && src[j] <= 0x3F) j++;
            
            if (j < len) {
                char cmd = src[j];
                uint32_t seq_len = j - (i + 2);
                const char *arg = src + i + 2;
                if (cmd == 'J' && seq_len == 1 && arg[0] == '2')
                    terminal_clear();       // \033[2J — erase display
                else if (cmd == 'H' && seq_len == 0)
                    terminal_set_cursor(0, 0);  // \033[H  — cursor home
                i = j + 1;
                continue;
            }
        }
        terminal_putchar(src[i++]);
    }
    return (int)len;
}

// stdout operations table
static vfs_ops_t stdout_ops = {
    .write = vga_write_op,
};

// /dev/stderr: serial port write-only
static int serial_write_op(vnode_t *v, const void *buf, uint32_t len, uint32_t offset) {

    (void)v; (void)offset;
    const char *src = (const char *)buf;                // cast buffer -> char

    for (uint32_t i = 0; i < len; i++)                  // // output loop
        serial_putchar(src[i]);
    return (int)len;
}

// stderr operations table
static vfs_ops_t stderr_ops = {
    .write = serial_write_op,
};

// /dev/null
static int null_read(vnode_t *v, void *buf, uint32_t len, uint32_t offset) {            // return 0 = EOF
    (void)v; (void)buf; (void)len; (void)offset;
    return 0;                           // EOF
}

static int null_write(vnode_t *v, const void *buf, uint32_t len, uint32_t offset) {     // discard data silently
    (void)v; (void)buf; (void)offset;
    return (int)len;                    // fake all bytes were written
}

// null operations table
static vfs_ops_t null_ops = {
    .read  = null_read,
    .write = null_write,
};

// /dev/zero
static int zero_read(vnode_t *v, void *buf, uint32_t len, uint32_t offset) {            // fill callers buffer with zero bytes
    (void)v; (void)offset;
    uint8_t *out = (uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) out[i] = 0;
    return (int)len;
}

// /dev/zero writes = discarded

// zero operations table
static vfs_ops_t zero_ops = {
    .read  = zero_read,
    .write = null_write,                // same discard behaviour as /dev/null
};

// /dev/random
static uint64_t s_rand_state = 0;

// generator = valid start
static void rand_seed_if_needed(void) {
    if (s_rand_state == 0) {
        uint32_t t = timer_get_ticks();
        s_rand_state = ((uint64_t)t ^ 0xDEADBEEFCAFEBABEULL) | 1ULL;
    }
}

// generate = 1 random byte
static uint8_t rand_byte(void) {
    // xorshift64
    s_rand_state ^= s_rand_state << 13;
    s_rand_state ^= s_rand_state >> 7;
    s_rand_state ^= s_rand_state << 17;
    return (uint8_t)(s_rand_state & 0xFF);
}

// read("/dev/random") = random_read()
static int random_read(vnode_t *v, void *buf, uint32_t len, uint32_t offset) {
    (void)v; (void)offset;
    rand_seed_if_needed();
    uint8_t *out = (uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) out[i] = rand_byte();
    return (int)len;
}

// random operations table
static vfs_ops_t random_ops = {
    .read = random_read,
};

// vnode storage: pointers to device vnodes
static vnode_t *g_stdin  = 0;
static vnode_t *g_stdout = 0;
static vnode_t *g_stderr = 0;
static vnode_t *g_null   = 0;
static vnode_t *g_zero   = 0;
static vnode_t *g_random = 0;

// accessor functions: kernel components recieve device vnodes
vnode_t *devfs_stdin_vnode (void) { return g_stdin;  }
vnode_t *devfs_stdout_vnode(void) { return g_stdout; }
vnode_t *devfs_stderr_vnode(void) { return g_stderr; }
vnode_t *devfs_null_vnode  (void) { return g_null;   }
vnode_t *devfs_zero_vnode  (void) { return g_zero;   }
vnode_t *devfs_random_vnode(void) { return g_random; }

// initialise devfs
void devfs_init(void) {

    kprintf("DEVFS: Initialising\n");

    if (vfs_mkdir("/dev") < 0) {                                            // create /dev directory inside ramfs
        kprintf("DEVFS: FATAL — could not create /dev\n");
        return;
    }

    g_stdin  = vnode_alloc(VNODE_DEV, &stdin_ops,  0);                      // allocate device vnodes
    g_stdout = vnode_alloc(VNODE_DEV, &stdout_ops, 0);
    g_stderr = vnode_alloc(VNODE_DEV, &stderr_ops, 0);
    g_null   = vnode_alloc(VNODE_DEV, &null_ops,   0);
    g_zero   = vnode_alloc(VNODE_DEV, &zero_ops,   0);
    g_random = vnode_alloc(VNODE_DEV, &random_ops, 0);

    if (!g_stdin || !g_stdout || !g_stderr || !g_null  || !g_zero   || !g_random) {     // OOM protection
        kprintf("DEVFS: FATAL — OOM allocating device vnodes\n");
        return;
    }

    ramfs_register_dev("/dev/stdin",  g_stdin);                             // insert into /dev
    ramfs_register_dev("/dev/stdout", g_stdout);
    ramfs_register_dev("/dev/stderr", g_stderr);
    ramfs_register_dev("/dev/null",   g_null);
    ramfs_register_dev("/dev/zero",   g_zero);
    ramfs_register_dev("/dev/random", g_random);

    kprintf("DEVFS: /dev/stdin, /dev/stdout, /dev/stderr registered\n");
    kprintf("DEVFS: /dev/null, /dev/zero, /dev/random registered\n");
}