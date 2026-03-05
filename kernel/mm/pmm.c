// Bitmap Physical Memory Manager

#include "pmm.h"
#include "multiboot.h"

static uint32_t bitmap[PMM_BITMAP_SIZE];            // bitmap storage = 128KB (lives in boot.asm/.BSS)


// [frame / 32] = select word
// (frame % 32) = select bit
static inline void bitmap_set(uint32_t frame)   {     bitmap[frame / 32] |= (1u << (frame % 32));       }       // mark frame in bitmap reserved
static inline void bitmap_clear(uint32_t frame) {   bitmap[frame / 32] &= ~(1u << (frame % 32));        }       // mark frame in bitmap available
static inline int bitmap_test(uint32_t frame)   {     return (bitmap[frame / 32] >> (frame % 32)) & 1;  }       // return frame state

static uint32_t pmm_total_frames = 0;   // memory detected from mmap
static uint32_t pmm_used_frames = 0;    // reserved frames
static uint32_t pmm_alloc_search = 1;         // Start searching here (skip frame 0)

#include "kprintf.h"
#include "serial.h"



// mark physical address range reserved
static void pmm_mark_region_reserved(uint64_t base, uint64_t len) {

    uint64_t aligned_base = (base + PAGE_SIZE - 1) &~(uint64_t)(PAGE_SIZE - 1);         // if base = mid page -> move to next page (base = ceiling)
    if (aligned_base > base + len) return;                                              

    uint64_t aligned_len  = len - (aligned_base - base);                                // mark only full frames (length = floor)
    aligned_len &= ~(uint64_t)(PAGE_SIZE - 1);

    // byte region -> frame range
    uint32_t frame = (uint32_t)(aligned_base >> PAGE_SHIFT);
    uint32_t count = (uint32_t)(aligned_len  >> PAGE_SHIFT);

    for (uint32_t i = 0; i < count; i++) {                          // mark frames reserved
        if (!bitmap_test(frame + i)) {
            bitmap_set(frame + i);
            pmm_used_frames++;                  // increment pmm_used_frames (prevent double counting)
        }
    }

}

// mark physical address range available
static void pmm_mark_region_available(uint64_t base, uint64_t len) {

    uint64_t aligned_base = (base + PAGE_SIZE - 1) &~(uint64_t)(PAGE_SIZE - 1);             // same logic as reversed func
    if (aligned_base >= base + len) return;

    uint64_t aligned_len  = (len - (aligned_base - base)) &~(uint64_t)(PAGE_SIZE - 1);      // similar logic as reversed func (remove partial frames)

    // byte region -> frame range
    uint32_t frame = (uint32_t)(aligned_base >> PAGE_SHIFT);
    uint32_t count = (uint32_t)(aligned_len  >> PAGE_SHIFT);

    for (uint32_t i = 0; i < count; i++) {              // mark frames available (frame 0 still protected)
        if (frame + i == 0) continue;                   // Never free the zero frame
        if (bitmap_test(frame + i)) {
            bitmap_clear(frame + i);
            pmm_used_frames--;                          // unmark reserved
            pmm_total_frames++;                         // mark available
        }
    }

}

// initialise physical memory manager
void pmm_init(multiboot_info_t *mbi, uint32_t kernel_phys_start, uint32_t kernel_phys_end) {

    kprintf("\nPMM: Initialising physical memory manager\n");

    for (uint32_t i = 0; i < PMM_BITMAP_SIZE; i++)                              // mark all frames reserved (deny-by-default memory policy)
        bitmap[i] = 0xFFFFFFFF;

    pmm_used_frames = PMM_MAX_FRAMES;                                           // used_frames as max_frames, (max_frames--) as frames freed
    pmm_total_frames = 0;

    if (!(mbi->flags & MULTIBOOT_FLAG_MMAP)) {      // if no GRUB mmap

        kprintf("PMM: FATAL ERROR — GRUB did not provide a memory map\n");

        if (mbi->flags & MULTIBOOT_FLAG_MEM) {                                  // fallback assuming memory up to mem_upper is free
            kprintf("PMM: Falling back to mem_upper field\n");

            uint64_t mem_bytes = (uint64_t)(mbi->mem_upper) * 1024;
            pmm_mark_region_available(0x100000, mem_bytes);                          // Free above 1MB
        }

    } else {                                        // parse GRUB mmap

        kprintf("PMM: Parsing GRUB memory map:\n");

        multiboot_mmap_entry_t *entry = (multiboot_mmap_entry_t *)(uint32_t)mbi->mmap_addr;
        uint32_t mmap_end = mbi->mmap_addr + mbi->mmap_length;

        kprintf("  [ HIGH BASE: LOW BASE  +   LENGTH  ]\n");

        while ((uint32_t)entry < mmap_end) {

            // print each entry for debugging
            //=================== example output ===================
            // mmap entry = base:0x0 len:0x9FC00 type:1
            // size = 639KB | RAM = 0-639KB | type = available
            // mmap entry = base:0x9FC00 len:0x400 type:2
            // size = 1KB | RAM = 639KB-640KB | type = unavailable
            //======================================================
            kprintf("  [%08x:%08x + %08x] type=%u",
                    (uint32_t)(entry->addr >> 32),
                    (uint32_t)entry->addr,
                    (uint32_t)entry->len,
                    entry->type);

            if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {    // if memory available

                kprintf(" (available)\n");

                if (entry->addr < 0x100000000ULL) {
                    uint64_t top = entry->addr + entry->len;
                    if (top > 0x100000000ULL)                                                           // clamp memory to 4GB explicitly
                        top = 0x100000000ULL;
                    pmm_mark_region_available((uint32_t)entry->addr, (uint32_t)(top - entry->addr)); 
                }

            } else {                                            // else memory reserved
                kprintf(" (reserved)\n");
            }

            entry = (multiboot_mmap_entry_t *)((uint32_t)entry + entry->size + sizeof(uint32_t));
        }
    }

    kprintf("PMM: Reserving kernel image %p - %p\n",           // reserve kernel image
            kernel_phys_start, kernel_phys_end);
    pmm_mark_region_reserved(kernel_phys_start, kernel_phys_end - kernel_phys_start);

    if (!bitmap_test(0)) {                                      // reserve frame 0 explicitly
        bitmap_set(0);
        pmm_used_frames++;
    }

    uint32_t free_mb = (pmm_total_frames * PAGE_SIZE) / (1024 * 1024);          // print total frames (usable) and free MB
    kprintf("PMM: Ready. Total usable frames: %u (~%u MB free)\n\n",
            pmm_total_frames, free_mb);

}

// mark available frame reserved
uint32_t pmm_alloc_frame(void) {

    if (pmm_total_frames == 0) return 0;                                    // OOM check

    uint32_t max_frames = PMM_MAX_FRAMES;

        for (uint32_t pass = 0; pass < 2; pass++) {                         // 2 pass search

            uint32_t start = (pass == 0) ? pmm_alloc_search : 1;
            uint32_t end = (pass == 0) ? max_frames : pmm_alloc_search;
            uint32_t word_start = start / 32;
            uint32_t word_end = (end + 31) / 32;

            for (uint32_t w = word_start; w < word_end; w++) { 

                if (bitmap[w] == 0xFFFFFFFF) continue;                  // skip fully used words

                for (uint32_t bit = 0; bit < 32; bit++) {

                    uint32_t frame = w * 32 + bit;
                    if (frame == 0) continue;
                    if (frame < start || frame >= end) continue;

                    if (!bitmap_test(frame)) {                          // find free frame 

                        bitmap_set(frame);                              // alloc reserved
                        pmm_used_frames++;
                        pmm_alloc_search = frame + 1;                   // update search pointer

                        if (pmm_alloc_search >= max_frames)             // search pointer safety
                            pmm_alloc_search = 1;

                        return FRAME_TO_ADDR(frame);                    // return frame address
                }
            }
        }
    }
kprintf("PMM: Out of memory \n");
return 0;
}

// mark reserved frame available
void pmm_free_frame(uint32_t phys_addr) {

    uint32_t frame = ADDR_TO_FRAME(phys_addr);                                          // address -> frame
    if (frame == 0) return;                                                             // protect frame 0

    if (!bitmap_test(frame)) {                                          // detect double free errors
        kprintf("PMM: WARNING — double-free of frame %p\n", phys_addr);
        return;
    }

    bitmap_clear(frame);                                // free frame
    pmm_used_frames--;

    if (frame < pmm_alloc_search)                       // reuse freed frames
        pmm_alloc_search = frame;

}


// return # of total, used, free frames
uint32_t pmm_get_total_frames(void) { return pmm_total_frames; }
uint32_t pmm_get_used_frames(void)  { return pmm_used_frames;  }
uint32_t pmm_get_free_frames(void)  { return pmm_total_frames - pmm_used_frames; }


