// proc.c - liborion process control

#include "orion.h"

// terminate immediately
void _exit(int code) {
    syscall(SYS_EXIT, (uint32_t)code, 0, 0);
    for (;;) asm volatile("hlt");
}

// return current PID
int getpid(void) {
    return (int)syscall(SYS_GETPID, 0, 0, 0);
}

// voluntarily give up CPU
void yield(void) {
    syscall(SYS_YIELD, 0, 0, 0);
}

// sleep for N PIT ticks
void sleep(uint32_t ticks) {
    syscall(SYS_SLEEP, ticks, 0, 0);
}

// wait for child process
int wait(int pid, int *exit_code) {
    return (int)syscall(SYS_WAIT, (uint32_t)(int32_t)pid, (uint32_t)exit_code, 0);
}

// spawn child process at entry point
int fork(uint32_t child_entry) {
    return (int)syscall(SYS_FORK, child_entry, 0, 0);
}

