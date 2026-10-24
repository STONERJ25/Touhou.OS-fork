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
8. **A HUMANVILLAGE volume that was not shut down cleanly is gone through before it is mounted** (read-only, no VFS lock held; the section
   "Dirty volumes" below): nothing out of place -> mounted read-write (`fsck=ok` / `fsck=left`), otherwise read-only with the precise reason
   in the log and in `/proc/fatcheck`. `hvforce` on the kernel command line mounts read-write whatever the check says (dangerous);
   `hvcheck=off` skips the check, and a dirty volume is then read-only as it always was.

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

An unclean pull leaves the volume flagged dirty. The next mount goes through the ledger first (see "Dirty volumes"): if nothing is out of
place the volume is mounted read-write and the flag is taken over (cleared again at the first idle flush); if something is, it stays
read-only until `fsck` has been through it (the policy never uses `-o force` by itself; `hvforce` is the owner's decision). Eject
(`umount`, `poweroff`, `sync`) before pulling anyway: the check costs a second or more, and every unclean pull is a gamble on the write order.

Decisions are logged with `klog("Keine", ...)`, one line per mounted volume:

```
[Keine] fat32: vda2 mounted on /HumanVillage, 126 MiB, 512-byte clusters, label 'HUMANVILLAG', home directories per user
[Keine] no HUMANVILLAGE volume found: homes stay in RAM (tmpfs)
[Keine] fat32: vda2 mounted on /HumanVillage, ..., read-only: the volume was not cleanly unmounted (mount with -o force to override)
[Keine] vda2 was not closed properly: Keine goes through the whole ledger, read-only, before anyone writes in it
[Keine] vda2 was left untidy (the power went out before it was closed), but Keine went through the whole ledger in 114 ms (48 files in 10 directories) and nothing is out of place: the Human Village may write in it again
[Keine] vda2 was left untidy and its ledger does not add up: 2 error(s). It stays read-only so that nothing gets worse. As root, `umount /HumanVillage` and run `fsck -a /dev/vda2` (see /proc/fatcheck)
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
| `fsck=ok`, `fsck=left` | set by the mount policy, not meant for typing: the caller has just gone through the volume with the read-only consistency check and found nothing out of place (`ok`: not even leftovers, so the dirty flag is cleared again at the first idle flush; `left`: only harmless leftovers, so it stays until `fsck`). Like `force` it lets a dirty volume be mounted read-write, and it also allows a file whose chain is longer than its size to be trimmed when it is next written |
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
  flagged dirty (or with the "disk errors seen" bit) mounts **read-only** with a warning unless `-o force` is given - or, for the
  HUMANVILLAGE volume, unless the policy's consistency check found nothing out of place (`fsck=ok|left`, "Dirty volumes").
* **Write ordering.** The cache flushes file data first, then the FAT, then directory entries/FSInfo/boot sector, with a
  device cache flush between the classes. A power cut can therefore leak clusters (fsck reclaims them) but not leave
  an entry pointing at data that was never written. Unlinking while open keeps the clusters until the last close.
  **Known gaps in that claim** (found by reading the driver, not yet changed - "State when paused" below): the classes are data (0), FAT
  (1) and metadata (2), and a page takes the class of its last writer. Freeing a chain therefore writes the FAT (class 1) *before* the
  directory entry that stops referring to it (class 2), so a cut between the two leaves an entry pointing at free clusters; a new
  directory's cluster (zeros and `.`/`..`) is class 2 like the entries that make it visible, and the order inside a class is by disk
  offset; a directory that grows links its new cluster in the FAT (class 1) before that cluster is zeroed on disk (class 2); a rename
  writes the new entry and the removal of the old one in the same class (a cut between them leaves two names for one chain).
* **Periodic and explicit flushing.** A kernel thread (`fat-flush`) writes back dirty pages older than 3 s; `sync(2)`
  (the `sync` applet, `dd`, `reboot`, `poweroff`) flushes every writable filesystem; a large backlog is flushed by the
  writer itself.
* **No overlap.** A device cannot be mounted twice, nor can a disk and one of its partitions.
* Everything is behind one mutex per volume; the vnode release path is lock-free and defers cleanup to the next locked
  operation.

## Dirty volumes: the ledger check, and fsck

The owner will often switch the machine off with the button after a hang, and the next boot used to mount the stick read-only (and could not even
write `TOUHOU.LOG`), exactly when the log is needed. So a dirty HUMANVILLAGE volume is now *looked at* before the decision is made.

```
 shared/fatck.[ch]    the scanner, freestanding C: the same source runs in the kernel, in /bin/fsck and (as a DLL) in the host tests
 shared/fatfix.[ch]   the repair engine (user space only; an empty file in the kernel build)
 kernel/fs/fat_check.c   kernel adapter: block device in, text out, /proc/fatcheck;  kernel/include/fatcheck.h has the contract
 kernel/fs/mountpolicy.c check_before_mount(): runs it, adds "fsck=ok|left" or "force" to the mount options
 user/bin/fsck/       the command line (fsck, fsck.fat, fsck.vfat)                  initrd/.../patchouli/8/fsck.pw
 tools/fatck_host.py  builds shared/*.c into a host DLL for the tests                tools/fatck_host_rt.c is its little C runtime
```

**The check** (`fat_check_device()`): reads the boot sector and FAT[0..1]; a volume whose flags are clear is not scanned (verdict 0). Otherwise the
scan runs in the automount thread, before the VFS mount and with no lock held (a stick can take seconds), through a session that has no write callback
at all. It compares the FAT copies, walks every directory iteratively (explicit stack, depth limit 256, a 2 MiB arena for duplicate-name tables),
claims the clusters of every chain in a one-bit-per-cluster bitmap (a cluster claimed twice is a cross-link or a loop), looks at the FAT once from end to
end for allocated clusters nobody owns, and checks FSInfo. Memory: the bitmap, 4 MiB of 16 KiB cache windows (sequential read-ahead of 64 KiB), one
cluster, the arena. Time: one pass over the FAT, one read of every directory, and a deadline of 120 s; a volume the scan cannot finish (time, memory,
nesting, a failing device) is *unverified* and stays read-only. Every cluster number and size read from the volume is validated before it is used.

Findings come in four severities (`FS_ERR`, `FS_LEAK`, `FS_WARN`, `FS_NOTE`; `fatck.h`):

| Severity | Means | Examples |
|----------|-------|----------|
| error | damage: data loss or storage that two things claim | a chain that runs into a free, bad or reserved cluster or leaves the volume; a loop or a cross-link; a size beyond its chain; an entry pointing outside the volume; wrong `.` / `..`; broken long-name sets; a directory with a size; duplicate names; a FAT copy that differs or FSInfo that is wrong *on a volume that is flagged clean* |
| leftover | what a power cut leaves on a volume that was being written to; harmless to write on | lost clusters; a chain longer than the file size says; bytes after a directory's end marker; a FAT copy or the FSInfo count that lags behind *on a dirty volume*; an empty file that owns clusters |
| warning | cosmetic | lower-case letters in an 8.3 name, odd attribute or NT bits, an impossible date, the dirty flag itself, the media byte, the backup boot sector |

The verdict decides the mount: **no error and no leftover** -> read-write, `fsck=ok`, the dirty flag is taken over and cleared at the first idle flush (the log
says `... nothing is out of place: the Human Village may write in it again`); **no error, leftovers** (or the disk-error flag) -> read-write, `fsck=left`, the
flag stays until `fsck -a` has tidied them (a file whose chain is longer than its size is trimmed when it is next written - only on such a verified mount; on any
other volume it stays read-only for the boot, as before); **any error, or no verdict** -> read-only, the first four problems are in the log with the fix
(`umount /HumanVillage`, `fsck -a /dev/sdb2`, reboot) and all of them in `/proc/fatcheck`:

```
sdb2: FAT32, flagged dirty: DAMAGED (114 ms, 48 files in 10 directories, 126758 free clusters, 137 lost)
  2 error(s), 2 harmless leftover(s), 2 warning(s)
  error:    /reimu/hello.txt: chain runs into a free cluster (cluster 37, after 3 cluster(s))
  error:    /marisa/spell.dat: chain is cross-linked or loops back (the cluster is already part of a chain) (cluster 35, after 1 cluster(s))
  leftover: 137 lost cluster(s) (allocated in the FAT but part of no file), first: 23, 38, 39, 40, 41, 42, 43, 44
  fix: as root, `umount /HumanVillage`, `fsck -a /dev/sdb2` (`fsck -y` also repairs what needs a decision), then reboot
```

`hvforce` on the kernel command line mounts read-write whatever the check says (and says so; the dirty flag stays set; "dangerous" in
REAL_HARDWARE.md). `hvcheck=off` skips the check: a dirty volume is read-only as before, and with `hvforce` it is forced as before. A device that is
read-only anyway is not checked. Volumes mounted by hand are not checked: `mount` still needs `-o force`.

**fsck** (`fsck [-n|-a|-p|-y] [-F] [-v] [-q] DEVICE`, section 8 of the manual): refuses a device (or a partition of it) that is listed in
`/proc/mounts`, refuses anything that is not FAT12/16/32, needs root. It scans, prints the findings, and (unless `-n`) runs `fatfix_run()`: passes of
*scan, repair what the findings allow, flush*, until a pass repairs nothing; lost clusters last, once nothing else changes what counts as lost. Every repair
re-reads the directory slot it is about to change and skips it if the scan's view is stale. `-a` makes the repairs that lose no readable data and no names
(FAT copies, FSInfo, media byte, backup boot sector, size beyond chain, chain beyond size, bytes after a directory's end, orphaned long-name entries, `.`/`..`,
cross-linked files get a copy of the shared clusters, lost chains become `FOUND.000/FILEnnnn.CHK`); `-y` also cuts broken chains, removes entries that point
nowhere, repeat a name or are stray labels, and mends bad 8.3 names; without either it asks per repair on a terminal. `-F` frees lost chains instead of saving
them. The dirty flag (and FSInfo) is touched only when the last scan finds no error and no leftover. Exit status: 0 nothing wrong, 1 corrected, 4 problems
left, 8 could not do it, 16 usage. Boot-sector damage is never repaired (the checker refuses such a volume).

**Held against the Python checker.** `tools/mkdisk.py`'s `FatReader.check()` is independent of the C code; `tests/case_fsck.py` makes the two agree on
about 50 kinds of deliberate damage per FAT size (FAT12/16/32) and on 240 random mutations per run (3000+ were run while developing, no difference left).
To make them agree the Python side learned FAT12, the active FAT of a FAT32 volume with mirroring off, a `benign` class next to `errors` (`Report.serious`
is `errors` minus `benign`: the leftovers above), invalid dates as warnings, and a FAT32/FAT16 layout that contradicts the cluster count as an error.


* FAT32 and FAT16 (the QEMU `fat:` ESP is FAT16) read/write. **FAT12, exFAT and NTFS are not mounted** (the checker and `fsck` do read
  and mend FAT12).
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
* There is no journal. After a crash the dirty flag makes the HUMANVILLAGE volume go through the ledger check; it is mounted read-write
  when that finds nothing out of place and stays read-only otherwise, until `fsck` (here, or on Windows/Linux) has mended it
  (`python tools/mkdisk.py --check image` is the host-side look). A volume mounted by hand with `mount` that is flagged dirty is read-only
  unless `-o force` says otherwise, at your own risk.
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

`python tests/run_all.py fsck` (`tests/case_fsck.py`): `fsck_c_*` need no VM (they build `shared/fatck.c` into a host DLL, `tools/fatck_host.py`, and hold it
against the Python checker over deliberately damaged images, 20 seconds); `fsck_kernel_*` boot a VM with a dirty volume: a flag and nothing else wrong
(read-write, the log reaches the stick), harmless leftovers (read-write, surplus chain trimmed on the next write, flag stays), real damage (read-only, the
log and `/proc/fatcheck` say what, the image is untouched), `hvforce` and `hvcheck=off`, and the limits (nesting beyond 256, a 12 GiB volume with 3 million
clusters). Tests that used to expect "dirty means read-only" (`storage_dirty_volume_readonly`, `storage_unclean_power_loss`, the bootlog test) run with
`hvcheck=off`; `usbstor_hotplug_idle` now expects the check and a writable mount.

## State when paused

The "dirty-flag trap" work (a dirty HUMANVILLAGE volume is checked instead of refused; a repair tool; a safer write order) was paused part-way. What is what:

**Done and tested**
* The checker, `shared/fatck.c`/`.h`, FAT12/16/32. `fsck_c_agrees_with_python` and `fsck_c_agrees_on_random_damage` passed against the Python checker; the
  engine was extended afterwards (retained findings, repeatable scans, the last good cluster in a finding, long-name/duplicate/dots details for the repair code) and
  exercised again by the repair scripts below with the same verdicts, but those two tests were not re-run on the final sources.
* The kernel side: `fat_check.c`, `check_before_mount()` in the policy, the mount options `fsck=ok|left`, `hvforce`, `hvcheck=off`, `/proc/fatcheck`, the trim of
  a surplus chain on verified volumes. `fsck_kernel_tidy`, `_leftovers`, `_damage`, `_force_and_off`, `_limits` passed, and so did the four adapted old tests
  (`storage_dirty_volume_readonly`, `storage_unclean_power_loss`, `usbstor_hotplug_idle`, `usbstor_bootlog_never_when_unsafe`) - on the sources of commit
  `bf52a4d`; the later engine edits are in the kernel build (it builds) but those tests were not re-run, and the big suites (`storage`, `usbstor`, `usb_`, `blk_`)
  were not re-run after this work at all.
* `tools/mkdisk.py`: FAT12 images, `Report.benign`/`serious`, the active FAT of FAT32; `tools/fatck_host.py` (host DLL) and `tools/fatck_host_rt.c`.

**Written, builds, only partly tested**
* The repair engine `shared/fatfix.c`/`.h`. Run through the host DLL by scratch scripts (not committed) over the whole damage menu of `tests/case_fsck.py` on FAT12,
  FAT16 and FAT32: with `-y` (FIX_ALL) every damage except a broken boot sector ends clean according to both checkers; with `-a` (FIX_SAFE) exactly the damage that
  needs a decision stays (cut chains, entries pointing nowhere, duplicates, bad names, strays), the rest is mended. There are no committed repair tests yet, and no
  check yet that undamaged files keep their bytes or that a lost chain is saved with the right contents.
* `/bin/fsck` (`user/bin/fsck/`, `fsck.fat`/`fsck.vfat` links in `tools/build.py` `EXTRA_LINKS`) and its manual page `fsck.pw`: they compile and link, and have **never
  been run in the guest**. Untested assumptions: `lseek(SEEK_END)` on a `/dev` node gives the size, writes plus `ioctl(BLKFLSBUF)` on a node, the `/proc/mounts`
  check, the interactive prompts, the exit codes, and `patchouli --check-all` (the `manual` test) accepting `fsck.pw`. The kernel's advice (`fsck -a /dev/sdb2`) points at it.

**Not started**
* Any change to the FAT write path (item 4 of the task): no code changed, so nothing half-finished is enabled. The review is done and is in "Safety design" ("Known
  gaps") and below. The random-power-cut test (kill the VM at random moments of a write-heavy workload, run the host checker) is not written.
* `docs/REAL_HARDWARE.md` has the short version of what the owner does after a bad shutdown; the full walk-through with a real stick is not tried on hardware.

## How to resume

1. `python tools/build.py`, then `python tests/run_all.py fsck_c` (no VM) and `python tests/run_all.py fsck_kernel` (about two minutes) to see the starting point; then the
   suites this work touched: `storage`, `usbstor`, `usb_`, `blk_`, `power_,manual`.
2. Put the repairs under test (`tests/case_fsck.py`): the host-level loop is `tools/fatck_host.py` `fix_volume(path, index, mode)` (see `-a`/`-y` in its `main`); for each entry
   of `damage_menu()` repair with `FIX_ALL` and `FIX_SAFE`, then require `py_class`/`c_class` to say ok/warn (only boot-sector damage may stay `err`) and compare
   the file tree before and after (`FatReader.snapshot()`) apart from the files the damage was aimed at; a file that lost its directory entry must come back as
   `FOUND.000/FILE0000.CHK` with its exact bytes. Fix whatever that finds in `shared/fatfix.c`.
3. Run `/bin/fsck` in a VM (`fsck_cmd_*`): an image as a second virtio disk (`vdb1`), `fsck -n`, `-a`, `-y` on the damage cases with the host checker looking at the image after
   the guest has shut down; the refusals (mounted -> exit 8, not FAT -> 8, non-root -> 8); a FAT12 image; `-F`; the flow the kernel message promises
   (`umount /HumanVillage; fsck -a /dev/vda1`, then `reboot`, then the volume is clean and writable). Expect the first guest run to find libc/devfs surprises (see above).
   Then `python tests/run_all.py manual` for the page.
4. The write order (item 4). The design that came out of the review, in order of value:
   * *Classes with a barrier on conflict.* Make the classes data (0), FAT allocations (1), entry creation/updates (2), FAT frees and chain cuts (3) and keep the rule
     "a page is written at its class's turn"; when a write would give a *dirty* page a different class (`fc_write`/`fc_zero` in `fat_cache.c`, before the `memcpy`),
     first write everything of class <= the page's old class and flush the device ("write the old state of the page first"), then change the page. `fat_put()` takes the class
     (alloc/link = 1, free and the EOC of a shrink = 3; the rollback paths after a failed allocation stay 1). Effects: a deletion's entry removal (2) now precedes the FAT free (3);
     a cluster freed in this epoch cannot be reused before the free is durable (the allocation would touch the same FAT page and force the flush).
   * *New directory clusters are data* (zeroing and `.`/`..` in `fat_mkdir`, `fatd_write_dots`, `chain_grow` for directories): they must be on disk before the FAT link and the
     parent's entry. Creating an entry in such a page changes its class 0 -> 2 and costs one cheap barrier.
   * *Rename without two names for one chain*: remove the source entry (and fix `..` of a moved directory), barrier, then add the new entry; when the destination exists and
     has the same spelling, rewrite its short entry in place instead. A cut then leaves lost clusters (fsck saves them), never a cross-link.
   * *Append*: data -> FAT -> size is already the order; a cut between the FAT and the size leaves a chain longer than the size, which the checker classes as a harmless leftover
     and a verified mount trims.
   * *Test*: boot with a data disk, run a write-heavy loop (create/append/delete/rename/mkdir/rmdir/truncate), `vm.qmp("quit")` at a random moment (50 times), then
     `mkdisk.check_image(path).serious` must be empty (lost clusters, chains longer than the size, stale FSInfo/FAT copy 2 are the allowed leftovers), and `fatck_host.scan_volume`
     must say `errors == 0`. QEMU keeps every acknowledged write when its process is killed, so this tests the order of the requests, not the device cache; the flushes between the
     classes are what protects against the device cache on real hardware.
   Measure the cost on `storage` (many small files, delete/create loops): the barrier flushes the dirty pages of lower classes.
5. `docs/REAL_HARDWARE.md`: section 5 (eject / dirty) now describes the check; add what the owner does when the log says the ledger does not add up, once `fsck` has been seen working.
