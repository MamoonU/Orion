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
#include "namespace.h"
#include "pulsar.h"

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

    uint32_t current_pd = (uint32_t)p->page_directory; // reuse existing PD
    return proc_exec(p, current_pd, entry); // ✅ now matches new signature
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

    char resolved[VFS_PATH_MAX];
    vfs_path_resolve(p->cwd_path, path, resolved);

    vnode_t *v = ns_resolve(p->namespace, resolved);  // namespace-aware
    if (!v) {

        if (!(flags & O_CREAT)) return -1;

        file_t *f = vfs_open(resolved, flags);          // creation path - fall through to vfs_open which handles O_CREAT
        if (!f) return -1;

        strncpy(f->path, resolved, VFS_PATH_MAX - 1);   // patch path onto created file
        f->path[VFS_PATH_MAX - 1] = '\0';

        int fd = fd_install(p->fd_table, f);
        if (fd < 0) { vfs_close(f); return -1; }
        return fd;
    }

    file_t *f = vfs_open_vnode_at(v, flags, resolved);
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
    const char  *path      = (const char *)r->ebx;
    char       **user_argv = (char **)r->ecx;

    if (!syscall_validate_ptr(path, 1)) return -1;

    pcb_t *p = sched_current();
    if (!p) return -1;

    kprintf("EXECVE: [%u] \"%s\" loading \"%s\"\n", (uint32_t)p->pid, p->name, path);

    // copy path and argv to kernel stack BEFORE destroying address space
    char kpath[256];
    strncpy(kpath, path, sizeof(kpath) - 1);
    kpath[sizeof(kpath) - 1] = '\0';

    #define KARGV_MAX 16
    #define KARG_LEN  128
    char kargs[KARGV_MAX][KARG_LEN];
    int  kargc = 0;

    if (user_argv && syscall_validate_ptr(user_argv, sizeof(char *))) {
        while (kargc < KARGV_MAX) {
            char *uarg = user_argv[kargc];
            if (!uarg || !syscall_validate_ptr(uarg, 1)) break;
            strncpy(kargs[kargc], uarg, KARG_LEN - 1);
            kargs[kargc][KARG_LEN - 1] = '\0';
            kargc++;
        }
    }

    // 1. create a new page directory for the process
    uint32_t new_pd = vmm_create_address_space();
    if (!new_pd) {
        kprintf("EXECVE: OOM creating address space\n");
        return -1;
    }

    // 2. load ELF into the new PD
    uint32_t entry = elf_load_into(kpath, new_pd);
    if (!entry) {
        kprintf("EXECVE: failed to load \"%s\"\n", kpath);
        vmm_destroy_address_space(new_pd);
        return -1;
    }

    // 3. call proc_exec() with preloaded PD and entry
    if (proc_exec(p, new_pd, entry) != 0) {
        // failed to set up stack inside proc_exec
        return -1;
    }

    // patch iret frame
    r->eip    = entry;
    r->cs     = 0x1Bu;
    r->eflags = 0x00000202u;

    // activate new PD — user stack pages now accessible via CR3
    vmm_switch(new_pd);

    // write argv strings from high to low
    uint32_t usp = USTACK_TOP;
    uint32_t uarg_ptrs[KARGV_MAX + 1];
    for (int i = kargc - 1; i >= 0; i--) {
        uint32_t slen = (uint32_t)strlen(kargs[i]) + 1;
        usp -= slen;
        usp &= ~3u;
        memcpy((void *)usp, kargs[i], slen);
        uarg_ptrs[i] = usp;
    }
    uarg_ptrs[kargc] = 0;

    // write argv pointer array
    usp -= (uint32_t)(sizeof(uint32_t) * ((uint32_t)kargc + 1));
    usp &= ~3u;
    uint32_t argv_va = usp;
    memcpy((void *)usp, uarg_ptrs, sizeof(uint32_t) * ((uint32_t)kargc + 1));

    // write argc and argv pointer as first two stack words
    usp -= 8;
    uint32_t *frame = (uint32_t *)usp;
    frame[0] = (uint32_t)kargc;
    frame[1] = argv_va;

    r->useresp = usp;
    proc_init_user_frame(p, entry, usp);
    r->ss      = 0x23u;
    r->ds = r->es = r->fs = r->gs = 0x23u;
    r->eax = r->ebx = r->ecx = r->edx = r->esi = r->edi = r->ebp = 0;

    for (int i = 0; i < NSIG; i++) p->signal_handlers[i] = 0;
    p->signal_trampoline = 0;
    p->in_signal = p->pending_signals = p->signal_mask = 0;
    p->heap_top = UHEAP_START;
    p->timeslice = p->timeslice_len;
    p->ticks_total = p->ticks_scheduled = 0;

    kprintf("EXECVE: [%u] \"%s\" -> ring-3 entry=0x%p  stack=0x%p  argc=%d\n", (uint32_t)p->pid, kpath, entry, usp, kargc);
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

    vnode_t *v = f->vnode;
    if (!v || v->type != VNODE_DIR) return -1;

    // if file has a recorded path, use namespace-aware readdir
    if (f->path[0] != '\0' && p->namespace) {
        vnode_t *child = 0;
        return ns_readdir(p->namespace, f->path, index, name_buf, buflen, &child);
    }
 
    return vfs_readdir(f, index, name_buf, buflen, 0);
}

// SYS_BIND (17)
static int32_t sys_bind(regs_t *r) {

    const char *src_path = (const char *)r->ebx;
    const char *new_path = (const char *)r->ecx;
    uint8_t     flags    = (uint8_t)r->edx;

    if (!syscall_validate_ptr(src_path, 1)) return -1;
    if (!syscall_validate_ptr(new_path, 1)) return -1;
    if (flags > NS_BIND_AFTER) return -1;

    pcb_t *p = sched_current();
    if (!p) return -1;

    char src_resolved[VFS_PATH_MAX];
    char dst_resolved[VFS_PATH_MAX];

    vfs_path_resolve(p->cwd_path, src_path, src_resolved);
    vfs_path_resolve(p->cwd_path, new_path, dst_resolved);

    vnode_t *v = ns_resolve(p->namespace, src_resolved);
    if (!v) return -1;
    return ns_bind(&p->namespace, v, dst_resolved, flags);
}

// SYS_UNBIND (18)
static int32_t sys_unbind(regs_t *r) {

    const char *new_path = (const char *)r->ebx;
    if (!syscall_validate_ptr(new_path, 1)) return -1;

    pcb_t *p = sched_current();
    if (!p) return -1;

    char resolved[VFS_PATH_MAX];

    vfs_path_resolve(p->cwd_path, new_path, resolved);
    return ns_unbind(&p->namespace, resolved);
}

// SYS_NSDUMP (19)
static int32_t sys_nsdump(regs_t *r) {

    (void)r;
    pcb_t *p = sched_current();
    if (!p) return -1;

    ns_dump(p->namespace);
    return 0;
}

// SYS_MOUNT (20): create a PULSAR session over srv_fd and mount at path
static int32_t sys_mount(regs_t *r) {

    int         srv_fd   = (int)r->ebx;
    const char *path     = (const char *)r->ecx;
    uint8_t     ns_flags = (uint8_t)r->edx;

    if (!syscall_validate_ptr(path, 1)) return -1;
    if (ns_flags > NS_BIND_AFTER) return -1;

    return pulsar_mount(srv_fd, path, ns_flags);
}

// SYS_SBRK (21): extend the calling process's heap by `increment` bytes
static int32_t sys_sbrk(regs_t *r) {
    uint32_t increment = r->ebx;
    pcb_t *p = sched_current();
    if (!p) return -1;

    if (increment == 0)
        return (int32_t)p->heap_top;        // query: return current break

    uint32_t old_top = p->heap_top;
    uint32_t new_top = old_top + increment;

    if (new_top < old_top || new_top > UHEAP_MAX) {
        kprintf("SBRK: [%u] heap ceiling exceeded\n", (uint32_t)p->pid);
        return -1;
    }

    uint32_t map_end = (new_top + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t pd_phys = (uint32_t)p->page_directory;
    uint32_t flags   = VMM_PRESENT | VMM_WRITABLE | VMM_USER;

    for (uint32_t addr = old_top; addr < map_end; addr += PAGE_SIZE) {
        uint32_t frame = pmm_alloc_frame();
        if (!frame) {
            // rollback: free frames allocated this call
            for (uint32_t undo = old_top; undo < addr; undo += PAGE_SIZE) {
                uint32_t *pd  = (uint32_t *)pd_phys;
                uint32_t  pde = pd[undo >> 22];
                if (pde & VMM_PRESENT) {
                    uint32_t *pt  = (uint32_t *)(pde & VMM_ADDR_MASK);
                    uint32_t  pte = pt[(undo >> 12) & 0x3FFu];
                    if (pte & VMM_PRESENT) {
                        pmm_free_frame(pte & VMM_ADDR_MASK);
                        pt[(undo >> 12) & 0x3FFu] = 0;
                    }
                }
            }
            return -1;
        }

        int frame_was_mapped = vmm_is_mapped(frame);                // new frame may already be in identity region - check before mapping
        if (!frame_was_mapped) {
            vmm_map_page(frame, frame, VMM_KERNEL_RW);
        }

        memset((void *)frame, 0, PAGE_SIZE);                        // zero the new page through the kernel before mapping into user space

        if (!frame_was_mapped) {
            vmm_unmap_page(frame);
        }

        vmm_map_page_in(pd_phys, addr, frame, flags);
    }

    p->heap_top = map_end;                                          // always page-aligned after this
    kprintf("SBRK: [%u] 0x%p -> 0x%p (+%u bytes)\n", (uint32_t)p->pid, old_top, p->heap_top, increment);
    return (int32_t)old_top;                                        // pointer to start of new region
}

// SYS_SIGNAL (22): store handler + trampoline in calling process PCB
static int32_t sys_signal(regs_t *r) {
    int      signum     = (int)r->ebx;
    uint32_t handler    = r->ecx;
    uint32_t trampoline = r->edx;

    if (signum <= 0 || signum >= NSIG) return -1;
    if (signum == SIGKILL || signum == SIGSTOP) return -1;     // uncatchable

    pcb_t *p = sched_current();
    if (!p) return -1;

    p->signal_handlers[signum] = handler;
    p->signal_trampoline       = trampoline;
    return 0;
}

// SYS_SIGRETURN (23): restore pre-signal context saved by kernel
static int32_t sys_sigreturn(regs_t *r) {
    pcb_t *p = sched_current();
    if (!p || !p->in_signal) return -1;

    *r          = p->signal_saved_ctx;      // restore full register frame
    p->in_signal = 0;
    return 0;
}

// SYS_KILL (24): mark signal as pending on target process
static int32_t sys_kill(regs_t *r) {
    pid_t pid    = (pid_t)(int32_t)r->ebx;
    int   signum = (int)r->ecx;

    if (signum < 0 || signum >= NSIG) return -1;

    pcb_t *target = proc_get(pid);
    if (!target) return -1;

    if (signum == 0) return 0;                              // sig 0 = existence check only

    target->pending_signals |= (1u << signum);              // queue the signal
    return 0;
}

// SYS_DIAL (25): dial TCP PULSAR server and mount at path
static int32_t sys_dial(regs_t *r) {
 
    const char *addr  = (const char *)r->ebx;
    const char *path  = (const char *)r->ecx;
    uint8_t     flags = (uint8_t)r->edx;
 
    if (!syscall_validate_ptr(addr, 1)) return -1;
    if (!syscall_validate_ptr(path, 1)) return -1;
    if (flags > NS_BIND_AFTER) return -1;
 
    return pulsar_connect(addr, path, flags);
}

// SYS_SEEK (26): reposition the read/write offset of an open file descriptor
static int32_t sys_seek(regs_t *r) {
 
    int     fd     = (int)r->ebx;
    int32_t offset = (int32_t)r->ecx;
    int     whence = (int)r->edx;
 
    pcb_t *p = sched_current();
    if (!p) return -1;
 
    return fd_seek(p->fd_table, fd, offset, whence);
}

// SYS_MKDIR (27): create a directory
static int32_t sys_mkdir(regs_t *r) {
    const char *path = (const char *)r->ebx;
    if (!syscall_validate_ptr(path, 1)) return -1;
    pcb_t *p = sched_current();
    if (!p) return -1;
    char resolved[VFS_PATH_MAX];
    vfs_path_resolve(p->cwd_path, path, resolved);
    return (int32_t)vfs_mkdir(resolved);
}

typedef int32_t (*syscall_fn_t)(regs_t *);

// define dispatch table
static syscall_fn_t syscall_table[SYSCALL_COUNT] = {
    [SYS_YIELD]     = sys_yield,
    [SYS_EXIT]      = sys_exit,
    [SYS_GETPID]    = sys_getpid,
    [SYS_SLEEP]     = sys_sleep,
    [SYS_FORK]      = sys_fork,
    [SYS_EXEC]      = sys_exec,
    [SYS_WRITE]     = sys_write,
    [SYS_READ]      = sys_read,
    [SYS_OPEN]      = sys_open,
    [SYS_CLOSE]     = sys_close,
    [SYS_PIPE]      = sys_pipe,
    [SYS_DUP2]      = sys_dup2,
    [SYS_EXECVE]    = sys_execve,
    [SYS_WAIT]      = sys_wait,
    [SYS_CHDIR]     = sys_chdir,
    [SYS_GETCWD]    = sys_getcwd,
    [SYS_READDIR]   = sys_readdir,
    [SYS_BIND]      = sys_bind,
    [SYS_UNBIND]    = sys_unbind,
    [SYS_NSDUMP]    = sys_nsdump,
    [SYS_MOUNT]     = sys_mount,
    [SYS_SBRK]      = sys_sbrk,
    [SYS_SIGNAL]    = sys_signal,
    [SYS_SIGRETURN] = sys_sigreturn,
    [SYS_KILL]      = sys_kill,
    [SYS_DIAL]      = sys_dial,
    [SYS_SEEK]      = sys_seek,
    [SYS_MKDIR]     = sys_mkdir,
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