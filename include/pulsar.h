// pulsar.h - PULSAR Distributed Filesystem Protocol

//          9P         |       PULSAR       
// --------------------|--------------------
// MESSAGE             | PULSE
// REQUEST             | EMIT
// RESPONSE            | ECHO
// FID                 | BEAM
// QID                 | SIGNAT
// SESSION             | SESSION

// PULSAR Glossary:
// PULSE    = one protocol message                        (T* or R*)
// EMIT_*   = client -> server request                    (T* in 9P)
// ECHO_*   = server → client response                    (R* in 9P)
// BEAM     = directed handle to a remote file/dir        (fid in 9P)
// SIGNAT   = unique server-side file identity            (qid in 9P)
// SESSION  = one active connection to a PULSAR server

#ifndef PULSAR_H
#define PULSAR_H

#include <stdint.h>
#include "vfs.h"

// protocol constants
#define PULSAR_VERSION      "9P2000"        // wire version (9P2000 compatible)
#define PULSAR_MSIZE        8192            // default max message size
#define PULSAR_NOTAG        0xFFFFu         // reserved tag used in EMIT_HAIL only
#define PULSAR_NOBEAM       0xFFFFFFFFu     // null beam / NOFID
#define PULSAR_BEAM_MAX     64              // max concurrent beams per session
#define PULSAR_ENAME_MAX    128             // max error string length
#define PULSAR_HDR          7               // header size: | size[4] | type[1] | tag[2]

// version negotiation
#define EMIT_HAIL       100     // (Tversion)
#define ECHO_HAIL       101     // (Rversion)

// authentication (future)
#define EMIT_AUTH       102     // (Tauth)
#define ECHO_AUTH       103     // (Rauth)

// attach to server root
#define EMIT_DOCK       104     // (Tattach)
#define ECHO_DOCK       105     // (Rattach)
#define ECHO_ANOMALY    107     // (Rerror) error - server -> client only

// cancel pending pulse
#define EMIT_ABORT      108     // (Tflush)
#define ECHO_ABORT      109     // (Rflush)

// walk directory tree
#define EMIT_TRAVERSE   110     // (Twalk)
#define ECHO_TRAVERSE   111     // (Rwalk)

//open beam for I/O
#define EMIT_OPEN       112     // (Topen)
#define ECHO_OPEN       113     // (Ropen)

// create a new file
#define EMIT_CREATE     114     // (Tcreate)
#define ECHO_CREATE     115     // (Rcreate)

// read data from beam
#define EMIT_READ       116     // (Tread)
#define ECHO_READ       117     // (Rread)

// write data to beam
#define EMIT_WRITE      118     // (Twrite)
#define ECHO_WRITE      119     // (Rwrite)

//close / release a beam
#define EMIT_RELEASE    120     // (Tclunk)
#define ECHO_RELEASE    121     // (Rclunk)

//delete a file
#define EMIT_REMOVE     122     // (Tremove)
#define ECHO_REMOVE     123     // (Rremove)

// stat / read metadata
#define EMIT_SCAN       124     // (Tstat)
#define ECHO_SCAN       125     // (Rstat)

//write metadata
#define EMIT_UPDATE     126     // (Twstat)
#define ECHO_UPDATE     127     // (Rwstat)

// open mode flags
#define PULSAR_OREAD    0x00    // open for reading
#define PULSAR_OWRITE   0x01    // open for writing
#define PULSAR_ORDWR    0x02    // open for read + write
#define PULSAR_OEXEC    0x03    // open for execution
#define PULSAR_OTRUNC   0x10    // truncate on open
#define PULSAR_ORCLOSE  0x40    // remove on last close

// SIGNAT (qid): unique file identity
#define SIGNAT_DIR      0x80    // entry is a directory
#define SIGNAT_APPEND   0x40    // append-only file
#define SIGNAT_EXCL     0x20    // exclusive-use file
#define SIGNAT_FILE     0x00    // regular file

// type[1] vers[4] path[8] = 13 bytes
typedef struct {

    uint8_t  type;              // SIGNAT_* flags
    uint32_t vers;              // version counter: increment when content changes
    uint64_t path;              // server-unique path identifier

} __attribute__((packed)) pulsar_signat_t;

// SESSION
typedef struct pulsar_session {

    file_t   *srv_file;                     // server endpoint (pipe or future socket)
    uint32_t  msize;                        // max message size
    uint16_t  next_tag;                     // tag counter
    uint32_t  next_beam;                    // beam (fid) counter

    // beam table: tracks which fid values are currently allocated
    uint32_t  beam_id  [PULSAR_BEAM_MAX];   // allocated fid values
    uint8_t   beam_live[PULSAR_BEAM_MAX];   // 1 = slot occupied

    uint8_t   attached;                     // 1 = EMIT_DOCK succeeded
    uint32_t  root_beam;                    // beam for the attached root

    uint8_t send_buf[PULSAR_MSIZE];         // send
    uint8_t recv_buf[PULSAR_MSIZE];         // recieve

} pulsar_session_t;

// vnode backing data
typedef struct {

    pulsar_session_t  *session;
    uint32_t          beam;             // fid for this file or directory
    uint8_t           beam_opened;      // 1 = EMIT_OPEN has been sent

    char              **dir_names;      // heap-allocated array of name strings
    uint32_t          dir_count;        // number of cached entries
    uint8_t           dir_loaded;       // 1 = cache is valid

} pulsar_vdata_t;

// session lifecycle
pulsar_session_t    *pulsar_session_create(file_t *srv_file);                       // allocate a session over open file (pipe end): negotiate version and msize
int                 pulsar_session_attach(pulsar_session_t *s, const char *aname);  // send EMIT_DOCK to attach to the server root
void                pulsar_session_destroy(pulsar_session_t *s);                    // release all beams and free session

// beam (fid) management
uint32_t pulsar_beam_alloc (pulsar_session_t *s);
void     pulsar_beam_free  (pulsar_session_t *s, uint32_t beam);
int      pulsar_release    (pulsar_session_t *s, uint32_t beam);

// protocol operations
// EMIT_TRAVERSE: clone src_beam into new_beam, walking path components
int pulsar_traverse(pulsar_session_t *s, uint32_t src_beam, uint32_t new_beam, const char **components, uint32_t ncomp, pulsar_signat_t *signat_out);

// EMIT_OPEN: open beam for I/O. mode = PULSAR_O*
int pulsar_open(pulsar_session_t *s, uint32_t beam, uint8_t mode, pulsar_signat_t *signat_out);

// EMIT_READ: read count bytes at offset. returns bytes read or -1
int pulsar_read(pulsar_session_t *s, uint32_t beam, uint32_t offset, void *buf, uint32_t count);

// EMIT_WRITE: write count bytes at offset. Returns bytes written or -1
int pulsar_write(pulsar_session_t *s, uint32_t beam, uint32_t offset, const void *data, uint32_t count);

// EMIT_SCAN: stat a beam - fills name, signat and size (any may = NULL)
int pulsar_scan(pulsar_session_t *s, uint32_t beam, char *name_out, uint32_t name_max, pulsar_signat_t *signat_out, uint64_t *size_out);

// VFS integration
vnode_t *pulsar_vnode_create(pulsar_session_t *s, uint32_t beam, uint8_t vnode_type);   // allocate a VFS vnode backed by (session, beam): vnode_type = file/dir

// create session from fd number in the current process's fd table -> attach -> bind server root at path in process namespace
int pulsar_mount(int srv_fd, const char *path, uint8_t ns_flags);

// dial a remote PULSAR server over TCP, negotiate protocol, mount at path.
int pulsar_connect(const char *addr, const char *mount_path, uint8_t ns_flags);

// serve one connected client until it disconnects
void pulsar_serve_session(file_t *data_f);

#endif