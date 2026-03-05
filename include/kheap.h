// Kernel Heap Header

// heap virtual address space = HEAP_START -> HEAP_END
// heap block = [ header : 4B ][ payload ... ][ footer : 4B ]
// header/footer encodes: bits[31:1] = total block size | bit[0] = allocated? flag

#ifndef KHEAP_H
#define KHEAP_H

#include <stdint.h>
#include <stddef.h>

// virtual address layout
#define HEAP_START 0x01000000u          // 16MB - above the 4 MB identity map
#define HEAP_MAX 0x05000000u            // 64MB ceiling  (48 MB of usable heap space)
#define HEAP_INITIAL (64 * 1024)         // 64KB initial free block (16 pages)

// initialise kernel heap
void kheap_init(void);

// allocate bytes of kernel heap memory
void *kmalloc(size_t size);

// allocate bytes aligned to page boundry
void *kmalloc_aligned(size_t size);

// free pointer returned by kmalloc
void kfree(void *ptr);

// free pointer returned by kmalloc_aligned
void kfree_aligned(void *ptr);

size_t kheap_used(void);                // bytes in allocated blocks (incl. headers)
size_t kheap_free_space(void);          // bytes in free blocks (incl. headers)
size_t kheap_size(void);                // total mapped heap bytes

// print dump to serial
void kheap_dump(void);


#endif