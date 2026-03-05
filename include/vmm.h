// Virtual Memory Manager Header

#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include "pmm.h"

// page entries = frame address (top 20 bits) | flags (bottom 12 bits)

// (page directory entry / page table entry) flag bits
#define VMM_PRESENT (1u << 0)   // 0000000[1] = mapped and accessible page | 0000000[0] = page fault
#define VMM_WRITABLE (1u << 1)  // 000000[1]1 = writable                   | 000000[0]1 = read only
#define VMM_USER (1u << 2)      // 00000[1]01 = ring 3 access (user)       | 00000[0]01 = ring 1 access only (kernel)
#define VMM_WRITETHRU (1u << 3) // 0000[1]001 = write-through caching      | 0000[0]001 = N/A
#define VMM_NOCACHE (1u << 4)   // 000[1]0001 = disable caching            | 000[0]0001 = N/A
#define VMM_ACCESSED (1u << 5)  // 00[1]00001 = CPU sets when read         | 00[0]00001 = N/A
#define VMM_DIRTY (1u << 6)     // 0[1]000001 = CPU sets when write (PTE)  | 0[0]000001 = N/A

// convenient combos
#define VMM_KERNEL_RW   (VMM_PRESENT | VMM_WRITABLE)                    // Kernel read-write mapping
#define VMM_KERNEL_RO   (VMM_PRESENT)                                   // kernel read only
#define VMM_MMIO        (VMM_PRESENT | VMM_WRITABLE | VMM_NOCACHE)      // memory-mapped-input-output

// physical frame address = (page entries - flags)
#define VMM_ADDR_MASK   0xFFFFF000u

void vmm_init(void);

void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);

void vmm_map_range(uint32_t virt, uint32_t phys, uint32_t length, uint32_t flags);

void vmm_unmap_page(uint32_t virt);

uint32_t vmm_get_phys(uint32_t virt);

int vmm_is_mapped(uint32_t virt);

#endif