# ext2, ext3 and ext4 (read only), and a Linux root on a disk

Linux disks and Linux-formatted sticks are ext4. TouhouOS read FAT (its own volume) and NTFS (the Windows partition of the Dell, [NTFS.md](NTFS.md)) and nothing else, so a Linux disk was a closed box, and the Linux root of the
compatibility work ([COMPAT.md](COMPAT.md), "The Linux root") lived in RAM, where Python's library and gcc with its headers (a few hundred MB) do not fit. This page describes the driver that reads **ext2, ext3 and ext4**
volumes: what exists, how it is built, what was checked against what. It never writes: there is no write path of any kind in the code, and none will be added.

```
 user space    mount -t ext4 -o ro /dev/vdb1 /linux        linux /bin/ls /     ls  cat  cp  sha256sum  lsblk (FSTYPE)
 --------------------------------------------------------------------------------------------------------------------------
 VFS           kernel/fs/ext4.c       types "ext2", "ext3", "ext4" (one driver, three names): mount options, one vnode per inode, lookup/readdir/read/readlink,
                                      statfs; every operation that would write answers EROFS
 parser        kernel/fs/ext4_core.c  superblock, group descriptors, inodes, extent trees and block maps, directories, inline data, symbolic links;  ext4_priv.h  ceilings
 block layer   kernel/drivers/blk.c   (a few lines added) partitions, /dev nodes, and the filesystem name of a Linux-typed partition for lsblk
```

The parser (`ext4_core.c`) knows nothing of the kernel: it reads through a callback and allocates through macros, so `tests/host/ext4tool.c` builds **the same source** for the host and runs it under AddressSanitizer and
UBSan. The three types register themselves at boot (`INITCALL` in `ext4.c`); the mount policy (`mountpolicy.c`) never mounts one.

## Sources, and what was checked against what

The format is taken from the public description of the on-disk layout (the kernel.org "ext4 Disk Layout" document, and the manual pages `ext4(5)` and `mke2fs(8)` of e2fsprogs). No source of Linux, e2fsprogs, FreeBSD or any other project was read.
The *programs* `mke2fs`, `dumpe2fs`, `debugfs` and `e2fsck` of e2fsprogs 1.47 (installed in WSL's Ubuntu) are the oracle: `tests/host/ext4img.py` makes the volumes with `mke2fs -d`, `debugfs` is asked what it sees
(`stat`, `ex`, `ls`) and compared with the parser's answers, `e2fsck -fn` must call every generated volume clean, and the manifest that decides whether the driver is right is computed from the **source tree on the host**
(the files that were handed to `mke2fs -d`), not from the generator's own bookkeeping. What this cannot show, because the generator and the driver are both written to the same reading of the document, is listed at the end.

## Using it

```
mkdir -p /mnt/linux
mount -t ext4 -o ro /dev/sdb1 /mnt/linux           # root only; ext2 and ext3 are names of the same driver; the whole disk (no partition table) works too
ls -l /mnt/linux ; umount /mnt/linux
```

| Option | Meaning |
|--------|---------|
| `ro` | accepted (the mount is read only whatever is given); **`rw` is refused** with `EROFS` and a log line |
| `nosuid` | really honoured here: the set-user-id and set-group-id bits of every file read as 0 (a stick from somewhere else need not be trusted with them) |
| `noload`, `norecovery` | accepted: the journal is never replayed anyway |
| `defaults noatime relatime nodev noexec exec suid dev auto noauto user async quiet utf8 errors=* data=* barrier nobarrier discard nodiscard user_xattr nouser_xattr acl noacl commit=* nodelalloc delalloc lazytime i_version init_itable=*` ... | accepted and ignored (the words real Linux systems put in `fstab`) |

The mount applet's `-t` defaults to `fat32` for `/dev/...` sources, so `-t ext4` (or `ext2`, `ext3`) is needed. Owners and permissions are the **real ones** of each inode: the VFS checks uid, gid and mode as for any filesystem
(this is not a filesystem without owners, so `FS_FIXED_OWNER` is not set). A volume is **never** mounted by the mount policy; only root's `mount` does it.

## What is supported, and what is not

* **Supported:**
  * superblock revisions 0 and 1; block sizes 1 KiB to 64 KiB; inodes of 128 bytes and larger (`extra_isize`); block groups with 32-byte and (`64bit`) 64-byte descriptors, `flex_bg`, `meta_bg` (with `sparse_super` and `sparse_super2`),
    `bigalloc` (the free counts are in clusters, file data is mapped in blocks as always);
  * **ext2/ext3 block maps** (12 direct blocks, then single, double and triple indirect), and **ext4 extents** (trees up to depth 5; unwritten extents and holes read as zeros);
  * **inline data** (`inline_data`: small files, small directories and long symbolic links, whose bytes live in the inode: the 60 bytes of `i_block` and the `system.data` attribute of the inode body);
  * directories in the linear format with or without `filetype`; **hashed directories** (`dir_index`) are read by walking all their blocks in order and ignoring the index, which works because every leaf block is an ordinary directory block and an index block
    looks like one empty entry; names up to 255 bytes; `large_dir`;
  * symbolic links (fast, in the inode; slow, in a block or by extents; inline), device, fifo and socket inodes (listed with the right mode and `st_rdev`, content operations answer `ENXIO`), hard links (one vnode per inode),
    `large_file` and `huge_file` (files over 2 GiB and 4 GiB, sparse, 48-bit block counts), times with the extra epoch and nanosecond bits (`extra_isize`), 32-bit user and group ids (the high halves), `dir_nlink`, `statfs`;
  * a volume that **needs recovery** (`needs_recovery`, the journal holds transactions that were never written to their place) is mounted as it is on disk with a log line that says the journal was not replayed and what that can mean.
* **Accepted and ignored** (safe for a reader): extended attributes, ACLs, quotas, project ids, the journal itself, `metadata_csum` and every other checksum (**nothing is verified**, but nothing that a checksum would have protected is trusted: see the safety rules),
  `mmp`, `ea_inode`, `csum_seed`, `casefold` (names are looked up byte for byte), `verity`, `sparse_super`, `resize_inode` (the reserved descriptor blocks), `uninit_bg`/`gdt_csum` and the `INODE_UNINIT`/`BLOCK_UNINIT` group flags
  (a reader never looks at the bitmaps), every other *compat* and *ro_compat* bit, including ones that do not exist yet: the mount is read only, so a feature that only matters to a writer cannot hurt.
* **Refused with an error, never a crash:** an *incompat* feature this driver does not know (`compression`, `dirdata`, an external journal device `journal_dev`, bits that did not exist when this was written: the mount fails with `EINVAL` and the log names them);
  a volume whose geometry does not add up (see below); an **encrypted** inode (`encrypt`: the file, directory or link answers `ENOKEY` for its content; it can be `stat`ed and named by its (encrypted) entry, nothing more);
  a name that contains `/` or a NUL (not listed; the first is logged); an inode of a reserved number (3 to 10 and 1: the journal, the resize inode, ...) reached through a directory entry (`EIO`, as for any corrupt volume).
* **Not done, and out of scope:** writing of any kind, journal replay (the log line says so), verifying checksums, the volume's own hash tree (a directory is read block by block; lookups in a big one use an in-memory name index instead, see
  "Speed"), extended attributes and ACLs, quotas, encryption, compression, external journals, `sb=` (a backup superblock), `-o rw`, mounting at boot, `fsck`.

## The on-disk structures as implemented

Little endian everywhere. Offsets are bytes. The parser reads every field through byte-granular loads (`e4_le16/32/64`), never through a cast, so alignment and byte order of the host do not matter.

| Structure | What the parser reads |
|-----------|-----------------------|
| superblock | at byte 1024 of the volume, 1024 bytes: inodes count 0x00, blocks count (low) 0x04, reserved blocks 0x08, free blocks 0x0C, free inodes 0x10, first data block 0x14, `log_block_size` 0x18 (block = 1024 << n, n <= 6), `log_cluster_size` 0x1C, blocks per group 0x20, clusters per group 0x24, inodes per group 0x28, state 0x3A (bit 0 clean, bit 1 errors), **magic 0xEF53 at 0x38**, revision 0x4C, `first_ino` 0x54, `inode_size` 0x58, feature words compat 0x5C / incompat 0x60 / ro_compat 0x64, uuid 0x68, volume name 0x78, `reserved_gdt_blocks` 0xCE, `last_orphan` 0xE8, `desc_size` 0xFE, `first_meta_bg` 0x104, blocks count (high) 0x150, reserved (high) 0x154, free (high) 0x158, `min_extra_isize` 0x15C, `overhead_clusters` 0x248, `backup_bgs` 0x24C. Revision 0 means 128-byte inodes, first inode 11 and no feature words |
| feature words | compat: has_journal 0x4, resize_inode 0x10, dir_index 0x20, sparse_super2 0x200 (the others are ignored). incompat: compression 0x1 (refused), filetype 0x2, needs_recovery 0x4, journal_dev 0x8 (refused), meta_bg 0x10, extent 0x40, 64bit 0x80, mmp 0x100, flex_bg 0x200, ea_inode 0x400, dirdata 0x1000 (refused), csum_seed 0x2000, large_dir 0x4000, inline_data 0x8000, encrypt 0x10000, casefold 0x20000; any other bit is refused. ro_compat: sparse_super 0x1, large_file 0x2, huge_file 0x8, dir_nlink 0x20, extra_isize 0x40, bigalloc 0x200 (the others are ignored) |
| group descriptor | 32 bytes, or `desc_size` (64) with `64bit`: block bitmap 0x00 (ignored), inode bitmap 0x04 (ignored), **inode table 0x08**, free blocks 0x0C (u16), free inodes 0x0E (u16), flags 0x12 (ignored), and with 64 bytes the high halves: inode table 0x28, free blocks 0x2C, free inodes 0x2E. Where they are: the block after the superblock (`first_data_block + 1`), `dpb = block size / desc size` per block; with `meta_bg` the groups from `first_meta_bg * dpb` on have their descriptors in the first block of the first group of their meta group, after the superblock backup if that group has one |
| inode | `inode_size` bytes at `inode_table(group) * block + (ino-1) % ipg * inode_size`: mode 0x00 (u16), uid 0x02 and 0x78 (high), size 0x04 and 0x6C (high, regular files and, with `large_dir`, directories), atime 0x08, ctime 0x0C, mtime 0x10, gid 0x18 and 0x7A, links 0x1A, blocks 0x1C and 0x74 (high), flags 0x20, **`i_block` 0x28 (60 bytes)**, generation 0x64, `extra_isize` 0x80, ctime/mtime/atime *extra* 0x84/0x88/0x8C (low two bits: epoch, the rest: nanoseconds), crtime 0x90 and 0x94. Flags: encrypt 0x800, huge_file 0x40000, extents 0x80000, inline_data 0x10000000 |
| extent tree | `i_block` (or a block of the volume) holds a header: magic **0xF30A** (u16), entries (u16), max (u16), depth (u16), then 12-byte entries. Depth 0: leaf, first logical block (u32), length (u16: up to 32768 initialised, above that unwritten with length minus 32768), start (u16 high, u32 low). Depth > 0: index, first logical block (u32), child (u32 low, u16 high). A node must say `depth` one less than its parent, at most 5 |
| block map | `i_block[0..11]` direct, `[12]` single, `[13]` double, `[14]` triple indirect; an indirect block is `block size / 4` little-endian block numbers; 0 is a hole |
| directory entry | inode (u32), `rec_len` (u16, a multiple of 4, at least 8 + name length rounded up to 4, ends inside the block; for 64 KiB blocks 0 and 65535 mean the whole block and the other values keep their top two bits in bits 16 and 17), name length (u8), type (u8, with `filetype`: 1 file, 2 directory, 3 character, 4 block, 5 fifo, 6 socket, 7 link), name. Inode 0 is an unused slot (the 12-byte checksum tail and the index nodes of hashed directories look like that) |
| inline data | the file's bytes are `i_block` (60), then the value of the attribute `system.data` of the inode body. The body (inode offset `128 + extra_isize`, to the end of the inode) starts with the magic 0xEA020000 (u32), then entries of 16 bytes plus the name rounded up to 4: name length (u8), name index (u8, 7 = `system`), value offset (u16, from the first entry), value inode (u32, 0), value size (u32), hash (u32), name ("data"); a u32 zero ends the list. A directory's first 4 bytes are the parent inode number, then comes a list of directory entries in the next 56 bytes of `i_block` and another one in the attribute; `.` and `..` are not stored |
| special files | a device node keeps its number in `i_block[0]` (old encoding, 8 and 8 bits) or, when that is 0, in `i_block[1]` (new encoding, 12 and 20 bits); `st_rdev` is made the way the C library's `makedev()` does |
| fast symbolic link | size under 60 and no blocks of its own: the target is the first `size` bytes of `i_block` |

## Safety rules

* **Read only for ever.** The vnode operations are lookup, readdir, read and readlink; create, mkdir, unlink, rename, write, truncate, chmod, chown and the time setters exist only as stubs that return `EROFS`. The parser's device interface is a read callback and nothing else.
  `tests/host/test_ext4.py::Source::test_the_source_has_no_way_to_write` fails if the sources mention `blk_write`, `blk_flush`, a write method or `fwrite`, and the QEMU test compares the checksum of the image after the guest has gone.
  Files keep their real modes (a file with mode 0644 owned by root cannot be opened for writing by an ordinary user), but **root can open anything**: the VFS does not look at a read-only mount when a file is opened, so root's `echo x > file` opens and every write fails with `Read-only file system`.
* **The volume is not trusted.** Every length, block number, extent, count and inode number that comes from the disk is checked before it is used: the geometry of the superblock (block size, group sizes, inode size, counts against each other and against the size of the device),
  the place of every descriptor and inode table, every extent and every block pointer (inside the volume), the extent tree (magic, entry counts against the node's size, depth strictly decreasing, at most 5 levels), directory entries (record length, name length, inode number),
  inline data (against the inode), sizes (a file larger than the format can address is `EIO`). Loops end in `EIO`: a directory walk makes progress on every step, an extent tree cannot loop because the depth decreases, an indirect block chain has three levels at most.
  Nothing recurses on volume data. Memory has fixed ceilings (a 1 MiB cache of metadata blocks, a 64 KiB read-ahead window per open file, the inline data of one inode, and for a directory of 128 KiB or more a name index of at most 8 MiB, freed with the vnode). A damaged file or directory gives `EIO` for that object; the volume stays mounted.
* **The mount policy never mounts it.** `mountpolicy.c` is not touched; nothing mounts `ext4` but root's `mount`. A device cannot be mounted twice, nor can a disk and one of its partitions.
* **`blk.ro` stays the default for the internal disks.** This driver does not need it lifted. The detection of the filesystem name for `lsblk` only reads two sectors (the superblock's), of partitions that can hold a Linux filesystem (MBR type 0x83; GPT: Linux data, the
  x86-64 root, /home, /srv, /var, and "basic data", which is what some tools put on a partition that they format as ext4 anyway) and of disks without a partition table; an EFI system partition, the Windows reserved and recovery partitions and unknown types are not read.
  (docs/STORAGE.md says the policy never reads Linux-typed or NTFS-typed partitions: that sentence is about the mount policy, and still holds; this is the block layer's own look, to name a volume.)

## Speed

* **Metadata** (superblock, group descriptors, inode tables, directory blocks, extent nodes, indirect blocks) goes through a direct-mapped cache of 1 MiB per mounted volume (256 blocks of 4 KiB, 1024 of 1 KiB, 16 of 64 KiB). **File data** does not: a read
  of 32 KiB or more goes straight from the device into the caller's buffer, in one request per run of contiguous blocks (an extent, or the neighbouring entries of one indirect block); a smaller read (a program that reads a page at a time, the loader mapping a library:
  `do_mmap` reads a page per call) is served from a 64 KiB window per open file that is filled in one request. Holes and unwritten extents cost a `memset`.
* **Lookups.** The hash tree that ext4 keeps in a big directory is ignored: a directory is a sequence of blocks of entries. A lookup scans from the block where the previous one found its name (so `ls -l` after a listing walks the blocks once), and a directory of 128 KiB
  or more gets an **in-memory name index** at its first lookup: two passes over the directory (count, then fill an open-addressing table of at least twice as many 4-byte slots, FNV-1a of the name), freed with the vnode. Without it a lookup of every name of a 20 000-file
  directory in random order took 1315 ms natively (65 microseconds per name: the blocks are read from the cache, but 20 000 times); with it 3 ms. A directory the index cannot be built for (damaged, or more than 1 048 576 entries) keeps being scanned.
* Numbers from QEMU's emulated CPU and virtio disk are in the section "Measured" below; they say how the code behaves, not how a real disk does.

### Measured

QEMU with TCG (an emulated CPU, shared with other guests, so read the numbers as orders of magnitude), a virtio-blk disk, `ext4_a_directory_of_twenty_thousand_files_and_a_big_file`; the host numbers are an optimised build of the parser without sanitizers, in WSL.
The guest numbers below are from a quiet PC; the same test run while other guests and a host test were running took about twice as long (`ls` 4.0 s, `ls -l` 3.0 s, `dd` 89 MB/s, `md5sum` of the big file 3.4 s).

| What | Result |
|------|--------|
| one directory of 20 000 files in the guest: `ls \| wc -l` | 2.0 s |
| the same: `ls -l \| wc -l` (a lookup and a stat per name, in sorted order) | 1.5 s |
| the same: `ls \| sort \| md5sum`, compared with the names of the source tree | 1.3 s |
| the same: `md5sum` of 40 files of that directory that have content | 4.0 s |
| a file of 100 MB: `dd bs=1M` to `/dev/null` | 0.8 s wall clock, 185 MB/s by dd's own clock |
| the same file: `md5sum` | 2.4 s |
| the same file: `cat \| wc -c` (64 KiB per read, through a pipe) | 8.0 s |
| the host: every one of 20 000 names looked up in random order, without the name index / with it | 1315 ms / 3 ms (20 000 lookups) |
| the host: walk, lookup and crc of all 20 002 entries of that directory under the sanitizers | 0.3 s |
| `linux /bin/ls /` from the ext4 Linux root (shell round trip included) | 0.4 s |
| the Linux root volume: device reads and metadata cache hits for the whole `ext4_linux_root_on_a_disk` run (the loader programs, three scripts under bash and one under dash) | 16 187 reads, 23 210 hits (from the unmount line of the log) |

## A Linux root on a disk

`mount -t ext4 -o ro /dev/sdb1 /linux` makes the Linux root of [COMPAT.md](COMPAT.md) ("The Linux root"), exactly as an unpacked archive does, but the tree stays on the disk: the kernel takes the top of whatever filesystem is mounted on `/linux` (`linux_root_get()`), the
loader maps the programs' pages from it, and nothing is copied to RAM. Python's library and gcc with its headers (a few hundred MB) fit; the tmpfs root, which is capped at half of the machine's memory, cannot hold them.

How to make the volume from the archive that `python tools/make-linux-root.py` builds (it needs a Linux; in WSL, as root, so that the owners stay root as they are in the archive):

```
wsl.exe -u root -e sh -c 'D=$(mktemp -d); tar -xpf build/linux-inputs/linux-root.tar -C $D; mke2fs -t ext4 -b 4096 -O ^has_journal -L linuxroot -d $D root.img 96M; rm -rf $D'
```

`tests/case_ext4.py::root_image()` does exactly this. Copy `root.img` to a partition of a disk or stick (`dd`), or attach it as a disk in QEMU, and on the machine: `mkdir -p /linux; mount -t ext4 -o ro /dev/sdb1 /linux; linux /bin/ls /`. What `ext4_linux_root_on_a_disk` checks:
`linuxroot` says a root is mounted (the test for that is a device number of its own); `linux /bin/ls /`, `cat /etc/os-release`, `sh -c 'echo $((1+2))'`; a link, an owner and a mode of the root come from the inodes; the dynamic programs of `linux_dyn_` (a PIE, a non-PIE with an interpreter,
`dlopen` through `LD_LIBRARY_PATH` and from the system directories, `fork` + `exec`, `posix_spawn`) print what a real Linux prints; bash runs the three scripts of `linux_dyn_shell_and_tools` and dash one of them, with the same output as on a real Linux;
`linuxroot -d` **refuses** (the root is a volume, not an unpacked archive: nothing to empty, and it is read only; a change in `user/bin/kappabox/linux.c`, which used to empty a root by removing its files and then unmount it, and would have unmounted the disk) and `linuxroot ARCHIVE` refuses to
unpack over it; `umount /linux` says busy while a Linux program lives in the root and works when none does; the image is unchanged afterwards. A program in the root cannot write the root (its files are read only); `/tmp` is the machine's own, as for the tmpfs root. Since `main` at `f4629f6` `execve` picks the personality from the file's own markers (docs/COMPAT.md), so a Linux program also starts by its path from the native shell;
the tests here keep `linux PROGRAM`, the explicit override, and starting by path from a disk root was not tried.

## Tests

Host (`python tools/hosttest.py discover -s tests/host -p "test_ext4.py"`, needs gcc, mke2fs, debugfs, e2fsck, all in WSL's Ubuntu; 5 to 8 minutes depending on the load of the PC; `-k Fuzz` runs one group, `TOUHOU_EXT4_FUZZ=N` sets the fuzz cases per image):

| Group | What it shows |
|-------|---------------|
| `Generator` | `e2fsck -fn` calls every image the generator makes clean (a check of the generator); the crc of a run of zeros against zlib |
| `Walk` | **the walk equals the source tree**: type, mode, uid, gid, size, link count, content crc and link target of every file of ext2 (1 KiB blocks, block maps), ext3, ext4 (1 KiB, 4 KiB, 64 KiB blocks), no journal, inline data (256- and 128-byte inodes), 255-byte names, `meta_bg` (32- and 64-byte descriptors), `bigalloc`, no `flex_bg` and the other feature mixes of `FUNCTIONAL`, with whole-file reads and with 4 KiB reads (the window); hard links share an inode; a directory of 20 000 files (hashed) listed, looked up by name and read; a name index test (every name found in random order, absent names not found); a file of 5 GiB with holes on ext4 and on ext2 (the triple indirect range of 4 KiB blocks); modes with set-id and sticky bits, a user id and group id above 16 bits, a time before 1970 and times with epoch and nanosecond bits; `debugfs stat` agrees on mode, owner, size, links, blocks, flags and the time stamps of every inode |
| `Geometry` | block size, counts, features, label and uuid against `dumpe2fs -h`; **the inode table, free blocks and free inodes of every block group against `dumpe2fs`** (also for `meta_bg` with 32- and 64-byte descriptors, `flex_bg`, `bigalloc` and a 5 GiB volume); `statfs` against the superblock |
| `Mapping` | the extents of a file (one extent; 1200 islands that need index blocks, two levels on 1 KiB blocks; holes) against `debugfs ex`; every mapped block of ext2 files (direct, single, double and triple indirect) against `debugfs stat` |
| `Features` | inline data (the inode's bytes, small directories, long links) and that debugfs agrees the inodes are inline; device nodes in the old and the new encoding, a fifo, a socket; symbolic links of 1 to 4000 bytes; names of 255 bytes; **unwritten extents read as zeros even where the volume holds other bytes**; **a hand-made extent tree of five levels** (found), of six, with a wrong depth in a node, and with a loop (errors) |
| `Hostile` | 26 damaged superblock fields, refusals with a reason, a volume cut short, group descriptors, 14 inode fields of a file and of the root (28 cases), directory blocks, a zeroed directory block, a directory that names its parent as a child, symbolic links with wrong sizes: an error or a partial listing, never a crash |
| `Fuzz` | thousands of random mutations of whole volumes (bit flips, bytes and fields set to extremes, copies, rare truncation), three quarters of them in the used part of the metadata blocks that a walk of the intact volume read, on ten volumes (ext2, ext3, ext4 with 1 KiB and 4 KiB blocks, inline data, `meta_bg`, no `filetype`, and directories of 6000 entries on ext4 with 1 KiB and 64 KiB blocks and on ext2 with block maps), each opened and walked with every file read and every name looked up, under AddressSanitizer and UBSan, 20 s limit per case |
| `Source` | the sources mention no way to write |

QEMU (`python tools/qemu-slot.py -- python tests/run_all.py ext4_`): `tests/case_ext4.py`.

| Test | Covers |
|------|--------|
| `ext4_mount_read_walk_and_refuse_every_write` | the mount policy leaves the disk alone; `lsblk` and `/proc/blkfs` name the filesystems; `mount -o rw` and unknown options are refused with a reason in the log; a walk with `find`, `md5sum` and `readlink` equals the source tree (type, mode, owner, size, time, md5, hard links, links); users' permissions are the inodes' (`su`); `df`; thirteen kinds of write fail with `Read-only file system`; busy while inside; `umount`; ext2 (block maps), ext3, inline data and `meta_bg` volumes the same way; after five mounts, walks and unmounts `/proc/kstats` shows no vnode or open file left, the heap no larger than before (the caches are freed with the volume) and no new warning; every image unchanged |
| `ext4_on_ahci_nvme_and_virtio_with_partition_tables` | SATA, NVMe and virtio; whole disks, an MBR partition of type 83 and a GPT one; an EFI-typed partition and a Linux-typed one full of garbage are not named; a partition cannot be mounted with its disk |
| `ext4_on_disks_with_4096_byte_sectors` | a virtio-blk disk and an NVMe namespace with 4096-byte sectors (`logical_block_size=4096`; the partition tables are built with 4096-byte sectors too): a whole disk with a volume of 4 KiB blocks, a GPT partition with a volume of 1 KiB blocks (a Linux kernel would not mount that, but every metadata read of this reader is then ragged against the sectors), an ext3 volume in an NVMe partition; `/proc/partitions` says 4096, `lsblk` names the volumes, each is walked and compared with its source tree, no warning, nothing left behind, images unchanged |
| `ext4_a_directory_of_twenty_thousand_files_and_a_big_file` | `ls` and `ls -l` of 20 000 entries, every name compared with the source tree (sorted, md5), the files with content read back, a name that is not there is not found, `stat`; a file of 100 MB read with `dd`, `cat` and `md5sum`; the numbers are printed (the volume is unchanged afterwards) |
| `ext4_damaged_volumes_do_not_hurt_the_kernel` | a broken superblock, root inode, inode table pointer, directory block and extent tree, a volume cut in half, six random ones: refused with a reason or listed with `EIO`; no warning in the log, the shell answers, a good volume mounts afterwards |
| `ext4_mount_options_encrypted_files_and_a_volume_that_needs_recovery` | the words of a Linux `fstab`; `nosuid`; the log lines for `needs_recovery` and an error flag; an encrypted file answers `ENOKEY`, its hard link too, the rest reads; the three names of the driver |
| `ext4_linux_root_on_a_disk` | the Linux root on an ext4 volume: see "A Linux root on a disk" |

### What was verified only against my own reading and `mke2fs`

The generator (`mke2fs`, `debugfs`), the manifest and the driver are three different programs, but the driver and the host test were written by one hand from one document, and `mke2fs` is the only writer that was used. What could hide a shared misreading, or something no test volume has:

* **A volume written by a real Linux kernel**: one was read, see "Checked against a volume that a real Linux wrote" below (the root file system of this PC's WSL: 69 231 entries of `/usr` and more, no difference). Not read: a stick that a Linux machine unplugged dirty (the journal holding transactions that were never written to their place), `bigalloc`
  clusters shared by several files, `inline_data` in transition, an `orphan_file` with entries, `casefold` directories, real encrypted files (only the flag was set by hand).
* **`statfs`.** The free counts are the sums of the group descriptors (what ext4 itself does when it mounts), the total is the block count minus `s_overhead_clusters`: the test compares them with `dumpe2fs`, not with what a Linux `df` prints for the same volume.
* **The record length of 64 KiB blocks** (0 or 65535 for a whole block, the top bits in the low two bits otherwise) and **the device number encodings** were checked on what `mke2fs` and `debugfs` write; the 64 KiB encoding of a record between 65 533 and 65 535 bytes cannot occur in a block with a name in it.
* **`INODE_UNINIT` and `BLOCK_UNINIT`** are never looked at, so an inode that a directory entry names in a group whose flag says "unused" is read from the table as it is (a damaged volume case; `e2fsck` would object).
* **Sector sizes.** The byte-granular read path (a 1 KiB superblock inside one 4096-byte sector; a read that starts or ends inside a sector goes through a bounce buffer) runs in QEMU on emulated disks with 4096-byte sectors (`ext4_on_disks_with_4096_byte_sectors`: virtio-blk and NVMe, whole disk and GPT partition), not on a real 4Kn disk or stick.
* **Names.** A name is bytes; invalid UTF-8 is passed on as it is, and a name with a slash or a NUL is left out of listings (counted, logged once).
* **Hardware.** Nothing of this ran on the Dell or on a real stick.

### Checked against a volume that a real Linux wrote

`tests/host/ext4real.py` reads a volume that an operating system has been using, with the parser (the host build, under the sanitizers), and compares every entry with what that operating system says about the mounted tree. On 2026-11-17 it was run, as root and read only, on the **root
file system of this PC's WSL Ubuntu** (`/dev/sdd`: 1 TiB, 8192 block groups, 67 million inodes of 256 bytes, 4 KiB blocks, written for months by a Linux 6.18 kernel; features `has_journal ext_attr resize_inode dir_index orphan_file filetype needs_recovery extent 64bit flex_bg metadata_csum_seed sparse_super
large_file huge_file dir_nlink extra_isize metadata_csum orphan_present`; it is mounted, so the journal holds transactions): `wsl.exe -u root -e python3 tests/host/ext4real.py /dev/sdd /usr`.

* `/usr`: 69 231 entries (5 386 directories, 53 513 files, 10 332 symbolic links), `/home`: 114 343 entries (3 904 directories, 110 407 files, 45 links, a socket), `/var` (3 655), `/etc` (1 368), `/root`: some 189 000 entries in all. Kind, mode, owner, inode number, link count, size, time of modification,
  **the crc32 of every file's content** (files up to 64 MiB; 17 bigger ones by size and time) and the target of every link agree with the mounted tree, and the parser lists nothing the tree does not have. `/usr` took 11 s and `/home` 92 s under AddressSanitizer. Several of the directories are hashed (`/usr/include`,
  `/usr/lib/x86_64-linux-gnu` and `/usr/share/doc` have the index flag, which the parser ignores).
* The only differences are honest ones: four files that were being written at that moment (`/var/log/syslog`, `kern.log`, the journal, the clock file: the disk lags the cache), and the 6 158 entries below `/usr/lib/modules`, which is an overlay of another volume mounted there.

This is the only volume that `mke2fs` did not make that was read. It covers what a long-lived 4 KiB-block ext4 holds (a kernel's allocation of extents, hashed directories, `metadata_csum`, an orphan file, security attributes in the inodes, a mounted volume whose journal holds transactions). It does not cover the cases listed above.

### Trying it on a real Linux stick (the lead's, read only)

Nothing below writes to the stick. On a Linux machine: `mkfs.ext4 -L LINUXSTICK /dev/sdX1`, mount it, copy a directory tree with links, a file of a few hundred MB, a directory with thousands of files, `sync`, `umount`, and `sha256sum` a few files. On the Dell:

```
lsblk                                                    # FSTYPE ext4, LABEL LINUXSTICK for the stick's partition
cat /proc/blkfs ; dmesg | grep -E "blk: sd.* holds|ext4"
sudo mkdir -p /mnt/stick
sudo mount -t ext4 -o ro,nosuid /dev/sdb1 /mnt/stick   # dmesg: "ext4: sdb1 mounted on /mnt/stick, N MiB, 4096-byte blocks, label 'LINUXSTICK', ext4 (...), read-only"
ls -l /mnt/stick ; find /mnt/stick | wc -l ; sha256sum /mnt/stick/<file> /tmp/<same file copied from the stick>
dd if=/mnt/stick/<big file> of=/dev/null bs=1M           # the throughput of the stick
sudo umount /mnt/stick
```

A stick that was pulled from the Linux machine without `umount` shows the `needs journal recovery` line; that is the expected message, and what is shown is the disk as it is. If `mount` fails, the reason is in `dmesg` (`ext4: sdb1 is not a mountable ext2/3/4 volume: ...`): send it with `sudo dd if=/dev/sdb1 bs=1024 skip=1 count=1 | xxd | head -20`. Reading a stick through the Dell's USB controller is the USB driver's business ([USB.md](USB.md)), not this one's.

## State (2026-11-17, branch `agent/ext4-ro`)

**Done and tested:** the parser (superblock revisions 0 and 1, 1 KiB to 64 KiB blocks, 32- and 64-byte descriptors, `flex_bg`, `meta_bg`, `sparse_super2`, `bigalloc`, block maps, extents, inline data, directories, links, special files, times, `statfs`), the three filesystem types, `mount`
(read only; `rw` refused; `nosuid`), the block layer's filesystem name and `/proc/blkfs`, the `FSTYPE` column of `lsblk`, the Linux root on a disk, the name index of big directories, the host tool, the image generator, the host suite (26 tests, 5 to 8 minutes, a fuzz of 15 500 cases in it: 13 894 volumes mounted, 1 606 refused, 24.6 million entries walked, none of the cases crashes or
hangs; and a one-off of 132 500 more: 53 runs of 2 500 cases on nine of the volumes, seeds 21 to 23, 3 or 12 mutations per case, 111 967 mounted and 20 533 refused, the slowest case 472 ms, no crash), the seven QEMU tests (all pass; on 2026-11-17 after
the merge of `main` at `f4629f6`, which changed how a Linux program gets its personality, `ext4_linux_root_on_a_disk` still passes).

**Files** (all new unless it says changed): `kernel/fs/ext4.c`, `ext4_core.c`, `ext4_priv.h`, `kernel/include/ext4.h`, `tests/host/ext4tool.c`, `ext4img.py`, `test_ext4.py`, `tests/case_ext4.py`, this page. **Changed, outside the files this driver owns, each by a few lines:** `kernel/fs/vfs.c` (the table of
filesystem types grew from 8 to 16 entries: ext2, ext3 and ext4 would have been the 8th, 9th and 10th), `kernel/include/blk.h` and `kernel/drivers/blk.c` (`BlockDev.fstype` and `fslabel`, the probe of two sectors, `/proc/blkfs`), `user/bin/kappabox/hw.c` (`lsblk`: the FSTYPE column),
`user/bin/kappabox/linux.c` (`linuxroot -d` refuses on a volume), the manual pages `mount.pw`, `lsblk.pw`, `linuxroot.pw`, and the documents `COMPAT.md`, `ROADMAP.md` and `STATUS.md`.

**Not done:** anything on real hardware; a stick or disk written by a real Linux (see "What was verified only against my own reading and `mke2fs`"); the C library has no text for `ENXIO` and `ENOKEY` (`cat` of a device node on a disk prints "Unknown error"); a symbolic link of 1024 bytes or more is cut at 1023 by the
VFS's resolution buffer (`PATH_MAX`), so that it cannot be followed, though `readlink` with a larger buffer and the parser read it whole; `st_blocks` is the size rounded up to 512 bytes, not the blocks the inode holds (the VFS computes it from the size).

**Next, in order of value:** (1) try a real Linux stick on the Dell with the plan above; (2) make a volume with Python's library and gcc for the Linux root (L3) and see how it behaves with a few hundred MB: the cache sizes and the read-ahead were chosen for the 15 MB root; (3) read-only replay of the journal in memory (a volume that needs recovery would show
what Linux would show), then extended attributes and ACLs for `ls -l`'s `+` and `getfacl`.
