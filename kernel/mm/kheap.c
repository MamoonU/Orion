// Kernel Heap

// algorithm: first-fit search | immediate coalescing on free

// (boundary-tag) (implicit) (free list)

// block Anatomy:
// header (4 bytes) = size | alloc_bit
// payload (variable size)
// footer (4 bytes) = size | alloc_bit

// heap memory map (virtual):
//   HEAP_START + 0 : alignment padding  (4 B, value = 0)
//   HEAP_START + 4 : prologue header    (8 | ALLOC)
//   HEAP_START + 8 : prologue footer    (8 | ALLOC)  ← heap_listp (bp)
//   HEAP_START + 12: initial epilogue   (0 | ALLOC)  ← heap_brk initially
//   ...free / alloc blocks grow here...
//   heap_brk       : current epilogue   (0 | ALLOC)

// lazy page mapping

#include "kheap.h"
#include "vmm.h"
#include "pmm.h"

#include "kprintf.h"
#include "panic.h"

#define WSIZE 4u                            // word = header/footer size (bytes)
#define DWSIZE 8u                           // double word - alignment unit
#define MIN_BLOCK (DWSIZE + DWSIZE)         // 16 B: 4B header + 8B payload + 4B footer

#define ALIGN(s) (((size_t)(s) + (DWSIZE - 1u)) & ~(size_t)(DWSIZE - 1u))   // round 's' up to the next multiple of DSIZE (8-byte alignment)

// MACROS

// pack (block size & allocated bit) -> one word
#define PACK(size, alloc) ((uint32_t)(size) | (uint32_t)(alloc))

// read/write word at address p
#define GET(p)      (*(volatile uint32_t *)(p))
#define PUT(p, val) (*(volatile uint32_t *)(p) = (uint32_t)(val))

// extract size and allocated bit from a header/footer (mask low 3 bits)
#define GET_SIZE(p)     (GET(p) & ~0x7u)
#define GET_ALLOC(p)    (GET(p) &  0x1u)

// BLOCK POINTER MACROS

// bp = payload pointer

// pointer to the block header word
#define HDRP(bp)    ((char *)(bp) - WSIZE)

// pointer to the block footer word
#define FTRP(bp)    ((char *)(bp) + GET_SIZE(HDRP(bp)) - DWSIZE)            // (bp + total_block_size - 8 = word before next block header = footer)

// bp of the next block in the heap
#define NEXT_BLKP(bp)   ((char *)(bp) + GET_SIZE(HDRP(bp)))

// bp of the previous block
#define PREV_BLKP(bp)   ((char *)(bp) - GET_SIZE((char *)(bp) - DWSIZE))    // uses the previous block's footer ( O(1) backwards traversal )

// HEAP

static char     *heap_listp = 0;            // bp of prologue - NEXT_BLKP(heap_listp) = first real block (skipping prologue)
static uint32_t heap_brk   = 0;             // byte address of epilogue header
static uint32_t heap_virt_mapped = 0;       // highest virtual byte mapped (prevent writing in unmapped memory)

// ensure virtual addresses [HEAP_START, end) are backed by physical pages
// maps new pages from PMM on demand.  return 0 on success, -1 on OOM
static int ensure_mapped(uint32_t end) {

    while (heap_virt_mapped < end) {

        if (heap_virt_mapped + PAGE_SIZE > HEAP_MAX) {                                  // kernel heap hit ceiling
            kprintf("KHEAP: FATAL — virtual heap ceiling reached\n");
            return -1;
        }

        uint32_t phys = pmm_alloc_frame();

        if (!phys) {
            kprintf("KHEAP: FATAL — PMM out of physical frames\n");                // out of physical memory
            return -1;
        }

        vmm_map_page(heap_virt_mapped, phys, VMM_KERNEL_RW);
        heap_virt_mapped += PAGE_SIZE;
    }

    return 0;
}

// Merge bp with any immediately adjacent free blocks
// Uses boundary tags so each direction is O(1)
// Returns the bp of the (possibly enlarged) free block
static char *coalesce(char *bp) {

    char *prev_bp   = PREV_BLKP(bp);                    // identify neighbours
    char *next_bp   = NEXT_BLKP(bp);

    int   prev_free = !GET_ALLOC(FTRP(prev_bp));        // identify allocated (from footer)
    int   next_free = !GET_ALLOC(HDRP(next_bp));        // identify allocated (from header)

    size_t size     = GET_SIZE (HDRP(bp));

    if (!prev_free && !next_free) {

        return bp;                                                  // case 1: both neighbours allocated (nothing to merge)

    } else if (!prev_free && next_free) {
        
        size += GET_SIZE(HDRP(next_bp));                            // case 2: only next block is free (absorb it)
        PUT(HDRP(bp),   PACK(size, 0));
        PUT(FTRP(bp),   PACK(size, 0));                 // FTRP recalculates with new size

    } else if (prev_free && !next_free) {
        
        size += GET_SIZE(HDRP(prev_bp));                // case 3: only prev block is free (merge into it)
        PUT(FTRP(bp),      PACK(size, 0));              // write footer at current bp's end
        PUT(HDRP(prev_bp), PACK(size, 0));              // update prev blocks header
        bp = prev_bp;                                   // return starts at merged block

    } else {
        
        size += GET_SIZE(HDRP(prev_bp)) + GET_SIZE(HDRP(next_bp));  // case 4: both neighbours are free (three-way merge)
        PUT(HDRP(prev_bp), PACK(size, 0));
        PUT(FTRP(next_bp), PACK(size, 0));
        bp = prev_bp;
    }
    return bp;
}

// add new free block of 'size' bytes
// convert epilogue -> block header
// place new epilogue
// return bp of the new free block or NULL on failure
static char *extend_heap(size_t size) {

    if (ensure_mapped(heap_brk + (uint32_t)size + WSIZE) < 0)       // ensure physical mapping
        return 0;

    // epilogue header = new blocks header
    char *bp = (char *)heap_brk + WSIZE;                            // payload starts one word in

    PUT((char *)heap_brk,    PACK(size, 0));                        // new free block header
    PUT(FTRP(bp),            PACK(size, 0));                        // new free block footer
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1));                           // new epilogue (size=0, alloc=1)

    heap_brk += (uint32_t)size;                                     // advance epilogue pointer
    
    return coalesce(bp);                                            // coalesce if (block before old epilogue was free)
}

// place allocation of 'asize' bytes -> free block at bp
// splits block if (remainder fits minimum-sized free block)
static void place(char *bp, size_t asize) {

    size_t csize = GET_SIZE(HDRP(bp));                              // read block size

    if (csize - asize >= MIN_BLOCK) {                               // if remainder is big enough (16 bytes)
        
        PUT(HDRP(bp), PACK(asize, 1));                              // split: allocate front, leave rear as free block
        PUT(FTRP(bp), PACK(asize, 1));

        char *remainder = NEXT_BLKP(bp);
        PUT(HDRP(remainder), PACK(csize - asize, 0));
        PUT(FTRP(remainder), PACK(csize - asize, 0));

    } else {
        
        PUT(HDRP(bp), PACK(csize, 1));                              // no split: use the whole block
        PUT(FTRP(bp), PACK(csize, 1));
    }
}

// initialise k_heap
void kheap_init(void) {

    kprintf("KHEAP: Initialising kernel heap\n");

    uint32_t phys = pmm_alloc_frame();                                          // physical frame from PMM
    if (!phys)
        panic("KHEAP: init — cannot allocate first heap page from PMM");

    vmm_map_page(HEAP_START, phys, VMM_KERNEL_RW);                              // map first page
    heap_virt_mapped = HEAP_START + PAGE_SIZE;

    char *base = (char *)HEAP_START;

    PUT(base + 0 * WSIZE, 0);                   // padding
    PUT(base + 1 * WSIZE, PACK(DWSIZE, 1));     // prologue header
    PUT(base + 2 * WSIZE, PACK(DWSIZE, 1));     // prologue footer
    PUT(base + 3 * WSIZE, PACK(0,     1));      // initial epilogue

    heap_listp = base + 2 * WSIZE;              // bp of prologue payload
    heap_brk   = HEAP_START + 3 * WSIZE;        // address of epilogue header (heap_brk always points to current epilogue header)

    size_t init = ALIGN(HEAP_INITIAL);          // ensure alignment correct
    if (init < MIN_BLOCK) init = MIN_BLOCK;

    char *bp = extend_heap(init);               // add first free block between prologue & epilogue
    if (!bp)
        panic("KHEAP: init — extend_heap failed (PMM OOM?)");

    // output kheap info
    kprintf("KHEAP: Base       @ %p\n", (uint32_t)HEAP_START);
    kprintf("KHEAP: First free @ %p\n", (uint32_t)bp);
    kprintf("KHEAP: Initial    = %u bytes free\n", (uint32_t)init);
    kprintf("KHEAP: Ceiling    @ %p\n", (uint32_t)HEAP_MAX);
    kprintf("KHEAP: Ready\n\n");

}

// kernel allocate algorithm
void *kmalloc(size_t size) {

    if (!size) return 0;                                                // defensive reject 0 size

    size_t asize = ALIGN(size) + DWSIZE;                                // block size =  ALIGN(size) + DSIZE = 8-byte-aligned payload + header(4) + footer(4)
    if (asize < MIN_BLOCK) asize = MIN_BLOCK;

    // walk heap sequentially first-fit search through the implicit free list
    for (char *bp = NEXT_BLKP(heap_listp); GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp)) {

        if (!GET_ALLOC(HDRP(bp)) && GET_SIZE(HDRP(bp)) >= asize) {
            place(bp, asize);
            return bp;
        }

    }

    size_t extsize = (asize > PAGE_SIZE) ? asize : PAGE_SIZE;           // if (asize > pagesize) -> extsize = asize, else extsize = PAGE_SIZE
    extsize = ALIGN(extsize);

    char *bp = extend_heap(extsize);                                    // extend heap by extsize

    if (!bp) {
        kprintf("KHEAP: kmalloc — out of memory\n");
        return 0;
    }

    place(bp, asize);
    return bp;

}

// kernel page-aligned allocation algorithm
void *kmalloc_aligned(size_t size) {

    // allocate enough to guarantee finding a page-aligned address inside
    size_t total = size + PAGE_SIZE + sizeof(uint32_t);                         // at least sizeof(uint32_t) bytes before it for back-pointer.
    char *raw = (char *)kmalloc(total);
    if (!raw) return 0;

    // align up, leave room for stored back-pointer
    uint32_t aligned = ((uint32_t)raw + (uint32_t)sizeof(uint32_t) + PAGE_SIZE - 1u) & ~(uint32_t)(PAGE_SIZE - 1u);

    // store original pointer one word before aligned address
    *(uint32_t *)(aligned - sizeof(uint32_t)) = (uint32_t)raw;

    return (void *)aligned;                                                     // return aligned pointer

}

void kfree(void *ptr) {

    if (!ptr) return;

    char *bp = (char *)ptr;
    size_t size = GET_SIZE(HDRP(bp));

    PUT(HDRP(bp), PACK(size, 0));                                               // mark free
    PUT(FTRP(bp), PACK(size, 0));

    coalesce(bp);                                                               // coalesce

}

void kfree_aligned(void *ptr) {

    if (!ptr) return;

    uint32_t original = *(uint32_t *)((char *)ptr - sizeof(uint32_t));          // original pointer
    kfree((void *)original);

}

void kheap_dump(void) {

    kprintf("KHEAP: ────────────── dump ──────────────\n");
    kprintf("KHEAP:  heap_brk @ %p\n", heap_brk);                              // print epilogue header

    uint32_t idx  = 0;                                                          // block number
    size_t   used = 0;                                                          // allocated blocks (bytes)
    size_t   free = 0;                                                          // free blocks (bytes)

    for (char *bp = NEXT_BLKP(heap_listp); GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp), idx++) {     // walk heap until epilogue

        uint32_t sz    = GET_SIZE(HDRP(bp));                                    // block size
        uint32_t alloc = GET_ALLOC(HDRP(bp));                                   // allocation bit

        kprintf("KHEAP:  [%u] @ %p  size=%u  %s\n",
                idx,                                                            // block number
                (uint32_t)bp,                                                   // payload address
                sz,                                                             // size
                alloc ? "ALLOCATED" : "FREE");                                  // allocated?

        if (alloc) used += sz; else free += sz;                                 // accumulate full block sizes
    }

    kprintf("KHEAP:  used=%u  free=%u  total=%u\n",
            (uint32_t)used, (uint32_t)free, (uint32_t)(used + free));
    kprintf("KHEAP: ────────────────────────────────────\n\n");

}