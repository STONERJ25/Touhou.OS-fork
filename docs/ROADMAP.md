# Roadmap

Release names follow the Touhou game order (see THEMING.md). Checked items are in the tree and tested; the unchecked ones
are what is left, roughly in the order they matter. (The areas that were once parked on `parked/<area>` branches are all merged
since 2026-11-03; see the project notes.)

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
- [x] Gensokyo Explorer (file manager; see `docs/APPS.md` and the notes in `docs/EXPLORER.md`)
- [x] image viewer (the Gensokyo Gallery, docs/MEDIA.md)
- [x] music player: Reimu's Jukebox, original songs and a `.mod` player (docs/AUDIO.md)

## 0.4+ -- system depth
- [x] ACPI poweroff/reset/power button, RTC and time zones, APIC virtual wire
- [x] accounts, passwords, sudo, seats, sticky directories
- [x] real-hardware preparation: boot-from-stick image, read-only-disks switch, USB boot path tested in QEMU
- [x] USB mass storage, so `/HumanVillage` lives on the stick the OS booted from, plus `TOUHOU.LOG` on it for debugging a PC without serial (docs/USB.md, docs/REAL_HARDWARE.md)
- [x] real-hardware audit, rounds 1 and 2 (framebuffer write-combining and the MTRR fallback, xHCI/AHCI/NVMe/PCI/ACPI against the specs, the loader's memory types); the OS booted to the desktop on the target Dell on 2026-10-23 (docs/HARDWARE_AUDIT.md)
- [ ] real hardware, rest: a network link and the internal disks on the Dell (SATA is in RAID mode there), USB stick quirks (the soft fallbacks are written and tested in QEMU, merged on 2026-11-09, [USB](USB.md) "Sticks that misbehave"; no misbehaving real stick has been tried)
- [x] networking: NICs (virtio, e1000/e1000e, rtl8139 tested in QEMU; I217-I219 and rtl8168 written but never run on hardware), ARP/IP/UDP/TCP, DHCP/DNS, tools (docs/NETWORK.md)
- [x] IPv6 beside IPv4 (2026-11-17, checked in QEMU only: link-local and SLAAC addresses with duplicate address detection, router advertisements with RDNSS, neighbour discovery, fragments and path MTU, ICMPv6, dual-stack AF_INET6 sockets, `getaddrinfo` with AAAA, `ping6`, `static6`; docs/NETWORK.md "IPv6")
- [ ] networking, next: a link on the real PC (the I219 was found on the Dell, no cable was plugged in; RTL8168 never run; IPv6 never run on a real card or LAN: the e1000 now sets multicast promiscuous); MLD and DHCPv6, IPv6 for libhttp (https), ssh and the update tools, SACK, a Kappa network panel
- [x] audio core: Intel HDA / AC97, mixer, `play`, `beep`, `volume`
- [x] audio rest: generic HDA codec path walk, Jukebox, songs, `.mod` player, system sounds, panel volume (docs/AUDIO.md; the codec walk is verified in QEMU only)
- [x] sound file formats: a FLAC decoder (RFC 9639), WAV in all its kinds, RF64, AIFF, AIFF-C, Sun .au; tags, seeking, gapless albums in the Jukebox (docs/AUDIO.md "Sound file formats"; FLAC verified against Microsoft's decoder and reference libFLAC)
- [ ] audio formats, next: MP3 (checkable on a Windows PC against its MP3 encoder and decoder, by RMS error), Ogg Vorbis and Opus (nothing independent to check against yet), Ogg FLAC, S3M/XM/IT modules, recording
- [x] threads (clone, futex, pthreads)
- [x] gzip/tar/zip
- [x] PNG/JPEG/GIF/BMP/PNM/TGA decoders, PNG/BMP/PPM encoders, the Gallery viewer, wallpaper from a picture, PNG screenshots, thumbnails for file browsers (docs/MEDIA.md)
- [x] awk, diff, patch, bc, dc, a first vi
- [x] kernel job control (stopped state, Ctrl-Z), nice/renice, hard links on tmpfs, `/proc/<pid>/fd`
- [x] vi hardening (about 950 scripted checks), gapsh job control with `jobs`/`fg`/`bg` and Ctrl-Z
- [ ] the remaining small tools (not started)
- [x] C compiler `mcc` that runs on the OS and compiles itself (merged on `agent/selfhost`; see [SELFHOST.md](SELFHOST.md))
- [x] read-only ext2/ext3/ext4 (`mount -t ext4 -o ro`), so a Linux disk or stick can be read and the Linux root can live on a disk instead of in RAM (docs/EXT4.md; checked in QEMU on generated images and under AddressSanitizer on the host, not yet on a real Linux disk)
- [ ] ext4, next: a real Linux stick on the Dell, a hash-tree lookup for huge directories, journal replay in memory (read only), extended attributes and ACLs; writing stays out of scope
- [x] consistency check of a dirty FAT volume at mount, an `fsck` tool (not yet run on real hardware) and a power-cut-safe FAT write order (docs/STORAGE.md)
- [x] kernel hardening: syscall fuzzer fixes and larger `exec` limits (docs/HARDENING.md)
- [x] kernel hardening, second round: SMAP, resource limits (`ulimit`), a reserve of pages and an OOM killer, a soft-lockup detector (docs/HARDENING.md; QEMU only, not yet on the Dell)
- [ ] kernel hardening, next: a hard-lockup detector (an NMI from a performance counter), failure injection for allocations (the `ENOMEM` paths), limits on threads per user and on the memory pipes and sockets pin, the panic-screen cases that are left
- [ ] install to an internal disk, so no stick is needed; updates over the network with a fallback kernel
- [ ] package format and `gapget`, text editor syntax colours for more languages

## Long term
- [ ] SMP (the other cores are idle), IOAPIC/MSI instead of polling
- [ ] Japanese text (a double-width bitmap font for kana and common kanji)
- [ ] self-hosting toolchain: userland rebuilt by `make` + `mcc` + `mas` inside the OS is done; the kernel (needs named sections, a no-SSE mode and a kernel layout in `mld`) and the libraries other than libgap are not ([SELFHOST.md](SELFHOST.md))
- [ ] PC-98 emulator for the user's own game copies (see ASSETS.md)
