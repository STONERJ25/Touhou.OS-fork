# Roadmap

Release names follow the Touhou game order (see THEMING.md). Checked items are in the tree and tested.

## 0.1 Story of Eastern Wonderland -- the kernel boots, runs processes, has a shell
- [x] UEFI loader, higher-half ELF kernel, framebuffer console, serial
- [x] GDT/IDT/exceptions, symbolised panics, PMM, VMM, heap, PIT, preemptive scheduler
- [ ] keyboard + mouse, TTY with line discipline
- [ ] VFS: tmpfs root, devfs, procfs, initrd; themed directory layout
- [ ] syscalls, ring 3, ELF loader, fork/exec/wait, pipes, signals
- [ ] libgap (libc), init, gapsh, coreutils
- [ ] PCI, virtio-blk, FAT32 (persistent `/HumanVillage`)

## 0.2 Phantasmagoria of Dim.Dream -- graphics
- [ ] framebuffer device, double buffering, window server + compositor (Fairies), mouse cursor
- [ ] HakureiDE desktop: panel, launcher, windows, wallpaper, icons
- [ ] terminal emulator (pty), Gensokyo Explorer

## 0.3 Lotus Land Story -- applications
- [ ] Spell Manager, Kappa Control Panel, Patchouli's Notebook, Ran's Calculator, Sakuya's Pocket Watch
- [ ] Bunbunmaru News, danmaku shooter, small games, image viewer, music/tracker player

## 0.4+ -- system depth
- [ ] ACPI (poweroff/reboot), RTC, APIC/IOAPIC, hardware RNG
- [ ] networking: e1000, ARP/IP/UDP/TCP, DHCP/DNS, simple HTTP client (Yukari)
- [ ] audio: AC97 / Intel HDA, mixer, tracker playback
- [ ] package format and `gapget`, shell scripting, text editor with syntax colours

## Long term
- [ ] PC-98 emulator for the user's own game copies (see ASSETS.md)
- [ ] SMP, USB HID, NVMe/AHCI, self-hosting toolchain
