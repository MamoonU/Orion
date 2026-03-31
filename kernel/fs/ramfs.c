// ramfs.c - In-Memory Filesystem

#include "ramfs.h"
#include "vfs.h"
#include "kheap.h"
#include "string.h"
#include "kprintf.h"

typedef struct ramfs_dirent {                               // directory entry structure

    char                 name[VFS_NAME_MAX];                // filename
    vnode_t             *vnode;                             // pointer to vnode
    struct ramfs_dirent *next;                              // next entry

} ramfs_dirent_t;

// filesystem metadata attached to each vnode
typedef struct {

    // file data
    uint8_t         *buf;
    uint32_t         capacity;

    // directory children (exist = if (vnode = directory))
    ramfs_dirent_t  *children;
    uint32_t         child_count;

} ramfs_inode_t;

// forward declarations
static int ramfs_open     (vnode_t *v, int flags);
static int ramfs_close    (vnode_t *v);
static int ramfs_read     (vnode_t *v, void *buf, uint32_t len, uint32_t offset);
static int ramfs_write    (vnode_t *v, const void *buf, uint32_t len, uint32_t offset);
static int ramfs_lookup   (vnode_t *dir, const char *name, vnode_t **out);
static int ramfs_create   (vnode_t *dir, const char *name, uint8_t type, vnode_t **out);
static int ramfs_mkdir_op (vnode_t *dir, const char *name, vnode_t **out);
static int ramfs_unlink   (vnode_t *dir, const char *name);
static int ramfs_readdir  (vnode_t *dir, uint32_t index, char *name_out, vnode_t **node_out);

// connect filesystem -> VFS
static vfs_ops_t ramfs_ops = {
    .open    = ramfs_open,
    .close   = ramfs_close,
    .read    = ramfs_read,
    .write   = ramfs_write,
    .lookup  = ramfs_lookup,
    .create  = ramfs_create,
    .mkdir   = ramfs_mkdir_op,
    .unlink  = ramfs_unlink,
    .readdir = ramfs_readdir,
};

static vnode_t *ramfs_root = 0;

// construct new vnode
static vnode_t *ramfs_new_vnode(uint8_t type) {

    ramfs_inode_t *ino = kmalloc(sizeof(ramfs_inode_t));            // allocate inode data
    if (!ino) return 0;
    ino->buf         = 0;
    ino->capacity    = 0;
    ino->children    = 0;
    ino->child_count = 0;

    vnode_t *v = vnode_alloc(type, &ramfs_ops, ino);                // vnode creation
    if (!v) { kfree(ino); return 0; }
    return v;
}

// open files
static int ramfs_open(vnode_t *v, int flags) {

    if ((flags & O_TRUNC) && v->type == VNODE_FILE) {               // handle O_TRUNC
        ramfs_inode_t *ino = v->data;

        ino->capacity = 0;                                          // reset file state
        v->size       = 0;
        if (ino->buf) { kfree(ino->buf); ino->buf = 0; }            // free prev buffer
    }

    return 0;
}

// close files
static int ramfs_close(vnode_t *v) {
    (void)v;
    return 0;
}

// read files
static int ramfs_read(vnode_t *v, void *buf, uint32_t len, uint32_t offset) {

    if (v->type != VNODE_FILE) return -1;                                           // reject directory vnodes
    ramfs_inode_t *ino = v->data;

    if (offset >= v->size) return 0;                                                // EOF

    uint32_t avail = v->size - offset;                                              // readable bytes
    uint32_t n     = (len < avail) ? len : avail;

    memcpy(buf, ino->buf + offset, n);                                              // copy data -> file memory buffer
    return (int)n;
}

// write files
static int ramfs_write(vnode_t *v, const void *buf, uint32_t len, uint32_t offset) {

    if (v->type != VNODE_FILE) return -1;                                           // reject directory vnodes
    ramfs_inode_t *ino = v->data;

    uint32_t end = offset + len;                                                    // determine required size

    // grow buffer if needed (double each time)
    if (end > ino->capacity) {

        uint32_t new_cap = ino->capacity ? ino->capacity : 64;                      // grow exponentially
        while (new_cap < end)
            new_cap *= 2;

        uint8_t *new_buf = kmalloc(new_cap);                                        // allocate new buffer
        if (!new_buf) return -1;

        if (ino->buf && v->size)                                                    // preserve existing data
            memcpy(new_buf, ino->buf, v->size);

        if (ino->buf)                                                               // free old buffer
            kfree(ino->buf);

        ino->buf      = new_buf;
        ino->capacity = new_cap;

    }
    memcpy(ino->buf + offset, buf, len);                                    // write
    if (end > v->size) v->size = end;                                       // extend file
    return (int)len;
}

// directory lookup
static int ramfs_lookup(vnode_t *dir, const char *name, vnode_t **out) {

    if (dir->type != VNODE_DIR) return -1;                                  // directory vnodes only
    ramfs_inode_t *ino = dir->data;

    for (ramfs_dirent_t *d = ino->children; d; d = d->next) {               // for each entry
        if (strcmp(d->name, name) == 0) {                                   // if name matches
            *out = d->vnode;                                                // return vnode
            return 0;
        }
    }
    return -1;
}

// add directory entry
static int ramfs_add_dirent(vnode_t *dir, const char *name, vnode_t *child) {

    ramfs_inode_t *ino = dir->data;

    ramfs_dirent_t *d = kmalloc(sizeof(ramfs_dirent_t));                    // allocate entry
    if (!d) return -1;

    strncpy(d->name, name, VFS_NAME_MAX - 1);                               // copy name
    d->name[VFS_NAME_MAX - 1] = '\0';
    d->vnode = child;

    d->next  = ino->children;                                               // insert into linked list
    ino->children = d;

    ino->child_count++;
    return 0;
}

// create files
static int ramfs_create(vnode_t *dir, const char *name, uint8_t type, vnode_t **out) {

    if (dir->type != VNODE_DIR) return -1;

    // if already exist, hand back to existing vnode
    vnode_t *existing = 0;
    if (ramfs_lookup(dir, name, &existing) == 0) {
        *out = existing;
        return 0;
    }

    vnode_t *child = ramfs_new_vnode(type);
    if (!child) return -1;

    if (ramfs_add_dirent(dir, name, child) < 0) {
        kfree(child->data);
        kfree(child);
        return -1;
    }

    *out = child;
    return 0;
}

// make directory
static int ramfs_mkdir_op(vnode_t *dir, const char *name, vnode_t **out) {
    return ramfs_create(dir, name, VNODE_DIR, out);
}

// remove files
static int ramfs_unlink(vnode_t *dir, const char *name) {

    if (dir->type != VNODE_DIR) return -1;
    ramfs_inode_t *ino = dir->data;

    ramfs_dirent_t *prev = 0;
    for (ramfs_dirent_t *d = ino->children; d; d = d->next) {

        if (strcmp(d->name, name) == 0) {

            if (prev) prev->next  = d->next;
            else      ino->children = d->next;
            ino->child_count--;

            ramfs_inode_t *ci = d->vnode->data;

            if (ci) {                                               // release file data
                if (ci->buf) kfree(ci->buf);
                kfree(ci);
            }

            kfree(d->vnode);
            kfree(d);
            return 0;

        }
        prev = d;
    }
    return -1;
}

// return directory entries
static int ramfs_readdir(vnode_t *dir, uint32_t index, char *name_out, vnode_t **node_out) {

    if (dir->type != VNODE_DIR) return -1;
    ramfs_inode_t *ino = dir->data;

    uint32_t i = 0;
    for (ramfs_dirent_t *d = ino->children; d; d = d->next, i++) {                  // iterate linked list

        if (i == index) {                                                           // stop when index matches
            if (name_out) strncpy(name_out, d->name, VFS_NAME_MAX - 1);
            if (node_out) *node_out = d->vnode;
            return 0;
        }

    }
    return -1;
}

vnode_t *ramfs_get_root(void) { return ramfs_root; }

// insert device node -> filesystem
int ramfs_register_dev(const char *path, vnode_t *dev_vnode) {

    if (!path || path[0] != '/') return -1;

    char parent[VFS_PATH_MAX];                                          // split into parent path + leaf name
    strncpy(parent, path, VFS_PATH_MAX - 1);
    parent[VFS_PATH_MAX - 1] = '\0';

    int slash = -1;
    for (int i = (int)strlen(parent) - 1; i >= 0; i--) {
        if (parent[i] == '/') { slash = i; break; }
    }
    if (slash < 0) return -1;

    const char *leaf = path + slash + 1;
    if (slash == 0) parent[1] = '\0';
    else            parent[slash] = '\0';

    vnode_t *dir = vfs_resolve(slash == 0 ? "/" : parent);              // resolve parent directory
    if (!dir || dir->type != VNODE_DIR) return -1;

    return ramfs_add_dirent(dir, leaf, dev_vnode);                      // insert entry
}

// initialise ramfs
void ramfs_init(void) {

    kprintf("RAMFS: Initialising\n");

    ramfs_root = ramfs_new_vnode(VNODE_DIR);                            // create root
    if (!ramfs_root) {
        kprintf("RAMFS: FATAL - could not allocate root vnode\n");
        return;
    }

    if (vfs_mount("/", ramfs_root) < 0) {                               // mount filesystem
        kprintf("RAMFS: FATAL - could not mount at /\n");
        return;
    }

    kprintf("RAMFS: Ready - root vnode @ %p\n", (uint32_t)ramfs_root);
}