; syscall.asm - liborion syscall mechanism
[bits 32]
[section .text]

global syscall

syscall:
    push    ebx                 ; save registers
    push    esi
    push    edi

    mov     eax, [esp+16]       ; syscall number
    mov     ebx, [esp+20]       ; arg1
    mov     ecx, [esp+24]       ; arg2
    mov     edx, [esp+28]       ; arg3

    int     0x80                ; trigger kernel

    pop     edi                 ; restore registers
    pop     esi
    pop     ebx
    ret                         ; return