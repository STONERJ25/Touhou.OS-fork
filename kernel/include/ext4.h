/*
 * ext2, ext3 and ext4, read only (docs/EXT4.md). The filesystem types "ext2", "ext3" and "ext4" (one driver, three names) register themselves at boot (INITCALL in
 * kernel/fs/ext4.c); nothing else needs to know about them:
 *
 *   mount -t ext4 -o ro /dev/sdb1 /mnt/linux
 *
 * There is no write path of any kind, in this driver or behind it.
 */
#ifndef K_EXT4_H
#define K_EXT4_H

#include "vfs.h"

extern const FsType g_ext2_type, g_ext3_type, g_ext4_type;

#define EXT4_SUPER_MAGIC 0xEF53

/* Is this an ext2/3/4 superblock? `sb` is the 1024 bytes that start at byte offset 1024 of a volume. Returns 1 with the filesystem's name ("ext2", "ext3" or "ext4", by the
 * features it uses) and its volume label, or 0. The block layer (kernel/drivers/blk.c) uses it to name what a Linux partition holds in lsblk. */
int ext4_probe_super(const void *sb, size_t len, char *fs, size_t fsn, char *label, size_t labeln);

#endif
