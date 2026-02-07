[bits 32]

extern irq_handler          ; Found in irq.c

; macro defining ISR_NOERR, 1 parameter
%macro IRQ 1
global irq%1
irq%1:
    cli
    push dword 0          ; Fake error code
    push dword (32 + %1)  ; Interrupt number
    jmp isr_common_stub
%endmacro

isr_common_stub:
    pusha           ;save registers, preserve CPU state


    push ds
    push es
    push fs
    push gs

    mov ax, 0x10        ;set kernel segments
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp                ; pass pointer to stack
    call irq_handler
    add esp, 4

    pop gs              ; restore CPU state
    pop fs
    pop es
    pop ds

    popa
    add esp, 8      ; Pop error code + int number
    iret