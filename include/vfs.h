// vfs.h - Virtual File System Core

#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

// open flags
#define O_RDONLY    0x00        // read only
#define O_WRONLY    0x01        // write only
#define O_RDWR      0x02        // read + write
#define O_CREAT     0x04        // create if missing
#define O_TRUNC     0x08        // truncate on open
#define O_APPEND    0x10        // always write at end

// seek origins
#define SEEK_SET    0           // seek from beginning of file
#define SEEK_CUR    1           // seek from current position
#define SEEK_END    2           // seek from end of file (requires valid vnode->size)

// vnode types
#define VNODE_FILE  1           // regular file
#define VNODE_DIR   2           // directory
#define VNODE_DEV   3           // character device
#define VNODE_PIPE  4           // anonymous pipe

// constants
#define VFS_MOUNT_MAX   8       // max number mounter filesystems
#define VFS_PATH_MAX    128     // max path length
#define VFS_NAME_MAX    64      // max file name length

// forward type declarations
typedef struct vnode   vnode_t;
typedef struct file    file_t;
typedef struct vfs_ops vfs_ops_t;

// operations vtable
struct vfs_ops {

    int (*open)   (vnode_t *v, int flags);                                                  // pointer to filesystems open func
    int (*close)  (vnode_t *v);                                                             // pointer to filesystems close handler

    int (*read)   (vnode_t *v, void *buf, uint32_t len, uint32_t offset);                   // pointer for read data
    int (*write)  (vnode_t *v, const void *buf, uint32_t len, uint32_t offset);             // pointer for write data

    // directory operations
    int (*lookup) (vnode_t *dir, const char *name, vnode_t **out);                          // find child entry in directory
    int (*create) (vnode_t *dir, const char *name, uint8_t type, vnode_t **out);            // create new file
    int (*mkdir)  (vnode_t *dir, const char *name, vnode_t **out);                          // create new directory
    int (*unlink) (vnode_t *dir, const char *name);                                         // delete file

    int (*readdir)(vnode_t *dir, uint32_t index, char *name_out, vnode_t **node_out);       // return directory entries
};

// file-like object
struct vnode {
    uint8_t     type;           // file ? directory ? device
    uint32_t    refcount;       // number of references existing
    uint32_t    size;           // file size (bytes)
    vfs_ops_t  *ops;            // pointer -> filesystem operations
    void       *data;           // filesystem specific meta-data
};

// one open file instance
struct file {
    vnode_t    *vnode;          // backing vnode
    uint32_t    offset;         // current read/write position
    uint32_t    flags;          // open flags
    uint32_t    refcount;       // reference to this file object
    char        path[VFS_PATH_MAX];
};



// initialise VFS subsystem
void     vfs_init(void);

// vnode lifecycle
vnode_t *vnode_alloc(uint8_t type, vfs_ops_t *ops, void *data);     // create new vnode
void vnode_ref  (vnode_t *v);                                       // increment vnode reference count
void vnode_unref(vnode_t *v);                                       // decrement vnode reference count

// mount filesystem at path
int vfs_mount(const char *path, vnode_t *root);

// resolve a path string to vnode
vnode_t *vfs_resolve(const char *path);

// open file by path
file_t *vfs_open(const char *path, int flags);

// open pre-resolved vnode
file_t *vfs_open_vnode(vnode_t *v, int flags);

file_t *vfs_open_vnode_at(vnode_t *v, int flags, const char *path);

// close an open file
void vfs_close(file_t *f);

// read/write from/to open file
int vfs_read (file_t *f,       void *buf, uint32_t len);
int vfs_write(file_t *f, const void *buf, uint32_t len);

// reposition the read/write offset of an open file
int32_t vfs_seek(file_t *f, int32_t offset, int whence);

// create a directory at path
int vfs_mkdir(const char *path);

void vfs_path_resolve(const char *cwd, const char *path, char *out);

int vfs_readdir(file_t *f, uint32_t index, char *name_out, uint32_t name_max, vnode_t **node_out);

#endif