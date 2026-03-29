// procfs.c - /proc synthetic filesystem

#include "procfs.h"
#include "vfs.h"
#include "ramfs.h"
#include "proc.h"
#include "sched.h"
#include "fd.h"
#include "namespace.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "timer.h"


// vnode tag ftypes
#define PROC_FILE_STATUS  0
#define PROC_FILE_MEM     1
#define PROC_FILE_FD      2
#define PROC_FILE_NS      3

// vnode tag structure
typedef struct {
    uint16_t pid;       // which process
    uint8_t  ftype;     // PROC_FILE_*
} procfs_tag_t;

// convert uint32 decimal -> decimal string
static uint32_t u32_to_dec(uint32_t v, char *buf, uint32_t bufsz) {
    if (!bufsz) return 0;
    if (v == 0) { buf[0] = '0'; if (bufsz > 1) buf[1] = '\0'; return 1; }
    char tmp[12]; int i = 11; tmp[11] = '\0';
    while (v && i > 0) { tmp[--i] = (char)('0' + v % 10); v /= 10; }
    uint32_t len = 11 - (uint32_t)i;
    if (len >= bufsz) len = bufsz - 1;
    memcpy(buf, &tmp[i], len);
    buf[len] = '\0';
    return len;
}

// append string S -> OUT
static void app(char *out, uint32_t *pos, uint32_t limit, const char *s) {
    while (*s && *pos + 1 < limit) out[(*pos)++] = *s++;
}

// convert number -> string
static void app_u32(char *out, uint32_t *pos, uint32_t limit, uint32_t v) {
    char tmp[12];
    u32_to_dec(v, tmp, sizeof(tmp));
    app(out, pos, limit, tmp);
}

// append "key: value" pair
static void app_kv(char *out, uint32_t *pos, uint32_t limit, const char *key, const char *val) {
    app(out, pos, limit, key);
    app(out, pos, limit, ": ");
    app(out, pos, limit, val);
    app(out, pos, limit, "\n");
}

// append "key: <uint>"
static void app_ku(char *out, uint32_t *pos, uint32_t limit, const char *key, uint32_t val) {
    char tmp[12]; u32_to_dec(val, tmp, sizeof(tmp));
    app_kv(out, pos, limit, key, tmp);
}

// convert PID -> decimal string
static uint32_t pid_to_str(uint16_t pid, char *buf, uint32_t bufsz) {
    return u32_to_dec((uint32_t)pid, buf, bufsz);
}

// /proc/N/status
static int procfs_status_read(vnode_t *v, void *buf, uint32_t len, uint32_t off) {

    procfs_tag_t *tag = (procfs_tag_t *)v->data;                                        // tag
    pcb_t *p = proc_get((pid_t)tag->pid);                                               // pid
    if (!p) return -1;

    char out[512];                                                                      // output buffer
    uint32_t pos = 0;

    app_ku(out, &pos, sizeof(out), "pid",       (uint32_t)p->pid);                      // build file
    app_ku(out, &pos, sizeof(out), "ppid",      (uint32_t)p->ppid);
    app_kv(out, &pos, sizeof(out), "name",      p->name);
    app_kv(out, &pos, sizeof(out), "state",     proc_state_name(p->state));
    app_ku(out, &pos, sizeof(out), "priority",  (uint32_t)p->priority);
    app_ku(out, &pos, sizeof(out), "timeslice", p->timeslice);
    app_ku(out, &pos, sizeof(out), "tslice_len",p->timeslice_len);
    app_ku(out, &pos, sizeof(out), "ticks",     p->ticks_total);
    app_ku(out, &pos, sizeof(out), "scheduled", p->ticks_scheduled);
    app_kv(out, &pos, sizeof(out), "cwd",       p->cwd_path);

    if (p->state == PROC_ZOMBIE) {                                                      // zombie handling
        app_ku(out, &pos, sizeof(out), "exit_code", (uint32_t)(int32_t)p->exit_code);
    }

    out[pos] = '\0';                                                                    // finalise (out = valid C string)

    if (off >= pos) return 0;                                                           // offset handling
    uint32_t avail = pos - off;                                                         // avail = remaining data
    uint32_t n = (len < avail) ? len : avail;                                           // len   = user buffer size
    memcpy(buf, out + off, n);
    return (int)n;
}

// /proc/N/mem
static int procfs_mem_read(vnode_t *v, void *buf, uint32_t len, uint32_t off) {

    procfs_tag_t *tag = (procfs_tag_t *)v->data;
    pcb_t *p = proc_get((pid_t)tag->pid);
    if (!p) return -1;

    char out[256];
    uint32_t pos = 0;

    char hex[12];                                                   // page_directory as hex
    uint32_t pd = (uint32_t)p->page_directory;
    static const char hx[] = "0123456789abcdef";

    hex[0]='0';                                                     // build hex backwards
    hex[1]='x';
    for (int i = 9; i >= 2; i--) {
        hex[i] = hx[pd & 0xF]; pd >>= 4;
    }
    hex[10] = '\0';

    // output
    app_kv(out, &pos, sizeof(out), "page_dir", hex);                // page directory = address space root
    app_ku(out, &pos, sizeof(out), "heap_top", p->heap_top);        // heap_top       = user heap end
    app_ku(out, &pos, sizeof(out), "kstack",   p->kstack_top);      // kstack         = kernel stack top

    out[pos] = '\0';
    if (off >= pos) return 0;
    uint32_t avail = pos - off; uint32_t n = (len < avail) ? len : avail;
    memcpy(buf, out + off, n);
    return (int)n;
}

// /proc/N/fd
static int procfs_fd_read(vnode_t *v, void *buf, uint32_t len, uint32_t off) {

    procfs_tag_t *tag = (procfs_tag_t *)v->data;
    pcb_t *p = proc_get((pid_t)tag->pid);
    if (!p) return -1;

    char out[512];
    uint32_t pos = 0;

    int any = 0;
    for (int i = 0; i < FD_MAX; i++) {                                      // loop through FD table

        if (!p->fd_table[i]) continue;                                      // skip unused

        app(out, &pos, sizeof(out), "fd ");
        app_u32(out, &pos, sizeof(out), (uint32_t)i);                       // print FD number

        app(out, &pos, sizeof(out), ": open  refcount=");
        app_u32(out, &pos, sizeof(out), p->fd_table[i]->refcount);          // print reference count

        app(out, &pos, sizeof(out), "  offset=");
        app_u32(out, &pos, sizeof(out), p->fd_table[i]->offset);            // print offset

        app(out, &pos, sizeof(out), "\n");
        any = 1;
    }

    if (!any) {                                                             // empty case
        app(out, &pos, sizeof(out), "(no open descriptors)\n");
    }

    out[pos] = '\0';
    if (off >= pos) return 0;
    uint32_t avail = pos - off; uint32_t n = (len < avail) ? len : avail;
    memcpy(buf, out + off, n);
    return (int)n;
}

// /proc/N/ns
static int procfs_ns_read(vnode_t *v, void *buf, uint32_t len, uint32_t off) {

    procfs_tag_t *tag = (procfs_tag_t *)v->data;
    pcb_t *p = proc_get((pid_t)tag->pid);
    if (!p || !p->namespace) return -1;

    char out[1024]; uint32_t pos = 0;                                           // output buffer
    const ns_t *ns = p->namespace;

    app(out, &pos, sizeof(out), "refcount: ");                                  // print reference count
    app_u32(out, &pos, sizeof(out), ns->refcount);
    app(out, &pos, sizeof(out), "\nbinds: ");                                   // print # of active bind mounts
    app_u32(out, &pos, sizeof(out), ns->nbinds);
    app(out, &pos, sizeof(out), "\n");

    for (uint32_t i = 0; i < NS_BINDS_MAX; i++) {                               // iterate bind entries

        const ns_bind_entry_t *b = &ns->binds[i];
        if (!b->active) continue;

        app(out, &pos, sizeof(out), "  [");
        app_u32(out, &pos, sizeof(out), i);                                     // print slot index in bind table
        app(out, &pos, sizeof(out), "] flags=");
        app_u32(out, &pos, sizeof(out), (uint32_t)b->flags);                    // print flags (binding type = default, before, after)
        app(out, &pos, sizeof(out), "  path=");
        app(out, &pos, sizeof(out), b->new_path);                               // print mount destination
        app(out, &pos, sizeof(out), "\n");
    }

    out[pos] = '\0';
    if (off >= pos) return 0;
    uint32_t avail = pos - off; uint32_t n = (len < avail) ? len : avail;
    memcpy(buf, out + off, n);
    return (int)n;
}

// /proc/uptime
static int procfs_uptime_read(vnode_t *v, void *buf, uint32_t len, uint32_t off) {

    (void)v;
    char out[64]; uint32_t pos = 0;

    uint32_t ticks   = timer_get_ticks();                                   // get kernel time
    uint32_t seconds = ticks / 100;
    uint32_t ms      = (ticks % 100) * 10;

    app_u32(out, &pos, sizeof(out), seconds);                               // print seconds
    app(out, &pos, sizeof(out), ".");

    if (ms < 100) app(out, &pos, sizeof(out), "0");                         // zero padding
    if (ms < 10)  app(out, &pos, sizeof(out), "0");

    app_u32(out, &pos, sizeof(out), ms);                                    // print ms
    app(out, &pos, sizeof(out), " seconds\n");

    out[pos] = '\0';
    if (off >= pos) return 0;
    uint32_t avail = pos - off; uint32_t n = (len < avail) ? len : avail;
    memcpy(buf, out + off, n);
    return (int)n;
}

// vfs_ops for per-file vnodes
static vfs_ops_t status_ops = { .read = procfs_status_read };
static vfs_ops_t mem_ops    = { .read = procfs_mem_read    };
static vfs_ops_t fd_ops     = { .read = procfs_fd_read     };
static vfs_ops_t ns_ops     = { .read = procfs_ns_read     };
static vfs_ops_t uptime_ops = { .read = procfs_uptime_read };

// PID directory design: dynamic lookup and readdir
static const char *pid_entries[] = { "status", "mem", "fd", "ns" };

// file type mapping: link name -> file types
static const uint8_t pid_ftypes[] = {
    PROC_FILE_STATUS, PROC_FILE_MEM, PROC_FILE_FD, PROC_FILE_NS
};

// ops mapping: link names -> behaviour
static vfs_ops_t *pid_ops_table[] = {
    &status_ops, &mem_ops, &fd_ops, &ns_ops
};

#define PID_ENTRY_COUNT 4

// PID directory tag
typedef struct {
    uint16_t pid;
} procfs_piddir_tag_t;

// PID directory lookup
static int piddir_lookup(vnode_t *dir, const char *name, vnode_t **out) {

    procfs_piddir_tag_t *dt = (procfs_piddir_tag_t *)dir->data;
    pcb_t *p = proc_get((pid_t)dt->pid);
    if (!p) return -1;

    for (int i = 0; i < PID_ENTRY_COUNT; i++) {
        if (strcmp(name, pid_entries[i]) != 0) continue;                    // match file name

        procfs_tag_t *tag = (procfs_tag_t *)kmalloc(sizeof(procfs_tag_t));  // create file tag
        if (!tag) return -1;
        tag->pid   = dt->pid;
        tag->ftype = pid_ftypes[i];

        vnode_t *v = vnode_alloc(VNODE_DEV, pid_ops_table[i], tag);         // create vnode
        if (!v) { kfree(tag); return -1; }

        *out = v;                                                           // return
        return 0;
    }
    return -1;
}

static int piddir_readdir(vnode_t *dir, uint32_t index, char *name_out, vnode_t **node_out) {

    procfs_piddir_tag_t *dt = (procfs_piddir_tag_t *)dir->data;
    if (!proc_get((pid_t)dt->pid)) return -1;
    if (index >= PID_ENTRY_COUNT) return -1;

    if (name_out)
        strncpy(name_out, pid_entries[index], VFS_NAME_MAX - 1);            // return name
    if (node_out) {
        vnode_t *dummy = 0;
        piddir_lookup(dir, pid_entries[index], &dummy);                     // optionally return node
        *node_out = dummy;
    }
    return 0;
}

// directory operations
static vfs_ops_t piddir_ops = {
    .lookup  = piddir_lookup,
    .readdir = piddir_readdir,
};

// create PID directory vnode for process pid: create /proc/<pid>
static vnode_t *make_pid_dir(uint16_t pid) {

    procfs_piddir_tag_t *tag = (procfs_piddir_tag_t *)kmalloc(sizeof(procfs_piddir_tag_t));
    if (!tag) return 0;
    tag->pid = pid;                                                                             // attach PID -> directory

    vnode_t *v = vnode_alloc(VNODE_DIR, &piddir_ops, tag);                                      // create vnode
    if (!v) { kfree(tag); return 0; }
    return v;
}

// parse PID = decimal string -> uint16
static int parse_pid(const char *s, uint16_t *out) {

    if (!s || !*s) return 0;                    // reject null/empty string

    uint32_t n = 0;                             // 32-bit accumulator

    while (*s) {                                // digit loop
        if (*s < '0' || *s > '9') return 0;     // ensure numeric chars
        n = n * 10 + (uint32_t)(*s - '0');      // decimal parsing
        if (n > 0xFFFEu) return 0;
        s++;
    }
    *out = (uint16_t)n;
    return 1;
}

// process root lookup: resolve /proc/<name>
static int procroot_lookup(vnode_t *dir, const char *name, vnode_t **out) {

    (void)dir;

    // "uptime" = special file
    if (strcmp(name, "uptime") == 0) {
        vnode_t *v = vnode_alloc(VNODE_DEV, &uptime_ops, 0);    // create vnode
        if (!v) return -1;
        *out = v;
        return 0;
    }

    uint16_t pid;
    if (!parse_pid(name, &pid)) return -1;                      // parse pid

    pcb_t *p = proc_get((pid_t)pid);                            // validate process
    if (!p) return -1;

    vnode_t *v = make_pid_dir(pid);                             // create pid directory
    if (!v) return -1;
    *out = v;
    return 0;
}

// process root read directory: list /proc
static int procroot_readdir(vnode_t *dir, uint32_t index, char *name_out, vnode_t **node_out) {

    (void)dir;

    uint32_t found = 0;
    for (uint16_t pid = 0; pid < MAX_PROCS; pid++) {                // iterate process table

        pcb_t *p = proc_get((pid_t)pid);                            // skip unused
        if (!p) continue;

        if (found == index) {                                       // match index
            if (name_out) {
                char tmp[8]; pid_to_str(pid, tmp, sizeof(tmp));
                strncpy(name_out, tmp, VFS_NAME_MAX - 1);           // return PID name
            }
            if (node_out) *node_out = make_pid_dir(pid);            // create directory node
            return 0;
        }
        found++;                                                    // increment
    }

    // after all PIDs: "uptime"
    if (found == index) {
        if (name_out) strncpy(name_out, "uptime", VFS_NAME_MAX - 1);
        if (node_out) *node_out = vnode_alloc(VNODE_DEV, &uptime_ops, 0);   // return uptime
        return 0;
    }

    return -1;
}

// process root operations
static vfs_ops_t procroot_ops = {
    .lookup  = procroot_lookup,
    .readdir = procroot_readdir,
};

// initialisation: mount /proc
void procfs_init(void) {

    kprintf("PROCFS: Initialising\n");

    vnode_t *root = vnode_alloc(VNODE_DIR, &procroot_ops, 0);               // create root vnode
    if (!root) {
        kprintf("PROCFS: FATAL — could not allocate root vnode\n");
        return;
    }

    if (vfs_mkdir("/proc") < 0) {                                           // create directory in VFS
        kprintf("PROCFS: FATAL — could not create /proc\n");
        return;
    }

    if (ramfs_register_dev("/proc", root) < 0) {                            // replace ramfs stub directory with our dynamic root
        kprintf("PROCFS: FATAL — could not register /proc\n");
        return;
    }

    kprintf("PROCFS: /proc ready  (dynamic PID lookup + /proc/uptime)\n");
}


