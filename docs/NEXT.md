# What is next (hand-over notes)

Written at the end of the first long build session. `main` is the verified tree: everything in docs/STATUS.md works and
is covered by `python tests/run_all.py`. The branches below were started by background agents and are **not merged**.
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
| a2a5636a2291d096d (+ a21c5271eaed80a2d notebook, ad1f2fdb71587f5e6 sketchbook) | Explorer, Patchouli's Notebook, Sketchbook | GUI apps; `tests/guitools.py` came from here |
| a6cf7b95973dbbeca | Ran's Calculator, Pocket Watch, Spell Manager, Kappa control panel, Bunbunmaru news | GUI apps |
| add32d8094441f6e5 | danmaku shooter, minesweeper, snake, `gfx2d` + `gamekit` helpers in libkappa | games |
| a46f2d415c0a1a9ca | networking: NIC drivers, TCP/IP, sockets, DHCP/DNS, tools | `docs/NETWORK.md` |
| a3d380c78d6285728 | audio: HDA, AC97, mixer, Jukebox | `docs/AUDIO.md` |
| a651f6420dbc8cc2c | libz, gzip/tar/zip, PNG/JPEG/GIF decoders, Gallery viewer, wallpaper, PNG screenshots | `docs/MEDIA.md` |
| a4adbf8e4aa5804b3 | threads: clone, futex, pthreads | `docs/THREADS.md` |
| abec503ce2c98467a | awk, diff, patch, vi, bc ... and gapsh conformance | `docs/UTILITIES.md` |

The agents were told to commit working states before the session ended; a branch may stop mid-task. `git log -1 <branch>` and
the doc of the area say how far it got. An agent that is still resumable can be sent a message with the same id.

## Ideas that were not started

- **USB mass storage** (the brief is below): the highest-value next step for the real PC, because it makes `/HumanVillage` on the stick persistent.
- Hi-dpi scaling in Fairies/Kappa (render at 1x, pixel-double into the window, divide pointer coordinates, scale the decorations).
- Japanese text: a double-width bitmap font (kana + common kanji) and double-width cells in the terminal and `kc_text`.
- SMP bring-up (LAPIC IPIs, per-CPU run queues, honest locking), IOAPIC/MSI.
- A small C compiler running on the OS itself.
- A kernel hardening pass: syscall fuzzing, stress and leak tests, an OOM killer.

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
