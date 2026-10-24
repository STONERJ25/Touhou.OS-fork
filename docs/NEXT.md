# What is next (hand-over notes)

Written on 2026-10-23 at about 21:00, when the user paused all work. Read [STATUS.md](STATUS.md) for what exists and this file for what is
parked on branches and what to do first when work resumes.

## Where things stand

`main` (`6be638f` at the time of writing) holds everything that has been merged so far:

- first wave: kernel foundations, desktop, accounts and login, power and time, FAT32 and the HUMANVILLAGE mount policy, AHCI and NVMe,
  xHCI USB with HID, the desktop apps, the games, Patchouli's library, Notebook and Sketchbook;
- merged today: the network stack with its tools, DNS and the NIC drivers (I219 and RTL8168 never ran on hardware), audio core
  (HDA, AC'97, mixer, `play`, `beep`, `volume`), threads, the kernel half of job control, `nice`/`renice`, hard links, `/proc/<pid>/fd`,
  libz with gzip/tar/zip, awk/diff/patch/bc/dc and a first `vi`, **USB mass storage with `TOUHOU.LOG` on the stick**, the Gensokyo
  Explorer, hardware audit round 1 (console rewrite, write-combining framebuffer, timer and APIC fallbacks, loader robustness), image
  decoders, the Gallery, wallpapers, PNG screenshots, the PS/2 wheel fix, `/bin/sh` as a link to gapsh and `TOUHOU_CPU` in `tools/qemu.py`.

How well `main` is verified: the whole suite (182 tests) passed at `711688b` (two mouse-driven GUI flakes passed when rerun alone); the Explorer (10 tests) and the manual-page test passed on top of
it; the media agent's whole run (201 tests, 0 failed) covered `f9358db` plus the media branch, and the network, USB-storage and audit work
were tested on their own branches. **There has been no full run on the exact final commit**; that is the first job when work resumes.

Nothing has ever run on the real PC (Dell, i5-7500). [HARDWARE_AUDIT.md](HARDWARE_AUDIT.md) (in `main`) has the first-boot checklist.

## Branches that are not merged

All paused with a clean, building tree unless a row says otherwise. Each has its own `docs/<AREA>.md` with "State when paused" and
"How to resume". Branch names are `worktree-agent-<id>`; the worktrees are under `.claude/worktrees/agent-<id>`.

| Id | Area | Head | What it holds / what to know |
|----|------|------|------------------------------|
| a6d5e6306014eb8c7 | hardware audit, round 2 | 9efbc0a | MTRR write-combining as a measured fallback (`fb.mtrr=auto|on|off`), **a PCI BAR sizing fix** (the old code could loop over ~2^47 pages on a 64-bit BAR and hang the first boot), `pci_set_d0` for every device, NVMe admin queue clamped to MQES; 15 `hwaudit` tests pass. Not re-run after it: usb, net, audio, desktop, the full suite. Side branch `wip/usb-storage-soft-fallbacks` (5bd592f) holds half-done stick quirk handling (size ladder 64K to 4K, quirk table, bring-up retries); it does **not compile** yet, the missing list is in `docs/HARDWARE_AUDIT.md` |
| a504bf8a5a198d471 | USB follow-up: dirty volumes, fsck | 55638b8 | A dirty HUMANVILLAGE volume is checked read-only at mount (FAT copies, chains, cross-links, sizes, names): consistent means read-write (so `TOUHOU.LOG` keeps working after a hard power-off), problems mean read-only with the reason in the log and `/proc/fatcheck`; `hvforce` and `hvcheck=off` switches; one C scanner shared by the kernel and a host DLL, agreeing with `tools/mkdisk.py --check` on ~50 damage kinds; 7 `fsck_*` tests pass. The `fsck` program and its repair engine (`shared/fatfix.c`) are written but **have never run in the guest**; the FAT write-order review found four ordering gaps (listed in `docs/STORAGE.md`) and nothing is changed there yet |
| a3d380c78d6285728 | audio, round 2 | 797966a | generic HDA codec path walk (unit-tested on 12 invented Realtek-style graphs), Reimu's Jukebox, five original songs (`.tms`, nobody has listened to them), a ProTracker `.mod` player, system sounds with `nosounds`, volume slider in the panel; 20 audio tests pass, the suite was 207/0 with 39 not reached. Merging main conflicts only in `user/bin/hakurei-shell/launch.c`: keep both menu lines (Gallery and Jukebox) |
| a46f2d415c0a1a9ca | network, round 2: HTTPS | 0212736 | `libtls` (TLS 1.3 and 1.2 client with its own crypto, strict X.509), a new kernel RNG (ChaCha20 seeded from RDSEED/RDRAND), a 119-root CA bundle in the initrd, `https://` in `fetch`/`wget`, 14 `tls_*` tests (they need the Python package `cryptography`). The kernel DHCP-stop commit `361314b` was never run in QEMU: if `net_ifconfig_set` or `net_link_*` fail, `git revert 361314b`. **The network panel is not started** (design in `docs/NETWORK.md`). New man pages were not checked by the `manual` test |
| a93e3061dde450b33 | `mcc`, a C compiler | 83acf6a | `mcc`/`cc`/`mld`/`mar` run on the OS; the docs claim byte-identical self-hosting (stage 2 = stage 3). The newest edits (constant folding, jump peephole, inline `mem*`, merged string literals) were **never run in QEMU**; with the whole userland built by `mcc`, 12 GUI tests failed (8 notebook, 4 sketchbook; two causes fixed, not re-run). An early commit contains a stray 400 KB binary: squash-merge it. `docs/MCC.md` |
| abec503ce2c98467a | utilities (parent), with `vi` hardening | c61ba0d | merged with main; `vi` has ~950 scripted checks plus screen tests, a differential test against the host's vim still shows differences (listed in `docs/VI.md`). Nothing shared touched. **Not started:** gapsh job control, the gapsh audit and conformance test, the small new tools and the extensions of the weak applets |
| a503fea82ea373559 | libc accuracy and speed (helper of the utilities agent) | 3a99e04 | exact `strtod` and `printf %f %e %g %a`, block-wise stdio, a faster `malloc`, accurate libm, a leftmost-longest regex: `sort -n` 38 s to 14 s, a 1 MB-line `grep 'a.*b'` no longer crashes. **Conflicts with main in `user/libc/src/stdio.c`** (main has per-stream locks); the resolution recipe and test list are in `docs/UTILITIES.md`; keep its worktree until merged |
| a4adbf8e4aa5804b3 | kernel hardening | c889090 | sysfuzz and groundwork (`kwarn`, `/proc/kstats`, heap limits); was still working when this file was written, see its report and `docs/HARDENING.md` if it exists. Planned: OOM killer, SMEP/SMAP, lockup detector, and raising the `exec` argument limits (today 256 entries and about 32 KiB) |
| a31f584d8c0674dd6 | `vi` helper | 2dfc883 | merged into abec503ce2c98467a; nothing more to do |

Old helper worktrees that are merged and can be deleted: `agent-a8e93238212e069d8`, `agent-ac04a73bcf43119ad`, `agent-a1fd026321d1dbac3`,
`agent-aa2e138ddaa272338`, `agent-a31f584d8c0674dd6`, and the explorer and notebook helpers (`a2a5636a2291d096d`, `a21c5271eaed80a2d`,
`ad1f2fdb71587f5e6`, `a217848ad6a67762c`). A scratch copy of about 1 GB from the media agent lives in
`C:\Users\jston\AppData\Local\Temp\claude\C--Users-jston--claude-spares-spare-rkeNO5\...\scratchpad\mc` and can be deleted.

## What to do first when work resumes

1. Merge in this order, building after each and running the tests named (the usual suspects for conflicts are `docs/STATUS.md`,
   `docs/ROADMAP.md`, `tools/qemu.py`, `tools/build.py` EXTRA_LINKS, `user/bin/hakurei-shell/launch.c`, `shared/syscall.h`):
   1. a6d5e6306014eb8c7 (audit round 2, small and important): `hwaudit,usb,blk,selftest`
   2. a504bf8a5a198d471 (dirty volumes): `fsck,storage,usbstor`, then run `/bin/fsck` in a VM for the first time
   3. a3d380c78d6285728 (audio): `audio,manual,desktop`
   4. a46f2d415c0a1a9ca (HTTPS): `net,tls,manual`
   5. abec503ce2c98467a (utilities and `vi`): `manual,vi,shell`
   6. a503fea82ea373559 (libc): hand-merge `stdio.c`, then `threads,utest,shell,libc` and everything that uses stdio
   7. a4adbf8e4aa5804b3 (hardening) once it has a final report: everything
   8. a93e3061dde450b33 (`mcc`), squashed: `mcc,mld,manual`
2. Run the whole suite on a quiet machine in six parallel worktrees (the split that worked: usb / storage / blk,crash,selftest,utest,kshell,jobctl /
   notebook,sketchbook / the rest of tools, audio, net, threads / power, accounts, login, time, apps, desktop and the games). Mouse-driven GUI
   tests (`notebook_*`, `sketchbook_*`, `audio_panel_volume`) flake when the host is busy; rerun a failure alone before suspecting the code.
3. Only then build a stick image (`python tools/mkimage.py --zip`) for the Dell. Before that, finish **gapsh job control**: in this tree Ctrl-Z in a
   terminal freezes the foreground job and the shell with it (`docs/JOBCONTROL.md` lists what the shell has to do). Raise the `exec` argument
   limits (hardening branch) so `rm *` and `xargs` work on big folders.
4. First real boot: use a spare stick, Secure Boot off, follow [REAL_HARDWARE.md](REAL_HARDWARE.md); read the on-screen log (photograph it) and, after
   a clean shutdown or a bad one, `TOUHOU.LOG` and `TOUHOU.PRV` on the stick from another PC. Useful command-line words: `video=1024x768`, `text`,
   `usbstorage=off`, `bootlog=off`, `noaudio`, `fb.mtrr=off`, `fb.cache=uc`, `hvforce`, `hvcheck=off`.

## Problems found along the way (not fixed)

- Fairies has no call that tells a window where it is on the screen after the user moved it, so popup menus are drawn inside the window.
- Kappa: the system clipboard is capped at 1024 bytes; KUI keeps 64 key presses per frame; `k_slider` ignores a click whose press and release land in
  one frame; `k_menubar` leaves the drop-down painted for a frame after a pick or Esc and crashes on a NULL label; `k_input_dialog` does not preselect its
  text; `k_confirm_dialog` has only two buttons; there is no triple click.
- `sed` and `grep` call `regexec` on `line + pos` with `REG_NOTBOL`, so `\<`, `\b`, `\B` lose the previous character (`echo aa | sed 's/\<a/X/g'` prints
  `XX`); they should use `REG_STARTEND`. gapsh: `"$@"` with no arguments expands to one empty word; the `printf` builtin needs `\0NNN`.
- Kernel: user-mode SIGSEGV handlers never seem to run; `alarm()` is a stub; `exec` refuses more than 256 arguments or 32 KiB of strings.
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
- Commits stay local; push to `origin` (`stonerj/Touhou.OS-fork`, private) only when the user asks. Never push to `upstream`.
