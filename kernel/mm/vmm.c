// Virtual Memory Manager

#include "vmm.h"
#include "pmm.h"

#include "kprintf.h"
#include "panic.h"

extern void enable_paging(uint32_t pd_phys);                            // from paging.asm
extern void tlb_flush_page(uint32_t virt);                              // from paging.asm


static uint32_t *page_directory = 0;    // physical address of page directory (= virtual address)
static uint32_t  kernel_pd_phys = 0;    // new PDs can copy kernel mappings

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
        kprintf("VMM: FATAL — out of physical memory for page table \n");
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

    uint32_t offset = 0;
    while (offset < length) {                                       // loop through memory in 4KB steps
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

    kprintf("VMM: Initialising virtual memory manager \n");

    // allocate & zero page directory
    uint32_t pd_phys = pmm_alloc_frame();                                       // pd = 4KB frame
    if (pd_phys == 0)
        panic("VMM: Cannot allocate page directory frame ");
    
    page_directory = (uint32_t *)pd_phys;                                       // store pd pointer
    kernel_pd_phys = pd_phys;                                                   // store for new address spaces

    for (int i = 0; i < 1024; i++)                                              // zero all entries in pd
        page_directory[i] = 0;

    // identity mapping first 4MB
    kprintf("VMM: Identity mapping first 4MB (kernel + VGA + low memory)\n");

    uint32_t pt0_phys = pmm_alloc_frame();                                      // allocate first page table
    if (pt0_phys == 0)
        panic("VMM: Cannot allocate page table 0 frame");

    uint32_t *pt0 = (uint32_t *)pt0_phys;                                       // pt0 = virtual addresses ( 0x00000000 - 0x003FFFFF )

    for (int i = 0; i < 1024; i++) {                                            // fill first page table
        pt0[i] = ((uint32_t)i * PAGE_SIZE) | VMM_KERNEL_RW;                     // phys frame number -> virt page
    }                                                                           // mark pages present & writable

    // install page table -> PD[0]
    page_directory[0] = pt0_phys | VMM_KERNEL_RW;

    kprintf("VMM: Loading CR3 and enabling paging\n");
    enable_paging(pd_phys);
    kprintf("VMM: Paging enabled\n");

    kprintf("VMM: Page directory @ %p  |  Page table 0 @ %p\n", pd_phys, pt0_phys);
    kprintf("VMM: Ready\n\n");

}

extern void vmm_load_cr3(uint32_t pd_phys);

uint32_t vmm_get_kernel_pd(void) {
    return kernel_pd_phys;
}

// create new PD
uint32_t vmm_create_address_space(void) {

    uint32_t pd_phys = pmm_alloc_frame();
    if (!pd_phys) {                                                         // OOM check
        kprintf("VMM: vmm_create_address_space — OOM\n");
        return 0;
    }

    uint32_t *pd = (uint32_t *)pd_phys;                                     // allocate fresh 4KB page directory

    for (int i = 0; i < 1024; i++)                                          // zero all 1024 entries
        pd[i] = 0;

    // copy kernel PDEs (first 4MB = PDE[0]) from the master kernel PD
    uint32_t *kpd = (uint32_t *)kernel_pd_phys;                             // PT frame is shared, !copied, so kernel mappings are always synced across all address spaces
    for (int i = 0; i < 1; i++)                                             // only PDE[0] = 0x00000000–0x003FFFFF
        pd[i] = kpd[i];

    kprintf("VMM: new address space @ phys 0x%p\n", pd_phys);
    return pd_phys;                                                         // new PD = return physical address 
}

// free a PD 
void vmm_destroy_address_space(uint32_t pd_phys) {

    if (!pd_phys || pd_phys == kernel_pd_phys) return;                      // dont touch kernel space

    uint32_t *pd = (uint32_t *)pd_phys;

    // free user-space page tables (skip PDE[0] = kernel)
    for (int i = 1; i < 1024; i++) {
        if (pd[i] & VMM_PRESENT) {
            uint32_t pt_phys = pd[i] & VMM_ADDR_MASK;
            pmm_free_frame(pt_phys);
        }
    }

    pmm_free_frame(pd_phys);
    kprintf("VMM: address space 0x%p destroyed\n", pd_phys);
}

// context switch: called by scheduler on every context switch
void vmm_switch(uint32_t pd_phys) {
    if (pd_phys)
        vmm_load_cr3(pd_phys);                                              // load pd_phys into CR3 = flush entire TLB
}

// is range mapped
int vmm_range_mapped(uint32_t *pd, uint32_t virt, uint32_t len, int user_only) {

    if (!pd || !len) return 0;

    uint32_t addr = virt & VMM_ADDR_MASK;                                   // align down to page boundary
    uint32_t end  = (virt + len + PAGE_SIZE - 1) & VMM_ADDR_MASK;

    while (addr < end) {                                                    // walk pd for every page covering (virt, virt+len)

        uint32_t pde = pd[PD_INDEX(addr)];
        if (!(pde & VMM_PRESENT)) return 0;
        if (user_only && !(pde & VMM_USER))  return 0;

        uint32_t *pt  = (uint32_t *)(pde & VMM_ADDR_MASK);
        uint32_t  pte = pt[PT_INDEX(addr)];
        if (!(pte & VMM_PRESENT)) return 0;
        if (user_only && !(pte & VMM_USER))  return 0;

        addr += PAGE_SIZE;                                                  // return 0 if any page is absent or fails the user check
    }
    return 1;                                                               // return 1 if every page is present (and has VMM_USER if user_only=1)
}



