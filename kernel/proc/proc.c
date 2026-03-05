// Process Control Block — Orion OS

#include "proc.h"
#include "kheap.h"
#include "timer.h"

#include "kprintf.h"
#include "panic.h"
#include "string.h"

static pcb_t proc_table[MAX_PROCS];                                                                     // fixed size array

#define PID_BITMAP_WORDS  (MAX_PROCS / 32)
static uint32_t pid_bitmap[PID_BITMAP_WORDS];                                                           // maintain bitmap
static pid_t    pid_search_hint = 1;

static inline void pid_bitmap_set(pid_t pid)   { pid_bitmap[pid/32] |=  (1u << (pid%32)); }
static inline void pid_bitmap_clear(pid_t pid) { pid_bitmap[pid/32] &= ~(1u << (pid%32)); }
static inline int  pid_bitmap_test(pid_t pid)  { return (pid_bitmap[pid/32] >> (pid%32)) & 1u; }


void proc_init(void) {                                                                                  // initialise

    kprintf("PROC: Initialising process table\n");

    for (int i = 0; i < MAX_PROCS; i++) {                                                               // mark all PCB entries unused
        proc_table[i].state = PROC_UNUSED;
        proc_table[i].pid   = (pid_t)i;
    }

    for (int i = 0; i < PID_BITMAP_WORDS; i++)                                                          // clear bitmap
        pid_bitmap[i] = 0;

    pid_bitmap_set(PID_KERNEL);                                                                         // reserve PID 0 for kernel
    pid_search_hint = PID_INIT;

    kprintf("PROC: Table ready - %u allocatable process slots\n\n", (uint32_t)(MAX_PROCS - 1));
}

// 2 pass scan allocator over bitmap
pid_t pid_alloc(void) {

    for (int pass = 0; pass < 2; pass++) {
        pid_t start = (pass == 0) ? pid_search_hint : PID_INIT;
        pid_t end   = (pass == 0) ? (pid_t)MAX_PROCS : pid_search_hint;

        for (pid_t pid = start; pid < end; pid++) {
            if (!pid_bitmap_test(pid)) {
                pid_bitmap_set(pid);
                pid_search_hint = (pid + 1 < MAX_PROCS) ? pid + 1 : PID_INIT;
                return pid;
            }
        }
    }

    kprintf("PROC: pid_alloc — PID table exhausted\n");
    return PID_INVALID;
}

// free pid 
void pid_free(pid_t pid) {

    if (pid == PID_KERNEL || pid == PID_INVALID) return;

    if (!pid_bitmap_test(pid)) {
        kprintf("PROC: pid_free — WARNING: double-free of PID %u\n", (uint32_t)pid);
        return;
    }

    pid_bitmap_clear(pid);
    if (pid < pid_search_hint) pid_search_hint = pid;
}

// construct pcb safely
pcb_t *proc_create(const char *name, uint8_t priority) {

    pid_t pid = pid_alloc();                                                    // alloc pid

    if (pid == PID_INVALID) {
        kprintf("PROC: proc_create — no free PID\n");
        return 0;
    }

    pcb_t *p = &proc_table[pid];                                                // return pcb slot

    if (p->state != PROC_UNUSED) {
        kprintf("PROC: proc_create — FATAL: slot not UNUSED\n");
        pid_free(pid);
        return 0;
    }

    uint8_t *kstack = (uint8_t *)kmalloc_aligned(KSTACK_SIZE);                  // allocate kernel stack

    if (!kstack) {
        kprintf("PROC: proc_create — OOM allocating kernel stack\n");
        pid_free(pid);
        return 0;
    }

    // identity initialisation
    p->pid  = pid;
    p->ppid = PID_KERNEL;
    strncpy(p->name, name ? name : "unnamed", PROC_NAME_LEN);

    // lifecycle
    p->state     = PROC_EMBRYO;
    p->exit_code = 0;

    // CPU context
    cpu_context_t *ctx = &p->context;
    ctx->gs       = 0x10;           // kernel data segment
    ctx->fs       = 0x10;
    ctx->es       = 0x10;
    ctx->ds       = 0x10;
    ctx->edi      = 0;
    ctx->esi      = 0;
    ctx->ebp      = 0;
    ctx->esp_saved= 0;
    ctx->ebx      = 0;
    ctx->edx      = 0;
    ctx->ecx      = 0;
    ctx->eax      = 0;
    ctx->int_no   = 0;
    ctx->err_code = 0;
    ctx->eip      = 0;              // caller sets this to the entry point
    ctx->cs       = 0x08;           // kernel code segment
    ctx->eflags   = 0x00000202u;    // IF=1, reserved bit 1
    ctx->useresp  = 0;
    ctx->ss       = 0;

    // kernel stack
    p->kstack_base = kstack;                                    // memory allocation pointer
    p->kstack_top  = (uint32_t)kstack + KSTACK_SIZE;            // initial stack pointer
    p->esp0        = p->kstack_top;                             // value to load into TSS for transitions

    // address space
    p->page_directory = 0;

    // priority
    if (priority > PROC_PRIO_IDLE) priority = PROC_PRIO_IDLE;   // prevents invalid priority values
    p->priority      = priority;
    p->base_priority = priority;

    // time-slice: higher priority gets a longer quantum
    uint32_t tslice = PROC_TIMESLICE_DEFAULT;
    if (priority < PROC_PRIO_NORMAL)
        tslice = PROC_TIMESLICE_DEFAULT + (PROC_PRIO_NORMAL - priority) / 2u;
    else if (priority > PROC_PRIO_NORMAL)
        tslice = PROC_TIMESLICE_DEFAULT - (priority - PROC_PRIO_NORMAL) / 5u;
    if (tslice < 2) tslice = 2;

    p->timeslice_len = tslice;
    p->timeslice     = tslice;

    // accounting
    p->ticks_total     = 0;
    p->ticks_scheduled = 0;
    p->tick_last_run   = 0;
    p->tick_created    = timer_get_ticks();
    p->wakeup_tick     = 0;

    // logging
    kprintf("PROC: created [%u] \"%s\" prio=%u quantum=%u kstack=%p\n",
            (uint32_t)pid, p->name, (uint32_t)priority, tslice, (uint32_t)kstack);

    return p;
}

// transition EMBRYO -> READY
void proc_set_ready(pcb_t *p) {

    if (!p) return;

    if (p->state != PROC_EMBRYO) {
        kprintf("PROC: proc_set_ready — not EMBRYO\n");
        panic("PROC: proc_set_ready called on non-EMBRYO process");
    }

    p->state = PROC_READY;

    kprintf("PROC: [%u] \"%s\" -> READY\n", (uint32_t)p->pid, p->name);
}


// transition ZOMBIE -> DESTROY
void proc_destroy(pcb_t *p) {

    if (!p) return;

    if (p->state != PROC_ZOMBIE) {
        kprintf("PROC: proc_destroy — not ZOMBIE\n");
        panic("PROC: proc_destroy called on non-ZOMBIE process");
    }

    kprintf("PROC: destroying [%u] \"%s\"\n", (uint32_t)p->pid, p->name);

    if (p->kstack_base) {
        kfree_aligned(p->kstack_base);
        p->kstack_base = 0;
        p->kstack_top  = 0;
        p->esp0        = 0;
    }

    pid_free(p->pid);

    pid_t saved_pid = p->pid;
    for (size_t i = 0; i < sizeof(pcb_t); i++)
        ((uint8_t *)p)[i] = 0;

    p->pid   = saved_pid;
    p->state = PROC_UNUSED;
}

// process lookup
pcb_t *proc_get(pid_t pid) {
    if (pid >= MAX_PROCS) return 0;
    if (proc_table[pid].state == PROC_UNUSED) return 0;
    return &proc_table[pid];
}

// map enum -> string (for dump)
const char *proc_state_name(proc_state_t s) {
    switch (s) {
        case PROC_UNUSED:  return "UNUSED";
        case PROC_EMBRYO:  return "EMBRYO";
        case PROC_READY:   return "READY";
        case PROC_RUNNING: return "RUNNING";
        case PROC_BLOCKED: return "BLOCKED";
        case PROC_ZOMBIE:  return "ZOMBIE";
        default:           return "UNKNOWN";
    }
}

// set priority
void proc_set_priority(pcb_t *p, uint8_t priority) {
    if (!p) return;
    if (priority > PROC_PRIO_IDLE) priority = PROC_PRIO_IDLE;
    p->priority      = priority;
    p->base_priority = priority;
}

// set time slice
void proc_set_timeslice(pcb_t *p, uint32_t ticks) {
    if (!p || ticks < 2) return;
    p->timeslice_len = ticks;
    p->timeslice     = ticks;
}

// diagnostic proccesses dump
void proc_dump(const pcb_t *p) {

    if (!p) return;

    kprintf("  +- PCB [%u] \"%s\"\n", (uint32_t)p->pid, p->name);

    kprintf("  |  state        = %s\n",     proc_state_name(p->state));
    kprintf("  |  ppid         = %u\n",     (uint32_t)p->ppid);
    kprintf("  |  priority     = %u (base=%u)\n", (uint32_t)p->priority, (uint32_t)p->base_priority);
    kprintf("  |  timeslice    = %u / %u ticks\n", p->timeslice, p->timeslice_len);
    kprintf("  |  kstack_base  = %p  top = %p\n", (uint32_t)p->kstack_base, p->kstack_top);
    kprintf("  |  esp0         = %p\n",     p->esp0);
    kprintf("  |  eip          = %p  eflags = %p\n", p->context.eip, p->context.eflags);
    kprintf("  |  ticks_total  = %u  scheduled = %ux\n", p->ticks_total, p->ticks_scheduled);
    kprintf("  |  created tick = %u\n",     p->tick_created);

    if (p->state == PROC_BLOCKED && p->wakeup_tick) {
        kprintf("  |  wakeup_tick  = %u\n", p->wakeup_tick);
    }
    if (p->state == PROC_ZOMBIE) {
        kprintf("  |  exit_code    = %u\n", (uint32_t)p->exit_code);
    }

    kprintf("  +---------------------------------\n");
}

// dump all active processes
void proc_dump_all(void) {

    kprintf("PROC: -- process table dump --\n");
    uint32_t count = 0;

    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state != PROC_UNUSED) {
            proc_dump(&proc_table[i]);
            count++;
        }
    }

    if (count == 0) kprintf("  (empty)\n");

    kprintf("PROC: total active: %u\n\n", count);
}