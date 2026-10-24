# Installing TouhouOS on a disk (`install`)

Kanako once moved the whole Moriya Shrine, lake and all, into Gensokyo; `install` does the smaller version: it writes TouhouOS onto a
disk of the computer so it starts without the USB stick. Everything on that disk is replaced, so the tool is mostly a set of refusals
with a disk writer at the end.

```
$ sudo install                                   # which disks are there, and what install thinks of each
DISK            SIZE  MODEL                    STATE
sda          14 GiB   PNY USB 3.0 FD           the system was started from this disk
nvme0n1      238 GiB  SAMSUNG MZVLB256HAHQ     holds partitions nvme0n1p1(...) ... (needs --force) [read-only: blk.ro]
$ sudo install --dry-run nvme0n1                 # the plan, nothing written
$ sudo install --force nvme0n1                   # asks you to type "nvme0n1", then writes
```

(The listing above is illustrative; the format is real.) The manual page is `man install`
(`initrd/ScarletDevilMansion/share/patchouli/1/install.pw`).

## Before you start, on the owner's Dell

**The Dell's SATA controller is in Intel RAID mode ("RAID On", Intel RST).** In that mode the controller hides its disks, and an NVMe drive
in the M.2 slot as well, from every operating system without Intel's RST driver: TouhouOS logs `SATA controller is in RAID (Intel RST)
mode` and `install` lists no internal disk at all. To install, the firmware setting *System Configuration > SATA Operation* has to be
switched to **AHCI** (F2 at power-on). **An existing Windows installed in RAID mode usually stops starting after that switch**
(`INACCESSIBLE_BOOT_DEVICE`) unless it was prepared first (`bcdedit /set {current} safeboot minimal` in Windows, switch, start Windows once
in safe mode, `bcdedit /deletevalue {current} safeboot`; details in [BLOCKDEVICES.md](BLOCKDEVICES.md), "Real hardware notes"). Back up
first; whether the internal disk may be wiped at all is the owner's decision. Installing onto a second internal disk, or onto a USB disk
that stays plugged in, leaves the Windows disk alone (and `blk.ro` keeps it read-only, see below).

Secure Boot stays off (the loader is unsigned). The installed disk starts through the firmware's removable-media path
(`EFI/BOOT/BOOTX64.EFI`); `install` writes no NVRAM boot entry. If the firmware does not pick the disk by itself, choose it once in the
boot menu (F12 on the Dell) or move it up in the boot order in the setup.

## What it writes

The layout of the stick image, decided in `tools/mkdisk.py` and copied from it (`build_image()`, `FatBuilder`, `gpt_write()`):

| | Where | Contents |
|-|-------|----------|
| protective MBR | LBA 0 | one `0xEE` entry over the disk (at most 2^32 - 1 sectors) |
| GPT | LBA 1-33, backup at the end | 128 entries of 128 bytes, a random disk GUID, header and entry CRCs |
| partition 1 `TOUHOUOS-ESP` | from 1 MiB, 64 MiB (258 MiB on a 4096-byte-sector disk, the smallest FAT32 volume it can hold) | EFI System Partition type `C12A7328-...`, FAT32 label `TOUHOUOS-ES`, `EFI/BOOT/BOOTX64.EFI`, `kernel.elf`, `initrd.img`, `cmdline.txt` |
| partition 2 `HUMANVILLAGE` | the next MiB boundary to the end of the usable area | basic data type `EBD0A0A2-...`, FAT32 label `HUMANVILLAG`, empty: the kernel creates `reimu`, `marisa`, `sakuya` when it mounts it the first time |

FAT32 parameters as mkdisk: 32 reserved sectors (FSInfo at 1, backup boot sector at 6 and FSInfo at 7), two FATs, media `0xF8`, the cluster
size by volume size (512 B up to 64 MiB, 1 KiB to 128 MiB, 2 KiB to 256 MiB, 4 KiB to 8 GiB, 8 KiB to 16 GiB, 16 KiB to 32 GiB, 32 KiB beyond;
never below the sector size), directories first and then each file in one contiguous run, lower-case 8.3 names stored with the NT case
bits (no long-name entries needed, which is how mkdisk stores `kernel.elf`). The volumes are written clean (FAT[1] clean bits set, flag
byte 0). A FAT32 volume counts its sectors in 32 bits, so on a disk larger than 2 TiB (512-byte sectors) HUMANVILLAGE stops at 2 TiB and the
rest stays unpartitioned.

Write order: ESP data and directories, its FATs, its reserved area with the boot sector last; the same for HUMANVILLAGE; then the GPT
(backup array and header, primary array and header, protective MBR last). A run that is cut off therefore never leaves a disk that looks
finished. Afterwards the device cache is flushed, the files and the primary GPT header are read back and compared, and `cmdline.txt`
names the new disk.

The kernel's partition list for that disk is the one it read at boot; the new partitions appear after a restart.

## Where the files come from

The running system no longer has its files as files: the loader read `kernel.elf` and `initrd.img` from the boot partition into memory, and
the kernel unpacked the initrd. So `install` copies them from the boot partition itself:

1. The loader asks the firmware for the device path of the volume it was loaded from and keeps its *HardDrive* node (UEFI 10.3.5.1:
   partition number, first sector, size, and the GPT partition GUID or the MBR disk signature). It passes it in `BootInfo.boot_part`
   (`shared/bootinfo.h`, `BOOTINFO_VERSION` 2) and logs `loader: booted from partition 1 (GPT, start 2048, 131072 blocks)`. This only reads
   what the firmware already has in memory.
2. The kernel shows it in `/proc/bootdisk`, with a `device` line for every registered partition with that number, start and size:

   ```
   partition 1
   table gpt
   start 2048
   size 131072
   signature 2f6c...-....            (MBR: 0xbe1afdfa, the disk signature QEMU's fat: drive carries)
   device sda1 sda
   ```

3. `install` confirms the signature on the disk itself (the GPT entry's unique GUID, or the 32 bits at offset 440 of the MBR), mounts that
   partition read-only on `/BambooForest/install-esp.PID` (or uses it where it is already mounted), reads `EFI/BOOT/BOOTX64.EFI`, `kernel.elf`,
   `initrd.img` and `cmdline.txt` into memory, checks that the first is a PE file (`MZ`) and the second an ELF file, and unmounts.

On the Dell that is the stick's ESP (`sda1` or `sdb1`, USB mass storage); in QEMU it is the `fat:` directory drive on SATA (`sda1`, FAT16).
`--from DIR` takes the files from a directory instead (for a system whose firmware gave no partition, or to install other builds).

`cmdline.txt` of the new system: `--cmdline WORDS` if given, else the boot partition's own `cmdline.txt` (the loader's default
`video=native blk.ro` when there is none), without any old `blk.rw=` word, plus `blk.rw=<the new HUMANVILLAGE partition's GPT GUID>` (see below).

## Safety design

All checks run before the disk is opened for writing, in this order; each refusal says why and ends with `install: nothing was written.`

1. **Root only** (`geteuid`); the kernel checks again at the unlock.
2. **A whole disk** that exists in `/proc/partitions` (`sdb1` is refused: "a partition of sdb"), with 512- or 4096-byte sectors.
3. **Not the boot disk**: the disk holding the partition `/proc/bootdisk` points at (signature confirmed) is refused even with `--force`. A disk
   that has a partition with the right number, start and size is refused as well when its signature cannot be read, or when it matches a
   second time (two sticks written from one image carry the same GUIDs); a disk whose signature is a different one (an older install,
   which has the very same layout) is not the boot disk and can be overwritten with `--force`. When the
   firmware did not say where the system came from, `--force` is not accepted at all (the boot disk, which always has partitions, cannot be
   told apart then).
4. **Not in use**: a disk with any mounted partition (or mounted itself) is refused even with `--force` (`umount` first).
5. **Empty unless `--force`**: partitions the kernel registered, an MBR with entries, a GPT header (primary, or backup in the last sector),
   a FAT/NTFS/exFAT boot sector without a partition table, ext2/3/4, XFS, btrfs, LVM, LUKS, Linux swap, ISO 9660, and Intel RST (IMSM) RAID
   metadata near the end. A disk full of unrecognisable data counts as empty (the typed name is the last guard).
6. **Big enough** for both FAT32 volumes (FAT32 needs at least 65525 clusters).
7. **The plan is shown and the name typed**: the prompt is read from `/dev/tty`, so piped input cannot answer it; anything but the exact
   name stops. For scripts `--yes-i-typed=NAME` gives the answer in advance and must be the exact name too. Without a terminal and without
   that option `install` refuses.
8. Only then: `open(/dev/DISK, O_RDWR)`, a refusal if the medium itself is write-protected, `BLKWRUNLOCK`, the writes, `BLKWRLOCK`,
   the read-back.

### The write unlock (instead of dropping `blk.ro`)

`blk.ro` (the default command line) registers every SATA disk and NVMe namespace read-only, so even root's `dd` gets EROFS. The installer
needs to write exactly one disk, so the block layer got a deliberately narrow way past it (`kernel/drivers/blk_unlock.c`,
`shared/blkabi.h`): an **ioctl on the open `/dev` node of the whole disk**.

```c
int fd = open("/dev/sdb", O_RDWR);
struct gblk_unlock u = { .name = "sdb" };   // the caller repeats the disk's name
ioctl(fd, BLKWRUNLOCK, &u);                 // 0, or EPERM / EINVAL / EBADF / EROFS / EBUSY
... writes through fd ...
ioctl(fd, BLKWRLOCK, &u);                   // or close(fd), or exit
```

| Rule | Enforced by |
|------|-------------|
| root only | `EPERM` unless the caller's effective uid is 0, checked at the ioctl; a process that inherited the open file but is not root cannot unlock it (`[Eiki] ... asked to unlock sdb for writing and was refused`) |
| that one disk | the unlock is a pair (open file, disk) in a table of four; only `blkdev_io` writes through that very open file get past the read-only flag |
| whole disks only | a partition node (`sdb1`) gives `EINVAL`, and writes through partition nodes keep failing while the disk is unlocked |
| nothing else | the disk stays registered read-only (`lsblk` shows `RO 1`): a second `open` of `/dev/sdb`, its partitions, mounted filesystems, the kernel's own writes and every other disk keep getting `EROFS` |
| the right disk | the name in the argument must be the node's name (`EINVAL` otherwise): a wrong file descriptor unlocks nothing |
| writable files only | a file opened `O_RDONLY` gives `EBADF` |
| not in use | `EBUSY` while the disk or one of its partitions is mounted |
| no hardware overrides | a medium that is read-only by itself (write-protected stick, read-only virtio disk) gives `EROFS`: only the `blk.ro` policy can be lifted |
| released | `BLKWRLOCK`, and the last `close` of the open file (also at exit or on a crash, when the kernel closes the descriptors). The table lives in kernel memory only: nothing is written to disk or NVRAM, a restart forgets it |
| visible | `blk: sdb unlocked for writing by install (pid N), through one open file only ...` and `blk: sdb is read-only again ...` in the kernel log |

The unlock belongs to the *open file*, not to the process: a child that shares the open file (fork) writes through it too, as with any
other property of an open file. `BLKWRSTATE` returns `BLKWR_READONLY | BLKWR_POLICY | BLKWR_UNLOCKED` bits for the disk as seen through the file.

Why an ioctl on the device rather than a new syscall or a `/proc` or `/dev` control file: the permission is then tied to an object the
kernel already reference-counts and closes for us (so it cannot outlive the installer), it cannot leak to other openers of the same disk,
and no syscall number is needed (syscall.h stays as it is). `BlockDev` got one field, `ro_policy` (read-only because of `blk.ro`, set by the
AHCI and NVMe drivers), so the unlock can tell the policy from a write-protected medium.

### `blk.rw=GUID`: the installed homes stay writable under `blk.ro`, nothing else

An installed system still wants `blk.ro` (a Windows disk next to it must stay untouched), but its own HUMANVILLAGE volume, on a SATA or NVMe
disk, has to be writable or the homes would be read-only. `install` therefore ends the new `cmdline.txt` with `blk.rw=<GUID>`, where the GUID
is the **unique partition GUID of the new HUMANVILLAGE partition** (GPT entry 2), not the disk's. When the kernel scans a disk that `blk.ro`
made read-only and one of its GPT entries carries a GUID named in `blk.rw=` (a comma-separated list), that one partition is registered
writable, with a log line:

```
[Marisa] blk: nvme0n1p2 carries partition GUID 5809aa93-..., named by blk.rw=: writable although blk.ro is set (the homes of the disk TouhouOS was installed on); the rest of nvme0n1 and every other SATA and NVMe disk stay read-only
```

Only that partition: the installed ESP (loader, kernel, initrd, `cmdline.txt`), the whole-disk node and every other disk stay read-only, so
not even root can damage the boot files of the running system by accident. Writes to the granted partition pass the disk's `blk.ro` only
inside the partition's own bounds (`part_write` in `kernel/drivers/blk.c`). A medium that is itself write-protected is never made writable,
and MBR partitions (no GUID) cannot be named. The GUID is random (version 4), so it names exactly the partition `install` wrote; a byte copy
of the disk carries the same GUID and would match too. Without `blk.ro` the word does nothing. Removing it from `cmdline.txt` (on any
computer: the ESP is plain FAT32) makes the installed homes read-only like everything else.

## The installed system

It starts like the stick: the loader from the ESP, `[Marisa] blk: ...` for the disk with its two partitions, `fat32: sda2 mounted on
/HumanVillage`, the home directories created on the first start, `TOUHOU.LOG` kept on the volume (the log line still says "on the stick"),
reimu's desktop. Booting from the stick again while the installed disk is present: see "Known gaps".

## Tests

`python tests/run_all.py install_` (`tests/case_install.py`, about two minutes on a 4-core Linux host without KVM). Every test boots from the
`fat:` ESP (sda), with `blk.ro` and a root shell; host-side checks after the VM has exited: `tools/mkdisk.py --check` on the installed image
(0 errors, 0 warnings), the partition table against the one `mkdisk.build_image()` makes for an image of the same size, the ESP's files
byte-identical to `build/esp`, the HUMANVILLAGE volume clean, `cmdline.txt` equal to the given words plus `blk.rw=` with the HUMANVILLAGE partition's GUID from GPT entry 2,
decoy disks byte-identical. Then the installed image is booted **alone** (`Qemu(boot_image=..., boot_bus=...)`: no stick, no `fat:` drive)
to reimu's desktop (the panel's start button in the screenshot), and its log must show the loader on GPT partition 1, the installed command
line, HUMANVILLAGE mounted read-write and (SATA, NVMe) the `blk.rw=` line.

| Test | What it covers |
|------|----------------|
| `install_sata_boots_desktop` | SATA target holding another system's GPT and FAT volumes: refused without `--force`; with `--force` and `--hold` the installer stops with the unlock held, and meanwhile writes to the target's own node (a second open), its partitions, another SATA disk and its partition, and an NVMe disk and its partition all fail with EROFS while `lsblk` still shows the target read-only; then it finishes, the unlock is gone (`dd` to the target fails again), the decoys are unchanged; the installed disk boots alone next to the SATA decoy, which stays read-only and unchanged |
| `install_nvme_boots_desktop` | NVMe target, confirmation typed at the terminal prompt; boots alone from NVMe |
| `install_virtio_boots_desktop` | virtio target holding a whole-disk FAT volume without partition table: refused without `--force`, installed with it (virtio is never read-only, so the unlock logs "it was writable anyway"); boots alone from virtio |
| `install_nvme_4k` | NVMe namespace with 4096-byte blocks: 258 MiB ESP, checker clean, layout equal to mkdisk's 4Kn layout |
| `install_from_stick_over_old_install` | the Dell's situation: started from a `tools/mkimage.py` stick image on USB, so the files come from the stick's FAT32 ESP; an older install on NVMe (same layout, other GUIDs, so `/proc/bootdisk` lists its partition 1 as a candidate too) is told apart by its signature and reinstalled with `--force`; a byte copy of the stick on SATA (same GUIDs) is refused like the stick itself; the copy stays unchanged |
| `install_unlock_rules` | `user/bin/unlocktest.c` against the kernel directly: starts read-only by policy, wrong name EINVAL, read-only file EBADF, a non-root child holding the open file gets EPERM (and the Eiki line), unlock, state bits, a second open and a partition stay read-only, partition node EINVAL, a write through the unlocked file (also from a forked child) lands and reads back, BLKWRLOCK and close release it, a read-only virtio medium gives EROFS, a mounted partition gives EBUSY; on the host the one written block is the only change to the image |
| `install_esp_stays_read_only` | installs on NVMe with `text user=root blk.ro`, boots the installed disk alone next to a SATA decoy: the `blk.rw=` line names `nvme0n1p2`, HUMANVILLAGE is mounted read-write and a file written there reads back, while `dd` to the installed ESP (`nvme0n1p1`), the whole-disk node and the decoy and its partition all fail with EROFS; `/proc/partitions` shows disk and ESP read-only, HUMANVILLAGE writable; on the host the ESP's bytes and the decoy are unchanged |
| `install_refusals` | the listing (boot disk, partitions, empty); refused with nothing written: partitions without `--force`, `--yes-i-typed` with another or differently-cased name, non-root (`su reimu`), the boot disk with `--force`, a partition name, a missing disk, a disk that is too small, a mounted partition with `--force`, three wrong answers typed at the prompt (another disk, empty, extra words); `--dry-run` shows the plan and the boot partition's command line plus `blk.rw=`; no unlock happened; every image byte-identical |

Results on this branch (Ubuntu 24.04, QEMU 8.2, TCG, `TOUHOU_TIMEOUT_SCALE=2`): all eight pass; the regression groups named in the report of
the branch pass as well (see the pull request).

## Known gaps

* **Never run on real hardware.** Everything above ran in QEMU/OVMF only. On the Dell nothing can be installed until SATA is in AHCI mode.
  How the Dell's firmware names the stick's partition in the device path, and whether it picks the installed disk by itself, is untested.
  If `/proc/bootdisk` says `unknown`, use `--from` with the stick's ESP mounted by hand (`mount -r /dev/sda1 /mnt`) and no `--force`.
* **Booting the stick while an installed disk is present**: the mount policy takes the first HUMANVILLAGE volume found and internal disks are
  scanned before USB, so the installed disk's volume becomes `/HumanVillage`; with the stick's `cmdline.txt` (no `blk.rw=`) that disk is
  read-only under `blk.ro`, so the homes are read-only for that session. Add the installed disk's `blk.rw=` word to the stick's
  `cmdline.txt`, or `umount /HumanVillage` and mount the stick's volume by hand. A policy that prefers a writable volume would fix it.
* A HUMANVILLAGE volume left dirty (power cut) mounts read-only until a file system check; on an installed disk that means the in-kernel
  check of the `parked/usb-dirty-fsck` branch (not merged here) or the stick.
* No in-place upgrade: `install` replaces the whole disk, homes included. Updating `kernel.elf`/`initrd.img` on an installed ESP is task 5
  (the update client). The installed ESP is read-only under `blk.ro` (only HUMANVILLAGE is named by `blk.rw=`), so by hand it is done from
  the stick, or from a boot without `blk.ro`.
* Two disks written from one image (same GUIDs) cannot be told apart: the first one the kernel lists (SATA before USB) counts as the boot
  disk and its ESP is where the files are copied from; both are refused as targets. The files are the same as long as the copies are.
* The kernel does not re-read the partition table after `install`; the new partitions appear after a restart.
* `/proc/partitions` prints a GPT name with spaces as it is (`EFI system partition`), which shifts the later columns for every reader of that
  file (`lsblk` and `install` show such a label cut at the first space). Cosmetic, older than this branch.
* Disks larger than 2 TiB (512-byte sectors): HUMANVILLAGE is capped at 2 TiB, the rest stays free. No exFAT.
* The firmware's own boot entries in NVRAM are not touched; on a PC that only boots NVRAM entries (no removable-media fallback for internal
  disks) the installed disk would need an entry made from the firmware setup.

## Files

| File | What |
|------|------|
| `user/bin/kappabox/install.c` | the `install` applet (checks, layout, FAT32 and GPT writer, read-back) |
| `kernel/drivers/blk_unlock.c` | BLKWRUNLOCK / BLKWRLOCK / BLKWRSTATE, `blk.rw=`, `/proc/bootdisk` |
| `kernel/drivers/blk.c` | the write path takes the open file into account; the ioctl default and the `closed` hook call `blk_unlock.c`; `blk_rw_check()` in the GPT scan |
| `kernel/include/blk.h` | `BlockDev.ro_policy`, the four functions of `blk_unlock.c` |
| `kernel/drivers/ahci.c`, `nvme.c` | one line each: `ro_policy` set with `readonly` under `blk.ro` |
| `shared/blkabi.h` | ioctl numbers, `struct gblk_unlock`, state bits |
| `shared/bootinfo.h`, `boot/main.c`, `boot/uefi.h` | `BootPartition boot_part` (BootInfo version 2), filled from the device path |
| `user/bin/unlocktest.c` | the unlock rules, from user space, for the tests |
| `tests/case_install.py` | the tests above |
| `initrd/.../patchouli/1/install.pw`, `5/proc.pw` | manual pages (`install(1)`, `/proc/bootdisk` in `proc(5)`) |

## State when paused

Done and tested in QEMU: the tool, the unlock, `blk.rw=` (one partition), `/proc/bootdisk`, the eight tests, the manual page. Not done: anything on real
hardware (the owner's Dell needs AHCI mode first and a decision about its internal disk), the mount-policy preference for a writable
HUMANVILLAGE volume (Known gaps), an upgrade-in-place mode.

## How to resume

1. Merge after `cloud/staging` (task 2): the shared-file edits are small (`blk.c` about 20 lines, `blk.h` 8, `bootinfo.h` 16, `boot/main.c` 35,
   one line each in `ahci.c`/`nvme.c`), new files otherwise. Rerun `install_`, `blk_`, `image_`, `storage`, `usbstor`, `manual`.
2. On the Dell: back up, decide what happens to Windows, switch SATA to AHCI, boot the stick, `sudo install` (the listing), `sudo install --dry-run DISK`,
   then the real thing; photograph `/proc/bootdisk` and the listing if anything looks wrong. Then remove the stick and start from the disk.
3. Task 5 (updates): `/proc/bootdisk` tells the updater which ESP is the live one. The installed ESP stays read-only under `blk.ro` on purpose,
   so the updater needs a root-only way to mount it read-write for the moment of the update, scoped like the write unlock (one partition,
   released at unmount, never persisted); that mechanism does not exist yet.
