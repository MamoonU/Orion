[bits 32]

extern isr_handler

%macro ISR_NOERR 1
global isr%1
isr%1:
    cli
    push dword 0          ; Fake error code
    push dword %1         ; Interrupt number
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    cli
    push dword %1         ; Interrupt number
    jmp isr_common_stub
%endmacro

ISR_NOERR 0     ; Divide by zero
ISR_NOERR 6     ; Invalid opcode
ISR_ERR   8     ; Double fault
ISR_ERR   13    ; GP fault
ISR_ERR   14    ; Page fault

isr_common_stub:
    pusha
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call isr_handler
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds
    popa

    add esp, 8      ; Pop error code + int number
    sti
    iret
