/*
 * The read-only consistency check for FAT volumes that were not shut down cleanly (kernel/fs/fat_check.c, built on shared/fatck.c), and the
 * opt-in repair that can follow it (hvfsck=safe, built on shared/fatfix.c).
 *
 * The mount policy runs the check on the HUMANVILLAGE volume before mounting, without any VFS lock held (a big stick can take seconds):
 *   0   the flags say the volume is clean: nothing was read beyond the boot sector and FAT[0..1]
 *   1   dirty flag set, but every directory, chain and size is in order and nothing is left over: mount read-write; the policy
 *       takes the flag over (it is cleared again at the first idle flush)
 *   2   in order, but a power cut left harmless leftovers (lost clusters, a chain longer than its file, bytes after a directory's end
 *       marker, FAT copies or FSInfo that lag behind) or the volume carries a disk-error flag: mount read-write, keep the flag set
 *   -1  damage (or the check could not finish): mount read-only and say precisely what is wrong; `fsck` is the way out
 *   -2  the boot sector is unusable: nothing to say here, the FAT driver refuses the volume with its own reason
 * `hvforce` on the kernel command line mounts read-write whatever the check says; `hvcheck=off` skips the check (a dirty volume is then
 * read-only, as before). The reports are kept for /proc/fatcheck.
 *
 * With `hvfsck=safe` the policy follows a check that found damage (-1, scan complete) with fat_repair_device(): the engine of /bin/fsck
 * runs with FIX_SAFE (what `fsck -a` does, never a repair that costs a name or part of a file) on the unmounted volume, a second check
 * reads the volume again from the device, and the answer of that second check (0, 1, 2 or -1, with the same meaning as above) decides
 * the mount. Only a volume that came out of the first check as damaged is touched; harmless leftovers keep the path of verdict 2.
 */
#ifndef K_FATCHECK_H
#define K_FATCHECK_H

#include "types.h"
#include "blk.h"

#define FATCHECK_ERR_LINES  8
#define FATCHECK_LEAK_LINES 4
#define FATCHECK_REPAIR_LINES 6         /* the first things the repair said, for /proc/fatcheck */
#define FATCHECK_AFTER_LINES  4         /* what is still wrong after the repair */
#define FATCHECK_LINE       176

/* FatCheckReport.rp_state */
enum {
    FATREPAIR_NONE = 0,         /* hvfsck=safe was not asked for, or there was nothing for it to do */
    FATREPAIR_DONE,             /* the repair ran to the end and the volume was gone through again (after_*) */
    FATREPAIR_STOPPED,          /* the repair began and could not be finished (the device failed, memory, time): rp_rc says why */
    FATREPAIR_READONLY,         /* asked for, but the device cannot be written (blk.ro without blk.rw=, or a write-protected medium) */
    FATREPAIR_UNVERIFIED,       /* asked for, but the check could not finish: with no verdict there is nothing to mend */
};

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

    /* hvfsck=safe (fat_repair_device): what the repair did, and what a second check found afterwards */
    int  rp_state;              /* FATREPAIR_* */
    int  rp_rc;                 /* the engine's return value: 0, or -errno (-5 the device failed, -12 memory, -62 out of time) */
    u32  rp_ms, rp_passes, rp_repaired, rp_declined, rp_failed;
    u32  rp_lost_chains, rp_lost_clusters, rp_copied;
    bool rp_cleared;            /* the engine marked the volume clean */
    u32  rp_nlines, rp_said;    /* lines kept / lines the engine said in all */
    char rp_line[FATCHECK_REPAIR_LINES][FATCHECK_LINE];
    int  after_verdict;         /* the second check: 0 clean, 1 flag only, 2 leftovers only, -1 damage or no verdict */
    bool after_incomplete;
    u32  after_errors, after_leaks, after_lost, after_ms, after_nlines;
    char after_line[FATCHECK_AFTER_LINES][FATCHECK_LINE];
    char after_leak[FATCHECK_LINE];     /* the first leftover that remains */
} FatCheckReport;

int  fat_check_device(BlockDev *dev, FatCheckReport *rep);
/* hvfsck=safe: repairs what `fsck -a` repairs on the unmounted volume `dev` (writes through the block layer), then checks it again from the
 * device. `rep` is the report of the check that found the damage (it keeps it and adds rp_* and after_*); `stamp` is the time stamp, in the
 * volume's own (local) time, for the files made from lost chains. Returns the verdict of the second check (0, 1, 2, -1: see above); -1 also
 * when the repair could not be finished. The caller has checked that the device is writable. */
int  fat_repair_device(BlockDev *dev, FatCheckReport *rep, u64 stamp);
void fat_check_publish(const FatCheckReport *rep);      /* keeps it for /proc/fatcheck (one slot per device name) */
void fat_check_init(void);                              /* registers /proc/fatcheck */

#endif
