// arch/cc.h - compiler/architecture definitions for Orion OS lwIP port

#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>
#include "kprintf.h"

// byte order
#define BYTE_ORDER  LITTLE_ENDIAN   // x86

// iwIP's integer types
typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;
typedef uintptr_t mem_ptr_t;    // pointer sized integers

// printf format specifiers
#define U16_F   "u"
#define S16_F   "d"
#define X16_F   "x"
#define U32_F   "u"
#define S32_F   "d"
#define X32_F   "x"
#define SZT_F   "u"

// packed struct support (used by network protocol structs)
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT      __attribute__((packed))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x)    x

// unused argument suppression
#define LWIP_UNUSED_ARG(x)      (void)(x)

// debugging output (used when LWIP_DEBUG=1)
#define LWIP_PLATFORM_DIAG(x)   do { kprintf x; } while(0)

// fatal assertion
#define LWIP_PLATFORM_ASSERT(x) do {                                    \
    kprintf("LWIP ASSERT %s:%d: %s\n", __FILE__, __LINE__, x);          \
    for (;;) asm volatile("cli; hlt");                                  \
} while(0)

#endif