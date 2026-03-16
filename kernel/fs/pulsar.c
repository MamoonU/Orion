// pulsar.c - PULSAR Distributed Filesystem Protocol
 
#include "pulsar.h"
#include "namespace.h"
#include "vfs.h"




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
