// io.c - liborion file I/O and namespace

#include "orion.h"

// open
int open(const char *path, int flags) {
    return (int)syscall(SYS_OPEN, (uint32_t)path, (uint32_t)flags, 0);
}

// close
int close(int fd) {
    return (int)syscall(SYS_CLOSE, (uint32_t)fd, 0, 0);
}

// read
int read(int fd, void *buf, uint32_t len) {
    return (int)syscall(SYS_READ, (uint32_t)fd, (uint32_t)buf, len);
}

// write
int write(int fd, const void *buf, uint32_t len) {
    return (int)syscall(SYS_WRITE, (uint32_t)fd, (uint32_t)buf, len);
}

// create pipe ([0]=read end, [1]=write end)
int pipe(int pipefd[2]) {
    return (int)syscall(SYS_PIPE, (uint32_t)pipefd, 0, 0);
}

// duplicate fd
int dup2(int oldfd, int newfd) {
    return (int)syscall(SYS_DUP2, (uint32_t)oldfd, (uint32_t)newfd, 0);
}

// change working directory
int chdir(const char *path) {
    return (int)syscall(SYS_CHDIR, (uint32_t)path, 0, 0);
}

// return working directory
int getcwd(char *buf, uint32_t len) {
    return (int)syscall(SYS_GETCWD, (uint32_t)buf, len, 0);
}

// read directories by index
int readdir(int fd, uint32_t index, char *name_buf, uint32_t buflen) {
    return (int)syscall(SYS_READDIR, (uint32_t)fd, index, (uint32_t)name_buf);
    (void)buflen;
}

// mount path -> another
int bind(const char *src, const char *dst, uint8_t flags) {
    return (int)syscall(SYS_BIND, (uint32_t)src, (uint32_t)dst, (uint32_t)flags);
}

// remove binding
int unbind(const char *dst) {
    return (int)syscall(SYS_UNBIND, (uint32_t)dst, 0, 0);
}

// debug: print namespace
void nsdump(void) {
    syscall(SYS_NSDUMP, 0, 0, 0);
}

// mount PULSAR server
int mount(int srv_fd, const char *path, uint8_t flags) {
    return (int)syscall(SYS_MOUNT, (uint32_t)srv_fd, (uint32_t)path, (uint32_t)flags);
}

// extend process heap
void *sbrk(int increment) {
    return (void *)(uintptr_t)syscall(SYS_SBRK, (uint32_t)increment, 0, 0);
}