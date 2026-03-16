// namespace.h - Per-Process Namespace (Plan 9 bind semantics)

#ifndef NAMESPACE_H
#define NAMESPACE_H

#include "vfs.h"
#include <stdint.h>

// max bind entries per namespace
#define NS_BINDS_MAX    32                                      // (vs VFS_MOUNT_MAX = 8 for global table)

#define NS_BIND_REPLACE  0                                      // replace: existing binding
#define NS_BIND_BEFORE   1                                      // union: prepend (this vnode searched first)
#define NS_BIND_AFTER    2                                      // union: append  (this vnode searched last)

typedef struct ns ns_t;

// one entry in the bind table
typedef struct ns_bind_entry {

    char      new_path[VFS_PATH_MAX];                               // ns path
    vnode_t  *vnode;                                                // path resolves to

    uint8_t   flags;                                                // NS_BIND_*
    uint8_t   active;                                               // 0 = free, 1 = used

    // 9P hook (future): when srv_fd >= 0 this is a remote mount.
    int       srv_fd;                                               // -1 = local vnode, >= 0 = 9P server fd

} ns_bind_entry_t;

// per-process namespace
struct ns {
    uint32_t         refcount;
    ns_bind_entry_t  binds[NS_BINDS_MAX];                           // bind table
    uint32_t         nbinds;                                        // number of active entries
};

// allocate fresh empty namespace (refcount = 1)
ns_t *ns_create(void);

// deep copy namespace: all active entries cloned, vnodes re-ref'd
ns_t *ns_clone(const ns_t *src);

void  ns_ref  (ns_t *ns);          // increment refcount
void  ns_unref(ns_t *ns);          // decrement refcount (refcount = 0 = free ns)

// bind operation
int ns_bind(ns_t **nsp, vnode_t *vnode, const char *new_path, uint8_t flags);

// unbind operation
int ns_unbind(ns_t **nsp, const char *new_path);

// path resolution 
vnode_t *ns_resolve(ns_t *ns, const char *path);

// handle union directory listing
int ns_readdir(ns_t *ns, const char *dir_path, uint32_t index, char *name_out, uint32_t name_max, vnode_t **node_out);

// debug
void ns_dump(const ns_t *ns);

#endif