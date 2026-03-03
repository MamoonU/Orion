# Orion
A minimal distributed OS based on Plan-9 philosophies (32-bit x86 architecture written in C)

<img src="https://github.com/MamoonU/Orion/raw/main/OrionInverted.png" alt="alt text" title="Logo" style="max-width: 100%;" data-canonical-src="https://github.com/MamoonU/Orion/raw/main/OrionInverted.png" width="500" height="500">

## Dev Commands

### Makefile Commands =
```
make clean   // clean project
make         // clean and build project
make run     // run project
```

### run cdrom image =
```
qemu-system-x86_64 -cdrom myos.iso -serial stdio -no-reboot
```
