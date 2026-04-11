// Virtual Memory Manager

#include "vmm.h"
#include "pmm.h"

#include "kprintf.h"
#include "panic.h"

extern void enable_paging(uint32_t pd_phys);                            // from paging.asm
extern void tlb_flush_page(uint32_t virt);                              // from paging.asm
extern void vmm_load_cr3(uint32_t pd_phys);

// extra window addresses
#define VMM_WIN_A  0x3FFFE000u
#define VMM_WIN_B  0x3FFFF000u

static uint32_t *page_directory = 0;    // physical address of page directory (= virtual address)
static uint32_t  kernel_pd_phys = 0;    // new PDs can copy kernel mappings

// 32 bit address layout = | PDE = 10 bits | PTE = 10 bits | OFFSET = 12 bits |
#define PD_INDEX(virt) ((virt) >> 22)                                               // extract top 10 bits
#define PT_INDEX(virt) (((virt) >> 12) & 0x3FFu)                                    // extract next 10 bits

#define VMM_PD_REGISTRY_MAX  256                            // 256 max process PD registers
static uint32_t pd_registry[VMM_PD_REGISTRY_MAX];           // live process PD registered here
static uint32_t pd_registry_count = 0;

// return kernel virt pointer -> phys frame
static uint32_t *phys_map_win(uint32_t phys, uint32_t win) {

    if (phys < 0x00400000u) {
        return (uint32_t *)phys;                           // identity region
    }
    
    // win_pt lives below 4 MB (allocated pre-paging), always reachable
    uint32_t *win_pt = (uint32_t *)(page_directory[255] & VMM_ADDR_MASK);
    win_pt[PT_INDEX(win)] = (phys & VMM_ADDR_MASK) | VMM_KERNEL_RW;
    tlb_flush_page(win);
    return (uint32_t *)win;
}

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

        uint32_t *rpd = (uint32_t *)phys_map_win(pd_registry[k], 1);
        rpd[pd_idx] = page_directory[pd_idx];
    }
}

// allocate PT -> install into kernel PD
static uint32_t create_table(uint32_t virt, uint32_t flags) {

    uint32_t pd_idx = PD_INDEX(virt);                                           // locate PDE

    if (page_directory[pd_idx] & VMM_PRESENT) {                                 // if table exists
        return (uint32_t *)(page_directory[pd_idx] & VMM_ADDR_MASK);            // return table( virtual address )
    }

    uint32_t pt_phys = pmm_alloc_frame();                                       // allocate zeroed 4KB frame for new PT
    if (pt_phys == 0) {
        kprintf("VMM: FATAL - out of physical memory for page table \n");
        return 0;
    }

    uint32_t *pt = phys_map_win(pt_phys, VMM_WIN_B);                            // zero the new PT through the physical window - safe for any address
    for (int i = 0; i < 1024; i++) {
        pt[i] = 0;
    }

    page_directory[pd_idx] = pt_phys | VMM_PRESENT | VMM_WRITABLE | (flags & VMM_USER);     // install into directory

    if (pd_idx < VMM_KERNEL_PDE_END)
        propagate_kernel_pde(pd_idx);

    return pt_phys;                                       // physical address only
}

// map single 4KB virtual page to physical page with given flags
void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags) {

    uint32_t pt_phys = create_table(virt, flags);
    if (!pt_phys) { panic("VMM: vmm_map_page - page table allocation failed"); }

    uint32_t *pt = phys_map_win(pt_phys, VMM_WIN_B);
    pt[PT_INDEX(virt)] = (phys & VMM_ADDR_MASK) | (flags | VMM_PRESENT);

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

    uint32_t pd_idx = PD_INDEX(virt);                                           // page table existence check
    if (!(page_directory[pd_idx] & VMM_PRESENT)) return;

    uint32_t pt_phys = page_directory[pd_idx] & VMM_ADDR_MASK;
    uint32_t *pt     = phys_map_win(pt_phys, VMM_WIN_B);
    pt[PT_INDEX(virt)] = 0;                                                     // clear page table entry

    tlb_flush_page(virt);
}

// return physical address mapped to a virtual address
uint32_t vmm_get_phys(uint32_t virt) {

    uint32_t pd_idx = PD_INDEX(virt);
    if (!(page_directory[pd_idx] & VMM_PRESENT)) return 0;

    uint32_t pt_phys = page_directory[pd_idx] & VMM_ADDR_MASK;
    uint32_t *pt     = phys_map_win(pt_phys, VMM_WIN_B);

    uint32_t  pte    = pt[PT_INDEX(virt)];
    if (!(pte & VMM_PRESENT)) return 0;

    return (pte & VMM_ADDR_MASK) | (virt & 0xFFFu);
}

// return 1 = virtual page exists | return 0 = virtual page doesnt exist
int vmm_is_mapped(uint32_t virt) {

    uint32_t pd_idx = PD_INDEX(virt);
    if (!(page_directory[pd_idx] & VMM_PRESENT)) return 0;

    uint32_t pt_phys = page_directory[pd_idx] & VMM_ADDR_MASK;
    uint32_t *pt     = phys_map_win(pt_phys, VMM_WIN_B);

    return (pt[PT_INDEX(virt)] & VMM_PRESENT) ? 1 : 0;
}

void vmm_init(void) {

    kprintf("VMM: Initialising virtual memory manager \n");

    // allocate & zero page directory
    uint32_t pd_phys = pmm_alloc_frame();                                       // pd = 4KB frame
    if (pd_phys == 0) {
        panic("VMM: Cannot allocate page directory frame ");
    }

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


    // physical window PT (PDE[255])
    uint32_t win_pt_phys = pmm_alloc_frame();
    if (!win_pt_phys) {
        panic("VMM: Cannot allocate window PT");
    }
    uint32_t *win_pt = (uint32_t *)win_pt_phys;
    for (int i = 0; i < 1024; i++) {
        win_pt[i] = 0;
    }
    page_directory[255] = win_pt_phys | VMM_KERNEL_RW;

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
        kprintf("VMM: vmm_create_address_space - OOM\n");
        return 0;
    }

    int pd_was_mapped = vmm_is_mapped(pd_phys);
    if (!pd_was_mapped) {
        vmm_map_page(pd_phys, pd_phys, VMM_KERNEL_RW);                      // map before use: only if above 4MB identity region
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

    if (!pd_was_mapped)
    vmm_unmap_page(pd_phys);                                                // only remove if we added it: don't disturb identity region

    kprintf("VMM: new address space @ phys 0x%p (kernel PDEs 0..%u shared)\n", pd_phys, VMM_KERNEL_PDE_END - 1);
    return pd_phys;                                                         // new PD = return physical address 
}

// free a PD 
void vmm_destroy_address_space(uint32_t pd_phys) {

    if (!pd_phys || pd_phys == kernel_pd_phys) return;                      // dont touch kernel space

    int was_mapped = vmm_is_mapped(pd_phys);
    if (!was_mapped) {
        vmm_map_page(pd_phys, pd_phys, VMM_KERNEL_RW);                      // map PD before reading it
    }

    uint32_t *pd  = (uint32_t *)pd_phys;
    uint32_t *kpd = (uint32_t *)kernel_pd_phys;                             // need kernel PD to identify shared vs private PTsz
    uint32_t pt_count = 0;
    uint32_t pg_count = 0;

    for (uint32_t i = 0; i < 1024; i++) {

        if (!(pd[i] & VMM_PRESENT)) continue;
        if (pd[i] == kpd[i]) continue;                                      // shared kernel PT — never free

        uint32_t pt_phys = pd[i] & VMM_ADDR_MASK;

        int pt_was_mapped = vmm_is_mapped(pt_phys);
        if (!pt_was_mapped)
            vmm_map_page(pt_phys, pt_phys, VMM_KERNEL_RW);                  // map PT before reading it

        uint32_t *pt = (uint32_t *)pt_phys;

        for (int j = 0; j < 1024; j++) {
            if (pt[j] & VMM_PRESENT) {
                pmm_free_frame(pt[j] & VMM_ADDR_MASK);                      // free each mapped page frame
                pg_count++;
            }
        }

        if (!pt_was_mapped)
            vmm_unmap_page(pt_phys);                                        // done reading PT, remove temp mapping

        pmm_free_frame(pt_phys);                                            // free the PT frame itself
        pt_count++;
    }

    if (!was_mapped)
        vmm_unmap_page(pd_phys);                                            // done reading PD, remove temp mapping

    pmm_free_frame(pd_phys);                                                // free the PD frame itself — was missing entirely

    kprintf("VMM: address space 0x%p destroyed (%u PTs, %u pages freed)\n", pd_phys, pt_count, pg_count);
}

// context switch: called by scheduler on every context switch
void vmm_switch(uint32_t pd_phys) {
    if (pd_phys)
        vmm_load_cr3(pd_phys);                                              // load pd_phys into CR3 = flush entire TLB
}

// copy kernel PDEs (full kernel range) from  master kernel PD -> process PD
void vmm_sync_kernel_pdes(uint32_t pd_phys) {

    if (!pd_phys || pd_phys == kernel_pd_phys) return;

    uint32_t *pd  = phys_map_win(pd_phys, VMM_WIN_A);
    uint32_t *kpd = (uint32_t *)kernel_pd_phys;             // always below 4 MB

    for (uint32_t i = 0; i < VMM_KERNEL_PDE_END; i++)       // VMM_KERNEL_PDE_END covers identity-mapped kernel + VGA
        pd[i] = kpd[i];                                     // extend loop if kernel grows beyond first 4 MB
}

// map single 4KB page -> *explicit* PD (pd_phys) rather than into currently-active one
void vmm_map_page_in(uint32_t pd_phys, uint32_t virt, uint32_t phys, uint32_t flags) {

    if (!pd_phys) {
        vmm_map_page(virt, phys, flags);
        return;
    }

    int pd_was_mapped = vmm_is_mapped(pd_phys);
    if (!pd_was_mapped)
        vmm_map_page(pd_phys, pd_phys, VMM_KERNEL_RW);     // map PD before reading it

    uint32_t *pd     = (uint32_t *)pd_phys;
    uint32_t  pd_idx = PD_INDEX(virt);
    uint32_t  pt_phys;

    if (pd[pd_idx] & VMM_PRESENT) {                        // case A: PT exists
        pt_phys = pd[pd_idx] & VMM_ADDR_MASK;
    } else {                                               // case B: no PT exists — allocate and zero it

        pt_phys = pmm_alloc_frame();
        if (!pt_phys) {
            kprintf("VMM: vmm_map_page_in - OOM (pd=0x%p virt=0x%p)\n", pd_phys, virt);
            if (!pd_was_mapped) vmm_unmap_page(pd_phys);   // clean up before returning
            return;
        }

        int pt_init_was_mapped = vmm_is_mapped(pt_phys);
        if (!pt_init_was_mapped)
            vmm_map_page(pt_phys, pt_phys, VMM_KERNEL_RW); // map new PT so we can zero it

        uint32_t *pt_init = (uint32_t *)pt_phys;
        for (int i = 0; i < 1024; i++) pt_init[i] = 0;    // zero new PT

        if (!pt_init_was_mapped)
            vmm_unmap_page(pt_phys);                        // done zeroing, remove temp mapping

        pd[pd_idx] = pt_phys | VMM_PRESENT | VMM_WRITABLE | (flags & VMM_USER);    // link PT -> PD
    }

    int pt_was_mapped = vmm_is_mapped(pt_phys);
    if (!pt_was_mapped)
        vmm_map_page(pt_phys, pt_phys, VMM_KERNEL_RW);     // map PT before writing PTE

    uint32_t *pt = (uint32_t *)pt_phys;
    pt[PT_INDEX(virt)] = (phys & VMM_ADDR_MASK) | (flags | VMM_PRESENT);           // write PTE

    if (!pt_was_mapped)
        vmm_unmap_page(pt_phys);                            // done writing PTE, remove temp mapping

    if (!pd_was_mapped)
        vmm_unmap_page(pd_phys);                            // done with PD, remove temp mapping

    if (vmm_get_current_pd() == pd_phys)
        tlb_flush_page(virt);                               // flush TLB only if this PD is currently active
}

// is range mapped

int vmm_range_mapped(uint32_t *pd, uint32_t virt, uint32_t len, int user_only) {

    if (!pd || !len) return 0;

    uint32_t addr = virt & VMM_ADDR_MASK;                                       // align down to page boundary
    uint32_t end  = (virt + len + PAGE_SIZE - 1) & VMM_ADDR_MASK;

    while (addr < end) {                                                        // walk pd for every page covering (virt, virt+len)

        uint32_t pde = pd[PD_INDEX(addr)];
        if (!(pde & VMM_PRESENT)) return 0;
        if (user_only && !(pde & VMM_USER)) return 0;

        uint32_t pt_phys = pde & VMM_ADDR_MASK;
        uint32_t *pt     = phys_map_win(pt_phys, VMM_WIN_B);
        uint32_t  pte    = pt[PT_INDEX(addr)];

        if (!(pte & VMM_PRESENT)) return 0;
        if (user_only && !(pte & VMM_USER)) return 0;

        addr += PAGE_SIZE;                                                      // return 0 if any page is absent or fails the user check
    }
    return 1;                                                                   // return 1 if every page is present (and has VMM_USER if user_only=1)
}

