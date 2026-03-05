# ===== Orion OS Makefile =====

ASM  := nasm
CC   := i686-elf-gcc
GRUB := grub-mkrescue

CFLAGS   := -ffreestanding -O2 -Wall -Wextra -I include
ASMFLAGS := -f elf32
LDFLAGS  := -T linker.ld -ffreestanding -O2 -nostdlib

ASM_OBJS := \
	kernel/arch/x86/boot.o     \
	kernel/arch/x86/gdt_asm.o  \
	kernel/arch/x86/isr.o      \
	kernel/arch/x86/irq.o      \
	kernel/arch/x86/paging.o

C_OBJS := \
	kernel/arch/x86/gdt.o      \
	kernel/arch/x86/idt.o      \
	kernel/arch/x86/irq_c.o    \
	kernel/mm/pmm.o             \
	kernel/mm/vmm.o             \
	kernel/mm/kheap.o           \
	kernel/proc/proc.o          \
	kernel/drivers/serial.o     \
	kernel/drivers/vga.o        \
	kernel/drivers/timer.o      \
	kernel/drivers/keyboard.o   \
	kernel/panic.o              \
	kernel/kernel.o             \
	lib/libk/string.o           \
	lib/libk/kprintf.o

OBJS := $(ASM_OBJS) $(C_OBJS)

ISODIR := isodir/boot

all: clean myos.iso

clean:
	@echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
	@echo "┃                            MAKE CLEAN                             ┃"
	@echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
	@rm -f $(OBJS) myos myos.iso
	@rm -rf isodir

kernel/arch/x86/boot.o:    kernel/arch/x86/boot.asm
	@$(ASM) $(ASMFLAGS) $< -o $@

kernel/arch/x86/gdt_asm.o: kernel/arch/x86/gdt.asm
	@$(ASM) $(ASMFLAGS) $< -o $@

kernel/arch/x86/isr.o:     kernel/arch/x86/isr.asm
	@$(ASM) $(ASMFLAGS) $< -o $@

kernel/arch/x86/irq.o:     kernel/arch/x86/irq.asm
	@$(ASM) $(ASMFLAGS) $< -o $@

kernel/arch/x86/paging.o:  kernel/arch/x86/paging.asm
	@$(ASM) $(ASMFLAGS) $< -o $@

kernel/arch/x86/irq_c.o: kernel/arch/x86/irq.c
	@$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	@$(CC) $(CFLAGS) -c $< -o $@

myos: $(OBJS)
	@echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
	@echo "┃                          Linking Kernel                           ┃"
	@echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
	@$(CC) $(LDFLAGS) $(OBJS) -o myos -lgcc


myos.iso: myos
	@echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
	@echo "┃                          Creating ISO                             ┃"
	@echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
	@mkdir -p $(ISODIR)/grub
	@cp myos         $(ISODIR)/myos
	@cp boot/grub.cfg $(ISODIR)/grub/grub.cfg
	@$(GRUB) -o myos.iso isodir


run:
	@echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
	@echo "┃                             QEMU                                  ┃"
	@echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
	@qemu-system-x86_64 -cdrom myos.iso -serial stdio -no-reboot
