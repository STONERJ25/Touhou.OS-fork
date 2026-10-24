/*
 * Block devices. Hardware drivers (virtio-blk, AHCI, NVMe) register a BlockDev; the layer adds /dev nodes, GPT/MBR
 * partition children, byte-granular access for dd, and /proc/partitions. Filesystems mount a BlockDev by name.
 *
 * Buffer rule: buffers passed to read/write must live in the kernel heap or in dma_alloc memory (the direct map), never
 * on a kernel stack. Drivers must accept any count (split big requests themselves) and may be called from any thread
 * but never from interrupt context.
 */
#ifndef K_BLK_H
#define K_BLK_H

#include "types.h"
#include "vfs.h"

typedef struct BlockDev BlockDev;

struct BlockDev {
    char name[16];                  /* "vda", "sda", "nvme0n1", and for partitions "vda1", "nvme0n1p2" */
    char model[48];                 /* human readable, may be empty */
    const char *driver;             /* "virtio-blk", "ahci", "nvme" */
    u32  sector_size;               /* bytes per logical sector: 512 or 4096 */
    u64  sectors;                   /* capacity in logical sectors */
    bool readonly;
    bool ro_policy;                 /* read-only because of blk.ro (not because the medium is write-protected): BLKWRUNLOCK can lift it */
    volatile bool removed;          /* hot-unplugged (blk_unregister): every request fails with -ENODEV; the struct itself is never freed */
    int  (*read)(BlockDev *d, u64 lba, u32 count, void *buf);            /* 0 or -errno */
    int  (*write)(BlockDev *d, u64 lba, u32 count, const void *buf);
    int  (*flush)(BlockDev *d);                                          /* may be NULL */
    void *priv;                     /* driver state */

    /* filled in by the block layer */
    BlockDev *parent;               /* partitions only */
    u64  part_start;                /* first sector of a partition inside its parent */
    int  part_index;                /* 1-based */
    u8   part_type_mbr;             /* MBR partition type, or 0xEE-less GPT marker 0 */
    u8   part_type_guid[16];        /* GPT partition type GUID (mixed-endian, as stored on disk) */
    char label[40];                 /* GPT partition name; filesystems may set the volume label here too */
    int  dev_index;
    BlockDev *next;
};

/* Registers a driver-created device (heap-allocated, zero-filled except the fields above) and scans its partitions. */
int        blk_register(BlockDev *d);
/* Same, for a SCSI-style disk: gives it the first free name of the shared sda, sdb, ... scheme (SATA and USB sticks draw from it). */
int        blk_register_sd(BlockDev *d);
/* Hot-unplug: marks the disk and its partitions removed, takes them out of the list, /dev and /proc/partitions. Requests that are still
 * running or arrive later (an open /dev node, a mounted filesystem) fail with -ENODEV. The BlockDev structs are kept (a few hundred bytes
 * per unplug) because nothing counts who still holds a pointer; the driver keeps `priv` valid for as long as it keeps the struct. */
void       blk_unregister(BlockDev *d);
BlockDev  *blk_find(const char *name);                  /* "vda1" or "/dev/vda1" */
BlockDev  *blk_find_label(const char *label);           /* partition label or filesystem label, case-insensitive */
BlockDev  *blk_first(void);
BlockDev  *blk_next(BlockDev *d);

int  blk_read(BlockDev *d, u64 lba, u32 count, void *buf);       /* bounds checked */
int  blk_write(BlockDev *d, u64 lba, u32 count, const void *buf);
int  blk_flush(BlockDev *d);
size_t blk_describe(char *buf, size_t cap);                      /* text for /proc/partitions */

/* Mount policy (kernel/fs/mountpolicy.c). storage_mount_boot_volumes() runs once at boot for the disks that exist by then. A driver whose
 * disks appear later (USB sticks) calls storage_disk_added() for each one: the policy then looks at it on a worker thread of its own
 * (a mount reads the disk through the very thread that announces it, so it must not run there) and mounts its HUMANVILLAGE volume if
 * nothing is mounted on /HumanVillage yet. storage_automount_busy() is true while a worker is still at work (usb_wait_settled() waits
 * for it). A volume that is mounted when its disk disappears keeps failing with -ENODEV (the FAT driver says so once in the log). */
void storage_mount_boot_volumes(void);
void storage_disk_added(BlockDev *disk);
bool storage_automount_busy(void);
int  storage_tz_offset_minutes(const char *tz);         /* POSIX TZ string -> minutes east of UTC (standard time only); 0 if unusable */

/* The installer's half of the block layer (kernel/drivers/blk_unlock.c, shared/blkabi.h): the per-disk write unlock held by one open
 * file, blk.rw=GUID (a disk exempt from blk.ro), /proc/bootdisk. */
int  blk_unlock_ioctl(BlockDev *d, File *f, unsigned long req, void *arg);
bool blk_unlock_allows(BlockDev *d, File *f);
void blk_unlock_closed(BlockDev *d, File *f);
void blk_rw_check(BlockDev *d, const u8 disk_guid[16]);

/* GPT type GUID helpers (on-disk byte order). */
extern const u8 GPT_GUID_EFI_SYSTEM[16], GPT_GUID_MS_BASIC_DATA[16], GPT_GUID_LINUX_FS[16];

#endif
