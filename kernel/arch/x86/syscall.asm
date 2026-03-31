; syscall.asm - int 0x80 kernel entry point

;   [esp+ 0] gs
;   [esp+ 4] fs
;   [esp+ 8] es
;   [esp+12] ds

;   [esp+16] edi
;   [esp+20] esi
;   [esp+24] ebp
;   [esp+28] esp                = from pusha
;   [esp+32] ebx
;   [esp+36] edx
;   [esp+40] ecx
;   [esp+44] eax                = syscall number in, return value out

;   [esp+48] err_code           = (always 0)
;   [esp+52] int_no             = (always 0x80)
;   [esp+56] eip
;   [esp+60] cs                 = pushed by CPU (ring-0 -> ring-0, no useresp/ss)
;   [esp+64] eflags

[bits 32]

extern syscall_dispatch         ; syscall.c
extern sched_switch_esp         ; sched.c
extern proc_deliver_signals     ; proc.c - check + deliver pending signals before iret

global syscall_entry
syscall_entry:
    cli                     ; disable interrupts

    push dword 0            ; fake err_code
    push dword 0x80         ; int_no

    pusha                   ; save all GPRs

    push ds                 ; save segment registers
    push es
    push fs
    push gs

    mov ax, 0x10            ; load kernel data segments
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp                ; call syscall dispatcher
    call syscall_dispatch   ; handler writes return value into r -> eax
    add esp, 4

    ; (optional) context switch
    ; sys_exit / sys_sleep set the process non-runnable before returning;
    ; sched_switch_esp will notice and hand the CPU to the next process
    push esp
    call sched_switch_esp
    add esp, 4

    test eax, eax           ; check scheduler results: 0 = stay, non-zero = new esp
    jz .no_switch
    mov esp, eax            ; switch to next process's kernel stack

.no_switch:
    push esp                    ; signal delivery
    call proc_deliver_signals
    add esp, 4

    pop gs                      ; restore segment registers
    pop fs
    pop es
    pop ds

    popa                        ; restore general registers
    add esp, 8                  ; discard err_code + int_no
    iret                        ; return to caller