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

`tools/mkdisk.py` builds the reference layout (`install` writes the same onto a disk of the running system, see [INSTALL.md](INSTALL.md)):

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
| `espgrant` | take the ESP write grant the caller holds (root, `BLKESPGRANT` first): the ESP the system started from mounts read-write although `blk.ro` is set, until the unmount or until the grant's file is closed; refused on a volume flagged dirty unless `force` ([INSTALL.md](INSTALL.md), `espgrant(1)`) |
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
* **A directory named `.system` at the top of any FAT volume**, and everything below it, belongs to root with directories 0700 and
  files 0600, whatever `uid=`/`fmask=` say and with or without `userdirs` (also when something is renamed to `.system`). It holds the
  `/etc` files kept across restarts ([PERSISTENT_ETC.md](PERSISTENT_ETC.md)), the shadow file among them.
* Root can hand a mounted `userdirs` volume the residents of `/etc/passwd` (uid 1000 to 59999, at most 32) with
  `ioctl(fd, FATIOC_USERS_RELOAD)` on any of its directories (`shared/abi.h`); `init`, `adduser` and `deluser` do, so a home made
  after boot belongs to its resident at once.

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
* **Write ordering.** The cache writes its dirty pages in six classes (file data, new FAT entries, links, directory
  entries, chain cuts, frees) with a device cache flush after each, and a dirty page that changes class is written with its
  old contents first. A power cut at any moment leaves at worst lost clusters (and the other harmless leftovers listed in
  "Write ordering" below), never a cross-link, an entry pointing at free or foreign clusters, a size that exposes stale
  bytes or a broken directory. Unlinking while open keeps the clusters until the last close. `tests/case_storagecrash.py`
  pulls the plug at random moments to hold the kernel to that. A flush that the order needs and the device refuses fails the
  operation that needed it; a file write whose directory entry was refused keeps the update pending until it can be written
  ("A refused entry update" below).
* **Rename over an existing file keeps the name.** The target's entry is rewritten in place (when the entry sets have the same size,
  as for the same name) after the source's old entry is gone from the disk, and the replaced file's clusters are freed only after that
  (`fatd_replace_file()`, a flush before and after the in-place write): a power cut leaves the old or the new file under the name, at
  worst with lost clusters. The ESP update relies on it ([UPDATE.md](UPDATE.md), "Writing the ESP"; test `espgrant_power_cut`).
* **The ESP write grant.** A FAT volume mounted with `-o espgrant` writes through `blk_write_as()` as the holder of the grant on its
  partition; when the grant ends while it is mounted it is written back and turns read-only ([INSTALL.md](INSTALL.md)).
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
`/proc/mounts` (or a partition of a disk that is, or a disk of a partition that is; `vda10` is not a partition of `vda1`), refuses anything that is not
FAT12/16/32, needs root. It scans, prints the findings, and (unless `-n`) runs `fatfix_run()`: passes of *scan, repair what the findings allow, flush*,
until a pass repairs nothing (at most 16); lost clusters last, once nothing else changes what counts as lost. Every repair re-reads the directory slot it is
about to change and skips it if the scan's view is stale. Within a pass the findings are taken in stages, because a repair can make the later findings
wrong: the FAT copies; **cross-links** (the second claimant gets a private copy of what it shares before anything is written into either chain or freed
from it); `.` and `..`; repeated names; everything else. A stage that changed something ends the pass, so the next one starts from a fresh scan; the
clusters of a chain that another entry has run into are never freed until the cross-link is separated. `-a` makes the repairs that lose no readable data
and no file (FAT copies, FSInfo, media byte, backup boot sector, size beyond chain, chain beyond size, a chain that breaks behind everything its file's size
needs, bytes after a directory's end, long-name entries that belong to no file, `.`/`..`, an entry that is an exact copy of another, cross-linked files get
a copy of the shared clusters, lost chains become `FOUND.000/FILEnnnn.CHK`; a long name that does not match its short entry is dropped and the file stays
under the short name); `-y` also cuts broken
chains, removes entries that point nowhere, repeat a name, stand where `.` and `..` belong or are stray labels, and mends bad 8.3 names; without either it
asks per repair on a terminal (a declined repair is not asked again). `-F` frees lost chains instead of saving them (without it, a volume with more than
10000 lost chains or no room for `FOUND.000` fails that one repair, not the run). The dirty flag (and FSInfo) is touched only when the last scan finds no
error and no leftover; an FSInfo sector is only rewritten when it is one already or empty (a damaged FSInfo pointer in the boot sector can name the backup
boot sector). Exit status: 0 nothing wrong, 1 corrected, 4 problems left, 8 could not do it, 16 usage, 32 stopped at a question. Boot-sector damage is never
repaired (the checker refuses such a volume).

**Held against the Python checker.** `tools/mkdisk.py`'s `FatReader.check()` is independent of the C code; `tests/case_fsck.py` makes the two agree on
about 50 kinds of deliberate damage per FAT size (FAT12/16/32) and on 240 random mutations per run (3000+ were run while developing, no difference left).
To make them agree the Python side learned FAT12, the active FAT of a FAT32 volume with mirroring off, a `benign` class next to `errors` (`Report.serious`
is `errors` minus `benign`: the leftovers above), invalid dates as warnings, and a FAT32/FAT16 layout that contradicts the cluster count as an error.

**Held against the repair tests.** `fsck_c_repair_*` run the repair engine in the host DLL (no VM) over every damage of the menu on FAT12, FAT16 and FAT32
(and a 4096-byte-sector volume, where the host session insists on whole-sector reads and writes) with `-y` and with `-a`, and require: both checkers say
ok or warn afterwards and the dirty flags are clear; every file that was not aimed at is byte for byte what it was; a file that lost its entry or its clusters
is saved in `FOUND.000` with its exact bytes (a cut chain: the start in the file, the rest in a `.CHK` file); `-a` leaves exactly the damage that needs a
decision (cut chains, entries that point nowhere, repeated or unusable names, stray labels, cross-linked directories); a second run changes nothing. A fuzz
(`TOUHOU_FSCK_FUZZ=N`, 400 by default; 3000 `-y` and 1500 `-a` cases were run clean) damages FAT bytes and entries, directory slots, takes another file's first
cluster for an entry, sets wrong sizes, and requires the same plus that every file the damage left readable survives. What these tests found, and what is
fixed: trimming a file whose chain was cross-linked gave back clusters that the other file still read (a 70 KB file cut at 1.5 KB); the second claimant of a
shared cluster (the innocent one, whichever the scan met second) was deleted instead of copied; erased `.`/`..` cost the directory's files their names; an
exact copy of an entry needed `-y`; a damaged FSInfo pointer made the repair write over the backup boot sector. `fsck_c_repair_interrupted` (a power cut in
the middle of `fsck`, see "State when paused") added: the link of a cross-linked file's copy could reach the disk before the copy; and a chain that breaks behind
everything its file needs (what an interrupted trim leaves) is cut by `-a`, not only by `-y`.


## Write ordering

FAT has no journal, so what a power cut leaves behind depends only on the order in which the cache's writes reach the
device. `kernel/fs/fat_cache.c` keeps one flush class per dirty 4 KiB page and writes the classes in this order, each in
disk order, with a device cache flush (`blk_flush`) after each class:

| # | Class | What goes in it | Why it waits for the classes before it |
|---|-------|-----------------|-----------------------------------------|
| 0 | DATA  | file data; the zeros of new clusters; a new directory's cluster with its `.` and `..`; `0xE5` fillers and the zeroing of garbage past a directory's end marker | nothing points at any of it yet |
| 1 | ALLOC | FAT entries of newly allocated clusters (their chain among themselves and the end mark) | still nothing points at them: a cut leaves lost clusters |
| 2 | LINK  | the FAT entry that hooks new clusters onto a chain in use (an append, a growing directory), when the new entries are not all in the link's own 512-byte sector | a link that lands before the entries it points at makes a chain run into free clusters, which a later allocation hands to another file |
| 3 | META  | directory entries (create, size and first cluster, rename, delete), FSInfo, the boot-sector flag | an entry may only point at clusters whose data, FAT entries and links are on the disk |
| 4 | CUT   | the end mark that shortens a chain (truncate to a smaller size) | the entry with the smaller size goes first, so a cut leaves a chain longer than the size, never a size the chain cannot hold |
| 5 | FREE  | FAT entries set to 0 (delete, rmdir, rename over a file, truncate) | the entry that pointed at the clusters is gone first (and the cut is in), so a cut leaves lost clusters, never an entry pointing at free clusters |

Three rules make the classes hold across operations and at any moment, not only at the end of a quiet `sync`:

* **A page holds one class.** A write of another class into a dirty page first writes everything dirty of classes up to
  the page's old class and flushes the device (`pg_reclass`): "write the old state of the page first". So a cluster that a
  delete freed is not handed to a new file before the delete is durable (the allocation touches the same FAT page and
  forces the flush), and a new directory cluster is zeroed on the disk before the first entry lands in it.
* **Program order follows the dependencies.** Every operation changes what others depend on first (data before the FAT
  before the entry; the entry with the smaller size before the cut; the removed entry before the frees), so a flush that
  starts at any point - the 3 s flusher, the dirty-limit throttle, an eviction when the cache is full, a barrier - writes
  a consistent state.
* **The atomic unit is a 512-byte sector** (`FAT_ATOMIC`), the smallest a device writes in one piece. A directory entry set
  of up to 16 slots (names up to 195 characters) never straddles one: `dir_reserve()` starts it in the next sector and
  fills the rest of the current one with `0xE5` slots, which are durable before the set (a barrier). A link whose new
  entries all sit in its own sector rides in ALLOC with them; otherwise the new entries are written first.

**Renames** are three steps with a barrier (classes 0..3 written and flushed) between them: (1) the old entry set goes,
together with a replaced entry that is spelled differently (two names that differ only in case must never be on the disk
together); (2) the `..` of a directory that changes parent; (3) the new entry. A cut in between leaves the file without
a name - its clusters are lost clusters, which `fsck` saves as `FOUND.000/FILEnnnn.CHK` - but never two names for one chain
(deleting one of them later would free the other's clusters). When a rename replaces a file whose name is spelled exactly
like the new one (an editor saving `notes.txt` through `notes.tmp`), step 3 rewrites that file's short entry in place to
point at the new clusters instead, so a cut leaves either the old file or the new one under the name, and the old
clusters are freed after it.

**What a power cut may leave** (and `tests/case_storagecrash.py` accepts): lost clusters (a file being created or
extended, a delete whose frees did not land, a file or a whole directory between the steps of a rename); a chain longer
than its file's size (an append whose size did not land; a truncate whose cut did not); FAT copy 2 behind copy 1 (copy 1
is written first within a class); a stale FSInfo free count; the dirty flags. On `main` a file whose chain is longer than
its size is read-only for the boot until a checker has tidied it (with the `fsck` work, a checked mount trims it).

**The four gaps** the review of the old order found ("data, FAT, metadata"; a page took the class of its last writer, and
the order inside a class was by disk offset), and what closed them:

1. *Freeing a chain wrote the FAT before the entry that stopped pointing at it* (a cut between them left an entry pointing
   at free clusters; a truncate left a size beyond its chain): frees are FREE and cuts CUT, both after META, and a
   truncate writes the smaller size before it cuts.
2. *A new directory's cluster (zeros, `.`, `..`) had the class of the entry that makes it visible* (by disk offset the
   entry could land first and point at a cluster of stale bytes): the cluster is DATA.
3. *A growing directory was linked in the FAT before its new cluster was zeroed*: the cluster is zeroed as DATA, and the
   link waits for it and for the new FAT entries (LINK, or ALLOC within one sector).
4. *A rename wrote the new entry and the removal of the old one in the same class* (two names for one chain): the three
   steps above.

Found and closed on the way: an append's link (old tail -> new cluster) and the new clusters' own FAT entries were in the
same class in different pages (a chain into free clusters: the LINK class); a truncate's cut and its frees likewise (the
CUT class); entry sets straddling a sector or a page (half a long name: the sector rule); zeroing garbage past a
directory's end in the class of the entries written behind it (DATA now).

**A refused entry update.** The order can need a flush in the middle of an operation: a page that changes class first goes out
with its old contents. With clusters smaller than a cache page (512 B to 2 KiB) the root directory, small files and the start
of a new file share a page, so the update of a file's entry after its data flushes the data first, and the device may refuse
(a failing disk). The operations that hand their result to the caller (create, mkdir, chmod, a truncate down, and unlink, rmdir
and the first removal of a rename, whose entry removal is written before anything is freed or moved) fail with the error and
have changed nothing in the cache: an unlink that could not write the removal of the entry must not free the clusters, or the
entry on the disk would point at free clusters. (The barriers between the steps of a rename are not stopped half-way when the
device refuses one of them: the error is remembered, and for that rename the order between its steps is no longer
guaranteed. Not covered.) A file write has copied its data into the cache by then and cannot
undo it: it returns the bytes written, and the file's node is marked `stale` (the entry on the disk is older than the node).
`vol_sync` (sync, fsync, umount, the 3 s flusher) writes every other dirty page first and the stale entries after them, so
the order holds; while the device keeps refusing, `sync` and `umount` fail and the data stays in memory; once it works the
entry follows and the volume is clean with the file complete (`blk_err_fat_writeback`). Before, the refusal was dropped, and
the volume unmounted cleanly with the file empty and its clusters lost. A truncate that cannot write its smaller entry returns
the error and leaves the clusters in the chain (the cut and the frees must come after the entry), which is then longer than
the size until the entry is written. A power cut with an entry still stale leaves what any cut leaves: lost clusters, or a
chain longer than the size.

**Not covered:** names longer than 195 characters (17 to 21 slots cannot fit one sector), and entry sets that another
system wrote across a sector boundary, can still be cut in half (long-name fragments without their short entry, which
`fsck` removes); the bytes inside a file are not written atomically (an overwrite in place can leave a mix of old and new
data, never another file's); a device that acknowledges a cache flush it did not do defeats all of this. In QEMU the test
checks the order of the requests (QEMU keeps every acknowledged write when it is killed); the device cache flushes
between the classes are what protects a real stick.

**Cost:** the barriers are extra device flushes; the unmount log line counts them (`unmounted cleanly (N extra flushes for the
write order)`). See "Crash test" below for numbers.

## Crash test

`tests/case_storagecrash.py`, `python tests/run_all.py storage_crash`. Each run builds a fresh image with a HUMANVILLAGE volume (one of
FAT32 with the default, 512-byte or 4 KiB clusters, or FAT16), boots, starts `fstest -churn SEED /HumanVillage/reimu` in the guest and
pulls the plug: QMP `quit` after a random number of operations (0 to 1200, read from the workload's `churn N` lines) plus a random
0 to 1 s, so the cut lands inside an operation, between operations or in the middle of a flush. No `sync`, no `poweroff`, the kernel
gets no warning. Then the host looks at the image with `tools/mkdisk.py`'s checker (no code shared with the kernel):

* allowed: lost clusters, a chain longer than its file's size, FAT copy 2 behind copy 1, a stale FSInfo count, the dirty flags;
* anything else fails the run (a cross-link, a chain into a free cluster, a size beyond the chain, a broken directory, a long name
  without its short entry, bytes after a directory's end), and so does a file holding a byte it never held: every byte of the
  workload's file `k<id>` is either 0 or `churn_byte(id, offset)` (`user/bin/fstest.c`; a numpy twin in the test), whatever renames
  it went through, so another file's or a directory's bytes in it are caught;
* the kernel's own ledger check (`shared/fatck.c`, run on the host through `tools/fatck_host.py`) must find no error either, which is
  what lets the next boot mount the volume read-write ("Dirty volumes");
* a call that failed in the guest (`CHURNFAIL`, anything but a full volume) fails the run too.

The workload (`fstest -churn SEED DIR [OPS]`): creates (empty, small, cluster-boundary and 40 KB sizes; 8.3 names in both cases and long
names of up to 9 slots), appends, overwrites inside and across the end, truncates down and up, deletes, renames to a new name and into
another directory, the editor's save (a `.tmp` copy renamed over the file, spelled alike or in the other case), mkdir, rmdir, moving
and renaming whole directories, bursts of 10 to 30 small files and of 10 deletes, a big file growing in 64 to 256 KiB steps up to 8 MiB,
and an occasional `sync`; a full volume deletes files instead of failing.

`TOUHOU_CRASH_RUNS=N` sets the number of runs (default 3, the full run is 50), `TOUHOU_CRASH_SEED=S` the seed of the first (default: from
the clock; run i uses S+i). A failing run prints its seed, geometry and kill point and the checker's report; `TOUHOU_CRASH_SEED=<seed>
TOUHOU_CRASH_RUNS=1` replays its workload and kill point (the exact moment varies with the guest's speed), `TOUHOU_KEEP_IMAGES=1` keeps
the image. `storage_crash_checker_selftest` (no VM) damages images by hand to make sure the judge lets the leftovers through and
catches a cross-link, a chain into a free cluster, a size beyond the chain and another file's bytes; the ledger check is held to the
same images (all but the last, which only the contents show).

**Results** (2026-10-24, Ubuntu 24.04, QEMU 8.2 TCG, `TOUHOU_TIMEOUT_SCALE=3`):

* 50 runs, seeds 7000..7049: **50 passed**, 491 s. Geometries: 17 FAT32 default clusters, 16 FAT32 4 KiB, 6 FAT32 512 B, 11 FAT16; cuts
  after 0 to 1180 operations. Leftovers seen: lost clusters in 33 runs (1 to 77 clusters), a stale FSInfo count in 34, FAT copy 2
  behind copy 1 in 8, a chain longer than its size in 6, nothing at all in 5.
* After the merge of `cloud/staging` (the ledger check added to the judge), 50 runs, seeds 8000..8049: **50 passed**, 517 s. 15 FAT16,
  13 FAT32 default, 12 FAT32 512 B, 10 FAT32 4 KiB; cuts after 20 to 1180 operations. Lost clusters in 32 runs (up to 127), a stale FSInfo
  count in 29, a chain longer than its size in 10, FAT copy 2 behind in 2, nothing in 7; the ledger check found 0 to 7 leftovers per run
  and never an error.
* Control: with the classes switched off (every dirty page in one class, so the cache writes in disk order as a plain write-back cache
  would), 2 of 12 runs failed (seeds 505 and 506; 506 passed when replayed, the moment of the cut differs); seed 505 replayed shows the gap of the LINK class: `K91.DAT: chain runs into a free
  cluster after 625`, `size 1180 needs 3 cluster(s) but the chain has 1` (the link of an append reached the disk, the new clusters'
  own entries did not). So the test does see a wrong order.
* Cost: `fstest -churn 11` and `-churn 12` with 600 operations each and an unmount: 691 and 703 extra device flushes (about 1.2 per
  operation; mostly a FAT page going from frees to allocations and back, and the barriers of renames and new entry sets). In QEMU a flush
  is almost free (the 600 operations took 3.2 and 3.4 s of wall time; the old order was not timed); on a USB stick each one is a SYNCHRONIZE CACHE, which can take milliseconds.
  Not measured on hardware.

## FAT specifics and limits

* FAT32 and FAT16 (the QEMU `fat:` ESP is FAT16) read/write. **exFAT is not supported, and FAT12 is not mounted** (the checker and `fsck` do read and mend FAT12). NTFS is **read-only**, including a BitLocker volume whose only key protector is a clear key: see [NTFS.md](NTFS.md) (`mount -t ntfs -o ro ...`; it is never mounted by the mount policy, and no write path exists).
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

`python tests/run_all.py fsck` (`tests/case_fsck.py`; the VM ones through `python tools/qemu-slot.py -- ...`):
* `fsck_c_*` need no VM (they build `shared/fatck.c` and `shared/fatfix.c` into a host DLL, `tools/fatck_host.py`): `fsck_c_agrees_*` hold the checker against
  the Python checker over deliberately damaged images (25 seconds); `fsck_c_repair_*` hold the repair engine to what "Dirty volumes" says (the damage menu on
  FAT12/16/32 and on 4096-byte sectors with `-y` and `-a`, the ways two chains meet, no room for `FOUND.000`, the numbering of `FOUND.000`, a damaged FSInfo pointer, random
  damage: about four minutes in all). Run them with `python tests/run_all.py fsck_c_` -- **with the underscore: `fsck_c` also selects the `fsck_cmd_*` tests, which boot VMs**.
* `fsck_kernel_*` boot a VM with a dirty volume: a flag and nothing else wrong (read-write, the log reaches the stick), harmless leftovers (read-write, surplus
  chain trimmed on the next write, flag stays), real damage (read-only, the log and `/proc/fatcheck` say what, the image is untouched), `hvforce` and `hvcheck=off`,
  and the limits (nesting beyond 256, a 12 GiB volume with 3 million clusters). Tests that used to expect "dirty means read-only"
  (`storage_dirty_volume_readonly`, `storage_unclean_power_loss`, the bootlog test) run with `hvcheck=off`; `usbstor_hotplug_idle` now expects the check and a writable mount.
* `fsck_cmd_*` run `/bin/fsck` itself in the guest: `fsck_cmd_options_and_exit_codes` (one disk of ten small FAT12/16/32 volumes, damaged in their own ways, gone through
  with every option from a root shell; afterwards the host compares each volume with its own run of the same repairs), `_not_root`, `_asks` (the questions through
  the serial console), `_4kn` (a 4096-byte-sector disk), `_kernel_advice` and `_kernel_advice_remount` (the flow the kernel's message promises: `umount /HumanVillage`,
  `fsck -a`, restart or mount by hand, the volume is clean and writable), and `_after_power_cuts`: the images of random power cuts (the crash test's workload and
  kill, `TOUHOU_FSCK_CUTS=N`, 2 by default), `fsck -n` on each must report leftovers and nothing worse, `fsck -a` saves every lost cluster in `FOUND.000` and
  changes no readable file, the result equals the host's own repair of the same image, and the next mount finds nothing to check.

`python tests/run_all.py storage_crash` (`tests/case_storagecrash.py`, part of the `storage` group): the power-cut test of "Crash test" above, 3 runs by
default (about 30 s), `TOUHOU_CRASH_RUNS=50` for the full run (about 9 minutes here).

## State when paused

The "dirty-flag trap" work (a dirty HUMANVILLAGE volume is checked instead of refused; a repair tool; a safer write order) was paused part-way; the
write order was then done on `cloud/fat-ordering` (cloud task 7, 2026-10-24), which also holds the merge of `cloud/staging`. What is what:

**FAT write order (task 7): done and tested**
* The six flush classes, `pg_reclass`, the sector rule for entry sets, renames in three steps ("Write ordering"); the code is in `fat_cache.c`, `fat_chain.c`,
  `fat_dir.c`, `fat32.c`, `fatfs.h`.
* `fstest -churn` and `tests/case_storagecrash.py`: 50 power cuts passed before the merge with staging (seeds 7000..7049) and 50 after it (seeds 8000..8049,
  now also judged by the kernel's ledger check: 0 to 7 leftovers per run, never an error); the control with the order switched off fails ("Crash test").
* After the merge: `storage`, `usbstor` and the regression set `blk,selftest,utest,shell,explorer_netherworld` - results in the pull request and in docs/the project notes.
* Not covered: names of more than 195 characters and entry sets another system wrote across a sector can still be cut in half; the cost of the extra flushes on a
  real stick is not measured; the test runs on virtio-blk only (QEMU keeps every acknowledged write, so it checks the order of the requests; whether a USB stick
  honours SYNCHRONIZE CACHE is not something QEMU can show).


**The check and `fsck` (agent `fsck`, 2026-11-04): done and tested in QEMU, never run on the real machine**
* The checker (`shared/fatck.c`): `fsck_c_agrees_with_python` and `fsck_c_agrees_on_random_damage` pass on the current sources.
* The repair engine (`shared/fatfix.c`): `fsck_c_repair_*` (no VM, see "Tests"): every damage of the menu on FAT12/16/32 and on 4096-byte sectors with `-y` and `-a`,
  the ways two chains can meet, no room for `FOUND.000`, a damaged FSInfo pointer, a power cut in the middle of `fsck` (`fsck_c_repair_interrupted`), and a random-damage
  fuzz (3000 `-y` and 1500 `-a` cases clean). It found and fixed five ways the repair itself lost data or names, and two more in the interrupted runs ("Dirty volumes",
  "Held against the repair tests"); against the old engine the menu test fails for three of them.
* `/bin/fsck`: `fsck_cmd_*` (see "Tests"). The assumptions of the hand-over all held: `lseek(SEEK_END)` gives the size of a `/dev` node, writes plus `ioctl(BLKFLSBUF)`
  on a node work (also on a 4096-byte-sector disk, whose size comes from `BLKSSZGET`), the `/proc/mounts` check, the prompts through the serial console, the exit
  codes (0, 1, 4, 8, 16, 32). What the guest runs found in the program: a mounted `vda10` stopped `fsck` from looking at `vda1` (a prefix match), a repair left alone
  was printed as "fixed", and quitting at a question dropped what had been agreed to; all fixed in `user/bin/fsck/main.c`. No kernel, `blk` or libc change was needed.
* The flow the kernel's message promises works: read-only mount, `umount /HumanVillage` (fsck refuses before it), `fsck -a`, restart: clean, read-write, no mention
  of a ledger; with damage `-a` leaves, `fsck -y` and a mount by hand without a restart.
* Power cuts and `fsck` together (`fsck_cmd_after_power_cuts`): the images of random cuts (the crash test's workload) go through `fsck -n` (leftovers and nothing
  worse), `fsck -a` (every lost cluster saved in `FOUND.000`, no readable file changed, equal to the host's repair of the same image), and the next mount finds nothing
  to check. 3 cuts passed first, then 24 (seeds 15375694..15375717: 6 FAT16, 18 FAT32 with 512 B, 4 KiB and default clusters; 438 s): 21 of them had lost clusters
  (up to 380 in 24 files, once 190 in 106 files), 0 to 4 leftover findings each, one left nothing at all; every one ended as the test requires; ten more cuts
  later with the final binary (one of them left a volume that was already clean: `fsck -a` then exits 0, which the test now accepts). The kernel (main `cf0436e`,
  with the refused-entry-update fix) was in the guests.
* `fsck.pw` (the manual page) is updated.

**Approximated, or not verified**
* **Not run on the Dell or any real stick.** The walk-through in REAL_HARDWARE.md ("After a bad shutdown") is what QEMU showed. In particular `umount /HumanVillage`
  while the desktop runs from that volume (the homes live there) has not been tried: expect "busy" until the desktop has let go; the page says what to do instead
  (leave the desktop, or boot once with `nohumanvillage`). The only account that may use `sudo` is `marisa`; root's login is locked.
* `fsck` is restartable, not atomic: a repair pass is written from the cache in whatever order the windows are evicted or flushed (except that a copy of
  clusters and a new directory cluster are flushed before anything points at them), so a power cut in the middle of `fsck` can leave a volume that needs
  another `fsck` run (each run starts from a fresh scan, so running it again is the answer). `fsck_c_repair_interrupted` cuts the repair on the host after the
  first, the last, the middle and some random sectors (writes are torn sector by sector) for every damage of the menu and every scenario on FAT16 and FAT12 (more
  and FAT32 with `TOUHOU_FSCK_CUT_POINTS=25`; that run, about 9000 cuts, takes six minutes and passed) with `-y` and with `-a`, and runs it again: the second run
  ends as clean as an uninterrupted one and loses nothing. It found one hole (the link of a cross-linked file's copy written before the copy: the file then read the
  new clusters' old bytes), fixed by a flush before the link. Not tested: a cut during `fsck` in a guest, or on a device that reorders or drops acknowledged writes.
* A long name whose checksum does not match its short entry is dropped (the file stays under the short name), as Windows and dosfstools do; it is not re-stamped.
* The checker refuses a volume whose boot sector it cannot trust, and a boot sector that is wrong but plausible (a root cluster that names some file's data, say) is
  taken at its word: `fsck -y` on such a volume would "mend" that data as a directory. The random tests leave boot-sector bytes out for this reason (the damage menu
  has the refused cases); an FSInfo sector the boot sector points at wrongly is left alone.
* More than 10000 lost chains: the 10001st is not saved (`FILE9999.CHK` is the last name); `-F` frees them.

**Not started**
* Power cuts *during* `fsck` in a guest (kill the VM at random moments of `fsck -y` on a damaged volume and require that a second `fsck` ends clean); the host test does
  it per sector, which also tears writes, but with the engine's own model of the device.
* A way to look at (or mend) the volume the system is running from without leaving the desktop, which the advice of the log line needs on the Dell.
* `fsck` at boot, before the mount (a command line word), for an installed machine that is only reachable over ssh.

## How to resume

1. `python tools/build.py`; `python tests/run_all.py fsck_c_` (no VM, about 5 minutes; mind the underscore) and, through the slot limiter,
   `python tools/qemu-slot.py -- python tests/run_all.py fsck_cmd,fsck_kernel` (about 5 and 5 minutes); then the suites this work touched: `storage`, `usbstor`, `usb_`, `blk_`,
   `power_,manual`. After a change to `kernel/fs/fat*.c` also `espgrant,update_push,storage` (the ESP update writes through the same code).
2. A repair that loses something is a bug in `shared/fatfix.c`: `TOUHOU_FSCK_FUZZ=3000 TOUHOU_FSCK_SEED=100000 python tests/run_all.py fsck_c_repair_fuzz` finds new ones; the
   failing line names the seed, and `tests/case_fsck.py` `random_damage(src, dst, random.Random(seed), kinds=REPAIR_KINDS)` rebuilds the damaged image
   (`tools/fatck_host.py -y IMAGE:1` repairs it; `-a` and `-n` too). A new damage for the menu goes into `damage_menu()` and, for the repair tests, into `AIMED`
   (the files it is aimed at, whether `-a` leaves an error, what has to survive).
3. After a change to the kernel's FAT driver: `TOUHOU_CRASH_RUNS=50 python tests/run_all.py storage_crash`, and `TOUHOU_FSCK_CUTS=20 python tests/run_all.py fsck_cmd_after_power`.
   A failing cut names its seed (`TOUHOU_CRASH_SEED=<seed> TOUHOU_FSCK_CUTS=1`, `TOUHOU_KEEP_IMAGES=1` keeps the image for `python tools/mkdisk.py --check IMAGE`).
4. The write order is done (`cloud/fat-ordering`). What could come next: fewer barriers (most come from a FAT page going from frees to allocations and back; frees could
   be collected per page and written in one go before an allocation reuses the page), a measurement of the flush cost on the real stick (the unmount log line
   counts the extra flushes), and the same test on a USB stick in QEMU (`usb-storage`) instead of virtio-blk.
5. On the Dell, with the owner's go: switch it off with the power button while something writes (a big copy), start it again, read `/proc/fatcheck` and the `[Keine]`
   line, and try the three commands of REAL_HARDWARE.md, "After a bad shutdown". Note what `umount /HumanVillage` says.
