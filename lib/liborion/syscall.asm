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

global syscall4

; int32_t syscall4(uint32_t num, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
syscall4:
    push    ebx                 ; save registers
    push    esi
    push    edi

    mov     eax, [esp+16]       ; num -> EAX
    mov     ebx, [esp+20]       ; arg1
    mov     ecx, [esp+24]       ; arg2
    mov     edx, [esp+28]       ; arg3
    mov     esi, [esp+32]       ; arg4

    int     0x80                ; trigger kernel

    pop     edi                 ; restore registers
    pop     esi
    pop     ebx
    ret                         ; return

global sigreturn_trampoline

; sigreturn_trampoline = kernel plants as return address on user stack before jumping to sig handler
sigreturn_trampoline:
    mov     eax, 23             ; SYS_SIGRETURN number = 23

    xor     ebx, ebx            ; clear registers
    xor     ecx, ecx
    xor     edx, edx

    int     0x80                ; trigger kernel

    hlt                         ; unreachable = kernel never returns here