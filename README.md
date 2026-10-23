# TouhouOS (build-first fork)

TouhouOS is a hobby x86-64 operating system inspired by Touhou Project. This fork of the learning-first project drops
the teaching constraints and builds the system as far as possible: a UEFI loader, a higher-half ELF kernel written in C,
and (as milestones land) a scheduler, virtual memory, ring-3 processes, a VFS, drivers, a window system and applications.
Current progress is tracked in [docs/STATUS.md](docs/STATUS.md); the design is in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Build and run (Windows, PowerShell)

Requires LLVM/Clang, NASM, QEMU (with its bundled OVMF firmware) and Python 3:

```powershell
python tools/build.py          # or .\tools\build.ps1
.\tools\run-qemu.ps1           # QEMU window; kernel log on this terminal's serial output
```

`python tools/qemu.py boot --until "REGEX" --shot build/shot.png` boots headless and saves a screenshot, which is how the
tests drive the system (keyboard/mouse injection over QMP).

## Disclaimer

This is largely Vibe/AI coded, I do not claim any of this code is original to myself, nor do I intend this to be taken in anyway seriously. I mean this as a fun educational experience for myself and nothing else.

### Code and use

This project is experimental and includes AI-assisted code. It is provided for learning and demonstration, without any promise that it is correct, secure, or suitable for use on real hardware. Review and test the code yourself before using it.

There is currently no `LICENSE` file for this repository. Its public availability does not, by itself, grant general permission to reuse, modify, or redistribute the code. Any third-party code, tools, or assets remain subject to their own licenses and attribution requirements.

### Touhou Project

TouhouOS is an unofficial fan work based on **Touhou Project**. It is not affiliated with, approved by, or endorsed by ZUN or Team Shanghai Alice. Touhou Project and its associated names, characters, settings, and other original material belong to their respective rights holders. This repository does not grant rights to that material.

The project intends to use original or properly licensed assets rather than material extracted from official Touhou games. Fan creators should consult the [official Touhou Project fan-content guidelines](https://touhou-project.news/guidelines_en/) before distributing derivative work.
