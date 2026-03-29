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
        kprintf("PULSAR: session_create — EMIT_HAIL send failed\n");
        kfree(s);
        return 0;
    }

    int len = pulse_recv(s, s->recv_buf);
    if (pulse_check(s->recv_buf, len, ECHO_HAIL, PULSAR_NOTAG) < 0) {
        kprintf("PULSAR: session_create — ECHO_HAIL failed\n");
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

    kprintf("PULSAR: session created — version=\"%s\" msize=%u\n", ver, s->msize);
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

    kprintf("PULSAR: attached — root_beam=%u signat.path=0x%p\n", root, (uint32_t)(uint32_t)sig.path);
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
        kprintf("PULSAR: traverse — partial walk: %u of %u components\n", (uint32_t)nwqid, ncomp);
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