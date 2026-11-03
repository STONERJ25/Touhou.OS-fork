/*
 * CPU temperature, frequency and package power: /proc/thermal (docs/SENSORS.md).
 *
 * What is read, all of it from Intel's architectural and model-specific registers and only ever READ:
 *   IA32_THERM_STATUS (0x19C)          digital sensor of the core this runs on: degrees below the throttling temperature
 *   MSR_TEMPERATURE_TARGET (0x1A2)     TjMax, the throttling temperature
 *   IA32_PACKAGE_THERM_STATUS (0x1B1)  the same for the package as a whole
 *   IA32_PERF_STATUS (0x198)           the multiplier the core runs at (times the 100 MHz reference clock on anything since Sandy Bridge)
 *   MSR_RAPL_POWER_UNIT / MSR_PKG_ENERGY_STATUS (0x606, 0x611)   the energy the package has used, from which the power follows
 * Only the boot processor runs this kernel, so "core 0 and the package" is what can be reported; the other cores' sensors are in registers
 * of cores that never run.
 *
 * Every read goes through read_msr(): CPUID says what to try, and msr_read_safe() (msrsafe.h) turns the #GP of a register that is not there
 * into "no". thermal.fake= replaces the CPU by a table, so that the decoding and the output are tested exactly on a machine (QEMU) that has
 * no sensor. The fan is NOT here: it is not in the CPU; see superio.c, which is never run at boot.
 */
#include "thermal.h"
#include "msrsafe.h"
#include "superio.h"
#include "kernel.h"
#include "cpu.h"
#include "cpuinfo.h"
#include "cmdline.h"
#include "ktime.h"
#include "irq.h"
#include "vfs.h"
#include "task.h"
#include "process.h"
#include "initcall.h"

#define TAG "Utsuho"

int procfs_register_rw(const char *name, size_t (*gen)(char *buf, size_t cap), ssize_t (*wr)(const char *buf, size_t n));

#define POWER_MIN_WINDOW_MS   500                   /* a shorter window is not a measurement: the last one keeps being shown */
#define POWER_MAX_WINDOW_MS   (20 * 60 * 1000)      /* the counter wraps in 43 minutes at 100 W; a longer silence could hide a wrap */

static struct {
    bool fake;
    bool forced;                                    /* thermal.force: read for real although CPUID offers no sensor (a test of the guard) */
    ThermalFake ft;
    bool dts, ptm, eist, tt, rapl;                  /* what this CPU has */
    const char *why_not;                            /* when it has no sensor */
    int  bus_khz, esu;
    u32  family, model, stepping;
    char vendor[13];
    /* the power window: the energy counter at the start of the last window long enough to count, and the numbers of the last complete one */
    bool base_ok, win_ok;
    u32  base_raw;
    u64  base_ms;
    u32  win_from, win_to;
    u64  win_ms, win_mw;
} g_th;

static bool read_msr(u32 msr, u64 *v) {
    if (g_th.fake) {
        u64 f = irq_save();
        bool ok = thermal_fake_read(&g_th.ft, msr, v);
        irq_restore(f);
        return ok;
    }
    return msr_read_safe(msr, v) == 0;
}

static bool fake_has(u32 msr) {
    for (int i = 0; i < g_th.ft.n; i++) if (g_th.ft.e[i].msr == msr) return true;
    return false;
}

/* ------------------------------------------------------------------ one look at the registers */

typedef struct {
    bool core_ok, pkg_ok, tj_ok, ratio_ok, energy_ok, unit_ok;
    u64  core, pkg, target, perf, energy, unit;
} Sample;

static void sample(Sample *s) {
    memset(s, 0, sizeof *s);
    if (!g_th.dts) return;
    s->core_ok = read_msr(MSR_IA32_THERM_STATUS, &s->core);
    if (g_th.ptm) s->pkg_ok = read_msr(MSR_IA32_PKG_THERM_STATUS, &s->pkg);
    if (g_th.tt) s->tj_ok = read_msr(MSR_TEMPERATURE_TARGET, &s->target);
    if (g_th.eist) s->ratio_ok = read_msr(MSR_IA32_PERF_STATUS, &s->perf);
    if (g_th.rapl) {
        s->unit_ok = read_msr(MSR_RAPL_POWER_UNIT, &s->unit);
        s->energy_ok = read_msr(MSR_PKG_ENERGY_STATUS, &s->energy);
    }
}

/* Folds a reading of the energy counter into the power window. Called with the sample of every look. */
static void power_update(const Sample *s) {
    if (!g_th.rapl || !s->energy_ok) return;
    u64 now = uptime_ms();
    u32 raw = (u32)s->energy;
    u64 f = irq_save();
    if (!g_th.base_ok) {
        g_th.base_ok = true;
        g_th.base_raw = raw;
        g_th.base_ms = now;
    } else if (now - g_th.base_ms >= POWER_MIN_WINDOW_MS) {
        u64 dt = now - g_th.base_ms;
        if (dt <= POWER_MAX_WINDOW_MS) {
            g_th.win_ok = true;
            g_th.win_from = g_th.base_raw;
            g_th.win_to = raw;
            g_th.win_ms = dt;
            g_th.win_mw = thermal_power_mw(g_th.base_raw, raw, g_th.esu, dt);
        } else {
            g_th.win_ok = false;                    /* too long ago to know how often the counter wrapped */
        }
        g_th.base_raw = raw;
        g_th.base_ms = now;
    }
    irq_restore(f);
}

/* ------------------------------------------------------------------ /proc/thermal */

typedef struct { char *b; size_t cap, n; } Out;

static void outf(Out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void outf(Out *o, const char *fmt, ...) {
    if (o->n >= o->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(o->b + o->n, o->cap - o->n, fmt, ap);
    va_end(ap);
    if (k > 0) o->n += MIN((size_t)k, o->cap - o->n - 1);
}

/* The limit the readings are relative to: MSR_TEMPERATURE_TARGET when it answers with something that can be a throttling temperature, else 100 C
 * (a register that reads 0, as a hypervisor may answer for one it does not model, is not a limit). *src says which. */
static int tjmax_of(const Sample *s, int *offset, const char **src) {
    *offset = 0;
    *src = "assumed (msr 0x1a2 did not answer)";
    if (!s->tj_ok) return 100;
    int t = thermal_decode_tjmax(s->target, offset);
    if (t < 60 || t > 130) {
        *offset = 0;
        *src = "assumed (msr 0x1a2 read an implausible value)";
        return 100;
    }
    *src = "msr 0x1a2";
    return t;
}

static void out_status(Out *o, const char *who, u64 msr, int tjmax) {
    ThermalStatus st;
    thermal_decode_status(msr, &st);
    char flags[96];
    outf(o, "%s.valid: %s\n", who, st.valid ? "yes" : "no");
    if (st.valid) {
        outf(o, "%s.temp: %d\n", who, tjmax - st.readout);
        outf(o, "%s.margin: %d\n", who, st.readout);
    }
    outf(o, "%s.resolution: %d\n", who, st.resolution);
    thermal_flag_names(st.status, flags, sizeof flags);
    outf(o, "%s.status: %s\n", who, flags);
    thermal_flag_names(st.log, flags, sizeof flags);
    outf(o, "%s.log: %s\n", who, flags);
}

static size_t gen_thermal(char *b, size_t cap) {
    Out o = { b, cap, 0 };
    if (!g_th.dts) {
        outf(&o, "thermal: no thermal sensor\nreason: %s\nvendor: %s\n", g_th.why_not, g_cpu.vendor);
        return o.n;
    }
    Sample s;
    sample(&s);
    power_update(&s);
    int offset = 0;
    const char *tj_src;
    int tjmax = tjmax_of(&s, &offset, &tj_src);
    outf(&o, "thermal: digital thermal sensor\n");
    outf(&o, "source: %s\n", g_th.fake ? "synthetic (thermal.fake= on the kernel command line)" : g_th.forced ? "cpu, forced (thermal.force on the kernel command line)" : "cpu");
    outf(&o, "vendor: %s\n", g_th.vendor);
    outf(&o, "cpu: family %u model %#x stepping %u\n", g_th.family, g_th.model, g_th.stepping);
    outf(&o, "cores: core 0 and the package (this kernel runs one core; the others' sensors cannot be read)\n");
    outf(&o, "tjmax: %d\n", tjmax);
    outf(&o, "tjmax_source: %s\n", tj_src);
    outf(&o, "tjmax_offset: %d\n", offset);
    if (s.core_ok) out_status(&o, "core0", s.core, tjmax);
    else outf(&o, "core0.error: msr 0x19c did not answer\n");
    if (g_th.ptm) {
        if (s.pkg_ok) out_status(&o, "package", s.pkg, tjmax);
        else outf(&o, "package.error: msr 0x1b1 did not answer\n");
    }
    if (g_th.eist && s.ratio_ok) {
        int ratio = (int)((s.perf >> 8) & 0xFF);
        outf(&o, "ratio: %d\n", ratio);
        if (g_th.bus_khz) {
            outf(&o, "bus_khz: %d\n", g_th.bus_khz);
            outf(&o, "mhz: %d\n", ratio * g_th.bus_khz / 1000);
        }
    }
    if (g_th.rapl && s.energy_ok) {
        outf(&o, "energy_unit: 1/2^%d J\n", g_th.esu);
        outf(&o, "energy_raw: 0x%08x\n", (u32)s.energy);
        outf(&o, "energy_uj: %lu\n", (unsigned long)thermal_energy_uj((u32)s.energy, g_th.esu));
        u64 f = irq_save();
        bool ok = g_th.win_ok;
        u32 from = g_th.win_from, to = g_th.win_to;
        u64 ms = g_th.win_ms, mw = g_th.win_mw;
        irq_restore(f);
        if (ok) {
            outf(&o, "power.window_ms: %lu\n", (unsigned long)ms);
            outf(&o, "power.window_from_raw: 0x%08x\n", from);
            outf(&o, "power.window_to_raw: 0x%08x\n", to);
            outf(&o, "power.mw: %lu\n", (unsigned long)mw);
        } else {
            outf(&o, "power.mw: -\n");
        }
    }
    if (s.core_ok) outf(&o, "msr.0x19c: 0x%016lx\n", (unsigned long)s.core);
    if (s.pkg_ok) outf(&o, "msr.0x1b1: 0x%016lx\n", (unsigned long)s.pkg);
    if (s.tj_ok) outf(&o, "msr.0x1a2: 0x%016lx\n", (unsigned long)s.target);
    if (s.ratio_ok) outf(&o, "msr.0x198: 0x%016lx\n", (unsigned long)s.perf);
    if (s.unit_ok) outf(&o, "msr.0x606: 0x%016lx\n", (unsigned long)s.unit);
    if (s.energy_ok) outf(&o, "msr.0x611: 0x%016lx\n", (unsigned long)s.energy);
    return o.n;
}

/* ------------------------------------------------------------------ /proc/superio: the fan's chip, only when somebody asks */

static SioResult g_sio[2];
static int  g_sio_answered;
static bool g_sio_done;

static void sio_out(void *ctx, u16 port, u8 v) { (void)ctx; outb(port, v); }
static u8   sio_in(void *ctx, u16 port) { (void)ctx; return inb(port); }

static size_t gen_superio(char *b, size_t cap) {
    if (!g_sio_done)
        return (size_t)snprintf(b, cap, "superio: not probed\nhint: root can look for the chip that has the fan with `sensors --probe-lpc`; it writes key bytes to the "
                                        "Super I/O configuration ports 0x2e and 0x4e (docs/SENSORS.md says exactly which)\n");
    u64 f = irq_save();
    SioResult copy[2] = { g_sio[0], g_sio[1] };
    int answered = g_sio_answered;
    irq_restore(f);
    return superio_format(copy, answered, b, cap);
}

/* "probe" (root): runs the probe of superio.c once and keeps its report. Never at boot, never by itself (docs/SENSORS.md). With interrupts off for the
 * few hundred microseconds it takes, so that no interrupt handler can put an access of its own between an index write and its data read. */
static ssize_t put_superio(const char *buf, size_t n) {
    char word[16];
    size_t l = 0;
    while (l < n && l < sizeof word - 1 && buf[l] != '\n' && buf[l] != ' ') { word[l] = buf[l]; l++; }
    word[l] = 0;
    if (strcmp(word, "probe")) return -EINVAL;
    SioPorts io = { sio_out, sio_in, NULL };
    SioResult r[2];
    u64 f = irq_save();
    int answered = superio_probe(&io, r);
    g_sio[0] = r[0];
    g_sio[1] = r[1];
    g_sio_answered = answered;
    g_sio_done = true;
    irq_restore(f);
    klog(TAG, "superio: probed by '%s': %s", current->proc ? current->proc->name : "the kernel", answered ? "a chip answered" : "nothing answered");
    for (int i = 0; i < 2; i++)
        if (r[i].family != SIO_NONE)
            klog(TAG, "superio: port %#x: %s id %#06x %s%s", r[i].port, superio_family_name(r[i].family), r[i].id, r[i].chip[0] ? r[i].chip : "(not in the table)",
                 r[i].hwm_known ? (r[i].hwm_active ? ", monitor active" : ", monitor not active") : "");
    return (ssize_t)n;
}

/* ------------------------------------------------------------------ setup */

static void thermal_init(void) {
    strlcpy(g_th.vendor, g_cpu.vendor, sizeof g_th.vendor);
    g_th.family = g_cpu.family;
    g_th.model = g_cpu.model;
    g_th.stepping = g_cpu.stepping;

    char spec[200];
    if (cmdline_value("thermal", spec, sizeof spec) && !strcmp(spec, "off")) {      /* the escape hatch for a machine that does not like it: no register is read, the IDT is not touched */
        g_th.why_not = "thermal=off on the kernel command line";
        procfs_register("thermal", gen_thermal);
        procfs_register_rw("superio", gen_superio, put_superio);
        klog(TAG, "thermal: off (thermal=off); no register is read");
        return;
    }
    msrsafe_init();
    bool force = cmdline_flag("thermal.force");
    if (cmdline_value("thermal.fake", spec, sizeof spec) && spec[0]) {
        if (thermal_fake_parse(spec, &g_th.ft)) {
            g_th.fake = true;
            strlcpy(g_th.vendor, "GenuineIntel", sizeof g_th.vendor);       /* the table stands in for a Kaby Lake desktop part */
            g_th.family = 6;
            g_th.model = 0x9E;
            g_th.stepping = 9;
        } else {
            klog(TAG, "thermal.fake=%s is not understood (hexadecimal MSR:VALUE[+STEP],...): ignored", spec);
        }
    }

    if (g_th.fake) {
        g_th.dts = fake_has(MSR_IA32_THERM_STATUS);
        g_th.ptm = fake_has(MSR_IA32_PKG_THERM_STATUS);
        g_th.eist = fake_has(MSR_IA32_PERF_STATUS);
        g_th.tt = fake_has(MSR_TEMPERATURE_TARGET);
        g_th.rapl = fake_has(MSR_RAPL_POWER_UNIT) && fake_has(MSR_PKG_ENERGY_STATUS);
        if (!g_th.dts) g_th.why_not = "the synthetic table has no register 0x19c";
    } else if (force) {
        /* The test of the real read path on a machine (QEMU) whose CPUID says there is nothing: pretend it said yes to everything and read the registers
         * for real, through the guard. Whatever the emulator answers -- zeros, or a #GP -- must end as a line of text, never as a panic. */
        g_th.dts = g_th.ptm = g_th.eist = true;
        g_th.forced = true;
    } else if (strcmp(g_th.vendor, "GenuineIntel")) {
        g_th.why_not = "this is not an Intel CPU (AMD reports its temperature through another interface, which is not supported)";
    } else if (g_cpu.max_leaf < 6) {
        g_th.why_not = "the CPU has no CPUID leaf 6";
    } else {
        u32 a, b, c, d;
        cpuid(6, 0, &a, &b, &c, &d);
        g_th.dts = a & 1;
        g_th.ptm = (a >> 6) & 1;
        g_th.eist = (g_cpu.feat_ecx >> 7) & 1;
        if (!g_th.dts) g_th.why_not = "the CPU does not report a digital thermal sensor (CPUID leaf 6)";
    }

    if (g_th.dts) {
        u64 v;
        if (!g_th.fake) {
            g_th.tt = read_msr(MSR_TEMPERATURE_TARGET, &v);
            g_th.rapl = (force || (g_th.family == 6 && thermal_bus_khz(g_th.vendor, g_th.family, g_th.model) != 0)) &&
                        read_msr(MSR_RAPL_POWER_UNIT, &v) && read_msr(MSR_PKG_ENERGY_STATUS, &v);
        }
        g_th.bus_khz = thermal_bus_khz(g_th.vendor, g_th.family, g_th.model);
        if (g_th.rapl) {
            u64 unit = 0;
            if (!read_msr(MSR_RAPL_POWER_UNIT, &unit) || !thermal_rapl_units(unit, &g_th.esu)) g_th.rapl = false;
        }
    }

    procfs_register("thermal", gen_thermal);
    procfs_register_rw("superio", gen_superio, put_superio);

    if (!g_th.dts) {
        klog(TAG, "thermal: no thermal sensor (%s)", g_th.why_not);
    } else {
        Sample s;
        sample(&s);
        power_update(&s);                                               /* the first point of the power window: the boot */
        int tj_offset;
        const char *tj_src;
        int tjmax = tjmax_of(&s, &tj_offset, &tj_src);
        ThermalStatus core, pkg;
        thermal_decode_status(s.core, &core);
        thermal_decode_status(s.pkg, &pkg);
        char t[64] = "", p[64] = "", f[40] = "";
        if (s.core_ok && core.valid) snprintf(t, sizeof t, "core 0 at %d C", tjmax - core.readout);
        else snprintf(t, sizeof t, "core 0 reading not valid");
        if (s.pkg_ok && pkg.valid) snprintf(p, sizeof p, ", package at %d C", tjmax - pkg.readout);
        if (s.ratio_ok && g_th.bus_khz) snprintf(f, sizeof f, ", %d MHz", (int)((s.perf >> 8) & 0xFF) * g_th.bus_khz / 1000);
        klog(TAG, "thermal: %s%s (TjMax %d C)%s%s%s%s", t, p, tjmax, f, g_th.rapl ? ", energy counter present" : "", g_th.fake ? " [synthetic: thermal.fake=]" : "",
             force && !g_th.fake ? " [forced: thermal.force]" : "");
    }

    if (cmdline_flag("msrsafe.test")) {                                 /* the test of the guarded read itself: see tests/case_thermal.py */
        u64 viaguard = 0, direct = rdmsr(MSR_EFER);
        int r = msr_read_safe(MSR_EFER, &viaguard);
        klog(TAG, "msrsafe: ready %s; EFER by the guarded read %s%#lx (direct read %#lx)", msrsafe_ready() ? "yes" : "no", r == 0 ? "" : "failed ", (unsigned long)viaguard, (unsigned long)direct);
        klog(TAG, "msrsafe: a deliberate #GP %s", msrsafe_selftest() == 0 ? "was caught" : "was NOT caught");
    }
    if (cmdline_flag("msrsafe.crash")) {                                /* a #GP that is NOT in the table must still reach the kernel's own handler (and panic): tests/case_thermal.py */
        klog(TAG, "msrsafe: now a #GP that is not in the table; the kernel is expected to panic");
        __asm__ volatile ("movw $0x100, %%ax; movw %%ax, %%ds" : : : "ax");
    }
}
INITCALL(thermal_init);
