[bits 32]

extern irq_handler              ; irq.c
extern sched_switch_esp         ; sched.c
extern proc_deliver_signals     ; proc.c - check + deliver pending signals before iret

; macro defining ISR_NOERR, 1 parameter
%macro IRQ 1
global irq%1
irq%1:
    cli
    push dword 0          ; fake error code
    push dword (32 + %1)  ; interrupt number
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
    add esp, 4              ; restore esp to frame top

    push esp                ; pass current esp ( regs_t* of current process)
    call sched_switch_esp   ; returns 0 or new esp
    add  esp, 4             ; restore esp to frame top

    test eax, eax           ; was a switch requested?
    jz   .no_switch
    mov  esp, eax           ; switch stack -> next process's irq frame

.no_switch:
    push esp                    ; pass current esp
    call proc_deliver_signals   ; check r->cs (if RPL != 3, return)
    add esp, 4                  ; restore esp to frame top

    pop gs                      ; restore CPU state
    pop fs
    pop es
    pop ds

    popa                        ; restore GPRs
    add esp, 8                  ; pop error code + int number
    iret                        ; return (ring-3: pops useresp + ss too)

global sched_start_first
sched_start_first:
    mov  esp, [esp+4]       ; load new_esp argument → switch to first process's stack

    pop gs                  ; restore segment registers from the fake frame
    pop fs
    pop es
    pop ds

    popa                    ; restore GPRs
    add esp, 8              ; discard int_no + err_code
    iret                    ; jump to entry_point with eflags

IRQ 0
IRQ 1
IRQ 2
IRQ 3
IRQ 4
IRQ 5
IRQ 6
IRQ 7
IRQ 8
IRQ 9
IRQ 10
IRQ 11
IRQ 12
IRQ 13
IRQ 14
IRQ 15