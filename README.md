# TouhouOS (build-first fork)

TouhouOS is a hobby x86-64 operating system inspired by Touhou Project, written from scratch in C. This fork of the learning-first
project drops the teaching constraints and builds the system as far as possible: a UEFI loader, a higher-half ELF kernel ("Gensokyo")
with a preemptive scheduler, virtual memory, ring-3 processes, threads and a VFS; drivers for PCI, SATA (AHCI), NVMe, virtio, xHCI USB
(hubs, keyboards, mice, sticks), FAT32, Intel HDA and AC'97 sound and several network cards with a TCP/IP stack; accounts and a login
screen; a window server (Fairies), a widget toolkit (Kappa), the HakureiDE desktop and a dozen applications; and its own C library
(`libgap`), shell (`gapsh`) and about 140 command-line tools (`kappabox`), each with a manual page.

Everything is themed (the trash is the Netherworld, processes are "spells", a page fault is a boundary violation), but the plain
technical fact always stays readable next to the joke: see [docs/THEMING.md](docs/THEMING.md).

It runs in QEMU, and since 2026-10-23 it also boots to the desktop on real hardware (a Dell OptiPlex with a Core i5-7500) from a USB stick.

## Using it

[docs/USER_GUIDE.md](docs/USER_GUIDE.md) is the guide for someone in front of the desktop: the applications, the keyboard shortcuts, the
terminal and the shell, where files live (and what survives a restart), accounts and logging in, shutting down, and the boot options.
On the system itself, `man gensokyo` is the map and every command has a manual page.

## Building

Needs LLVM/Clang (`clang`, `ld.lld`, `lld-link`, `llvm-objcopy`, `llvm-ar`), NASM, QEMU with the OVMF UEFI firmware, and Python 3
(3.11 or newer) with Pillow and numpy.

```powershell
python tools/build.py          # loader + kernel + userland + initrd -> build/esp   (or .\tools\build.ps1)
```

The tooling was written on Windows (PowerShell, LLVM, NASM and QEMU in their default install places). For building on Linux, see
[docs/LINUX.md](docs/LINUX.md).

## Running it in QEMU

```powershell
python tools/qemu.py run                         # a QEMU window; the kernel log goes to this terminal (or .\tools\run-qemu.ps1)
python tools/qemu.py run --cmdline "text"        # a text console instead of the desktop
python tools/qemu.py boot --until "REGEX" --shot build/shot.png    # headless: wait for a log line, save a screenshot
```

Without a disk everything, the home folders included, lives in memory and is gone when QEMU stops. For a home that persists, make a data
disk with a `HUMANVILLAGE` volume; `tools/qemu.py` attaches `build/disk.img` when it exists:

```powershell
python tools/mkdisk.py build/disk.img --size 300M --no-esp
```

`TOUHOU_CPU=Skylake-Client-noTSX-IBRS` makes QEMU present a CPU like the real target's instead of `-cpu max`.

## Running it on a real PC

Read [docs/REAL_HARDWARE.md](docs/REAL_HARDWARE.md) first. `python tools/mkimage.py --zip` builds `build/touhou-os.img`: a GPT disk image
with a small boot partition and a FAT32 partition labelled `HUMANVILLAGE` that holds the home folders, to be written to a USB stick.
Secure Boot has to be off. **The OS never writes to a disk it was not told to**: it mounts only the volume labelled `HUMANVILLAGE`, and the
stick's default command line (`video=native blk.ro`) makes every SATA and NVMe disk read-only. What was checked for the target machine,
and what its first boot showed, is in [docs/HARDWARE_AUDIT.md](docs/HARDWARE_AUDIT.md).

## Tests

```powershell
python tests/run_all.py                    # all of them (about 260 QEMU tests; a full run takes a while)
python tests/run_all.py desktop,explorer   # only the tests whose names start with these prefixes
```

The tests boot the OS in QEMU and drive it over the serial port and QMP (typing, mouse, screenshots). Never run two test runs in one checkout:
they share `build/esp/cmdline.txt`.

## Documentation

| Document | What is in it |
|----------|---------------|
| [docs/USER_GUIDE.md](docs/USER_GUIDE.md) | using the desktop, the shell and the files |
| [docs/STATUS.md](docs/STATUS.md) | what exists, area by area, and the known issues |
| [docs/ROADMAP.md](docs/ROADMAP.md) | what is done and what is left, by release |
| [docs/NEXT.md](docs/NEXT.md) | hand-over notes: unmerged branches, merge order, problems found along the way |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | how the system is put together |
| [docs/THEMING.md](docs/THEMING.md) | the naming and theming rules |
| [docs/REAL_HARDWARE.md](docs/REAL_HARDWARE.md), [docs/HARDWARE_AUDIT.md](docs/HARDWARE_AUDIT.md) | booting on a real PC, and what was reviewed for it |
| `docs/<AREA>.md` | one per area: [DESKTOP](docs/DESKTOP.md), [APPS](docs/APPS.md), [EXPLORER](docs/EXPLORER.md), [GAMES](docs/GAMES.md), [MEDIA](docs/MEDIA.md), [ACCOUNTS](docs/ACCOUNTS.md), [STORAGE](docs/STORAGE.md), [BLOCKDEVICES](docs/BLOCKDEVICES.md), [USB](docs/USB.md), [NETWORK](docs/NETWORK.md), [AUDIO](docs/AUDIO.md), [THREADS](docs/THREADS.md), [JOBCONTROL](docs/JOBCONTROL.md), [POWER_AND_TIME](docs/POWER_AND_TIME.md), [TOOLS](docs/TOOLS.md), [UTILITIES](docs/UTILITIES.md) |

Work that is finished but not merged yet (a music player, HTTPS, a check of dirty FAT volumes, `vi` hardening, libc accuracy, kernel
hardening, a C compiler that runs on the OS) waits on branches named `parked/<area>`; [docs/NEXT.md](docs/NEXT.md) has their state and the
merge order.

## Disclaimer

This is largely Vibe/AI coded, I do not claim any of this code is original to myself, nor do I intend this to be taken in anyway seriously. I mean this as a fun educational experience for myself and nothing else.

### Code and use

This project is experimental and includes AI-assisted code. It is provided for learning and demonstration, without any promise that it is correct, secure, or suitable for use on real hardware. Review and test the code yourself before using it.

There is currently no `LICENSE` file for this repository. Its public availability does not, by itself, grant general permission to reuse, modify, or redistribute the code. Any third-party code, tools, or assets remain subject to their own licenses and attribution requirements.

### Touhou Project

TouhouOS is an unofficial fan work based on **Touhou Project**. It is not affiliated with, approved by, or endorsed by ZUN or Team Shanghai Alice. Touhou Project and its associated names, characters, settings, and other original material belong to their respective rights holders. This repository does not grant rights to that material.

The project intends to use original or properly licensed assets rather than material extracted from official Touhou games. Fan creators should consult the [official Touhou Project fan-content guidelines](https://touhou-project.news/guidelines_en/) before distributing derivative work.
