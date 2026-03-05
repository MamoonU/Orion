#ifndef PANIC_H
#define PANIC_H

__attribute__((noreturn)) void panic(const char *msg);

#define kassert(condition)                              \
    do {                                                \
        if (!(condition))                               \
            panic("Assertion failed: " #condition);     \
    } while (0)

#endif
