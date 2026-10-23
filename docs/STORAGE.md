# Persistent storage

Keine keeps the Human Village's records on a real disk when there is one: a virtio-blk driver, a FAT32/FAT16
filesystem with long names, a mount policy that touches exactly one volume, `df`/`statfs`, and a pure-Python tool that
builds and checks disk images. Without such a disk nothing changes: the homes live in RAM (tmpfs) as before.

```
 user space     df  mount  umount  sync  ls ...            libc: statfs(), gap_mount_opts()
 ------------------------------------------------------------------------------------------------
 VFS            mount options, FsType.statfs/sync/busy, vfs_sync_all()      kernel/fs/vfs.c
 FAT            fat32.c   vnode ops, mount/unmount, dirty-bit protocol, flusher thread
                fat_dir.c directory listings, entry sets, slot allocation
                fat_chain.c FAT table, cluster allocator, per-file extent cache
                fat_name.c UTF-8/UTF-16, 8.3 names, LFN checksums, case folding
                fat_cache.c write-back page cache with ordered flush
 policy         mountpolicy.c   what is mounted, where, and what is never touched
 block layer    blk.c           /dev/vda, vda1, ... (GPT/MBR scan), /proc/partitions, hot-unplug
 driver         virtio_blk.c    legacy I/O-port and modern MMIO transports, polled
                ahci.c nvme.c   SATA and NVMe (docs/BLOCKDEVICES.md)
                usb/storage.c   USB sticks, card readers, enclosures: sdb, sdc, ... (docs/USB.md)
 bootlog        bootlog.c       the kernel log kept on the HUMANVILLAGE volume (docs/REAL_HARDWARE.md)
```

## Layout of a TouhouOS disk

`tools/mkdisk.py` builds the reference layout (the installer will one day do the same):

| Partition | GPT name | FAT label | Contents |
|-----------|----------|-----------|----------|
| 1 | `TOUHOUOS-ESP` (EFI System Partition) | `TOUHOUOS-ES` | `EFI/BOOT/BOOTX64.EFI`, `kernel.elf`, `initrd.img`, `cmdline.txt` |
| 2 | `HUMANVILLAGE` (basic data) | `HUMANVILLAG` | `reimu/`, `marisa/`, `sakuya/`, anything else |

FAT volume labels hold 11 characters, so the twelve-letter names are stored truncated on the volume itself; the GPT
partition name has room for the full text. The mount policy accepts both spellings.

## Mount policy (`kernel/fs/mountpolicy.c`)

1. **Exactly one volume is mounted automatically**: the one labelled `HUMANVILLAGE`, read-write, on `/HumanVillage`.
   The label is, in this order, a GPT partition name (no I/O needed) or a FAT volume label.
2. **No other disk or partition is ever mounted, written, formatted or probed with writes.** To find a FAT label the
   kernel *reads* the boot sector (and the first root-directory cluster) of partitions whose type says FAT
   (MBR 0x01/0x04/0x06/0x0B/0x0C/0x0E, GPT "basic data") and of whole disks without a partition table. It never reads
   EFI System, Linux, NTFS-typed or unknown partitions. A disk of another operating system stays byte-identical
   (`tests/case_storage.py` compares checksums of whole images and of single partitions).
3. When nothing matches, boot continues with the in-RAM tmpfs home directories exactly as today.
4. After a successful mount `/HumanVillage/reimu`, `marisa` and `sakuya` are created if missing.
5. The command line word `nohumanvillage` skips all of this. The policy runs once at boot for the disks that exist by then (virtio,
   SATA, NVMe), and again for every disk that appears later (USB sticks) through `storage_disk_added()`, see "Sticks" below; or root
   mounts by hand: `mount -t fat32 -o userdirs /dev/sdb2 /HumanVillage`.
6. Writes to a device happen only through (a) a FAT volume that was selected by the policy or by `mount`, and (b) root
   writing `/dev/<disk>` by hand. Nothing else in the kernel writes to a block device.
7. **Which volume wins when several are labelled HUMANVILLAGE: the first one found.** Internal disks are looked at at boot, before the
   USB bus has even been enumerated, so an internal HUMANVILLAGE volume (a virtio disk in QEMU, a SATA or NVMe partition) is mounted
   and a stick's is left alone with a log line (`sdb2 holds a HUMANVILLAGE volume too (partition label), but /HumanVillage already lives on
   /dev/vda2: the first one found wins, this one is left alone`). Without an internal one the first stick to become ready wins, and the
   second stick's volume is ignored the same way. Within one pass, a GPT partition *name* beats a FAT volume *label*, and device order
   decides between equals.

### Sticks (USB)

USB enumeration finishes seconds after the kernel starts, long after the boot-time call, so there are two extra pieces:

* **Hot-plug hook.** When the mass storage driver has registered a stick's disk (and its partitions), it calls
  `storage_disk_added(disk)`. That starts a worker thread (`hv-automount`; the mount reads the stick through the very thread that
  announces it, and holds the VFS namespace lock, so it must not run there), which runs the same policy: nothing is mounted on
  `/HumanVillage` yet -> mount the first HUMANVILLAGE volume found; else only say so if this disk has one too. A disk without the label
  is logged (`sdb has no HUMANVILLAGE volume: nothing to mount from it`) and otherwise left strictly alone.
* **Boot waits for USB** (`usb_wait_settled(8000)` just before the first session starts, see USB.md): with a stick present the volume is
  mounted before login, normally about a second after the controller came up; the wait is bounded at 8 s and skipped without USB, so a stick that
  is slow or absent never holds up boot. A stick that becomes ready after the wait is mounted by the hook later, while sessions run: new
  processes see the stick's `/HumanVillage`, old ones keep the tmpfs homes they had.

A pulled stick leaves its mount behind (nothing can be written back; reads of what was never cached fail with `ENODEV`): the FAT driver
logs `fat32: sdb2 vanished while it was mounted on /HumanVillage ...` once, `umount` drops the mount without a write-back, and the next
stick is mounted by the hook. While the stale mount is still there a new stick is *not* mounted (the log says `... /HumanVillage is
still mounted on /dev/sdb2, whose stick was pulled out: umount it (or reboot) ...`).

An unclean pull leaves the volume flagged dirty, so the next mount is read-only (the policy never uses `-o force`; see "Safety design").
That is the price of the flag protocol on a medium people unplug; eject (`umount`, `poweroff`, `sync`) before pulling.

Decisions are logged with `klog("Keine", ...)`, one line per mounted volume:

```
[Keine] fat32: vda2 mounted on /HumanVillage, 126 MiB, 512-byte clusters, label 'HUMANVILLAG', home directories per user
[Keine] no HUMANVILLAGE volume found: homes stay in RAM (tmpfs)
[Keine] fat32: vda2 mounted on /HumanVillage, ..., read-only: the volume was not cleanly unmounted (mount with -o force to override)
```

### mount / umount

```
mount [-t fat32|vfat|fat16] [-o options] [-r] /dev/vda2 /mnt        (-t defaults to fat32 for /dev/... sources)
umount /mnt
```

`umount` flushes the volume, marks it clean and detaches it. It is refused (`busy`) while any file or directory of the
volume is open or is a process's current directory, and also when the final write-back fails (the volume then stays
mounted rather than losing data).

| Option | Meaning |
|--------|---------|
| `ro`, `rw` | read-only / read-write (also `-r`); a read-only device is always mounted read-only |
| `force` | mount a volume that is flagged dirty read-write anyway (its flags then stay dirty) |
| `sync` | write back after every operation |
| `uid=N`, `gid=N` | owner of every file (default root, 0) |
| `fmask=OCT`, `dmask=OCT`, `umask=OCT` | permission bits to remove; defaults `fmask=0133`, `dmask=0022` give files 0644, directories 0755 |
| `userdirs` | a top-level directory named like a user, and everything below it, belongs to that user (see below) |
| `usermap=name:uid:gid/name:uid:gid` | the user table for `userdirs`; the `mount` applet fills it from `/etc/passwd`, the boot mount uses reimu, marisa, sakuya |
| `tzoffset=MINUTES` | FAT stores *local* time; the volume's clock is this many minutes east of UTC (`-300` for EST, `540` for JST; range +-1440). Default 0: UTC, as before. The `mount` applet adds the current offset of its own time zone (summer time included) to FAT mounts unless the option is given; the boot-time mount derives it from the standard offset of the `tz=` command line word (`tz=EST5EDT,M3.2.0,M11.1.0` -> `-300`; summer time rules are not applied) and passes nothing when there is no `tz=` |
| `defaults`, `noatime`, `relatime`, `nodev`, `nosuid`, `noexec`, `exec`, `suid`, `dev`, `auto`, `noauto`, `user`, `async`, `quiet`, `utf8` | accepted and ignored |

Any other option fails the mount. The option string reaches the kernel through the fifth argument of `SYS_mount`
(`MNT_OPTS` in the flags word, `gap_mount_opts()` in libgap).

### Owners and modes on a filesystem that has none

* Outside user directories: owner `uid:gid` (root), files `0777 & ~fmask`, directories `0777 & ~dmask`.
* With `userdirs`: `/HumanVillage/reimu` and everything below it belong to `reimu:reimu`, directories `0700`, files `0600`,
  so reimu's home is private (`marisa` cannot even list it; root can).
* The DOS read-only attribute removes the write bits.
* `chmod`/`chown` that change nothing succeed (so `cp -p`, `tar` and friends keep working). A `chmod` that only toggles the
  write bits of a file sets/clears the read-only attribute and is persisted. Everything else fails with `EPERM`, because
  the filesystem would only pretend to store it.
* New files take their mode and owner from these rules, not from the creating process or its umask.

## Safety design

* **Validation before any write.** The boot sector is checked (sector size, cluster size, number of FATs, reserved
  sectors, FAT size against the cluster count, declared size against the device, root cluster, signature, FAT32 version).
  A volume that fails is not mounted and nothing is written; the reason is logged.
* **Bounded chains.** Cluster numbers from disk are range-checked before use; chain walks are bounded by the file size
  (files), by the 2 MiB directory limit (directories) and by the volume size; the extent cache is capped. A chain that
  disagrees with the size (loop, too long, too short, cross-linked to nothing) makes the file read-only for this boot
  (`EIO` on write); deleting it still works. Damaged directory entries read as size 0 with `EIO`.
* **Dirty flag protocol.** On a read-write mount nothing is written until the first modification. Before it, the FAT32
  clean-shutdown bit (FAT[1] bit 27, FAT16 bit 15) is cleared in every FAT copy and the boot-sector flag byte is set, and
  both are forced to the device. `sync`, `umount`, `reboot`/`poweroff` and the periodic flush (when the volume has been
  idle for 3 s) write everything back and restore the flags (flag byte and FSInfo first, FAT[1] last). A volume found
  flagged dirty (or with the "disk errors seen" bit) mounts **read-only** with a warning unless `-o force` is given.
* **Write ordering.** The cache flushes file data first, then the FAT, then directory entries/FSInfo/boot sector, with a
  device cache flush between the classes. A power cut can therefore leak clusters (fsck reclaims them) but not leave
  an entry pointing at data that was never written. Unlinking while open keeps the clusters until the last close.
* **Periodic and explicit flushing.** A kernel thread (`fat-flush`) writes back dirty pages older than 3 s; `sync(2)`
  (the `sync` applet, `dd`, `reboot`, `poweroff`) flushes every writable filesystem; a large backlog is flushed by the
  writer itself.
* **No overlap.** A device cannot be mounted twice, nor can a disk and one of its partitions.
* Everything is behind one mutex per volume; the vnode release path is lock-free and defers cleanup to the next locked
  operation.

## FAT specifics and limits

* FAT32 and FAT16 (the QEMU `fat:` ESP is FAT16) read/write. **FAT12, exFAT and NTFS are not supported.**
* Long file names (UTF-16 on disk, UTF-8 at the VFS; up to 255 UTF-16 units), proper 8.3 short names with `~N` tails and
  checksums, NT case flags for pure lower-case 8.3 names, short-name aliases (`PROGRA~1`) are accepted on lookup.
* Names are **case-insensitive** (simple folding for ASCII, Latin-1/Extended-A, Greek, Cyrillic) and the stored case is
  preserved. `"` `*` `/` `:` `<` `>` `?` `\` `|`, control characters and trailing dots/spaces are rejected with `EINVAL`.
* Files are limited to 4 GiB - 1 (`EFBIG`); a directory to 65 536 entries (2 MiB); the FAT16 root directory to its fixed
  512 (or whatever the volume says) entries - it fills up with `ENOSPC` and recovers after a delete.
* No symlinks, hard links, FIFOs or device nodes; no execute bits unless you mount with a different `fmask`.
* Timestamps have 2 s resolution and are clamped to 1980..2107. Access times are not updated. A directory's own modification time is
  not persisted. FAT has no time zone field: Windows, Linux (without `tz=`) and most cameras read the stored fields as *local* time. With
  `-o tzoffset=MINUTES` the driver adds the offset when it writes and subtracts it when it reads, so a file saved at 12:04 JST on the
  stick shows 12:04 in Windows, and a file saved there at 09:30 local shows as 00:30 UTC here. Without the option (a mount made by hand
  in UTC, a boot without `tz=`) the fields are UTC, exactly as before. The offset is one fixed number per mount: files saved in summer
  and winter differ by an hour from what a DST-aware reader computes, which is how FAT has always behaved on non-DST-aware systems.
* Sparse files do not exist: seeking past the end and writing, or growing with `truncate`, really allocates zeros. The
  bytes between the old end and the new data always read as zero (stale cluster contents are never exposed).
* The allocator prefers contiguous runs: a new file starts in a free run of at least 64 clusters when one exists, and
  appends continue right after the file's last cluster.
* There is no journal. After a crash the dirty flag keeps the volume read-only until a file system checker ran
  (`python tools/mkdisk.py --check image`, or Windows/Linux `fsck`), then `mount -o force` to override at your own risk.
* 4096-byte-sector devices work (FAT with 4096-byte sectors, `logical_block_size=4096`); a FAT with 512-byte sectors on a
  4Kn device is refused (sector size smaller than the device's). The cache works in 4 KiB pages relative to the volume.
* The page cache is capped at 1/8 of RAM (at most 64 MiB); free clusters are counted at mount for small FATs and trusted
  from FSInfo for large ones (counted on demand when FSInfo is missing).

## virtio-blk

`kernel/drivers/virtio_blk.c` drives virtio block devices found on the PCI bus (1af4:1001 transitional/legacy and
1af4:1042 modern). The modern transport (PCI capabilities, MMIO common/notify/device structures, `VERSION_1`) is preferred;
the legacy I/O-port interface is used when it is all the device has. One request is in flight at a time on a single
virtqueue (header, data segments, status), completion is polled with a 10 s timeout (a device that stops answering is reset
so it cannot DMA into freed memory, and then reports `EIO`). Negotiated: `BLK_SIZE` (logical sector size 512..4096; the
LBA is scaled because the wire protocol counts 512-byte units), `FLUSH`, `RO`, `SIZE_MAX`, `SEG_MAX`. Disks are named
`vda`, `vdb`, ... and show up in `lsblk`, `/proc/partitions`, `/proc/pci` and `/dev`.

## df and statfs

`SYS_statfs` (67): `statfs(path, struct statfs *)`, struct layout in `shared/abi.h` (`struct gstatfs`), libc `sys/statfs.h`.
Each filesystem type may provide `FsType.statfs`; tmpfs reports the shared RAM pool, FAT the cluster counts, procfs zeros.

```
$ df -hT
Keine's ledger of the lands -- how much room each realm has left
Filesystem         Type          Size       Used      Avail Use% Mounted on
gensokyo           tmpfs         251M       504K       251M   1% /
/dev/vda2          fat32         149M       4.9M       145M   4% /HumanVillage
```

Flags: `-h` human sizes, `-k` 1K blocks (default), `-T` type column, `-a` also empty pseudo filesystems, `-P` no banner
(the banner is only printed on a terminal); paths select the filesystem that holds them. `/proc/mounts` shows `ro`/`rw`.

## Building and inspecting disk images (`tools/mkdisk.py`)

Pure Python, no external programs.

```
python tools/mkdisk.py disk.img --size 512M --esp build/esp --hv-size 256M            # ESP + HUMANVILLAGE
python tools/mkdisk.py disk.img --size 300M --no-esp --hv-dir some/files              # data disk, pre-populated
python tools/mkdisk.py disk.img --size 700M --sector-size 4096                        # 4Kn (FAT32 needs >= 256 MiB per volume)
python tools/mkdisk.py disk.img --size 128M --table mbr --no-esp --hv-fat16           # MBR, FAT16
python tools/mkdisk.py disk.img --size 200M --extra DECOY:40M:fat32                   # more partitions (fat32|fat16|raw)

python tools/mkdisk.py --check disk.img        # GPT + FAT fsck: FAT copies, FSInfo, LFN checksums, chains, cross-links,
                                               # lost clusters, ".", "..", duplicate names, dirty flags
python tools/mkdisk.py --ls disk.img:2 /reimu  # also --cat, --extract IMAGE:N PATH DEST, --tree
```

`python tools/qemu.py run` attaches `build/disk.img` automatically when it exists (`Qemu(disk=path)` in tests).
**Note:** OVMF boots the first bootable ESP it finds, so a data disk whose ESP contains `EFI/BOOT/BOOTX64.EFI` will be
booted instead of `build/esp`; tests therefore use ESPs without it.

### A bootable image, and a USB stick

```
python tools/build.py
python tools/mkdisk.py build/touhouos.img --size 512M --esp build/esp --hv-size 256M
```

This writes a GPT disk with a 64 MiB FAT32 EFI System Partition (a copy of `build/esp`: `EFI/BOOT/BOOTX64.EFI`, `kernel.elf`,
`initrd.img`, `cmdline.txt`) and a FAT32 partition named and labelled HUMANVILLAGE that takes the rest, then runs the checker
on the result. The image was booted from that file alone in QEMU/OVMF (`-drive if=none,id=d0,format=raw,file=build/touhouos.img
-device virtio-blk-pci,drive=d0`, no `fat:` drive): UEFI starts `EFI/BOOT/BOOTX64.EFI` through the removable-media path, and the
kernel logs `fat32: vda2 mounted on /HumanVillage` and creates `reimu`, `marisa` and `sakuya`.

* `build/esp/cmdline.txt` is rewritten by every `tools/qemu.py` and test run: look at it (or empty it) before building an image
  for real hardware.
* Do not name a bootable image `build/disk.img`: `tools/qemu.py run` attaches that file as a data disk, and OVMF may boot the
  (older) kernel stored in its ESP instead of `build/esp`. To test persistence use a data-only disk:
  `python tools/mkdisk.py build/disk.img --size 256M --no-esp`.
* On a USB stick: write the image with a raw-image writer (`dd`, Rufus in DD mode, balenaEtcher); the whole stick is replaced.
  Secure Boot must be off (the loader is unsigned). The backup GPT sits at the end of the image, not of the stick, so disk tools
  may offer to fix it; that is harmless. HUMANVILLAGE is an ordinary FAT32 volume: Windows, Linux and macOS can open it to copy
  files in and out (eject it safely first, otherwise TouhouOS finds the dirty flag and mounts it read-only). TouhouOS mounts
  only that volume, wherever it sits, and nothing else on the machine.
* The stick the OS booted from is a disk inside the OS too (USB mass storage, see USB.md): `sda` or `sdb` with `sdX1` (the ESP, never
  mounted) and `sdX2` (HUMANVILLAGE, mounted on `/HumanVillage`), so the homes persist. `blk.ro` does not apply to a stick (it is for the
  internal disks); a write-protected stick is read-only by its own switch. Once the volume is mounted read-write the kernel log is kept
  on it as `TOUHOU.LOG` (and `TOUHOU.PRV` for the boot before): see REAL_HARDWARE.md.

The checker is deliberately independent of the kernel (it shares no code with it), and a self-test makes sure it notices
loops, holes, leaked clusters, FAT mirror mismatches, wrong LFN checksums, wrong FSInfo and broken GPT CRCs. Images written
by the kernel were also mounted read-only with Linux's vfat driver and compared with `diff -r`.

## Tests

`python tests/run_all.py usbstor` runs the tests of HUMANVILLAGE on a USB stick (`tests/case_usbstorage.py`, listed in USB.md): automount
before the first session, persistence over two boots, hot-plug, the first-found rule, time stamps, the kernel log on the stick.

`python tests/run_all.py storage` (about five minutes) - see `tests/case_storage.py`. `/bin/fstest` (user/bin/fstest.c) is a
system-call level checker that the tests run inside the guest (errno values, open-then-unlink, renames over files and
directories, 255-character and Unicode names, holes, cluster-boundary sizes, 700-entry directories); you can run it on any
mounted volume (`fstest /HumanVillage/reimu`). Besides the scenarios listed in the task the suite covers: both virtio transports, 512 and 4096-byte sectors, 512 B and 4 KiB clusters, FAT16 root-directory
exhaustion, a write-protected disk, power loss (unflushed write leaves the dirty flag), periodic flush without `sync`,
nine different corrupt boot sectors, damaged chains and directories, and a seeded random stress run (create, append,
overwrite, truncate, delete, rename over files, move directories, mkdir/rmdir) mirrored in a Python model whose final tree
must match the disk byte for byte.
