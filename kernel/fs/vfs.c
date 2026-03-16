// vfs.c - Virtual File System Core

#include "vfs.h"
#include "kheap.h"
#include "string.h"
#include "kprintf.h"

// mount table - map paths to filesystems
typedef struct {
    char     path[VFS_PATH_MAX];                                            // where filesystem mounted
    vnode_t *root;                                                          // filesystem root vnode
    uint8_t  active;                                                        // mark free slots
} mount_entry_t;

static mount_entry_t mount_table[VFS_MOUNT_MAX];
static uint32_t      mount_count = 0;                                       // track count

// initialise VFS subsystem
void vfs_init(void) {

    for (int i = 0; i < VFS_MOUNT_MAX; i++) {                               // clear mount table
        mount_table[i].active = 0;
        mount_table[i].root   = 0;
    }

    mount_count = 0;
    kprintf("VFS: Initialised\n");

}

// vnode lifecycle
vnode_t *vnode_alloc(uint8_t type, vfs_ops_t *ops, void *data) {

    vnode_t *v = kmalloc(sizeof(vnode_t));                                  // dynamically allocate
    if (!v) return 0;

    v->type     = type;
    v->refcount = 0;
    v->size     = 0;
    v->ops      = ops;
    v->data     = data;

    return v;
}

// reference counting - increment reference
void vnode_ref(vnode_t *v) {
    if (v) v->refcount++;
}

// reference counting - decrement reference
void vnode_unref(vnode_t *v) {
    if (!v) return;
    if (v->refcount > 0) v->refcount--;                                     // vnodes in ramfs are permanent; do not free on zero refcount
}

// attach filesystem tree -> global namespace
int vfs_mount(const char *path, vnode_t *root) {

    if (!path || !root) return -1;

    if (mount_count >= VFS_MOUNT_MAX) {                                     // limit mounts
        kprintf("VFS: vfs_mount — mount table full\n");
        return -1;
    }

    for (uint32_t i = 0; i < VFS_MOUNT_MAX; i++) {                          // find free slot

        if (mount_table[i].active) continue;                                // skip active entries

        strncpy(mount_table[i].path, path, VFS_PATH_MAX - 1);               // copy mount path
        mount_table[i].path[VFS_PATH_MAX - 1] = '\0';                       // guarentee null termination

        mount_table[i].root   = root;                                       // mount
        mount_table[i].active = 1;
        mount_count++;

        kprintf("VFS: mounted \"%s\"\n", path);
        return 0;

    }
    return -1;
}

// find correct filesystem 
static mount_entry_t *find_mount(const char *path) {

    mount_entry_t *best     = 0;                                            // best match
    uint32_t       best_len = 0;                                            // longest best match

    for (uint32_t i = 0; i < VFS_MOUNT_MAX; i++) {

        if (!mount_table[i].active) continue;                               // skip inactive

        const char *mp   = mount_table[i].path;
        uint32_t    mlen = (uint32_t)strlen(mp);                            // return mount path length

        if (strncmp(mp, path, mlen) != 0) continue;                         // prefix check

        int boundary = (mlen == 1 && mp[0] == '/') || (path[mlen] == '/') || (path[mlen] == '\0');      // boundry validation

        if (boundary && mlen > best_len) {                                  // update best match
            best     = &mount_table[i];
            best_len = mlen;
        }

    }
    return best;
}

// convert file path -> vnode 
vnode_t *vfs_resolve(const char *path) {

    if (!path || path[0] != '/') return 0;                              // enforce absolute paths

    mount_entry_t *m = find_mount(path);                                // find filesystem
    if (!m) return 0;

    vnode_t     *cur  = m->root;                                        // traverse from root vnode
    uint32_t     mlen = (uint32_t)strlen(m->path);
    const char  *rest = path + mlen;                                    // skip mount prefix

    while (*rest == '/') rest++;                                        // skip extra slashes after mount prefix
    if (*rest == '\0') return cur;                                      // path points at mount root
    char component[VFS_NAME_MAX];                                       // walk remaining path components

    while (*rest) {                                                     // 1 iteration processes 1 directory component

        uint32_t n = 0;                                                 // extract next name component

        while (*rest && *rest != '/' && n < VFS_NAME_MAX - 1)
            component[n++] = *rest++;
        component[n] = '\0';

        while (*rest == '/') rest++;                                    // skip slashes between components

        if (!cur->ops || !cur->ops->lookup) return 0;

        vnode_t *next = 0;                                              // delegate to filesystem
        if (cur->ops->lookup(cur, component, &next) < 0 || !next) return 0;

        cur = next;
    }
    return cur;
}

// convert vnode -> open file object
file_t *vfs_open_vnode(vnode_t *v, int flags) {

    if (!v) return 0;

    if (v->ops && v->ops->open) {                                       // let filesystem backend react to open
        if (v->ops->open(v, flags) < 0) return 0;
    }

    file_t *f = kmalloc(sizeof(file_t));                                // allocate file object
    if (!f) return 0;

    f->vnode    = v;
    f->offset   = (flags & O_APPEND) ? v->size : 0;                     // append eof
    f->flags    = (uint32_t)flags;
    f->refcount = 1;

    vnode_ref(v);
    return f;
}

// open by path
file_t *vfs_open(const char *path, int flags) {

    if (!path) return 0;

    vnode_t *v = vfs_resolve(path);                                                         // resolve path -> vnode

    if (!v) {                                                                               // create path
        
        if (!(flags & O_CREAT)) return 0;                                                   // create file

        char parent[VFS_PATH_MAX];                                                          // split path into parent directory + new filename
        strncpy(parent, path, VFS_PATH_MAX - 1);
        parent[VFS_PATH_MAX - 1] = '\0';

        int slash = -1;
        for (int i = (int)strlen(parent) - 1; i >= 0; i--) {                                // find parent directory
            if (parent[i] == '/') { slash = i; break; }                                     // scan backwards
        }
        if (slash < 0) return 0;

        const char *filename = path + slash + 1;
        if (slash == 0) parent[1] = '\0';                                                   // parent is root "/"
        else            parent[slash] = '\0';

        vnode_t *dir = vfs_resolve(slash == 0 ? "/" : parent);
        if (!dir || dir->type != VNODE_DIR) return 0;
        if (!dir->ops || !dir->ops->create)  return 0;

        vnode_t *newfile = 0;
        if (dir->ops->create(dir, filename, VNODE_FILE, &newfile) < 0 || !newfile)          // create vnode
            return 0;

        return vfs_open_vnode(newfile, flags);

    }
    return vfs_open_vnode(v, flags);
}

// close vnode
void vfs_close(file_t *f) {

    if (!f) return;

    f->refcount--;                                                      // count refs
    if (f->refcount > 0) return;                                        // still referenced (e.g. shared after fork)

    if (f->vnode && f->vnode->ops && f->vnode->ops->close) f->vnode->ops->close(f->vnode);

    vnode_unref(f->vnode);
    kfree(f);
}

// read vnode
int vfs_read(file_t *f, void *buf, uint32_t len) {

    if (!f || !f->vnode || !buf || !len)                   return -1;
    if (!f->vnode->ops || !f->vnode->ops->read)            return -1;

    int n = f->vnode->ops->read(f->vnode, buf, len, f->offset);
    if (n > 0) f->offset += (uint32_t)n;
    return n;
}

// write
int vfs_write(file_t *f, const void *buf, uint32_t len) {

    if (!f || !f->vnode || !buf || !len)                   return -1;
    if (!f->vnode->ops || !f->vnode->ops->write)           return -1;

    if (f->flags & O_APPEND) f->offset = f->vnode->size;

    int n = f->vnode->ops->write(f->vnode, buf, len, f->offset);
    if (n > 0) {
        f->offset += (uint32_t)n;
        if (f->offset > f->vnode->size)f->vnode->size = f->offset;
    }
    return n;
}

// mkdir
int vfs_mkdir(const char *path) {

    if (!path || path[0] != '/') return -1;

    char parent[VFS_PATH_MAX];
    strncpy(parent, path, VFS_PATH_MAX - 1);
    parent[VFS_PATH_MAX - 1] = '\0';

    int slash = -1;
    for (int i = (int)strlen(parent) - 1; i >= 0; i--) {
        if (parent[i] == '/') { slash = i; break; }
    }
    if (slash < 0) return -1;

    const char *dirname = path + slash + 1;
    if (slash == 0) parent[1] = '\0';
    else            parent[slash] = '\0';

    vnode_t *dir = vfs_resolve(slash == 0 ? "/" : parent);
    if (!dir || dir->type != VNODE_DIR) return -1;
    if (!dir->ops || !dir->ops->mkdir)   return -1;

    vnode_t *newdir = 0;
    return dir->ops->mkdir(dir, dirname, &newdir);
}

// read a single directory entry at the given index
int vfs_readdir(file_t *f, uint32_t index, char *name_out, uint32_t name_max, vnode_t **node_out) {
 
    if (!f || !f->vnode)                               return -1;
    if (f->vnode->type != VNODE_DIR)                   return -1;
    if (!f->vnode->ops || !f->vnode->ops->readdir)     return -1;
 
    // readdir writes into a VFS_NAME_MAX buffer; we then copy with caller's limit
    char tmp[VFS_NAME_MAX];
    tmp[0] = '\0';
 
    vnode_t *child = 0;
    int rc = f->vnode->ops->readdir(f->vnode, index, tmp, &child);
    if (rc < 0) return -1;
 
    if (name_out && name_max > 0) {
        strncpy(name_out, tmp, name_max - 1);
        name_out[name_max - 1] = '\0';
    }
 
    if (node_out) *node_out = child;
    return 0;
}

// Normalise an absolute path string in-place.
// Collapses redundant slashes, resolves '.' and '..' components.
// path must be a writable buffer of at least VFS_PATH_MAX bytes.
static void path_normalise(char *path) {
 
    // component pointer array — points into a scratch copy
    char scratch[VFS_PATH_MAX];
    strncpy(scratch, path, VFS_PATH_MAX - 1);
    scratch[VFS_PATH_MAX - 1] = '\0';
 
    const char *parts[64];
    int         nparts = 0;
 
    char *p = scratch;
    while (*p) {
        while (*p == '/') p++;          // skip leading / repeated slashes
        if (!*p) break;
 
        parts[nparts++] = p;
 
        while (*p && *p != '/') p++;    // walk to end of component
        if (*p) *p++ = '\0';            // null-terminate component
 
        if (nparts >= 64) break;        // safety cap
    }
 
    // resolve . and ..
    const char *out[64];
    int         nout = 0;
 
    for (int i = 0; i < nparts; i++) {
        if (strcmp(parts[i], ".") == 0) {
            continue;                   // skip current-dir marker
        } else if (strcmp(parts[i], "..") == 0) {
            if (nout > 0) nout--;       // pop last component
        } else {
            out[nout++] = parts[i];
        }
    }
 
    // rebuild absolute path
    path[0] = '/';
    path[1] = '\0';
    for (int i = 0; i < nout; i++) {
        if (i > 0) {
            uint32_t l = (uint32_t)strlen(path);
            if (l + 1 < VFS_PATH_MAX) { path[l] = '/'; path[l+1] = '\0'; }
        }
        uint32_t l = (uint32_t)strlen(path);
        strncpy(path + l, out[i], VFS_PATH_MAX - l - 1);
        path[VFS_PATH_MAX - 1] = '\0';
    }
}

// Resolve a path relative to base, producing a normalised absolute path in out.
// If path is already absolute the base is ignored.
// out must point to a VFS_PATH_MAX byte buffer.
void vfs_path_resolve(const char *base, const char *path, char *out) {
 
    if (!path || !out) return;
 
    if (path[0] == '/') {
        // absolute input — just copy and normalise
        strncpy(out, path, VFS_PATH_MAX - 1);
        out[VFS_PATH_MAX - 1] = '\0';
    } else {
        // relative input — prepend base
        strncpy(out, base ? base : "/", VFS_PATH_MAX - 1);
        out[VFS_PATH_MAX - 1] = '\0';
 
        uint32_t blen = (uint32_t)strlen(out);
        if (blen > 0 && out[blen - 1] != '/' && blen + 1 < VFS_PATH_MAX) {
            out[blen]   = '/';
            out[blen+1] = '\0';
            blen++;
        }
        strncpy(out + blen, path, VFS_PATH_MAX - blen - 1);
        out[VFS_PATH_MAX - 1] = '\0';
    }
 
    path_normalise(out);
}