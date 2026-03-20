// signal.c - liborion signal handling

#include "orion.h"

int errno = 0;                                                      // errno: single global state

static sig_handler_t handlers[NSIG];                                // local table of sig handler

// register signal handler with kernel
sig_handler_t signal(int signum, sig_handler_t handler) {

    if (signum <= 0 || signum >= NSIG) {                            // validate signum
        errno = EINVAL;
        return SIG_ERR;
    }

    if (signum == SIGKILL || signum == SIGSTOP) {                   // SIGKILL and SIGSTOP cannot be caught
        errno = EINVAL;
        return SIG_ERR;
    }

    sig_handler_t old = handlers[signum];                           // save old handler
    handlers[signum]  = handler;

    // inform kernel:              |   EBX = signum  | ECX = handler_va |      EDX = trampoline_va       |
    int32_t rc = syscall(SYS_SIGNAL, (uint32_t)signum, (uint32_t)handler, (uint32_t)sigreturn_trampoline);

    if (rc < 0) {                                                   // load local state if kernel fails
        handlers[signum] = old;
        errno = EINVAL;
        return SIG_ERR;
    }

    return old;
}

// send signal to process by PID
int kill(int pid, int signum) {

    if (signum < 0 || signum >= NSIG) {                                             // validate signum
        errno = EINVAL; return -1;
    }

    int32_t rc = syscall(SYS_KILL, (uint32_t)(int32_t)pid, (uint32_t)signum, 0);    // signum[9]

    if (rc < 0) {
        errno = ESRCH; return -1;                                                   // no process found
    }

    return 0;
}

// send signal to self
int raise(int signum) {
    return kill(getpid(), signum);
}