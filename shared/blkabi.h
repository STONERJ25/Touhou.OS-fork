/*
 * Block device controls shared by the kernel (kernel/drivers/blk_unlock.c) and user space (the installer, `install`).
 *
 * The per-disk write unlock. `blk.ro` on the kernel command line registers every SATA disk and NVMe namespace read-only. Instead of dropping
 * that switch for the whole machine, root can lift it for one disk, through one open file:
 *
 *   int fd = open("/dev/sdb", O_RDWR);
 *   struct gblk_unlock u = { .name = "sdb" };          the caller repeats the device's name: a stray fd number unlocks nothing
 *   ioctl(fd, BLKWRUNLOCK, &u);                         root only; the whole disk only, never a partition node
 *   ... write(fd, ...) ...                              only writes through this very open file get past blk.ro
 *   ioctl(fd, BLKWRLOCK, &u);                           or close(fd), or exit: the unlock dies with the open file
 *
 * Nothing else becomes writable: the disk's partition nodes, other opens of the same node, mounted filesystems and every other disk
 * still get EROFS. The unlock lives in kernel memory only (never on disk, never on the command line), so a reboot forgets it.
 * BLKWRSTATE returns BLKWR_* bits for the disk as seen through this open file.
 */
#ifndef SHARED_BLKABI_H
#define SHARED_BLKABI_H

#define BLKWRUNLOCK  0x4B570001UL       /* 'K' 'W': unlock writes through this open file (root, whole disk, name must match) */
#define BLKWRLOCK    0x4B570002UL       /* take the unlock back (also happens on the last close of the file) */
#define BLKWRSTATE   0x4B570003UL       /* -> BLKWR_* bits */

#define BLKWR_READONLY   1              /* the disk is registered read-only */
#define BLKWR_POLICY     2              /* ... because of blk.ro (an unlock can lift it); without this bit the medium itself refuses writes */
#define BLKWR_UNLOCKED   4              /* this open file holds the unlock */

struct gblk_unlock {                    /* 256 bytes: ioctl(2) copies that much in and back out */
    char name[32];                      /* "sdb", "nvme0n1", "vda": must be the name of the node the file was opened on */
    char reserved[224];
};

#endif
