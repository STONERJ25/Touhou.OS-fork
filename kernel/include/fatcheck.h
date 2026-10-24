/*
 * The read-only consistency check for FAT volumes that were not shut down cleanly (kernel/fs/fat_check.c, built on shared/fatck.c).
 *
 * The mount policy runs it on the HUMANVILLAGE volume before mounting, without any VFS lock held (a big stick can take seconds):
 *   0   the flags say the volume is clean: nothing was read beyond the boot sector and FAT[0..1]
 *   1   dirty flag set, but every directory, chain and size is in order and nothing is left over: mount read-write; the policy
 *       takes the flag over (it is cleared again at the first idle flush)
 *   2   in order, but a power cut left harmless leftovers (lost clusters, a chain longer than its file, bytes after a directory's end
 *       marker, FAT copies or FSInfo that lag behind) or the volume carries a disk-error flag: mount read-write, keep the flag set
 *   -1  damage (or the check could not finish): mount read-only and say precisely what is wrong; `fsck` is the way out
 *   -2  the boot sector is unusable: nothing to say here, the FAT driver refuses the volume with its own reason
 * `hvforce` on the kernel command line mounts read-write whatever the check says; `hvcheck=off` skips the check (a dirty volume is then
 * read-only, as before). The reports are kept for /proc/fatcheck.
 */
#ifndef K_FATCHECK_H
#define K_FATCHECK_H

#include "types.h"
#include "blk.h"

#define FATCHECK_ERR_LINES  8
#define FATCHECK_LEAK_LINES 4
#define FATCHECK_LINE       176

typedef struct FatCheckReport {
    char dev[16];
    bool ran;                   /* the full scan was started */
    bool dirty, hard_error;
    bool incomplete;            /* no verdict: out of time, memory, or the device failed */
    int  bits;
    int  verdict;               /* the return value of fat_check_device() */
    u32  errors, leaks, warnings;
    u32  files, dirs, free_clusters, lost_clusters;
    u32  ms;
    u32  nerr_lines, nleak_lines;
    char err_line[FATCHECK_ERR_LINES][FATCHECK_LINE];
    char leak_line[FATCHECK_LEAK_LINES][FATCHECK_LINE];
} FatCheckReport;

int  fat_check_device(BlockDev *dev, FatCheckReport *rep);
void fat_check_publish(const FatCheckReport *rep);      /* keeps it for /proc/fatcheck (one slot per device name) */
void fat_check_init(void);                              /* registers /proc/fatcheck */

#endif
