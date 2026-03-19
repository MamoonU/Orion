// Virtual Memory Manager

#include "vmm.h"
#include "pmm.h"

#include "kprintf.h"
#include "panic.h"

extern void enable_paging(uint32_t pd_phys);                            // from paging.asm
extern void tlb_flush_page(uint32_t virt);                              // from paging.asm
extern void vmm_load_cr3(uint32_t pd_phys);


static uint32_t *page_directory = 0;    // physical address of page directory (= virtual address)
static uint32_t  kernel_pd_phys = 0;    // new PDs can copy kernel mappings

// 32 bit address layout = | PDE = 10 bits | PTE = 10 bits | OFFSET = 12 bits |
#define PD_INDEX(virt) ((virt) >> 22)                                               // extract top 10 bits
#define PT_INDEX(virt) (((virt) >> 12) & 0x3FFu)                                    // extract next 10 bits

#define VMM_PD_REGISTRY_MAX  256                            // 256 max process PD registers
static uint32_t pd_registry[VMM_PD_REGISTRY_MAX];           // live process PD registered here
static uint32_t pd_registry_count = 0;

// add process -> pd_registry
static void pd_registry_add(uint32_t pd_phys) {
    if (pd_registry_count >= VMM_PD_REGISTRY_MAX) {
        kprintf("VMM: pd_registry_add - registry full!\n");
        return;
    }
    pd_registry[pd_registry_count++] = pd_phys;
}

// remove process -> pd_registry
static void pd_registry_remove(uint32_t pd_phys) {
    for (uint32_t i = 0; i < pd_registry_count; i++) {
        if (pd_registry[i] == pd_phys) {
            pd_registry[i] = pd_registry[--pd_registry_count];
            pd_registry[pd_registry_count] = 0;
            return;
        }
    }
}

// push one kernel PDE -> every registered process PD
static void propagate_kernel_pde(uint32_t pd_idx) {
    for (uint32_t k = 0; k < pd_registry_count; k++) {
        if (!pd_registry[k]) continue;
        uint32_t *rpd = (uint32_t *)pd_registry[k];
        rpd[pd_idx] = page_directory[pd_idx];           // called from create_table() when new kernel PDE allocated
    }
}

// allocate PT -> install into kernel PD
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
    for (int i = 0; i < 1024; i++) {
        pt[i] = 0;
    }

    // PDE = always mark writable (per-page permissions enforced at PTE level)
    page_directory[pd_idx] = pt_phys | VMM_PRESENT | VMM_WRITABLE | (flags & VMM_USER);     // install into directory


    if (pd_idx < VMM_KERNEL_PDE_END) {                                          // if kernel PDE: push it to all registered process PDs
        propagate_kernel_pde(pd_idx);
    }

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

// read physical address of currently-active PD from CR3
uint32_t vmm_get_current_pd(void) {
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

// create new PD
uint32_t vmm_create_address_space(void) {

    uint32_t pd_phys = pmm_alloc_frame();

    if (!pd_phys) {                                                         // OOM check
        kprintf("VMM: vmm_create_address_space — OOM\n");
        return 0;
    }

    uint32_t *pd = (uint32_t *)pd_phys;                                     // allocate fresh 4KB PD
    uint32_t *kpd = (uint32_t *)kernel_pd_phys;                             // kernel 4KB PD

    for (int i = 0; i < 1024; i++) {                                        // PT frame is shared, !copied, so kernel mappings are always synced across all address spaces
        pd[i] = 0;
    }

    // copy kernel PDEs (PDE[0] -> VMM_KERNEL_PDE_END)
    for (uint32_t i = 0; i < VMM_KERNEL_PDE_END; i++) {
        pd[i] = kpd[i];
    }

    pd_registry_add(pd_phys);                                               //register for future propagation

    kprintf("VMM: new address space @ phys 0x%p (kernel PDEs 0..%u shared)\n", pd_phys);
    return pd_phys;                                                         // new PD = return physical address 
}

// free a PD 
void vmm_destroy_address_space(uint32_t pd_phys) {

    if (!pd_phys || pd_phys == kernel_pd_phys) return;                      // dont touch kernel space

    pd_registry_remove(pd_phys);                                            // unregister before any frees

    uint32_t *pd = (uint32_t *)pd_phys;
    uint32_t pt_count = 0;
    uint32_t pg_count = 0;

    // free user-space page tables (start -> VMM_KERNEL_PDE_END)
    for (uint32_t i = VMM_KERNEL_PDE_END; i < 1024; i++) {

        if (!(pd[i] & VMM_PRESENT)) continue;

        uint32_t  pt_phys = pd[i] & VMM_ADDR_MASK;
        uint32_t *pt      = (uint32_t *)pt_phys;

        // free every present page frame inside this table
        for (int j = 0; j < 1024; j++) {
            if (pt[j] & VMM_PRESENT) {
                pmm_free_frame(pt[j] & VMM_ADDR_MASK);
                pg_count++;
            }
            pmm_free_frame(pt_phys);    // free the page table frame
            pt_count++;
        }
        pmm_free_frame(pt_phys);    // free table
        kprintf("VMM: address space 0x%p destroyed (%u PTs, %u pages freed)\n", pd_phys, pt_count, pg_count);
    }
}

// context switch: called by scheduler on every context switch
void vmm_switch(uint32_t pd_phys) {
    if (pd_phys)
        vmm_load_cr3(pd_phys);                                              // load pd_phys into CR3 = flush entire TLB
}

// copy kernel PDEs (full kernel range) from  master kernel PD -> process PD
void vmm_sync_kernel_pdes(uint32_t pd_phys) {

    if (!pd_phys || pd_phys == kernel_pd_phys) return;

    uint32_t *pd  = (uint32_t *)pd_phys;
    uint32_t *kpd = (uint32_t *)kernel_pd_phys;

    for (uint32_t i = 0; i < VMM_KERNEL_PDE_END; i++) {     // VMM_KERNEL_PDE_END covers identity-mapped kernel + VGA
        pd[i] = kpd[i];                                     // extend loop if kernel grows beyond first 4 MB
    }
}

// map single 4KB page -> *explicit* PD (pd_phys) rather than into currently-active one
void vmm_map_page_in(uint32_t pd_phys, uint32_t virt, uint32_t phys, uint32_t flags) {

    if (!pd_phys) {                                     // NULL = use current (kernel) PD
        vmm_map_page(virt, phys, flags);
        return;
    }

    uint32_t *pd     = (uint32_t *)pd_phys;
    uint32_t  pd_idx = PD_INDEX(virt);                  // where to map
    uint32_t *pt;

    // get/create PT
    if (pd[pd_idx] & VMM_PRESENT) {                             // case A: PT exists
        pt = (uint32_t *)(pd[pd_idx] & VMM_ADDR_MASK);
    } else {                                                    // case B: no PT exists

        uint32_t pt_phys = pmm_alloc_frame();                   // alloc new table
        if (!pt_phys) {
            kprintf("VMM: vmm_map_page_in - OOM (pd=0x%p virt=0x%p)\n", pd_phys, virt);
            return;
        }
        pt = (uint32_t *)pt_phys;
        for (int i = 0; i < 1024; i++) {                        // zero table
            pt[i] = 0;
        }
        pd[pd_idx] = pt_phys | VMM_PRESENT | VMM_WRITABLE | (flags & VMM_USER);     // link to PD
    }

    pt[PT_INDEX(virt)] = (phys & VMM_ADDR_MASK) | (flags | VMM_PRESENT);            // create mapping
 
    if (vmm_get_current_pd() == pd_phys)                                            // flush TLB entry only when: this PD is active one to avoid
        tlb_flush_page(virt);
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



