# Brief for a cloud agent working on TouhouOS

Read this whole page first. It is written for an agent that starts with only this repository and no other context. The owner's other
documents (`docs/STATUS.md`, `docs/NEXT.md`, `docs/ARCHITECTURE.md`, `docs/THEMING.md`, `docs/HARDWARE_AUDIT.md`) go deeper; this page says what to do and what to avoid.

## What this is

TouhouOS is a from-scratch x86-64 UEFI hobby operating system written in C (a Touhou Project fan project: everything is themed, see `docs/THEMING.md`):
a UEFI loader (`boot/`, PE/COFF) -> a higher-half ELF64 kernel (`kernel/`) -> static ELF userland (`user/`): its own C library `libgap`, a POSIX-style shell `gapsh`,
about 140 command-line tools (`kappabox`), a window server `Fairies`, a widget toolkit `Kappa`, the `HakureiDE` desktop and a dozen applications. Drivers are polled
(no interrupts for devices): xHCI USB (hubs, HID, mass storage), AHCI, NVMe, virtio, FAT32, Intel HDA, e1000/e1000e/I219, RTL8139/8168, a TCP/IP stack.

It boots in QEMU **and on the owner's real PC** (a Dell OptiPlex, Core i5-7500, 2560x1440 monitor, USB keyboard and mouse behind a hub, no serial port, SATA controller in Intel
RAID mode). The first real boot happened on 2026-10-23 (`docs/HARDWARE_AUDIT.md`, `docs/logs/`). Nobody can reproduce hardware-only problems for you: be conservative with anything that
touches real disks or firmware, and say plainly what you could not test.

Layout: `boot/` loader, `kernel/` kernel, `user/` libc, toolkit, desktop, apps, tools, `shared/` headers used by several parts, `initrd/` files that end up in the initrd (including the
manual pages), `tools/` host-side build, image and QEMU tooling (Python), `tests/` the QEMU test suite (`case_*.py`), `docs/` everything written down.

## Rules of the house

- **Verify in QEMU.** Build: `python tools/build.py`. Tests: `python tests/run_all.py PREFIX[,PREFIX...]` (a prefix with `~` in front matches names containing it). Look at screenshots
  (`tools/qemu.py boot ... --shot x.png`) for anything graphical.
- **Never run two test runs in one checkout** (they share `build/esp/cmdline.txt`). Use one `git worktree` per parallel group. The whole suite (about 260 tests) takes about 25 minutes split into
  seven parallel groups: `usb_` / `storage` / `usbstor` / `blk,crash,selftest,utest,kshell,jobctl,hwaudit` / `notebook,sketchbook` /
  `explorer,media,apps,desktop,snake,danmaku,games,minesweeper,lock,autologin,power,accounts,login,time,acpi,shrine` /
  `net,audio,threads,diff,patch,bc,dc,awk,vi,shell,pnote,pager,manual,image`.
- **Tests never sleep for a fixed time**: the guest clock runs several times slower than wall time when the host is busy (and slower still under QEMU TCG without KVM): wait for serial
  output or pixels with generous timeouts and retry. Mouse-driven GUI tests (`notebook_*`, `sketchbook_*`, `audio_panel_volume`) flake under load: rerun a failure alone before suspecting the code.
- Tests that wait for a serial shell prompt need `text` in their kernel command line (the desktop is the default boot and prints no prompt).
- **Every new command-line applet needs a manual page** (`initrd/ScarletDevilMansion/share/patchouli/1/NAME.pw`, lines at most 78 columns) or the `manual` test fails.
- **Theming is a first-class requirement**: kernel log tags, messages, names follow `docs/THEMING.md`; the plain technical fact must stay readable next to the joke.
- Do not use the words "genuinely" or "genuine" in code, docs or commit messages (the owner dislikes the tic).
- **Safety rule of the project: the OS never writes to a disk it was not told to.** `blk.ro` (the default kernel command line word) makes SATA and NVMe drives read-only; the only volume that is
  ever mounted by itself is the FAT32 volume labelled `HUMANVILLAGE`. Do not weaken this. Anything that writes to a whole disk needs explicit confirmation by the user.
- Syscall numbers live in `shared/syscall.h` in documented ranges (network 90-99, audio 100-105, threads and job control 106-118; the next free number is 119).
- Prefer small, local changes in shared files (`kernel/main.c`, `tools/build.py`, `tools/qemu.py`, `shared/*.h`): several branches touch them.
- Write down what you did: the doc of the area (`docs/<AREA>.md`) with a "State when paused" / "How to resume" section, and a line in `docs/NEXT.md`.
- Git: work on a branch named `cloud/<task-slug>`, commit often, open a pull request against `main` when the acceptance criteria are met, and put a short report in it (what changed, tests run with
  results, what is left, what you could not verify). **Do not push to `main`, do not force-push, do not delete branches.** No secrets in the repository.

## Environment

The tooling was written on Windows and still has Windows paths in it (`tools/build.py`: `LLVM_DIR`, `NASM_DIR`; `tools/qemu.py`: `QEMU_DIR`, OVMF files): **task 1 fixes that**, and until it lands
you have to adapt locally. Needed on Linux: `clang`, `lld` (`lld-link`, `ld.lld`), `llvm` (`llvm-ar`, `llvm-objcopy`, `llvm-nm`), `nasm`, `qemu-system-x86` with the OVMF firmware (`ovmf` / edk2),
Python 3.11 or newer with `Pillow`, `numpy` and `cryptography` (some tests skip without the last). `TOUHOU_CPU=Skylake-Client-noTSX-IBRS` makes QEMU look like the target CPU.

## Where things stand

`main` is verified: all 261 tests pass (one mouse-driven GUI test flakes under load). It contains everything up to the first real boot of the Dell. Work that was finished by earlier agents but is **not merged**
is on branches named `parked/<area>`; each has its own `docs/<AREA>.md` with "State when paused" and "How to resume". `docs/NEXT.md` has the merge order and the tests to run after each.

| Branch | What it holds | Notes |
|--------|---------------|-------|
| `parked/usb-dirty-fsck` | in-kernel consistency check of a dirty FAT volume at mount, `fsck` tool and repair engine | the `fsck` program never ran in the guest; the FAT write-order work is not started |
| `parked/audio-jukebox` | generic HDA codec path walk, Jukebox app, original songs, `.mod` player, system sounds, panel volume | merge conflict only in `user/bin/hakurei-shell/launch.c`: keep both menu lines |
| `parked/network-https` | `libtls` (TLS 1.3/1.2 client, own crypto), RNG, CA bundle, HTTPS in `fetch`/`wget`, DHCP stop | network panel not started; tests need the Python package `cryptography` |
| `parked/utilities-vi` | `vi` hardening (about 950 scripted checks), differs from vim in places listed in `docs/VI.md` | no shared files touched |
| `parked/libc-accuracy` | exact `strtod`/`printf`, block-wise stdio, faster `malloc`, accurate libm, leftmost-longest regex | **conflicts with main in `user/libc/src/stdio.c`** (main has per-stream locks), recipe in `docs/UTILITIES.md` |
| `parked/kernel-hardening` | syscall fuzzer (found two panics an ordinary user could cause), `exec` limits raised, `kwarn`, `/proc/kstats`, safer backtraces | OOM killer, SMAP and lockup detector are NOT done (stubs only) |
| `parked/mcc-compiler` | `mcc`/`mld`/`mar`: a C compiler that runs on the OS and rebuilds itself | newest edits never ran in QEMU; an early commit carries a 400 KB stray binary: squash-merge |
| `parked/usb-stick-quirks-wip` | half-done USB stick quirk handling in `kernel/drivers/usb/storage.c` | does not compile yet; the missing list is in `docs/HARDWARE_AUDIT.md` |

## Tasks, in priority order

Task 1 blocks everything that needs QEMU. Tasks 3 and 9 need no QEMU and can start at once.

1. **Linux portability of the build and test tooling, plus CI.** Make `tools/build.py`, `tools/qemu.py`, `tools/mkimage.py` and the tests run on Ubuntu 24.04 without changing behaviour on Windows:
   environment variables (`TOUHOU_LLVM_DIR`, `TOUHOU_NASM`, `TOUHOU_QEMU`, `TOUHOU_OVMF_CODE`, `TOUHOU_OVMF_VARS`), then a lookup on `PATH`, then the old Windows defaults; no hard-coded `.exe`/`C:\`;
   a `TOUHOU_TIMEOUT_SCALE` factor for slow hosts (QEMU TCG without KVM). Accept when `python tools/build.py` succeeds and `selftest,utest,shell,desktop_boots,image_,hwaudit_stage_marks` pass on Linux, a
   GitHub Actions workflow builds and runs that smoke set, and `docs/LINUX.md` lists the packages and the commands. Do not weaken or skip tests to get there.
2. **Merge the parked branches** (needs task 1) into a staging branch `cloud/staging`, one at a time, in this order: `usb-dirty-fsck`, `audio-jukebox`, `network-https`, `utilities-vi`, `libc-accuracy`,
   `kernel-hardening`, `mcc-compiler`. After each: build and the tests named in `docs/NEXT.md`; at the end the whole suite in the seven groups above. Resolve conflicts by keeping both sides' intent
   (the notes in the table). Report every failure with its output and whether it passes alone. Open one pull request with the result.
3. **Update server and manifest tooling, host side only** (no QEMU needed). `tools/serve-update.py`: builds a manifest of the files an installed system needs (`kernel.elf`, `initrd.img`,
   `BOOTX64.EFI`, `cmdline.txt`: sizes, sha256, a version from `git describe`), signs it with ECDSA P-256 (the public key is meant to be baked into the OS image; the private key stays outside the repository),
   and serves manifest and files over HTTP on the LAN (`--bind`, `--port`); host-side unit tests under `tests/host/`; the format and the intended update protocol (download to `*.new`, verify, rename, keep
   `kernel.old` as a fallback) in `docs/UPDATE.md`. It is the server half of the owner's "update over the network instead of reflashing" goal (`docs/NEXT.md`, "Goals for the next session").
4. **The installer** (needs task 1): an `install` tool that puts the OS on a chosen disk so no stick is needed: GPT with a 64 MiB FAT32 ESP (`EFI/BOOT/BOOTX64.EFI`, `kernel.elf`, `initrd.img`, `cmdline.txt`) and a
   `HUMANVILLAGE` partition, reusing the layout code of `tools/mkdisk.py`. Safety is the point: refuse a disk with any partition unless `--force`, make the user type the device name, and add a root-only,
   per-disk write unlock to the block layer instead of dropping `blk.ro` globally. Tests in QEMU with virtual SATA, NVMe and virtio disks: install, boot the installed disk image (no stick) to the desktop,
   and the refusal paths. Write `docs/INSTALL.md`. Mention in it that the owner's Dell has SATA in Intel RAID mode (internal disks invisible until the BIOS is set to AHCI).
5. **The update client with an A/B fallback** (after tasks 2, 3 and 4): an `update URL` tool on the OS that fetches the manifest and files (HTTPS through `libtls` with a pinned certificate, or HTTP plus the signed
   manifest), verifies signature and hashes, mounts the ESP read-write only for the moment of the update, writes `*.new`, renames, keeps `kernel.old`; the loader falls back to `kernel.old` when the new kernel
   did not reach "boot complete" (count the starts with the NVRAM variable trick of `start_count()` in `boot/main.c`). Tests against the task 3 server through QEMU's user-mode network (10.0.2.2): good update,
   bad signature, bad hash, VM killed in the middle of the write (must still boot).
6. **gapsh job control** (needs task 1): `jobs`, `fg`, `bg`, `%n` job specs, `wait %n`, `disown`, Ctrl-Z. The kernel half exists; `docs/JOBCONTROL.md` lists exactly what the shell has to do. Until it lands, Ctrl-Z in a
   terminal freezes the foreground job and the shell prompt with it. Test through ptys like `tests/case_jobctl.py` does.
7. **FAT write ordering and a crash-consistency test** (needs task 2 for the `fsck` pieces): the four ordering gaps found in `docs/STORAGE.md` ("Safety design"), then kill the VM at random moments during a
   write-heavy workload (50 runs) and check the image with the host checker `tools/mkdisk.py --check`: nothing worse than lost clusters may appear.
8. **Hi-dpi scaling for the desktop** (needs task 1): the owner's monitor is 2560x1440 and the 8x16 bitmap font is tiny there. An integer scale (2x) for Fairies, Kappa and `hakurei-shell`, chosen automatically at
   large resolutions and by `ui.scale=N` on the command line: fonts, window decorations, icons, pointer coordinates; keep 1x pixel-identical. Pixel tests at both scales.
9. **Documentation** (no QEMU needed): a `docs/USER_GUIDE.md` for someone sitting in front of the desktop (apps, shortcuts, the terminal, files, shutting down), a refreshed `README.md`, and a pass over `docs/`
   for statements that are stale after the merges (`docs/STATUS.md`, `docs/ROADMAP.md`, `docs/NEXT.md`). Facts only: check them against the code.

Good parallel plan: task 1 and tasks 3 and 9 at once; when task 1 is merged, tasks 2, 4, 6, 8 in parallel (2 touches the most files, so merge it first), then 5 and 7.
