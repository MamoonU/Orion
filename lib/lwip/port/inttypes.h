// inttypes.h - minimal shim for lwIP port

#ifndef LWIP_PORT_INTTYPES_H
#define LWIP_PORT_INTTYPES_H

#include <stdint.h>

// printf format macros for fixed-width types (x86 / i686-elf)
#define PRId8    "d"
#define PRIu8    "u"
#define PRIx8    "x"
#define PRIX8    "X"

#define PRId16   "d"
#define PRIu16   "u"
#define PRIx16   "x"
#define PRIX16   "X"

#define PRId32   "d"
#define PRIu32   "u"
#define PRIx32   "x"
#define PRIX32   "X"

#define PRId64   "lld"
#define PRIu64   "llu"
#define PRIx64   "llx"
#define PRIX64   "llX"

#define PRIuPTR  "u"
#define PRIxPTR  "x"

#endif