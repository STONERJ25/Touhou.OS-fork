/*
 * Keine goes through the ledger before she lets anyone write in it: the kernel side of the FAT consistency check, and of the opt-in repair.
 *
 * The scan itself is shared/fatck.c (the same source runs in /bin/fsck and in the host tests). This file gives it a block device to read,
 * heap memory and a deadline, keeps the first few problems it finds as text, and decides what the answer means for the mount policy
 * (fatcheck.h has the contract). The check never writes: its session has no write callback at all.
 *
 * With `hvfsck=safe` on the kernel command line the mount policy follows a check that found damage with fat_repair_device(): the repair
 * engine of /bin/fsck (shared/fatfix.c) runs with FIX_SAFE on the unmounted volume, in a session of its own that writes through the block
 * layer and flushes the device where the engine asks for a barrier, and a second check, in a fresh session that reads everything from the
 * device again, says what the volume is like afterwards. docs/STORAGE.md ("Mending at boot") has the design and the limits.
 */
#include "fatcheck.h"
#include "fatck.h"
#include "fatfix.h"
#include "kernel.h"
#include "vfs.h"
#include "task.h"
#include "mm.h"
#include "irq.h"
#include "tcowdt.h"

#define CHECK_BUDGET_MS 120000          /* a volume that cannot be gone through in two minutes stays read-only */
#define REPAIR_BUDGET_MS (15 * 60000)   /* the whole repair (every pass of the engine) gets a quarter of an hour; see docs/STORAGE.md for the numbers */
#define SECOND_BUDGET_MS (5 * 60000)    /* ... and the check after it five minutes */
#define BEAT_MS 15000                   /* a repair says that it is still at it this often */
#define MAX_REPORTS 4

typedef struct {
    BlockDev *dev;
    FatCheckReport *rep;
    u64 t0, last_beat;
    u32 beat_ms;                        /* 0: say nothing while it works (the check); else a line every beat_ms (the repair) */
} CheckCtx;

/* The check and the repair run in the boot thread, before the Momiji thread that feeds the hardware watchdog exists, and a big volume can take longer than the timer
 * the loader armed: every call into the device feeds it (tcowdt_boot_feed, kernel/drivers/tcowdt.c). */
static int io_read(void *ctx, uint64_t off, void *buf, size_t n) {
    CheckCtx *c = ctx;
    u32 ss = c->dev->sector_size;
    if (off % ss || n % ss) return -EINVAL;
    tcowdt_boot_feed();
    return blk_read(c->dev, off / ss, (u32)(n / ss), buf);
}

static int io_write(void *ctx, uint64_t off, const void *buf, size_t n) {
    CheckCtx *c = ctx;
    u32 ss = c->dev->sector_size;
    if (off % ss || n % ss) return -EINVAL;
    tcowdt_boot_feed();
    return blk_write(c->dev, off / ss, (u32)(n / ss), buf);
}

static int io_flush(void *ctx) {
    tcowdt_boot_feed();
    return blk_flush(((CheckCtx *)ctx)->dev);
}

static void *io_alloc(void *ctx, size_t n) { (void)ctx; return kzalloc(n); }
static void io_release(void *ctx, void *p, size_t n) { (void)ctx; (void)n; kfree(p); }

/* The clock the scanner asks for its deadline. A repair can take a while on a big volume and the screen is all the owner has, so it also says
 * every so often that it is still at it. */
static uint64_t io_now(void *ctx) {
    CheckCtx *c = ctx;
    u64 now = uptime_ms();
    tcowdt_boot_feed();
    if (c->beat_ms && now - c->last_beat >= c->beat_ms) {
        c->last_beat = now;
        klog("Keine", "%s: Keine is still going through the ledger (%lu s so far)", c->dev->name, (unsigned long)((now - c->t0) / 1000));
    }
    return now;
}

static void io_report(void *ctx, const FatCkFinding *f, const char *text) {
    CheckCtx *c = ctx;
    FatCheckReport *r = c->rep;
    if (f->sev == FS_ERR) {
        if (r->nerr_lines < FATCHECK_ERR_LINES) strlcpy(r->err_line[r->nerr_lines++], text, FATCHECK_LINE);
    } else if (f->sev == FS_LEAK) {
        if (r->nleak_lines < FATCHECK_LEAK_LINES) strlcpy(r->leak_line[r->nleak_lines++], text, FATCHECK_LINE);
    }
}

static void session_io(FatCkIo *io, CheckCtx *ctx, BlockDev *dev) {
    memset(io, 0, sizeof *io);
    io->read = io_read;
    io->alloc = io_alloc;
    io->release = io_release;
    io->now_ms = io_now;
    io->ctx = ctx;
    io->bytes = (uint64_t)dev->sectors * dev->sector_size;
    io->align = dev->sector_size;
    io->max_windows = 256;              /* 4 MiB of FAT and directory windows */
    io->arena_bytes = 2u << 20;
}

/* Runs the check on `dev`, which holds one FAT volume. A volume whose flags say it is clean is not scanned unless `force` is set (the second
 * check of a repair wants to see the contents, not the flags); `announce` says in the log that the ledger is being gone through. */
static int run_check(BlockDev *dev, FatCheckReport *rep, bool force, bool announce, u32 budget_ms) {
    memset(rep, 0, sizeof *rep);
    strlcpy(rep->dev, dev->name, sizeof rep->dev);
    CheckCtx ctx = { .dev = dev, .rep = rep };
    FatCkIo io;
    session_io(&io, &ctx, dev);
    io.report = io_report;
    u64 t0 = uptime_ms();
    io.deadline_ms = t0 + budget_ms;
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
    if (!dirty && !hard && !force) {
        fatck_close(ck);
        rep->verdict = 0;
        return 0;
    }
    rep->ran = true;
    if (announce)
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
    else rep->verdict = (dirty || hard) ? 1 : 0;
    return rep->verdict;
}

/* Runs the check on `dev`, which holds one FAT volume. See fatcheck.h for the return value. */
int fat_check_device(BlockDev *dev, FatCheckReport *rep) { return run_check(dev, rep, false, true, CHECK_BUDGET_MS); }

/* ------------------------------------------------------------------------------------------------------------------------ the repair */

static void repair_say(void *ctx, int kind, const char *line) {
    CheckCtx *c = ctx;
    FatCheckReport *r = c->rep;
    r->rp_said++;
    if (r->rp_nlines < FATCHECK_REPAIR_LINES)
        snprintf(r->rp_line[r->rp_nlines++], FATCHECK_LINE, "%s%s", kind == FSAY_FIXED ? "fixed: " : "", line);
}

int fat_repair_device(BlockDev *dev, FatCheckReport *rep, u64 stamp) {
    rep->rp_state = FATREPAIR_STOPPED;
    rep->rp_rc = -EIO;
    rep->after_verdict = -1;
    CheckCtx ctx = { .dev = dev, .rep = rep, .beat_ms = BEAT_MS };
    ctx.t0 = ctx.last_beat = uptime_ms();
    FatCkIo io;
    session_io(&io, &ctx, dev);
    io.write = io_write;
    io.flush = io_flush;
    io.deadline_ms = ctx.t0 + REPAIR_BUDGET_MS;
    io.keep = 4096;                     /* the findings of a pass, with their text: what the engine repairs from */
    FatCk *ck;
    int rc = fatck_open(&ck, &io);
    if (rc) {
        rep->rp_rc = rc;
        return -1;
    }
    FatFixOpts o;
    memset(&o, 0, sizeof o);
    o.mode = FIX_SAFE;                  /* what `fsck -a` does: never a repair that costs a name or part of a file */
    o.say = repair_say;
    o.ctx = &ctx;
    o.unix_time = stamp;
    FatFixStats st;
    rc = fatfix_run(ck, &o, &st);
    fatck_close(ck);
    rep->rp_ms = (u32)(uptime_ms() - ctx.t0);
    rep->rp_rc = rc;
    rep->rp_passes = st.passes;
    rep->rp_repaired = st.repaired;
    rep->rp_declined = st.declined;
    rep->rp_failed = st.failed;
    rep->rp_lost_chains = st.lost_chains;
    rep->rp_lost_clusters = st.lost_clusters;
    rep->rp_copied = st.copied_clusters;
    rep->rp_cleared = st.flags_cleared;
    if (rc) return -1;                  /* what was written was written one repair at a time; the next start goes on from there */
    rep->rp_state = FATREPAIR_DONE;

    /* the second check: a fresh session, so that everything is read from the device again and not from the engine's cache */
    FatCheckReport *after = kzalloc(sizeof *after);
    if (!after) return -1;
    int v = run_check(dev, after, true, false, SECOND_BUDGET_MS);
    rep->after_incomplete = after->incomplete || v == -2;
    if (v == -2) v = -1;                                        /* (the volume that was just mended cannot be opened: no verdict is a verdict of read-only) */
    rep->after_verdict = v;
    rep->after_errors = after->errors;
    rep->after_leaks = after->leaks;
    rep->after_ms = after->ms;
    rep->after_lost = after->lost_clusters;
    rep->after_nlines = after->nerr_lines < FATCHECK_AFTER_LINES ? after->nerr_lines : FATCHECK_AFTER_LINES;
    for (u32 i = 0; i < rep->after_nlines; i++) strlcpy(rep->after_line[i], after->err_line[i], FATCHECK_LINE);
    if (after->nleak_lines) strlcpy(rep->after_leak, after->leak_line[0], FATCHECK_LINE);
    kfree(after);
    return v;
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

static const char *rc_text(int rc) {
    return rc == -5 ? "the device failed" : rc == -12 ? "out of memory" : rc == -62 ? "out of time" : rc == -22 ? "the volume could not be opened" : "see the log";
}

/* snprintf into b + n, never past cap (snprintf returns what it would have written, which can be more than fits) */
#define ADD(...) do { if (n < cap) { n += (size_t)snprintf(b + n, cap - n, __VA_ARGS__); if (n > cap) n = cap; } } while (0)

/* The repair section of one report: what hvfsck=safe did and what a second check found. */
static size_t gen_repair(char *b, size_t cap, const FatCheckReport *r) {
    size_t n = 0;
    switch (r->rp_state) {
    case FATREPAIR_READONLY:
        ADD("  hvfsck=safe: not done, the device is read-only (blk.ro without a blk.rw= for this partition, or a write-protected medium)\n");
        return n;
    case FATREPAIR_UNVERIFIED:
        ADD("  hvfsck=safe: not done, the check could not be finished, so there is no verdict to mend\n");
        return n;
    case FATREPAIR_STOPPED:
        ADD("  hvfsck=safe: stopped after %u ms (%s): what was written was written one repair at a time; the next start goes on from there\n",
            r->rp_ms, rc_text(r->rp_rc));
        break;
    case FATREPAIR_DONE:
        ADD("  hvfsck=safe: mended in %u ms, %u pass(es): %u repair(s)", r->rp_ms, r->rp_passes, r->rp_repaired);
        if (r->rp_lost_chains) ADD(", %u lost chain(s) of %u cluster(s) saved in FOUND.000", r->rp_lost_chains, r->rp_lost_clusters);
        if (r->rp_copied) ADD(", %u cluster(s) copied to part cross-linked files", r->rp_copied);
        if (r->rp_declined) ADD(", %u left alone because they need -y", r->rp_declined);
        if (r->rp_failed) ADD(", %u could not be made", r->rp_failed);
        ADD("\n");
        break;
    default:
        return 0;
    }
    for (u32 k = 0; k < r->rp_nlines; k++) ADD("    %s\n", r->rp_line[k]);
    if (r->rp_said > r->rp_nlines) ADD("    ... and %u more\n", r->rp_said - r->rp_nlines);
    if (r->rp_state == FATREPAIR_DONE) {
        ADD("  afterwards (gone through again from the device, %u ms): %u error(s), %u harmless leftover(s)%s\n", r->after_ms, r->after_errors, r->after_leaks,
            r->after_incomplete ? ", NOT FINISHED" : r->after_verdict == 0 ? ": the ledger is in order and the volume is clean" : "");
        for (u32 k = 0; k < r->after_nlines; k++) ADD("  error:    %s\n", r->after_line[k]);
        if (r->after_errors > r->after_nlines) ADD("  ... and %u more error(s)\n", r->after_errors - r->after_nlines);
        if (r->after_verdict == 2 && r->after_leak[0]) ADD("  leftover: %s\n", r->after_leak);
    }
    return n;
}

static size_t gen_fatcheck(char *b, size_t cap) {
    lock_once();
    size_t n = 0;
    mutex_lock(&g_lock);
    if (!g_nreports) ADD("no volume has needed a consistency check since boot (only volumes flagged dirty get one)\n");
    for (unsigned i = 0; i < g_nreports && n < cap; i++) {
        const FatCheckReport *r = &g_reports[i];
        const char *v = r->verdict < 0 ? (r->incomplete ? "UNVERIFIED" : "DAMAGED") : r->verdict == 0 ? "clean" : "in order";
        ADD("%s: FAT%d, %s%s: %s", r->dev, r->bits, r->dirty ? "flagged dirty" : "flags clear", r->hard_error ? ", disk errors flagged" : "", v);
        if (r->ran) ADD(" (%u ms, %u files in %u directories, %u free clusters, %u lost)", r->ms, r->files, r->dirs, r->free_clusters, r->lost_clusters);
        ADD("\n");
        if (!r->ran) continue;
        ADD("  %u error(s), %u harmless leftover(s), %u warning(s)\n", r->errors, r->leaks, r->warnings);
        for (u32 k = 0; k < r->nerr_lines; k++) ADD("  error:    %s\n", r->err_line[k]);
        if (r->errors > r->nerr_lines) ADD("  ... and %u more error(s)\n", r->errors - r->nerr_lines);
        for (u32 k = 0; k < r->nleak_lines; k++) ADD("  leftover: %s\n", r->leak_line[k]);
        if (r->rp_state) {
            size_t m = gen_repair(b + n, cap - n, r);
            n += m;
        }
        /* what the mount was based on: the second check when a repair ran to the end, else the first */
        int final = r->rp_state == FATREPAIR_DONE ? r->after_verdict : r->verdict;
        if (r->rp_state == FATREPAIR_DONE && final < 0)
            ADD("  what is left needs a decision, which only fsck -y makes: restart once with nohumanvillage on the kernel command line (nothing is mounted then), "
                "run `fsck -y /dev/%s` as root, take the word out again\n", r->dev);
        else if (final < 0) {
            if (r->rp_state == FATREPAIR_NONE && !r->incomplete)       /* the word is not set: it is the way that needs no hands */
                ADD("  fix: put hvfsck=safe on the kernel command line (cmdline.txt on the EFI partition) and restart: Keine then makes the repairs of fsck -a "
                    "before she mounts the volume\n");
            ADD("  fix: as root, `umount /HumanVillage`, `fsck -a /dev/%s` (`fsck -y` also repairs what needs a decision), then reboot\n", r->dev);
        }
        else if (final == 2)
            ADD("  mounted read-write; `fsck -a /dev/%s` tidies the leftovers and clears the dirty flag\n", r->dev);
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
