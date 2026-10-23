# What is next (hand-over notes)

Written at the end of the first long build session. `main` is the verified tree: everything in docs/STATUS.md works and
is covered by `python tests/run_all.py`. Already merged from agents: Patchouli's library (pnote, pager, manuals), FAT32/virtio-blk,
AHCI/NVMe, USB, the five desktop apps, the games, the Notebook and the Sketchbook. The branches below were started by background agents and are **not merged**.
Each has its own `docs/<AREA>.md` written by its author; read that first, then `git log main..<branch>`.

## Branches and how to merge one

```powershell
git log --oneline main..refs/heads/<branch>          # what it adds
git merge --no-ff refs/heads/<branch>                # resolve conflicts (shared files: shared/syscall.h, kernel/sys/syscalls.c,
                                                     #  tools/qemu.py, kappabox.h, launch.c, build.py are the usual suspects)
python tools/build.py ; python tests/run_all.py      # the whole suite (about 25 minutes; split it with comma-separated
                                                     #  prefixes, e.g. run_all.py "usb" in a second worktree to run groups in parallel)
```

Never run two test runs in the same checkout at once: they share `build/esp/cmdline.txt`. A second `git worktree` has its own `build/`.
New kappabox applets need a manual page (`initrd/ScarletDevilMansion/share/patchouli/1/NAME.pw`) or the `manual` test fails.

| Branch (worktree-agent-...) | Area | Notes |
|-----------------------------|------|-------|
| a2a5636a2291d096d | Gensokyo Explorer (file manager) | **no application code exists**. The branch adds only `docs/EXPLORER.md` (design decisions, platform facts, known toolkit problems, the order to build it in); `tests/guitools.py` from this agent is already in `main` |
| a21c5271eaed80a2d | Notebook polish (a sub-agent of the explorer agent) | 6 small commits on top of the merged Notebook: Reload from Disk (F5), the unsaved-changes Save goes through the changed-on-disk check, a hint on an empty page, line-ending and gutter-click tests. Merges cleanly into `main` (dry run); the context-menu retry in its last commit is unverified |
| ad1f2fdb71587f5e6 | Sketchbook polish (a sub-agent of the explorer agent) | 3 small commits on top of the merged Sketchbook (Enter confirms the size dialog and so on). Merges cleanly; the last tweak was only rebuilt, not re-tested |
| a4adbf8e4aa5804b3 | threads: clone, futex, pthreads (syscalls 106-115) | 15 commits, paused, builds; the most mature branch of the second wave. Its last full test run was at a279b04. `docs/THREADS.md` on the branch |
| a46f2d415c0a1a9ca | networking (syscalls 90-99) | 4 commits, paused: loopback TCP/IP, sockets, virtio-net/e1000/e1000e drivers and DHCP are in. Not done: ping/wget/nslookup tools, DNS, the RTL8168 driver. `docs/NETWORK.md` |
| a3d380c78d6285728 | audio (syscalls 100-105) | 12 commits, paused: kernel mixer, HDA and AC97 drivers, libaudio, `play`/`beep`/`volume`. Not done: Jukebox, songs. `docs/AUDIO.md` |
| a651f6420dbc8cc2c | media: libz, gzip/tar/zip, libimg, Gallery, wallpaper, screenshots | done and merged into `main` (the first part twice: `SYS_utimes` = 82 stays). libimg reads PNG, JPEG (baseline and progressive), GIF (with animation), BMP, PNM and TGA and writes PNG, BMP and PPM; the Gallery, a wallpaper picture (`wallpaper`, Control Panel, Gallery), PNG screenshots in `~/Pictures`, `k_image_thumbnail`, `imginfo`/`convert`, and the FAT `settimes` hook are in. Not done: thumbnails inside the Explorer, JPEG decode-time downscaling. `docs/MEDIA.md` |
| abec503ce2c98467a (parent) | userland utilities: merges the four children below, plus `docs/UTILITIES.md` | merge this one, not the children (same content). Done: awk, diff, patch, bc, dc and a first vi. Not started: the gapsh audit, job control (needs a kernel stopped state), join/split/fmt/column/timeout/time/watch/nohup/pgrep/killall..., extensions of the weak applets. `docs/UTILITIES.md` lists everything |
| a8e93238212e069d8 / ac04a73bcf43119ad / a1fd026321d1dbac3 / aa2e138ddaa272338 | awk / diff and patch / bc and dc / vi | children of the utilities parent. **vi has only run 11 checks in the guest** (basic insert/delete/yank/put, undo, search, counts, dot, `:wq`); motions, text objects, registers, macros, visual modes, `:s`, `:g`, filters and mappings are written but untested, so expect bugs. awk, diff, patch, bc and dc passed their own tests on their branches |

The agents were stopped at about 15:58 on 2026-10-23 (the user had to close the laptop). All of these branches, and `main`, are on the
Forgejo copy of the repository (`origin` = `stonerj/Touhou.OS-fork`, private).

When merging any of them: tests that wait for a serial shell prompt need `text` in their command line (the desktop is the default
boot now, and it never prints a prompt), and the guest clock runs slowly when the host is busy, so give tests generous timeouts.

The agents were told to commit working states before the session ended; a branch may stop mid-task. `git log -1 <branch>` and
the doc of the area say how far it got. An agent that is still resumable can be sent a message with the same id.

## Ideas that were not started

- **USB mass storage** (the brief is below): the highest-value next step for the real PC, because it makes `/HumanVillage` on the stick persistent.
- Hi-dpi scaling in Fairies/Kappa (render at 1x, pixel-double into the window, divide pointer coordinates, scale the decorations).
- Japanese text: a double-width bitmap font (kana + common kanji) and double-width cells in the terminal and `kc_text`.
- SMP bring-up (LAPIC IPIs, per-CPU run queues, honest locking), IOAPIC/MSI.
- A small C compiler running on the OS itself.
- A kernel hardening pass: syscall fuzzing, stress and leak tests, an OOM killer.

## Problems found along the way (not fixed)

- `kernel/drivers/keyboard.c` `mouse_byte()` passes the PS/2 wheel nibble through unchanged, so every app scrolls backwards with
  QEMU's mouse; the fix is probably `wheel = -wheel;` there (check the xHCI HID mouse path separately).
- Fairies has no call that tells a window where it is on the screen after the user moved it, so a popup menu cannot be placed at the
  pointer (the Notebook draws its menu inside the window; `docs/EXPLORER.md` on the explorer branch has a plan).
- Kappa: the system clipboard is capped at 1024 bytes; KUI keeps 64 key presses per frame; `k_slider` ignores a click whose press and release
  land in one frame; `k_menubar` leaves the drop-down painted for a frame after a pick or Esc and crashes on a NULL label;
  `k_input_dialog` does not preselect its initial text; there is no triple click.
- libc/kernel (from the utilities agent): `strtod` is inexact ("0.3" parses as 0.30000000000000004), `printf %f` rounds approximately,
  `fwrite` goes byte by byte, regex is a recursive leftmost-first backtracker, there is no hard link, no process priority, no
  `/proc/<pid>/fd`; SIGTSTP/SIGSTOP are ignored because the kernel has no stopped state (so no job control).
- The guest clock lags when the host is busy and the PS/2 pointer drops packets under load: GUI tests should poll and retry once.

## Brief for the USB mass storage task

YOUR TASK: USB mass storage, so that the stick TouhouOS booted from can be used as storage.

Context: docs/USB.md ("Writing another class driver"), docs/STORAGE.md, docs/BLOCKDEVICES.md and docs/REAL_HARDWARE.md describe what exists: an xHCI driver with hubs and HID (kernel/drivers/usb/*), the block layer (kernel/fs/blk.c, kernel/include/blk.h), SATA/NVMe drivers, a FAT32 driver and the HUMANVILLAGE mount policy (kernel/fs/mountpolicy.c: exactly one volume labelled HUMANVILLAGE is mounted automatically, nothing else ever is). The real target is a Dell PC that boots from a USB stick containing an EFI System Partition and a HUMANVILLAGE FAT32 partition (tools/mkimage.py builds the image). Today the OS runs from RAM and cannot see the stick; with this task `/HumanVillage` becomes the stick's persistent partition.

1. A bulk-only-transport class driver (interface class 8, subclass 6 "SCSI transparent", protocol 0x50; also accept subclass 2/4/5 only if trivially similar, otherwise ignore) in kernel/drivers/usb/storage.c, following the extension recipe in docs/USB.md: GET MAX LUN, CBW/data/CSW with tag checking, reset recovery (bulk-only reset + clear halt on both endpoints) on STALL or a bad CSW, INQUIRY, TEST UNIT READY with REQUEST SENSE polling (sticks need seconds to become ready; give up after a bounded time), READ CAPACITY(10) and (16), READ/WRITE(10) and (16) in chunks of at most 64 KiB, SYNCHRONIZE CACHE(10) on flush (ignore "not supported"), write-protect detection through MODE SENSE(6) (register the device read-only), 512 and 4096 byte logical blocks, removable media states (no media / media changed), several LUNs, and hot-unplug (requests in flight fail with -ENODEV, the block device goes away cleanly; blk.h has no unregister call yet: add one in the block layer if needed, keeping it small and safe, partition children included). Remember the thread rule from USB.md: the USB stack belongs to the usbd thread, so block requests run through usb_call().
2. Register disks with the block layer under the shared SCSI-disk naming the SATA driver uses (sda, sdb ... : look at how kernel/drivers/ahci.c allocates names and share the allocator instead of inventing a second scheme), with a driver name like "usb-storage" visible in /proc/partitions and lsblk. Honour the `blk.ro` command-line switch only for internal SATA/NVMe disks (as documented), but make USB sticks respect the device's own write-protect state. Writes need the same write-through policy the block drivers use (flush after writes).
3. Make HUMANVILLAGE-on-the-stick work end to end: USB enumeration finishes seconds after the kernel starts, long after the boot-time storage_mount_boot_volumes() call, so (a) call it (or a hotplug-aware variant in kernel/fs/mountpolicy.c) when a USB disk appears, and (b) make boot wait for USB to settle before init starts the first session: expose something simple like /proc/usb-ready or a kernel function `usb_wait_settled(ms)` that kmain calls (bounded, at most about 8 seconds, only when an xHCI controller exists and at least one port reported a device; the wait must end early the moment the controller and all ports are quiet) so the stick's HUMANVILLAGE partition is mounted before login. The policy must still mount only the volume labelled HUMANVILLAGE; if a second one shows up, ignore it with a log line (already the policy). Also decide what happens when the internal disk has a HUMANVILLAGE label too (first one found wins; document it).
4. FAT timestamps: the FAT driver writes times as UTC while Windows and most other systems read FAT times as local time. Add a mount option (e.g. tzoffset=MINUTES) and make the mount policy and the `mount` applet pass the system's current UTC offset (the kernel has no time zone: derive it in user space for `mount`, and for the boot-time automount use the offset implied by the `tz=` command-line word, parsing only the standard offset of a POSIX TZ string, or leave UTC when absent). Keep it small, test it.
5. Safety: never write to a USB disk unless it is mounted (as above) or root writes the /dev node; never format; the partition scan is read-only. Validate everything the device reports (capacity zero or absurd, block size not a power of two, sense data lies, CSW residue larger than the request): a hostile or broken stick must not crash the kernel or hang boot beyond the bounded waits.
6. Tests (tests/case_usbstorage.py) using QEMU's `-device usb-storage,drive=...` on an `-device qemu-xhci` with a raw image built by tools/mkdisk.py: the device appears with the right size/model, partitions are scanned, reads match the host's checksums, writes land in the image after the VM exits, HUMANVILLAGE on a USB disk is mounted automatically before the first session and files persist across a reboot (two boots on the same image), a stick plugged in later via QMP device_add (hot-plug, mounted by policy hook or by hand) and unplugged via device_del while idle and during I/O (no panic, the block device vanishes), a read-only stick (QEMU `readonly=on`), a stick with a corrupt partition table, UASP-capable QEMU device (`usb-uas` needs a bus; skip if QEMU lacks it) is simply ignored, two sticks, 4096-byte blocks if QEMU can emulate them (`logical_block_size=4096`), and the timestamp option (create a file with a known time zone, read it back with tools/mkdisk.py --ls). Extend tools/qemu.py minimally if the harness needs it (extra drives/devices already work through Qemu(extra=[...])).
7. Update docs/USB.md, docs/STORAGE.md and docs/REAL_HARDWARE.md (section 5 says reading the stick from inside TouhouOS needs USB mass storage; that becomes true) and tools/mkimage.py's help text if anything changes.
