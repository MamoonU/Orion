// start.c - liborion ELF entry point

#include "orion.h"
#include <stdint.h>

// forward declaration: defined in the user program
int main(int argc, char **argv);

__attribute__((naked)) void _start(void) {      // naked = no C prologue/epilogue
    asm volatile (
        "xor    %ebp, %ebp      \n"             // clear ebp: mark outermost frame
        "mov    (%esp), %eax    \n"             // eax = argc
        "lea    4(%esp), %ecx   \n"             // ecx = &argv[0]

        "push   %ecx            \n"             // push argv
        "push   %eax            \n"             // push argc
        "call   main            \n"             // call main(argc, argv)

        "push   %eax            \n"             // push main()'s return value
        "call   _exit           \n"             // _exit(ret): terminate process
        "hlt                    \n"             // halt: stop CPU if _exit fail
    );
}
