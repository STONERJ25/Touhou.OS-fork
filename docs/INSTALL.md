# Installing TouhouOS on a disk (`install`)

Kanako once moved the whole Moriya Shrine, lake and all, into Gensokyo; `install` does the smaller version: it writes TouhouOS onto a
disk of the computer so it starts without the USB stick. Everything on that disk is replaced, so the tool is mostly a set of refusals
with a disk writer at the end.

```
$ sudo install                                   # which disks are there, and what install thinks of each
DISK            SIZE  MODEL                    STATE
sda          14 GiB   PNY USB 3.0 FD           the system was started from this disk
nvme0n1      238 GiB  SAMSUNG MZVLB256HAHQ     holds partitions nvme0n1p1(...) ... (needs --force) [read-only: blk.ro]
sdb          466 GiB  ST500DM002-1BD142        holds WINDOWS (protected: needs --force and a typed sentence): partitions sdb1(EFI) ...
$ sudo install --dry-run nvme0n1                 # the plan, nothing written
$ sudo install --force nvme0n1                   # asks you to type "nvme0n1", then writes
$ sudo install --force sdb                       # lists what would be lost, asks for "ERASE WINDOWS ON ST500DM002-1BD142 sdb"
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
finished; a write error stops the run at once and names the place (`writing sdb failed at byte 1064960 (sector 2080, the FATs of
TOUHOUOS-ESP): Input/output error`). Afterwards the device cache is flushed (`BLKFLSBUF`; a failed flush fails the install) and **every
byte written is read back and compared**: each write goes through a log that keeps a copy of the bytes (a pointer for the file data, a
length for zero fills), and the read-back covers the four files, the three directories, both FATs of both volumes, the reserved areas
(boot sector, FSInfo, their backups at sectors 6 and 7), both GPT entry arrays and headers and the protective MBR. Each area that differs
or cannot be read is listed (`READ-BACK MISMATCH: the FATs of HUMANVILLAGE, 512 bytes at byte 68173824 (sector 133152): ...`) and the install
fails with `*** sdb DOES NOT READ BACK AS WRITTEN ***` and exit status 1. On a 238 GiB SSD the read-back is about 75 MB (mostly the two
HUMANVILLAGE FATs, which are zeros, and the 8 MB of boot files), a second or two. `cmdline.txt` names the new disk.

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
   **A disk that holds another operating system is protected even from `--force`** (next section): it needs `--force` *and* a typed sentence.
6. **Big enough** for both FAT32 volumes (FAT32 needs at least 65525 clusters).
7. **The plan is shown and the name typed**: the prompt is read from `/dev/tty`, so piped input cannot answer it; anything but the exact
   name stops. For scripts `--yes-i-typed=NAME` gives the answer in advance and must be the exact name too. Without a terminal and without
   that option `install` refuses.
8. Only then: `open(/dev/DISK, O_RDWR)`, a refusal if the medium itself is write-protected, `BLKWRUNLOCK`, the writes, `BLKWRLOCK`,
   the read-back.

### Other operating systems: `--force` is not enough

The owner's Dell holds an activated Windows 11 that must not be lost (owner constraint of 2026-10-24 in
[CLOUD_AGENT_BRIEF.md](CLOUD_AGENT_BRIEF.md)), so a disk that holds another operating system is not erased by `--force` alone. `install`
reads the partition table itself (the primary GPT, the backup GPT in the last sector when the primary header is gone, otherwise the MBR
with its chain of logical partitions, otherwise the whole disk as one volume) and the first 72 KiB of every partition (`other_systems()` in
`install.c`). It recognises:

| System | By partition type | By what the partition holds |
|--------|-------------------|-----------------------------|
| WINDOWS | GPT: Microsoft reserved `E3C9E316-...`, Windows recovery `DE94BBA4-...`, LDM metadata `5808C8AA-...` and data `AF9B60A0-...`, Storage Spaces `E75CAF8F-...`, and **Microsoft basic data `EBD0A0A2-...` unless it holds a FAT file system**; MBR: `07` (unless FAT), `27`, `42` | NTFS (`NTFS    ` at byte 3), exFAT (`EXFAT   `), ReFS (`ReFS`), BitLocker (`-FVE-FS-`) |
| LINUX | GPT: Linux filesystem `0FC63DAF-...`, root x86-64/x86/ARM64, `/usr`, `/home`, `/srv`, `/var`, swap, LVM, RAID, LUKS, extended boot, GRUB BIOS boot `21686148-...`; MBR: `82`, `83`, `8E`, `FD` | ext2/3/4 (`53 EF` at byte 1080), XFS, btrfs, Linux swap (`SWAPSPACE2` at 4086), LVM (`LABELONE`), LUKS |
| MACOS | GPT: HFS+ `48465300-...`, APFS `7C3457EF-...`, Apple boot; MBR: `AF` | HFS+, APFS |
| RAID | | Intel RST metadata (`Intel Raid ISM Cfg Sig.`) in the last sectors: the disk is a member of a firmware RAID set |
| UNREADABLE DATA | | a partition, a GPT entry array or a logical-partition chain that cannot be read: what cannot be seen is treated as precious |

The basic data exception keeps sticks, cameras and an earlier TouhouOS install (HUMANVILLAGE is a FAT32 basic data partition) at plain
`--force`; Windows always has an MSR and an NTFS C: next to its FAT ESP, so a Windows disk is caught either way. An EFI System Partition by
itself is not a sign of anything (`install` does not look inside it for `EFI/Microsoft/Boot`).

For such a disk `install` first lists everything that would be lost, partition by partition (number, GPT name, size, type, file system and
label, and which system it belongs to):

```
install: sdb (256 MiB, WIN-SSD) holds WINDOWS. Everything on it would be lost:
install:   partition 1  "EFI system partition"            40 MiB  EFI System Partition, FAT32 "SYSTEM"
install:   partition 2  "Microsoft reserved partition"    16 MiB  Microsoft reserved  <- WINDOWS
install:   partition 3  "Basic data partition"           150 MiB  Microsoft basic data, NTFS  <- WINDOWS
install:   partition 4                                    48 MiB  Windows recovery, NTFS  <- WINDOWS
```

and then goes on only with **the sentence**, typed exactly (case, spaces and all):

```
ERASE <SYSTEMS> ON <model> <disk>          e.g.  ERASE WINDOWS ON SAMSUNG MZVLB256HAHQ-000L7 nvme0n1
                                                 ERASE WINDOWS AND LINUX ON WDC WD10EZEX-08WN4A0 sda
                                                 ERASE WINDOWS ON vda          (a disk without a model name)
```

`<SYSTEMS>` lists every system found, joined by `AND`, in the order WINDOWS, LINUX, MACOS, RAID, UNREADABLE DATA; `<model>` is the model
the disk reports (as in `lsblk`), `<disk>` its name. The sentence makes the user read the disk's model and the system's name aloud, so a
wrong disk or a Windows disk the user did not know about stops the install.

| Situation | Result |
|-----------|--------|
| no `--force` | refused; the list and the sentence are shown |
| `--force --yes-i-typed=DISK` (no sentence) | refused: `--force and --yes-i-typed are not enough`, with the `--erase-phrase='...'` to use |
| `--force --erase-phrase='...'` that differs in anything (case, a space, another system) | refused |
| `--force` at a terminal | the list, then `To go on, type this sentence exactly`; anything else typed stops it |
| `--force --dry-run` | the list, the plan, and the sentence it would ask for |
| `--force` and the exact sentence (typed, or `--erase-phrase=`) | `the sentence matches: WINDOWS on sdb will be erased`, then the usual install |
| `--erase-phrase=` for a disk without another system | refused (a sentence for the wrong disk is a mistake) |

The other refusals (the boot disk, a mounted disk, a partition name) come first and are not affected by any sentence.

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

| `install_other_systems` | a Windows-like disk (ESP, MSR, NTFS basic data, NTFS recovery), the same with BitLocker on C:, a Linux disk (ESP, ext4 root, swap, ext4 /home) and a whole-disk NTFS volume on virtio: the listing marks all four protected; each is refused without `--force`, with `--force --yes-i-typed`, with a lower-case or otherwise altered `--erase-phrase`, with the sentence of another system; `--dry-run` lists every partition with name, size, type and file system; four wrong answers typed at the prompt stop it; no unlock happens and every image stays byte-identical; then the Windows disk is installed with the sentence typed at the prompt and the Linux disk with `--erase-phrase`, and both check clean |

The installer's failure paths are tested with injected disk errors in `tests/case_blkerr.py` (`python tests/run_all.py blk_err_install`):
`blk_err_install_write` (a write error in the ESP's FAT on SATA: install stops there, names the place, the GPT and MBR are never written; a failing
final flush on NVMe fails the install) and `blk_err_install_readback` (six writes silently lost through the `ahci.losewrite=` knob: the MBR, the backup
GPT header, the ESP's reserved area, its second FAT, `kernel.elf`, HUMANVILLAGE's FAT; all six are named and the install fails). See
[HARDWARE_AUDIT.md](HARDWARE_AUDIT.md), section 5a.

Results on the installer branch (Ubuntu 24.04, QEMU 8.2, TCG, `TOUHOU_TIMEOUT_SCALE=2`): all eight pass; the regression groups named in the report of
the branch pass as well (see the pull request). The write-path review branch added `install_other_systems` and the `blk_err_install_*` tests; results
in its pull request.

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
* The recognition of other systems reads partition types and signatures; it does not look inside an ESP (a disk that holds nothing but an
  ESP with `EFI/Microsoft/Boot` is not protected), into an LVM volume group or a RAID set, and does not know FreeBSD, ChromeOS or other rarer
  partition types (those count as data and need only `--force`). A disk whose both GPT headers are destroyed is judged by its MBR, which on a
  GPT disk is the protective one: then only the file system signatures at the start of the disk are left. The real Windows 11 layout on the
  Dell has not been seen yet (DUALBOOT.md part D will show it).
* There is no mode that keeps the other system (install into free space, task 19); until it exists the sentence is the only way, and it
  erases the whole disk.

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
| `tests/case_blkerr.py` | the installer under injected disk errors (`blk_err_install_*`), with the drivers' and FAT's error tests |
| `initrd/.../patchouli/1/install.pw`, `5/proc.pw` | manual pages (`install(1)`, `/proc/bootdisk` in `proc(5)`) |

## State when paused

Done and tested in QEMU: the tool, the unlock, `blk.rw=` (one partition), `/proc/bootdisk`, the eight tests, the manual page. Not done: anything on real
hardware (the owner's Dell needs AHCI mode first and a decision about its internal disk), the mount-policy preference for a writable
HUMANVILLAGE volume (Known gaps), an upgrade-in-place mode.

Write-path review (task 13, branch `cloud/write-path-review`): the protection of disks with another operating system (the sentence), the full read-back,
a failed final flush failing the install and the failure message with the place are done and tested (`install_other_systems`, `blk_err_install_write`,
`blk_err_install_readback`); `install(1)` describes them. Nothing of it has run on real hardware.

## How to resume

1. Merge after `cloud/staging` (task 2): the shared-file edits are small (`blk.c` about 20 lines, `blk.h` 8, `bootinfo.h` 16, `boot/main.c` 35,
   one line each in `ahci.c`/`nvme.c`), new files otherwise. Rerun `install_`, `blk_`, `image_`, `storage`, `usbstor`, `manual`.
2. On the Dell: back up, decide what happens to Windows, switch SATA to AHCI, boot the stick, `sudo install` (the listing), `sudo install --dry-run DISK`,
   then the real thing; photograph `/proc/bootdisk` and the listing if anything looks wrong. Then remove the stick and start from the disk.
3. Task 5 (updates): `/proc/bootdisk` tells the updater which ESP is the live one. The installed ESP stays read-only under `blk.ro` on purpose,
   so the updater needs a root-only way to mount it read-write for the moment of the update, scoped like the write unlock (one partition,
   released at unmount, never persisted); that mechanism does not exist yet.
4. Task 19 (install into free space next to Windows) can reuse `other_systems()` to show what stays, and the write log and `read_back()` as they
   are (they only compare what was written); it must additionally prove that nothing outside its own partitions and the two GPT copies changed.
   On the Dell, when the internal disk becomes visible: `sudo install` (the listing) must say `holds WINDOWS (protected ...)` for it; if it does
   not, stop and send the output of `install --force --dry-run DISK`.
