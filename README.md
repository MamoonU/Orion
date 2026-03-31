<h1 align="center">Orion</h1>
<p align="center">
  <img src="https://github.com/MamoonU/Orion/raw/main/Orion.png" alt="Logo" width="350" height="350">
</p>

A minimal distributed OS based on Plan-9 philosophies (32-bit x86 architecture written in C)

## Dependencies

- **GCC Cross Compiler (i686-elf)** - used to compile the kernel for 32-bit x86
- **NASM**                          - assembler used for low-level assembly files
- **QEMU**                          - emulator used to run the OS
- **GRUB**                          - bootloader used to load the kernel
- **xorriso / grub-mkrescue**       - used to generate a bootable ISO image
- **GNU Make**                      - build automation

### Debian/Ubuntu:
```
sudo apt update && sudo apt upgrade

sudo apt install gcc-i686-linux-gnu
sudo apt install nasm
sudo apt install qemu-system-x86
sudo apt install grub-pc-bin
sudo apt install xorriso
sudo apt install build-essential
```

## Dev Commands
### Makefile Commands:
```
make clean   // clean project
make         // build project
make run     // run project
```
