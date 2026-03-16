; syscall.asm - liborion syscall mechanism
[bits 32]
[section .text]

global syscall

; int32_t syscall(uint32_t num, uint32_t a, uint32_t b, uint32_t c)
syscall:
    push    ebx                 ; save registers
    push    esi
    push    edi

    mov     eax, [esp+16]       ; syscall num
    mov     ebx, [esp+20]       ; arg1
    mov     ecx, [esp+24]       ; arg2
    mov     edx, [esp+28]       ; arg3

    int     0x80                ; trigger kernel

    pop     edi                 ; restore registers
    pop     esi
    pop     ebx
    ret                         ; return