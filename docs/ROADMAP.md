# Roadmap

Release names follow the Touhou game order (see THEMING.md). Checked items are in the tree and tested; the unchecked ones
are what is left, roughly in the order they matter.

## 0.1 Story of Eastern Wonderland -- the kernel boots, runs processes, has a shell
- [x] UEFI loader, higher-half ELF kernel, framebuffer console, serial
- [x] GDT/IDT/exceptions, symbolised panics, PMM, VMM, heap, PIT, preemptive scheduler
- [x] keyboard + mouse (PS/2 and USB HID), TTY with line discipline
- [x] VFS: tmpfs root, devfs, procfs, initrd; themed directory layout
- [x] syscalls, ring 3, ELF loader, fork/exec/wait, pipes, signals
- [x] libgap (libc), init, gapsh, coreutils (kappabox), manual pages
- [x] PCI, virtio-blk, AHCI, NVMe, FAT32/FAT16 (persistent `/HumanVillage`, one labelled volume, never anything else)

## 0.2 Phantasmagoria of Dim.Dream -- graphics
- [x] framebuffer device, window server + compositor (Fairies), mouse cursor
- [x] HakureiDE desktop: panel, launcher, windows, wallpaper, icons, login screen, screen lock
- [x] terminal emulator (pty)
- [ ] hi-dpi scaling for 4K screens (everything is drawn at 1x)

## 0.3 Lotus Land Story -- applications
- [x] terminal, text editor (pnote), manual viewer, pager
- [ ] Explorer, Spell Manager, Kappa Control Panel, Patchouli's Notebook, Ran's Calculator, Sakuya's Pocket Watch,
      Bunbunmaru News, danmaku shooter, small games, image viewer, music player (written by background agents; see
      `docs/NEXT.md` for the state of each branch)

## 0.4+ -- system depth
- [x] ACPI poweroff/reset/power button, RTC and time zones, APIC virtual wire
- [x] accounts, passwords, sudo, seats, sticky directories
- [x] real-hardware preparation: boot-from-stick image, read-only-disks switch, USB boot path tested in QEMU
- [ ] USB mass storage, so `/HumanVillage` can live on the stick the OS booted from (docs/NEXT.md has the full brief)
- [ ] networking: NICs (virtio, e1000/e1000e, rtl8139/8168), ARP/IP/UDP/TCP, DHCP/DNS, tools (in progress on a branch)
- [ ] audio: Intel HDA / AC97, mixer, jukebox (in progress on a branch)
- [ ] threads (clone, futex, pthreads) (in progress on a branch)
- [ ] gzip/tar/zip, PNG/JPEG/GIF decoders, image viewer (in progress on a branch)
- [ ] awk, diff, patch, vi, bc and a more POSIX gapsh (in progress on a branch)
- [ ] package format and `gapget`, text editor syntax colours for more languages

## Long term
- [ ] SMP (the other cores are idle), IOAPIC/MSI instead of polling
- [ ] Japanese text (a double-width bitmap font for kana and common kanji)
- [ ] self-hosting toolchain (a small C compiler that runs on the OS itself)
- [ ] PC-98 emulator for the user's own game copies (see ASSETS.md)
