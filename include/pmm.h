// Physical Memory Manager Header

#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>
#include "multiboot.h"

#define PAGE_SIZE 4096                  // page_size = 4096 bytes
#define PAGE_SHIFT 12                   // allows fast address conversion
// page_size = 1 << page_shift ----- 1 << 12 = 4096
// page = addr >> PAGE_SHIFT;
// addr = page << PAGE_SHIFT;

#define ADDR_TO_FRAME(addr) ((addr) >> PAGE_SHIFT)
#define FRAME_TO_ADDR(frame) ((frame) << PAGE_SHIFT)


#define PMM_MAX_FRAMES  (0x100000000ULL >> PAGE_SHIFT)      // 4GB memory = 1,048,576 frames
#define PMM_BITMAP_SIZE (PMM_MAX_FRAMES / 32)               // 32 frames per uint32
// 4GB total = 1,048,576 frames = 32768 uint32s = 128KB

// parse GRUB's memory map and set up bitmap
void pmm_init(multiboot_info_t *mbi, uint32_t kernel_phys_start, uint32_t kernel_phys_end);     // multiboot_info & physical addresses of loaded kernel start/end

// allocate one available physical page frame
uint32_t pmm_alloc_frame(void);

// release one reserved physical page frame
void pmm_free_frame(uint32_t phys_addr);

void print_uint32_hex(uint32_t v);
void print_uint32_dec(uint32_t v);


// tests
uint32_t pmm_get_total_frames(void);
uint32_t pmm_get_used_frames(void);
uint32_t pmm_get_free_frames(void);

#endif
