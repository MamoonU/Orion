#include "gdt.h"
#include "kprintf.h"

extern void gdt_flush(uint32_t);                //gdt.asm

static struct gdt_entry gdt[5];         // allocate gdt (5 entries = null, k-code, k-data, u-code, u-data)
static struct gdt_ptr   gdtp;           // allocate??? gdt registers

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {   // output = index, base, limit, access, granularity

    // base = low | mid<<16 | high<<24
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_mid = (base >> 16) & 0xFF;
    gdt[num].base_high = (base >> 24) & 0xFF;

    gdt[num].limit_low   = (limit & 0xFFFF);                                // limit 4GB
    gdt[num].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);          // limit + flags (1 byte)

    gdt[num].access = access;

}

void gdt_init(void) {

    gdtp.limit = (sizeof(struct gdt_entry) * 5) - 1;            // size - 1
    gdtp.base = (uint32_t)&gdt;                               // base address (1 byte)

    gdt_set_gate(0, 0, 0, 0, 0);                //null descriptor (required (i found out the hard way))


    // kernel = code segment (0x08)
    //          (num, base, limit, access, gran)
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);             // index = 1, base = 0, limit = 4GB, ring 0 - executable & readable, selector: 1 << 3 = 8 = 0x08

    // kernel = data segment (0x10)
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);             // index = 2, base = 0, limit = 4GB, ring 0 - writable, selector: 2 << 3 = 16 = 0x10

    // user = code segment (0x18)
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);             // index = 3, base = 0, limit = 4GB, ring 3 - executable & readable, selector: 3 << 3 = 24 = 0x18

    // user = data segment (0x20)
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);             // index = 4, base = 0, limit = 4GB, ring 3 - writable, selector: 4 << 3 = 32 = 0x20

    gdt_flush((uint32_t)&gdtp);
    // load gdt [gdtp]
    // reload segment registers
    // reload code segment -> far jump

    kprintf("GDT: Loaded\n");

}