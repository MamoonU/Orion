// syscall.c - system call dispatcher

#include "syscall.h"
#include "proc.h"
#include "sched.h"
#include "vga.h"
#include "kprintf.h"
#include "vmm.h"
#include "fd.h"

// user pointer validation
static int syscall_validate_ptr(const void *ptr, uint32_t len) {

    if (!ptr || !len) return 0;

    pcb_t *p = sched_current();
    if (!p || !p->page_directory) return 0;

    // user_only=0: kernel-mode processes dont have VMM_USER set yet
    return vmm_range_mapped(p->page_directory, (uint32_t)ptr, len, 0);      // return 1 if the range (ptr, ptr+len) is fully mapped in the calling
}

// SYS_YIELD (0): voluntarily give up the CPU
static int32_t sys_yield(regs_t *r) {
    (void)r;
    return 0;
}

// SYS_EXIT (1): terminate the calling process
static int32_t sys_exit(regs_t *r) {
    int32_t code = (int32_t)r->ebx;
    proc_exit(code);
    return 0;
}

// SYS_GETPID (2): return calling process PID
static int32_t sys_getpid(regs_t *r) {
    (void)r;
    pcb_t *p = sched_current();
    return p ? (int32_t)p->pid : -1;
}

// SYS_SLEEP (3): sleep for N timer ticks
static int32_t sys_sleep(regs_t *r) {
    uint32_t ticks = r->ebx;
    proc_sleep(ticks);
    return 0;
}

// SYS_FORK (4): spawn a child process
static int32_t sys_fork(regs_t *r) {
    uint32_t entry = r->ebx;
    if (!entry) return -1;
    pid_t child = proc_fork(entry);
    return (child == PID_INVALID) ? -1 : (int32_t)child;
}

// SYS_EXEC (5): replace a process's entry point
static int32_t sys_exec(regs_t *r) {
    pid_t    pid   = (pid_t)r->ebx;
    uint32_t entry = r->ecx;
    pcb_t   *p     = proc_get(pid);
    if (!p || !entry) return -1;
    return proc_exec(p, entry);
}

// SYS_WRITE (6): write len bytes from buf -> fd
static int32_t sys_write(regs_t *r) {
    int      fd  = (int)r->ebx;                                                 // extract args
    void    *buf = (void *)r->ecx;
    uint32_t len = r->edx;

    if (!syscall_validate_ptr(buf, len)) return -1;                             // validate buffer

    pcb_t *p = sched_current();                                                 // return current process
    if (!p) return -1;

    return fd_write(p->fd_table, fd, buf, len);                                 // write to fd
}

// SYS_READ (7): read  len bytes from fd -> buf
static int32_t sys_read(regs_t *r) {
    int      fd  = (int)r->ebx;                                                 // extract args
    void    *buf = (void *)r->ecx;
    uint32_t len = r->edx;

    if (!syscall_validate_ptr(buf, len)) return -1;                             // validate buffer

    pcb_t *p = sched_current();                                                 // return current process
    if (!p) return -1;

    return fd_read(p->fd_table, fd, buf, len);                                  // read to fd
}

// SYS_OPEN (8): open a file by path
static int32_t sys_open(regs_t *r) {
    (void)r;
    kprintf("SYSCALL: sys_open - not yet implemented (needs VFS)\n");
    return -1;
}

// SYS_CLOSE (9): close a file descriptor
static int32_t sys_close(regs_t *r) {
    int fd = (int)r->ebx;                                                       // read fd

    if (fd < 0 || fd >= FD_MAX) return -1;

    pcb_t *p = sched_current();                                                 // return current process
    if (!p) return -1;

    if (p->fd_table[fd].type == FD_NONE) return -1;                             // already closed

    fd_close(p->fd_table, fd);                                                  // close fd
    return 0;
}

typedef int32_t (*syscall_fn_t)(regs_t *);

// define dispatch table
static syscall_fn_t syscall_table[SYSCALL_COUNT] = {
    [SYS_YIELD]  = sys_yield,
    [SYS_EXIT]   = sys_exit,
    [SYS_GETPID] = sys_getpid,
    [SYS_SLEEP]  = sys_sleep,
    [SYS_FORK]   = sys_fork,
    [SYS_EXEC]   = sys_exec,
    [SYS_WRITE]  = sys_write,
    [SYS_READ]   = sys_read,
    [SYS_OPEN]   = sys_open,
    [SYS_CLOSE]  = sys_close,
};

void syscall_dispatch(regs_t *r) {

    uint32_t n = r->eax;                                                                // read syscall number

    if (n >= SYSCALL_COUNT || !syscall_table[n]) {                                      // validate syscall
        kprintf("SYSCALL: unknown syscall %u from PID %u\n",
                n, sched_current() ? (uint32_t)sched_current()->pid : 0u);
        r->eax = (uint32_t)-1;
        return;
    }

    int32_t ret = syscall_table[n](r);                                                  // call handler
    r->eax = (uint32_t)ret;                                                             // write return value back into the saved frame
}

extern void syscall_entry(void);                                                        // syscall.asm

void syscall_init(void) {
    kprintf("SYSCALL: Dispatcher ready (%u syscalls)\n", (uint32_t)SYSCALL_COUNT);
}