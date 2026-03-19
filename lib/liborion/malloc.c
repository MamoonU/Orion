// malloc.c - liborion heap allocator

#include "orion.h"

#define HDR_SIZE    8               // size of the block header
#define ALIGN       8

// block structure
typedef struct block {
    uint32_t        size;           // usable bytes (not including header)
    uint32_t        in_use;         // 1 = allocated, 0 = free
} block_t;

static block_t *heap_start = 0;     // first block in heap

// align size -> ALIGN
static uint32_t align_up(uint32_t n) {
    return (n + ALIGN - 1) & ~(uint32_t)(ALIGN - 1);
}

// extend heap by `needed` bytes: return pointer to new block
static block_t *heap_extend(uint32_t need) {
    uint32_t total = align_up(need + HDR_SIZE);
    block_t *b = (block_t *)sbrk((int)total);
    if (!b || b == (block_t *)(uintptr_t)-1) return 0;
    b->size   = total - HDR_SIZE;
    b->in_use = 0;
    return b;
}

// allocate new pages
void *malloc(uint32_t size) {

    if (size == 0) return 0;
    size = align_up(size);

    if (!heap_start) {                                                          // initialise heap on first call
        heap_start = heap_extend(size);
        if (!heap_start) return 0;
    }

    block_t *b = heap_start;
    while (1) {                                                                 // first-fit search
        if (!b->in_use && b->size >= size) {
            if (b->size >= size + HDR_SIZE + ALIGN) {                           // split if room for another header + 8 bytes
                block_t *next = (block_t *)((uint8_t *)b + HDR_SIZE + size);
                next->size    = b->size - size - HDR_SIZE;
                next->in_use  = 0;
                b->size       = size;
            }
            b->in_use = 1;
            return (void *)((uint8_t *)b + HDR_SIZE);
        }

        block_t *next = (block_t *)((uint8_t *)b + HDR_SIZE + b->size);         // move to next block: if past the current heap, extend

        if (b->size == 0 || next == b) {                                        // extending heap
            block_t *ext = heap_extend(size);
            if (!ext) return 0;

            if (!b->in_use) {                                                   // if last block free: absorb extension into it
                b->size += HDR_SIZE + ext->size;
            } else {
                b = ext;
            }
            continue;
        }
        b = next;
    }
}

// allocate new pages + zero
void *calloc(uint32_t nmemb, uint32_t size) {

    uint32_t total = nmemb * size;
    if (!total) return 0;

    void *p = malloc(total);
    if (p) memset(p, 0, total);

    return p;
}

// resize existing allocation
void *realloc(void *ptr, uint32_t new_size) {

    if (!ptr)      return malloc(new_size);         // realloc(NULL, n) = malloc(n)

    if (!new_size) {                                // realloc(p, 0)    = free(p)
        free(ptr); return 0;
    }

    block_t *b = (block_t *)((uint8_t *)ptr - HDR_SIZE);

    if (b->size >= new_size) return ptr;            // if already big enough; no-op

    void *new_ptr = malloc(new_size);
    if (!new_ptr) return 0;                         // OOM: original block untouched

    memcpy(new_ptr, ptr, b->size);                  // copy old payload
    free(ptr);
    return new_ptr;
}

// free pages
void free(void *ptr) {

    if (!ptr) return;

    block_t *b = (block_t *)((uint8_t *)ptr - HDR_SIZE);                        // convert: user pointer -> block header
    b->in_use = 0;                                                              // mark free

    block_t *next = (block_t *)((uint8_t *)b + HDR_SIZE + b->size);             // coalescing logic
    if (!next->in_use && next->size > 0)
        b->size += HDR_SIZE + next->size;
}