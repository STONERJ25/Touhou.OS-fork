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
- [x] Ran's Calculator, Sakuya's Pocket Watch, Spell Manager, Kappa Control Panel, Bunbunmaru News
- [x] Patchouli's Notebook, Sketchbook, danmaku shooter, Minesweeper, Snake
- [ ] Gensokyo Explorer (file manager; in progress, design in `docs/EXPLORER.md`)
- [ ] image viewer, music player (in progress)

## 0.4+ -- system depth
- [x] ACPI poweroff/reset/power button, RTC and time zones, APIC virtual wire
- [x] accounts, passwords, sudo, seats, sticky directories
- [x] real-hardware preparation: boot-from-stick image, read-only-disks switch, USB boot path tested in QEMU
- [ ] USB mass storage, so `/HumanVillage` can live on the stick the OS booted from, plus a boot log file for debugging a PC without serial (in progress; docs/NEXT.md has the brief)
- [ ] real-hardware audit: framebuffer write-combining, xHCI/AHCI/NVMe/PCI/ACPI against the specs and the target Dell (in progress)
- [x] networking core: NICs (virtio, e1000/e1000e), ARP/IP/ICMP/UDP/TCP, sockets, DHCP
- [ ] networking rest: DNS, tools (ping, ifconfig, fetch, nc ...), Realtek cards (in progress)
- [x] audio core: Intel HDA / AC97, mixer, `play`, `beep`, `volume`
- [ ] audio rest: chiptune engine, Jukebox, songs, system sounds wired in (in progress)
- [x] threads (clone, futex, pthreads)
- [x] gzip/tar/zip
- [ ] PNG/JPEG/GIF decoders, image viewer, wallpaper, PNG screenshots (in progress)
- [x] awk, diff, patch, bc, dc, a first vi
- [x] kernel job control (stopped state, Ctrl-Z), nice/renice, hard links on tmpfs, `/proc/<pid>/fd`
- [ ] vi hardening, a more POSIX gapsh with `jobs`/`fg`/`bg`, the remaining small tools (in progress)
- [ ] C compiler `mcc` that runs on the OS and compiles itself (in progress)
- [ ] package format and `gapget`, text editor syntax colours for more languages

## Long term
- [ ] SMP (the other cores are idle), IOAPIC/MSI instead of polling
- [ ] Japanese text (a double-width bitmap font for kana and common kanji)
- [ ] self-hosting toolchain (a small C compiler that runs on the OS itself)
- [ ] PC-98 emulator for the user's own game copies (see ASSETS.md)
