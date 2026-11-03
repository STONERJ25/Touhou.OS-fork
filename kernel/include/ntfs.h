/*
 * NTFS, read only (docs/NTFS.md). The filesystem type "ntfs" registers itself at boot (INITCALL in kernel/fs/ntfs.c); nothing else needs to know about it.
 *
 *   mount -t ntfs -o ro /dev/nvme0n1p3 /mnt/windows
 *
 * A BitLocker volume whose key is a clear key (BitLocker "suspended") is recognised by its first sector and unlocked through kernel/fs/bitlocker.c: the
 * decrypted view shows up as /dev/bdeN while the volume is mounted. There is no write path of any kind, in this driver or behind it.
 */
#ifndef K_NTFS_H
#define K_NTFS_H

#include "vfs.h"

extern const FsType g_ntfs_type;

#define NTFS_SUPER_MAGIC 0x5346544e         /* "NTFS", as Linux's statfs reports it */

/* Reads any byte range of a block device (a bounce buffer takes care of ragged ends); the parser's and the BitLocker layer's way to the disk. Read only. */
int ntfs_blk_io(void *ctx, u64 off, void *buf, size_t len);

#endif
