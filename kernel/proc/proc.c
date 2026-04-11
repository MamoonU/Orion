// Process Control Block - Orion OS

#include "proc.h"
#include "kheap.h"
#include "timer.h"
#include "kprintf.h"
#include "panic.h"
#include "string.h"
#include "sched.h"
#include "fd.h"
#include "namespace.h"
#include "vmm.h"
#include "pmm.h"

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

    kprintf("PROC: pid_alloc - PID table exhausted\n");
    return PID_INVALID;
}

// free pid 
void pid_free(pid_t pid) {

    if (pid == PID_KERNEL || pid == PID_INVALID) return;

    if (!pid_bitmap_test(pid)) {
        kprintf("PROC: pid_free - WARNING: double-free of PID %u\n", (uint32_t)pid);
        return;
    }

    pid_bitmap_clear(pid);
    if (pid < pid_search_hint) pid_search_hint = pid;
}

// construct pcb safely
pcb_t *proc_create(const char *name, uint8_t priority) {

    pid_t pid = pid_alloc();                                                    // alloc pid
    if (pid == PID_INVALID) {
        kprintf("PROC: proc_create - no free PID\n");
        return 0;
    }

    pcb_t *p = &proc_table[pid];                                                // return pcb slot
    if (p->state != PROC_UNUSED) {
        kprintf("PROC: proc_create - FATAL: slot not UNUSED\n");
        pid_free(pid);
        return 0;
    }

    uint8_t *kstack = (uint8_t *)kmalloc_aligned(KSTACK_SIZE);                  // allocate kernel stack
    if (!kstack) {
        kprintf("PROC: proc_create - OOM allocating kernel stack\n");
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

    for (uint32_t i = 0; i < sizeof(cpu_context_t); i++)
        ((uint8_t *)&p->context)[i] = 0;

    p->context.cs     = 0x08;           // kernel code segment
    p->context.eflags = 0x00000202u;    // IF=1 + reserved bit 1
    p->context.ds     = 0x10;
    p->context.es     = 0x10;
    p->context.fs     = 0x10;
    p->context.gs     = 0x10;

    // kernel stack
    p->kstack_base = kstack;                                    // memory allocation pointer
    p->kstack_top  = (uint32_t)kstack + KSTACK_SIZE;            // initial stack pointer
    p->esp0        = p->kstack_top;                             // value to load into TSS for transitions

    p->esp_kernel  = 0;

    uint32_t pd_phys = vmm_create_address_space();
    if (!pd_phys) {
        kprintf("PROC: proc_create - failed to create address space for [%u]\n", (uint32_t)pid);
        kfree_aligned(kstack);
        pid_free(pid);
        return 0;
    }

    p->page_directory = (uint32_t *)pd_phys;
    p->heap_top       = UHEAP_START;

    if (priority > PROC_PRIO_IDLE) priority = PROC_PRIO_IDLE;
    p->priority      = priority;
    p->base_priority = priority;

    uint32_t tslice = PROC_TIMESLICE_DEFAULT;
    if      (priority < PROC_PRIO_NORMAL)
        tslice = PROC_TIMESLICE_DEFAULT + (PROC_PRIO_NORMAL - priority) / 2u;
    else if (priority > PROC_PRIO_NORMAL)
        tslice = PROC_TIMESLICE_DEFAULT - (priority - PROC_PRIO_NORMAL) / 5u;
    if (tslice < 2) tslice = 2;

    p->timeslice_len = tslice;
    p->timeslice     = tslice;

    p->ticks_total     = 0;
    p->ticks_scheduled = 0;
    p->tick_last_run   = 0;
    p->tick_created    = timer_get_ticks();
    p->wakeup_tick     = 0;

    p->wait_for_pid    = PID_INVALID;
    p->waiting         = 0;

    // open stdin(0), stdout(1), stderr(2) for this process
    fd_table_init(p->fd_table);

    // filesystem context: inherit from parent when forking, else default to root
    strncpy(p->cwd_path, "/", VFS_PATH_MAX - 1);
    p->cwd_path[VFS_PATH_MAX - 1] = '\0';
    p->namespace = ns_create();                             // fresh namespace

    // signal state - all zeroed: SIG_DFL for every signal, no pending, not in handler
    p->pending_signals   = 0;
    p->signal_mask       = 0;
    p->in_signal         = 0;
    p->signal_trampoline = 0;
    for (int i = 0; i < NSIG; i++) p->signal_handlers[i] = 0;

    kprintf("PROC: created [%u] \"%s\" prio=%u quantum=%u kstack=0x%p\n", (uint32_t)pid, p->name, (uint32_t)priority, tslice, (uint32_t)kstack);
    return p;
}

// transition EMBRYO -> READY
void proc_set_ready(pcb_t *p) {

    if (!p) return;

    if (p->state != PROC_EMBRYO) {
        kprintf("PROC: proc_set_ready - not EMBRYO\n");
        panic("PROC: proc_set_ready called on non-EMBRYO process");
    }

    p->state = PROC_READY;

    kprintf("PROC: [%u] \"%s\" -> READY\n", (uint32_t)p->pid, p->name);
}

// transition ZOMBIE -> DESTROY
void proc_destroy(pcb_t *p) {

    if (!p) return;

    if (p->state != PROC_ZOMBIE) {
        kprintf("PROC: proc_destroy - not ZOMBIE\n");
        panic("PROC: proc_destroy called on non-ZOMBIE process");
    }

    kprintf("PROC: destroying [%u] \"%s\"\n", (uint32_t)p->pid, p->name);

    if (p->page_directory) {
        vmm_destroy_address_space((uint32_t)p->page_directory);
        p->page_directory = 0;
    }

    if (p->kstack_base) {
        kfree_aligned(p->kstack_base);
        p->kstack_base = 0;
        p->kstack_top  = 0;
        p->esp0        = 0;
        p->esp_kernel  = 0;
    }

    fd_table_close_all(p->fd_table);
    ns_unref(p->namespace);
    p->namespace = 0;
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

// initialise frame 
void proc_init_frame(pcb_t *p, uint32_t entry_point) {

    if (!p || !p->kstack_top) {
        kprintf("PROC: proc_init_frame - invalid PCB\n");
        return;
    }

    uint32_t *sp = (uint32_t *)p->kstack_top;

    // CPU-pushed
    *--sp = 0x00000202u;                                                        // eflags  (IF=1, reserved bit 1)
    *--sp = 0x08;                                                               // cs      (kernel code segment)
    *--sp = entry_point;                                                        // eip

    // stub metadata
    *--sp = 0;                                                                  // err_code
    *--sp = 0;                                                                  // int_no

    // pusha restores in order: edi, esi, ebp, esp, ebx, edx, ecx, eax
    *--sp = 0;                                                                  // eax
    *--sp = 0;                                                                  // ecx
    *--sp = 0;                                                                  // edx
    *--sp = 0;                                                                  // ebx
    *--sp = 0;                                                                  // esp_saved (popa ignores this)
    *--sp = 0;                                                                  // ebp
    *--sp = 0;                                                                  // esi
    *--sp = 0;                                                                  // edi

    // segment registers - popped in order: gs, fs, es, ds
    *--sp = 0x10;                                                               // ds
    *--sp = 0x10;                                                               // es
    *--sp = 0x10;                                                               // fs
    *--sp = 0x10;                                                               // gs  <- esp_kernel points here

    p->esp_kernel = (uint32_t)sp;

    kprintf("PROC: [%u] \"%s\" frame @ 0x%p  eip=0x%p\n", (uint32_t)p->pid, p->name, p->esp_kernel, entry_point);
}

// build ring-3 frame -> process's kernel stack
void proc_init_user_frame(pcb_t *p, uint32_t entry_point, uint32_t user_esp) {

    if (!p || !p->kstack_top) {
        kprintf("PROC: proc_init_user_frame - invalid PCB\n");
        return;
    }

    uint32_t *sp = (uint32_t *)p->kstack_top;       // stack

    // CPU pops these during ring-3 iret
    *--sp = 0x23u;                                  // ss       (user data segment)
    *--sp = user_esp;                               // user_esp (ring-3 stack pointer)
    *--sp = 0x00000202u;                            // eflags   (interrupts enabled)
    *--sp = 0x1Bu;                                  // cs       (user code segment)
    *--sp = entry_point;                            // eip

    // fake interrupt metadata
    *--sp = 0;                                      // err_code
    *--sp = 0;                                      // int_no

    // pusha layout block
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;    // eax ecx edx ebx
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;    // esp_saved ebp esi edi

    // segment registers
    *--sp = 0x23u;                                  // ds
    *--sp = 0x23u;                                  // es
    *--sp = 0x23u;                                  // fs
    *--sp = 0x23u;                                  // gs  <- esp_kernel points here

    p->esp_kernel = (uint32_t)sp;                   // fully constructed stack frame

    kprintf("PROC: [%u] \"%s\" user frame @ 0x%p  eip=0x%p  user_esp=0x%p\n", (uint32_t)p->pid, p->name, p->esp_kernel, entry_point, user_esp);
}
 
// setup usable user-mode stack
int proc_setup_user_stack(pcb_t *p) {

    if (!p || !p->page_directory) {
        kprintf("PROC: proc_setup_user_stack - invalid PCB or missing PD\n");
        return -1;
    }

    uint32_t pd_phys    = (uint32_t)p->page_directory;
    uint32_t stack_base = USTACK_TOP - USTACK_SIZE;
    uint32_t flags      = VMM_PRESENT | VMM_WRITABLE | VMM_USER;

    for (uint32_t addr = stack_base; addr < USTACK_TOP; addr += PAGE_SIZE) {        // loop over each page

        uint32_t frame = pmm_alloc_frame();                                         // alloc phys mem
        if (!frame) {
            kprintf("PROC: proc_setup_user_stack - OOM at virt=0x%p\n", addr);
            return -1;
        }

        int frame_was_mapped = vmm_is_mapped(frame);
        if (!frame_was_mapped) {
            vmm_map_page(frame, frame, VMM_KERNEL_RW);
        }

        memset((void *)frame, 0, PAGE_SIZE);                                        // zero mem

        if (!frame_was_mapped) {
            vmm_unmap_page(frame);
        }

        vmm_map_page_in(pd_phys, addr, frame, flags);                               // map into process
    }

    kprintf("PROC: [%u] user stack mapped 0x%p-0x%p (pd=0x%p)\n", (uint32_t)p->pid, stack_base, USTACK_TOP, pd_phys);
    return 0;
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
    kprintf("  |  esp_kernel   = 0x%p\n",    p->esp_kernel);
    kprintf("  |  page_dir     = 0x%p\n", (uint32_t)p->page_directory);
    kprintf("  |  eip          = %p  eflags = %p\n", p->context.eip, p->context.eflags);
    kprintf("  |  ticks_total  = %u  scheduled = %ux\n", p->ticks_total, p->ticks_scheduled);
    kprintf("  |  created tick = %u\n",     p->tick_created);

    if (p->state == PROC_BLOCKED && p->wakeup_tick) {
        kprintf("  |  wakeup_tick  = %u\n", p->wakeup_tick);
    }
    if (p->state == PROC_ZOMBIE) {
        kprintf("  |  exit_code    = %u\n", (uint32_t)p->exit_code);
    }
    kprintf("  |  cwd          = \"%s\"\n", p->cwd_path);
    kprintf("  |  namespace    = 0x%p (%u binds, refcount=%u)\n", (uint32_t)p->namespace, p->namespace ? p->namespace->nbinds  : 0u, p->namespace ? p->namespace->refcount : 0u);

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

void proc_exit(int32_t exit_code) {

    pcb_t *p = sched_current();
    if (!p) { kprintf("PROC: proc_exit - no current process\n"); return; }

    kprintf("PROC: [%u] \"%s\" exiting (code=%d)\n", (uint32_t)p->pid, p->name, (int)exit_code);

    p->exit_code = exit_code;
    p->state     = PROC_ZOMBIE;

    if (p->ppid != PID_KERNEL && p->ppid != PID_INVALID) {

        pcb_t *parent = proc_get(p->ppid);

        if (parent) {

            parent->pending_signals |= (1u << SIGCHLD);

            if (parent->waiting) {

                if (parent->wait_for_pid == PID_INVALID || parent->wait_for_pid == p->pid) {
                    kprintf("PROC: waking parent [%u] \"%s\"\n", (uint32_t)parent->pid, parent->name);
                    proc_wake(parent);
                }
            }
        }
    }
    sched_remove(p);
    sched_yield();
}

pid_t proc_wait(pid_t pid, int32_t *out_code) {

    pcb_t *self = sched_current();
    if (!self) return PID_INVALID;

    kprintf("PROC: [%u] \"%s\" waiting for %s\n",
            (uint32_t)self->pid, self->name,
            (pid == PID_INVALID) ? "any child" : "specific child");

    while (1) {
        for (pid_t i = 0; i < MAX_PROCS; i++) {
            pcb_t *child = proc_get(i);
            if (!child) continue;
            if (child->ppid != self->pid) continue;
            if (pid != PID_INVALID && child->pid != pid) continue;
            if (child->state == PROC_ZOMBIE) {
                if (out_code) *out_code = child->exit_code;
                pid_t cpid = child->pid;
                kprintf("PROC: [%u] \"%s\" reaped child [%u] (code=%d)\n",
                        (uint32_t)self->pid, self->name,
                        (uint32_t)cpid, (int)child->exit_code);
                proc_destroy(child);
                return cpid;
            }
        }

        self->wait_for_pid = pid;
        self->waiting      = 1;
        self->wakeup_tick  = 0;
        self->state        = PROC_BLOCKED;
        sched_remove(self);
        sched_yield();
        self->waiting = 0;
    }
}

void proc_sleep(uint32_t ticks) {

    pcb_t *p = sched_current();
    if (!p || ticks == 0) return;

    p->wakeup_tick = timer_get_ticks() + ticks;
    p->state       = PROC_BLOCKED;

    kprintf("PROC: [%u] \"%s\" sleeping for %u ticks (wake at %u)\n",
            (uint32_t)p->pid, p->name, ticks, p->wakeup_tick);

    sched_remove(p);
    sched_yield();
}

void proc_wake(pcb_t *p) {

    if (!p) return;

    if (p->state != PROC_BLOCKED) {
        kprintf("PROC: proc_wake - [%u] not BLOCKED (state=%s), ignoring\n",
                (uint32_t)p->pid, proc_state_name(p->state));
        return;
    }

    p->wakeup_tick = 0;
    p->state       = PROC_READY;
    sched_add(p);

}

// check process -> ring-3 has pending signals
void proc_deliver_signals(regs_t *r) {

    pcb_t *p = sched_current();
    if (!p) return;

    if ((r->cs & 0x3) != 3) return;                                 // only deliver when returning to ring-3 (RPL bits of CS = 3)

    uint32_t deliverable = p->pending_signals & ~p->signal_mask;
    if (!deliverable) return;                                       // no deliverable signals

    if (p->in_signal) return;                                       // no nesting - if already inside a handler: wait for sigreturn

    int signum = -1;
    for (int i = 1; i < NSIG; i++) {                                // find the lowest-numbered pending signal (signal 1 has highest priority)
        if (deliverable & (1u << i)) {
            signum = i;
            break;
        }
    }
    if (signum < 0) return;

    p->pending_signals &= ~(1u << signum);                          // consume pending bit

    uint32_t handler = p->signal_handlers[signum];

    if (handler == 1) return;                                                   // SIG_IGN: silently discard

    if (handler == 0) {                                                         // SIG_DFL: terminate
        kprintf("SIGNAL: [%u] \"%s\" killed by signal %d (SIG_DFL)\n", (uint32_t)p->pid, p->name, signum);
        proc_exit(-(signum));
        return;
    }

    // custom handler delivery

    p->signal_saved_ctx = *r;                       // save complete pre-signal CPU register state
    p->in_signal        = 1;

    uint32_t new_esp    = r->useresp - 8;           // build fake stack frame
    uint32_t *ustack    = (uint32_t *)new_esp;      // valid: process PD is active in CR3

    ustack[0]           = p->signal_trampoline;     // [esp+0]: return address for handler
    ustack[1]           = (uint32_t)signum;         // [esp+4]: argument

    r->useresp = new_esp;                           // redirect iret to the handler
    r->eip     = handler;

    kprintf("SIGNAL: [%u] \"%s\" sig=%d -> handler=0x%p  trampoline=0x%p\n", (uint32_t)p->pid, p->name, signum, handler, p->signal_trampoline);
}