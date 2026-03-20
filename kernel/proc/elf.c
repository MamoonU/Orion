// elf.c - ELF32 executable loader

#include "elf.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "kprintf.h"
#include "string.h"
#include "proc.h"
#include "sched.h"

// file positioning helper
static int elf_read_at(file_t *f, void *buf, uint32_t len, uint32_t offset) {

    f->offset = offset;                             // simulate pread()
    int n = vfs_read(f, buf, len);
    return n;

}

// load a single PT_LOAD segment
static int elf_load_segment(file_t *f, const Elf32_Phdr *ph, uint32_t pd_phys) {

    uint32_t vaddr   = ph->p_vaddr;     // virt address
    uint32_t filesz  = ph->p_filesz;    // bytes of file image to copy
    uint32_t memsz   = ph->p_memsz;     // bytes to occupy in memory (>= filesz; difference = BSS)
    uint32_t foffset = ph->p_offset;    // offset of segment data within the file

    if (filesz > memsz) {
        kprintf("ELF: corrupt segment: filesz %u > memsz %u\n", filesz, memsz);
        return -1;
    }

    if (memsz == 0) return 0;           // nothing to map

    // derive page flags from ELF segment flags instead of VMM_KERNEL_RW
    uint32_t page_flags = VMM_PRESENT | VMM_USER;
    if (ph->p_flags & 0x2u) {
        page_flags |= VMM_WRITABLE;                 // PF_W
    }

    // align addresses to page boundaries
    uint32_t page_start = vaddr & ~(PAGE_SIZE - 1);
    uint32_t page_end   = (vaddr + memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint32_t page = page_start; page < page_end; page += PAGE_SIZE) {              // allocate and map physical pages

        uint32_t frame = pmm_alloc_frame();                                             // allocate physical frame

        if (!frame) {
            kprintf("ELF: OOM allocating frame for vaddr 0x%p\n", page);                // OOM
            return -1;
        }

        memset((void *)frame, 0, PAGE_SIZE);                                             // zero mapped page: BSS and alignment padding are clean
        vmm_map_page_in(pd_phys, page, frame, page_flags);                              // map virtual -> physical
    }

    if (filesz > 0) {

        static uint8_t bounce[64 * 1024];                                               // 64 KB bounce buffer
        if (filesz > sizeof(bounce)) {
            kprintf("ELF: segment filesz %u exceeds bounce buffer\n", filesz);
            return -1;
        }

        int n = elf_read_at(f, bounce, filesz, foffset);                                // read from file into bounce buffer
        if (n < 0 || (uint32_t)n < filesz) {
            kprintf("ELF: short read at 0x%p (got %d, want %u)\n", vaddr, n, filesz);
            return -1;
        }

        uint32_t *pd    = (uint32_t *)pd_phys;
        uint32_t  written = 0;

        while (written < filesz) {                                                      // copy into map pages

            uint32_t  cur_virt = vaddr + written;                                       // compute virt addr
            uint32_t *pt       = (uint32_t *)(pd[cur_virt >> 22] & VMM_ADDR_MASK);      // find PT
            uint32_t  frame    = pt[(cur_virt >> 12) & 0x3FFu] & VMM_ADDR_MASK;         // find phys frame
            uint32_t  pg_off   = cur_virt & 0xFFFu;                                     // offset
            uint32_t  chunk    = PAGE_SIZE - pg_off;                                    // copy (only what fits in page)

            if (chunk > filesz - written) {
                chunk = filesz - written;
            }

            memcpy((void *)(frame + pg_off), bounce + written, chunk);                  // copy ELF bytes -> RAM
            written += chunk;
        }
    }
    kprintf("ELF:   PT_LOAD  0x%p  filesz=%u  memsz=%u  flags=%p\n", vaddr, filesz, memsz, page_flags);
    return 0;
}

// public loader: load executable & return entry point addr
uint32_t elf_load(const char *path) {

    if (!path) return 0;

    // resolve target PD from calling process
    pcb_t    *p       = sched_current();
    uint32_t  pd_phys = (p && p->page_directory) ? (uint32_t)p->page_directory : vmm_get_kernel_pd();

    file_t *f = vfs_open(path, O_RDONLY);                                               // open binary
    if (!f) {
        kprintf("ELF: cannot open \"%s\"\n", path);
        return 0;
    }

    Elf32_Ehdr ehdr;                                                                    // read and validate the ELF header

    int n = elf_read_at(f, &ehdr, sizeof(Elf32_Ehdr), 0);

    // ELF size
    if (n != (int)sizeof(Elf32_Ehdr)) {
        kprintf("ELF: \"%s\" too small for ELF header\n", path);
        goto fail;
    }
    // ELF magic
    if (ehdr.e_ident[0] != ELF_MAGIC0 || ehdr.e_ident[1] != ELF_MAGIC1 || ehdr.e_ident[2] != ELF_MAGIC2 || ehdr.e_ident[3] != ELF_MAGIC3) {
        kprintf("ELF: \"%s\" bad magic\n", path);
        goto fail;
    }
    // ELF class 
    if (ehdr.e_ident[4] != ELFCLASS32) {
        kprintf("ELF: \"%s\" not a 32-bit ELF\n", path);
        goto fail;
    }
    // ELF endianness
    if (ehdr.e_ident[5] != ELFDATA2LSB) {
        kprintf("ELF: \"%s\" not little-endian\n", path);
        goto fail;
    }
    // ELF type
    if (ehdr.e_type != ET_EXEC) {
        kprintf("ELF: \"%s\" not an executable (type=%u)\n", path, (uint32_t)ehdr.e_type);
        goto fail;
    }
    // ELF architecture
    if (ehdr.e_machine != EM_386) {
        kprintf("ELF: \"%s\" not IA-32 (machine=%u)\n", path, (uint32_t)ehdr.e_machine);
        goto fail;
    }
    // ELF header count
    if (ehdr.e_phnum == 0) {
        kprintf("ELF: \"%s\" has no program headers\n", path);
        goto fail;
    }
    // ELF version
    if (ehdr.e_version != 1) {
        kprintf("ELF: \"%s\" unsupported version %u\n", path, ehdr.e_version);
        goto fail;
    }

    kprintf("ELF: loading \"%s\"  entry=0x%p  phdrs=%u  pd=0x%p\n", path, ehdr.e_entry, (uint32_t)ehdr.e_phnum, pd_phys);

    // iterate program headers
    for (uint32_t i = 0; i < (uint32_t)ehdr.e_phnum; i++) {

        Elf32_Phdr phdr;
        uint32_t ph_off = ehdr.e_phoff + i * (uint32_t)ehdr.e_phentsize;        // location calculation

        n = elf_read_at(f, &phdr, sizeof(Elf32_Phdr), ph_off);
        if (n != (int)sizeof(Elf32_Phdr)) {
            kprintf("ELF: \"%s\" short read on phdr %u\n", path, i);
            goto fail;
        }

        if (phdr.p_type != PT_LOAD) continue;                                   // PT_LOAD segments only

        if (elf_load_segment(f, &phdr, pd_phys) != 0) goto fail;                         // map mem & copy file data
    }

    vfs_close(f);
    kprintf("ELF: \"%s\" loaded OK  entry=0x%p\n", path, ehdr.e_entry);         // return entry point
    return ehdr.e_entry;

fail:
    vfs_close(f);
    return 0;
}