# ===== Orion OS Makefile =====

ASM  := nasm
CC   := i686-elf-gcc
AR   := i686-elf-ar
GRUB := grub-mkrescue

# kernel: can see all kernel headers
CFLAGS   := -ffreestanding -O2 -Wall -Wextra -I include

# liborion: freestanding userspace — must NOT see kernel headers
UFLAGS   := -ffreestanding -O2 -Wall -Wextra -I lib/liborion \
            -fno-builtin -nostdlib

ASMFLAGS := -f elf32
LDFLAGS  := -T linker.ld -ffreestanding -O2 -nostdlib

# ─────────────────────────────────────────────────────────────
# Kernel objects
# ─────────────────────────────────────────────────────────────
ASM_OBJS := \
	kernel/arch/x86/boot.o     \
	kernel/arch/x86/gdt_asm.o  \
	kernel/arch/x86/isr.o      \
	kernel/arch/x86/irq.o      \
	kernel/arch/x86/paging.o   \
	kernel/arch/x86/syscall.o

C_OBJS := \
	kernel/arch/x86/gdt.o      \
	kernel/arch/x86/idt.o      \
	kernel/arch/x86/tss.o      \
	kernel/arch/x86/irq_c.o    \
	kernel/mm/pmm.o             \
	kernel/mm/vmm.o             \
	kernel/mm/kheap.o           \
	kernel/fs/vfs.o             \
	kernel/fs/ramfs.o           \
	kernel/fs/devfs.o           \
	kernel/fs/pipe.o            \
	kernel/fs/namespace.o       \
	kernel/fs/pulsar.o          \
	kernel/proc/proc.o          \
	kernel/proc/sched.o         \
	kernel/proc/fork.o          \
	kernel/proc/exec.o          \
	kernel/proc/elf.o           \
	kernel/proc/fd.o            \
	kernel/syscall/syscall.o    \
	kernel/shell/shell.o        \
	kernel/drivers/serial.o     \
	kernel/drivers/vga.o        \
	kernel/drivers/timer.o      \
	kernel/drivers/keyboard.o   \
	kernel/panic.o              \
	kernel/kernel.o             \
	lib/libk/string.o           \
	lib/libk/kprintf.o

OBJS := $(ASM_OBJS) $(C_OBJS)

# ─────────────────────────────────────────────────────────────
# liborion — userspace runtime static library
# .lo = userspace object (distinct from kernel .o)
# ─────────────────────────────────────────────────────────────
LIBORION_OBJS := \
	lib/liborion/syscall.lo    \
	lib/liborion/start.lo      \
	lib/liborion/proc.lo       \
	lib/liborion/signal.lo     \
	lib/liborion/io.lo         \
	lib/liborion/malloc.lo     \
	lib/liborion/printf.lo     \
	lib/liborion/string.lo     \
	lib/liborion/env.lo

LIBORION := lib/liborion/liborion.a

ISODIR := isodir/boot

.PHONY: all clean run

all: myos.iso

# ─────────────────────────────────────────────────────────────
# liborion build rules
# ─────────────────────────────────────────────────────────────

# .asm -> .lo  (userspace asm — same format, different output name)
lib/liborion/syscall.lo: lib/liborion/syscall.asm
	@$(ASM) $(ASMFLAGS) $< -o $@

# .c -> .lo  (userspace C flags — no kernel headers)
lib/liborion/%.lo: lib/liborion/%.c
	@$(CC) $(UFLAGS) -c $< -o $@

# archive all .lo into liborion.a
$(LIBORION): $(LIBORION_OBJS)
	@echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
	@echo "┃                       Building liborion                           ┃"
	@echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
	@$(AR) rcs $@ $^

# ─────────────────────────────────────────────────────────────
# Kernel build rules (unchanged)
# ─────────────────────────────────────────────────────────────
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

kernel/arch/x86/syscall.o: kernel/arch/x86/syscall.asm
	@$(ASM) $(ASMFLAGS) $< -o $@

kernel/arch/x86/irq_c.o: kernel/arch/x86/irq.c
	@$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	@$(CC) $(CFLAGS) -c $< -o $@

# ─────────────────────────────────────────────────────────────
# Top-level targets
# ─────────────────────────────────────────────────────────────
myos: $(OBJS) $(LIBORION)
	@echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
	@echo "┃                          Linking Kernel                           ┃"
	@echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
	@$(CC) $(LDFLAGS) $(OBJS) -o myos -lgcc

myos.iso: myos
	@echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
	@echo "┃                          Creating ISO                             ┃"
	@echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
	@mkdir -p $(ISODIR)/grub
	@cp myos          $(ISODIR)/myos
	@cp boot/grub.cfg $(ISODIR)/grub/grub.cfg
	@$(GRUB) -o myos.iso isodir

clean:
	@echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
	@echo "┃                            MAKE CLEAN                             ┃"
	@echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
	@rm -f $(OBJS) $(LIBORION_OBJS) $(LIBORION) myos myos.iso
	@rm -rf isodir

run:
	@echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
	@echo "┃                             QEMU                                  ┃"
	@echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
	@qemu-system-x86_64 -cdrom myos.iso -serial stdio -no-reboot