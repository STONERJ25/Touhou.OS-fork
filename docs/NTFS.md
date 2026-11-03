# NTFS (read only) and BitLocker with a clear key

The owner's Dell holds their Windows 11 on partitions 1 to 3 of the NVMe disk, next to TouhouOS (partitions 4 and 5). Partition 3 is `C:`: NTFS inside a **BitLocker** container whose only key protector is a
*clear key* (BitLocker is "suspended", see [DUALBOOT.md](DUALBOOT.md) and `tools/bde-probe.py`). This page describes the driver that lets TouhouOS **read** those files: what exists, how it is built,
what was checked against what, and what to try on the real machine. It never writes: there is no write path of any kind in the code, and none will be added.

```
 user space    mount -t ntfs -o ro /dev/nvme0n1p3 /mnt/windows        ls  cat  cp  md5sum  the explorer (it lists every mount)
 --------------------------------------------------------------------------------------------------------------------------------
 VFS           kernel/fs/ntfs.c       type "ntfs": mount options, vnodes (one per file), lookup/readdir/read/readlink, statfs; every write operation answers EROFS
 parser        kernel/fs/ntfs_core.c  boot sector, MFT records and fixups, attributes, $ATTRIBUTE_LIST, run lists, LZNT1 units, B+tree directories, reparse points
               kernel/fs/ntfs_name.c  UTF-16 <-> UTF-8, $UpCase collation, FILETIME;  ntfs_lznt1.c  the LZNT1 decompressor;  ntfs_priv.h  structures and ceilings
 BitLocker     kernel/fs/bitlocker.c  AES, CCM, XTS, CBC, Elephant diffuser; the FVE metadata; the clear-key unlock; the decrypted view as /dev/bdeN
 block layer   kernel/drivers/blk.c   (unchanged) partitions, /dev nodes; the internal disks stay read-only under `blk.ro`
```

The parser (`ntfs_core.c`, `ntfs_name.c`, `ntfs_lznt1.c`, `bitlocker.c`) knows nothing of the kernel: it reads through a callback and allocates through macros, so `tests/host/ntfstool.c` builds **the same sources** for the host and runs them
under AddressSanitizer and UBSan. The type registers itself at boot (`INITCALL` in `ntfs.c`): `vfs.c`, `mountpolicy.c` and `tools/build.py` were not touched.

## Using it

```
mkdir -p /mnt/windows
mount -t ntfs -o ro /dev/nvme0n1p3 /mnt/windows           # root only; BitLocker is recognised by the first sector and unlocked on the way in
ls -l /mnt/windows ; umount /mnt/windows
```

| Option | Meaning |
|--------|---------|
| `ro` | accepted (the mount is read only whatever is given); **`rw` is refused** with `EROFS` and a log line |
| `uid=N`, `gid=N` | owner of every file (default root) |
| `fmask=OCT`, `dmask=OCT`, `umask=OCT` | permission bits to remove; defaults 0333 / 0222 give files 0444 and directories 0555. For a private view of someone's documents: `-o uid=1001,gid=1001,umask=077` |
| `sysfiles` | also list and open the metadata files (`$MFT` ... record 15). Hidden otherwise, by name too |
| `showdos` | also list the 8.3 twin names (hidden by default; they always resolve) |
| `nobitlocker` | do not try to unlock a BitLocker volume (the mount fails with `EACCES`) |
| `defaults noatime relatime nodev nosuid noexec exec suid dev auto noauto user async quiet utf8` | accepted and ignored |

The mount applet's `-t` defaults to `fat32` for `/dev/...` sources, so `-t ntfs` is needed (that applet is not part of this change).

## What is supported, and what is not

* **Supported:** NTFS 1.2 to 3.1 volumes with 512 to 4096-byte sectors, clusters of 512 bytes to 2 MiB, MFT records and index blocks of 512 bytes to 64 KiB (sub-node numbers in clusters or, for blocks smaller than a cluster, in 512-byte units);
  update sequence arrays (always 512-byte strides); resident and non-resident data; run lists with sparse runs; `$ATTRIBUTE_LIST` with extension records; a fragmented `$MFT`; files with an uninitialised tail; **LZNT1 compression**
  (units of 2^n clusters, stored plain, compressed or sparse); sparse files; B+tree directories of any size; the Win32/DOS/POSIX name spaces (the long name is listed once; the 8.3 twin still resolves); case-insensitive lookup through the
  volume's own `$UpCase`; time stamps; hard links (same inode); **symbolic links and junctions** as symlinks (`\??\C:\x` is taken to be on this volume and becomes `<mount point>/x`; targets on other volumes or UNC paths give `EINVAL`);
  the volume label, version and dirty flag; `statfs`/`df` (free clusters from `$Bitmap`).
* **Refused with an error, never a crash:** EFS-encrypted files (`EACCES`); files whose data lives elsewhere, marked by a reparse tag (Windows Overlay Filter / CompactOS, cloud placeholders such as OneDrive, dedup, HSM: `ENOTSUP`);
  names that need more than 255 bytes of UTF-8 (not listed; the first is logged); compression units larger than 1 MiB.
* **Not shown:** alternate data streams, ACLs, extended attributes, `$Extend` and the other metadata files (unless `-o sysfiles`).
* **Not done, and out of scope:** writing of any kind, journal (`$LogFile`) replay (a volume that Windows left dirty, e.g. by Fast Startup, is mounted as it is on disk, with a log line), ReFS, exFAT, mounting at boot, BitLocker with a TPM, PIN,
  password, startup key or recovery password.

## The on-disk structures as implemented

Field offsets are those of the libfsntfs "New Technologies File System (NTFS) format" document; `tests/host/ntfsimg.py` repeats each one next to the place it writes it.

| Structure | What the parser reads |
|-----------|-----------------------|
| boot sector | `NTFS    ` at 3; bytes per sector 11 (u16); sectors per cluster 13 (u8: 1..128 power of two, or 244..255 meaning 2^(256-n)); the FAT-era fields at 14, 16, 17, 19, 22, 32 must be 0; total sectors 40 (u64, at most the device's); `$MFT` cluster 48; MFT record size 64 and index block size 68 (signed: n > 0 clusters, n < 0 means 2^-n bytes); serial 72; 0x55AA at 510 |
| MFT record | `FILE` at 0; update sequence array offset 4 and count 6 (count-1 must be size/512); sequence 16; link count 18; first attribute 20; flags 22 (1 in use, 2 directory); used size 24; base record 32 (non-zero: an extension record); record number 44 (NTFS 3.1: a mismatch is logged once, not refused) |
| attribute | type 0 (u32); length 4 (multiple of 8, within the used part); non-resident flag 8; name length 9 and offset 10; data flags 12 (0x1 compressed, 0x4000 encrypted, 0x8000 sparse); instance 14. Resident: value length 16, offset 20. Non-resident: lowest VCN 16, mapping pairs offset 32, compression unit (log2) 34, allocated 40, size 48, initialised size 56 |
| run list | header byte: low nibble = bytes of length, high nibble = bytes of the signed LCN difference (none: sparse); every run is checked to lie inside the volume; at most 262144 runs per stream |
| `$ATTRIBUTE_LIST` | entries: type 0, length 4, name length 6 and offset 7, lowest VCN 8, file reference 16, instance 24; walked once, at most 65536 matching entries; the extension record must name this file as its base |
| `$STANDARD_INFORMATION` | 0 creation, 8 modification, 16 MFT change, 24 access (FILETIME), 32 flags. `mtime` = modification, `ctime` = MFT change |
| `$FILE_NAME` | parent 0, flags 56, reparse tag 60, name length 64, name space 65, name 66 |
| `$INDEX_ROOT` / `INDX` | root: type 0 (0x30), collation 4 (1), block size 8, then the node header at 16; INDX: `INDX` at 0, array 4/6, VCN 16, node header at 0x18. Node header: first entry 0, used 4, allocated 8, flags 12. Entry: reference 0, length 8, key length 10, flags 12 (1 sub-node, 2 last), key at 16, sub-node VCN in the last 8 bytes |
| reparse point | tag 0, data length 4; symlink (0xA000000C): offsets/sizes at 0..7, flags 8 (1 relative), names at 12; junction (0xA0000003): names at 8 |
| LZNT1 | chunks with a 16-bit header (bit 15 compressed, bits 14..12 = 3, low 12 bits = size - 3 counting the header); flag bytes (LSB first) of literals and 2-byte copy tokens; the displacement/length split of a token depends on how much the chunk has produced (MS-XCA 2.5) |

Directory walks use an explicit stack (`NtfsDirCursor`, depth at most 24) and a count of the blocks the index can hold, so a loop in the tree ends in `EIO`. A directory that is read twice is read from the disk once (32 records and 8 index blocks are cached);
a per-file window of 64 KiB serves small sequential reads.

## BitLocker with a clear key

`mount` looks at the first sector. `-FVE-FS-` at byte 3 means BitLocker (Windows 7 and later layout), and `bitlocker.c` then:

1. reads the three FVE metadata blocks (byte offsets at 176, 184, 192 of the first sector; 64-byte block header: version 2 at 10, encrypted volume size at 16, number of relocated header sectors at 28, "volume header offset" at 56; 48-byte metadata header at 64; entries
   of (size, entry type, value type, version) from 112), and uses the first valid copy;
2. lists the VMK entries (entry type 2, value type 8: GUID 0, time 16, **protection type at 26**, nested properties from 28). Type 0 is the clear key. Without one it **stops and says which protectors there are** (`EACCES`);
3. opens the clear-key VMK entry: its nested *key* property (value type 1: method u32, then the clear key) opens, with **AES-CCM** (12-byte nonce, 16-byte tag stored first, no associated data), the nested *AES-CCM encrypted key* (value type 5: nonce 0, tag 12, data 28) which holds
   the VMK; the VMK opens the FVEK entry (entry type 3, value type 5) the same way; both plaintexts are `[size u32][version u16][unknown u16][method u32][key]`;
4. sets up the sector cipher from the method: 0x8004/0x8005 **AES-XTS** 128/256 (tweak = sector number, 16 bytes little endian; the key is both keys), 0x8002/0x8003 AES-CBC (IV = AES(key, byte offset of the sector as 16 bytes)),
   0x8000/0x8001 AES-CBC + **Elephant diffuser** (Ferguson 2006: CBC, diffuser B, diffuser A, xor with the sector key, all spelled out in the paper and in `bitlocker.c`);
5. **proves the key**: logical sector 0, which BitLocker keeps encrypted at the "volume header offset" (the first 16 sectors of the volume were moved there), must decrypt to an NTFS boot sector (`NTFS    ` and 0xAA55). If it does not, the plausible alternatives
   are tried (the second XTS key at the other possible place in a padded key blob; the relocated sectors tweaked by where they are stored instead of where they belong) and the log says which one worked; if none does, the log shows the 16 bytes at offset 3 of what came out;
6. registers the decrypted view as **`/dev/bdeN`** (read only, size of the partition, `parent` = the partition) and the NTFS driver mounts that. Sectors at or past the "encrypted volume size" are passed through as they are (a partly converted volume). `umount` removes the view
   and wipes the key.

The log lines (`dmesg | grep -E 'bitlocker|ntfs'`):

```
[Keine] vda: bitlocker: unlocked with the clear key: AES-XTS 128 (method 0x8004), metadata copy 1 of 3, 16 volume header sectors relocated to 0x17afe00, encrypted size 23 MiB, protectors: clear key
[Marisa] blk: bde0  bitlocker  24 MiB, 512-byte sectors, read-only  BitLocker view of vda
[Keine] ntfs: vda via bde0 mounted on /mnt/windows, 23 MiB, 4096-byte clusters, label 'TESTVOL', NTFS 3.1, read-only (this driver cannot write)
```

## Safety rules

* **Read only for ever.** The vnode operations are lookup, readdir, read and readlink; create, mkdir, unlink, rename, write, truncate, chmod, chown and the time setters exist only as stubs that return `EROFS`. The parser's device interface is a read callback and nothing else;
  the BitLocker view has no write function and is marked read only. `tests/host/test_ntfs.py::Parser::test_the_source_has_no_way_to_write` fails if the sources mention `blk_write`, `blk_flush`, a write method or `fwrite`.
* Files are shown as 0444 and directories as 0555, so an ordinary user cannot even open one for writing. **Root can**: the VFS does not look at a read-only mount when a file is opened, so `echo x > file` as root opens (and, with `>`, asks for a
  truncate that is answered `EROFS`) and then every write fails with `Read-only file system`; some applets (`echo`) do not turn that into an exit status. Nothing on the disk changes either way (the QEMU test compares checksums).
* **`blk.ro` stays the default for the internal disks.** This driver does not need it lifted and nothing here asks for it.
* **The mount policy never mounts a Windows volume by itself.** `mountpolicy.c` only reads the boot sector of FAT-typed partitions to look for a label, and an NTFS or BitLocker boot sector has no FAT fields; nothing mounts `ntfs` but root's `mount`. The test
  `ntfs_mount_read_copy_and_refuse_every_write` checks `/proc/mounts` before the mount and the image's checksum after the guest has gone.
* **The volume is not trusted.** Every length, offset, count and run is checked; loops end in `EIO`; nothing recurses on volume data; ceilings: 262144 runs per stream, 8 MiB of attribute list, 65536 list entries, 24 index levels, 1 MiB per compression unit.
  A damaged file gives `EIO` for that file; the volume stays mounted.

## Tests

Host (`python tools/hosttest.py discover -s tests/host -p "test_ntfs.py"`, needs gcc and the `cryptography` package, both in WSL's Ubuntu; about 3 minutes):

| Group | What it shows |
|-------|---------------|
| `Lznt1` | the MS-XCA worked example; 17 streams made by **Windows' own RtlCompressBuffer** (`tests/host/ntfs_lznt1_vectors.py`, both engines); the generator's compressor read by the C and the Python decompressor; damaged streams and 300 random mutations |
| `GeneratorAndPythonReader` | the generator's volumes read by the separately written `tools/ntfs-look.py` |
| `Parser` | every feature on generated volumes, compared with what the generator put in (kind, size, inode, time, crc32 of the content): resident/fragmented/sparse/compressed/`init < size`, ADS hidden, hard links, symlinks, junctions, EFS, WOF; Unicode and case; names too long for the VFS; 3000-entry directory; 512-byte index blocks on 4096-byte clusters, 512-byte clusters, 64 KiB clusters, 4Kn, 8 KiB blocks, 4096-byte records; attribute lists with extension records and a fragmented `$MFT`; old-style record headers; metadata files hidden / readable with `--system` (`$MFT` reads as the generator's MFT, `$UpCase` as the table); DOS names; label, version, free clusters; caching |
| `Hostile` | boot sector fields, record 0, attribute headers, one damaged file among good ones, loops in a three-level index, truncated devices, runs outside the volume, absurd sizes, garbage compressed data, attribute list damage, index entry damage |
| `Fuzz` | thousands of random mutations of whole volumes (bit flips, extreme field values, copies, truncation), biased to the boot sector, the MFT and the first megabytes, each mount-and-walk limited to 20 s, under ASan and UBSan |
| `BitLocker` | FIPS-197 vectors; AES blocks, CCM, XTS (also IEEE 1619 vector 2) and CBC against the **`cryptography` package**; the Elephant diffuser against the paper's encryption direction; all six methods end to end; the decrypted view equals the plain volume sector by sector; layouts a real volume might use; a partly encrypted volume; refusals (no clear key, a key that does not open, sector 0 that is not NTFS); the Python reader unlocks the same volumes; fuzz of the metadata |

QEMU (`python tools/qemu-slot.py -- python tests/run_all.py ntfs`): `tests/case_ntfs.py`.

| Test | Covers |
|------|--------|
| `ntfs_mount_read_copy_and_refuse_every_write` | the mount policy leaves the disk alone; `mount -o rw` refused; listing, links, case, `md5sum` and `cp` against the host's values (fragmented, compressed, sparse), 300-entry directory, EFS refused, `df`, twelve kinds of write all `Read-only file system`, `..` through the mount point, busy while inside, `umount`; image unchanged |
| `ntfs_bitlocker_clear_key_is_unlocked_and_read_only` | unlock with TPM + clear key listed, `/dev/bde0` appears, shows decrypted NTFS, rejects writes, blocks a second mount, goes away at `umount`; image unchanged |
| `ntfs_bitlocker_without_a_clear_key_stays_locked` | TPM + recovery protectors only: the mount fails, the log names them, no `bde` device, image unchanged |
| `ntfs_damaged_volumes_do_not_hurt_the_kernel` | a damaged file record among good ones, a bad boot sector, a damaged `$MFT` |

### What was verified only against my own generator

The generator (`ntfsimg.py`), the Python reader (`ntfs-look.py`) and the driver were written by the same hand from the same documents, so a shared misreading would pass every test above. The spots where that could hide:

* **The BitLocker metadata layout**, apart from the facts that `tools/bde-probe.py` already confirmed on the real Dell volume (signature, the three offsets at 176, metadata header at 64, entries from 112, VMK entries with the protection type at 26): the nested property layout
  of a clear-key VMK entry (which value types hold what), the CCM blob layout (nonce, tag, data), the decrypted key structure (12-byte prefix), the FVEK blob length (32 bytes for XTS-128 assumed; padded layouts are tried), **the tweak of the relocated header sectors** (logical position assumed,
  physical position tried as a fallback), and the meaning of "encrypted volume size". The ciphers themselves are checked against independent code.
* **NTFS details a real Windows volume might do differently from my reading**: the record number field check (relaxed to a warning for this reason), `$UpCase` handling (the volume's own table is used), junction targets, `$ATTRIBUTE_LIST` entry matching by instance, directories whose index
  tree Windows balances differently from my generator (the walk makes no assumption about balance), "used space only" BitLocker encryption (free space is not encrypted, harmless for reading files; not tested).
* **Independent checks that do exist:** LZNT1 against Windows' compressor; AES/CCM/XTS against `cryptography`; the fixup, run list and index structures against the Python reader (same author, different code); the on-disk offsets against the libfsntfs and libbde documents (re-read twice).

## Trying it on the Dell (the lead's, read only)

Nothing below writes. The internal disk is already read-only (`blk.ro`). Do the steps in order and stop at the first surprise; every step prints its reason in `dmesg`.

```
sudo dd if=/dev/nvme0n1p3 bs=512 count=1 2>/dev/null | xxd | head -2      # byte 3 must read -FVE-FS-
python tools/bde-probe.py                                                 # (dev PC) the protectors: a clear key (0x0000) must be listed; if Windows has re-activated BitLocker since 2026-10-25 there is a TPM protector and no clear key
sudo mkdir -p /mnt/windows
sudo mount -t ntfs -o ro,uid=1001,gid=1001,umask=077 /dev/nvme0n1p3 /mnt/windows
dmesg | grep -E 'bitlocker|ntfs|bde0'                                     # the unlock line, the view, the mount line
ls -l /mnt/windows ; ls -l /mnt/windows/Users ; df /mnt/windows ; lsblk
sudo dd if=/dev/bde0 bs=512 count=1 2>/dev/null | xxd | head -3          # decrypted: "NTFS" at byte 3
cp /mnt/windows/Users/<name>/Documents/<file> /tmp/ ; md5sum /mnt/windows/Users/<name>/Documents/<file> /tmp/<file>
sudo umount /mnt/windows                                                  # bde0 disappears
```

What a failure looks like (all in `dmesg`, tag `[Keine]`):

| Log line | Meaning | Next step |
|----------|---------|-----------|
| `bitlocker: protectors: TPM, ...; there is no clear-key protector` | BitLocker is on again | nothing to unlock without the owner's key; suspend protection in Windows (`manage-bde -protectors -disable C: -RebootCount 2`) and retry |
| `none of the three FVE metadata copies ... is readable` | the metadata offsets or block layout differ from the document | send the first sector and the 64 KiB at the first offset (`dd`), compare with `tools/bde-probe.py` |
| `the clear key did not open the volume key` | the nested property layout or the CCM blob layout differs | dump the VMK entry (the bytes after the protection type) |
| `... but sector 0 of the decrypted volume is not an NTFS boot sector (bytes 3..18: ...)` | the volume key opened, but the cipher, the key layout or the tweak of the relocated header is not what was assumed | the hex shows what came out; try `sudo dd if=/dev/nvme0n1p3 bs=512 skip=<hdr offset/512> count=16` and decrypt offline with `tools/ntfs-look.py` |
| `ntfs: ... is not a mountable NTFS volume: <reason>` | decrypted fine; the NTFS side refused (the reason is exact) | `mount ... ; dmesg` and the MFT record named in the line |
| `ntfs: MFT record N: damaged update sequence array` while listing | a torn record (dirty volume) or the wrong tweak for a region | compare with `chkdsk` state; try another directory |
| the mount hangs | the NVMe driver, not this one: `cat /proc/nvme` counters | |

If the volume is marked dirty the log says so (Fast Startup hibernation leaves it that way); the view is of the disk as it is, and the journal is not replayed.

## The first try on the Dell (2026-11-03, the lead, read only)

Build `3fdee2b` on the Dell; the plan above in order, nothing written. `tools/bde-probe.py` still found only a clear-key protector (no TPM protector, no recovery password) in the metadata copies at `0x59758000`, `0x59768000` and `0x87970000`.
The kernel log:

```
[Keine] nvme0n1p3: bitlocker: unlocked with the clear key: AES-XTS 128 (method 0x8004), metadata copy 1 of 3, 16 volume header sectors relocated to 0x134e5000, encrypted size 243569 MiB, protectors: clear key (key layout 0, physical tweak)
[Marisa] blk: bde0  bitlocker  57213 MiB, 512-byte sectors, read-only  BitLocker view of nvme0n1p3
[Keine] ntfs: nvme0n1p3 via bde0 mounted on /mnt/windows, 57212 MiB, 4096-byte clusters, label '', NTFS 3.1, read-only (this driver cannot write)
[Keine] ntfs: nvme0n1p3 unmounted (445 device reads in all)
[Marisa] blk: bde0 removed
```

What was checked: the decrypted sector 0 reads `eb 52 90 4e 54 46 53` (NTFS at byte 3); the top directory lists with its owners and dates, `Documents and Settings` shows as a link to `/mnt/windows/Users` (a junction), hibernation and page files appear with their real sizes (`hiberfil.sys` is 3.4 GB, so Windows' hibernation or Fast Startup file exists; the driver reads the disk as it is and replays no journal); `df` says 58586108 KiB, 62 % used;
`ntoskrnl.exe` (12076528 bytes, many runs) begins with `MZ` and `cp` plus `md5sum` of the copy and of the original agree (`133a2649ba183dc8e6a5669a9097fe77`, read twice). Nothing was opened beyond `/`, `/Users` (names only) and two files under `Windows/System32`.
`encrypted size 243569 MiB` in the metadata is larger than the partition (57213 MiB); the view has the partition's size, nothing reads past it, and why the two differ was not looked into.
Not tried: a large sequential read (throughput), a compressed file, a directory with thousands of entries, the owner's own folders, a volume that is dirty.

## State when paused / how to resume

*(written 2026-11-03, when the owner's usage ended; the branch is `agent/ntfs`)*

**Done and committed:** the parser, the VFS glue, LZNT1, the BitLocker layer (all six sector methods, clear key), the generator, the Python reader, the host tool, the host test suite, the QEMU test file, and this page. Both programs were run by hand in QEMU: mount, list,
read, md5, symlinks and junctions, EFS refused, every write refused, umount; the BitLocker volume unlocked with `bde0` appearing and disappearing.

**Test status at the pause:** the host suite `tests/host/test_ntfs.py` passes in full (48 tests, about 3 minutes in WSL: `python tools/hosttest.py discover -s tests/host -p "test_ntfs.py"`). The QEMU file `tests/case_ntfs.py` (4 tests, `python tools/qemu-slot.py -- python tests/run_all.py ntfs`):
`ntfs_bitlocker_without_a_clear_key_stays_locked` and `ntfs_damaged_volumes_do_not_hurt_the_kernel` passed on the first run; the other two failed only in test-side expectations (a shell builtin and the `dd` applet do not turn EROFS into an exit status), were fixed, and
passed on the rerun (26 s and 9 s): **all four pass**. `main` was merged into the branch last (clean, no conflicts) and the merged tree builds (`python tools/build.py`, 22 s).

**Not done:** throughput numbers (sequential read in QEMU against the raw device); the hardware try on the Dell (the lead's job, section above); a mutation fuzz of whole BitLocker volumes longer than the 120 iterations in the test; Explorer and the `mount` applet were not touched
(the applet needs `-t ntfs`; it could guess the type from the boot sector).

**Next steps, in order of value:** (1) run the three-step Dell plan and put the exact log lines into this page; (2) if the unlock fails, the log's hex of the decrypted sector 0 and the fallbacks in `bde_open()` tell which assumption was wrong; (3) update [STORAGE.md](STORAGE.md), which still says "NTFS is
not supported" (not my file); (4) optionally show 8.3 names for long UTF-8 names, and read WOF-compressed files (XPRESS/LZX streams in `WofCompressedData`).

**Files** (all mine): `kernel/fs/ntfs.c`, `ntfs_core.c`, `ntfs_name.c`, `ntfs_lznt1.c`, `ntfs_priv.h`, `bitlocker.c`, `kernel/include/ntfs.h`, `bitlocker.h`, `tests/host/ntfsimg.py`, `ntfstool.c`, `test_ntfs.py`, `ntfs_lznt1_vectors.py`, `tests/case_ntfs.py`,
`tools/ntfs-look.py`, `docs/NTFS.md`. No file of anyone else was changed.
