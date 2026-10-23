# Status

Fork of TouhouOS without the learning-first constraints: the goal was to take the OS as far as one autonomous run
allows. Everything below is in the tree and covered by `python tests/run_all.py` unless it says otherwise.

## Build, run, test

```powershell
python tools/build.py                      # loader + kernel + userland + initrd + build/esp
.\tools\run-qemu.ps1                       # interactive window, serial on this terminal
python tools/mkimage.py                    # bootable GPT disk image for a USB stick: build/touhou-os.img
python tests/run_all.py [name]             # the scripted QEMU tests (optionally only those matching name)
python tools/qemu.py boot --until "Welcome to Gensokyo" --shot build/shot.png    # headless boot + screenshot
```

Needs (all installed on this machine): LLVM/Clang 22 (clang, ld.lld, lld-link, llvm-objcopy, llvm-ar), NASM, QEMU 11
(bundles OVMF), Python 3.13 with Pillow/numpy. `tools/dev-shell.ps1` adds them to PATH for manual use.
Running on a real PC: [REAL_HARDWARE.md](REAL_HARDWARE.md).

## What exists

| Area | State | Docs |
|------|-------|------|
| Boot | UEFI loader (PE/COFF) -> higher-half ELF kernel; GOP modes `video=WxH|native|max`, PixelBitMask handled; ACPI RSDP passed on | [ARCHITECTURE](ARCHITECTURE.md) |
| Kernel core | GDT/IDT, symbolised panics, buddy PMM (DMA below 4 GiB), slab heap, PIC + PIT (APIC virtual wire), preemptive scheduler, wait queues, syscalls (SYSCALL/SYSRET), signals, fork/exec/wait, pipes, ptys, UNIX sockets | [ARCHITECTURE](ARCHITECTURE.md) |
| Filesystems | VFS with tmpfs root, devfs, procfs, initrd (tar); **FAT32/FAT16** read-write with long names and a write-back cache; the volume labelled HUMANVILLAGE is the only one ever mounted by itself | [STORAGE](STORAGE.md) |
| Drivers | PCI, virtio-blk, AHCI (SATA), NVMe, xHCI USB with hubs and HID keyboards/mice, PS/2 keyboard + mouse, serial, framebuffer; all polled | [BLOCKDEVICES](BLOCKDEVICES.md), [USB](USB.md) |
| Power and time | ACPI soft-off, reset register, polled power button; POSIX time zones, settable clock, `hwclock`, `rtc=local` | [POWER_AND_TIME](POWER_AND_TIME.md) |
| Accounts | users and groups, setuid exec, salted password hashes, `login`, `su`, `sudo` (`eiki`), `passwd`, `adduser`/`deluser`, sticky `/tmp`, a seat group for the desktop and the power button | [ACCOUNTS](ACCOUNTS.md) |
| Userland | libgap (C library with regex, getopt, sockets, ptys, crypto, time zones), gapsh (POSIX-style shell), kappabox (~120 applets), pnote editor, pager, manual pages for everything (`man`), omikuji and friends | [TOOLS](TOOLS.md) |
| Applications | Ran's Calculator, Sakuya's Pocket Watch, Spell Manager (task manager), Kappa Control Panel, Bunbunmaru News, Patchouli's Notebook (text editor), Sketchbook (paint), a danmaku shooter, Minesweeper (fairies under the grass), Snake; the Gensokyo Explorer (file manager) is still on a branch | [APPS](APPS.md), [GAMES](GAMES.md) |
| Window system | Fairies server (compositor, decorations, damage tracking, clipboard), Kappa toolkit (immediate-mode widgets, dialogs, icons), HakureiDE desktop (wallpaper, panel, start menu, task list, terminal), login screen, screen lock | [DESKTOP](DESKTOP.md) |

## Known issues / next

- Only one CPU is used (the other cores are idle); everything is polled.
- Nothing is persistent unless a HUMANVILLAGE volume is mounted: `/etc` and the homes live in RAM and are rebuilt at boot.
- No network, audio, threads or USB mass storage yet, and the Explorer app is unmerged (see ROADMAP and NEXT).
- The local agent log (`docs/AI_SUMMARY.md`, gitignored) has the newest development notes.
