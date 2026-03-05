// GRUB(mmap) -> Kernel(mmap) Header

#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

#define MULTIBOOT_MAGIC 0x2BADB002

// set flag bits
// flags = which fields are valid
//bit 0 -> mem_lower / mem_upper usable
//bit 6 -> memory map present
#define MULTIBOOT_FLAG_MEM (1 << 0)         // 00000001 = 1  = 
#define MULTIBOOT_FLAG_MMAP (1 << 6)        // 01000000 = 64 = 

#define MULTIBOOT_MEMORY_AVAILABLE 1        // usable RAM
#define MULTIBOOT_MEMORY_RESERVED 2

// multiboot_mmap_entry_t - single memory map entry
typedef struct {

    uint32_t size;              // sizeof(entry) - sizeof(size) | != struct size | implemented this way for future entries
    uint64_t addr;              // physical address in RAM space
    uint64_t len;               // length of mmap entry
    uint32_t type;              // available or reserved

}__attribute__((packed)) multiboot_mmap_entry_t;

// multiboot_info_t - main info structure passed from GRUB in EBX
typedef struct {

    uint32_t flags;             // what is valid below

    uint32_t mem_lower;     // low memory in KB below 1MB (fallback RAM estimate)
    uint32_t mem_upper;     // high memory in KB above 1MB (fallback RAM estimate)

    uint32_t boot_device;   //===========================================
    uint32_t cmdline;       //
    uint32_t mods_count;    // (not used, needs to be defined regardless)
    uint32_t mods_addr;     //
    uint32_t syms[4];       //===========================================

    uint32_t mmap_length;   // byte length of the mmap buffer
    uint32_t mmap_addr;     // physical address of first mmap entry

}__attribute__((packed)) multiboot_info_t;

#endif