# Architecture

## Boot flow

1. OVMF (UEFI) starts `\EFI\BOOT\BOOTX64.EFI` from the FAT volume (`build/esp`).
2. The loader (`boot/`, PE/COFF, MS ABI) reads `kernel.elf`, `initrd.img` and `cmdline.txt`, selects a GOP mode
   (default 1024x768, override with `video=WxH` on the command line), copies the ELF's PT_LOAD segments into one
   physically contiguous block, exits boot services, converts the UEFI memory map to `BootMemRegion[]`, builds boot page
   tables and jumps to the kernel with a `BootInfo*` (see `shared/bootinfo.h`).
3. The kernel (`kernel/`, ELF64, SysV ABI, `-mcmodel=kernel`, linked at `0xFFFFFFFF80000000`) takes over.

## Virtual address space

| Range | Use |
|-------|-----|
| `0x0000000000000000 - 0x00007FFFFFFFFFFF` | user space |
| `0xFFFF800000000000` + phys | direct map (HHDM) of RAM |
| `0xFFFFFFFF80000000` and up | kernel image |

## Source tree

| Dir | Contents |
|-----|----------|
| `boot/` | UEFI loader |
| `shared/` | headers/sources used by loader, kernel and userland (`bootinfo.h`, `elf.h`, 8x16 font) |
| `kernel/` | the kernel; headers are all in `kernel/include/` |
| `user/` | libc (`libgap`), `crt0`, programs (`user/bin/*`) |
| `initrd/` | files copied into the initrd verbatim |
| `tools/` | build driver, QEMU harness, generators |
| `tests/` | scripted QEMU tests |

## Conventions

- Canonical pixel format is `0x00RRGGBB`; the framebuffer layer swaps channels when the hardware is RGBX.
- Log lines use `klog("Subsystem", ...)`; each subsystem tag gets a stable colour on screen.
- Kernel code is compiled with `-mgeneral-regs-only` (no SSE/x87 in the kernel).
