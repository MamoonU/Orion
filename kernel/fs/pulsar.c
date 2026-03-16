// pulsar.c - PULSAR Distributed Filesystem Protocol
 
#include "pulsar.h"
#include "namespace.h"
#include "vfs.h"

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

// shared static buffers
static uint8_t s_send_buf[PULSAR_MSIZE];    // send
static uint8_t s_recv_buf[PULSAR_MSIZE];    // recieve

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
    pulse_begin(s_send_buf, EMIT_RELEASE, tag, &pos);               // build message
    put_u32(s_send_buf, &pos, beam);

    if (pulse_send(s, s_send_buf, pos) < 0) return -1;              // send request

    int len = pulse_recv(s, s_recv_buf);                            // recieve reply
    if (pulse_check(s_recv_buf, len, ECHO_RELEASE, tag) < 0) return -1;

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
    pulse_begin(s_send_buf, EMIT_HAIL, PULSAR_NOTAG, &pos);
    put_u32(s_send_buf, &pos, PULSAR_MSIZE);
    put_str(s_send_buf, &pos, PULSAR_VERSION);

    if (pulse_send(s, s_send_buf, pos) < 0) {
        kprintf("PULSAR: session_create — EMIT_HAIL send failed\n");
        kfree(s);
        return 0;
    }

    int len = pulse_recv(s, s_recv_buf);
    if (pulse_check(s_recv_buf, len, ECHO_HAIL, PULSAR_NOTAG) < 0) {
        kprintf("PULSAR: session_create — ECHO_HAIL failed\n");
        kfree(s);
        return 0;
    }

    // parse ECHO_HAIL body
    pos = PULSAR_HDR;
    uint32_t server_msize = get_u32(s_recv_buf, &pos);                                  // parsing msize
    char     ver[16];                                                                   // parsing version
    get_str(s_recv_buf, &pos, ver, sizeof(ver));

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
    pulse_begin(s_send_buf, EMIT_DOCK, tag, &pos);                                  // EMIT_DOCK: build request
    put_u32(s_send_buf, &pos, root);                                                // root beam fid
    put_u32(s_send_buf, &pos, PULSAR_NOBEAM);                                       // authentication fid = NOBEAM (no auth)
    put_str(s_send_buf, &pos, "");                                                  // uname (kernel mounts as "")
    put_str(s_send_buf, &pos, aname ? aname : "");                                  // aname (filesystem name)

    if (pulse_send(s, s_send_buf, pos) < 0) {
        pulsar_beam_free(s, root);
        return -1;
    }

    int len = pulse_recv(s, s_recv_buf);
    if (pulse_check(s_recv_buf, len, ECHO_DOCK, tag) < 0) {                         // ECHO_DOCK = returns SIGNAT
        pulsar_beam_free(s, root);
        return -1;
    }

    // parse ECHO_DOCK body
    pos = PULSAR_HDR;
    pulsar_signat_t sig;
    get_signat(s_recv_buf, &pos, &sig);

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




}

// EMIT_OPEN: open beam for I/O. mode = PULSAR_O*
int pulsar_open(pulsar_session_t *s, uint32_t beam, uint8_t mode, pulsar_signat_t *signat_out) {




}

// EMIT_READ: read count bytes at offset. returns bytes read or -1
int pulsar_read(pulsar_session_t *s, uint32_t beam, uint32_t offset, void *buf, uint32_t count) {




}

// EMIT_WRITE: write count bytes at offset. Returns bytes written or -1
int pulsar_write(pulsar_session_t *s, uint32_t beam, uint32_t offset, const void *data, uint32_t count) {



}

// EMIT_SCAN: stat a beam - fills name, signat and size (any may = NULL)
int pulsar_scan(pulsar_session_t *s, uint32_t beam, char *name_out, uint32_t name_max, pulsar_signat_t *signat_out, uint64_t *size_out) {




}

// allocate a VFS vnode backed by (session, beam): vnode_type = file/dir
vnode_t *pulsar_vnode_create(pulsar_session_t *s, uint32_t beam, uint8_t vnode_type) {



}

// create session from fd number in the current process's fd table -> attach -> bind server root at path in process namespace
int pulsar_mount(int srv_fd, const char *path, uint8_t ns_flags) {




}
