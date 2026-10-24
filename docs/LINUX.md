# Building and testing on Linux

The build and the QEMU test suite were written on Windows; since the `cloud/linux-tooling` work they run on Linux as well (checked on Ubuntu 24.04)
with the same commands and the same results. Nothing changed for Windows: with no environment variable set, the tools are found where they were before.

## Packages (Ubuntu 24.04)

```sh
sudo apt-get install clang lld llvm nasm qemu-system-x86 ovmf python3 python3-pil python3-numpy python3-cryptography
```

| Package | What it gives the build |
|---------|-------------------------|
| `clang` | the C compiler for loader, kernel and userland (cross-targets `x86_64-unknown-windows` and `x86_64-unknown-none-elf`; no other cross toolchain needed) |
| `lld` | `lld-link` (the loader, PE/COFF) and `ld.lld` (kernel and programs, ELF64) |
| `llvm` | `llvm-ar`, `llvm-objcopy`, `llvm-nm` |
| `nasm` | the kernel's assembly files |
| `qemu-system-x86` | `qemu-system-x86_64` (version 8.2 on Ubuntu 24.04; the Windows machine runs a newer one) |
| `ovmf` | the UEFI firmware, `/usr/share/OVMF/OVMF_CODE_4M.fd` and `OVMF_VARS_4M.fd` |
| `python3-pil`, `python3-numpy`, `python3-cryptography` | screenshots and pixel checks, audio measurements, the TLS tests (those skip without `cryptography`) |

Python 3.11 or newer is needed (24.04 ships 3.12). `pip install pillow numpy cryptography` works as well as the Debian packages. The host-side fuzzers in `tests/host/`
(`imgtest.py`, `ztest.py`) additionally need `gcc` with AddressSanitizer (`build-essential`); they are not part of `tests/run_all.py`.

Other distributions: the same tools under their names there. The firmware is looked for in the Fedora (`/usr/share/edk2/ovmf/`) and Arch (`/usr/share/edk2/x64/`) places too.

## Commands

```sh
python3 tools/build.py                                   # everything into build/ (about 35 s on 4 cores)
python3 tests/run_all.py selftest,utest,shell,desktop_boots,image_,hwaudit_stage_marks    # the smoke set (about 75 s)
python3 tests/run_all.py                                 # the whole suite, one VM at a time
python3 tools/qemu.py run                                # a window plus the serial console on the terminal
python3 tools/qemu.py boot --until "boot complete" --shot build/x.png      # headless boot and a screenshot
python3 tools/mkimage.py --zip                           # the USB stick image, build/touhou-os.img(.zip)
```

The whole suite in parallel: one `git worktree` per group (two runs in one checkout fight over `build/esp/cmdline.txt`), each with its own build;
the seven groups are listed in [CLOUD_AGENT_BRIEF.md](CLOUD_AGENT_BRIEF.md).

## Environment variables

Each tool is found from, in order: its environment variable, the `PATH`, then the Windows install folders the project was written with.

| Variable | Meaning | Default when unset |
|----------|---------|--------------------|
| `TOUHOU_LLVM_DIR` | folder holding `clang`, `lld-link`, `ld.lld`, `llvm-ar`, `llvm-objcopy`, `llvm-nm` (e.g. `/usr/lib/llvm-18/bin`) | `PATH`, then `C:\Program Files\LLVM\bin` on Windows |
| `TOUHOU_NASM` | the `nasm` program, or the folder holding it | `PATH`, then `%LOCALAPPDATA%\bin\NASM` on Windows |
| `TOUHOU_QEMU` | the `qemu-system-x86_64` program | `PATH`, then `C:\Program Files\qemu\qemu-system-x86_64.exe` |
| `TOUHOU_OVMF_CODE` | the UEFI firmware code image (read-only pflash) | `share/edk2-x86_64-code.fd` next to QEMU, then the distribution's OVMF |
| `TOUHOU_OVMF_VARS` | the UEFI variable store template (copied to `build/<vm>-vars.fd`) | `share/edk2-i386-vars.fd` next to QEMU, then the distribution's OVMF |
| `TOUHOU_TIMEOUT_SCALE` | multiplies every wait of the test harness (`expect`, the GUI and shell helpers, the tests' own deadlines) | `1` |
| `TOUHOU_CPU` | QEMU `-cpu` model; `Skylake-Client-noTSX-IBRS` looks like the owner's i5-7500 | `max` |

The firmware code and variable images must belong together (same build, same size): set both variables or neither. Copies of the variable store in `build/*-vars.fd` survive between runs;
delete them after switching firmware.

`TOUHOU_TIMEOUT_SCALE` is for slow hosts: QEMU runs the guest with TCG (no KVM, no WHPX) everywhere, so a busy machine or a CI runner sees the guest clock crawl. `3` is what the GitHub
Actions workflow uses. It only stretches how long the harness waits; it changes nothing the tests check. Two kinds of checks are left unscaled on purpose because they measure speed rather than
wait for something: the "opening took too long" limits in `tests/case_notebook.py` and the dead-controller check in `tests/case_blockdev.py`.

## Continuous integration

`.github/workflows/smoke.yml` runs on pushes to `main` and `cloud/**`, on pull requests against `main`, and by hand (`workflow_dispatch`): it installs the packages above on
`ubuntu-24.04`, builds, and runs the smoke set with `TOUHOU_TIMEOUT_SCALE=3`. Serial and QEMU logs and screenshots of a failed run are kept for a week as the `smoke-logs` artifact.
The whole suite is not run there (about 25 minutes in seven parallel groups on a fast desktop).

## State when paused

Done and verified on Ubuntu 24.04 (4 cores, no KVM, QEMU 8.2.2, OVMF 2024.02): `python3 tools/build.py`, `python3 tools/mkimage.py --zip`, and the smoke set
`selftest,utest,shell,desktop_boots,image_,hwaudit_stage_marks` (8 tests) pass with scale 1 and with scale 3. The Windows paths are untouched except that the `PATH` is now looked at before the
install folders (the order the brief asked for); a different `clang` or `qemu-system-x86_64` earlier on a Windows `PATH` would now win. Not verified: the scripts on Windows itself (no Windows
machine here), and the whole suite on Linux.

## How to resume

- Run the whole suite on Linux in the seven groups and fix what differs from Windows (likely candidates: tests that depend on the QEMU version, since Ubuntu ships 8.2 and the Windows machine 11).
- If CI time matters: GitHub's Linux runners can use KVM (`/dev/kvm` after a udev rule); the harness would need an opt-in `-accel kvm`, which also changes guest timing, so keep TCG the default.
