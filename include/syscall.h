// syscall.h - System Call Interface

#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include "irq.h"

// syscall numbers
#define SYS_YIELD       0       //                                  - yield CPU voluntarily
#define SYS_EXIT        1       // EBX = exit code                  - terminate calling process
#define SYS_GETPID      2       //                                  - return calling process PID
#define SYS_SLEEP       3       // EBX = ticks                      - sleep for N timer ticks
#define SYS_FORK        4       // EBX = child entry point          - spawn child process
#define SYS_EXEC        5       // EBX = pid, ECX = entry           - replace process entry point
#define SYS_WRITE       6       // EBX = fd, ECX = buf, EDX = len   - write bytes to fd
#define SYS_READ        7       // EBX = fd, ECX = buf, EDX = len   - read bytes from fd
#define SYS_OPEN        8       // EBX = path, ECX = flags          - open file (stub until VFS)
#define SYS_CLOSE       9       // EBX = fd                         - close file descriptor
#define SYS_PIPE        10      // EBX = &pipefd[2]                 - create anonymous pipe
#define SYS_DUP2        11      // EBX = oldfd, ECX = newfd         - duplicate file descriptor
#define SYS_EXECVE      12      // EBX = path                       - exec ELF binary in place
#define SYS_WAIT        13      // EBX = pid (-1 = any), ECX = &exit_code
#define SYS_CHDIR       14      // change directory
#define SYS_GETCWD      15      // return cwd_path
#define SYS_READDIR     16      // read directory
#define SYS_BIND        17      // EBX = src_path, ECX = new_path, EDX = flags
#define SYS_UNBIND      18      // EBX = new_path
#define SYS_NSDUMP      19      // (debug) dump calling process namespace
#define SYS_MOUNT       20      // EBX = srv_fd, ECX = path, EDX = ns_flags
#define SYS_SBRK        21      // EBX = increment (bytes) - extend process heap
#define SYS_SIGNAL      22      // EBX = signum, ECX = handler, EDX = trampoline
#define SYS_SIGRETURN   23      //                                  - restore pre-signal context
#define SYS_KILL        24      // EBX = pid, ECX = signum          - send signal to process
#define SYS_DIAL        25      // EBX = addr ("ip!port"), ECX = path, EDX = ns_flags - dial TCP PULSAR server + mount
 
#define SYSCALL_COUNT   26

// kernel-side entry point (registered in IDT as int 0x80)
void syscall_dispatch(regs_t *r);

// register syscall handler in idt
void syscall_init(void);

#endif