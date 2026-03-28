// elf.h - ELF32 executable loader interface

#ifndef ELF_H
#define ELF_H

#include <stdint.h>

// ELF identification: first 4 bytes of elf file
#define ELF_MAGIC0   0x7Fu
#define ELF_MAGIC1   'E'
#define ELF_MAGIC2   'L'
#define ELF_MAGIC3   'F'

#define ELFCLASS32   1          // 32-bit object
#define ELFDATA2LSB  1          // little-endian encoding
#define ET_EXEC      2          // executable file type
#define EM_386       3          // target ISA = Intel 80386

// program-header type
#define PT_LOAD      1          // loadable segment

// ELF32 types
typedef uint32_t Elf32_Addr;    // virtual memory address
typedef uint32_t Elf32_Off;     // file offset
typedef uint16_t Elf32_Half;    // 16-bit
typedef uint32_t Elf32_Word;    // 32-bit

// ELF32 file header (52 bytes)
typedef struct {

    uint8_t    e_ident[16];     // magic + class + data + version + padding
    Elf32_Half e_type;          // object file type (excecutable)
    Elf32_Half e_machine;       // target ISA (EM_386 = 3)
    Elf32_Word e_version;       // ELF version (must be 1)
    Elf32_Addr e_entry;         // entry point virtual address

    Elf32_Off  e_phoff;         // offset of program-header table in file
    Elf32_Off  e_shoff;         // offset of section-header table (ignored)
    Elf32_Word e_flags;         // architecture-specific flags

    Elf32_Half e_ehsize;        // size of this header (52 for ELF32)
    Elf32_Half e_phentsize;     // size of one program-header entry
    Elf32_Half e_phnum;         // number of program-header entries
    Elf32_Half e_shentsize;     // size of one section-header entry (unused)

    Elf32_Half e_shnum;         // number of section-header entries (unused)
    Elf32_Half e_shstrndx;      // section-name string table index (unused)

} __attribute__((packed)) Elf32_Ehdr;

// ELF32 program header (32 bytes)
typedef struct {

    Elf32_Word p_type;          // segment type (PT_LOAD = 1)
    Elf32_Off  p_offset;        // offset of segment data in file
    Elf32_Addr p_vaddr;         // desired virtual address in memory
    Elf32_Addr p_paddr;         // physical address (ignored on x86)

    Elf32_Word p_filesz;        // bytes in file (may be less than p_memsz)
    Elf32_Word p_memsz;         // bytes in memory (BSS padding = memsz - filesz)

    Elf32_Word p_flags;         // segment permissions (PF_R/PF_W/PF_X)
    Elf32_Word p_align;         // required alignment

} __attribute__((packed)) Elf32_Phdr;

// load ELF32 executable from VFS path into an explicit page directory
uint32_t elf_load_into(const char *path, uint32_t pd_phys);

// load ELF32 executable from VFS path
uint32_t elf_load(const char *path);

#endif // ELF_H