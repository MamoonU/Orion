# ===== Orion OS Makefile =====

.DEFAULT_GOAL := all

ASM  := nasm
CC   := i686-elf-gcc
AR   := i686-elf-ar
GRUB := grub-mkrescue

# kernel: can see all kernel headers
CFLAGS   := -ffreestanding -O2 -Wall -Wextra -I include

# liborion: freestanding userspace - must NOT see kernel headers
UFLAGS   := -ffreestanding -O2 -Wall -Wextra -I lib/liborion \
            -fno-builtin -nostdlib

ASMFLAGS := -f elf32
LDFLAGS  := -T linker.ld -ffreestanding -O2 -nostdlib
LWIP_CFLAGS := -ffreestanding -O2 \
               -I include \
               -I lib/lwip/include \
               -I lib/lwip/port \
               -Wno-unused-function \
               -Wno-unused-parameter \
               -Wno-address

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
	kernel/arch/x86/gdt.o		\
	kernel/arch/x86/idt.o		\
	kernel/arch/x86/tss.o		\
	kernel/arch/x86/irq_c.o		\
	kernel/mm/pmm.o             \
	kernel/mm/vmm.o             \
	kernel/mm/kheap.o           \
	kernel/fs/vfs.o             \
	kernel/fs/ramfs.o           \
	kernel/fs/devfs.o           \
	kernel/fs/pipe.o            \
	kernel/fs/namespace.o       \
	kernel/fs/pulsar.o          \
	kernel/fs/netfs.o           \
	kernel/fs/procfs.o          \
	kernel/fs/chatfs.o          \
	kernel/proc/proc.o          \
	kernel/proc/sched.o         \
	kernel/proc/fork.o          \
	kernel/proc/exec.o          \
	kernel/proc/elf.o           \
	kernel/proc/fd.o            \
	kernel/syscall/syscall.o    \
	kernel/drivers/serial.o     \
	kernel/drivers/vga.o        \
	kernel/drivers/timer.o      \
	kernel/drivers/keyboard.o   \
	kernel/drivers/pci.o        \
	kernel/drivers/virtio_net.o \
	kernel/panic.o              \
	kernel/kernel.o             \
	lib/libk/string.o           \
	lib/libk/kprintf.o

LWIP_CORE_OBJS := \
	lib/lwip/src/core/init.o          \
	lib/lwip/src/core/def.o           \
	lib/lwip/src/core/dns.o           \
	lib/lwip/src/core/inet_chksum.o   \
	lib/lwip/src/core/ip.o            \
	lib/lwip/src/core/mem.o           \
	lib/lwip/src/core/memp.o          \
	lib/lwip/src/core/netif.o         \
	lib/lwip/src/core/pbuf.o          \
	lib/lwip/src/core/raw.o           \
	lib/lwip/src/core/stats.o         \
	lib/lwip/src/core/sys.o           \
	lib/lwip/src/core/altcp.o         \
	lib/lwip/src/core/altcp_alloc.o   \
	lib/lwip/src/core/altcp_tcp.o     \
	lib/lwip/src/core/tcp.o           \
	lib/lwip/src/core/tcp_in.o        \
	lib/lwip/src/core/tcp_out.o       \
	lib/lwip/src/core/timeouts.o      \
	lib/lwip/src/core/udp.o           \
	lib/lwip/src/core/ipv4/autoip.o   \
	lib/lwip/src/core/ipv4/dhcp.o     \
	lib/lwip/src/core/ipv4/etharp.o   \
	lib/lwip/src/core/ipv4/icmp.o     \
	lib/lwip/src/core/ipv4/igmp.o     \
	lib/lwip/src/core/ipv4/ip4_addr.o \
	lib/lwip/src/core/ipv4/ip4.o      \
	lib/lwip/src/core/ipv4/ip4_frag.o \
	lib/lwip/src/netif/ethernet.o

LWIP_PORT_OBJS := \
	lib/lwip/port/sys_arch.o    \
	lib/lwip/port/orion_netif.o

LWIP_OBJS := $(LWIP_CORE_OBJS) $(LWIP_PORT_OBJS)

OBJS := $(ASM_OBJS) $(C_OBJS)

# ─────────────────────────────────────────────────────────────
# Usermode binaries
# ─────────────────────────────────────────────────────────────
USER_SH_SRC   	:= user/sh/sh.c
USER_SH_LD    	:= user/sh/linker_user.ld
USER_SH_ELF   	:= user/sh/sh.elf
USER_SH_BIN_O 	:= user/sh/sh_bin.o
USER_OTOP_SRC   := user/otop/otop.c
USER_OTOP_LD    := user/sh/linker_user.ld
USER_OTOP_ELF   := user/otop/otop.elf
USER_OTOP_BIN_O := user/otop/otop_bin.o

# ─────────────────────────────────────────────────────────────
# liborion - userspace runtime static library
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

# .asm -> .lo
lib/liborion/syscall.lo: lib/liborion/syscall.asm
	@$(ASM) $(ASMFLAGS) $< -o $@

# .c -> .lo
lib/liborion/%.lo: lib/liborion/%.c
	@$(CC) $(UFLAGS) -c $< -o $@

# archive .lo -> liborion.a
$(LIBORION): $(LIBORION_OBJS)
	@echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
	@echo "┃                       Building liborion                           ┃"
	@echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
	@$(AR) rcs $@ $^

# ─────────────────────────────────────────────────────────────
# user shell - must depend on liborion
# ─────────────────────────────────────────────────────────────
$(USER_SH_ELF): $(USER_SH_SRC) $(LIBORION) $(USER_SH_LD)
	@echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
	@echo "┃                       Building user shell                         ┃"
	@echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
	@$(CC) -T $(USER_SH_LD) $(UFLAGS) $(USER_SH_SRC) $(LIBORION) -lgcc -o $@

$(USER_SH_BIN_O): $(USER_SH_ELF)
	@i686-elf-objcopy -I binary -O elf32-i386 -B i386 $< $@

# ─────────────────────────────────────────────────────────────
# user otop
# ─────────────────────────────────────────────────────────────
$(USER_OTOP_ELF): $(USER_OTOP_SRC) $(LIBORION) $(USER_OTOP_LD)
	@echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
	@echo "┃                        Building user top                          ┃"
	@echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
	@$(CC) -T $(USER_OTOP_LD) $(UFLAGS) $(USER_OTOP_SRC) $(LIBORION) -lgcc -o $@

$(USER_OTOP_BIN_O): $(USER_OTOP_ELF)
	@i686-elf-objcopy -I binary -O elf32-i386 -B i386 $< $@

# ─────────────────────────────────────────────────────────────
# Kernel build rules
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

kernel/fs/netfs.o: kernel/fs/netfs.c
	@$(CC) $(CFLAGS) -I lib/lwip/include -I lib/lwip/port -Wno-unused-parameter -Wno-address -c $< -o $@

kernel/arch/x86/irq_c.o: kernel/arch/x86/irq.c
	@$(CC) $(CFLAGS) -c $< -o $@

lib/lwip/src/%.o: lib/lwip/src/%.c
	@$(CC) $(LWIP_CFLAGS) -c $< -o $@

lib/lwip/port/%.o: lib/lwip/port/%.c
	@$(CC) $(LWIP_CFLAGS) -c $< -o $@

%.o: %.c
	@$(CC) $(CFLAGS) -c $< -o $@

# ─────────────────────────────────────────────────────────────
# Top-level targets
# ─────────────────────────────────────────────────────────────
ISODIR := isodir/boot

.PHONY: all clean run
all: myos.iso

myos: $(LIBORION) $(OBJS) $(USER_SH_BIN_O) $(USER_OTOP_BIN_O) $(LWIP_OBJS)
	@echo "┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓"
	@echo "┃                          Linking Kernel                           ┃"
	@echo "┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛"
	@$(CC) $(LDFLAGS) $(OBJS) $(USER_SH_BIN_O) $(USER_OTOP_BIN_O) $(LWIP_OBJS) -o myos -lgcc

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
	@rm -f $(OBJS) $(LIBORION_OBJS) $(LIBORION) $(LWIP_OBJS) \
	       $(USER_SH_ELF) $(USER_SH_BIN_O) \
	       $(USER_OTOP_ELF) $(USER_OTOP_BIN_O) myos myos.iso
	@rm -rf isodir

QEMU_FLAGS := -cdrom myos.iso -serial stdio -no-reboot -m 128M
QEMU_NET   := -netdev socket,id=net0,mcast=230.0.0.1:1234

# Single instance - general dev (keep as-is)
run:
	@qemu-system-i386 \
		-cdrom myos.iso \
		-serial stdio \
		-no-reboot \
		-netdev user,id=net0 \
		-device virtio-net-pci,netdev=net0

run-a:
	@qemu-system-i386 $(QEMU_FLAGS) \
		$(QEMU_NET) \
		-device virtio-net-pci,netdev=net0,mac=52:54:00:00:00:0A

run-b:
	@qemu-system-i386 $(QEMU_FLAGS) \
		$(QEMU_NET) \
		-device virtio-net-pci,netdev=net0,mac=52:54:00:00:00:0B

run-c:
	@qemu-system-i386 $(QEMU_FLAGS) \
		$(QEMU_NET) \
		-device virtio-net-pci,netdev=net0,mac=52:54:00:00:00:0C

run-d:
	@qemu-system-i386 $(QEMU_FLAGS) \
		$(QEMU_NET) \
		-device virtio-net-pci,netdev=net0,mac=52:54:00:00:00:0D