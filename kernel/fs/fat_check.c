/*
 * Keine goes through the ledger before she lets anyone write in it: the kernel side of the FAT consistency check.
 *
 * The scan itself is shared/fatck.c (the same source runs in /bin/fsck and in the host tests). This file gives it a block device to read,
 * heap memory and a deadline, keeps the first few problems it finds as text, and decides what the answer means for the mount policy
 * (fatcheck.h has the contract). It never writes: the session has no write callback at all.
 */
#include "fatcheck.h"
#include "fatck.h"
#include "kernel.h"
#include "vfs.h"
#include "task.h"
#include "mm.h"
#include "irq.h"

#define CHECK_BUDGET_MS 120000          /* a volume that cannot be gone through in two minutes stays read-only */
#define MAX_REPORTS 4

typedef struct { BlockDev *dev; FatCheckReport *rep; } CheckCtx;

static int io_read(void *ctx, uint64_t off, void *buf, size_t n) {
    CheckCtx *c = ctx;
    u32 ss = c->dev->sector_size;
    if (off % ss || n % ss) return -EINVAL;
    return blk_read(c->dev, off / ss, (u32)(n / ss), buf);
}

static void *io_alloc(void *ctx, size_t n) { (void)ctx; return kzalloc(n); }
static void io_release(void *ctx, void *p, size_t n) { (void)ctx; (void)n; kfree(p); }
static uint64_t io_now(void *ctx) { (void)ctx; return uptime_ms(); }

static void io_report(void *ctx, const FatCkFinding *f, const char *text) {
    CheckCtx *c = ctx;
    FatCheckReport *r = c->rep;
    if (f->sev == FS_ERR) {
        if (r->nerr_lines < FATCHECK_ERR_LINES) strlcpy(r->err_line[r->nerr_lines++], text, FATCHECK_LINE);
    } else if (f->sev == FS_LEAK) {
        if (r->nleak_lines < FATCHECK_LEAK_LINES) strlcpy(r->leak_line[r->nleak_lines++], text, FATCHECK_LINE);
    }
}

/* Runs the check on `dev`, which holds one FAT volume. See fatcheck.h for the return value. */
int fat_check_device(BlockDev *dev, FatCheckReport *rep) {
    memset(rep, 0, sizeof *rep);
    strlcpy(rep->dev, dev->name, sizeof rep->dev);
    CheckCtx ctx = { dev, rep };
    FatCkIo io;
    memset(&io, 0, sizeof io);
    io.read = io_read;
    io.alloc = io_alloc;
    io.release = io_release;
    io.now_ms = io_now;
    io.report = io_report;
    io.ctx = &ctx;
    io.bytes = (uint64_t)dev->sectors * dev->sector_size;
    io.align = dev->sector_size;
    u64 t0 = uptime_ms();
    io.deadline_ms = t0 + CHECK_BUDGET_MS;
    io.max_windows = 256;               /* 4 MiB of FAT and directory windows */
    io.arena_bytes = 2u << 20;
    FatCk *ck;
    int rc = fatck_open(&ck, &io);
    if (rc) {                           /* the boot sector is unusable: the driver will refuse this volume with its own words */
        rep->errors = rep->nerr_lines ? rep->nerr_lines : 1;
        rep->verdict = -2;
        return -2;
    }
    bool dirty = false, hard = false;
    rc = fatck_quick_state(ck, &dirty, &hard);
    rep->bits = fatck_geom(ck)->bits;
    rep->dirty = dirty;
    rep->hard_error = hard;
    if (rc) {
        snprintf(rep->err_line[0], FATCHECK_LINE, "the device failed while the flags were being read (%d)", rc);
        rep->nerr_lines = rep->errors = 1;
        rep->incomplete = true;
        rep->verdict = -1;
        fatck_close(ck);
        return -1;
    }
    if (!dirty && !hard) {
        fatck_close(ck);
        rep->verdict = 0;
        return 0;
    }
    rep->ran = true;
    klog("Keine", "%s was not closed properly: Keine goes through the whole ledger, read-only, before anyone writes in it", dev->name);
    rc = fatck_scan(ck);
    const FatCkResult *r = fatck_result(ck);
    rep->ms = (u32)(uptime_ms() - t0);
    rep->errors = r->errors;
    rep->leaks = r->leaks;
    rep->warnings = r->warnings;
    rep->files = r->files;
    rep->dirs = r->dirs;
    rep->free_clusters = r->free_clusters;
    rep->lost_clusters = r->lost_clusters;
    rep->incomplete = r->incomplete || rc != 0;
    fatck_close(ck);
    if (rep->incomplete || rep->errors) rep->verdict = -1;
    else if (rep->leaks || hard) rep->verdict = 2;
    else rep->verdict = 1;
    return rep->verdict;
}

/* ---------------------------------------------------------------------------------------------- /proc/fatcheck */

static FatCheckReport g_reports[MAX_REPORTS];
static unsigned g_nreports, g_next;
static Mutex g_lock;
static bool g_lock_ready;

static void lock_once(void) {
    if (!g_lock_ready) { mutex_init(&g_lock); g_lock_ready = true; }
}

void fat_check_publish(const FatCheckReport *rep) {
    lock_once();
    mutex_lock(&g_lock);
    unsigned slot = g_nreports;
    for (unsigned i = 0; i < g_nreports; i++)
        if (!strcmp(g_reports[i].dev, rep->dev)) slot = i;
    if (slot == g_nreports) {
        if (g_nreports < MAX_REPORTS) g_nreports++;
        else slot = g_next++ % MAX_REPORTS;
    }
    g_reports[slot] = *rep;
    mutex_unlock(&g_lock);
}

static size_t gen_fatcheck(char *b, size_t cap) {
    lock_once();
    size_t n = 0;
    mutex_lock(&g_lock);
    if (!g_nreports) n += (size_t)snprintf(b + n, cap - n, "no volume has needed a consistency check since boot (only volumes flagged dirty get one)\n");
    for (unsigned i = 0; i < g_nreports && n < cap; i++) {
        const FatCheckReport *r = &g_reports[i];
        const char *v = r->verdict < 0 ? (r->incomplete ? "UNVERIFIED" : "DAMAGED") : r->verdict == 0 ? "clean" : "in order";
        n += (size_t)snprintf(b + n, cap - n, "%s: FAT%d, %s%s: %s", r->dev, r->bits, r->dirty ? "flagged dirty" : "flags clear",
                              r->hard_error ? ", disk errors flagged" : "", v);
        if (r->ran) n += (size_t)snprintf(b + n, cap - n, " (%u ms, %u files in %u directories, %u free clusters, %u lost)", r->ms, r->files, r->dirs, r->free_clusters, r->lost_clusters);
        n += (size_t)snprintf(b + n, cap - n, "\n");
        if (!r->ran) continue;
        n += (size_t)snprintf(b + n, cap - n, "  %u error(s), %u harmless leftover(s), %u warning(s)\n", r->errors, r->leaks, r->warnings);
        for (u32 k = 0; k < r->nerr_lines && n < cap; k++) n += (size_t)snprintf(b + n, cap - n, "  error:    %s\n", r->err_line[k]);
        if (r->errors > r->nerr_lines) n += (size_t)snprintf(b + n, cap - n, "  ... and %u more error(s)\n", r->errors - r->nerr_lines);
        for (u32 k = 0; k < r->nleak_lines && n < cap; k++) n += (size_t)snprintf(b + n, cap - n, "  leftover: %s\n", r->leak_line[k]);
        if (r->verdict < 0)
            n += (size_t)snprintf(b + n, cap - n, "  fix: as root, `umount /HumanVillage`, `fsck -a /dev/%s` (`fsck -y` also repairs what needs a decision), then reboot\n", r->dev);
        else if (r->verdict == 2)
            n += (size_t)snprintf(b + n, cap - n, "  mounted read-write; `fsck -a /dev/%s` tidies the leftovers and clears the dirty flag\n", r->dev);
    }
    mutex_unlock(&g_lock);
    return n < cap ? n : cap;
}

void fat_check_init(void) {
    static bool done;
    if (done) return;
    done = true;
    lock_once();
    procfs_register("fatcheck", gen_fatcheck);
}
