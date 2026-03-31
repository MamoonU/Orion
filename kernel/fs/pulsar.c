// pulsar.c - PULSAR Distributed Filesystem Protocol
 
#include "pulsar.h"
#include "namespace.h"
#include "vfs.h"
#include "proc.h"
#include "sched.h"
#include "fd.h"
#include "kheap.h"
#include "string.h"
#include "kprintf.h"
#include "timer.h"

// encoding helpers
static void put_u8 (uint8_t *b, uint32_t *p, uint8_t  v) {                      // write 1 byte
    b[(*p)++] = v;
}

static uint8_t  get_u8 (const uint8_t *b, uint32_t *p) {                        // read 1 byte
    return b[(*p)++];
}

static void put_u16(uint8_t *b, uint32_t *p, uint16_t v) {                      // write 16 bit int
    b[(*p)++] = (uint8_t)(v);
    b[(*p)++] = (uint8_t)(v >> 8);
}

static uint16_t get_u16(const uint8_t *b, uint32_t *p) {                        // read 16 bit int
    uint16_t v = (uint16_t)b[*p] | ((uint16_t)b[*p + 1] << 8);
    *p += 2;
    return v;
}

static void put_u32(uint8_t *b, uint32_t *p, uint32_t v) {                      // write 32 bit int
    b[(*p)++] = (uint8_t)(v);
    b[(*p)++] = (uint8_t)(v >> 8);
    b[(*p)++] = (uint8_t)(v >> 16);
    b[(*p)++] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *b, uint32_t *p) {                        // read 32 bit int
    uint32_t v = (uint32_t)b[*p] | ((uint32_t)b[*p+1] << 8) | ((uint32_t)b[*p+2] << 16) | ((uint32_t)b[*p+3] << 24);
    *p += 4;
    return v;
}

static void put_u64(uint8_t *b, uint32_t *p, uint64_t v) {                      // write 64 bit int
    put_u32(b, p, (uint32_t)(v & 0xFFFFFFFFu));
    put_u32(b, p, (uint32_t)(v >> 32));
}

static uint64_t get_u64(const uint8_t *b, uint32_t *p) {                        // read 64 bit int
    uint64_t lo = get_u32(b, p);
    uint64_t hi = get_u32(b, p);
    return lo | (hi << 32);
}

static void put_str(uint8_t *b, uint32_t *p, const char *s) {                   // encode 9p string
    uint16_t len = s ? (uint16_t)strlen(s) : 0;
    put_u16(b, p, len);
    for (uint16_t i = 0; i < len; i++) b[(*p)++] = (uint8_t)s[i];
}

static void get_str(const uint8_t *b, uint32_t *p, char *out, uint32_t max) {   // read 9p string
    uint16_t len  = get_u16(b, p);
    uint32_t copy = (len < max - 1) ? len : max - 1;
    for (uint32_t i = 0; i < copy; i++) out[i] = (char)b[*p + i];
    out[copy] = '\0';
    *p += len;
}

static void put_signat(uint8_t *b, uint32_t *p, const pulsar_signat_t *q) {     // encode SIGNAT
    put_u8 (b, p, q->type);
    put_u32(b, p, q->vers);
    put_u64(b, p, q->path);
}

static void get_signat(const uint8_t *b, uint32_t *p, pulsar_signat_t *q) {     // read SIGNAT
    q->type = get_u8 (b, p);
    q->vers = get_u32(b, p);
    q->path = get_u64(b, p);
}

// write 7-byte header into buf -> advance *pos to the body start
static void pulse_begin(uint8_t *buf, uint8_t type, uint16_t tag, uint32_t *pos) {
    *pos = 0;
    put_u32(buf, pos, 0);       // size placeholder: filled in by pulse_send
    put_u8 (buf, pos, type);    // write type
    put_u16(buf, pos, tag);     // write msg tag
}

// finalise size field & write bytes to server
static int pulse_send(pulsar_session_t *s, uint8_t *buf, uint32_t total) {

    // patch in real size (includes 4-byte size field itself)
    buf[0] = (uint8_t)(total);
    buf[1] = (uint8_t)(total >> 8);
    buf[2] = (uint8_t)(total >> 16);
    buf[3] = (uint8_t)(total >> 24);

    uint32_t sent = 0;
    while (sent < total) {                                              // loop until everything written
        int n = vfs_write(s->srv_file, buf + sent, total - sent);       // write call
        if (n <= 0) {
            kprintf("PULSAR: pulse_send - write failed\n");
            return -1;
        }
        sent += (uint32_t)n;
    }
    return 0;
}

// read framed pulse into buf
static int pulse_recv(pulsar_session_t *s, uint8_t *buf) {

    uint32_t got = 0;
    while (got < 4) {                                           // read 4-byte size field first
        int n = vfs_read(s->srv_file, buf + got, 4 - got);
        if (n <= 0) return -1;
        got += (uint32_t)n;
    }

    // decode size
    uint32_t size = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
 
    if (size < PULSAR_HDR || size > PULSAR_MSIZE) {             // validate size
        kprintf("PULSAR: pulse_recv - bad size %u\n", size);
        return -1;
    }

    // read rest of msg
    uint32_t rest = size - 4;
    got = 0;
    while (got < rest) {
        int n = vfs_read(s->srv_file, buf + 4 + got, rest - got);
        if (n <= 0) return -1;
        got += (uint32_t)n;
    }
    return (int)size;
}
 
// validate a server response
static int pulse_check(const uint8_t *buf, int len, uint8_t expected_type, uint16_t tag) {

    if (len < PULSAR_HDR) return -1;                                            // length check

    // decode response header
    uint32_t pos   = 4;
    uint8_t  rtype = get_u8 (buf, &pos);
    uint16_t rtag  = get_u16(buf, &pos);

    if (rtype == ECHO_ANOMALY) {                                                // ANOMALY handling
        char ename[PULSAR_ENAME_MAX];
        get_str(buf, &pos, ename, PULSAR_ENAME_MAX);
        kprintf("PULSAR: server error: %s\n", ename);
        return -1;
    }

    if (rtype != expected_type) {                                               // response type validation
        kprintf("PULSAR: pulse_check - expected type %u got %u\n", (uint32_t)expected_type, (uint32_t)rtype);
        return -1;
    }

    if (rtag != tag && tag != PULSAR_NOTAG) {                                   // tag validation
        kprintf("PULSAR: pulse_check - tag mismatch (want %u got %u)\n", (uint32_t)tag, (uint32_t)rtag);
        return -1;
    }

    return 0;
}

// tag allocation
static uint16_t alloc_tag(pulsar_session_t *s) {
    uint16_t t = s->next_tag++;
    if (t == PULSAR_NOTAG) t = s->next_tag++;
    return t;
}

// allocate beam: new beam ID + track
uint32_t pulsar_beam_alloc(pulsar_session_t *s) {

    for (uint32_t i = 0; i < PULSAR_BEAM_MAX; i++) {
        if (!s->beam_live[i]) {                                     // find free entry
            uint32_t id = s->next_beam++;
            if (id == PULSAR_NOBEAM) id = s->next_beam++;           // allocate new beam ID
            s->beam_id  [i] = id;
            s->beam_live[i] = 1;
            return id;
        }
    }
    kprintf("PULSAR: beam_alloc - table full (max %u)\n", (uint32_t)PULSAR_BEAM_MAX);
    return PULSAR_NOBEAM;
}

// release beam: remove beam ID from local table
void pulsar_beam_free(pulsar_session_t *s, uint32_t beam) {

    for (uint32_t i = 0; i < PULSAR_BEAM_MAX; i++) {                // loop through beams
        if (s->beam_live[i] && s->beam_id[i] == beam) {
            s->beam_live[i] = 0;                                    // remove local beam
            return;
        }
    }
}

// release beam: network operation
int pulsar_release(pulsar_session_t *s, uint32_t beam) {

    uint16_t tag = alloc_tag(s);                                    // allocate tag
    uint32_t pos = 0;

    pulse_begin(s->send_buf, EMIT_RELEASE, tag, &pos);               // build message
    put_u32(s->send_buf, &pos, beam);

    if (pulse_send(s, s->send_buf, pos) < 0) return -1;              // send request

    int len = pulse_recv(s, s->recv_buf);                            // recieve reply
    if (pulse_check(s->recv_buf, len, ECHO_RELEASE, tag) < 0) return -1;

    pulsar_beam_free(s, beam);                                      // free local beam
    return 0;
}

// create pulsar session
pulsar_session_t *pulsar_session_create(file_t *srv_file) {

    pulsar_session_t *s = kmalloc(sizeof(pulsar_session_t));                            // allocate session
    if (!s) return 0;

    for (uint32_t i = 0; i < sizeof(pulsar_session_t); i++) ((uint8_t *)s)[i] = 0;      // zero memory

    // initialise fields
    s->srv_file  = srv_file;
    s->msize     = PULSAR_MSIZE;
    s->next_tag  = 1;
    s->next_beam = 1;
    s->attached  = 0;
    s->root_beam = PULSAR_NOBEAM;

    // EMIT_HAIL (check version & msize)
    uint32_t pos = 0;
    pulse_begin(s->send_buf, EMIT_HAIL, PULSAR_NOTAG, &pos);

    put_u32(s->send_buf, &pos, PULSAR_MSIZE);
    put_str(s->send_buf, &pos, PULSAR_VERSION);

    if (pulse_send(s, s->send_buf, pos) < 0) {
        kprintf("PULSAR: session_create - EMIT_HAIL send failed\n");
        kfree(s);
        return 0;
    }

    int len = pulse_recv(s, s->recv_buf);
    if (pulse_check(s->recv_buf, len, ECHO_HAIL, PULSAR_NOTAG) < 0) {
        kprintf("PULSAR: session_create - ECHO_HAIL failed\n");
        kfree(s);
        return 0;
    }

    // parse ECHO_HAIL body
    pos = PULSAR_HDR;
    uint32_t server_msize = get_u32(s->recv_buf, &pos);                                  // parsing msize
    char     ver[16];                                                                   // parsing version
    get_str(s->recv_buf, &pos, ver, sizeof(ver));


    // accept smaller of two msizes
    if (server_msize < s->msize) s->msize = server_msize;

    kprintf("PULSAR: session created - version=\"%s\" msize=%u\n", ver, s->msize);
    return s;
}

// attach pulsar session to filesystem root
int pulsar_session_attach(pulsar_session_t *s, const char *aname) {

    if (!s) return -1;

    uint32_t root = pulsar_beam_alloc(s);                                           // allocate root beam
    if (root == PULSAR_NOBEAM) return -1;

    uint16_t tag = alloc_tag(s);
    uint32_t pos = 0;
    
    pulse_begin(s->send_buf, EMIT_DOCK, tag, &pos);                                  // EMIT_DOCK: build request
    put_u32(s->send_buf, &pos, root);                                                // root beam fid
    put_u32(s->send_buf, &pos, PULSAR_NOBEAM);                                       // authentication fid = NOBEAM (no auth)
    put_str(s->send_buf, &pos, "");                                                  // uname (kernel mounts as "")
    put_str(s->send_buf, &pos, aname ? aname : "");                                  // aname (filesystem name)

    if (pulse_send(s, s->send_buf, pos) < 0) {
        pulsar_beam_free(s, root);
        return -1;
    }

    int len = pulse_recv(s, s->recv_buf);
    if (pulse_check(s->recv_buf, len, ECHO_DOCK, tag) < 0) {                         // ECHO_DOCK = returns SIGNAT
        pulsar_beam_free(s, root);
        return -1;
    }

    // parse ECHO_DOCK body
    pos = PULSAR_HDR;
    pulsar_signat_t sig;
    get_signat(s->recv_buf, &pos, &sig);
    

    s->root_beam = root;                                                            // update session
    s->attached  = 1;

    kprintf("PULSAR: attached - root_beam=%u signat.path=0x%p\n", root, (uint32_t)(uint32_t)sig.path);
    return 0;
}

// destroy pulsar session 
void pulsar_session_destroy(pulsar_session_t *s) {
 
    if (!s) return;

    for (uint32_t i = 0; i < PULSAR_BEAM_MAX; i++) {            // loop through beams
        if (s->beam_live[i]) pulsar_release(s, s->beam_id[i]);  // release all beams
    }

    kprintf("PULSAR: session destroyed\n");
    kfree(s);
}

// PULSAR server-side beam table

#define SRV_BEAM_MAX  32                                        // max # of beams a connected client can have open at one time

// srv_beam_t: beams (file identifiers in 9P2000)
typedef struct {
    uint32_t id;
    char     path[VFS_PATH_MAX];                                // absolute path on server
    uint8_t  is_dir;
    uint8_t  opened;
    uint8_t  live;
    uint8_t *dir_cache;                                         // stores = flat text file directories -> byte stream serialised file metadata
    uint32_t dir_cache_len;
} srv_beam_t;

// pulsar_svr_t: state of a single client's session
typedef struct {
    srv_beam_t beams[SRV_BEAM_MAX];
    uint32_t   msize;
    uint8_t    send_buf[PULSAR_MSIZE];
    uint8_t    recv_buf[PULSAR_MSIZE];
} pulsar_srv_t;

// find active beam: scan client beams array
static srv_beam_t *srv_get(pulsar_srv_t *s, uint32_t id) {

    for (int i = 0; i < SRV_BEAM_MAX; i++) {
        if (s->beams[i].live && s->beams[i].id == id) {     // if beam == live
            return &s->beams[i];
        }
    }
    return 0;
}

// find inactive beam: mark active
static srv_beam_t *srv_alloc(pulsar_srv_t *s, uint32_t id) {

    if (srv_get(s, id)) return 0;

    for (int i = 0; i < SRV_BEAM_MAX; i++) {
        if (!s->beams[i].live) {
            memset(&s->beams[i], 0, sizeof(s->beams[i]));       // wipe clean
            s->beams[i].live = 1;                               // mark active
            s->beams[i].id   = id;                              // assign ID
            return &s->beams[i];
        }
    }
    return 0;
}

// mark beam free
static void srv_free(srv_beam_t *b) {
    if (!b) return;
    if (b->dir_cache) { kfree(b->dir_cache); b->dir_cache = 0; }
    b->live = 0;
}


// Send an ANOMALY error response
static void srv_error(pulsar_session_t *ps, uint8_t *send_buf, uint16_t tag, const char *msg) {
    uint32_t pos = 0;
    pulse_begin(send_buf, ECHO_ANOMALY, tag, &pos);
    put_str(send_buf, &pos, msg);
    pulse_send(ps, send_buf, pos);
}

// standard file metadata -> byte aligned format
static uint32_t srv_build_stat(const char *name, uint8_t is_dir, uint64_t file_size, uint8_t *out, uint32_t out_max) {

    uint8_t  tmp[256];                                                              // inner size
    uint32_t p = 0;

    tmp[p++] = 0; tmp[p++] = 0;                                                     // inner-size placeholder [2]
    tmp[p++] = 0; tmp[p++] = 0;                                                     // type[2] dev[4]
    tmp[p++] = 0; tmp[p++] = 0; tmp[p++] = 0; tmp[p++] = 0;
    tmp[p++] = is_dir ? SIGNAT_DIR : 0;                                             // qid: type[1] vers[4] path[8]
    tmp[p++] = 0; tmp[p++] = 0; tmp[p++] = 0; tmp[p++] = 0;                         // vers

    uint64_t qpath = (uint64_t)(uintptr_t)name;                                     // unique per session
    for (int i = 0; i < 8; i++) tmp[p++] = (uint8_t)(qpath >> (i * 8));

    uint32_t mode = is_dir ? (0x80000000u | 0755u) : 0644u;                         // mode = directory flag + permissions
    tmp[p++] = mode; tmp[p++] = mode>>8; tmp[p++] = mode>>16; tmp[p++] = mode>>24;

    uint32_t ts = timer_get_ticks();                                                // fake timestamps
    for (int i = 0; i < 2; i++) {
        tmp[p++] = ts; tmp[p++] = ts>>8; tmp[p++] = ts>>16; tmp[p++] = ts>>24;
    }

    for (int i = 0; i < 8; i++) tmp[p++] = (uint8_t)(file_size >> (i * 8));         // file size

    uint16_t nlen = (uint16_t)strlen(name);                                         // name
    tmp[p++] = nlen; tmp[p++] = nlen >> 8;
    for (uint16_t i = 0; i < nlen; i++) tmp[p++] = (uint8_t)name[i];

    tmp[p++] = 0; tmp[p++] = 0;
    tmp[p++] = 0; tmp[p++] = 0;
    tmp[p++] = 0; tmp[p++] = 0;

    uint16_t inner = (uint16_t)(p - 2);                                             // fill inner size (everything after 2-byte size field)
    tmp[0] = inner; tmp[1] = inner >> 8;

    uint32_t copy = (p < out_max) ? p : out_max;
    memcpy(out, tmp, copy);
    return copy;
}

// build flattened byte stream of directory entries
static uint8_t *srv_build_dir_cache(const char *dir_path, uint32_t *out_len) {

    uint8_t  *cache = 0;                                                                        // growing buffer
    uint32_t  cap   = 0;                                                                        // allocated size
    uint32_t  used  = 0;                                                                        // used buffer

    for (uint32_t idx = 0; ; idx++) {                                                           // loop through directory entries

        char     name[VFS_NAME_MAX];
        vnode_t *child = 0;

        file_t *dir_f = vfs_open(dir_path, O_RDONLY);
        if (!dir_f) break;

        int r = vfs_readdir(dir_f, idx, name, VFS_NAME_MAX, &child);                                    // read entry name
        vfs_close(dir_f);
        if (r < 0) break;

        char child_path[VFS_PATH_MAX];
        if (strcmp(dir_path, "/") == 0) {                                                       // build full child path
            strncpy(child_path, "/", sizeof(child_path)-1);
            strncat(child_path, name,  sizeof(child_path)-1 - strlen(child_path));
        } else {
            strncpy(child_path, dir_path, sizeof(child_path)-1);
            strncat(child_path, "/",      sizeof(child_path)-1 - strlen(child_path));
            strncat(child_path, name,     sizeof(child_path)-1 - strlen(child_path));
        }
        (void)child;

        uint8_t is_dir  = 0;                                                                    // determine file type and size
        uint64_t fsize  = 0;
        file_t *cf = vfs_open(child_path, O_RDONLY);
        
        if (cf) {
            is_dir = (uint8_t)(cf->vnode && cf->vnode->type == VNODE_DIR);
            if (!is_dir) {
                uint8_t tmp[4];
                int nr = vfs_read(cf, tmp, sizeof(tmp));
                if (nr > 0) fsize = (uint64_t)nr;   /* rough: real OS tracks size */
            }
            vfs_close(cf);
        }

        uint8_t stat_buf[256];
        uint32_t stat_len = srv_build_stat(name, is_dir, fsize, stat_buf, sizeof(stat_buf));    // build stat entry

        if (used + stat_len > cap) {                                                            // grow buffer if needed
            cap = (cap + stat_len + 512) * 2;
            uint8_t *nb = kmalloc(cap);
            if (!nb) break;
            if (cache) { memcpy(nb, cache, used); kfree(cache); }
            cache = nb;
        }
        memcpy(cache + used, stat_buf, stat_len);                                               // copy stat into buffer
        used += stat_len;
    }
    *out_len = used;
    return cache;
}


// serve one connected client until it disconnects
void pulsar_serve_session(file_t *data_f) {

    pulsar_session_t ps;                                                            // Reuse the client-side pulse_send/pulse_recv via a fake session
    memset(&ps, 0, sizeof(ps));
    ps.srv_file  = data_f;
    ps.msize     = PULSAR_MSIZE;
    ps.next_tag  = 1;

    pulsar_srv_t srv;                                                               // holds beam table, buffers, msize
    memset(&srv, 0, sizeof(srv));
    srv.msize = PULSAR_MSIZE;

    // ------------------------------------------------------------ EMIT_HAIL ------------------------------------------------------------ //

    int len = pulse_recv(&ps, srv.recv_buf);                                        // recieve message
    if (len < PULSAR_HDR) { kprintf("PULSAR-SRV: no HAIL\n"); return; }

    uint32_t pos  = 4;
    uint8_t  type = get_u8 (srv.recv_buf, &pos);                                    // parse header
    uint16_t tag  = get_u16(srv.recv_buf, &pos);

    if (type != EMIT_HAIL) {                                                        // validate hail
        kprintf("PULSAR-SRV: expected HAIL got %u\n", type); return;
    }

    uint32_t cli_msize = get_u32(srv.recv_buf, &pos);                               // extract client parameters
    char     cli_ver[16]; get_str(srv.recv_buf, &pos, cli_ver, sizeof(cli_ver));

    srv.msize = ps.msize = (cli_msize < PULSAR_MSIZE) ? cli_msize : PULSAR_MSIZE;   // negotiate size

    pos = 0;
    pulse_begin(srv.send_buf, ECHO_HAIL, PULSAR_NOTAG, &pos);                       // send response
    put_u32(srv.send_buf, &pos, srv.msize);
    put_str(srv.send_buf, &pos, PULSAR_VERSION);
    if (pulse_send(&ps, srv.send_buf, pos) < 0) return;
    kprintf("PULSAR-SRV: HAIL ok  ver=%s msize=%u\n", cli_ver, srv.msize);

    // ------------------------------------------------------------ MAIN MESSAGE LOOP ------------------------------------------------------------ //

    for (;;) {                                                                      // read request -> decode type -> dispatch to handler

        len = pulse_recv(&ps, srv.recv_buf);
        if (len < PULSAR_HDR) {
            kprintf("PULSAR-SRV: client disconnected\n"); break;
        }

        pos  = 4;
        type = get_u8 (srv.recv_buf, &pos);
        tag  = get_u16(srv.recv_buf, &pos);

        switch (type) {

        // ---------------------------------------- EMIT_DOCK: attach client -> root directory ---------------------------------------- //
        case EMIT_DOCK: {

            uint32_t root_id = get_u32(srv.recv_buf, &pos);                         // read inputs
            get_u32(srv.recv_buf, &pos);
            char tmp[64];
            get_str(srv.recv_buf, &pos, tmp, sizeof(tmp));
            get_str(srv.recv_buf, &pos, tmp, sizeof(tmp));

            srv_beam_t *b = srv_alloc(&srv, root_id);                               // create beam
            if (!b) {
                srv_error(&ps, srv.send_buf, tag, "no beams");
                break;
            }

            strncpy(b->path, "/", VFS_PATH_MAX - 1);                                // initialise beam
            b->is_dir = 1;

            pos = 0;
            pulse_begin(srv.send_buf, ECHO_DOCK, tag, &pos);                        // send response
            pulsar_signat_t sig = { SIGNAT_DIR, 0, 1 };
            put_signat(srv.send_buf, &pos, &sig);
            pulse_send(&ps, srv.send_buf, pos);
            kprintf("PULSAR-SRV: DOCK root_beam=%u\n", root_id);
            break;
        }

        // ---------------------------------------- EMIT_TRAVERSE: walk path components ---------------------------------------- //
        case EMIT_TRAVERSE: {

            uint32_t src_id  = get_u32(srv.recv_buf, &pos);                         // read inputs
            uint32_t new_id  = get_u32(srv.recv_buf, &pos);
            uint16_t ncomp   = get_u16(srv.recv_buf, &pos);

            srv_beam_t *src = srv_get(&srv, src_id);                                // lookup source
            if (!src) { srv_error(&ps, srv.send_buf, tag, "bad src beam"); break; }

            char     new_path[VFS_PATH_MAX];                                        // initialise working state
            strncpy(new_path, src->path, VFS_PATH_MAX - 1);

            pulsar_signat_t qids[16];
            uint16_t nwqid = 0;
            uint8_t  is_dir = src->is_dir;
            int      walk_ok = 1;

            for (uint16_t i = 0; i < ncomp && nwqid < 16; i++) {                    // walk loop

                char comp[VFS_NAME_MAX];
                get_str(srv.recv_buf, &pos, comp, sizeof(comp));                    // read component

                char try_path[VFS_PATH_MAX];
                if (strcmp(new_path, "/") == 0) {                                   // build candidate path
                    strncpy(try_path, "/", sizeof(try_path)-1);
                    strncat(try_path, comp, sizeof(try_path)-1 - strlen(try_path));
                } else {
                    strncpy(try_path, new_path, sizeof(try_path)-1);
                    strncat(try_path, "/",  sizeof(try_path)-1 - strlen(try_path));
                    strncat(try_path, comp, sizeof(try_path)-1 - strlen(try_path));
                }

                file_t *tf = vfs_open(try_path, O_RDONLY);                          // check existence
                if (!tf) { walk_ok = 0; break; }                                    // fail = partial walk

                is_dir = (uint8_t)(tf->vnode && tf->vnode->type == VNODE_DIR);      // determine type
                vfs_close(tf);

                strncpy(new_path, try_path, VFS_PATH_MAX - 1);                      // update path

                qids[nwqid].type = is_dir ? SIGNAT_DIR : 0;                         // build QID (unique file id)
                qids[nwqid].vers = 0;
                qids[nwqid].path = (uint64_t)(uintptr_t)new_path ^ nwqid;
                nwqid++;
            }

            if (walk_ok && nwqid == ncomp) {                                        // allocate new beam (if fully successful)
                srv_beam_t *nb = srv_alloc(&srv, new_id);
                if (nb) {
                    strncpy(nb->path, new_path, VFS_PATH_MAX - 1);
                    nb->is_dir = is_dir;
                }
            }

            pos = 0;
            pulse_begin(srv.send_buf, ECHO_TRAVERSE, tag, &pos);
            put_u16(srv.send_buf, &pos, nwqid);                                     // send response
            for (uint16_t i = 0; i < nwqid; i++)
                put_signat(srv.send_buf, &pos, &qids[i]);
            pulse_send(&ps, srv.send_buf, pos);
            break;
        }

        // ---------------------------------------- EMIT_OPEN: open a file ---------------------------------------- //
        case EMIT_OPEN: {

            uint32_t beam_id = get_u32(srv.recv_buf, &pos);                                     // read input
            get_u8(srv.recv_buf, &pos);

            srv_beam_t *b = srv_get(&srv, beam_id);                                             // lookup beam
            if (!b) {
                srv_error(&ps, srv.send_buf, tag, "bad beam");
                break;
            }

            b->opened = 1;                                                                      // mark opened

            pos = 0;
            pulse_begin(srv.send_buf, ECHO_OPEN, tag, &pos);
            pulsar_signat_t sig = { b->is_dir ? SIGNAT_DIR : 0, 0, (uint64_t)(uintptr_t)b };    // send response
            put_signat(srv.send_buf, &pos, &sig);
            put_u32(srv.send_buf, &pos, srv.msize - PULSAR_HDR - 11);                           // iounit: max bytes per read/write
            pulse_send(&ps, srv.send_buf, pos);
            break;
        }

        // ---------------------------------------- EMIT_READ: read a file/directory ---------------------------------------- //
        case EMIT_READ: {

            uint32_t beam_id = get_u32(srv.recv_buf, &pos);                             // read inputs
            uint64_t offset  = get_u64(srv.recv_buf, &pos);
            uint32_t count   = get_u32(srv.recv_buf, &pos);

            srv_beam_t *b = srv_get(&srv, beam_id);                                     // lookup beam
            if (!b || !b->opened) {
                srv_error(&ps, srv.send_buf, tag, "bad beam");
                break;
            }

            uint32_t max_data = srv.msize - (PULSAR_HDR + 4);                           // clamp size
            if (count > max_data) count = max_data;

            uint8_t tmp[PULSAR_MSIZE];
            int     nr = 0;

            if (b->is_dir) {                                                            // case 1: DIRECTORY

                if (!b->dir_cache) {                                                    // build cache lazily
                    b->dir_cache = srv_build_dir_cache(b->path, &b->dir_cache_len);
                }

                uint32_t off = (uint32_t)offset;                                        // apply offset
                if (b->dir_cache && off < b->dir_cache_len) {
                    uint32_t avail = b->dir_cache_len - off;
                    nr = (int)((avail < count) ? avail : count);
                    memcpy(tmp, b->dir_cache + off, (uint32_t)nr);                      // copy data
                }

            } else {                                                                    // case 2: FILE

                file_t *rf = vfs_open(b->path, O_RDONLY);                               // open file
                if (rf) {

                    uint32_t skip = (uint32_t)offset;
                    uint8_t  junk[64];

                    while (skip > 0) {                                                  // seek by reading and discarding
                        uint32_t chunk = (skip < sizeof(junk)) ? skip : sizeof(junk);
                        int sk = vfs_read(rf, junk, chunk);
                        if (sk <= 0) break;
                        skip -= (uint32_t)sk;
                    }

                    nr = vfs_read(rf, tmp, count);                                      // read data
                    if (nr < 0) nr = 0;
                    vfs_close(rf);
                }
            }

            pos = 0;
            pulse_begin(srv.send_buf, ECHO_READ, tag, &pos);
            put_u32(srv.send_buf, &pos, (uint32_t)nr);                                  // send response
            memcpy(srv.send_buf + pos, tmp, (uint32_t)nr);
            pos += (uint32_t)nr;
            pulse_send(&ps, srv.send_buf, pos);
            break;
        }

        // ---------------------------------------- EMIT_WRITE: write client data -> file ---------------------------------------- //
        case EMIT_WRITE: {

            uint32_t beam_id = get_u32(srv.recv_buf, &pos);                                 // read inputs
            get_u64(srv.recv_buf, &pos);
            uint32_t count = get_u32(srv.recv_buf, &pos);

            srv_beam_t *b = srv_get(&srv, beam_id);                                         // lookup beam
            if (!b || !b->opened) {
                srv_error(&ps, srv.send_buf, tag, "bad beam");
                break;
            }

            int nw = 0;
            file_t *wf = vfs_open(b->path, O_WRONLY);                                       // open file
            if (wf) {
                nw = vfs_write(wf, srv.recv_buf + pos, count);                              // write to file
                if (nw < 0) nw = 0;
                vfs_close(wf);
            }

            pos = 0;
            pulse_begin(srv.send_buf, ECHO_WRITE, tag, &pos);
            put_u32(srv.send_buf, &pos, (uint32_t)nw);                                      // return bytes written
            pulse_send(&ps, srv.send_buf, pos);
            break;
        }

        // ---------------------------------------- EMIT_SCAN (stat): return file metadata ---------------------------------------- //
        case EMIT_SCAN: {

            uint32_t beam_id = get_u32(srv.recv_buf, &pos);
            srv_beam_t *b = srv_get(&srv, beam_id);                                         // lookup beam
            if (!b) { srv_error(&ps, srv.send_buf, tag, "bad beam"); break; }

            const char *name = b->path;                                                     // extract name
            for (const char *p = b->path; *p; p++) if (*p == '/') name = p + 1;
            if (!*name) name = "/";

            uint8_t stat_buf[256];
            uint32_t slen = srv_build_stat(name, b->is_dir, 0, stat_buf, sizeof(stat_buf)); // build stat

            pos = 0;
            pulse_begin(srv.send_buf, ECHO_SCAN, tag, &pos);
            put_u16(srv.send_buf, &pos, (uint16_t)slen);                                    // send [length][stat data]
            memcpy(srv.send_buf + pos, stat_buf, slen);
            pos += slen;
            pulse_send(&ps, srv.send_buf, pos);
            break;
        }

        // ---------------------------------------- EMIT_RELEASE: destroy beam ---------------------------------------- //
        case EMIT_RELEASE: {

            uint32_t beam_id = get_u32(srv.recv_buf, &pos);                 // read beam ID
            srv_free(srv_get(&srv, beam_id));                               // lookup beam + free beam

            pos = 0;
            pulse_begin(srv.send_buf, ECHO_RELEASE, tag, &pos);             // send response
            pulse_send(&ps, srv.send_buf, pos);
            break;
        }

        default:
            kprintf("PULSAR-SRV: unknown type=%u tag=%u\n", type, tag);
            srv_error(&ps, srv.send_buf, tag, "unknown message");
            break;
        }
    }
    for (int i = 0; i < SRV_BEAM_MAX; i++) {
        srv_free(&srv.beams[i]);
    }
}


// EMIT_TRAVERSE: clone src_beam into new_beam, walking path components
int pulsar_traverse(pulsar_session_t *s, uint32_t src_beam, uint32_t new_beam, const char **components, uint32_t ncomp, pulsar_signat_t *signat_out) {

    uint16_t tag = alloc_tag(s);
    uint32_t pos = 0;
    
    pulse_begin(s->send_buf, EMIT_TRAVERSE, tag, &pos);                          // build request
    put_u32(s->send_buf, &pos, src_beam);                                        // src_beam = starting directory
    put_u32(s->send_buf, &pos, new_beam);                                        // new_beam = destination handle
    put_u16(s->send_buf, &pos, (uint16_t)ncomp);                                 // components

    for (uint32_t i = 0; i < ncomp; i++)
        put_str(s->send_buf, &pos, components[i]);                               // wire as strings

    if (pulse_send(s, s->send_buf, pos) < 0) return -1;

    int len = pulse_recv(s, s->recv_buf);                                        // pulse recieve
    if (pulse_check(s->recv_buf, len, ECHO_TRAVERSE, tag) < 0) return -1;        // return QID for each component walked

    // parse ECHO_TRAVERSE
    pos = PULSAR_HDR;
    uint16_t nwqid = get_u16(s->recv_buf, &pos);                                 // partial walk detection

    if (ncomp > 0 && nwqid != ncomp) {
        kprintf("PULSAR: traverse - partial walk: %u of %u components\n", (uint32_t)nwqid, ncomp);
        return -1;
    }

    // walk all qids
    pulsar_signat_t last;
    last.type = 0; last.vers = 0; last.path = 0;
    for (uint16_t i = 0; i < nwqid; i++)                                        // for each QID
        get_signat(s->recv_buf, &pos, &last);                                    // return last QID
    

    if (signat_out) *signat_out = last;
    return 0;
}

// EMIT_OPEN: open beam for I/O. mode = PULSAR_O*
int pulsar_open(pulsar_session_t *s, uint32_t beam, uint8_t mode, pulsar_signat_t *signat_out) {

    uint16_t tag = alloc_tag(s);
    uint32_t pos = 0;

    
    pulse_begin(s->send_buf, EMIT_OPEN, tag, &pos);                              // build request
    put_u32(s->send_buf, &pos, beam);                                            // return beam
    put_u8 (s->send_buf, &pos, mode);                                            // return mode (0=read | 1=write | 2=read/write | 3=execute)

    if (pulse_send(s, s->send_buf, pos) < 0) return -1;

    int len = pulse_recv(s, s->recv_buf);                                        // pulse recieve
    if (pulse_check(s->recv_buf, len, ECHO_OPEN, tag) < 0) return -1;

    // parse ECHO_OPEN
    pos = PULSAR_HDR;
    pulsar_signat_t sig;
    get_signat(s->recv_buf, &pos, &sig);                                         // return QID
    (void)get_u32(s->recv_buf, &pos);                                            // return iounit (max transfer size read/write)
    

    if (signat_out) *signat_out = sig;
    return 0;
}

// EMIT_READ: read count bytes at offset. returns bytes read or -1
int pulsar_read(pulsar_session_t *s, uint32_t beam, uint32_t offset, void *out, uint32_t count) {

    uint32_t max_data = s->msize - (PULSAR_HDR + 4);                            // cap to what fits in a response message
    if (count > max_data) count = max_data;

    uint16_t tag = alloc_tag(s);
    uint32_t pos = 0;
    
    pulse_begin(s->send_buf, EMIT_READ, tag, &pos);                              // build request
    put_u32(s->send_buf, &pos, beam);                                            // beam
    put_u64(s->send_buf, &pos, (uint64_t)offset);                                // offset
    put_u32(s->send_buf, &pos, count);                                           // count bytes

    if (pulse_send(s, s->send_buf, pos) < 0) return -1;

    int len = pulse_recv(s, s->recv_buf);                                        // pulse recieve
    if (pulse_check(s->recv_buf, len, ECHO_READ, tag) < 0) return -1;

    // parse ECHO_READ
    pos = PULSAR_HDR;
    uint32_t rcount = get_u32(s->recv_buf, &pos);
    if (rcount > count) rcount = count;

    for (uint32_t i = 0; i < rcount; i++)                                       // copy data into output buffer
        ((uint8_t *)out)[i] = s->recv_buf[pos + i];
    

    return (int)rcount;
}

// EMIT_WRITE: write count bytes at offset. Returns bytes written or -1
int pulsar_write(pulsar_session_t *s, uint32_t beam, uint32_t offset, const void *data, uint32_t count) {

    // EMIT_WRITE overhead
    uint32_t max_data = s->msize - (PULSAR_HDR + 16);                       // max write = msize - (header + (beam + offset + count))
    if (count > max_data) count = max_data;

    uint16_t tag = alloc_tag(s);
    uint32_t pos = 0;
    
    pulse_begin(s->send_buf, EMIT_WRITE, tag, &pos);                         // build request
    put_u32(s->send_buf, &pos, beam);                                        // beam
    put_u64(s->send_buf, &pos, (uint64_t)offset);                            // offset
    put_u32(s->send_buf, &pos, count);                                       // count bytes

    for (uint32_t i = 0; i < count; i++)                                    // data copy
        s->send_buf[pos++] = ((const uint8_t *)data)[i];

    if (pulse_send(s, s->send_buf, pos) < 0) return -1;

    int len = pulse_recv(s, s->recv_buf);                                    // pulse recieve
    if (pulse_check(s->recv_buf, len, ECHO_WRITE, tag) < 0) return -1;

    // parse ECHO_WRITE
    pos = PULSAR_HDR;
    return (int)get_u32(s->recv_buf, &pos);
    
}

// EMIT_SCAN: scan a beam - fill name, signat and size (any may = NULL)
int pulsar_scan(pulsar_session_t *s, uint32_t beam, char *name_out, uint32_t name_max, pulsar_signat_t *signat_out, uint64_t *size_out) {

    uint16_t tag = alloc_tag(s);
    uint32_t pos = 0;
    
    pulse_begin(s->send_buf, EMIT_SCAN, tag, &pos);                          // begin request
    put_u32(s->send_buf, &pos, beam);                                        // beam

    if (pulse_send(s, s->send_buf, pos) < 0) return -1;

    int len = pulse_recv(s, s->recv_buf);                                    // pulse recieve
    if (pulse_check(s->recv_buf, len, ECHO_SCAN, tag) < 0) return -1;

    pos = PULSAR_HDR;
    (void)get_u16(s->recv_buf, &pos);                    // outer byte count = count[2]
    (void)get_u16(s->recv_buf, &pos);                    // inner size       = size[2]
    (void)get_u16(s->recv_buf, &pos);                    // type             = type[2]
    (void)get_u32(s->recv_buf, &pos);                    // dev              = dev[4]

    pulsar_signat_t sig;
    get_signat(s->recv_buf, &pos, &sig);                 // QID              = qid[13]

    (void)get_u32(s->recv_buf, &pos);                    // mode             = mode[4]
    (void)get_u32(s->recv_buf, &pos);                    // atime            = atime[4]
    (void)get_u32(s->recv_buf, &pos);                    // mtime            = mtime[4]

    uint64_t fsize = get_u64(s->recv_buf, &pos);         // fsize    = length[8]

    char name_tmp[VFS_NAME_MAX];                        // name
    get_str(s->recv_buf, &pos, name_tmp, VFS_NAME_MAX);
    

    if (name_out && name_max > 0) {
        strncpy(name_out, name_tmp, name_max - 1);
        name_out[name_max - 1] = '\0';
    }

    if (signat_out) *signat_out = sig;
    if (size_out)   *size_out   = fsize;
    return 0;
}

// VFS open: convert to PULSAR modes
static int pulsar_vfs_open(vnode_t *v, int flags) {

    pulsar_vdata_t *vd = (pulsar_vdata_t *)v->data;                     // beam data lookup
    if (!vd) return -1;

    if (vd->beam_opened) return 0;                                      // avoid reopen

    uint8_t mode = PULSAR_OREAD;                                        // translate VFS flags -> PULSAR flags
    if ((flags & O_RDWR)  == O_RDWR) mode  = PULSAR_ORDWR;
    else if (flags & O_WRONLY)       mode  = PULSAR_OWRITE;
    if (flags & O_TRUNC)             mode |= PULSAR_OTRUNC;

    if (pulsar_open(vd->session, vd->beam, mode, 0) < 0) return -1;     // send open protocol

    vd->beam_opened = 1;                                                // mark beam opened
    return 0;
}

// VFS close: convert to PULSAR modes
static int pulsar_vfs_close(vnode_t *v) {

    pulsar_vdata_t *vd = (pulsar_vdata_t *)v->data;                     // beam data lookup
    if (!vd) return 0;

    pulsar_release(vd->session, vd->beam);                              // release the beam on the server side

    if (vd->dir_names) {                                                // free directory name cache
        for (uint32_t i = 0; i < vd->dir_count; i++) {                  // for each name
            if (vd->dir_names[i]) kfree(vd->dir_names[i]);              // free directory name
        }
        kfree(vd->dir_names);
        vd->dir_names = 0;                                              // free array
    }
    kfree(vd);                                                          // free vnode data
    v->data = 0;
    return 0;
}

// VFS read: convert to PULSAR modes
static int pulsar_vfs_read(vnode_t *v, void *buf, uint32_t len, uint32_t offset) {

    pulsar_vdata_t *vd = (pulsar_vdata_t *)v->data;                     // beam data lookup
    if (!vd) return -1;

    if (!vd->beam_opened) {                                             // auto-open if needed
        if (pulsar_open(vd->session, vd->beam, PULSAR_OREAD, 0) < 0)
            return -1;
        vd->beam_opened = 1;
    }
    return pulsar_read(vd->session, vd->beam, offset, buf, len);        // return # of bytes read
}

// VFS write: convert to PULSAR modes
static int pulsar_vfs_write(vnode_t *v, const void *buf, uint32_t len, uint32_t offset) {

    pulsar_vdata_t *vd = (pulsar_vdata_t *)v->data;                     // beam data lookup
    if (!vd) return -1;

    if (!vd->beam_opened) {                                             // auto-open if needed
        if (pulsar_open(vd->session, vd->beam, PULSAR_OWRITE, 0) < 0)
            return -1;
        vd->beam_opened = 1;
    }
    return pulsar_write(vd->session, vd->beam, offset, buf, len);       // return # of bytes written
}
 
// VFS lookup: convert to PULSAR modes
static int pulsar_vfs_lookup(vnode_t *dir, const char *name, vnode_t **out) {

    pulsar_vdata_t *vd = (pulsar_vdata_t *)dir->data;                                   // beam data lookup
    if (!vd || !name || !out) return -1;

    uint32_t new_beam = pulsar_beam_alloc(vd->session);                                 // allocate new beam
    if (new_beam == PULSAR_NOBEAM) return -1;

    const char     *components[1] = { name };
    pulsar_signat_t sig;

    if (pulsar_traverse(vd->session, vd->beam, new_beam, components, 1, &sig) < 0) {    // traverse 
        pulsar_beam_free(vd->session, new_beam);                                        // return QID of new beam
        return -1;
    }
    uint8_t vtype = (sig.type & SIGNAT_DIR) ? VNODE_DIR : VNODE_FILE;                   // determine vnode type
    *out = pulsar_vnode_create(vd->session, new_beam, vtype);                           // create vnode
    return *out ? 0 : -1;
}

// VFS read directories: convert to PULSAR modes
static int pulsar_vfs_readdir(vnode_t *dir, uint32_t index, char *name_out, vnode_t **node_out) {

    pulsar_vdata_t *vd = (pulsar_vdata_t *)dir->data;                                   // beam data lookup
    if (!vd) return -1;

    if (!vd->dir_loaded) {                                                              // lazy directory loading

        if (!vd->beam_opened) {                                                         // auto-open if needed
            if (pulsar_open(vd->session, vd->beam, PULSAR_OREAD, 0) < 0)
                return -1;
            vd->beam_opened = 1;
        }

        // read packed 9P stat records until no more data arrives
        #define DIR_CACHE_MAX 64                                                        // temp cache = temp store names
        char     tmp[DIR_CACHE_MAX][VFS_NAME_MAX];
        uint32_t tmp_count = 0;
        uint32_t read_off  = 0;                                                         // directory read offset
        
        uint8_t  rbuf[PULSAR_MSIZE];                                                    // returned data (local stack buffer is okay here)

        while (tmp_count < DIR_CACHE_MAX) {
            int n = pulsar_read(vd->session, vd->beam, read_off, rbuf, sizeof(rbuf));
            if (n <= 0) break;
            read_off += (uint32_t)n;

            uint32_t p = 0;
            while (p + 2 <= (uint32_t)n && tmp_count < DIR_CACHE_MAX) {                 // parsing buffer

                uint16_t stat_size = (uint16_t)rbuf[p] | ((uint16_t)rbuf[p+1] << 8);    // stat record body size

                if (stat_size < 47) break;                                              // minimum valid stat structure = 47 bytes
                if (p + 2 + stat_size > (uint32_t)n) break;                             // ensure record fits inside buffer

                // offset -> name string = inner_size[2] type[2] dev[4] qid[13] mode[4] atime[4] mtime[4] length[8] = 41 bytes, then name count[2]
                uint32_t name_off = p + 2 + 2 + 2 + 4 + 13 + 4 + 4 + 4 + 8;             // name offset
                if (name_off + 2 > (uint32_t)n) { p += 2 + stat_size; continue; }

                uint16_t nlen = (uint16_t)rbuf[name_off] | ((uint16_t)rbuf[name_off + 1] << 8);     // read name length
                uint32_t src  = name_off + 2;                                                       // read name bytes

                if (src + nlen > (uint32_t)n) { p += 2 + stat_size; continue; }                     // bounds check

                uint32_t copy = nlen < VFS_NAME_MAX - 1 ? nlen : VFS_NAME_MAX - 1;                  // copy name -> temp cache
                for (uint32_t i = 0; i < copy; i++) tmp[tmp_count][i] = (char)rbuf[src + i];
                tmp[tmp_count][copy] = '\0';
                tmp_count++;

                p += 2 + stat_size;                                                                 // advance to next record
            }
        }

        if (tmp_count > 0) {
            vd->dir_names = kmalloc(tmp_count * sizeof(char *));                        // allocate space for name pointers
            if (vd->dir_names) {
                for (uint32_t i = 0; i < tmp_count; i++) {                              // copy each string into heap memory
                    uint32_t slen = (uint32_t)strlen(tmp[i]) + 1;
                    vd->dir_names[i] = kmalloc(slen);
                    if (vd->dir_names[i]) strncpy(vd->dir_names[i], tmp[i], slen);
                }
                vd->dir_count = tmp_count;                                              // count # of entries
            }
        }
        vd->dir_loaded = 1;                                                             // directory contents cached
        #undef DIR_CACHE_MAX                                                            // undef temp cache
    }

    if (index >= vd->dir_count) return -1;                                              // return directory entries
    if (!vd->dir_names || !vd->dir_names[index]) return -1;

    if (name_out) {
        strncpy(name_out, vd->dir_names[index], VFS_NAME_MAX - 1);
        name_out[VFS_NAME_MAX - 1] = '\0';
    }

    if (node_out) {
        vnode_t *child = 0;
        pulsar_vfs_lookup(dir, vd->dir_names[index], &child);                           // return child vnode
        *node_out = child;
    }
    return 0;
}

// VFS operations table (for vnodes belonging to PULSAR)
static vfs_ops_t pulsar_ops = {
    .open    = pulsar_vfs_open,
    .close   = pulsar_vfs_close,
    .read    = pulsar_vfs_read,
    .write   = pulsar_vfs_write,
    .lookup  = pulsar_vfs_lookup,
    .create  = 0,                   // TODO(pulsar): EMIT_CREATE
    .mkdir   = 0,                   // TODO(pulsar): EMIT_CREATE with DMDIR flag
    .unlink  = 0,                   // TODO(pulsar): EMIT_REMOVE
    .readdir = pulsar_vfs_readdir,
};

// allocate PULSAR vnode: convert PULSAR beam (FID) -> VFS node
vnode_t *pulsar_vnode_create(pulsar_session_t *s, uint32_t beam, uint8_t vnode_type) {

    pulsar_vdata_t *vd = kmalloc(sizeof(pulsar_vdata_t));   // pulsar metadata lookup
    if (!vd) return 0;

    // initialise fields
    vd->session     = s;        // session
    vd->beam        = beam;     // beam
    vd->beam_opened = 0;        // beam opened?
    vd->dir_names   = 0;        // directory names
    vd->dir_count   = 0;        // directory count
    vd->dir_loaded  = 0;        // directory loaded

    vnode_t *v = vnode_alloc(vnode_type, &pulsar_ops, vd);  // allocate new VFS vnode
    if (!v) { kfree(vd); return 0; }
    return v;
}

// pulsar_connect
int pulsar_connect(const char *addr, const char *mount_path, uint8_t ns_flags) {
 
    if (!addr || !mount_path) return -1;
 
    pcb_t *p = sched_current();
    if (!p) return -1;
 
    // step 1: allocate a TCP slot via /net/tcp/clone
    file_t *clone_f = vfs_open("/net/tcp/clone", O_RDONLY);
    if (!clone_f) {
        kprintf("PULSAR: connect - cannot open /net/tcp/clone\n");
        return -1;
    }
 
    char slot_str[16];
    int n = vfs_read(clone_f, slot_str, sizeof(slot_str) - 1);
    vfs_close(clone_f);
 
    if (n <= 0) {
        kprintf("PULSAR: connect - clone read failed (no free TCP slots?)\n");
        return -1;
    }
 
    // trim trailing whitespace / newline
    slot_str[n] = '\0';
    int slen = n;
    while (slen > 0 && (slot_str[slen-1] == '\n' || slot_str[slen-1] == '\r' || slot_str[slen-1] == ' '))
        slot_str[--slen] = '\0';
 
    kprintf("PULSAR: connect - TCP slot %s, dialling %s\n", slot_str, addr);
 
    // step 2: connect via /net/tcp/N/ctl
    char ctl_path[VFS_PATH_MAX];
    strncpy(ctl_path, "/net/tcp/", VFS_PATH_MAX - 1);
    strncat(ctl_path, slot_str,    VFS_PATH_MAX - 1 - (uint32_t)strlen(ctl_path));
    strncat(ctl_path, "/ctl",      VFS_PATH_MAX - 1 - (uint32_t)strlen(ctl_path));
    ctl_path[VFS_PATH_MAX - 1] = '\0';
 
    file_t *ctl_f = vfs_open(ctl_path, O_WRONLY);
    if (!ctl_f) {
        kprintf("PULSAR: connect - cannot open %s\n", ctl_path);
        return -1;
    }
 
    // "connect ip!port"
    char cmd[VFS_PATH_MAX];
    strncpy(cmd, "connect ", sizeof(cmd) - 1);
    strncat(cmd, addr,      sizeof(cmd) - 1 - (uint32_t)strlen(cmd));
    cmd[sizeof(cmd) - 1] = '\0';
 
    // write blocks until TCP handshake completes (tcp_connected_cb fires)
    n = vfs_write(ctl_f, cmd, (uint32_t)strlen(cmd));
    vfs_close(ctl_f);
 
    if (n < 0) {
        kprintf("PULSAR: connect - TCP connect to \"%s\" failed\n", addr);
        return -1;
    }
 
    // step 3: open /net/tcp/N/data
    char data_path[VFS_PATH_MAX];
    strncpy(data_path, "/net/tcp/", VFS_PATH_MAX - 1);
    strncat(data_path, slot_str,    VFS_PATH_MAX - 1 - (uint32_t)strlen(data_path));
    strncat(data_path, "/data",     VFS_PATH_MAX - 1 - (uint32_t)strlen(data_path));
    data_path[VFS_PATH_MAX - 1] = '\0';
 
    file_t *data_f = vfs_open(data_path, O_RDWR);
    if (!data_f) {
        kprintf("PULSAR: connect - cannot open %s\n", data_path);
        return -1;
    }
 
    // step 4: PULSAR version negotiation
    pulsar_session_t *s = pulsar_session_create(data_f);
    if (!s) {
        kprintf("PULSAR: connect - session_create failed\n");
        vfs_close(data_f);
        return -1;
    }
 
    // step 5: attach to server root
    if (pulsar_session_attach(s, "") < 0) {
        kprintf("PULSAR: connect - attach failed\n");
        pulsar_session_destroy(s);
        vfs_close(data_f);
        return -1;
    }
 
    // step 6: wrap root beam as a VFS vnode and bind into namespace
    vnode_t *root_vnode = pulsar_vnode_create(s, s->root_beam, VNODE_DIR);
    if (!root_vnode) {
        kprintf("PULSAR: connect - vnode_create failed\n");
        pulsar_session_destroy(s);
        vfs_close(data_f);
        return -1;
    }
 
    char resolved[VFS_PATH_MAX];
    vfs_path_resolve(p->cwd_path, mount_path, resolved);
 
    if (ns_bind(&p->namespace, root_vnode, resolved, ns_flags) < 0) {
        kprintf("PULSAR: connect - ns_bind at \"%s\" failed\n", resolved);
        pulsar_session_destroy(s);
        vfs_close(data_f);
        return -1;
    }
 
    // mark bind entry as a network PULSAR mount (srv_fd = -2) for nsdump
    for (uint32_t i = 0; i < NS_BINDS_MAX; i++) {
        ns_bind_entry_t *e = &p->namespace->binds[i];
        if (e->active && e->vnode == root_vnode
                && strcmp(e->new_path, resolved) == 0) {
            e->srv_fd = -2;     // -2 = network PULSAR (distinguish from -1 = local)
            break;
        }
    }
 
    kprintf("PULSAR: \"%s\" mounted at \"%s\"\n", addr, resolved);
    return 0;
}

// mount PULSAR
int pulsar_mount(int srv_fd, const char *path, uint8_t ns_flags) {

    pcb_t *p = sched_current();                                 // return current process
    if (!p) return -1;

    if (srv_fd < 0 || srv_fd >= FD_MAX) return -1;              // validate fd
    file_t *srv_file = p->fd_table[srv_fd];                     // server file
    if (!srv_file) {
        kprintf("PULSAR: mount - bad fd %d\n", srv_fd);
        return -1;
    }

    pulsar_session_t *s = pulsar_session_create(srv_file);      // create pulsar session
    if (!s) {
        kprintf("PULSAR: mount - session_create failed\n");
        return -1;
    }

    if (pulsar_session_attach(s, "") < 0) {                     // attach to server root
        kprintf("PULSAR: mount - attach failed\n");
        pulsar_session_destroy(s);
        return -1;
    }

    vnode_t *root_vnode = pulsar_vnode_create(s, s->root_beam, VNODE_DIR);  // wrap root beam into a VFS vnode
    if (!root_vnode) {
        pulsar_session_destroy(s);
        return -1;
    }

    char resolved[VFS_PATH_MAX];                                // normalise destination path against cwd
    vfs_path_resolve(p->cwd_path, path, resolved);              // convert: relative path -> absolute path

    if (ns_bind(&p->namespace, root_vnode, resolved, ns_flags) < 0) {       // bind into process namespace
        kprintf("PULSAR: mount - ns_bind failed\n");
        pulsar_session_destroy(s);
        return -1;
    }

    for (uint32_t i = 0; i < NS_BINDS_MAX; i++) {               // tag bind entry so nsdump shows it as PULSAR
        ns_bind_entry_t *e = &p->namespace->binds[i];
        if (e->active && e->vnode == root_vnode && strcmp(e->new_path, resolved) == 0) {
            
            e->srv_fd = srv_fd;                                 // bind metadata
            break;
        }
    }
    kprintf("PULSAR: mounted fd=%d at \"%s\"\n", srv_fd, resolved);
    return 0;
}