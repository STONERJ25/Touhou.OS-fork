/*
 * fatfix -- the repair engine behind /bin/fsck, on top of the scanner in fatck.h. User space only (the kernel never repairs anything:
 * it checks, and when it is not satisfied it mounts read-only and says what to run). tools/fatck_host.py builds it into the test DLL.
 *
 * It works in passes: scan (findings are retained), repair what the findings allow, flush, scan again, until a pass repairs nothing.
 * Within a pass the findings are taken in an order that keeps the later ones valid (fatfix.c, stage_of): the FAT copies, then cross-links
 * (the second claimant gets a private copy before anything is written into or freed from either chain), then "." and "..", then repeated
 * names, then the rest; a stage that changed something ends the pass, so that the next one starts from a fresh scan. Lost clusters are dealt
 * with last, once nothing else is left to change what counts as lost. Every repair re-reads the directory slot it is about to change and
 * skips it when it no longer is what the scan saw (the next pass finds it again, if it still matters).
 *
 *   FIX_NONE  report only (-n): never writes
 *   FIX_SAFE  (-a) repairs that lose no readable data and no file: FAT copies, FSInfo, flags, a size beyond its chain, a chain beyond its
 *             size, a chain that breaks behind everything its file's size needs, bytes after a directory's end, long-name entries that
 *             belong to no file (a file whose long name does not match its short entry stays under the short one), "." and "..", an entry
 *             that is an exact copy of another one, cross-linked files (the shared tail is copied into free clusters), lost chains (saved
 *             as FOUND.000/FILEnnnn.CHK)
 *   FIX_ALL   (-y) and also what costs something: chains cut where they break, entries that point nowhere, entries that repeat a name or
 *             stand where "." and ".." belong, stray labels removed, bad names mended
 * A volume is declared clean (dirty flag cleared, FSInfo rewritten) only when the last scan finds no error and no leftover.
 */
#ifndef SHARED_FATFIX_H
#define SHARED_FATFIX_H

#include "fatck.h"

enum { FIX_NONE = 0, FIX_SAFE = 1, FIX_ALL = 2 };

/* what a line from FatFixOpts.say is about */
enum { FSAY_FIXED = 0, FSAY_LEFT = 1, FSAY_FAILED = 2 };          /* a repair that was made / one left alone (the mode or the answer did not allow it) / one that could not be made */

typedef struct FatFixOpts {
    int      mode;
    bool     free_lost;                 /* give lost chains back to the free pool instead of saving them as files */
    bool   (*ask)(void *ctx, int level, const char *question);   /* interactive: asked before every repair (level = FIX_SAFE / FIX_ALL) */
    void   (*say)(void *ctx, int kind, const char *line);          /* one line per repair, refusal or failure (kind: FSAY_*) */
    void    *ctx;
    uint64_t unix_time;                 /* time stamp of the files made from lost chains (0 = 1980-01-01) */
    uint32_t max_passes;                /* 0 = 8 */
} FatFixOpts;

typedef struct FatFixStats {
    uint32_t passes;
    uint32_t repaired;                  /* repairs applied */
    uint32_t declined;                  /* repairs the mode or the answer did not allow */
    uint32_t failed;                    /* repairs that could not be done (no space, I/O) */
    uint32_t lost_chains, lost_clusters;/* lost chains saved as files (or freed) and the clusters in them */
    uint32_t freed_clusters;
    uint32_t copied_clusters;           /* clusters duplicated to separate cross-linked files */
    bool     wrote;                     /* the device was changed */
    bool     clean_now;                 /* the last scan found no error and no leftover */
    bool     flags_cleared;             /* the dirty flag was cleared */
} FatFixStats;

/* `ck` must be open on a volume with a write callback and FatCkIo.keep >= 1 (a few thousand is sensible). Returns 0 when the passes ran
 * (fatck_result(ck) is the last scan), a negative code when a scan could not finish. */
int fatfix_run(FatCk *ck, const FatFixOpts *o, FatFixStats *st);

#endif
