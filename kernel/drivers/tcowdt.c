/*
 * The hardware watchdog: Momiji keeps watch (docs/RELIABILITY.md). Momiji Inubashiri is the white wolf tengu who stands guard on Youkai
 * Mountain; here she guards the machine: the Intel TCO timer (shared/tcohw.h has the hardware) resets it when nobody reloads it in time,
 * which turns a hung kernel or a hung user space into a restart. On a machine that is updated over the network that restart counts as a
 * failed start of an update on trial (boot/update.c), and after the second the loader starts the previous kernel: nobody has to go to the
 * machine and press its reset button.
 *
 * Who feeds it:
 *   the loader     arms it just before the kernel starts (boot/recovery.c) when the kernel carries BOOT_WDT_MARK (this file does), so
 *                  that a kernel that hangs before it gets here is restarted too;
 *   the kernel     the "Momiji" thread reloads it from boot until user space opens /dev/watchdog: a scheduler that stops lets it run out;
 *   user space     init's feeder (user/sbin/init.c) writes to /dev/watchdog once a second: a user space that hangs lets it run out;
 *   nobody         after /dev/watchdog was closed without the magic 'V' (the Linux protocol: any write reloads, a 'V' in the last write
 *                  before the last close stops it), in a shutdown (tcowdt_shutdown: the shutdown gets a time budget of its own) and
 *                  under "test" (the restart is the expected result).
 *
 * Kernel command line: wdt=SECONDS (2 to 600, default 120: from the last feed to the restart), wdt=off (the hardware is found and reported,
 * the timer is not armed, and stopped if the firmware or the loader left it running), wdt.nosmi (clear SMI_EN.TCO_EN, unless TCO_LOCK is set:
 * a firmware whose SMI handler swallows the first timeout would keep the machine from ever restarting), wdt.keepnr (leave NO_REBOOT as the
 * firmware set it).
 *
 * /proc/watchdog says what was found and who feeds it. Root writes "stop", "start [SECONDS]", "timeout SECONDS" or "test" (nobody feeds it
 * from now on: the machine must restart within the timeout). A second timeout that does not restart the machine is noticed by the thread,
 * whatever the reason (a pin strap, the firmware locking NO_REBOOT, an SMI handler) and reported: "the reset is blocked".
 */
#include "tcowdt.h"
#include "panicrec.h"
#include "kernel.h"
#include "cpu.h"
#include "irq.h"
#include "mm.h"
#include "vfs.h"
#include "task.h"
#include "ktime.h"
#include "cmdline.h"
#include "process.h"
#include "initcall.h"
#include "tcohw.h"

int procfs_register_rw(const char *name, size_t (*gen)(char *buf, size_t cap), ssize_t (*wr)(const char *buf, size_t n));

#define DEFAULT_SECONDS   120
#define SHUTDOWN_BUDGET   300           /* seconds a restart or poweroff gets before the watchdog ends it */

/* The loader looks for this string in the kernel image before it arms the timer (BOOT_WDT_MARK): this kernel takes it over. */
__attribute__((used)) static const char g_feeds_mark[] = BOOT_WDT_MARK;

static TcoHw g_hw;

static struct {
    bool found;                 /* a TCO watchdog is there */
    bool armed;                 /* the counter runs and leads to a reset */
    bool off;                   /* wdt=off */
    bool test;                  /* "test": nobody feeds it */
    bool shutdown;              /* a shutdown began: feeds are ignored, its budget runs */
    int  feeder;                /* TCOWDT_* */
    u32  seconds;               /* the timeout */
    u64  last_feed;             /* uptime ms of the last reload */
    int  opens;                 /* /dev/watchdog files open */
    int  user_pid;              /* who opened it first */
    char user_name[32];
    int  nr_before, nr;         /* NO_REBOOT when the kernel looked and after its attempt: 1 set, 0 clear, -1 unknown */
    bool keepnr;
    u16  boot_sts2;             /* TCO2_STS as this start found it */
    bool blocked;               /* a second timeout passed and the machine went on running */
    u64  blocked_ms;
    u32  late;                  /* first timeouts noticed (a feeder that came late), once each */
    bool late_now;              /* the one noticed last has not been fed away yet */
    u64  late_said;
    bool smi, smi_cleared, locked;
    u32  loader_state, loader_seconds;
    bool loader_v5;             /* the loader knows about the watchdog (BootInfo version 5) */
} W;

/* ---- the timer ---- */

static void feed_locked(void) {                    /* interrupts off */
    tcohw_reload(&g_hw);
    W.last_feed = uptime_ms();
    W.late_now = false;
}

static void arm_locked(u32 seconds) {
    W.seconds = seconds;
    tcohw_start(&g_hw, seconds);
    W.armed = true;
    W.last_feed = uptime_ms();
}

static void stop_locked(void) {
    tcohw_halt(&g_hw);
    W.armed = false;
    W.feeder = TCOWDT_OFF;
}

int tcowdt_feeder(void) { return W.armed ? W.feeder : TCOWDT_OFF; }
u32 tcowdt_timeout(void) { return W.armed ? W.seconds : 0; }

u32 tcowdt_seconds_left(void) {
    if (!W.armed) return 0;
    u64 gone = (uptime_ms() - W.last_feed) / 1000;
    return gone >= W.seconds ? 0 : W.seconds - (u32)gone;
}

static const char *nr_text(void) {
    if (W.nr == 1) return "NO_REBOOT stays set: a pin strap or the firmware keeps it";
    if (W.nr == -1) return g_hw.why ? g_hw.why : "NO_REBOOT unknown";
    if (W.smi) return "NO_REBOOT is clear, but the first timeout raises an SMI (SMI_EN.TCO_EN): the firmware's handler may swallow it; wdt.nosmi";
    return "NO_REBOOT is clear: something else holds the reset back";
}

/* The thread: feeds the timer while the kernel is the feeder, and keeps an eye on what the hardware says. */
static void momiji_main(void *arg) {
    (void)arg;
    for (;;) {
        u64 period = W.armed && W.feeder == TCOWDT_KERNEL ? MAX(250u, MIN(5000u, W.seconds * 250u)) : 500u;
        task_sleep_ms(period);
        if (!W.armed) continue;
        u64 f = irq_save();
        u16 s1 = tcohw_r16(&g_hw, TCO1_STS), s2 = tcohw_r16(&g_hw, TCO2_STS);
        bool second = (s2 & TCO2_STS_SECOND_TO) != 0, first = (s1 & TCO1_STS_TIMEOUT) != 0;
        if (second) tcohw_w16(&g_hw, TCO2_STS, TCO2_STS_SECOND_TO | TCO2_STS_BOOT);
        bool feeding = W.feeder == TCOWDT_KERNEL && !W.test && !W.shutdown;
        if (feeding) feed_locked();
        irq_restore(f);
        u64 now = uptime_ms();
        if (second) {
            bool first_time = !W.blocked;
            W.blocked = true;
            W.blocked_ms = now;
            if (first_time)
                klog("Momiji", "watchdog: the timer ran out twice and the machine did not restart: the reset is blocked (%s)", nr_text());
        } else if (first && !W.late_now) {
            W.late++;
            W.late_now = !feeding;                  /* (a kernel feed has just cleared it again) */
            if (!W.late_said || now - W.late_said > 30000) {
                W.late_said = now;
                if (feeding) klog("Momiji", "watchdog: the kernel fed it late: the first of its two countdowns had run out (the machine stalled for more than %u s)", W.seconds / 2);
                else klog("Momiji", "watchdog: nobody has fed it for %lu s (%s); the machine restarts within %u s", (unsigned long)((now - W.last_feed) / 1000),
                          W.feeder == TCOWDT_USER ? "user space is late" : W.test ? "a test" : W.shutdown ? "the shutdown takes long" : "/dev/watchdog was closed",
                          tcowdt_seconds_left());
            }
        }
    }
}

/* ---- /dev/watchdog ---- */

static void wd_opened(VNode *v, File *f) {
    (void)v;
    f->priv = NULL;                                 /* set when the last write carried the magic 'V' */
    Process *p = current ? current->proc : NULL;
    u64 fl = irq_save();
    bool first = ++W.opens == 1;
    bool takes = first && W.armed && !W.shutdown && !W.test;
    if (takes) { W.feeder = TCOWDT_USER; feed_locked(); }
    if (first) {
        W.user_pid = p ? p->pid : 0;
        strlcpy(W.user_name, p ? p->name : "?", sizeof W.user_name);
    }
    irq_restore(fl);
    if (takes) klog("Momiji", "'%s' (pid %d) feeds the watchdog from now on: if user space hangs, the machine restarts within %u s", W.user_name, W.user_pid, W.seconds);
}

static ssize_t wd_write(VNode *v, File *f, const void *buf, size_t n, u64 off) {
    (void)v; (void)off;
    bool magic = n && memchr(buf, 'V', n) != NULL;
    u64 fl = irq_save();
    f->priv = magic ? (void *)1 : NULL;
    if (W.armed && !W.shutdown && !W.test) {
        W.feeder = TCOWDT_USER;
        feed_locked();
    }
    irq_restore(fl);
    return (ssize_t)n;
}

static void wd_closed(VNode *v, File *f) {
    (void)v;
    bool magic = f->priv != NULL;
    u64 fl = irq_save();
    bool last = --W.opens <= 0;
    if (W.opens < 0) W.opens = 0;
    bool stop = last && magic && W.armed && !W.shutdown;
    bool orphan = last && !magic && W.armed && !W.shutdown && !W.test;
    if (stop) stop_locked();
    if (orphan) W.feeder = TCOWDT_NOBODY;
    irq_restore(fl);
    if (stop) klog("Momiji", "magic close: the watchdog is stopped (nothing restarts a hung machine until 'start' is written to /proc/watchdog)");
    if (orphan) klog("Momiji", "/dev/watchdog was closed without the magic 'V': the watchdog keeps running and restarts the machine in %u s unless it is fed again",
                     tcowdt_seconds_left());
}

static int wd_poll(VNode *v, File *f) { (void)v; (void)f; return POLLOUT; }

static const VNodeOps g_wd_ops = { .write = wd_write, .opened = wd_opened, .closed = wd_closed, .poll = wd_poll };

/* ---- the shutdown ---- */

static u32 shutdown_budget(void) { return MIN(MAX((u32)SHUTDOWN_BUDGET, W.seconds), TCOHW_TIMEOUT_MAX); }

void tcowdt_shutdown(void) {
    panicrec_shutdown();
    if (!W.found || !W.armed) return;
    u32 budget = shutdown_budget();
    u64 f = irq_save();
    W.shutdown = true;
    W.feeder = TCOWDT_NOBODY;
    arm_locked(budget);
    irq_restore(f);
    klog("Momiji", "shutting down: the watchdog gives the shutdown %u s and nobody feeds it any more", budget);
}

/* The panic path is about to restart the machine itself. The timer gets the shutdown's long budget first: where it outlives the reset (QEMU's
 * model does; a PC's platform reset stops it) a short timeout would otherwise run out during the firmware's next start and reset the machine
 * a second time, before the loader could set it again. Port I/O only: safe with interrupts off. */
void tcowdt_panic_restart(void) {
    if (!W.found || !W.armed) return;
    W.shutdown = true;
    W.feeder = TCOWDT_NOBODY;
    arm_locked(shutdown_budget());
}

/* ---- /proc/watchdog ---- */

static const char *feeder_name(void) {
    if (!W.armed) return "nobody (not armed)";
    switch (W.feeder) {
    case TCOWDT_KERNEL: return "the kernel (until user space opens /dev/watchdog)";
    case TCOWDT_USER:   return "user space (/dev/watchdog)";
    default:            return W.test ? "nobody: test (the machine must restart)" : W.shutdown ? "nobody: a shutdown is under way" : "nobody: /dev/watchdog was closed without the magic 'V'";
    }
}

static size_t gen_proc(char *b, size_t cap) {
    size_t n = 0;
#define P(...) do { if (n < cap) n += (size_t)snprintf(b + n, cap - n, __VA_ARGS__); } while (0)
    if (!W.found) {
        P("chipset:  LPC %04x:%04x\nwatchdog: none: %s\n", g_hw.lpc_vendor, g_hw.lpc_device, g_hw.why ? g_hw.why : "not looked for");
        return MIN(n, cap);
    }
    P("chipset:  %s, LPC %04x:%04x\n", g_hw.what, g_hw.lpc_vendor, g_hw.lpc_device);
    P("tco:      io %#x, ACPI block io %#x", g_hw.tcobase, g_hw.abase);
    if (g_hw.nr_phys) P(", NO_REBOOT: bit %d of the register at %#lx", __builtin_ctz(g_hw.nr_bit), (unsigned long)g_hw.nr_phys);
    P("\n");
    P("state:    %s", W.armed ? "armed" : W.off ? "off (wdt=off)" : "stopped");
    if (W.armed) P(", %u s, %u s left if nobody feeds it", W.seconds, tcowdt_seconds_left());
    P("\nfeeder:   %s", feeder_name());
    if (W.armed && W.feeder == TCOWDT_USER) P(": '%s' (pid %d)", W.user_name, W.user_pid);
    if (W.armed) P(", last fed %lu ms ago", (unsigned long)(uptime_ms() - W.last_feed));
    P("\nreset:    %s\n", W.blocked ? "BLOCKED: the timer ran out twice and the machine went on running" :
                          W.nr == 0 ? "possible (NO_REBOOT is clear)" : W.nr == 1 ? "blocked: NO_REBOOT stays set (a pin strap or the firmware)" : "unknown (NO_REBOOT cannot be read)");
    P("noreboot: %s when the kernel looked%s\n", W.nr_before == 1 ? "set" : W.nr_before == 0 ? "clear" : "unknown",
      W.keepnr ? ", left as it was (wdt.keepnr)" : W.nr_before == 1 && W.nr == 0 ? ", cleared by the kernel" : "");
    P("smi:      %s%s%s\n", W.smi ? "the first timeout raises an SMI (SMI_EN.TCO_EN)" : "no SMI on the first timeout", W.smi_cleared ? " (cleared: wdt.nosmi)" : "",
      W.locked ? "; TCO_LOCK is set" : "");
    P("late:     %u first timeouts noticed\n", W.late);
    const char *ls;
    switch (BOOT_WDT_STATE(W.loader_state)) {
    case BOOT_WDT_ARMED:    ls = "armed it before the kernel started"; break;
    case BOOT_WDT_ARMED_NR: ls = "armed it before the kernel started, but could not clear NO_REBOOT"; break;
    case BOOT_WDT_OFF:      ls = "did not arm it (wdt=off)"; break;
    case BOOT_WDT_NOFEED:   ls = "did not arm it (it found no feeder mark in the kernel)"; break;
    case BOOT_WDT_NOTFOUND: ls = "found no TCO watchdog"; break;
    default:                ls = W.loader_v5 ? "did not look at it" : "is older than the watchdog: the kernel armed it during its start"; break;
    }
    P("loader:   %s", ls);
    if (W.loader_seconds) P(" (%u s)", W.loader_seconds);
    if (W.loader_state & BOOT_WDT_NR_CLEARED) P("; it cleared NO_REBOOT");
    P("\nprevious: %s (TCO2_STS %#x at boot)\n", (W.boot_sts2 & TCO2_STS_SECOND_TO) ? "the previous start ended in a watchdog reset" : "no watchdog reset", W.boot_sts2);
#undef P
    return MIN(n, cap);
}

static ssize_t put_proc(const char *buf, size_t n) {
    char line[48];
    size_t l = MIN(n, sizeof line - 1);
    memcpy(line, buf, l);
    line[l] = 0;
    while (l && (line[l - 1] == '\n' || line[l - 1] == ' ')) line[--l] = 0;
    if (!W.found) return -ENODEV;
    char *arg = strchr(line, ' ');
    long val = 0;
    if (arg) { *arg++ = 0; while (*arg >= '0' && *arg <= '9') val = val * 10 + (*arg++ - '0'); }
    u64 f;
    if (!strcmp(line, "stop")) {
        f = irq_save(); stop_locked(); W.test = false; irq_restore(f);
        klog("Momiji", "the watchdog is stopped (written to /proc/watchdog)");
    } else if (!strcmp(line, "start") || !strcmp(line, "timeout")) {
        u32 s = val ? (u32)val : (W.seconds ? W.seconds : DEFAULT_SECONDS);
        if (s < 2 || s > 600) return -EINVAL;
        if (!strcmp(line, "timeout") && !W.armed) { W.seconds = s; return (ssize_t)n; }
        f = irq_save();
        W.test = false;
        W.shutdown = false;
        W.feeder = W.opens > 0 ? TCOWDT_USER : TCOWDT_KERNEL;
        arm_locked(s);
        irq_restore(f);
        klog("Momiji", "the watchdog is armed: %u s, fed by %s", s, W.feeder == TCOWDT_USER ? "user space" : "the kernel");
    } else if (!strcmp(line, "test")) {
        if (!W.armed) return -EINVAL;
        f = irq_save(); W.test = true; W.feeder = TCOWDT_NOBODY; irq_restore(f);
        klog("Momiji", "watchdog test: nobody feeds it from now on; the machine must restart within %u s (if it goes on running, that is reported)", tcowdt_seconds_left());
    } else {
        return -EINVAL;
    }
    return (ssize_t)n;
}

/* ---- start-up ---- */

static void tcowdt_init(void) {
    procfs_register_rw("watchdog", gen_proc, put_proc);
    char v[16];
    u32 secs = DEFAULT_SECONDS;
    if (cmdline_value("wdt", v, sizeof v)) {
        if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "no")) W.off = true;
        else {
            long s = cmdline_int("wdt", DEFAULT_SECONDS);
            secs = s >= 2 && s <= 600 ? (u32)s : DEFAULT_SECONDS;
        }
    }
    W.keepnr = cmdline_flag("wdt.keepnr");
    if (g_boot.version >= 5) {
        W.loader_v5 = true;
        W.loader_state = g_boot.wdt_state;
        W.loader_seconds = g_boot.wdt_seconds;
    }
    W.nr = W.nr_before = -1;
    if (tcohw_locate(&g_hw) == TCO_KIND_NONE) {
        klog("Momiji", "no Intel TCO watchdog (%s): nothing restarts a hung machine", g_hw.why);
        return;
    }
    if (g_hw.nr_phys) {
        u8 *page = ioremap(g_hw.nr_phys & ~0xFFFUL, 4096, PTE_PCD);
        g_hw.nr_reg = (volatile u32 *)(page + (g_hw.nr_phys & 0xFFF));
    }
    W.found = true;

    u16 s2 = tcohw_r16(&g_hw, TCO2_STS);
    W.boot_sts2 = s2;
    if (s2 & TCO2_STS_SECOND_TO) {
        klog("Momiji", "watchdog: the previous start ended in a watchdog reset (TCO2_STS %#x)", s2);
        tcohw_w16(&g_hw, TCO2_STS, s2 & (TCO2_STS_SECOND_TO | TCO2_STS_BOOT));
    }
    if (tcohw_r16(&g_hw, TCO1_STS) & TCO1_STS_TIMEOUT) tcohw_w16(&g_hw, TCO1_STS, TCO1_STS_TIMEOUT);
    klog("Momiji", "TCO watchdog: %s; LPC %04x:%04x, TCO at io %#x, ACPI block at io %#x", g_hw.what, g_hw.lpc_vendor, g_hw.lpc_device, g_hw.tcobase, g_hw.abase);

    if (g_hw.abase) {
        u32 smi = inl((u16)(g_hw.abase + ACPI_SMI_EN));
        W.smi = smi != 0xFFFFFFFFu && (smi & SMI_EN_TCO);
        W.locked = (tcohw_r16(&g_hw, TCO1_CNT) & TCO1_CNT_LOCK) != 0;
        if (W.smi && cmdline_flag("wdt.nosmi") && !W.locked) {
            outl((u16)(g_hw.abase + ACPI_SMI_EN), smi & ~SMI_EN_TCO);
            W.smi = (inl((u16)(g_hw.abase + ACPI_SMI_EN)) & SMI_EN_TCO) != 0;
            W.smi_cleared = !W.smi;
        }
    }
    W.nr_before = tcohw_noreboot(&g_hw);
    bool running = tcohw_running(&g_hw);
    if (W.off) {
        W.nr = W.nr_before;
        if (running) { u64 f = irq_save(); stop_locked(); irq_restore(f); }
        klog("Momiji", "wdt=off: the watchdog is not armed%s", running ? " (its counter was not halted; it is now)" : "");
        return;
    }
    W.nr = W.keepnr ? W.nr_before : tcohw_noreboot_clear(&g_hw);
    if (W.nr == 1) klog("Momiji", "NO_REBOOT is set and stays set (a pin strap, or the firmware locks it): the timer runs out but cannot restart the machine");
    else if (W.nr == -1) klog("Momiji", "NO_REBOOT: %s; whether a timeout restarts the machine is not known", g_hw.why ? g_hw.why : "the register cannot be read");
    else if (W.nr_before == 1) klog("Momiji", "NO_REBOOT was set: cleared, a timeout restarts the machine");
    if (W.smi) klog("Momiji", "the first timeout raises an SMI (SMI_EN.TCO_EN%s): if the firmware's handler swallows it the machine never restarts; wdt.nosmi clears it",
                    W.locked ? ", locked" : "");

    const char *how;
    switch (BOOT_WDT_STATE(W.loader_state)) {
    case BOOT_WDT_ARMED: case BOOT_WDT_ARMED_NR: how = running ? "the loader armed it, the kernel takes it over" : "the loader armed it, but it was not running"; break;
    case BOOT_WDT_NOFEED: how = "armed now (the loader found no feeder mark)"; break;
    default: how = W.loader_v5 ? "armed now" : "armed now (the loader is older than the watchdog: a hang before this point is not covered)"; break;
    }
    u64 f = irq_save();
    W.feeder = TCOWDT_KERNEL;
    arm_locked(secs);
    irq_restore(f);
    klog("Momiji", "the watchdog is armed: %u s, %s; the kernel feeds it until user space opens /dev/watchdog", secs, how);
    devfs_register("watchdog", S_IFCHR | 0600, &g_wd_ops, NULL, 0x0A82);         /* (Linux's misc device 10:130) */
    task_create_detached("Momiji", momiji_main, NULL);
}
INITCALL(tcowdt_init);
