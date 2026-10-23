/*
 * The kernel log on the stick. A PC without a serial port gives the owner of a failed first boot nothing but a photograph of the screen,
 * so once the HUMANVILLAGE volume is mounted read-write the kernel keeps what dmesg shows in two ordinary FAT files at its root:
 *
 *   TOUHOU.LOG   this boot's log, from the very first line (the whole ring is written when the volume comes up), then appended every
 *                ~5 s by a thread of its own, but only when new lines exist; kept under 256 KiB by starting over with the newest part
 *   TOUHOU.PRV   the previous boot's TOUHOU.LOG (rotated at mount time), so a hang followed by a power cycle leaves the failed boot readable
 *
 * Every write is followed by a sync of the volume, so the data is on the stick (and the volume marked clean again) within a moment of
 * being written: a hang plus a power cut one second later neither loses the lines nor leaves the volume flagged dirty, which would make the
 * next boot mount it read-only. A read-only or dirty-mounted volume (the policy mounts those read-only) is never written; the log then
 * stays in RAM as before. `bootlog=off` on the kernel command line switches the whole thing off.
 *
 * Poweroff and reboot write the pending lines plus a closing line first (bootlog_shutdown, called before the final vfs_sync_all). There
 * is no panic hook: the block drivers sleep and wait for interrupts (and, for a stick, for the USB thread), so a write from inside a
 * panic could only hang the machine mid-message; the panic text is on screen and serial, the file ends at the last periodic write.
 *
 * Wear: nothing is written when nothing was logged; a quiet system writes nothing at all. While the log is busy (boot) there is one
 * append and one sync per five seconds.
 */
#include "bootlog.h"
#include "vfs.h"
#include "kernel.h"
#include "cmdline.h"
#include "ktime.h"
#include "task.h"
#include "mm.h"
#include "irq.h"
#include "abi.h"

#define LOG_PATH   "/HumanVillage/TOUHOU.LOG"
#define PRV_PATH   "/HumanVillage/TOUHOU.PRV"
#define MAX_FILE   (256u * 1024)
#define BUF_MAX    (192u * 1024)               /* the in-memory ring is 128 KiB, so one buffer always holds all of it */
#define PERIOD_MS  5000
#define MAX_FAILS  3                           /* consecutive failed writes before the log gives up until the next mount */

static Mutex g_lock;
static bool g_ready, g_thread_started;
static volatile bool g_active;                 /* the file is being kept */
static u64 g_sent;                             /* klog offset: everything below it is in TOUHOU.LOG */
static u64 g_size;                             /* bytes in TOUHOU.LOG */
static int g_fails;

static bool disabled_on_cmdline(void) {
    char v[8];
    return cmdline_value("bootlog", v, sizeof v) && (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "no"));
}

static int write_all(File *f, const char *p, size_t n) {
    while (n) {
        ssize_t w = vfs_write(f, p, n > 32768 ? 32768 : n);
        if (w < 0) return (int)w;
        if (w == 0) return -EIO;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Puts the lines the file does not have yet into TOUHOU.LOG (appending), or starts the file over with what the ring holds
 * (start_over, or when the file would grow past MAX_FILE). `trailer` is a line that exists only in the file. Caller holds g_lock. */
static int flush_log(bool start_over, const char *trailer) {
    u64 total = klog_total();
    u64 from = start_over ? 0 : g_sent;
    size_t want = (size_t)(total - from > BUF_MAX ? BUF_MAX : total - from);
    if (!start_over && g_size + want > MAX_FILE) {                       /* too big: keep the newest part */
        start_over = true;
        from = 0;
        want = (size_t)(total > BUF_MAX ? BUF_MAX : total);
    }
    if (!want && !trailer) return 0;
    char *buf = kmalloc(want + 1);
    if (!buf) return -ENOMEM;
    u64 off = from;
    size_t got = want ? klog_read_clamped(&off, buf, want) : 0;
    char head[128];
    head[0] = 0;
    if (start_over && g_sent > 0)
        snprintf(head, sizeof head, "-- bootlog: the file was getting big (%lu KiB), older lines were dropped --\n", (unsigned long)(g_size >> 10));
    else if (off > from && !start_over)
        snprintf(head, sizeof head, "-- bootlog: %lu bytes of the log were overwritten before they could be written --\n", (unsigned long)(off - from));
    File *f = NULL;
    int rc = vfs_open(LOG_PATH, O_WRONLY | O_CREAT | (start_over ? O_TRUNC : O_APPEND), 0644, NULL, NULL, &f);
    if (rc == 0) {
        rc = write_all(f, head, strlen(head));
        if (!rc) rc = write_all(f, buf, got);
        if (!rc && trailer) rc = write_all(f, trailer, strlen(trailer));
        u64 size = f->vn->size;
        file_put(f);
        if (!rc) {
            g_size = size;
            g_sent = off + got;
        }
    }
    kfree(buf);
    return rc;
}

static void fail(int rc) {
    if (++g_fails >= MAX_FAILS && g_active) {
        g_active = false;
        klog("Keine", "bootlog: writing TOUHOU.LOG failed %d times in a row (last error %d): the kernel log stays in RAM until the volume is mounted again", g_fails, rc);
    }
}

static void bootlog_thread(void *arg) {
    (void)arg;
    for (;;) {
        task_sleep_ms(PERIOD_MS);
        if (!g_active || klog_total() == g_sent) continue;
        mutex_lock(&g_lock);
        if (g_active && klog_total() != g_sent) {
            int rc = flush_log(false, NULL);
            if (!rc) {
                rc = vfs_sync_all();
                if (!rc) g_fails = 0;
            }
            if (rc) fail(rc);
        }
        mutex_unlock(&g_lock);
    }
}

void bootlog_volume_mounted(const char *mountpoint, bool rw) {
    (void)mountpoint;                                                   /* the paths above are for /HumanVillage, the only volume the policy mounts */
    if (disabled_on_cmdline()) {
        klog("Keine", "bootlog=off: the kernel log is not kept on the stick");
        return;
    }
    if (!rw) {
        klog("Keine", "TOUHOU.LOG is not kept: the Human Village is mounted read-only (a stick that was not unplugged cleanly, or a write-protected one)");
        return;
    }
    if (!g_ready) { mutex_init(&g_lock); g_ready = true; }
    mutex_lock(&g_lock);
    struct gstat st;
    if (vfs_stat(LOG_PATH, NULL, NULL, 0, &st) == 0) {                  /* last boot's log becomes TOUHOU.PRV, replacing the one before */
        int rc = vfs_rename(LOG_PATH, PRV_PATH, NULL, NULL);
        if (rc) klog("Keine", "bootlog: cannot rotate TOUHOU.LOG to TOUHOU.PRV (%d)", rc);
    }
    g_sent = 0;
    g_size = 0;
    g_fails = 0;
    klog("Keine", "the kernel log is kept on the stick: TOUHOU.LOG here, TOUHOU.PRV for the boot before -- Shikieiki's ledger, readable on any other computer");
    int rc = flush_log(true, NULL);
    if (!rc) rc = vfs_sync_all();
    if (rc) {
        klog("Keine", "bootlog: cannot write TOUHOU.LOG (%d): the kernel log stays in RAM", rc);
        mutex_unlock(&g_lock);
        return;
    }
    g_active = true;
    if (!g_thread_started) {
        g_thread_started = true;
        if (!task_create_detached("bootlog", bootlog_thread, NULL)) {
            klog("Keine", "bootlog: cannot start the writer thread; TOUHOU.LOG will only hold the log up to the mount and the shutdown");
        }
    }
    mutex_unlock(&g_lock);
}

void bootlog_shutdown(void) {
    if (!g_active) return;
    u64 end = uptime_ms() + 4000;
    while (!mutex_trylock(&g_lock)) {                                   /* the writer thread may be stuck on a dying stick: do not hang the shutdown on it */
        if (uptime_ms() > end) return;
        task_sleep_ms(10);
    }
    char trailer[96];
    snprintf(trailer, sizeof trailer, "-- bootlog: the machine is being shut down (uptime %lu ms); this line ends a clean run --\n", (unsigned long)uptime_ms());
    flush_log(false, trailer);
    mutex_unlock(&g_lock);
}
