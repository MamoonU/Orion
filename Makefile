# ===== Orion OS Makefile =====

ASM      := nasm
CC       := i686-elf-gcc
GRUB     := grub-mkrescue

CFLAGS   := -ffreestanding -O2 -Wall -Wextra
ASMFLAGS := -f elf32
LDFLAGS  := -T linker.ld -ffreestanding -O2 -nostdlib

OBJS := boot_asm.o gdt_asm.o isr_asm.o irq_asm.o paging_asm.o \
        gdt.o idt.o irq.o pmm.o vmm.o timer.o keyboard.o kheap.o proc.o kernel.o

ISODIR := isodir/boot

all: clean myos.iso


# ================= CLEAN =================

clean:
	@echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
	@echo "┃                            MAKE CLEAN                             ┃"
	@echo "┣━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┫"
	@echo "┃                                                                   ┃"
	@rm -f $(OBJS) myos myos.iso
	@rm -rf isodir
	@echo "┃                      MAKE CLEAN: successful                       ┃"
	@echo "┃                                                                   ┃"
	@echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
	@echo ""


# ================= ASM FILES =================

boot_asm.o: boot.asm
	@echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
	@echo "┃                         Assembling ASM                            ┃"
	@echo "┣━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┫"
	@echo "┃                            .asm files:                            ┃"
	@echo "┃───────────────────────────────────────────────────────────────────┃"
	@echo "┃ boot.asm                                                          ┃"
	@$(ASM) $(ASMFLAGS) boot.asm -o boot_asm.o

gdt_asm.o: gdt.asm
	@echo "┃ gdt.asm                                                           ┃"
	@$(ASM) $(ASMFLAGS) gdt.asm -o gdt_asm.o

isr_asm.o: isr.asm
	@echo "┃ isr.asm                                                           ┃"
	@$(ASM) $(ASMFLAGS) isr.asm -o isr_asm.o

irq_asm.o: irq.asm
	@echo "┃ irq.asm                                                           ┃"
	@$(ASM) $(ASMFLAGS) irq.asm -o irq_asm.o

paging_asm.o: paging.asm
	@echo "┃ paging.asm                                                        ┃"
	@$(ASM) $(ASMFLAGS) paging.asm -o paging_asm.o
	@echo "┣━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┫"
	@echo "┃                       NASM Assembled Files:                       ┃"
	@echo "┃───────────────────────────────────────────────────────────────────┃"
	@echo "┃ boot_asm.o                                                        ┃"
	@echo "┃ gdt_asm.o                                                         ┃"
	@echo "┃ isr_asm.o                                                         ┃"
	@echo "┃ irq_asm.o                                                         ┃"
	@echo "┃ paging_asm.o                                                      ┃"
	@echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
	@echo ""


# ================= C FILES =================

gdt.o: gdt.c gdt.h
	@echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
	@echo "┃                           Compiling C                             ┃"
	@echo "┣━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┫"
	@echo "┃                            .c files:                              ┃"
	@echo "┃───────────────────────────────────────────────────────────────────┃"	
	@echo "┃ gdt.c                                                             ┃"
	@$(CC) $(CFLAGS) -c gdt.c -o gdt.o

idt.o: idt.c idt.h
	@echo "┃ idt.c                                                             ┃"
	@$(CC) $(CFLAGS) -c idt.c -o idt.o

irq.o: irq.c irq.h
	@echo "┃ irq.c                                                             ┃"
	@$(CC) $(CFLAGS) -c irq.c -o irq.o

pmm.o: pmm.c pmm.h multiboot.h
	@echo "┃ pmm.c                                                             ┃"
	@$(CC) $(CFLAGS) -c pmm.c -o pmm.o

vmm.o: vmm.c vmm.h
	@echo "┃ vmm.c                                                             ┃"
	@$(CC) $(CFLAGS) -c vmm.c -o vmm.o

timer.o: timer.c timer.h
	@echo "┃ timer.c                                                           ┃"
	@$(CC) $(CFLAGS) -c timer.c -o timer.o

keyboard.o: keyboard.c keyboard.h
	@echo "┃ keyboard.c                                                        ┃"
	@$(CC) $(CFLAGS) -c keyboard.c -o keyboard.o

kheap.o: kheap.c kheap.h
	@echo "┃ kheap.c                                                           ┃"
	@$(CC) $(CFLAGS) -c kheap.c -o kheap.o

proc.o: proc.c proc.h
	@echo "┃ proc.c                                                            ┃"
	@$(CC) $(CFLAGS) -c proc.c -o proc.o

kernel.o: kernel.c
	@echo "┃ kernel.c                                                          ┃"
	@$(CC) $(CFLAGS) -c kernel.c -o kernel.o
	@echo "┣━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┫"
	@echo "┃                       GCC Assembled files:                        ┃"	
	@echo "┃───────────────────────────────────────────────────────────────────┃"	
	@echo "┃ gdt.o                                                             ┃"
	@echo "┃ idt.o                                                             ┃"
	@echo "┃ irq.o                                                             ┃"
	@echo "┃ pmm.o                                                             ┃"
	@echo "┃ vmm.o                                                             ┃"
	@echo "┃ timer.o                                                           ┃"
	@echo "┃ keyboard.o                                                        ┃"
	@echo "┃ kernel.o                                                          ┃"
	@echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
	@echo ""


# ================= LINK =================

myos: $(OBJS)
	@echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
	@echo "┃                          Linking Kernel                           ┃"
	@echo "┣━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┫"
	@$(CC) $(LDFLAGS) $(OBJS) -o myos -lgcc
	@echo "┃                      Kernel Link Successful                       ┃"
	@echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
	@echo ""


# ================= ISO =================

myos.iso: myos
	@echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
	@echo "┃                                          Creating ISO                                              ┃"
	@echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┫"
	@mkdir -p $(ISODIR)/grub
	@cp myos $(ISODIR)/myos
	@cp grub.cfg $(ISODIR)/grub/grub.cfg
	@$(GRUB) -o myos.iso isodir
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
	@echo ""


# ================= RUN =================

run:
	@echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
	@echo "┃                                          QEMU                                          ┃"
	@echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┫"
	@qemu-system-x86_64 -cdrom myos.iso -serial stdio -no-reboot