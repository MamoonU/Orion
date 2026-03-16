// fork.c - proc_fork

#include "proc.h"
#include "sched.h"
#include "kprintf.h"
#include "fd.h"
#include "string.h"
#include "namespace.h"

// create new process
pid_t proc_fork(uint32_t child_entry) {

    pcb_t *parent = sched_current();                                                // return current process

    const char *name     = parent ? parent->name     : "child";                     // inherit parent properties
    uint8_t     priority = parent ? parent->priority : PROC_PRIO_NORMAL;

    kprintf("PROC: proc_fork — [%u] \"%s\" forking child at 0x%p\n", parent ? (uint32_t)parent->pid : 0u, name, child_entry);

    // allocate and initialise a new PCB
    pcb_t *child = proc_create(name, priority);
    if (!child) {
        kprintf("PROC: proc_fork — proc_create failed\n");
        return PID_INVALID;
    }

    // wire parent-child relationship
    if (parent) {
        child->ppid = parent->pid;
        fd_table_close_all(child->fd_table);                        // discard the fresh stdin/out/err
        fd_table_clone(parent->fd_table, child->fd_table);          // inherit parent's fds
 
        // inherit filesystem context
        strncpy(child->cwd_path, parent->cwd_path, VFS_PATH_MAX - 1);
        child->cwd_path[VFS_PATH_MAX - 1] = '\0';
        child->namespace = parent->namespace;
    }

    proc_init_frame(child, child_entry);                                            // build child stack frame
    proc_set_ready(child);                                                          // child = runnable
    sched_add(child);                                                               // add child -> scheduler queue

    kprintf("PROC: proc_fork — child [%u] queued, returning to parent [%u]\n",
            (uint32_t)child->pid,
            parent ? (uint32_t)parent->pid : 0u);

    return child->pid;                                                              // return to parent
}