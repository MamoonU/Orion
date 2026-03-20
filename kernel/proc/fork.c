// fork.c - proc_fork

#include "proc.h"
#include "sched.h"
#include "kprintf.h"
#include "fd.h"
#include "string.h"
#include "namespace.h"
#include "pmm.h"
#include "vmm.h"

// copy-on-fork clone (process_PD[x] -> process_PD[y])
static int copy_address_space(uint32_t dst_pd_phys, uint32_t src_pd_phys) {

    if (!dst_pd_phys || !src_pd_phys) return -1;

    uint32_t *src_pd = (uint32_t *)src_pd_phys;                                                 // source PD
    uint32_t *dst_pd = (uint32_t *)dst_pd_phys;                                                 // destination PD

    for (int pde = VMM_KERNEL_PDE_END; pde < 1024; pde++) {                                     // walk user space PDEs

        if (!(src_pd[pde] & VMM_PRESENT)) continue;                                             // skip emty regions

        uint32_t *src_pt    = (uint32_t *)(src_pd[pde] & VMM_ADDR_MASK);                        // source PD's PT
        uint32_t  pde_flags = src_pd[pde] & ~VMM_ADDR_MASK;                                     // extract flags

        uint32_t dst_pt_phys = pmm_alloc_frame();                                               // alloc new PT for destination PD
        if (!dst_pt_phys) {
            kprintf("PROC: copy_address_space - OOM allocating PT (pde=%d)\n", pde);
            return -1;
        }

        uint32_t *dst_pt = (uint32_t *)dst_pt_phys;

        for (int pte = 0; pte < 1024; pte++) {                                                  // walk every page within table

            // case A: page not present
            if (!(src_pt[pte] & VMM_PRESENT)) {
                dst_pt[pte] = 0; continue;
            }

            // case B: page is present
            uint32_t src_frame = src_pt[pte] & VMM_ADDR_MASK;                                   // return source frame
            uint32_t pte_flags = src_pt[pte] & ~VMM_ADDR_MASK;

            uint32_t dst_frame = pmm_alloc_frame();                                             // alloc new frame for destination 
            if (!dst_frame) {
                kprintf("PROC: copy_address_space - OOM copying frame (pde=%d pte=%d)\n", pde, pte);
                pmm_free_frame(dst_pt_phys);
                return -1;
            }

            memcpy((void *)dst_frame, (const void *)src_frame, PAGE_SIZE);                      // copy memory
            dst_pt[pte] = dst_frame | pte_flags;                                                // copy flags + install mapping
        }
        dst_pd[pde] = dst_pt_phys | pde_flags;                                                  // install new PT -> destination PD
    }
    return 0;
}

// create new process
pid_t proc_fork(uint32_t child_entry) {

    pcb_t       *parent = sched_current();                                          // return current process
    const char  *name = parent ? parent->name : "child";                            // inherit parent properties
    uint8_t     priority = parent ? parent->priority : PROC_PRIO_NORMAL;

    kprintf("PROC: proc_fork — [%u] \"%s\" forking child at 0x%p\n", parent ? (uint32_t)parent->pid : 0u, name, child_entry);

    // allocate and initialise a new PCB
    pcb_t *child = proc_create(name, priority);
    if (!child) {
        kprintf("PROC: proc_fork — proc_create failed\n");
        return PID_INVALID;
    }

    // deep-copy parent's user pages into child's address space
    if (parent && parent->page_directory) {

        if (copy_address_space((uint32_t)child->page_directory, (uint32_t)parent->page_directory) != 0) {

            kprintf("PROC: proc_fork - address-space clone failed\n");
            child->state = PROC_ZOMBIE;
            proc_destroy(child);
            return PID_INVALID;
        }
    }

    // wire parent-child relationship
    if (parent) {
        child->ppid = parent->pid;
        child->heap_top = parent->heap_top;                         // inherit (pages already cloned)
        fd_table_close_all(child->fd_table);                        // discard the fresh stdin/out/err
        fd_table_clone(parent->fd_table, child->fd_table);          // inherit parent's fds
 
        // inherit filesystem context
        strncpy(child->cwd_path, parent->cwd_path, VFS_PATH_MAX - 1);
        child->cwd_path[VFS_PATH_MAX - 1] = '\0';

        ns_unref(child->namespace);             // drop fresh empty ns from proc_create
        child->namespace = parent->namespace;   // share parent's namespace
        ns_ref(child->namespace);               // copy-on-bind isolates on first mutation

        // inherit signal handlers and mask - POSIX: fork copies dispositions
        for (int i = 0; i < NSIG; i++) {
            child->signal_handlers[i] = parent->signal_handlers[i];
        }
        child->signal_mask       = parent->signal_mask;
        child->signal_trampoline = parent->signal_trampoline;

        // do NOT inherit pending signals or in_signal state - child starts clean
        child->pending_signals = 0;
        child->in_signal       = 0;
    }

    proc_init_frame(child, child_entry);                                            // build child stack frame
    proc_set_ready(child);                                                          // child = runnable
    sched_add(child);                                                               // add child -> scheduler queue

    kprintf("PROC: proc_fork — child [%u] queued, returning to parent [%u]\n", (uint32_t)child->pid, parent ? (uint32_t)parent->pid : 0u);

    return child->pid;                                                              // return to parent
}