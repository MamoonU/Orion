/* user/sh/linker_user.ld - userspace ELF linker script for Orion usermode binaries */

ENTRY(_start)

SECTIONS {

    . = 0x08048000;             /* base virtual address where program is loaded */

    /* .text = executable code */
    .text ALIGN(4096) : {
        *(.text._start)
        *(.text*)
    }

    /* .rodata = read-only data: string literals, const globals */
    .rodata ALIGN(4096) : {
        *(.rodata*)
    }

    /* .data = initialised data */
    .data ALIGN(4096) : {
        *(.data*)
    }

    /* .bss = uninitialised data: zeroed by the kernel ELF loader */
    .bss ALIGN(4096) : {
        *(COMMON)
        *(.bss*)
    }

    /* DISCARD = discarded sections: notes, comments, exception handling, toolchain specific items */
    /DISCARD/ : {
        *(.note*)
        *(.comment*)
        *(.eh_frame*)
        *(.gnu*)
    }
}
