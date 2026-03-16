// devfs.h - Device Node Registration

#ifndef DEVFS_H
#define DEVFS_H

#include "vfs.h"

// create /dev and register all standard devices
void     devfs_init(void);

// standard stream vnodes
vnode_t *devfs_stdin_vnode (void);
vnode_t *devfs_stdout_vnode(void);
vnode_t *devfs_stderr_vnode(void);

// utility device vnodes
vnode_t *devfs_null_vnode  (void);     // /dev/null     - reads return EOF, writes discard
vnode_t *devfs_zero_vnode  (void);     // /dev/zero     - reads return 0x00 bytes
vnode_t *devfs_random_vnode(void);     // /dev/random   - reads return pseudorandom bytes

#endif