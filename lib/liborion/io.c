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
    return (int)syscall4(SYS_READDIR, (uint32_t)fd, index, (uint32_t)name_buf, buflen);
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

// dial a remote PULSAR server over TCP and mount it at path
int dial(const char *addr, const char *path, uint8_t flags) {
    return (int)syscall(SYS_DIAL, (uint32_t)addr, (uint32_t)path, (uint32_t)flags);
}

// human-readable errno string
char *strerror(int errnum) {
    switch (errnum) {
        case EPERM:   return "Operation not permitted";
        case ENOENT:  return "No such file or directory";
        case ESRCH:   return "No such process";
        case EINTR:   return "Interrupted system call";
        case EIO:     return "I/O error";
        case EBADF:   return "Bad file descriptor";
        case ECHILD:  return "No child processes";
        case ENOMEM:  return "Out of memory";
        case EACCES:  return "Permission denied";
        case EFAULT:  return "Bad address";
        case EBUSY:   return "Device or resource busy";
        case EEXIST:  return "File exists";
        case ENOTDIR: return "Not a directory";
        case EISDIR:  return "Is a directory";
        case EINVAL:  return "Invalid argument";
        case EMFILE:  return "Too many open files";
        case ENOSPC:  return "No space left on device";
        case EPIPE:   return "Broken pipe";
        case ENOSYS:  return "Function not implemented";
        default:      return "Unknown error";
    }
}