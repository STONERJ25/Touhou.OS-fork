# Status

Fork of TouhouOS without the learning-first constraints. Goal: take the OS as far as one autonomous run allows.

## Build, run, test

```powershell
python tools/build.py                      # loader + kernel + userland + initrd + build/esp
.\tools\run-qemu.ps1                       # interactive window, serial on this terminal
python tools/qemu.py boot --until "Welcome to Gensokyo" --shot build/shot.png    # headless boot + screenshot
python tools/qemu.py boot --keys "ls\n" --after "bin" ...                       # type keys (QMP) and wait on serial
```

Needs (all installed on this machine): LLVM/Clang 22 (clang, ld.lld, lld-link, llvm-objcopy, llvm-ar), NASM, QEMU 11
(bundles OVMF), Python 3.13 with Pillow/numpy. `tools/dev-shell.ps1` adds them to PATH for manual use.

## Milestones

- [x] M0  UEFI loader -> ELF higher-half kernel, serial + framebuffer console (UTF-8, ANSI colours), memory map
- [ ] M1  GDT/TSS, IDT + exceptions with symbolised backtraces, PMM (buddy), VMM, heap
- [ ] M2  PIC/PIT timer, keyboard IRQ, scheduler (kernel threads), sync primitives
- [ ] M3  VFS, initrd (tar), tmpfs, devfs, procfs
- [ ] M4  syscalls, ELF loader, ring 3 processes, fork/exec/wait, pipes, libc, gapsh in userland
- [ ] M5  PCI, virtio-blk, FAT32 persistent disk
- [ ] M6  mouse, compositor + window manager, desktop (HakureiDE)
- [ ] M7  applications (terminal, explorer, editor, spell manager, danmaku game, ...)

## Known issues / next

See the newest entries in `docs/AI_SUMMARY.md` (local agent log).
