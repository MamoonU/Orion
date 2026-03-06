[bits 32]

; eax = phys address of page directory
; cr0 = 32-bit control register = master switch which holds CPU control flags
; cr3 = 32-bit control register = CPU store of phys address of page directory
; paging_bit (paging enabled) = (bit 31) = 0x80000000

global enable_paging
global tlb_flush_page

tlb_flush_page:
    mov eax, [esp+4]    ; get virt address argument
    invlpg [eax]        ; flush that page from TLB
    ret
enable_paging:
    mov eax, [esp + 4]
    mov cr3, eax        ; (tell cpu where page tables live)
    mov eax, cr0        ; (read control flags into eax)
    or eax, 0x80000000  ; (set bits 31 to cr0 without changing other bits)
    mov cr0, eax        ; (write changed value back to cr0)
    ret
global vmm_load_cr3
vmm_load_cr3:
    mov eax, [esp+4]
    mov cr3, eax
    ret