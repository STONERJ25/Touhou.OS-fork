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
$env:TOUHOU_CPU = "Skylake-Client-noTSX-IBRS"   # optional: run QEMU as a Skylake/Kaby Lake CPU instead of `-cpu max` (the real target is an i5-7500)
```

Needs (all installed on this machine): LLVM/Clang 22 (clang, ld.lld, lld-link, llvm-objcopy, llvm-ar), NASM, QEMU 11
(bundles OVMF), Python 3.13 with Pillow/numpy. `tools/dev-shell.ps1` adds them to PATH for manual use.
Running on a real PC: [REAL_HARDWARE.md](REAL_HARDWARE.md); what was reviewed for it, what was fixed and what can only be checked on the
machine: [HARDWARE_AUDIT.md](HARDWARE_AUDIT.md).

## What exists

| Area | State | Docs |
|------|-------|------|
| Boot | UEFI loader (PE/COFF) -> higher-half ELF kernel; GOP modes `video=WxH|native|max`, PixelBitMask handled; ACPI RSDP passed on | [ARCHITECTURE](ARCHITECTURE.md) |
| Kernel core | GDT/IDT, symbolised panics, buddy PMM (DMA below 4 GiB), slab heap, PIC + PIT (APIC virtual wire), preemptive scheduler, wait queues, syscalls (SYSCALL/SYSRET), signals, fork/exec/wait, pipes, ptys, UNIX sockets | [ARCHITECTURE](ARCHITECTURE.md) |
| Filesystems | VFS with tmpfs root, devfs, procfs, initrd (tar); **FAT32/FAT16** read-write with long names and a write-back cache; the volume labelled HUMANVILLAGE (on an internal disk or on a USB stick, the first one found) is the only one ever mounted by itself; FAT times follow `tz=`; the kernel log is kept on that volume | [STORAGE](STORAGE.md) |
| Drivers | PCI, virtio-blk, AHCI (SATA), NVMe, xHCI USB with hubs, HID keyboards/mice and **mass storage (sticks as sda/sdb..., hot-plug)**, PS/2 keyboard + mouse, serial, framebuffer, network cards (virtio-net, e1000, e1000e and the PCH I217-I219, RTL8139, RTL8168), sound (Intel HDA, AC'97); all polled | [BLOCKDEVICES](BLOCKDEVICES.md), [USB](USB.md), [NETWORK](NETWORK.md), [AUDIO](AUDIO.md) |
| Network | TCP/IP in the kernel (loopback, ARP, IPv4 with fragments, ICMP, UDP, a full TCP), AF_INET sockets, DHCP client, DNS resolver, tools (`ifconfig ip route ping netstat ss nslookup host fetch wget nc sntp ntpdate httpd`); **HTTPS** in `fetch`/`wget` through `libtls` (TLS 1.3 and 1.2 client, X.509 validation, 119 Mozilla roots, ChaCha20 kernel random generator; no side-channel guarantees, no revocation); drivers for virtio-net, e1000, e1000e, RTL8139 (tested in QEMU) and the PCH I217/I218/I219 and RTL8168/8111 parts of real PCs (**never run on hardware**); the Kappa network page and `/etc/network.conf` are designed but not written | [NETWORK](NETWORK.md), [TLS](TLS.md) |
| Audio | `/dev/audio` with a software mixer, HDA and AC'97 drivers, libaudio (WAV, tones, system sounds), `beep`, `play`, `volume`; the chiptune engine, Jukebox and songs are still being written | [AUDIO](AUDIO.md) |
| Threads | kernel threads (`thread_create`, futex, per-thread signals), complete pthreads in libgap (mutexes, condition variables, rwlocks, barriers, semaphores, TLS, cancellation), C11 `<threads.h>`; priorities (`nice`, `renice`), hard links on tmpfs, `/proc/<pid>/fd`, the stopped process state | [THREADS](THREADS.md), [JOBCONTROL](JOBCONTROL.md) |
| Archives | `libz` (inflate/deflate), `gzip`/`gunzip`/`zcat`, `tar`, `zip`/`unzip`, `utimes()`; image decoders are still being written | [MEDIA](MEDIA.md) |
| Power and time | ACPI soft-off, reset register, polled power button; POSIX time zones, settable clock, `hwclock`, `rtc=local` | [POWER_AND_TIME](POWER_AND_TIME.md) |
| Accounts | users and groups, setuid exec, salted password hashes, `login`, `su`, `sudo` (`eiki`), `passwd`, `adduser`/`deluser`, sticky `/tmp`, a seat group for the desktop and the power button | [ACCOUNTS](ACCOUNTS.md) |
| Userland | libgap (C library with regex, getopt, sockets, ptys, crypto, time zones, pthreads), gapsh (POSIX-style shell), kappabox (~130 applets including awk, diff, patch, bc, dc), `vi`/`vim`/`view` (a first version: only the basics have been run), pnote editor, pager, manual pages for everything (`man`), omikuji and friends | [TOOLS](TOOLS.md), [UTILITIES](UTILITIES.md) |
| Applications | Ran's Calculator, Sakuya's Pocket Watch, Spell Manager (task manager), Kappa Control Panel, Bunbunmaru News, Patchouli's Notebook (text editor), Sketchbook (paint), a danmaku shooter, Minesweeper (fairies under the grass), Snake, the Gensokyo Explorer (file manager) | [APPS](APPS.md), [GAMES](GAMES.md) |
| Window system | Fairies server (compositor, decorations, damage tracking, clipboard), Kappa toolkit (immediate-mode widgets, dialogs, icons), HakureiDE desktop (wallpaper, panel, start menu, task list, terminal), login screen, screen lock | [DESKTOP](DESKTOP.md) |
| Media | libz (inflate/deflate, zlib/gzip), libimg (PNG, JPEG baseline + progressive, GIF + animation, BMP, PNM, TGA readers; PNG/BMP/PPM writers; scaling, rotation), `gzip` `gunzip` `zcat` `tar` `zip` `unzip`, `imginfo` `convert`, the Gensokyo Gallery viewer, a wallpaper picture (`wallpaper`, the Gallery's W key, the Control Panel), PNG screenshots in `~/Pictures` (`screenshot`, Ctrl+F12), thumbnails for file browsers (`k_image_thumbnail`, not used by the Explorer yet; the Explorer opens pictures in the Gallery), `SYS_utimes` (also on FAT) | [MEDIA](MEDIA.md) |

## Known issues / next

- Only one CPU is used (the other cores are idle); everything is polled.
- Nothing is persistent unless a HUMANVILLAGE volume is mounted: `/etc` and the homes live in RAM and are rebuilt at boot.
- USB mass storage is in (bulk-only sticks, [USB](USB.md)): the stick the OS booted from carries `/HumanVillage`, and the kernel log is kept on it
  (`TOUHOU.LOG`, [REAL_HARDWARE](REAL_HARDWARE.md)). A stick flagged dirty is gone through before it is mounted (read-write when nothing is out of place; read-only with
  the reason in `/proc/fatcheck` otherwise; `hvforce` / `hvcheck=off`, [STORAGE](STORAGE.md)). Not covered: UAS; `fsck` (written, host-tested, **never run in the guest yet**: see
  "State when paused" in STORAGE.md); a safer FAT write order; the dirty-volume check has not run on real silicon.
- The target PC (a Dell OptiPlex, Core i5-7500) booted this OS to the desktop on 2026-10-23: the screen (write-combining, about 9 GB/s), USB keyboard and mouse behind a hub, the USB stick as `/HumanVillage`, `TOUHOU.LOG` and a clean shutdown all work; its first log and what it showed are in [HARDWARE_AUDIT](HARDWARE_AUDIT.md). Not yet seen on it: a network link (the I219 reported no carrier), sound, the internal disks (SATA is in RAID mode there). A slow screen would correct itself (a write-combining MTRR fallback), and the audit has the first-boot checklist.
- Job control: the kernel side exists (stopped state, Ctrl-Z, `waitpid` with WUNTRACED, orphaned groups; [JOBCONTROL](JOBCONTROL.md)), but gapsh does not use it yet (`jobs`, `fg`, `bg` are being written), so for now Ctrl-Z freezes the foreground job and the shell prompt along with it.
- The local agent log (`docs/AI_SUMMARY.md`, gitignored) has the newest development notes.
