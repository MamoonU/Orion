// Process Control Block Header

//  state transition diagram:
//
//            proc_create()        proc_ready()
//  (nothing) ──────────> EMBRYO ─────────────> READY <─────────┐
//                                                  │           │
//                                    scheduler     │           │  unblock /
//                                    picks proc    ▼           │  wakeup
//                                              RUNNING ────────┘
//                                                  │         yield / preempt
//                                                  │
//                                       exit()     │  wait for I/O, lock, sleep
//                                                  ├────────────────────────────────> BLOCKED
//                                                  │
//                                                  ▼
//                                               ZOMBIE ──> proc_destroy() ──> UNUSED

#ifndef PROC_H
#define PROC_H

#include <stdint.h>
#include <stddef.h>

#define MAX_PROCS       256     // maximum concurrent processes (PID 0 - 255)
#define KSTACK_SIZE     4096    // 4KB kernel stack per process (one page)
#define PROC_NAME_LEN   32      // process_name(max length)

typedef enum {
    PROC_UNUSED  = 0,   // slot in the process table is free
    PROC_EMBRYO  = 1,   // being constructed
    PROC_READY   = 2,   // runnable, waiting on the ready queue
    PROC_RUNNING = 3,   // executing on the CPU
    PROC_BLOCKED = 4,   // waiting for an event (I/O, sleep, lock)
    PROC_ZOMBIE  = 5,   // exited, waiting for parent to call proc_destroy()
} proc_state_t;

// lower the number, higher the prio
#define PROC_PRIO_REALTIME   0              // hard real-time — never preempted by lower
#define PROC_PRIO_HIGH       8              // interactive / high-priority system tasks
#define PROC_PRIO_NORMAL    16              // default for all user processes
#define PROC_PRIO_LOW       24              // background / batch work
#define PROC_PRIO_IDLE      31              // idle thread only

#define PROC_PRIO_DEFAULT   PROC_PRIO_NORMAL

// Default time quantum in PIT ticks
#define PROC_TIMESLICE_DEFAULT  10          // 10 ticks = 100 ms


// layout of the interrupt stack frame constructed by irq.asm / isr.asm.
typedef struct cpu_context {

    // segment registers (pushed by stub after pusha)
    uint32_t gs;
    uint32_t fs;
    uint32_t es;
    uint32_t ds;

    // general-purpose registers (pusha - edi = lowest addr)
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp_saved;                                             // esp at the instant of pusha (not the interrupt esp)
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;

    // interrupt metadata (pushed by stub)
    uint32_t int_no;
    uint32_t err_code;

    // pushed automatically by the CPU on interrupt
    uint32_t eip;                                                   // instruction to return to
    uint32_t cs;                                                    // code segment at time of interrupt
    uint32_t eflags;                                                // processor flags (IF, DF, etc.)

    // pushed by CPU on privilege-level change (ring 3 -> ring 0)
    uint32_t useresp;                                               // user stack pointer  (ring 3 processes only)
    uint32_t ss;                                                    // user stack segment  (ring 3 processes only)

} __attribute__((packed)) cpu_context_t;


typedef uint16_t pid_t;                             // process id type (16 bit cleaner)

#define PID_KERNEL   0          // PID 0 - reserved for kernel idle process
#define PID_INIT     1          // PID 1 - first user process (future initilise)
#define PID_INVALID  0xFFFFu    // sentinel for "no PID / allocation failed"

typedef struct pcb {

    // identity
    pid_t           pid;
    pid_t           ppid;
    char            name[PROC_NAME_LEN];        // 32 bit

    // lifecycle
    proc_state_t    state;
    int32_t         exit_code;                  // stored until parent process reaps

    // CPU context
    cpu_context_t   context;
    uint32_t        esp0;                       // loaded into TSS.esp0

    // kernel stack
    uint8_t        *kstack_base;
    uint32_t        kstack_top;

    // address space
    uint32_t       *page_directory;             // (NULL = kernel PD)

    // future scheduling

    // scheduler — priority
    uint8_t         priority;
    uint8_t         base_priority;

    // scheduler — time-slice
    uint32_t        timeslice_len;
    uint32_t        timeslice;

    // scheduler — accounting
    uint32_t        ticks_total;
    uint32_t        ticks_scheduled;
    uint32_t        tick_last_run;
    uint32_t        tick_created;

    // blocking / sleep
    uint32_t        wakeup_tick;


} pcb_t;


void proc_init(void);                                           // setup process table, PID pool, kernel process

pid_t  pid_alloc(void);                                         // alloc pid
void   pid_free(pid_t pid);                                     // free pid

pcb_t *proc_create(const char *name, uint8_t priority);
void   proc_set_ready(pcb_t *p);
void   proc_destroy(pcb_t *p);

pcb_t *proc_get(pid_t pid);                                     // lookup pid
const char *proc_state_name(proc_state_t s);

void   proc_set_priority(pcb_t *p, uint8_t priority);           // dynamic schedulings
void   proc_set_timeslice(pcb_t *p, uint32_t ticks);

void   proc_dump(const pcb_t *p);                               // debugging
void   proc_dump_all(void);

#endif