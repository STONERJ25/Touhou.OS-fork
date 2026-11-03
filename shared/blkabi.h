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
 *
 * The ESP write grant (docs/INSTALL.md, "The ESP write grant"). Under blk.ro the installed ESP (loader, kernel, initrd, cmdline.txt) is
 * read-only; an update has to write it for a moment. Root asks through an open file of the ESP's partition node, then mounts it:
 *
 *   int fd = open("/dev/nvme0n1p1", O_RDWR | O_CLOEXEC);
 *   struct gblk_unlock u = { .name = "nvme0n1p1" };
 *   ioctl(fd, BLKESPGRANT, &u);                         root only; only the ESP the loader came from (/proc/bootdisk, signature checked)
 *   mount("/dev/nvme0n1p1", dir, "fat32", 0, "rw,espgrant") through gap_mount_opts(): this mount, and nothing else, may write it
 *   ... write files below dir ...
 *   umount(dir); close(fd);                             the grant ends at the unmount; closing the file (or exit) ends it as well, and a
 *                                                       volume still mounted then is written back and turns read-only
 *
 * Writes through the partition node itself, the whole disk, its other partitions and every other disk keep getting EROFS. Kernel memory
 * only, as the unlock above. BLKWRLOCK on that file gives the grant back early.
 */
#ifndef SHARED_BLKABI_H
#define SHARED_BLKABI_H

#define BLKWRUNLOCK  0x4B570001UL       /* 'K' 'W': unlock writes through this open file (root, whole disk, name must match) */
#define BLKWRLOCK    0x4B570002UL       /* take the unlock back (also happens on the last close of the file) */
#define BLKWRSTATE   0x4B570003UL       /* -> BLKWR_* bits */
#define BLKESPGRANT  0x4B570004UL       /* on the boot ESP's partition node: one read-write mount with -o espgrant (root, name must match) */

#define BLKWR_READONLY   1              /* the disk is registered read-only */
#define BLKWR_POLICY     2              /* ... because of blk.ro (an unlock can lift it); without this bit the medium itself refuses writes */
#define BLKWR_UNLOCKED   4              /* this open file holds the unlock */
#define BLKWR_ESPGRANT   8              /* this open file holds the ESP write grant */
#define BLKWR_ESPMOUNTED 16             /* ... and a mount has taken it (the volume is writable until it is unmounted or the file closed) */

struct gblk_unlock {                    /* 256 bytes: ioctl(2) copies that much in and back out */
    char name[32];                      /* "sdb", "nvme0n1", "vda" (or "nvme0n1p1" for BLKESPGRANT): the name of the node the file is on */
    char reserved[224];
};

#endif
