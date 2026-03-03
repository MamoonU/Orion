# ===== Orion OS Makefile =====
ASM      := nasm
CC       := i686-elf-gcc
GRUB     := grub-mkrescue

CFLAGS   := -ffreestanding -O2 -Wall -Wextra
ASMFLAGS := -f elf32
LDFLAGS  := -T linker.ld -ffreestanding -O2 -nostdlib

OBJS := boot_asm.o gdt_asm.o isr_asm.o irq_asm.o paging_asm.o gdt.o idt.o irq.o pmm.o vmm.o timer.o keyboard.o kernel.o

ISODIR := isodir/boot

all: clean myos.iso

boot_asm.o: boot.asm
	$(ASM) $(ASMFLAGS) $< -o $@

gdt_asm.o: gdt.asm
	$(ASM) $(ASMFLAGS) $< -o $@

isr_asm.o: isr.asm
	$(ASM) $(ASMFLAGS) $< -o $@

irq_asm.o: irq.asm
	$(ASM) $(ASMFLAGS) $< -o $@

paging_asm.o: paging.asm
	$(ASM) $(ASMFLAGS) $< -o $@

gdt.o: gdt.c gdt.h
	$(CC) $(CFLAGS) -c $< -o $@

idt.o: idt.c idt.h
	$(CC) $(CFLAGS) -c $< -o $@

irq.o: irq.c irq.h
	$(CC) $(CFLAGS) -c $< -o $@

pmm.o: pmm.c pmm.h multiboot.h
	$(CC) $(CFLAGS) -c $< -o $@

vmm.o: vmm.c vmm.h
	$(CC) $(CFLAGS) -c $< -o $@

timer.o: timer.c timer.h
	$(CC) $(CFLAGS) -c $< -o $@

keyboard.o: keyboard.c keyboard.h
	$(CC) $(CFLAGS) -c $< -o $@

kernel.o: kernel.c
	$(CC) $(CFLAGS) -c $< -o $@

myos: $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $@ -lgcc

myos.iso: myos
	mkdir -p $(ISODIR)/grub
	cp myos $(ISODIR)/myos
	cp grub.cfg $(ISODIR)/grub/grub.cfg
	$(GRUB) -o myos.iso isodir

clean:
	rm -f $(OBJS) myos myos.iso
	rm -rf isodir