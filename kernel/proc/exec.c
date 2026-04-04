// exec.c - proc_exec

#include "proc.h"
#include "sched.h"
#include "kprintf.h"
#include "vmm.h"

// replace program of existing process
int proc_exec(pcb_t *p, uint32_t new_entry) {

    if (!p) {
        kprintf("PROC: proc_exec - NULL pcb\n");
        return -1;
    }

    if (p->state == PROC_RUNNING) {
        kprintf("PROC: proc_exec - cannot exec a RUNNING process\n");
        return -1;
    }

    kprintf("PROC: proc_exec - [%u] \"%s\" -> new entry 0x%p\n", (uint32_t)p->pid, p->name, new_entry);

    // remove from ready queue if already queued
    sched_remove(p);

    // destroy old address space
    if (p->page_directory) {
        vmm_destroy_address_space((uint32_t)p->page_directory);
        p->page_directory = 0;
    }

    // create new address space
    uint32_t new_pd = vmm_create_address_space();
    if (!new_pd) {
        kprintf("PROC: proc_exec - failed to create address space\n"); return -1;
    }
    p->page_directory = (uint32_t *)new_pd;
 
    // setup user stack
    if (proc_setup_user_stack(p) != 0) {
        kprintf("PROC: proc_exec - failed to set up user stack\n");
        vmm_destroy_address_space(new_pd);
        p->page_directory = 0;
        return -1;
    }

    // ring 3 excecution
    proc_init_user_frame(p, new_entry, USTACK_TOP);
    p->heap_top = UHEAP_START;                          // heap break for freshh addr space

    // reset accounting and re-arm full timeslice
    p->ticks_total     = 0;
    p->ticks_scheduled = 0;
    p->timeslice       = p->timeslice_len;
    p->wakeup_tick     = 0;
    p->waiting         = 0;

    p->state = PROC_READY;                                              // mark process ready again
    sched_add(p);                                                       // add back to scheduler

    kprintf("PROC: proc_exec - [%u] re-queued at 0x%p\n", (uint32_t)p->pid, new_entry);

    return 0;                                                           // return success
}