#ifndef _SYS_STATFS_H
#define _SYS_STATFS_H
#include <sys/types.h>
#include <abi.h>

/* Size and free space of the filesystem that holds a path (SYS_statfs 67). Block counts are in units of f_bsize.
 * The layout is struct gstatfs from abi.h. */
struct statfs {
    unsigned long f_type;               /* magic: tmpfs 0x01021994, FAT 0x4d44, proc 0x9fa0 */
    unsigned long f_bsize;
    unsigned long f_blocks;
    unsigned long f_bfree;
    unsigned long f_bavail;
    unsigned long f_files;              /* 0 = not tracked (FAT, tmpfs) */
    unsigned long f_ffree;
    unsigned long f_namelen;
    unsigned long f_flags;              /* MNT_RDONLY */
    unsigned long f_fsid;
    char f_fstypename[16];              /* "tmpfs", "fat32", "fat16", ... */
    char f_mntfromname[64];             /* "/dev/vda2" */
    char f_mntonname[64];               /* "/HumanVillage" */
};

int statfs(const char *path, struct statfs *buf);
#endif
