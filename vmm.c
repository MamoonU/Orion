// Virtual Memory Manager

#include "vmm.h"
#include "pmm.h"

extern void serial_write(const char *s);
extern void print_uint32_hex(uint32_t v);
extern void print_uint32_dec(uint32_t v);
extern void panic(const char *msg);
extern void enable_paging();                                            // from paging.asm
extern void tlb_flush_page();                                           // from paging.asm

// physical address of page directory (= virtual address)
static uint32_t *page_directory = 0;

// 32 bit address layout = | PDE = 10 bits | PTE = 10 bits | OFFSET = 12 bits |
#define PD_INDEX(virt) ((virt) >> 22)                                               // extract top 10 bits
#define PT_INDEX(virt) (((virt) >> 12) & 0x3FFu)                                    // extract next 10 bits

static uint32_t *create_table(uint32_t virt, uint32_t flags) {

    uint32_t pd_idx = PD_INDEX(virt);                                           // locate PDE

    if (page_directory[pd_idx] & VMM_PRESENT) {                                 // if table exists
        return (uint32_t *)(page_directory[pd_idx] & VMM_ADDR_MASK);            // return table( virtual address )
    }

    uint32_t pt_phys = pmm_alloc_frame();                                       // allocate zeroed 4KB frame for new PT
    if (pt_phys == 0) {
        serial_write("VMM: FATAL — out of physical memory for page table \n");
        return 0;
    }

    uint32_t *pt = (uint32_t *)pt_phys;                                         // clear every entry (all PTEs = !present)
    for (int i = 0; i < 1024; i++)
        pt[i] = 0;

    // PDE = always mark writable (per-page permissions enforced at PTE level)
    page_directory[pd_idx] = pt_phys | VMM_PRESENT | VMM_WRITABLE | (flags & VMM_USER);     // install into directory

    return pt;

}

// map single 4KB virtual page to physical page with given flags
void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags) {

    uint32_t *pt = create_table(virt, flags);                                   // return page table
    if (!pt) {
        panic("VMM: vmm_map_page — page table allocation failed");
    }

    uint32_t pt_idx = PT_INDEX(virt);                                           // construct PTE = | frame address | flags |
    pt[pt_idx] = (phys & VMM_ADDR_MASK) | (flags | VMM_PRESENT);

    tlb_flush_page(virt);
}

// map multiple consecutive pages
void vmm_map_range(uint32_t virt, uint32_t phys, uint32_t length, uint32_t flags) {

    uint32_t offset = 0;                                            // loop through memory in 4KB steps
    while (offset < length) {
        vmm_map_page(virt + offset, phys + offset, flags);          // call vmm_map_page for each page
        offset += PAGE_SIZE;
    }
}

// remove single virtual page mapping
void vmm_unmap_page(uint32_t virt) {

    uint32_t pd_idx = PD_INDEX(virt);                               // page table existence check
    if (!(page_directory[pd_idx] & VMM_PRESENT))
        return;

    uint32_t *pt = (uint32_t *)(page_directory[pd_idx] & VMM_ADDR_MASK);        // return page table
    pt[PT_INDEX(virt)] = 0;                                                     // clear page table entry

    tlb_flush_page(virt);
}

// return physical address mapped to a virtual address
uint32_t vmm_get_phys(uint32_t virt) {

    uint32_t pd_idx = PD_INDEX(virt);                               // PDE check
    if (!(page_directory[pd_idx] & VMM_PRESENT))
        return 0;
    
    uint32_t *pt = (uint32_t *)(page_directory[pd_idx] & VMM_ADDR_MASK);        // PTE check
    uint32_t pte = pt[PT_INDEX(virt)];
    if (!(pte & VMM_PRESENT))
        return 0;

    return (pte & VMM_ADDR_MASK) | (virt & 0xFFFu);                             // combine frame and offset

}

// return 1 = virtual page exists | return 0 = virtual page doesnt exist
int vmm_is_mapped(uint32_t virt) {

    uint32_t pd_idx = PD_INDEX(virt);                                           // PDE check
    if (!(page_directory[pd_idx] & VMM_PRESENT))
        return 0;

    uint32_t *pt = (uint32_t *)(page_directory[pd_idx] & VMM_ADDR_MASK);        // PTE check
    return (pt[PT_INDEX(virt)] & VMM_PRESENT) ? 1 : 0;                          // return 1 if VMM_PRESENT

}

void vmm_init(void) {

    serial_write("VMM: Initialising virtual memory manager \n");

    // allocate & zero page directory
    uint32_t pd_phys = pmm_alloc_frame();                                       // pd = 4KB frame
    if (pd_phys == 0)
        panic("VMM: Cannot allocate page directory frame ");
    
    page_directory = (uint32_t *)pd_phys;                                       // store pd pointer

    for (int i = 0; i < 1024; i++)                                              // zero all entries in pd
        page_directory[i] = 0;

    // identity mapping first 4MB
    serial_write("VMM: Identity mapping first 4MB (kernel + VGA + low memory)\n");

    uint32_t pt0_phys = pmm_alloc_frame();                                      // allocate first page table
    if (pt0_phys == 0)
        panic("VMM: Cannot allocate page table 0 frame");

    uint32_t *pt0 = (uint32_t *)pt0_phys;                                       // pt0 = virtual addresses ( 0x00000000 - 0x003FFFFF )

    for (int i = 0; i < 1024; i++) {                                            // fill first page table
        pt0[i] = ((uint32_t)i * PAGE_SIZE) | VMM_KERNEL_RW;                     // phys frame number -> virt page
    }                                                                           // mark pages present & writable

    // install page table -> PD[0]
    page_directory[0] = pt0_phys | VMM_KERNEL_RW;

    serial_write("VMM: Loading CR3 and enabling paging\n");
    enable_paging(pd_phys);
    serial_write("VMM: Paging enabled\n");

    serial_write("VMM: Page directory @ ");
    print_uint32_hex(pd_phys);
    serial_write("  |  Page table 0 @ ");
    print_uint32_hex(pt0_phys);
    serial_write("\n");
    serial_write("VMM: Ready\n");
    serial_write("\n");

}