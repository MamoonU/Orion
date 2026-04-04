// sched.c - Round-robin priority scheduler

#include "sched.h"
#include "proc.h"
#include "tss.h"
#include "kprintf.h"
#include "timer.h"
#include "syscall.h"
#include "vmm.h"

#define SCHED_MAX_PROCS MAX_PROCS

static pcb_t   *ready_queue[SCHED_MAX_PROCS];       // pointers to READY / RUNNING PCBs
static uint32_t queue_size  = 0;                    // number of entries in ready_queue
static uint32_t current_idx = 0;                    // index of currently running process

static pcb_t    *current_proc = 0;                  // currently running PCB
static int      sched_enabled = 0;                  // 0 = disabled, 1 = active

static volatile int tick_flag = 0;                  // only switch on timer interrupts

// reset scheduler on clean state
void sched_init(void) {
    queue_size    = 0;
    current_idx   = 0;
    current_proc  = 0;
    sched_enabled = 0;
    tick_flag     = 0;
    kprintf("SCHED: Scheduler initialised\n\n");
}

// add a READY process
void sched_add(pcb_t *p) {

    if (!p || p->state != PROC_READY) {                             // ensure proc = READY
        kprintf("SCHED: sched_add - process not READY\n");
        return;
    }

    if (queue_size >= SCHED_MAX_PROCS) {                            // ensure queue = not full
        kprintf("SCHED: sched_add - queue full\n");
        return;
    }

    // insert into queue
    ready_queue[queue_size++] = p;
}

// remove process from ready queue
void sched_remove(pcb_t *p) {

    for (uint32_t i = 0; i < queue_size; i++) {
        if (ready_queue[i] == p) {
            ready_queue[i] = ready_queue[--queue_size];
            ready_queue[queue_size] = 0;
            return;
        }
    }
}

// Called from timer_handler() on every PIT tick.
void sched_tick(void) {

    tick_flag = 1;

    uint32_t now = timer_get_ticks();
    for (pid_t i = 0; i < MAX_PROCS; i++) {
        pcb_t *p = proc_get(i);
        if (!p) continue;
        if (p->state != PROC_BLOCKED) continue;
        if (p->wakeup_tick == 0) continue;
        if (p->wakeup_tick <= now)
            proc_wake(p);
    }
}

// return current process
pcb_t *sched_current(void) {
    return current_proc;
}

// switch_process : no switch ? switch
uint32_t sched_switch_esp(uint32_t current_esp) {

    // only switch on timer ticks
    if (!sched_enabled || !tick_flag) return 0;
    tick_flag = 0;

    if (!current_proc) return 0;

    // decrement timeslice
    if (current_proc->timeslice > 0)
        current_proc->timeslice--;

    // timeslice remaining: continue running current process
    if (current_proc->timeslice > 0) return 0;

    // find next READY process (highest priority = lowest number)
    pcb_t *next = 0;
    uint32_t next_idx = 0;
    uint8_t  best_prio = 255;

    for (uint32_t i = 0; i < queue_size; i++) {

        pcb_t *p = ready_queue[i];
        if (!p) continue;
        if (p == current_proc) continue;           // skip current
        if (p->state != PROC_READY) continue;      // skip non-runnable

        if (p->priority < best_prio) {
            best_prio = p->priority;
            next      = p;
            next_idx  = i;
        }
    }

    // no other process is ready: reset timeslice and keep running
    if (!next) {
        current_proc->timeslice = current_proc->timeslice_len;
        return 0;
    }

    // 1. save the current kernel-stack pointer
    current_proc->esp_kernel = current_esp;
    current_proc->ticks_total += current_proc->timeslice_len;
    current_proc->tick_last_run = timer_get_ticks();
    if (current_proc->state == PROC_RUNNING) {
        current_proc->state = PROC_READY;
    }

    // 2. advance the current index (simple round-robin tie-break)
    current_idx = next_idx;

    // 3. activate the chosen process
    current_proc = next;
    current_proc->state      = PROC_RUNNING;
    current_proc->timeslice  = current_proc->timeslice_len;
    current_proc->ticks_scheduled++;

    // 4. update TSS.esp0
    tss_set_esp0(current_proc->esp0);

    // 5. switch address space: load next process PD into CR3
    uint32_t new_pd = current_proc->page_directory ? (uint32_t)current_proc->page_directory : vmm_get_kernel_pd();
    vmm_switch(new_pd);

    return current_proc->esp_kernel;
}

// launch scheduler
void sched_start(void) {

    if (queue_size == 0) {
        kprintf("SCHED: sched_start - no processes in queue\n");
        return;
    }

    // pick the highest-priority READY process
    pcb_t   *first    = 0;
    uint32_t first_idx = 0;
    uint8_t  best_prio = 255;

    for (uint32_t i = 0; i < queue_size; i++) {
        pcb_t *p = ready_queue[i];
        if (p && p->state == PROC_READY && p->priority < best_prio) {
            best_prio = p->priority;
            first     = p;
            first_idx = i;
        }
    }

    if (!first) {
        kprintf("SCHED: sched_start - no READY process found\n");
        return;
    }

    current_proc  = first;                                              // active process
    current_idx   = first_idx;

    current_proc->state = PROC_RUNNING;                                 // update state
    current_proc->ticks_scheduled++;

    sched_enabled = 1;                                                  // enable scheduler

    tss_set_esp0(current_proc->esp0);                                   // update TSS

    // load first process's PD before jumping into it
    uint32_t first_pd = current_proc->page_directory ? (uint32_t)current_proc->page_directory : vmm_get_kernel_pd();
    vmm_switch(first_pd);

    kprintf("SCHED: Starting - first process [%u] \"%s\"\n", (uint32_t)current_proc->pid, current_proc->name);

    asm volatile ("cli");                                               // disable interrupts
    sched_start_first(current_proc->esp_kernel);                        // start first process

}

// diagnostic dump scheduled queue
void sched_dump(void) {

    kprintf("SCHED: -- scheduler dump --\n");
    kprintf("SCHED: enabled=%d  queue_size=%u  current_idx=%u\n",
            sched_enabled, queue_size, current_idx);

    if (current_proc)
        kprintf("SCHED: current = [%u] \"%s\"  timeslice=%u\n",
                (uint32_t)current_proc->pid, current_proc->name,
                current_proc->timeslice);
    else
        kprintf("SCHED: current = (none)\n");

    for (uint32_t i = 0; i < queue_size; i++) {
        pcb_t *p = ready_queue[i];
        if (!p) continue;
        kprintf("SCHED:   [%u] idx=%u \"%s\" state=%s prio=%u slice=%u\n",
                (uint32_t)p->pid, i, p->name,
                proc_state_name(p->state),
                (uint32_t)p->priority,
                p->timeslice);
    }
    kprintf("\n");
}

void sched_force_switch(void) {
    tick_flag = 1;
    if (current_proc)
        current_proc->timeslice = 0;
}

void sched_yield(void) {
    sched_force_switch();
    asm volatile("int $0x80" : : "a"(SYS_YIELD) : "memory");
}