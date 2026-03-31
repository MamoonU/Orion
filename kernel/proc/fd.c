// fd.c - File Descriptor Layer

#include "fd.h"
#include "vfs.h"
#include "devfs.h"
#include "kprintf.h"

// initialise descriptor table
void fd_table_init(file_t **table) {

    for (int i = 0; i < FD_MAX; i++)
        table[i] = 0;

    table[0] = vfs_open_vnode(devfs_stdin_vnode(),  O_RDONLY);              // fd 0 = stdin
    table[1] = vfs_open_vnode(devfs_stdout_vnode(), O_WRONLY);              // fd 1 = stdout
    table[2] = vfs_open_vnode(devfs_stderr_vnode(), O_WRONLY);              // fd 2 = stderr
}

// close descriptor
void fd_close(file_t **table, int fd) {

    if (fd < 0 || fd >= FD_MAX || !table[fd]) return;
    vfs_close(table[fd]);
    table[fd] = 0;
}

// close all descriptors
void fd_table_close_all(file_t **table) {

    for (int i = 0; i < FD_MAX; i++) {
        if (table[i]) {
            vfs_close(table[i]);
            table[i] = 0;
        }
    }
}

// used for fork()
void fd_table_clone(file_t **src, file_t **dst) {

    for (int i = 0; i < FD_MAX; i++) {
        dst[i] = src[i];
        if (dst[i]) dst[i]->refcount++;
    }
}

// install file_t at the lowest free slot
int fd_install(file_t **table, file_t *f) {

    for (int i = 0; i < FD_MAX; i++) {
        if (!table[i]) {
            table[i] = f;
            return i;
        }
    }
    return -1;
}

// file descriptor read
int fd_read(file_t **table, int fd, void *buf, uint32_t len) {

    if (fd < 0 || fd >= FD_MAX || !table[fd]) return -1;
    return vfs_read(table[fd], buf, len);
}

// file descriptor write
int fd_write(file_t **table, int fd, const void *buf, uint32_t len) {

    if (fd < 0 || fd >= FD_MAX || !table[fd]) return -1;
    return vfs_write(table[fd], (void *)buf, len);
}

// file descriptor seek
int32_t fd_seek(file_t **table, int fd, int32_t offset, int whence) {

    if (fd < 0 || fd >= FD_MAX || !table[fd]) return -1;
    return vfs_seek(table[fd], offset, whence);
}

