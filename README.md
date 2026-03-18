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
## Current Progress

<p>
<img src="https://img.shields.io/badge/Boot_%26_Architecture-6%2F6-09ed09?style=for-the-badge" alt="Boot & Architecture">
<img src="https://img.shields.io/badge/Memory-3%2F5-e66a05?style=for-the-badge" alt="Memory">
<img src="https://img.shields.io/badge/Processes-8%2F10-aaed0e?style=for-the-badge" alt="Processes">
<img src="https://img.shields.io/badge/System_Calls-2%2F3-aaed0e?style=for-the-badge" alt="System Calls">
<img src="https://img.shields.io/badge/Virtual_Filesystem-4%2F5-aaed0e?style=for-the-badge" alt="Virtual Filesystem">
<img src="https://img.shields.io/badge/Namespaces-4%2F5-aaed0e?style=for-the-badge" alt="Namespaces">
<img src="https://img.shields.io/badge/PULSAR-6%2F7-aaed0e?style=for-the-badge" alt="PULSAR">
<img src="https://img.shields.io/badge/Networking-0%2F3-e61405?style=for-the-badge" alt="Networking">
<img src="https://img.shields.io/badge/User_Runtime-0%2F4-e61405?style=for-the-badge" alt="User Runtime">
<img src="https://img.shields.io/badge/Shell-3%2F4-aaed0e?style=for-the-badge" alt="Shell">
<img src="https://img.shields.io/badge/Applications-0%2F3-e61405?style=for-the-badge" alt="Applications">
</p>

<!-- red = e61405 -->
<!-- orange = e66a05 -->
<!-- yellow = aaed0e -->
<!-- green = 09ed09 -->

<details>
<summary><b>Full breakdown</b></summary>

| **Boot & Architecture** | Status |
|---|---|
| Multiboot-compliant boot stub (GRUB) | ✅ |
| UART serial output | ✅ |
| VGA text-mode terminal | ✅ |
| x86 GDT (kernel + user segments) | ✅ |
| x86 IDT, ISRs, IRQ handling | ✅ |
| TSS | ✅ |

| **Memory** | Status |
|---|---|
| Physical memory manager (frame allocator) | ✅ |
| Virtual memory manager (paging, identity map) | ✅ |
| Kernel heap | ✅ |
| Per-process user page directory | ❌ |
| User heap (SYS_SBRK page allocation) | ❌ |

| **Processes** | Status |
|---|---|
| Process control block (PCB) | ✅ |
| Process lifecycle (create, ready, block, zombie) | ✅ |
| Kernel stack per process | ✅ |
| Context switching | ✅ |
| Round-robin priority scheduler | ✅ |
| fork, exec, wait, exit | ✅ |
| ELF binary loader | ✅ |
| File descriptor table | ✅ |
| Ring-3 user-mode execution | ❌ |
| User-mode stack setup (argc/argv) | ❌ |

| **System Calls** | Status |
|---|---|
| int 0x80 dispatch | ✅ |
| 22 syscalls (I/O, process, filesystem, namespace, PULSAR) | ✅ |
| Signals (SIGKILL, SIGPIPE, SIGCHLD) | ❌ |

| **Virtual Filesystem** | Status |
|---|---|
| VFS core (vnode, vfs_ops vtable, mount table) | ✅ |
| RamFS (in-memory filesystem) | ✅ |
| DevFS (/dev/stdin, stdout, stderr, null, zero, random) | ✅ |
| Pipes (anonymous IPC) | ✅ |
| /proc synthetic filesystem | ❌ |

| **Namespaces** | Status |
|---|---|
| Per-process bind table (Plan 9 semantics) | ✅ |
| bind / unbind with union mount modes (before, replace, after) | ✅ |
| Copy-on-bind isolation on fork | ✅ |
| Path resolution through namespace with VFS fallthrough | ✅ |
| rfork with namespace inheritance flags | ❌ |

| **PULSAR (Distributed Filesystem Protocol)** | Status |
|---|---|
| Wire-compatible with 9P2000 | ✅ |
| Full message encode/decode (EMIT/ECHO pairs) | ✅ |
| Session lifecycle (HAIL, DOCK, DESTROY) | ✅ |
| Protocol operations (TRAVERSE, OPEN, READ, WRITE, SCAN, RELEASE) | ✅ |
| VFS integration (pulsar_ops vtable) | ✅ |
| Mount via SYS_MOUNT into process namespace | ✅ |
| PULSAR over TCP | ❌ |

| **Networking** | Status |
|---|---|
| virtio-net NIC driver | ❌ |
| lwIP integration | ❌ |
| /net filesystem (connections as files) | ❌ |

| **User Runtime (liborion)** | Status |
|---|---|
| _start ELF entry point | ❌ |
| Syscall wrappers | ❌ |
| malloc / free | ❌ |
| printf / sprintf | ❌ |

| **Shell** | Status |
|---|---|
| Interactive kernel-mode shell | ✅ |
| Built-ins: echo, pwd, cd, ls, cat, mkdir, ps, clear | ✅ |
| Namespace built-ins: bind, unbind, nsdump | ✅ |
| Shell as user-mode process | ❌ |

| **Applications** | Status |
|---|---|
| orion-top (process monitor) | ❌ |
| Blackjack | ❌ |
| IRC client | ❌ |

</details>

## Dev Commands
### Makefile Commands:
```
make clean   // clean project
make         // build project
make run     // run project
```
