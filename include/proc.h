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

#include "irq.h"
#include "fd.h"
#include "namespace.h"
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

#define NSIG                32              // matches liborion signal count
#define SIGKILL             9               // kill: cannot be caught or ignored
#define SIGSTOP             19              // stop: cannot be caught or ignored

// Default time quantum in PIT ticks
#define PROC_TIMESLICE_DEFAULT  10          // 10 ticks = 100 ms

typedef regs_t cpu_context_t;               // alias for regs_t (same interrupt stack frame)

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
    uint32_t        esp_kernel;                       // loaded into TSS.esp0
    uint32_t        esp0;

    // kernel stack
    uint8_t        *kstack_base;
    uint32_t        kstack_top;

    // address space
    uint32_t       *page_directory;             // (NULL = kernel PD)
    uint32_t        heap_top;

    // scheduler - priority
    uint8_t         priority;
    uint8_t         base_priority;

    // scheduler - time-slice
    uint32_t        timeslice_len;
    uint32_t        timeslice;

    // scheduler - accounting
    uint32_t        ticks_total;
    uint32_t        ticks_scheduled;
    uint32_t        tick_last_run;
    uint32_t        tick_created;

    // blocking / sleep
    uint32_t        wakeup_tick;

    // parent-child coordination
    pid_t           wait_for_pid;           // PID blocked waiting for (PID_INVALID = any child)
    uint8_t         waiting;                // 1 = blocked inside proc_wait

    // file descriptors
    file_t          *fd_table[FD_MAX];

    // filesystem context (future: per-process 9P namespace binding point)
    char            cwd_path[VFS_PATH_MAX];     // current working directory (absolute path)
    ns_t            *namespace;                 // namespace root vnode (NULL = global VFS root)

    uint32_t        pending_signals;            // bitmask: bit N = signal N queued
    uint32_t        signal_mask;                // bitmask: bit N = signal N blocked
    uint32_t        signal_handlers[NSIG];      // registered user-space handler VAs
    uint32_t        signal_trampoline;          // VA of sigreturn_trampoline in user image
    cpu_context_t   signal_saved_ctx;           // full register snapshot saved before delivery
    uint8_t         in_signal;                  // 1 = currently inside a signal handler

} pcb_t;


void proc_init(void);                                           // setup process table, PID pool, kernel process

pid_t  pid_alloc(void);                                         // alloc pid
void   pid_free(pid_t pid);                                     // free pid

pcb_t *proc_create(const char *name, uint8_t priority);
void   proc_set_ready(pcb_t *p);
void   proc_destroy(pcb_t *p);

void   proc_init_frame(pcb_t *p, uint32_t entry_point);         // fake irq-stub register frame for scheduler

void   proc_init_user_frame(pcb_t *p, uint32_t entry_point, uint32_t user_esp);     // build ring-3 iret frame on the kernel stack
int    proc_setup_user_stack(pcb_t *p);                                             // allocate and map USTACK_SIZE bytes below USTACK_TOP in the process's page

pcb_t *proc_get(pid_t pid);                                     // lookup pid
const char *proc_state_name(proc_state_t s);

void   proc_set_priority(pcb_t *p, uint8_t priority);           // dynamic scheduling
void   proc_set_timeslice(pcb_t *p, uint32_t ticks);

void   proc_exit(int32_t exit_code);                            // running -> zombie
pid_t  proc_wait(pid_t pid, int32_t *out_code);                 // running -> blocked -> destroy
void   proc_sleep(uint32_t ticks);                              // running -> blocked (until proc_wake)
void   proc_wake(pcb_t *p);                                     // blocked -> ready

pid_t  proc_fork(uint32_t child_entry);                         // new process (child of current)
int    proc_exec(pcb_t *p, uint32_t new_entry);                 // replace stopped process' excecution -> new entry point

void   proc_dump(const pcb_t *p);                               // debugging
void   proc_dump_all(void);

void   proc_deliver_signals(regs_t *r);                         // signal delivery: called from irq.asm and syscall.asm before every iret to ring-3

#endif