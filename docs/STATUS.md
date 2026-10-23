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
Running on a real PC: [REAL_HARDWARE.md](REAL_HARDWARE.md).

## What exists

| Area | State | Docs |
|------|-------|------|
| Boot | UEFI loader (PE/COFF) -> higher-half ELF kernel; GOP modes `video=WxH|native|max`, PixelBitMask handled; ACPI RSDP passed on | [ARCHITECTURE](ARCHITECTURE.md) |
| Kernel core | GDT/IDT, symbolised panics, buddy PMM (DMA below 4 GiB), slab heap, PIC + PIT (APIC virtual wire), preemptive scheduler, wait queues, syscalls (SYSCALL/SYSRET), signals, fork/exec/wait, pipes, ptys, UNIX sockets | [ARCHITECTURE](ARCHITECTURE.md) |
| Filesystems | VFS with tmpfs root, devfs, procfs, initrd (tar); **FAT32/FAT16** read-write with long names and a write-back cache; the volume labelled HUMANVILLAGE (on an internal disk or on a USB stick, the first one found) is the only one ever mounted by itself; FAT times follow `tz=`; the kernel log is kept on that volume | [STORAGE](STORAGE.md) |
| Drivers | PCI, virtio-blk, AHCI (SATA), NVMe, xHCI USB with hubs, HID keyboards/mice and **mass storage (sticks as sda/sdb..., hot-plug)**, PS/2 keyboard + mouse, serial, framebuffer, network cards (virtio-net, e1000, e1000e), sound (Intel HDA, AC'97); all polled | [BLOCKDEVICES](BLOCKDEVICES.md), [USB](USB.md), [NETWORK](NETWORK.md), [AUDIO](AUDIO.md) |
| Network | ARP, IPv4, ICMP, UDP, TCP (loopback and real NICs), BSD sockets, DHCP client, `/proc/net`; tools, DNS and the Realtek cards are still being written | [NETWORK](NETWORK.md) |
| Audio | `/dev/audio` with a software mixer, HDA and AC'97 drivers, libaudio (WAV, tones, system sounds), `beep`, `play`, `volume`; the chiptune engine, Jukebox and songs are still being written | [AUDIO](AUDIO.md) |
| Threads | kernel threads (`thread_create`, futex, per-thread signals), complete pthreads in libgap (mutexes, condition variables, rwlocks, barriers, semaphores, TLS, cancellation), C11 `<threads.h>`; priorities (`nice`, `renice`), hard links on tmpfs, `/proc/<pid>/fd`, the stopped process state | [THREADS](THREADS.md), [JOBCONTROL](JOBCONTROL.md) |
| Archives | `libz` (inflate/deflate), `gzip`/`gunzip`/`zcat`, `tar`, `zip`/`unzip`, `utimes()`; image decoders are still being written | [MEDIA](MEDIA.md) |
| Power and time | ACPI soft-off, reset register, polled power button; POSIX time zones, settable clock, `hwclock`, `rtc=local` | [POWER_AND_TIME](POWER_AND_TIME.md) |
| Accounts | users and groups, setuid exec, salted password hashes, `login`, `su`, `sudo` (`eiki`), `passwd`, `adduser`/`deluser`, sticky `/tmp`, a seat group for the desktop and the power button | [ACCOUNTS](ACCOUNTS.md) |
| Userland | libgap (C library with regex, getopt, sockets, ptys, crypto, time zones, pthreads), gapsh (POSIX-style shell), kappabox (~130 applets including awk, diff, patch, bc, dc), `vi`/`vim`/`view` (a first version: only the basics have been run), pnote editor, pager, manual pages for everything (`man`), omikuji and friends | [TOOLS](TOOLS.md), [UTILITIES](UTILITIES.md) |
| Applications | Ran's Calculator, Sakuya's Pocket Watch, Spell Manager (task manager), Kappa Control Panel, Bunbunmaru News, Patchouli's Notebook (text editor), Sketchbook (paint), a danmaku shooter, Minesweeper (fairies under the grass), Snake, the Gensokyo Explorer (file manager) | [APPS](APPS.md), [GAMES](GAMES.md) |
| Window system | Fairies server (compositor, decorations, damage tracking, clipboard), Kappa toolkit (immediate-mode widgets, dialogs, icons), HakureiDE desktop (wallpaper, panel, start menu, task list, terminal), login screen, screen lock | [DESKTOP](DESKTOP.md) |

## Known issues / next

- Only one CPU is used (the other cores are idle); everything is polled.
- Nothing is persistent unless a HUMANVILLAGE volume is mounted: `/etc` and the homes live in RAM and are rebuilt at boot.
- USB mass storage is in (bulk-only sticks, [USB](USB.md)): the stick the OS booted from carries `/HumanVillage`, and the kernel log is kept on it
  (`TOUHOU.LOG`, [REAL_HARDWARE](REAL_HARDWARE.md)). Not covered: UAS, a stick flagged dirty stays read-only until a file system check, and nothing of it
  has run on real silicon. No image decoders yet.
- Nothing has run on the real target PC; [HARDWARE_AUDIT](HARDWARE_AUDIT.md) (when it exists) lists what is unverified.
- Job control: the kernel side exists (stopped state, Ctrl-Z, `waitpid` with WUNTRACED, orphaned groups; [JOBCONTROL](JOBCONTROL.md)), but gapsh does not use it yet (`jobs`, `fg`, `bg` are being written), so for now Ctrl-Z freezes the foreground job and the shell prompt along with it.
- The local agent log (`docs/AI_SUMMARY.md`, gitignored) has the newest development notes.
