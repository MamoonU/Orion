# Orion
A minimal distributed OS based on Plan-9 philosophies

Inline-style: 
![alt text](https://github.com/MamoonU/Orion/OrionInverted.png "Logo")


## Dev Commands

### assemble boot.s =
i686-elf-as boot.s -o boot.o

### assemble kernel.c =
i686-elf-gcc -c kernel.c -o kernel.o -std=gnu99 -ffreestanding -O2 -Wall -Wextra (C implementation of kernel)

### link kernel and boot =
i686-elf-gcc -T linker.ld -o myos -ffreestanding -O2 -nostdlib boot.o kernel.o -lgcc

### creating bootable .iso =
mkdir -p isodir/boot/grub

cp myos isodir/boot/myos

cp grub.cfg isodir/boot/grub/grub.cfg

grub-mkrescue -o myos.iso isodir

### run cdrom image =
qemu-system-x86_64 -cdrom myos.iso -serial stdio -no-reboot

