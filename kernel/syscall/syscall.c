// syscall.c - system call dispatcher

#include "syscall.h"
#include "proc.h"
#include "sched.h"
#include "vga.h"
#include "kprintf.h"
#include "vmm.h"
#include "fd.h"
#include "vfs.h"
#include "pipe.h"
#include "elf.h"
#include "string.h"

// user pointer validation
static int syscall_validate_ptr(const void *ptr, uint32_t len) {

    if (!ptr || !len) return 0;

    pcb_t *p = sched_current();
    if (!p) return 0;
    if (!p->page_directory) return 1;                                       // kernel process: implicitly trusted

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
    const char *path  = (const char *)r->ebx;
    int         flags = (int)r->ecx;

    if (!syscall_validate_ptr(path, 1)) return -1;

    pcb_t *p = sched_current();
    if (!p) return -1;

    file_t *f = vfs_open(path, flags);
    if (!f) return -1;

    int fd = fd_install(p->fd_table, f);
    if (fd < 0) { vfs_close(f); return -1; }
    return fd;
}

// SYS_CLOSE (9): close a file descriptor
static int32_t sys_close(regs_t *r) {
    int fd = (int)r->ebx;                                                       // read fd

    pcb_t *p = sched_current();                                                 // return current process
    if (!p || fd < 0 || fd >= FD_MAX) return -1;
    if (!p->fd_table[fd]) return -1;                                            // already closed

    fd_close(p->fd_table, fd);                                                  // close fd
    return 0;
}

// SYS_PIPE (10): create an anonymous pipe
static int32_t sys_pipe(regs_t *r) {
    int *pipefd = (int *)r->ebx;

    if (!syscall_validate_ptr(pipefd, 2 * sizeof(int))) return -1;

    pcb_t *p = sched_current();
    if (!p) return -1;

    return pipe_create(p->fd_table, pipefd);
}

// SYS_DUP2 (11): duplicate oldfd onto newfd

static int32_t sys_dup2(regs_t *r) {                            // closes newfd if open -> install oldfd's file_t at newfd
    int oldfd = (int)r->ebx;
    int newfd = (int)r->ecx;

    pcb_t *p = sched_current();
    if (!p) return -1;
    if (oldfd < 0 || oldfd >= FD_MAX) return -1;
    if (newfd < 0 || newfd >= FD_MAX) return -1;
    if (!p->fd_table[oldfd]) return -1;

    if (oldfd == newfd) return newfd;                           // POSIX: dup2 to self is a no-op

    if (p->fd_table[newfd]) fd_close(p->fd_table, newfd);       // close newfd if currently open

    p->fd_table[newfd] = p->fd_table[oldfd];                    // share the file_t
    p->fd_table[newfd]->refcount++;                             // bump refcount 

    return newfd;
}

// SYS_EXECVE (12): replace the calling process's image with ELF binary
static int32_t sys_execve(regs_t *r) {
    const char *path = (const char *)r->ebx;

    if (!syscall_validate_ptr(path, 1)) return -1;

    uint32_t entry = elf_load(path);
    if (!entry) {
        kprintf("EXECVE: failed to load \"%s\"\n", path);
        return -1;
    }

    pcb_t *p = sched_current();
    if (!p) return -1;

    kprintf("EXECVE: [%u] \"%s\" -> ELF entry 0x%p\n",
            (uint32_t)p->pid, path, entry);

    // Redirect the iret target to the new binary's entry point.
    // Clear all general-purpose registers so the new image starts clean.
    r->eip       = entry;
    r->eax       = 0;
    r->ebx       = 0;
    r->ecx       = 0;
    r->edx       = 0;
    r->esi       = 0;
    r->edi       = 0;
    r->ebp       = 0;
    r->eflags    = 0x00000202u;

    // Reset scheduler accounting so the new image gets a full timeslice
    p->timeslice       = p->timeslice_len;
    p->ticks_total     = 0;
    p->ticks_scheduled = 0;

    return 0;
}

// SYS_WAIT (13)
static int32_t sys_wait(regs_t *r) {
    pid_t    pid      = (pid_t)(int32_t)r->ebx;
    int32_t *out_code = (int32_t *)r->ecx;
    if (out_code && !syscall_validate_ptr(out_code, sizeof(int32_t))) return -1;
    pid_t result = proc_wait(pid, out_code);
    return (result == PID_INVALID) ? -1 : (int32_t)result;
}

// SYS_CHDIR (14): change calling process's working directory
static int32_t sys_chdir(regs_t *r) {
 
    const char *path = (const char *)r->ebx;
    if (!syscall_validate_ptr(path, 1)) return -1;
 
    pcb_t *p = sched_current();
    if (!p) return -1;
 
    char resolved[VFS_PATH_MAX];
    vfs_path_resolve(p->cwd_path, path, resolved);     // handles relative + absolute + normalisation
 
    vnode_t *v = vfs_resolve(resolved);
    if (!v || v->type != VNODE_DIR) return -1;          // must resolve to a real directory
 
    strncpy(p->cwd_path, resolved, VFS_PATH_MAX - 1);
    p->cwd_path[VFS_PATH_MAX - 1] = '\0';
    return 0;
}

// SYS_GETCWD (15): copy cwd string into caller-supplied buffer
static int32_t sys_getcwd(regs_t *r) {
 
    char    *buf = (char *)r->ebx;
    uint32_t len = r->ecx;
 
    if (!syscall_validate_ptr(buf, len) || len == 0) return -1;
 
    pcb_t *p = sched_current();
    if (!p) return -1;
 
    strncpy(buf, p->cwd_path, len - 1);
    buf[len - 1] = '\0';
    return 0;
}
 
// SYS_READDIR (16): read one directory entry by index from an open dir fd
static int32_t sys_readdir(regs_t *r) {
 
    int      fd       = (int)r->ebx;
    uint32_t index    = r->ecx;
    char    *name_buf = (char *)r->edx;
    uint32_t buflen   = r->esi;
 
    if (!syscall_validate_ptr(name_buf, buflen) || buflen == 0) return -1;
 
    pcb_t *p = sched_current();
    if (!p) return -1;
 
    file_t *f = (fd >= 0 && fd < FD_MAX) ? p->fd_table[fd] : 0;
    if (!f) return -1;
 
    return vfs_readdir(f, index, name_buf, buflen, 0);
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
    [SYS_PIPE]   = sys_pipe,
    [SYS_DUP2]   = sys_dup2,
    [SYS_EXECVE] = sys_execve,
    [SYS_WAIT]   = sys_wait,
    [SYS_CHDIR]   = sys_chdir,
    [SYS_GETCWD]  = sys_getcwd,
    [SYS_READDIR] = sys_readdir,
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