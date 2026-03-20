// pipe.c - Anonymous Pipe IPC

#include "pipe.h"
#include "vfs.h"
#include "kheap.h"
#include "fd.h"
#include "proc.h"
#include "sched.h"
#include "kprintf.h"
#include "string.h"

// forward declarations
static int pipe_open_noop (vnode_t *v, int flags);
static int pipe_read_op   (vnode_t *v, void *buf, uint32_t len, uint32_t off);
static int pipe_write_op  (vnode_t *v, const void *buf, uint32_t len, uint32_t off);
static int pipe_close_read (vnode_t *v);
static int pipe_close_write(vnode_t *v);

// ops tables (separate per direction)
static vfs_ops_t pipe_read_ops = {
    .open    = pipe_open_noop,
    .close   = pipe_close_read,
    .read    = pipe_read_op,
    .write   = NULL,
    .lookup  = NULL,
    .create  = NULL,
    .mkdir   = NULL,
    .unlink  = NULL,
    .readdir = NULL,
};

// ops tables (separate per direction)
static vfs_ops_t pipe_write_ops = {
    .open    = pipe_open_noop,
    .close   = pipe_close_write,
    .read    = NULL,
    .write   = pipe_write_op,
    .lookup  = NULL,
    .create  = NULL,
    .mkdir   = NULL,
    .unlink  = NULL,
    .readdir = NULL,
};

// block current process on this pipe until woken by peer
static void pipe_block(pipe_t *p, uint8_t is_write) {

    pcb_t *self = sched_current();
    if (!self) return;

    if (is_write)
        p->blocked_writer = (uint16_t)self->pid;                                    // set blocked_writer -> PID
    else
        p->blocked_reader = (uint16_t)self->pid;                                    // set blocked_reader -> PID

    self->state = PROC_BLOCKED;                                                     // mark blocked

    sched_remove(self);
    sched_yield();
    
}

// wake process sleeping on opposite end
static void pipe_wake_peer(pipe_t *p, uint8_t woke_by_write) {

    uint16_t peer_pid = woke_by_write ? p->blocked_reader : p->blocked_writer;

    if (peer_pid == PIPE_NO_WAITER) return;

    if (woke_by_write)
        p->blocked_reader = PIPE_NO_WAITER;
    else
        p->blocked_writer = PIPE_NO_WAITER;

    pcb_t *peer = proc_get((pid_t)peer_pid);
    if (peer) proc_wake(peer);

}

// Free shared pipe_t once both ends closed
static void pipe_maybe_free(pipe_t *p) {

    if (p->readers == 0 && p->writers == 0) {                                       // if readers & writers = 0
        kprintf("PIPE: releasing pipe @ 0x%p\n", (uint32_t)p);
        kfree(p);                                                                   // free pipe_t
    }
}

static int pipe_open_noop(vnode_t *v, int flags) {

    (void)v; (void)flags;
    return 0;

}

// read up to len bytes from pipe
static int pipe_read_op(vnode_t *v, void *buf, uint32_t len, uint32_t off) {

    (void)off;

    pipe_end_t *end  = (pipe_end_t *)v->data;
    pipe_t     *pipe = end->pipe;
    uint8_t    *dst  = (uint8_t *)buf;
    uint32_t    read = 0;

    while (read < len) {

        while (pipe->count == 0) {                                      // block if buffer is empty but writers exist
            if (pipe->writers == 0) return (int)read;                   // EOF
            pipe_block(pipe, 0);                                        // sleep: retry on wake
        }

        uint32_t batch = len - read;                                    // read as much as available (up to len)
        if (batch > pipe->count) batch = pipe->count;

        for (uint32_t i = 0; i < batch; i++) {
            dst[read++] = pipe->buf[pipe->head];
            pipe->head  = (pipe->head + 1) & (PIPE_BUFSZ - 1);
        }
        pipe->count -= batch;

        // wake a blocked writer now that space has freed up
        pipe_wake_peer(pipe, 0);

    }
    return (int)read;
}

// write len bytes into pipe
static int pipe_write_op(vnode_t *v, const void *buf, uint32_t len, uint32_t off) {

    (void)off;

    pipe_end_t    *end  = (pipe_end_t *)v->data;
    pipe_t        *pipe = end->pipe;
    const uint8_t *src  = (const uint8_t *)buf;
    uint32_t       written = 0;

    while (written < len) {

        if (pipe->readers == 0) {                                               // broken pipe detection
            kprintf("PIPE: write on broken pipe\n");
            pcb_t *writer = sched_current();
            if (writer) {
                writer->pending_signals |= (1u << SIGPIPE);
            }
            return -1;
        }

        while (pipe->count == PIPE_BUFSZ) {                                     // block if buffer is full
            if (pipe->readers == 0) return -1;
            pipe_block(pipe, 1);
        }

        uint32_t space = PIPE_BUFSZ - pipe->count;                              // write as much as available (up to len)
        uint32_t batch = len - written;
        if (batch > space) batch = space;

        uint32_t tail = (pipe->head + pipe->count) & (PIPE_BUFSZ - 1);          // batch writing and wraparound
        for (uint32_t i = 0; i < batch; i++) {
            pipe->buf[tail] = src[written++];
            tail = (tail + 1) & (PIPE_BUFSZ - 1);
        }
        pipe->count += batch;

        // wake a blocked reader
        pipe_wake_peer(pipe, 1);

    }
    return (int)written;
}

// close read end: decrement readers -> free if both ends gone
static int pipe_close_read(vnode_t *v) {

    pipe_end_t *end  = (pipe_end_t *)v->data;
    pipe_t     *pipe = end->pipe;

    if (pipe->readers > 0) pipe->readers--;                         // decrement readers counter

    if (pipe->readers == 0) pipe_wake_peer(pipe, 0);                // wake any blocked writer to observe the broken-pipe condition

    kfree(end);                                                     // free pipe_end_t
    v->data = NULL;
    pipe_maybe_free(pipe);                                          // release pipe_t
    kfree(v);
    return 0;

}

// close write end: decrement writers -> free if both ends gone
static int pipe_close_write(vnode_t *v) {

    pipe_end_t *end  = (pipe_end_t *)v->data;
    pipe_t     *pipe = end->pipe;

    if (pipe->writers > 0) pipe->writers--;                         // decrement writers counter

    if (pipe->writers == 0) pipe_wake_peer(pipe, 1);                // wake any blocked reader to observe EOF

    kfree(end);                                                     // free pipe_end_t
    v->data = NULL;
    pipe_maybe_free(pipe);                                          // release pipe_t
    kfree(v);
    return 0;

}

// create pipe
int pipe_create(file_t **fd_table, int pipefd[2]) {

    pipe_t *pipe = (pipe_t *)kmalloc(sizeof(pipe_t));               // allocate shared ring buffer
    if (!pipe) {
        kprintf("PIPE: pipe_create — OOM (pipe_t)\n");
        return -1;
    }
    memset(pipe, 0, sizeof(pipe_t));
    pipe->readers        = 1;
    pipe->writers        = 1;
    pipe->blocked_reader = PIPE_NO_WAITER;
    pipe->blocked_writer = PIPE_NO_WAITER;

    pipe_end_t *rend = (pipe_end_t *)kmalloc(sizeof(pipe_end_t));       // allocate read end
    if (!rend) goto err_free_pipe;
    rend->pipe     = pipe;
    rend->is_write = 0;

    vnode_t *rv = vnode_alloc(VNODE_PIPE, &pipe_read_ops, rend);        // allocate read end vnode
    if (!rv) goto err_free_rend;

    pipe_end_t *wend = (pipe_end_t *)kmalloc(sizeof(pipe_end_t));       // allocate write end
    if (!wend) goto err_free_rv;
    wend->pipe     = pipe;
    wend->is_write = 1;

    vnode_t *wv = vnode_alloc(VNODE_PIPE, &pipe_write_ops, wend);       // allocate write end vnode
    if (!wv) goto err_free_wend;

    file_t *rf = vfs_open_vnode(rv, O_RDONLY);                          // open file_t wrappers
    if (!rf) goto err_free_wv;
    file_t *wf = vfs_open_vnode(wv, O_WRONLY);
    if (!wf) { vfs_close(rf); goto err_free_wv; }

    int rfd = fd_install(fd_table, rf);                                 // install -> fd table
    if (rfd < 0) { vfs_close(rf); vfs_close(wf); goto err_free_wv; }

    int wfd = fd_install(fd_table, wf);                                 // install -> fd table
    if (wfd < 0) {
        fd_close(fd_table, rfd);
        vfs_close(wf);
        goto err_free_wv;
    }

    pipefd[0] = rfd;
    pipefd[1] = wfd;

    kprintf("PIPE: created pipe [r=%d w=%d] buf @ 0x%p\n", rfd, wfd, (uint32_t)pipe);
    return 0;

    err_free_wv:   kfree(wv);
    err_free_wend: kfree(wend);
    err_free_rv:   kfree(rv);
    err_free_rend: kfree(rend);
    err_free_pipe: kfree(pipe);
        kprintf("PIPE: pipe_create — allocation failed\n");
        return -1;
}