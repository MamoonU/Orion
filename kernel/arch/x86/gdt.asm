[bits 32]

; load gdt
; reload DS/ES/FS/GS/SS/CS
; CS reload achieved with far jump

global gdt_flush

gdt_flush:

    mov eax, [esp+4]        ; pointer -> gdt pointer address
    lgdt [eax]              ; inform cpu: new gdt

    ; k-data reload
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; mov cs, ax
    jmp 0x08:.flush         ; far jump code selector = hidden descriptor caches = pain and suffering

.flush:
    ret