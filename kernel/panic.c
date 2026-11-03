/*
 * Kernel panics, and the record that outlives them (docs/RELIABILITY.md). Hieda no Akyuu remembers in each life what the previous one
 * saw; the record remembers across a reset what the previous start saw.
 *
 * panic() and panic_frame() (trap.c, for fatal exceptions) print the incident between panic_begin() and panic_end(); panic_end() then:
 *   1. says what happens next, and writes the record: the last 60 KiB of the kernel log (the panic text, the registers and the backtrace at
 *      its end) go into the RAM region the loader kept out of the memory map (BootInfo.persist_*, boot/recovery.c) with a CRC-32, through an
 *      uncached mapping, so that the bytes are in DRAM and not in a cache line when the machine resets;
 *   2. lets the netconsole send what it has not sent yet, through the card's polled transmit path (netcons.c);
 *   3. restarts the machine after "panic=SECONDS" (default 30 on a start under trial, update.trial=N: a panicking update then ends in the
 *      loader's fallback without a person; off otherwise: the screen stays for whoever looks, and an armed watchdog restarts the machine when
 *      its timeout runs out).
 * A panic inside one of these steps (a fault in the netconsole's path, say) goes on with the next step; a third one only stops.
 *
 * While the system runs, the record's header says "running" with a heartbeat every 5 s (uptime, wall clock, who feeds the watchdog); a
 * restart or poweroff that was asked for marks it "shut down". So the next start can say how the previous one ended: a panic ("previous
 * panic: ...", and the panic's lines go into this start's log, so into TOUHOU.LOG and the netconsole), a clean shutdown, or neither ("ended
 * without a shutdown or a panic": a hang ended by the watchdog or the reset button, or a power cut), with the last sign of life.
 *
 * /proc/panic shows the policy, the region and the previous record (its last part: a /proc file holds 16 KiB). Root may write "panic" (a
 * deliberate panic) or "pf" (a page fault in kernel mode, which takes the fatal-exception path) to it: that is how the tests, and the checks
 * on real hardware, cause one.
 */
#include "kernel.h"
#include "cpu.h"
#include "irq.h"
#include "arch.h"
#include "task.h"
#include "fbcon.h"
#include "mm.h"
#include "ktime.h"
#include "cmdline.h"
#include "process.h"
#include "initcall.h"
#include "vfs.h"
#include "panicrec.h"
#include "tcowdt.h"
#include "netcons.h"

int procfs_register_rw(const char *name, size_t (*gen)(char *buf, size_t cap), ssize_t (*wr)(const char *buf, size_t n));

#define REC_MAGIC    0x3143455255594B41ULL      /* "AKYUREC1" */
#define REC_VERSION  1
#define REC_TEXT     4096                       /* the text follows the header at this offset */
#define TEXT_MAX     (60u * 1024)

enum { REC_RUNNING = 1, REC_SHUTDOWN = 2, REC_PANIC = 3 };

typedef struct {
    u64  magic;
    u32  version, size;             /* REC_VERSION; the region's size */
    u32  boot;                      /* the number of this start: one more than the start before (1 when there was no record) */
    u32  state;                     /* REC_* */
    u64  alive_ms;                  /* uptime at the last heartbeat, the shutdown or the panic */
    u64  alive_sec;                 /* the wall clock then (Unix time as the kernel knew it) */
    u32  feeder, wdt_seconds;       /* the watchdog then (TCOWDT_*) */
    u32  text_len, text_crc;        /* REC_PANIC: the text at REC_TEXT and its CRC-32 */
    u32  trial;                     /* update.trial=N of that start, 0 when it was not on trial */
    u32  reserved;
    char build[24];                 /* when that kernel was built: tells kernel.elf from kernel.old */
    char reason[200];               /* REC_PANIC: the panic's own line */
    u32  crc;                       /* CRC-32 of everything above */
} Rec;

static volatile u8 *g_region;       /* the region, mapped uncached; NULL until mapped */
static Rec g_hdr;                   /* this start's header; copied to the region after every change */
static bool g_hdr_ready;
static char g_text[TEXT_MAX];       /* scratch for the text, in kernel memory (the panic path cannot allocate) */
static char *g_prev_text;           /* the previous start's panic text, for /proc/panic */
static size_t g_prev_len;
static Rec g_prev;                  /* the previous start's header */
static bool g_prev_valid;
static const char *g_none_why;      /* why there is no region */

/* ---- CRC-32 (the IEEE 802.3 polynomial, reflected), four bits at a time ---- */

static u32 crc32(const void *data, size_t n) {
    static u32 t[16];
    if (!t[8]) for (u32 i = 0; i < 16; i++) { u32 c = i; for (int k = 0; k < 4; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1; t[i] = c; }
    const u8 *p = data;
    u32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        c = (c >> 4) ^ t[c & 15];
        c = (c >> 4) ^ t[c & 15];
    }
    return ~c;
}

/* The log as the console received it, escape sequences included, made plain text: a cursor move (the banner) becomes a line break, other
 * escape sequences and control characters other than tab and newline go. In place; returns the new length. */
size_t klog_plain(char *s, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1B) {
            size_t j = i + 1;
            if (j < n && s[j] == '[') {
                j++;
                while (j < n && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';' || s[j] == '?')) j++;
                if (j < n && (s[j] == 'H' || s[j] == 'f') && o && s[o - 1] != '\n') s[o++] = '\n';
            }
            i = j < n ? j : n;
            continue;
        }
        if (c == '\n' || c == '\t' || c >= 0x20) s[o++] = (char)c;
    }
    return o;
}

/* ---- the region ---- */

static volatile u8 *region(void) {
    if (g_region) return g_region;
    if (g_boot.version < 5 || !g_boot.persist_phys || g_boot.persist_len < REC_TEXT + 4096) return NULL;
    if (!vmm_kernel_pml4()) return (volatile u8 *)phys_to_virt(g_boot.persist_phys);   /* still on the loader's page tables: their direct map reaches it */
    g_region = ioremap(g_boot.persist_phys, g_boot.persist_len, PTE_PCD);
    return g_region;
}

static size_t text_room(void) { return MIN((size_t)TEXT_MAX, (size_t)g_boot.persist_len - REC_TEXT); }

static void hdr_store(void) {
    volatile u8 *r = region();
    if (!r) return;
    g_hdr.crc = crc32(&g_hdr, offsetof(Rec, crc));
    memcpy((void *)r, &g_hdr, sizeof g_hdr);
    __asm__ volatile ("mfence" ::: "memory");
}

static bool rec_valid(const Rec *p) {
    return p->magic == REC_MAGIC && p->version == REC_VERSION && p->size == g_boot.persist_len && crc32(p, offsetof(Rec, crc)) == p->crc &&
           p->text_len <= text_room();
}

/* This start's header: numbered after the previous start's, when there is one. */
static void hdr_begin(void) {
    volatile u8 *r = region();
    if (!r || g_hdr_ready) return;
    Rec prev;
    memcpy(&prev, (const void *)r, sizeof prev);
    memset(&g_hdr, 0, sizeof g_hdr);
    g_hdr.magic = REC_MAGIC;
    g_hdr.version = REC_VERSION;
    g_hdr.size = (u32)g_boot.persist_len;
    g_hdr.boot = rec_valid(&prev) ? prev.boot + 1 : 1;
    g_hdr.state = REC_RUNNING;
    g_hdr.trial = (u32)cmdline_int("update.trial", 0);
    strlcpy(g_hdr.build, __DATE__ " " __TIME__, sizeof g_hdr.build);
    g_hdr_ready = true;
}

static void heartbeat(void) {
    g_hdr.alive_ms = uptime_ms();
    g_hdr.alive_sec = time_now_sec();
    g_hdr.feeder = (u32)tcowdt_feeder();
    g_hdr.wdt_seconds = tcowdt_timeout();
    hdr_store();
}

void panicrec_shutdown(void) {
    if (!g_hdr_ready) return;
    g_hdr.state = REC_SHUTDOWN;
    heartbeat();
}

/* ---- the panic path ---- */

static volatile int g_depth;
static int g_stage;                 /* how far panic_end() got: 1 the record, 2 the netconsole, 3 the restart */

bool panic_in_progress(void) { return g_depth > 0; }

int panic_begin(void) {
    cli();
    int d = ++g_depth;
    if (d == 1) fbcon_force_on();   /* the window server may own the screen: a panic must be readable (there is no serial port on a PC) */
    return d;
}

/* panic=SECONDS; *explicit is false when it is the default (30 under a trial start, off otherwise). */
static long panic_seconds(bool *explicit_) {
    char v[16];
    *explicit_ = cmdline_value("panic", v, sizeof v);
    if (*explicit_) {
        if (!strcmp(v, "off") || !strcmp(v, "no")) return 0;
        long s = cmdline_int("panic", 0);
        return s < 0 ? 0 : s > 3600 ? 3600 : s;
    }
    return cmdline_value("update.trial", v, sizeof v) ? 30 : 0;
}

/* The line after the last "*** INCIDENT" header: what the panic said. */
static void find_reason(const char *t, size_t n, char *out, size_t cap) {
    static const char head[] = "*** INCIDENT: KERNEL PANIC ***";
    const size_t hl = sizeof head - 1;
    out[0] = 0;
    for (size_t i = n >= hl ? n - hl + 1 : 0; i-- > 0;) {
        if (memcmp(t + i, head, hl)) continue;
        size_t s = i + hl;
        while (s < n && t[s] != '\n') s++;
        if (s < n) s++;
        size_t e = s;
        while (e < n && t[e] != '\n') e++;
        size_t l = MIN(e - s, cap - 1);
        memcpy(out, t + s, l);
        out[l] = 0;
        return;
    }
}

static void record_panic(void) {
    volatile u8 *r = region();
    if (!r) return;
    hdr_begin();
    u64 from = klog_total();
    size_t room = text_room();
    from = from > room ? from - room : 0;
    size_t got = klog_read_clamped(&from, g_text, room);
    size_t len = klog_plain(g_text, got);
    memcpy((void *)(r + REC_TEXT), g_text, len);
    g_hdr.text_len = (u32)len;
    g_hdr.text_crc = crc32(g_text, len);
    find_reason(g_text, len, g_hdr.reason, sizeof g_hdr.reason);
    g_hdr.state = REC_PANIC;
    heartbeat();
    __asm__ volatile ("wbinvd" ::: "memory");       /* (the early mapping through the loader's direct map is write-back) */
}

static void say_policy(void) {
    bool explicit_;
    long s = panic_seconds(&explicit_);
    if (s > 0) klog("Akyuu", "the machine restarts in %ld s (panic=%ld%s)", s, s, explicit_ ? "" : ": the default on a start under trial");
    else if (tcowdt_timeout()) klog("Akyuu", "the machine stays like this; the watchdog restarts it within %u s (panic=SECONDS restarts it sooner, wdt=off panic=0 keep it here)",
                                    tcowdt_seconds_left());
    else klog("Akyuu", "the machine stays like this (panic=SECONDS on the kernel command line restarts it by itself)");
    if (region()) klog("Akyuu", "the record of this panic is kept in RAM for the next start (%#lx)", (unsigned long)g_boot.persist_phys);
}

NORETURN void panic_end(void) {
    if (g_depth <= 2) {
        if (g_stage < 1) { g_stage = 1; say_policy(); record_panic(); }
        if (g_stage < 2) { g_stage = 2; netcons_panic_flush(); }
    }
    if (g_stage < 3) {
        g_stage = 3;
        bool explicit_;
        long s = panic_seconds(&explicit_);
        if (s > 0) {
            for (long i = 0; i < s * 10; i++) udelay(100000);
            klog("Akyuu", "restarting now");
            netcons_panic_flush();
            tcowdt_panic_restart();
            machine_reboot(0);
        }
    }
    for (;;) { cli(); hlt(); }
}

NORETURN void panic(const char *fmt, ...) {
    int depth = panic_begin();
    va_list ap;
    va_start(ap, fmt);
    if (depth == 1) {
        printk("\n\x1b[1;31m*** INCIDENT: KERNEL PANIC ***\x1b[0m\n");
        vprintk(fmt, ap);
        printk("\n");
        printk("\x1b[90m%s\x1b[0m\n", incident_quip(255, 0, 0));
        task_print_current();
        dump_backtrace((u64)__builtin_return_address(0), (u64)__builtin_frame_address(0));
    } else if (depth == 2) {
        printk("\n*** a second incident while the first was being handled: ");
        vprintk(fmt, ap);
        printk("\n");
    }
    va_end(ap);
    panic_end();
}

/* ---- at boot: the previous start ---- */

static void report_previous(void) {
    const Rec *p = &g_prev;
    const char *trial = p->trial ? ", on trial" : "";
    unsigned long secs = (unsigned long)(p->alive_ms / 1000);
    if (p->state == REC_SHUTDOWN) {
        klog("Akyuu", "the previous start (#%u%s, kernel built %s) ended with a restart or poweroff that was asked for, %lu s after it began", p->boot, trial, p->build, secs);
        return;
    }
    if (p->state != REC_PANIC) {
        klog("Akyuu", "the previous start (#%u%s, kernel built %s) ended without a shutdown or a panic: its last sign of life came %lu s after it began "
             "(a hang that the watchdog or a person ended, or a power cut)", p->boot, trial, p->build, secs);
        return;
    }
    klog("Akyuu", "previous panic: %s (start #%u%s, %lu s after it began, kernel built %s)", p->reason[0] ? p->reason : "(no reason line)", p->boot, trial, secs, p->build);
    size_t n = p->text_len;
    memcpy(g_text, (const void *)(g_region + REC_TEXT), n);
    if (crc32(g_text, n) != p->text_crc) {
        klog("Akyuu", "  (the text of that panic did not survive: its checksum does not match)");
        return;
    }
    g_prev_text = kmalloc(n + 1);
    if (g_prev_text) { memcpy(g_prev_text, g_text, n); g_prev_text[n] = 0; g_prev_len = n; }
    static const char head[] = "*** INCIDENT: KERNEL PANIC ***";
    size_t start = 0;
    for (size_t i = 0; i + sizeof head - 1 <= n; i++) if (!memcmp(g_text + i, head, sizeof head - 1)) start = i;
    int lines = 0;
    for (size_t i = start; i < n && lines < 120;) {
        size_t e = i;
        while (e < n && g_text[e] != '\n') e++;
        char line[200];
        size_t l = MIN(e - i, sizeof line - 1);
        memcpy(line, g_text + i, l);
        line[l] = 0;
        klog("Akyuu", "  | %s", line);
        lines++;
        i = e + 1;
    }
    klog("Akyuu", "that start's log up to the panic (its last %lu KiB) is in /proc/panic", (unsigned long)(n >> 10));
}

/* ---- /proc/panic ---- */

static size_t gen_proc(char *b, size_t cap) {
    size_t n = 0;
#define P(...) do { if (n < cap) n += (size_t)snprintf(b + n, cap - n, __VA_ARGS__); } while (0)
    bool explicit_;
    long s = panic_seconds(&explicit_);
    if (s > 0) P("policy:   restart %ld s after a panic%s\n", s, explicit_ ? " (panic=)" : " (the default on a start under trial)");
    else P("policy:   stay after a panic%s\n", tcowdt_timeout() ? " (the watchdog restarts the machine)" : "");
    if (g_region) P("record:   RAM at %#lx, %lu KiB, uncached; this start is #%u\n", (unsigned long)g_boot.persist_phys, (unsigned long)(g_boot.persist_len >> 10), g_hdr.boot);
    else P("record:   none (%s)\n", g_none_why ? g_none_why : "no region");
    if (!g_prev_valid) P("previous: no record\n");
    else if (g_prev.state == REC_SHUTDOWN) P("previous: start #%u ended with a restart or poweroff that was asked for, after %lu s\n", g_prev.boot, (unsigned long)(g_prev.alive_ms / 1000));
    else if (g_prev.state == REC_PANIC) P("previous: start #%u ended in a kernel panic after %lu s: %s\n", g_prev.boot, (unsigned long)(g_prev.alive_ms / 1000), g_prev.reason);
    else P("previous: start #%u ended without a shutdown or a panic; last sign of life after %lu s\n", g_prev.boot, (unsigned long)(g_prev.alive_ms / 1000));
    if (g_prev_text && n + 64 < cap) {
        P("--- the end of that start's log ---\n");
        size_t room = cap - n - 1, from = g_prev_len > room ? g_prev_len - room : 0;
        while (from < g_prev_len && from && g_prev_text[from - 1] != '\n') from++;          /* begin at a line */
        size_t l = g_prev_len - from;
        memcpy(b + n, g_prev_text + from, l);
        n += l;
    }
#undef P
    return MIN(n, cap);
}

static ssize_t put_proc(const char *buf, size_t n) {
    char w[16];
    size_t l = MIN(n, sizeof w - 1);
    memcpy(w, buf, l);
    w[l] = 0;
    while (l && (w[l - 1] == '\n' || w[l - 1] == ' ')) w[--l] = 0;
    Process *p = current ? current->proc : NULL;
    if (!strcmp(w, "panic")) panic("requested through /proc/panic by '%s' (pid %d)", p ? p->name : "?", p ? p->pid : 0);
    if (!strcmp(w, "pf")) {
        klog("Akyuu", "a page fault in kernel mode, as requested through /proc/panic");
        *(volatile u64 *)0xdead0000UL = 1;
    }
    return -EINVAL;
}

static void heartbeat_main(void *arg) {
    (void)arg;
    for (;;) {
        task_sleep_ms(5000);
        if (!panic_in_progress()) heartbeat();
    }
}

static void panicrec_init(void) {
    procfs_register_rw("panic", gen_proc, put_proc);
    if (g_boot.version < 5) { g_none_why = "the loader is older than the record (push it with --loader)"; }
    else if (!g_boot.persist_phys) {
        u32 note = g_boot.persist_note & 0xFF;
        g_none_why = note == BOOT_PERSIST_OFF ? "persist=off" : note == BOOT_PERSIST_BUSY ? "the firmware used that memory in this start" :
                     note == BOOT_PERSIST_NOTRAM ? "that address is not RAM on this machine" : "the loader did not keep one";
    }
    if (!region()) {
        if (!g_none_why) g_none_why = "the region is too small";
        klog("Akyuu", "no panic record region (%s): a panic is kept only on the screen, in the netconsole and in the log file", g_none_why);
        return;
    }
    memcpy(&g_prev, (const void *)g_region, sizeof g_prev);
    g_prev_valid = rec_valid(&g_prev);
    if (g_prev_valid) report_previous();
    else klog("Akyuu", "the record region at %#lx holds no record of the previous start (the first start with it, a power cut, or firmware that clears memory%s)",
              (unsigned long)g_boot.persist_phys, (g_boot.persist_note & BOOT_PERSIST_MOR) ? ": MemoryOverwriteRequestControl is set" : " at a restart");
    hdr_begin();
    heartbeat();
    task_create_detached("Akyuu", heartbeat_main, NULL);
}
INITCALL(panicrec_init);
