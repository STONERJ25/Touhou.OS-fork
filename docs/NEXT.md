# What is next (hand-over notes)

Written on 2026-10-23 at about 20:50, when the user paused all work; all ten agents have reported. Read [STATUS.md](STATUS.md) for what exists and this file for what is
parked on branches and what to do first when work resumes.

## Where things stand

`main` (`b73b7d4`, the loader fix `5c0d74b` and docs commits since) holds everything that has been merged so far:

- first wave: kernel foundations, desktop, accounts and login, power and time, FAT32 and the HUMANVILLAGE mount policy, AHCI and NVMe,
  xHCI USB with HID, the desktop apps, the games, Patchouli's library, Notebook and Sketchbook;
- merged today: the network stack with its tools, DNS and the NIC drivers (I219 and RTL8168 never ran on hardware), audio core
  (HDA, AC'97, mixer, `play`, `beep`, `volume`), threads, the kernel half of job control, `nice`/`renice`, hard links, `/proc/<pid>/fd`,
  libz with gzip/tar/zip, awk/diff/patch/bc/dc and a first `vi`, **USB mass storage with `TOUHOU.LOG` on the stick**, the Gensokyo
  Explorer, hardware audit rounds 1 (console rewrite, write-combining framebuffer, timer and APIC fallbacks, loader robustness) and 2 (merged as `88acd67`: MTRR fallback, PCI BAR sizing fix, D0 wake-up), image
  decoders, the Gallery, wallpapers, PNG screenshots, the PS/2 wheel fix, `/bin/sh` as a link to gapsh and `TOUHOU_CPU` in `tools/qemu.py`.

How well `main` is verified: the whole suite (261 tests, seven parallel groups) passed on the loader with the memory-type fix (`5c0d74b`, docs commits since); the only failure was one mouse-driven GUI test (`notebook_menus_and_recent`) that fails under load and passes alone. The parked branches below are verified only by their own authors.

The OS has booted on the real PC (Dell, i5-7500) since 2026-10-23 evening; [HARDWARE_AUDIT.md](HARDWARE_AUDIT.md) has what that showed and the checklist for the rest (network link, the internal disks).

- Docs (cloud task 9, branch `cloud/docs`): [USER_GUIDE.md](USER_GUIDE.md) for someone at the desktop, a refreshed README, and STATUS / ROADMAP / NEXT checked against the code on main.

## Goals for the next session (the owner, 2026-10-23 night)

The OS has now booted on the real Dell and the fixed image works (desktop, USB keyboard and mouse behind a hub, the stick as `/HumanVillage`, sound: `beep` comes out of the speakers).
The next two goals, in this order:

1. **Install to permanent storage** (so no stick is needed): an `install` tool that partitions a chosen disk (GPT: a 64 MiB ESP with `EFI/BOOT/BOOTX64.EFI`, `kernel.elf`,
   `initrd.img`, `cmdline.txt`, plus a `HUMANVILLAGE` partition), refuses disks with existing partitions unless forced, asks the owner to type the device name, and needs a
   deliberate way past `blk.ro` (a root-only, per-disk write unlock, not the global command-line switch). Questions for the owner first: what is on the Dell's internal disk (Windows? Linux?)
   and may it be wiped, is it SATA or NVMe, and the BIOS "SATA operation" is **RAID On** (Intel RST), which hides every internal disk: it has to be switched to AHCI, which stops an existing
   Windows from booting unless it was prepared. An installed disk boots through the firmware's fallback path (`EFI/BOOT/BOOTX64.EFI`), no NVRAM entry is needed.
2. **Updates over the network** (so the owner stops reflashing): needs the Ethernet link tested first (no cable was plugged in during the first boot; the I219 driver found the chip and reported no
   carrier), then the unmerged network/HTTPS branch. Design sketch: `tools/serve-update.py` on the development PC serves a signed manifest (sha256 of `kernel.elf`, `initrd.img`,
   `BOOTX64.EFI`, `cmdline.txt`, a version number) and the files; an `update URL` command on the OS fetches them (HTTPS with libtls and a pinned certificate, or HTTP plus an ECDSA-signed manifest
   whose public key is baked into the image), checks the signature and hashes, writes them to the ESP as `*.new`, then renames into place; the loader keeps the previous kernel as
   `kernel.old` and falls back to it when the new one did not reach "boot complete" (the NVRAM start counter of the ladder, `start_count()` in `boot/main.c`, is the mechanism for counting
   failed starts). The ESP is never mounted by the policy today: the updater mounts it explicitly (root, read-write, briefly). The forge (`server.tailfbc785.ts.net`) is Tailscale-only,
   so the OS cannot fetch from it directly: the update server is a plain LAN HTTP(S) server on the owner's PC.

Goal 1 has a first implementation on branch `cloud/installer` (task 4 of [CLOUD_AGENT_BRIEF.md](CLOUD_AGENT_BRIEF.md)): `install DISK`, a per-disk write unlock (`BLKWRUNLOCK`), `blk.rw=`, `/proc/bootdisk`, tested in QEMU only; see [INSTALL.md](INSTALL.md).

Hi-dpi (brief task 8) is on branch `cloud/hidpi` (2026-10-24): ui scale 2 by itself at 2560x1440 (or `ui.scale=N`), Fairies/Kappa/shell drawn at full resolution, pixel-drawing apps enlarged by the server, 1x unchanged; tests `hidpi_`, details in [DESKTOP.md](DESKTOP.md) "Hi-dpi screens".

The server half of goal 2 is on branch `cloud/update-server` (2026-10-24): `tools/serve-update.py` signs a manifest and serves it over HTTP, host tests in `tests/host/test_serve_update.py`; format, protocol and the loader fallback design are in [UPDATE.md](UPDATE.md). The OS client and the fallback are not started.

gapsh job control is on branch `cloud/job-control` (2026-10-24): Ctrl-Z, `jobs`, `fg`, `bg`, `wait %N`, `disown`, `%` job names; tests `jobctl_shell_console`, `jobctl_shell_pty`, `jobctl_desktop_terminal`; see [JOBCONTROL.md](JOBCONTROL.md) "State when paused".

The ESP write grant (task 10) is on branch `cloud/esp-grant` (2026-10-24): root mounts the installed ESP read-write for one mount (`BLKESPGRANT` + `mount -o rw,espgrant`, released at the unmount or when the program ends), the `espgrant` applet and `espgrant.h` with the power-cut-safe update sequence for task 5, a FAT rename that keeps the target name in place; tests `espgrant_rules`, `espgrant_helper`, `espgrant_power_cut`; see [INSTALL.md](INSTALL.md) "The ESP write grant" and [UPDATE.md](UPDATE.md) "Writing the ESP".

Test infrastructure is on branch `cloud/test-infra` (task 15, 2026-10-24): `tests/run_all.py --retry-failed` (flaky or failed), an `@xfail` marker (only `usbstor_4k_blocks`, on QEMU before 9), `tools/suite.py` (the seven groups in parallel worktrees) and the manual/weekly `full-suite.yml` workflow (not run yet: it appears once it is on `main`); see [LINUX.md](LINUX.md) "Test runner".

Before both: merge the parked branches (below) and run the whole suite on the result, in particular the network/HTTPS branch, the dirty-volume `fsck` branch (an installed disk and a
stick that is switched off with the button need it) and the audio branch.

## The first real boot (2026-10-23) and the images

The first image hung on `loader: leaving boot services`: **the Dell's firmware never returned from `ExitBootServices`** because the loader had allocated the kernel image and the initrd with
OS-defined UEFI memory types (`0x80000001`/`0x80000002`). The fix (now the default in `boot/main.c`): both are ordinary `EfiLoaderData` and the loader retypes those two ranges in the map it
hands to the kernel; it also writes nothing to the screen once the exit call has returned (`noexitmarks`, not proven necessary). It was found in one evening with no serial port: boot-stage squares
drawn into the framebuffer (`shared/stagemark.h`), a restart probe, and a "ladder" of workarounds tried one per boot and counted in an NVRAM variable (`ladder`, `strategy=N`; the rungs and all
debug words are in [HARDWARE_AUDIT.md](HARDWARE_AUDIT.md)). The log of the first boot that got through is [logs/dell-optiplex-first-boot-2026-10-23.log](logs/dell-optiplex-first-boot-2026-10-23.log).

Images in `build/` (git-ignored; rebuild with `python tools/mkimage.py --zip`): `touhou-os.img.zip` is the fixed normal image (`video=native blk.ro tz=EST5EDT,M3.2.0,M11.1.0`, sha256 of the
`.img` `7f5da4e498c5c13a984b55b35097c4a6e024a2c7225712e92fc6816237e3aac3`), confirmed working on the Dell by the owner; `touhou-os-safe.img.zip` is the text-mode fallback (`text video=1024x768 blk.ro
noaudio nonet usbstorage=off bootlog=off ...`, sha256 `4f61c3e2f6caef65807baab6b28c304bddbf0affdb2957fde5e1330378ce0720`), not needed so far. The `touhou-os-ladder*.img` and `touhou-os-diag*.img`
files are diagnostic builds and can be deleted. Known in these images: Ctrl-Z in a terminal freezes the job and the shell, the `exec` argument limit is still 256 (fixed on the hardening branch), and a
stick that was not shut down cleanly mounts read-only next time (fixed on the dirty-volume branch). The owner's keyboard and mouse sit behind a USB switch/hub that appears when they flip it, so
USB hot-plug of a hub after boot matters.

## Branches that are not merged

All paused with a clean, building tree unless a row says otherwise. Each has its own `docs/<AREA>.md` with "State when paused" and
"How to resume". Locally the branches were `worktree-agent-<id>` (worktrees under `.claude/worktrees/agent-<id>`); on `origin` they are `parked/<area>`:
a504bf8a5a198d471 = `parked/usb-dirty-fsck`, a3d380c78d6285728 = `parked/audio-jukebox`, a46f2d415c0a1a9ca = `parked/network-https`,
a93e3061dde450b33 = `parked/mcc-compiler`, abec503ce2c98467a = `parked/utilities-vi`, a503fea82ea373559 = `parked/libc-accuracy`,
a4adbf8e4aa5804b3 = `parked/kernel-hardening`, `wip/usb-storage-soft-fallbacks` = `parked/usb-stick-quirks-wip`.

| Id | Area | Head | What it holds / what to know |
|----|------|------|------------------------------|
| a6d5e6306014eb8c7 | hardware audit, round 2: **merged into main** (`88acd67`); only its side branch is left, `parked/usb-stick-quirks-wip` | 9efbc0a | The round itself (MTRR fallback, PCI BAR sizing fix, D0 wake-up, NVMe admin queue clamp) is on main and the whole suite passed after it. Side branch `wip/usb-storage-soft-fallbacks` (5bd592f, pushed as `parked/usb-stick-quirks-wip`) holds half-done stick quirk handling (size ladder 64K to 4K, quirk table, bring-up retries); it does **not compile** yet, the missing list is in `docs/HARDWARE_AUDIT.md` |
| a504bf8a5a198d471 | USB follow-up: dirty volumes, fsck | 55638b8 | A dirty HUMANVILLAGE volume is checked read-only at mount (FAT copies, chains, cross-links, sizes, names): consistent means read-write (so `TOUHOU.LOG` keeps working after a hard power-off), problems mean read-only with the reason in the log and `/proc/fatcheck`; `hvforce` and `hvcheck=off` switches; one C scanner shared by the kernel and a host DLL, agreeing with `tools/mkdisk.py --check` on ~50 damage kinds; 7 `fsck_*` tests pass. The `fsck` program and its repair engine (`shared/fatfix.c`) are written but **have never run in the guest**; the FAT write-order review found four ordering gaps (listed in `docs/STORAGE.md`) and nothing is changed there yet |
| a3d380c78d6285728 | audio, round 2 | 797966a | generic HDA codec path walk (unit-tested on 12 invented Realtek-style graphs), Reimu's Jukebox, five original songs (`.tms`, nobody has listened to them), a ProTracker `.mod` player, system sounds with `nosounds`, volume slider in the panel; 20 audio tests pass, the suite was 207/0 with 39 not reached. Merging main conflicts only in `user/bin/hakurei-shell/launch.c`: keep both menu lines (Gallery and Jukebox) |
| a46f2d415c0a1a9ca | network, round 2: HTTPS | 0212736 | `libtls` (TLS 1.3 and 1.2 client with its own crypto, strict X.509), a new kernel RNG (ChaCha20 seeded from RDSEED/RDRAND), a 119-root CA bundle in the initrd, `https://` in `fetch`/`wget`, 14 `tls_*` tests (they need the Python package `cryptography`). The kernel DHCP-stop commit `361314b` was never run in QEMU: if `net_ifconfig_set` or `net_link_*` fail, `git revert 361314b`. **The network panel is not started** (design in `docs/NETWORK.md`). New man pages were not checked by the `manual` test |
| a93e3061dde450b33 | `mcc`, a C compiler | 83acf6a | `mcc`/`cc`/`mld`/`mar` run on the OS; the docs claim byte-identical self-hosting (stage 2 = stage 3). The newest edits (constant folding, jump peephole, inline `mem*`, merged string literals) were **never run in QEMU**; with the whole userland built by `mcc`, 12 GUI tests failed (8 notebook, 4 sketchbook; two causes fixed, not re-run). An early commit contains a stray 400 KB binary: squash-merge it. `docs/MCC.md` |
| abec503ce2c98467a | utilities (parent), with `vi` hardening | c61ba0d | merged with main; `vi` has ~950 scripted checks plus screen tests, a differential test against the host's vim still shows differences (listed in `docs/VI.md`). Nothing shared touched. **Not started:** gapsh job control, the gapsh audit and conformance test, the small new tools and the extensions of the weak applets |
| a503fea82ea373559 | libc accuracy and speed (helper of the utilities agent) | 3a99e04 | exact `strtod` and `printf %f %e %g %a`, block-wise stdio, a faster `malloc`, accurate libm, a leftmost-longest regex: `sort -n` 38 s to 14 s, a 1 MB-line `grep 'a.*b'` no longer crashes. **Conflicts with main in `user/libc/src/stdio.c`** (main has per-stream locks); the resolution recipe and test list are in `docs/UTILITIES.md`; keep its worktree until merged |
| a4adbf8e4aa5804b3 | kernel hardening | 46e03d0 | **`exec` limits raised** (32768 arguments, 128 KiB per string, 512 KiB total; `rm *` and `xargs` work on big folders); `sysfuzz` plus `tools/fuzz.py` found **two kernel panics an ordinary user could cause** (taking init down through `TIOCSPGRP`/`kill`, and a huge offset in tmpfs), both fixed along with ~15 smaller bugs; `kwarn`, `/proc/kstats`, `/proc/vnodes`, heap double-free detection, backtraces that cannot fault. Merges cleanly into main (dry run). Nothing is half-enabled: **SMAP, the OOM killer and the soft-lockup detector are not done** (`kernel/mm/oom.c` and `kernel/sched/watchdog.c` are counter-only stubs). Not run on the final head: the `crash_*` tests (backtrace code was rewritten after the last run). `docs/HARDENING.md` has the findings, the open list and the resume steps |
| a31f584d8c0674dd6 | `vi` helper | 2dfc883 | merged into abec503ce2c98467a; nothing more to do |

Old helper worktrees that are merged and can be deleted: `agent-a8e93238212e069d8`, `agent-ac04a73bcf43119ad`, `agent-a1fd026321d1dbac3`,
`agent-aa2e138ddaa272338`, `agent-a31f584d8c0674dd6`, and the explorer and notebook helpers (`a2a5636a2291d096d`, `a21c5271eaed80a2d`,
`ad1f2fdb71587f5e6`, `a217848ad6a67762c`). A scratch copy of about 1 GB from the media agent lives in
`C:\Users\jston\AppData\Local\Temp\claude\C--Users-jston--claude-spares-spare-rkeNO5\...\scratchpad\mc` and can be deleted.

## What to do first when work resumes

1. Merge in this order, building after each and running the tests named (the usual suspects for conflicts are `docs/STATUS.md`,
   `docs/ROADMAP.md`, `tools/qemu.py`, `tools/build.py` EXTRA_LINKS, `user/bin/hakurei-shell/launch.c`, `shared/syscall.h`):
   1. ~~a6d5e6306014eb8c7 (audit round 2)~~: merged (`88acd67`)
   2. a504bf8a5a198d471 (dirty volumes): `fsck,storage,usbstor`, then run `/bin/fsck` in a VM for the first time
   3. a3d380c78d6285728 (audio): `audio,manual,desktop`
   4. a46f2d415c0a1a9ca (HTTPS): `net,tls,manual`
   5. abec503ce2c98467a (utilities and `vi`): `manual,vi,shell`
   6. a503fea82ea373559 (libc): hand-merge `stdio.c`, then `threads,utest,shell,libc` and everything that uses stdio
   7. a4adbf8e4aa5804b3 (hardening, touches many kernel files, so merge it after the smaller ones): `crash,hardening,selftest,utest,shell,jobctl`, then everything
   8. a93e3061dde450b33 (`mcc`), squashed: `mcc,mld,manual`
2. Run the whole suite on a quiet machine in seven parallel worktrees (the groups are listed in [CLOUD_AGENT_BRIEF.md](CLOUD_AGENT_BRIEF.md), "Rules of the house"). Mouse-driven GUI
   tests (`notebook_*`, `sketchbook_*`, `audio_panel_volume`) flake when the host is busy; rerun a failure alone before suspecting the code.
3. Only then build a stick image (`python tools/mkimage.py --zip`) for the Dell. (The 2026-10-23 images were built before either of the following; both are still open.) Before that, finish **gapsh job control**: in this tree Ctrl-Z in a
   terminal freezes the foreground job and the shell with it (`docs/JOBCONTROL.md` lists what the shell has to do). Raise the `exec` argument
   limits (hardening branch) so `rm *` and `xargs` work on big folders.
4. First real boot (done on 2026-10-23, see above; the advice holds for the next one): use a spare stick, Secure Boot off, follow [REAL_HARDWARE.md](REAL_HARDWARE.md); read the on-screen log (photograph it) and, after
   a clean shutdown or a bad one, `TOUHOU.LOG` and `TOUHOU.PRV` on the stick from another PC. Useful command-line words: `video=1024x768`, `text`,
   `usbstorage=off`, `bootlog=off`, `noaudio`, `fb.mtrr=off`, `fb.cache=uc`, and once `parked/usb-dirty-fsck` is merged `hvforce`, `hvcheck=off`.

## Problems found along the way (not fixed)

- Fairies has no call that tells a window where it is on the screen after the user moved it, so popup menus are drawn inside the window.
- Kappa: the system clipboard is capped at 1024 bytes; KUI keeps 64 key presses per frame; `k_slider` ignores a click whose press and release land in
  one frame; `k_menubar` leaves the drop-down painted for a frame after a pick or Esc and crashes on a NULL label; `k_input_dialog` does not preselect its
  text; `k_confirm_dialog` has only two buttons; there is no triple click.
- `sed` and `grep` call `regexec` on `line + pos` with `REG_NOTBOL`, so `\<`, `\b`, `\B` lose the previous character (`echo aa | sed 's/\<a/X/g'` prints
  `XX`); they should use `REG_STARTEND`. gapsh: `"$@"` with no arguments expands to one empty word; the `printf` builtin needs `\0NNN`.
- Kernel: user-mode SIGSEGV handlers never seem to run; `alarm()` is a stub; `exec` refuses more than 256 arguments or 32 KiB of strings (fixed on the hardening branch).
- A kernel panic while the desktop is running **is** drawn on the main branch (the audit's `fbcon_force_on` in the panic path; the hardening agent's note about a frozen frame came from a base older than that fix). `hwaudit_panic_on_desktop` kills init as root with the desktop up and checks the screen.
- Rare, seen once in about ten full runs: `accounts_passwords` failed with the typed password echoed at the first `New password:` prompt of `passwd` (the second prompt did not echo). `getpass` disables echo before it prints its prompt, so the likely culprit is another writer of the terminal settings (the shell restoring cooked mode after it forked, or `TCSAFLUSH` applying late); it passes alone and in six reruns under load. Worth a look at gapsh's terminal handling around `fork`.
- The fuzz seed 105 with `heapdebug` (`python tools/fuzz.py --secs 30 --extra heapdebug 105`) probably hangs the guest; look at it first.
- `/etc` is rebuilt in RAM at every boot, so passwords, new accounts and a planned `/etc/network.conf` do not persist; only `/HumanVillage` does.
- The `bc_language` test once caught the kernel log line about a SIGPIPE-killed `echo` (a race in the test, not in bc).
- Security: libtls has no side-channel guarantees, no revocation or OCSP, and the CA bundle is frozen at build time (see `docs/TLS.md`).

## Ideas that were not started

- Hi-dpi scaling in Fairies/Kappa (render at 1x, pixel-double into the window, divide pointer coordinates, scale the decorations).
- Japanese text: a double-width bitmap font (kana and common kanji) and double-width cells in the terminal and `kc_text`.
- SMP bring-up (LAPIC IPIs, per-CPU run queues, honest locking), IOAPIC/MSI instead of polling.
- A web browser (HTML subset in a Kappa window) on top of libtls and libimg; a Kappa network panel.
- A PC-98 emulator for the user's own game copies (see `docs/ASSETS.md`: only original or procedural art ships with the OS).
- Panic-time log writes to the stick (unsafe today because block drivers need the scheduler), `gapget` package format.

## Working notes

- Never run two test runs in one checkout (they share `build/esp/cmdline.txt`); use extra `git worktree`s. The guest clock runs several times
  slower than wall time when the host is busy, so tests wait for serial output or pixels and retry instead of sleeping.
- Tests that wait for a serial shell prompt need `text` in their kernel command line (the desktop is the default boot and prints no prompt).
- Every new kappabox applet needs a manual page (`initrd/ScarletDevilMansion/share/patchouli/1/NAME.pw`, lines at most 78 columns) or `manual` fails.
- Agents started with worktree isolation get a worktree made from `origin/main`: tell them to run `git merge main` first, or push before starting them.
- `TOUHOU_CPU=Skylake-Client-noTSX-IBRS` runs QEMU as a CPU closer to the target i5-7500.
- Linux works too (`cloud/linux-tooling`): packages, `TOUHOU_*` variables (tool paths, `TOUHOU_TIMEOUT_SCALE` for slow hosts) and the GitHub Actions smoke run are in [LINUX.md](LINUX.md).
- Commits stay local; push to `origin` (`STONERJ25/Touhou.OS-fork`, private) only when the user asks. Never push to `upstream`.
