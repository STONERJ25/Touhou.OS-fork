/*
 * hwreport -- one plain-text report of the hardware this system runs on, for the people who want TouhouOS to run on more machines
 * (docs/HARDWARE_BREADTH.md, docs/HARDWARE_DB.md). Stable section headers ([system], [cpu], ...) and "key: value" lines, so that a script
 * (tools/hwreport.py) can read many reports and tell which devices have no driver.
 *
 * Where the facts come from: /proc (dmi, hwinfo, pcicaps, pci, acpi, ahci, nvme, partitions, usb, audio, thermal, ...), the kernel log,
 * the network control call, and the CPUID instruction (which user programs may execute). Nothing is written to any device; missing
 * information is always a line that says so ("acpi: not available"), never an empty section and never a crash.
 *
 * Privacy: a report gets pasted into public places. By default it cuts the last three bytes off every MAC address, replaces every serial
 * number (system, board, chassis, memory modules, disks), every UUID and a host name that is not the default with a placeholder, in the
 * fields that carry them and in every free-text line (the kernel log) by looking for the same strings. `hwreport --full` keeps them.
 *
 * CPUID leaves and bits follow the Intel SDM vol. 2 and the AMD APM vol. 3; the PCI class names follow the PCI-SIG class code list.
 */
#include "kappabox.h"
#include "netutil.h"
#include <stdarg.h>
#include <stdint.h>

#define HW_FORMAT 1

/* ------------------------------------------------------------------ output and privacy */

static FILE *g_out;
static int   g_full;                    /* --full: keep serial numbers, UUIDs, MAC addresses and the host name */
static int   g_lines;
static char  g_host[64];
static char  g_secret[48][72];          /* strings that must not appear in a default report */
static int   g_nsecret;

static const char *const PLACEHOLDERS[] = {
    "none", "n/a", "na", "unknown", "default string", "to be filled by o.e.m.", "not specified", "not applicable", "not available",
    "system serial number", "chassis serial number", "serial number", "base board serial number", "0123456789", "0123456789abcdef",
    "0000000000", "to be filled by oem", "empty", "default", "123456789", "1234567890", "xxxxxxxx", "0", "o.e.m.", "oem", "(null)", NULL,
};

static int is_placeholder(const char *s) {
    for (int i = 0; PLACEHOLDERS[i]; i++) if (!strcasecmp(s, PLACEHOLDERS[i])) return 1;
    int same = 1;                                           /* "00000000", "FFFFFFFF", "........" */
    for (const char *p = s; *p; p++) if (*p != s[0]) same = 0;
    return same;
}

/* A string the default report must never print (a serial number, a UUID, a host name). Short and placeholder values are not secrets. */
static void add_secret(const char *s) {
    char t[72];
    if (g_full || !s) return;
    while (*s == ' ' || *s == '"') s++;
    strlcpy(t, s, sizeof t);
    size_t n = strlen(t);
    while (n && (t[n - 1] == ' ' || t[n - 1] == '"' || t[n - 1] == '\n')) t[--n] = 0;
    if (n < 4 || is_placeholder(t) || g_nsecret >= (int)(sizeof g_secret / sizeof g_secret[0])) return;
    for (int i = 0; i < g_nsecret; i++) if (!strcmp(g_secret[i], t)) return;
    strlcpy(g_secret[g_nsecret++], t, sizeof g_secret[0]);
}

static int alnum(int c) { return isalnum((unsigned char)c); }
static int hexd(int c) { return isxdigit((unsigned char)c); }

/* hh:hh:hh:hh:hh:hh (or with '-') at s; returns 17 or 0 */
static int mac_at(const char *s) {
    char sep = s[2];
    if (sep != ':' && sep != '-') return 0;
    for (int i = 0; i < 6; i++) {
        if (!hexd(s[i * 3]) || !hexd(s[i * 3 + 1])) return 0;
        if (i < 5 && s[i * 3 + 2] != sep) return 0;
    }
    return 17;
}

/* 8-4-4-4-12 hex digits at s; returns 36 or 0 */
static int uuid_at(const char *s) {
    static const int dash[] = { 8, 13, 18, 23 };
    for (int i = 0; i < 36; i++) {
        int is_dash = i == dash[0] || i == dash[1] || i == dash[2] || i == dash[3];
        if (is_dash ? s[i] != '-' : !hexd(s[i])) return 0;
    }
    return 36;
}

/* The default report's last line of defence, run over every line: MAC addresses lose their last three bytes, UUIDs and every known
 * secret (bounded by characters that cannot be part of a serial number) become placeholders. */
static void redact_line(char *line, size_t cap) {
    if (g_full) return;
    char out[1100];
    size_t o = 0;
    size_t len = strlen(line);
    for (size_t i = 0; i < len && o + 40 < sizeof out;) {
        int boundary = i == 0 || !alnum(line[i - 1]);
        int n;
        if (boundary && (n = mac_at(line + i)) && !alnum(line[i + n])) {
            memcpy(out + o, line + i, 8);
            memcpy(out + o + 8, ":xx:xx:xx", 9);
            o += 17;
            i += (size_t)n;
            continue;
        }
        if (boundary && (n = uuid_at(line + i)) && !alnum(line[i + n])) {
            memcpy(out + o, "<uuid>", 6);
            o += 6;
            i += (size_t)n;
            continue;
        }
        int hit = 0;
        if (boundary) {
            for (int k = 0; k < g_nsecret && !hit; k++) {
                size_t sl = strlen(g_secret[k]);
                if (!strncmp(line + i, g_secret[k], sl) && !alnum(line[i + sl])) {
                    memcpy(out + o, "<redacted>", 10);
                    o += 10;
                    i += sl;
                    hit = 1;
                }
            }
        }
        if (!hit) out[o++] = line[i++];
    }
    out[o] = 0;
    strlcpy(line, out, cap);
}

static void emit(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void emit(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    redact_line(line, sizeof line);
    fputs(line, g_out);
    fputc('\n', g_out);
    g_lines++;
}

static void section(const char *name) { fprintf(g_out, "\n[%s]\n", name); g_lines += 2; }

/* A field that is private in itself: the key says what it is, the value is replaced unless --full. */
static void emit_private(const char *key, const char *value, const char *placeholder) {
    if (!value[0]) return;
    if (g_full) emit("%s: %s", key, value);
    else emit("%s: %s", key, is_placeholder(value) ? value : placeholder);
}

/* ------------------------------------------------------------------ reading /proc */

static char *slurp(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    size_t n = 0;
    char *t = kb_read_all(fd, &n);
    close(fd);
    return t;
}

/* The text of one line is cut at the newline in place; lines[] points into text. */
static char **split(char *text, size_t *count) {
    size_t cap = 64, n = 0;
    char **v = malloc(cap * sizeof *v);
    if (!v) kb_die("out of memory");
    for (char *p = text; *p;) {
        if (n == cap) { cap *= 2; v = realloc(v, cap * sizeof *v); if (!v) kb_die("out of memory"); }
        v[n++] = p;
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = 0;
        p = nl + 1;
    }
    *count = n;
    return v;
}

/* The value of "key: value" (or "key value") on a line of a loaded file; NULL if there is no such line. */
static const char *find_key(char **lines, size_t n, const char *key) {
    size_t kl = strlen(key);
    for (size_t i = 0; i < n; i++) {
        if (strncmp(lines[i], key, kl)) continue;
        const char *v = lines[i] + kl;
        if (*v == ':') v++;
        else if (*v != ' ') continue;
        while (*v == ' ') v++;
        return v;
    }
    return NULL;
}

/* One line of a /proc file as a report line: the kernel pads its columns ("poweroff:    S5", " 4: 38", "pin 0x03:  line out"), the report has
 * the key (spaces become dots) and one space before the value. */
static void emit_kv(const char *prefix, const char *line) {
    while (*line == ' ') line++;
    const char *c = strchr(line, ':');
    if (!c) { emit("%s%s", prefix, line); return; }
    char key[80];
    size_t kl = (size_t)(c - line);
    if (kl >= sizeof key) kl = sizeof key - 1;
    memcpy(key, line, kl);
    key[kl] = 0;
    for (char *p = key; *p; p++) if (*p == ' ') *p = '.';
    const char *v = c + 1;
    while (*v == ' ') v++;
    emit("%s%s: %s", prefix, key, v);
}

/* Prints every line of a /proc file whose key starts with one of the prefixes (NULL-terminated list), or all if prefixes is NULL. Returns the count. */
static int pass_through(char **lines, size_t n, const char *const *prefixes, const char *prefix_out) {
    int count = 0;
    for (size_t i = 0; i < n; i++) {
        if (!lines[i][0]) continue;
        int ok = !prefixes;
        for (int k = 0; prefixes && prefixes[k]; k++) if (!strncmp(lines[i], prefixes[k], strlen(prefixes[k]))) ok = 1;
        if (!ok) continue;
        emit_kv(prefix_out ? prefix_out : "", lines[i]);
        count++;
    }
    return count;
}

static int starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }

/* ------------------------------------------------------------------ the kernel log, loaded once */

static char  *g_dmesg;
static char **g_dlines;
static size_t g_ndlines;

static void load_log(void) {
    int fd = open("/dev/kmsg", O_RDONLY);
    if (fd < 0) return;
    size_t n;
    g_dmesg = kb_read_all(fd, &n);
    close(fd);
    g_dlines = split(g_dmesg, &g_ndlines);
}

static int log_has(const char *s) {
    for (size_t i = 0; i < g_ndlines; i++) if (strstr(g_dlines[i], s)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ CPUID */

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    __asm__ volatile ("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

typedef struct { char vendor; unsigned fam; unsigned lo, hi; const char *name; } Arch;      /* vendor: 'I' Intel, 'A' AMD */

static const Arch ARCHES[] = {
    { 'I', 5, 0x00, 0xFF, "P5 Pentium" }, { 'I', 15, 0x00, 0xFF, "NetBurst (Pentium 4)" },
    { 'I', 6, 0x0E, 0x0E, "Yonah (Core Duo)" }, { 'I', 6, 0x0F, 0x0F, "Core 2 (Merom/Conroe)" }, { 'I', 6, 0x16, 0x16, "Core 2 (Merom-L)" },
    { 'I', 6, 0x17, 0x17, "Core 2 (Penryn)" }, { 'I', 6, 0x1D, 0x1D, "Core 2 (Dunnington)" },
    { 'I', 6, 0x1A, 0x1A, "Nehalem" }, { 'I', 6, 0x1E, 0x1F, "Nehalem" }, { 'I', 6, 0x2E, 0x2E, "Nehalem" },
    { 'I', 6, 0x25, 0x25, "Westmere" }, { 'I', 6, 0x2C, 0x2C, "Westmere" }, { 'I', 6, 0x2F, 0x2F, "Westmere" },
    { 'I', 6, 0x2A, 0x2A, "Sandy Bridge" }, { 'I', 6, 0x2D, 0x2D, "Sandy Bridge" },
    { 'I', 6, 0x3A, 0x3A, "Ivy Bridge" }, { 'I', 6, 0x3E, 0x3E, "Ivy Bridge" },
    { 'I', 6, 0x3C, 0x3C, "Haswell" }, { 'I', 6, 0x3F, 0x3F, "Haswell" }, { 'I', 6, 0x45, 0x46, "Haswell" },
    { 'I', 6, 0x3D, 0x3D, "Broadwell" }, { 'I', 6, 0x47, 0x47, "Broadwell" }, { 'I', 6, 0x4F, 0x4F, "Broadwell" }, { 'I', 6, 0x56, 0x56, "Broadwell" },
    { 'I', 6, 0x4E, 0x4E, "Skylake" }, { 'I', 6, 0x5E, 0x5E, "Skylake" }, { 'I', 6, 0x55, 0x55, "Skylake-SP / Cascade Lake" },
    { 'I', 6, 0x8E, 0x8E, "Kaby Lake / Coffee Lake (mobile)" }, { 'I', 6, 0x9E, 0x9E, "Kaby Lake / Coffee Lake (desktop)" },
    { 'I', 6, 0xA5, 0xA6, "Comet Lake" }, { 'I', 6, 0x66, 0x66, "Cannon Lake" },
    { 'I', 6, 0x7D, 0x7E, "Ice Lake" }, { 'I', 6, 0x6A, 0x6A, "Ice Lake-SP" }, { 'I', 6, 0x6C, 0x6C, "Ice Lake-D" },
    { 'I', 6, 0x8C, 0x8D, "Tiger Lake" }, { 'I', 6, 0xA7, 0xA7, "Rocket Lake" }, { 'I', 6, 0x8F, 0x8F, "Sapphire Rapids" }, { 'I', 6, 0xCF, 0xCF, "Emerald Rapids" },
    { 'I', 6, 0x97, 0x97, "Alder Lake" }, { 'I', 6, 0x9A, 0x9A, "Alder Lake" }, { 'I', 6, 0xBE, 0xBE, "Alder Lake-N" },
    { 'I', 6, 0xB7, 0xB7, "Raptor Lake" }, { 'I', 6, 0xBA, 0xBA, "Raptor Lake" }, { 'I', 6, 0xBF, 0xBF, "Raptor Lake" },
    { 'I', 6, 0xAA, 0xAA, "Meteor Lake" }, { 'I', 6, 0xAC, 0xAC, "Meteor Lake" }, { 'I', 6, 0xBD, 0xBD, "Lunar Lake" },
    { 'I', 6, 0xC5, 0xC6, "Arrow Lake" },
    { 'I', 6, 0x1C, 0x1C, "Bonnell (Atom)" }, { 'I', 6, 0x26, 0x26, "Bonnell (Atom)" }, { 'I', 6, 0x27, 0x27, "Saltwell (Atom)" },
    { 'I', 6, 0x35, 0x36, "Saltwell (Atom)" }, { 'I', 6, 0x37, 0x37, "Silvermont (Atom)" }, { 'I', 6, 0x4A, 0x4A, "Silvermont (Atom)" },
    { 'I', 6, 0x4D, 0x4D, "Silvermont (Atom)" }, { 'I', 6, 0x5A, 0x5A, "Silvermont (Atom)" }, { 'I', 6, 0x5D, 0x5D, "Silvermont (Atom)" },
    { 'I', 6, 0x4C, 0x4C, "Airmont (Atom)" }, { 'I', 6, 0x5C, 0x5C, "Goldmont (Atom)" }, { 'I', 6, 0x5F, 0x5F, "Goldmont (Atom)" },
    { 'I', 6, 0x7A, 0x7A, "Goldmont Plus (Atom)" }, { 'I', 6, 0x86, 0x86, "Tremont (Atom)" }, { 'I', 6, 0x96, 0x96, "Tremont (Atom)" },
    { 'I', 6, 0x9C, 0x9C, "Tremont (Atom)" }, { 'I', 6, 0x57, 0x57, "Knights Landing" }, { 'I', 6, 0x85, 0x85, "Knights Mill" },
    { 'A', 0x0F, 0x00, 0xFF, "K8" }, { 'A', 0x10, 0x00, 0xFF, "K10" }, { 'A', 0x11, 0x00, 0xFF, "K8/K10 hybrid (Turion X2)" }, { 'A', 0x12, 0x00, 0xFF, "Llano" },
    { 'A', 0x14, 0x00, 0xFF, "Bobcat" }, { 'A', 0x15, 0x00, 0x0F, "Bulldozer" }, { 'A', 0x15, 0x10, 0x2F, "Piledriver" },
    { 'A', 0x15, 0x30, 0x3F, "Steamroller" }, { 'A', 0x15, 0x60, 0x7F, "Excavator" }, { 'A', 0x16, 0x00, 0x2F, "Jaguar" }, { 'A', 0x16, 0x30, 0x3F, "Puma" },
    { 'A', 0x17, 0x01, 0x01, "Zen" }, { 'A', 0x17, 0x08, 0x08, "Zen+" }, { 'A', 0x17, 0x11, 0x11, "Zen" }, { 'A', 0x17, 0x18, 0x18, "Zen+" },
    { 'A', 0x17, 0x20, 0x20, "Zen" }, { 'A', 0x17, 0x31, 0x31, "Zen 2" }, { 'A', 0x17, 0x60, 0x6F, "Zen 2" }, { 'A', 0x17, 0x70, 0x7F, "Zen 2" },
    { 'A', 0x17, 0x90, 0xAF, "Zen 2" },
    { 'A', 0x19, 0x00, 0x0F, "Zen 3" }, { 'A', 0x19, 0x10, 0x1F, "Zen 4" }, { 'A', 0x19, 0x20, 0x2F, "Zen 3" }, { 'A', 0x19, 0x30, 0x3F, "Zen 3" },
    { 'A', 0x19, 0x40, 0x4F, "Zen 3+" }, { 'A', 0x19, 0x50, 0x5F, "Zen 3" }, { 'A', 0x19, 0x60, 0x7F, "Zen 4" }, { 'A', 0x19, 0xA0, 0xAF, "Zen 4c" },
    { 'A', 0x1A, 0x00, 0xFF, "Zen 5" },
};

static const char *microarch(const char *vendor, unsigned fam, unsigned model) {
    char v = !strcmp(vendor, "GenuineIntel") ? 'I' : !strcmp(vendor, "AuthenticAMD") ? 'A' : 0;
    if (!v) return "unknown";
    for (unsigned i = 0; i < sizeof ARCHES / sizeof ARCHES[0]; i++)
        if (ARCHES[i].vendor == v && ARCHES[i].fam == fam && model >= ARCHES[i].lo && model <= ARCHES[i].hi) return ARCHES[i].name;
    return "unknown";
}

static void add_flag(char *out, size_t cap, uint32_t reg, int bit, const char *name) {
    if (reg & (1u << bit)) { if (out[0]) strlcat(out, " ", cap); strlcat(out, name, cap); }
}

static unsigned g_cpu_fam, g_cpu_model;
static int g_cpu_hypervisor;
static char g_cpu_vendor[13];

static void report_cpu(char **dmi, size_t ndmi) {
    uint32_t r[4], r1[4], r7[4] = { 0, 0, 0, 0 }, e1[4] = { 0, 0, 0, 0 }, e7[4] = { 0, 0, 0, 0 };
    char brand[49] = "";
    section("cpu");
    cpuid(0, 0, r);
    uint32_t maxleaf = r[0];
    memcpy(g_cpu_vendor, &r[1], 4); memcpy(g_cpu_vendor + 4, &r[3], 4); memcpy(g_cpu_vendor + 8, &r[2], 4);
    g_cpu_vendor[12] = 0;
    cpuid(0x80000000u, 0, r);
    uint32_t maxext = r[0];
    if (maxext >= 0x80000004u) {
        for (int i = 0; i < 3; i++) { cpuid(0x80000002u + (uint32_t)i, 0, r); memcpy(brand + i * 16, r, 16); }
        brand[48] = 0;
    }
    char *b = brand;
    while (*b == ' ') b++;
    cpuid(1, 0, r1);
    if (maxleaf >= 7) cpuid(7, 0, r7);
    if (maxext >= 0x80000001u) cpuid(0x80000001u, 0, e1);
    if (maxext >= 0x80000007u) cpuid(0x80000007u, 0, e7);
    unsigned fbase = (r1[0] >> 8) & 0xF, mbase = (r1[0] >> 4) & 0xF;
    g_cpu_fam = fbase == 0xF ? fbase + ((r1[0] >> 20) & 0xFF) : fbase;
    g_cpu_model = (fbase == 6 || fbase == 0xF) ? (((r1[0] >> 16) & 0xF) << 4) | mbase : mbase;
    emit("vendor: %s", g_cpu_vendor);
    emit("brand: %s", b[0] ? b : "not reported");
    emit("signature: family %u model %#x stepping %u", g_cpu_fam, g_cpu_model, r1[0] & 0xF);
    emit("microarchitecture: %s (a guess from the family and model; \"unknown\" is normal for parts newer than this table)",
         microarch(g_cpu_vendor, g_cpu_fam, g_cpu_model));

    char fl[400] = "";
    add_flag(fl, sizeof fl, r1[3], 4, "tsc");        add_flag(fl, sizeof fl, r1[3], 9, "apic");      add_flag(fl, sizeof fl, r1[3], 16, "pat");
    add_flag(fl, sizeof fl, r1[3], 25, "sse");       add_flag(fl, sizeof fl, r1[3], 26, "sse2");     add_flag(fl, sizeof fl, r1[2], 0, "sse3");
    add_flag(fl, sizeof fl, r1[2], 9, "ssse3");      add_flag(fl, sizeof fl, r1[2], 19, "sse4.1");   add_flag(fl, sizeof fl, r1[2], 20, "sse4.2");
    add_flag(fl, sizeof fl, r1[2], 25, "aes");       add_flag(fl, sizeof fl, r1[2], 1, "pclmul");    add_flag(fl, sizeof fl, r1[2], 26, "xsave");
    add_flag(fl, sizeof fl, r1[2], 28, "avx");       add_flag(fl, sizeof fl, r1[2], 12, "fma");      add_flag(fl, sizeof fl, r1[2], 30, "rdrand");
    add_flag(fl, sizeof fl, r1[2], 21, "x2apic");    add_flag(fl, sizeof fl, r1[2], 24, "tsc-deadline");
    add_flag(fl, sizeof fl, r7[1], 5, "avx2");       add_flag(fl, sizeof fl, r7[1], 7, "smep");      add_flag(fl, sizeof fl, r7[1], 20, "smap");
    add_flag(fl, sizeof fl, r7[1], 18, "rdseed");    add_flag(fl, sizeof fl, r7[1], 16, "avx512f");
    add_flag(fl, sizeof fl, e1[3], 20, "nx");        add_flag(fl, sizeof fl, e1[3], 26, "1gb-pages"); add_flag(fl, sizeof fl, e1[3], 29, "lm");
    add_flag(fl, sizeof fl, e7[3], 8, "invariant-tsc");
    add_flag(fl, sizeof fl, r1[2], 5, "vmx");        add_flag(fl, sizeof fl, e1[2], 2, "svm");
    emit("features: %s", fl[0] ? fl : "none reported");
    emit("invariant_tsc: %s", (e7[3] & (1u << 8)) ? "yes" : "no");

    g_cpu_hypervisor = (r1[2] >> 31) & 1;
    if (g_cpu_hypervisor) {
        char sig[13] = "";
        uint32_t h[4];
        cpuid(0x40000000u, 0, h);
        memcpy(sig, &h[1], 4); memcpy(sig + 4, &h[2], 4); memcpy(sig + 8, &h[3], 4);
        sig[12] = 0;
        for (int i = 0; sig[i]; i++) if (sig[i] < 0x20 || sig[i] > 0x7E) sig[i] = '?';
        emit("hypervisor: yes (%s)", sig);
    } else {
        emit("hypervisor: no");
    }

    char **acpi = NULL;
    size_t nacpi = 0;
    char *acpi_text = slurp("/proc/acpi");
    if (acpi_text) acpi = split(acpi_text, &nacpi);
    const char *procs = acpi ? find_key(acpi, nacpi, "processors") : NULL;
    if (procs) emit("logical_processors: %d (from the ACPI MADT; only the boot processor is used)", atoi(procs));
    else emit("logical_processors: not available (no ACPI tables)");
    free(acpi);
    free(acpi_text);

    char *hw = slurp("/proc/hwinfo");
    if (hw) {
        size_t nh;
        char **hl = split(hw, &nh);
        const char *tsc = find_key(hl, nh, "tsc.mhz");
        emit("tsc_mhz: %s", tsc ? tsc : "not available");
        free(hl);
        free(hw);
    } else {
        emit("tsc_mhz: not available (no /proc/hwinfo)");
    }
    static const char *const cpu_pre[] = { "processor.", NULL };
    if (dmi) pass_through(dmi, ndmi, cpu_pre, "dmi.");
}

/* ------------------------------------------------------------------ PCI */

typedef struct {
    char bdf[8];
    unsigned cls, sub, pif, ven, dev;
    char name[64], driver[24], bars[200], link[24], max[24];
    int msi, msix, pcie;
} Pci;

static Pci g_pci[128];
static int g_npci;

static const char *class_name(unsigned cls, unsigned sub) {
    static const struct { unsigned cs; const char *n; } T[] = {
        { 0x0000, "unclassified device" }, { 0x0001, "VGA-compatible unclassified device" },
        { 0x0100, "SCSI storage controller" }, { 0x0101, "IDE interface" }, { 0x0102, "floppy disk controller" }, { 0x0104, "RAID bus controller" },
        { 0x0105, "ATA controller" }, { 0x0106, "SATA controller" }, { 0x0107, "Serial Attached SCSI controller" }, { 0x0108, "non-volatile memory controller" },
        { 0x0109, "UFS controller" }, { 0x0180, "mass storage controller" },
        { 0x0200, "Ethernet controller" }, { 0x0280, "network controller" },
        { 0x0300, "VGA compatible controller" }, { 0x0301, "XGA compatible controller" }, { 0x0302, "3D controller" }, { 0x0380, "display controller" },
        { 0x0400, "multimedia video controller" }, { 0x0401, "multimedia audio controller" }, { 0x0403, "audio device" }, { 0x0480, "multimedia controller" },
        { 0x0500, "RAM memory" }, { 0x0501, "flash memory" }, { 0x0580, "memory controller" },
        { 0x0600, "host bridge" }, { 0x0601, "ISA bridge" }, { 0x0604, "PCI bridge" }, { 0x0607, "CardBus bridge" }, { 0x0680, "bridge" },
        { 0x0700, "serial controller" }, { 0x0701, "parallel controller" }, { 0x0703, "modem" }, { 0x0780, "communication controller" },
        { 0x0800, "interrupt controller" }, { 0x0801, "DMA controller" }, { 0x0802, "timer" }, { 0x0803, "RTC" }, { 0x0805, "SD host controller" },
        { 0x0806, "IOMMU" }, { 0x0880, "system peripheral" },
        { 0x0900, "keyboard controller" }, { 0x0902, "mouse controller" }, { 0x0980, "input device controller" },
        { 0x0c00, "FireWire (IEEE 1394)" }, { 0x0c03, "USB controller" }, { 0x0c05, "SMBus" }, { 0x0c07, "IPMI interface" }, { 0x0c80, "serial bus controller" },
        { 0x0d11, "Bluetooth" }, { 0x0d80, "wireless controller" },
        { 0x1000, "encryption controller" }, { 0x1100, "signal processing controller" }, { 0x1180, "signal processing controller" },
        { 0x1200, "processing accelerator" },
    };
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) if (T[i].cs == (cls << 8 | sub)) return T[i].n;
    return "device";
}

static const char *usb_host_name(unsigned pif) {
    switch (pif) {
    case 0x00: return "UHCI"; case 0x10: return "OHCI"; case 0x20: return "EHCI"; case 0x30: return "xHCI";
    case 0x40: return "USB4 host interface"; case 0x80: return "unspecified host controller"; case 0xFE: return "device controller";
    default: return "host controller";
    }
}

static void load_pci(void) {
    char *text = slurp("/proc/pci"), *caps = slurp("/proc/pcicaps");
    if (!text) return;
    size_t n;
    char **lines = split(text, &n);
    Pci *cur = NULL;
    for (size_t i = 0; i < n; i++) {
        char *l = lines[i];
        if (hexd(l[0]) && l[2] == ':') {
            if (g_npci >= (int)(sizeof g_pci / sizeof g_pci[0])) break;
            cur = &g_pci[g_npci++];
            memset(cur, 0, sizeof *cur);
            unsigned cs, pif;
            if (sscanf(l, "%7s %x:%x %x:%x", cur->bdf, &cs, &pif, &cur->ven, &cur->dev) != 5) { g_npci--; cur = NULL; continue; }
            cur->cls = cs >> 8;
            cur->sub = cs & 0xFF;
            cur->pif = pif;
            strlcpy(cur->driver, "none", sizeof cur->driver);
            char *drv = strrchr(l, '[');
            if (drv && strchr(drv, ']')) {
                strlcpy(cur->driver, drv + 1, sizeof cur->driver);
                char *e = strchr(cur->driver, ']');
                if (e) *e = 0;
            }
            strlcpy(cur->link, "-", sizeof cur->link);
            strlcpy(cur->max, "-", sizeof cur->max);
        } else if (cur && l[0] == ' ') {                                   /* "    bar0 mem 0x81060000 size 0x20000" */
            int idx;
            char kind[8];
            unsigned long base, size;
            if (sscanf(l, " bar%d %7s %lx size %lx", &idx, kind, &base, &size) == 4) {
                char one[48];
                snprintf(one, sizeof one, "%s%d:%s:%#lx", cur->bars[0] ? "," : "", idx, kind, size);
                strlcat(cur->bars, one, sizeof cur->bars);
            }
        }
    }
    if (caps) {
        size_t nc;
        char **cl = split(caps, &nc);
        for (size_t i = 0; i < nc; i++) {                                   /* "00:04.0: 1b36:0010 msi=no msix=yes pcie=no [link=2.5GT/s:x1 max=...]" */
            char bdf[8];
            if (sscanf(cl[i], "%7[0-9a-f:.]", bdf) != 1) continue;
            size_t bl = strlen(bdf);
            if (bl && bdf[bl - 1] == ':') bdf[bl - 1] = 0;
            for (int k = 0; k < g_npci; k++) {
                if (strcmp(g_pci[k].bdf, bdf)) continue;
                g_pci[k].msi = strstr(cl[i], "msi=yes") != NULL;
                g_pci[k].msix = strstr(cl[i], "msix=yes") != NULL;
                g_pci[k].pcie = strstr(cl[i], "pcie=yes") != NULL;
                const char *lk = strstr(cl[i], " link="), *mx = strstr(cl[i], " max=");
                if (lk) sscanf(lk + 6, "%23s", g_pci[k].link);
                if (mx) sscanf(mx + 5, "%23s", g_pci[k].max);
            }
        }
        free(cl);
    }
    for (int i = 0; i < g_npci; i++) {                                      /* a name from the class code: the kernel's own list is short */
        Pci *p = &g_pci[i];
        if (p->cls == 0x0c && p->sub == 0x03) snprintf(p->name, sizeof p->name, "USB controller (%s)", usb_host_name(p->pif));
        else if (p->cls == 0x01 && p->sub == 0x06) snprintf(p->name, sizeof p->name, "SATA controller%s", p->pif == 1 ? " (AHCI)" : p->pif == 0 ? " (vendor specific)" : "");
        else if (p->cls == 0x01 && p->sub == 0x08) snprintf(p->name, sizeof p->name, "NVMe controller%s", p->pif == 2 ? "" : " (not NVMe-programmable)");
        else strlcpy(p->name, class_name(p->cls, p->sub), sizeof p->name);
    }
    free(lines);
    free(text);
    free(caps);
}

static void report_pci(void) {
    section("pci");
    if (!g_npci) { emit("pci: not available (no /proc/pci)"); return; }
    int nodrv = 0;
    for (int i = 0; i < g_npci; i++) if (!strcmp(g_pci[i].driver, "none")) nodrv++;
    emit("count: %d functions, %d without a driver", g_npci, nodrv);
    for (int i = 0; i < g_npci; i++) {
        const Pci *p = &g_pci[i];
        emit("pci.%s: %04x:%04x class=%02x%02x:%02x driver=%s msi=%s msix=%s pcie=%s link=%s max=%s bars=%s name=%s", p->bdf, p->ven, p->dev, p->cls, p->sub,
             p->pif, p->driver, p->msi ? "yes" : "no", p->msix ? "yes" : "no", p->pcie ? "yes" : "no", p->link, p->max, p->bars[0] ? p->bars : "-", p->name);
    }
}

/* ------------------------------------------------------------------ storage */

typedef struct { char name[24], driver[16], model[64]; unsigned long long bytes; unsigned ss; int ro; char serial[72]; } Disk;
static Disk g_disk[24];
static int g_ndisk;

static const char *storage_kind(const Pci *p) {
    unsigned cs = p->cls << 8 | p->sub;
    if (cs == 0x0106) return p->pif == 1 ? "AHCI (SATA)" : "SATA (not AHCI)";
    if (cs == 0x0108) return p->pif == 2 ? "NVMe" : "non-volatile memory (not NVMe)";
    if (cs == 0x0104) return p->ven == 0x8086 ? "RAID mode (Intel RST or VMD): the drives behind it are hidden from this OS" : "RAID controller";
    if (cs == 0x0101) return "IDE";
    if (cs == 0x0105) return "ATA";
    if (cs == 0x0100 || cs == 0x0180) return (p->ven == 0x1af4 && (p->dev == 0x1001 || p->dev == 0x1042)) ? "virtio-blk" : (cs == 0x0100 ? "SCSI" : "mass storage");
    if (cs == 0x0107) return "SAS";
    if (cs == 0x0109) return "UFS";
    if (cs == 0x0805) return "SD host controller (SDHCI)";
    return NULL;
}

static void load_disks(char **ahci, size_t nahci, char **nvme, size_t nnvme) {
    char *text = slurp("/proc/partitions");
    if (!text) return;
    size_t n;
    char **lines = split(text, &n);
    for (size_t i = 0; i < n && g_ndisk < (int)(sizeof g_disk / sizeof g_disk[0]); i++) {
        char name[24], parent[24], label[48], driver[16], model[64] = "";
        unsigned long long size, start;
        unsigned ss;
        int ro;
        if (sscanf(lines[i], "%23s %llu %u %d %23s %llu %47s %15s %63[^\n]", name, &size, &ss, &ro, parent, &start, label, driver, model) < 8) continue;
        if (strcmp(parent, "-")) continue;                                  /* partitions are not listed here */
        Disk *d = &g_disk[g_ndisk++];
        memset(d, 0, sizeof *d);
        strlcpy(d->name, name, sizeof d->name);
        strlcpy(d->driver, driver, sizeof d->driver);
        strlcpy(d->model, strcmp(model, "-") ? model : "", sizeof d->model);
        d->bytes = size;
        d->ss = ss;
        d->ro = ro;
        /* the serial number is in /proc/ahci ("port 0 sda "MODEL" serial X fw Y ...") and /proc/nvme ("nsid 1 nvme0n1 ..." under its controller line) */
        for (size_t k = 0; k < nahci; k++) {
            char needle[40];
            snprintf(needle, sizeof needle, " %s \"", name);
            const char *at = strstr(ahci[k], needle), *sr = strstr(ahci[k], " serial ");
            if (at && sr) { const char *e = strstr(sr + 8, " fw "); size_t len = e ? (size_t)(e - (sr + 8)) : strlen(sr + 8); if (len >= sizeof d->serial) len = sizeof d->serial - 1; memcpy(d->serial, sr + 8, len); d->serial[len] = 0; }
        }
        for (size_t k = 0; k < nnvme; k++) {
            if (!strncmp(nvme[k], "nvme", 4) && strstr(nvme[k], " serial ") && !strncmp(name, nvme[k], strlen(nvme[k]) < 5 ? 4 : 5)) {
                const char *sr = strstr(nvme[k], " serial "), *e = strstr(sr + 8, " fw ");
                size_t len = e ? (size_t)(e - (sr + 8)) : strlen(sr + 8);
                if (len >= sizeof d->serial) len = sizeof d->serial - 1;
                memcpy(d->serial, sr + 8, len);
                d->serial[len] = 0;
            }
        }
        add_secret(d->serial);
    }
    free(lines);
    free(text);
}

/* Before the first line is written: the serial numbers of the disks are secrets from the start (the kernel log mentions them too). */
static void load_storage(void) {
    char *at = slurp("/proc/ahci"), *nt = slurp("/proc/nvme");
    size_t nahci = 0, nnvme = 0;
    char **ahci = at ? split(at, &nahci) : NULL, **nvme = nt ? split(nt, &nnvme) : NULL;
    load_disks(ahci, nahci, nvme, nnvme);
    free(ahci); free(nvme); free(at); free(nt);
}

static void report_storage(void) {
    section("storage");
    int nctl = 0, raid = 0;
    for (int i = 0; i < g_npci; i++) {
        const Pci *p = &g_pci[i];
        const char *k = storage_kind(p);
        if (!k) continue;
        if (p->cls == 0x01 && p->sub == 0x04) raid = 1;
        emit("controller.%d: %s %04x:%04x %s driver=%s", nctl++, p->bdf, p->ven, p->dev, k, p->driver);
    }
    if (!nctl) emit("controllers: none found (no storage controller on the PCI bus)");
    else emit("controllers: %d", nctl);
    if (raid) emit("raid_hint: a controller in RAID mode hides its drives from this OS; the firmware setup usually has a switch to AHCI (see docs/REAL_HARDWARE.md, a Windows that was installed in RAID mode needs preparing first)");
    if (log_has("is in RAID (Intel RST) mode")) emit("raid_log: the kernel log says the SATA controller is in RAID (Intel RST) mode");
    if (log_has("legacy IDE mode")) emit("ide_log: the kernel log says the SATA controller is in legacy IDE mode");
    emit("disks: %d", g_ndisk);
    for (int i = 0; i < g_ndisk; i++) {
        const Disk *d = &g_disk[i];
        char sz[16];
        kb_format_size(d->bytes, sz, sizeof sz);
        emit("disk.%d: %s driver=%s size=%llu (%s) sector=%u read_only=%s model=%s", i, d->name, d->driver, d->bytes, sz, d->ss, d->ro ? "yes" : "no", d->model[0] ? d->model : "-");
        char key[40];
        snprintf(key, sizeof key, "disk.%d.serial", i);
        emit_private(key, d->serial, "<redacted>");
    }
    char *ut = slurp("/proc/usbstorage");
    if (ut) {
        size_t nu;
        char **ul = split(ut, &nu);
        int shown = 0;
        for (size_t i = 0; i < nu; i++) if (ul[i][0]) { emit("usbstorage.%d: %s", shown++, ul[i]); if (shown >= 12) break; }
        free(ul);
        free(ut);
    }
}

/* ------------------------------------------------------------------ USB, input, network, audio */

static void report_usb(void) {
    section("usb");
    int nhc = 0;
    for (int i = 0; i < g_npci; i++) {
        const Pci *p = &g_pci[i];
        if (p->cls != 0x0c || p->sub != 0x03) continue;
        emit("host.%d: %s %04x:%04x %s driver=%s", nhc++, p->bdf, p->ven, p->dev, usb_host_name(p->pif), p->driver);
    }
    emit("hosts: %d", nhc);
    char *text = slurp("/proc/usb");
    if (!text) { emit("devices: not available (no /proc/usb: the kernel has no USB subsystem or usb=off)"); return; }
    size_t n;
    char **lines = split(text, &n);
    int ndev = 0, nbus = 0;
    for (size_t i = 0; i < n; i++) {
        if (starts(lines[i], "Bus ") && strstr(lines[i], " Port ")) emit("device.%d: %s", ndev++, lines[i]);
        else if (starts(lines[i], "Bus ")) emit("bus.%d: %s", nbus++, lines[i]);
    }
    emit("devices: %d", ndev);
    free(lines);
    free(text);
}

static void report_input(void) {
    section("input");
    int usb_k = 0, usb_m = 0, other = 0;
    char *text = slurp("/proc/usb");
    if (text) {
        size_t n;
        char **lines = split(text, &n);
        for (size_t i = 0; i < n; i++) {
            if (!strstr(lines[i], " Port ")) continue;
            if (strstr(lines[i], "driver hid-keyboard")) { emit("keyboard.usb.%d: %s", usb_k++, lines[i]); }
            else if (strstr(lines[i], "driver hid-mouse")) { emit("mouse.usb.%d: %s", usb_m++, lines[i]); }
            else if (strstr(lines[i], "driver hid")) { emit("other.usb.%d: %s", other++, lines[i]); }
        }
        free(lines);
        free(text);
    }
    int ps2_kbd = log_has("keyboard online: reading your mind"), ps2_mouse = log_has("mouse online");
    emit("keyboard.ps2: %s", ps2_kbd ? "found" : "not found");
    emit("mouse.ps2: %s", ps2_mouse ? "found" : "not found");
    emit("keyboards: %d usb, %d ps/2", usb_k, ps2_kbd);
    emit("mice: %d usb, %d ps/2", usb_m, ps2_mouse);
    if (log_has("no PS/2 controller found")) emit("ps2: no controller on this machine");
    for (size_t i = 0; i < g_ndlines; i++)
        if (strstr(g_dlines[i], "no USB keyboard is bound") || strstr(g_dlines[i], "i2c") || strstr(g_dlines[i], "I2C")) emit("note: %s", g_dlines[i]);
}

/* An address is not a serial number, but a report is posted in public: a private address loses its last byte, a public one is not shown (--full shows it). */
static const char *shown_ip(uint32_t ip) {
    static char buf[24];
    if (!ip) return "-";
    if (g_full) return nu_ip(ip);
    unsigned a = ip >> 24, b = (ip >> 16) & 255, c = (ip >> 8) & 255;
    int priv = a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254);
    if (!priv) return "<public>";
    snprintf(buf, sizeof buf, "%u.%u.%u.x", a, b, c);
    return buf;
}

static void report_network(void) {
    section("network");
    struct gnet_if v[16];
    int n = 0;
    for (; n < 16; n++) {
        memset(&v[n], 0, sizeof v[n]);
        v[n].index = (uint32_t)n;
        if (gap_netctl(NETCTL_IF_GET, &v[n]) < 0) break;
    }
    int nic = 0;
    for (int i = 0; i < n; i++) {
        const struct gnet_if *f = &v[i];
        if (f->flags & GIFF_LOOPBACK) continue;
        char speed[32] = "not reported";
        for (size_t k = g_ndlines; k > 0; k--) {                           /* "[Yukari] eth0: link up, 1000 Mb/s full duplex" */
            char needle[40];
            snprintf(needle, sizeof needle, "%s: link up, ", f->name);
            const char *at = strstr(g_dlines[k - 1], needle);
            if (at) { snprintf(speed, sizeof speed, "%d Mb/s", atoi(at + strlen(needle))); break; }
        }
        emit("interface.%d: %s driver=%s mac=%s link=%s speed=%s mtu=%u address=%s", nic++, f->name, f->driver, nu_mac(f->mac), (f->flags & GIFF_RUNNING) ? "up" : "no carrier",
             speed, f->mtu, shown_ip(f->addr));
    }
    emit("interfaces: %d", nic);
    int pci_nics = 0, undriven = 0;
    for (int i = 0; i < g_npci; i++) {
        if (g_pci[i].cls != 0x02) continue;
        pci_nics++;
        if (!strcmp(g_pci[i].driver, "none")) undriven++;
    }
    emit("pci_network_controllers: %d, %d without a driver", pci_nics, undriven);
}

static void report_audio(void) {
    section("audio");
    int nac = 0;
    for (int i = 0; i < g_npci; i++)
        if (g_pci[i].cls == 0x04 && (g_pci[i].sub == 0x01 || g_pci[i].sub == 0x03)) emit("controller.%d: %s %04x:%04x %s driver=%s", nac++, g_pci[i].bdf, g_pci[i].ven, g_pci[i].dev, g_pci[i].name, g_pci[i].driver);
    emit("controllers: %d", nac);
    char *text = slurp("/proc/audio");
    if (!text) { emit("codec: not available (no /proc/audio: no sound driver is running)"); return; }
    size_t n;
    char **lines = split(text, &n);
    static const char *const keys[] = { "device:", "path:", "codec:", "pin ", "state:", NULL };
    pass_through(lines, n, keys, "proc.");
    const char *codec = find_key(lines, n, "codec");
    if (codec) {
        const char *vd = strstr(codec, "vendor/device ");
        if (vd) emit("codec_id: %.8s", vd + 14);
    }
    int pins = 0, usable = 0;
    for (size_t i = 0; i < n; i++) if (starts(lines[i], "pin ")) { pins++; if (!strstr(lines[i], "config 00000000") && !strstr(lines[i], "no connection")) usable++; }
    emit("pin_config: %s (%d pins listed, %d with a connection)", usable ? "usable" : (pins ? "no usable pin" : "none listed"), pins, usable);
    free(lines);
    free(text);
}

/* ------------------------------------------------------------------ the rest: system, memory, firmware, acpi, interrupts, sensors, log */

static void report_log(void) {
    section("log");
    if (!g_ndlines) { emit("lines: not available (/dev/kmsg could not be read)"); return; }
    static const char *const words[] = { "warn", "error", "fail", "unsupported", "timeout", "no ", NULL };
    int matched = 0;
    for (size_t i = 0; i < g_ndlines; i++) {
        int hit = 0;
        for (int k = 0; words[k] && !hit; k++) hit = strcasestr(g_dlines[i], words[k]) != NULL;
        if (hit) matched++;
    }
    emit("lines: %d matching of %zu in the whole log%s", matched, g_ndlines, matched > 100 ? " (the first 100 follow)" : "");
    int shown = 0;
    for (size_t i = 0; i < g_ndlines && shown < 100; i++) {
        int hit = 0;
        for (int k = 0; words[k] && !hit; k++) hit = strcasestr(g_dlines[i], words[k]) != NULL;
        if (hit) { emit("log: %s", g_dlines[i]); shown++; }
    }
}

static const char *const SYSTEM_KEYS[] = { "smbios", "bios.", "system.", "baseboard.", "chassis.", NULL };

static void report_system(char **dmi, size_t ndmi) {
    section("system");
    if (!dmi) { emit("smbios: not provided (no /proc/dmi)"); return; }
    for (size_t i = 0; i < ndmi; i++) {
        int ok = 0;
        for (int k = 0; SYSTEM_KEYS[k]; k++) if (starts(dmi[i], SYSTEM_KEYS[k])) ok = 1;
        if (!ok || !dmi[i][0]) continue;
        char *c = strchr(dmi[i], ':');
        if (c && (strstr(dmi[i], ".serial:") || strstr(dmi[i], ".uuid:") || strstr(dmi[i], ".asset:"))) {
            *c = 0;
            emit_private(dmi[i], c + 2, strstr(dmi[i], "uuid") ? "<uuid>" : "<redacted>");
            *c = ':';
        } else {
            emit("%s", dmi[i]);
        }
    }
}

static void report_memory(char **dmi, size_t ndmi) {
    section("memory");
    char *hw = slurp("/proc/hwinfo");
    if (hw) {
        size_t n;
        char **l = split(hw, &n);
        static const char *const keys[] = { "memory.", NULL };
        pass_through(l, n, keys, "");
        free(l);
        free(hw);
    } else {
        emit("map: not available (no /proc/hwinfo)");
    }
    if (dmi) {
        for (size_t i = 0; i < ndmi; i++) {
            if (!starts(dmi[i], "memory.")) continue;
            char *c = strchr(dmi[i], ':');
            if (c && strstr(dmi[i], ".serial:")) { *c = 0; emit_private(dmi[i], c + 2, "<redacted>"); *c = ':'; }
            else emit("dmi.%s", dmi[i]);
        }
    }
}

static void report_firmware(void) {
    section("firmware");
    char *hw = slurp("/proc/hwinfo");
    if (!hw) { emit("firmware: not available (no /proc/hwinfo)"); return; }
    size_t n;
    char **l = split(hw, &n);
    static const char *const keys[] = { "firmware", "gop.", NULL };
    if (!pass_through(l, n, keys, "")) emit("firmware: not available");
    free(l);
    free(hw);
}

static void report_acpi(void) {
    section("acpi");
    char *hw = slurp("/proc/hwinfo");
    if (hw) {
        size_t n;
        char **l = split(hw, &n);
        static const char *const keys[] = { "acpi", NULL };
        if (!pass_through(l, n, keys, "")) emit("acpi: not available");
        free(l);
        free(hw);
    } else {
        emit("acpi: not available (no /proc/hwinfo)");
    }
    char *ac = slurp("/proc/acpi");
    if (ac) {
        size_t n;
        char **l = split(ac, &n);
        static const char *const keys[] = { "poweroff", "reset", "button", NULL };
        pass_through(l, n, keys, "proc.");
        free(l);
        free(ac);
    }
}

static void report_interrupts(void) {
    section("interrupts");
    char *hw = slurp("/proc/hwinfo");
    int shown = 0;
    if (hw) {
        size_t n;
        char **l = split(hw, &n);
        static const char *const keys[] = { "interrupts.", NULL };
        shown = pass_through(l, n, keys, "");
        free(l);
        free(hw);
    }
    if (!shown) emit("interrupts: not available (no /proc/hwinfo)");
    char *it = slurp("/proc/interrupts");
    if (it) {
        size_t n;
        char **l = split(it, &n);
        for (size_t i = 0; i < n; i++) if (l[i][0]) emit_kv("irq.count.", l[i]);
        free(l);
        free(it);
    }
}

static void report_sensors(void) {
    section("sensors");
    char *th = slurp("/proc/thermal");
    if (!th) { emit("thermal: not available (no /proc/thermal)"); return; }
    size_t n;
    char **l = split(th, &n);
    pass_through(l, n, NULL, "");
    free(l);
    free(th);
}

static void report_cmdline(void) {
    section("cmdline");
    char *c = slurp("/proc/cmdline");
    if (c) {
        size_t n = strlen(c);
        while (n && (c[n - 1] == '\n' || c[n - 1] == ' ')) c[--n] = 0;
        emit("cmdline: %s", c);
        free(c);
    } else {
        emit("cmdline: not available");
    }
    section("quirks");
    emit("quirks: none recorded (this kernel keeps no table of machine-specific workarounds yet; the options on the command line above and the log lines are all there is)");
}

/* ------------------------------------------------------------------ the whole report */

static void report_header(void) {
    char *ver = slurp("/proc/version");
    if (ver) { size_t n = strlen(ver); while (n && (ver[n - 1] == '\n' || ver[n - 1] == ' ')) ver[--n] = 0; }
    char *id = slurp("/ScarletDevilMansion/share/doc/touhouos/BUILD");
    if (id) { size_t n = strlen(id); while (n && (id[n - 1] == '\n' || id[n - 1] == ' ')) id[--n] = 0; }
    time_t now = time(NULL);
    struct tm tm;
    char when[40] = "unknown";
    if (gmtime_r(&now, &tm)) strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &tm);
    fprintf(g_out, "format: hwreport %d\n", HW_FORMAT);
    g_lines++;
    emit("build: %s; hwreport compiled %s %s; commit %s", ver ? ver : "unknown kernel", __DATE__, __TIME__, id && id[0] ? id : "not recorded by this build");
    emit("date: %s", when);
    if (g_full) emit("redaction: off (--full: serial numbers, UUIDs, MAC addresses and the host name are in this file; do not post it)");
    else emit("redaction: on (the last three bytes of MAC addresses, serial numbers, UUIDs, a non-default host name and the last byte of a private IP address are replaced; hwreport --full keeps them)");
    emit("# Bunbunmaru Shimbun, hardware edition: Aya's reporters looked into every corner of this machine. Plain facts follow; \"key: value\" lines are for scripts.");
    free(ver);
    free(id);
}

static void collect_secrets(char **dmi, size_t ndmi) {
    if (g_full) return;
    for (size_t i = 0; dmi && i < ndmi; i++) {
        const char *c = strchr(dmi[i], ':');
        if (c && (strstr(dmi[i], ".serial:") || strstr(dmi[i], ".uuid:") || strstr(dmi[i], ".asset:"))) add_secret(c + 1);
    }
    if (gethostname(g_host, sizeof g_host) == 0 && g_host[0] && strcmp(g_host, "gensokyo")) add_secret(g_host);
}

static int run(void) {
    char *dmi_text = slurp("/proc/dmi");
    size_t ndmi = 0;
    char **dmi = dmi_text ? split(dmi_text, &ndmi) : NULL;
    collect_secrets(dmi, ndmi);
    load_log();
    load_pci();
    load_storage();
    report_header();
    report_system(dmi, ndmi);
    report_cpu(dmi, ndmi);
    report_memory(dmi, ndmi);
    report_firmware();
    report_acpi();
    report_pci();
    report_storage();
    report_usb();
    report_network();
    report_audio();
    report_input();
    report_interrupts();
    report_sensors();
    report_log();
    report_cmdline();
    fprintf(g_out, "\nend: hwreport %d\n", HW_FORMAT);
    g_lines += 2;
    free(dmi);
    free(dmi_text);
    return 0;
}

/* The HUMANVILLAGE volume counts as mounted when /proc/mounts lists it read-write (the directory exists in every boot). */
static int stick_mounted(void) {
    char *m = slurp("/proc/mounts");
    if (!m) return 0;
    int yes = 0;
    size_t n;
    char **l = split(m, &n);
    for (size_t i = 0; i < n; i++) {
        char src[64], target[64], type[16], mode[8];
        if (sscanf(l[i], "%63s %63s %15s %7s", src, target, type, mode) == 4 && !strcmp(target, "/HumanVillage") && !strcmp(mode, "rw")) yes = 1;
    }
    free(l);
    free(m);
    return yes;
}

static void usage(void) {
    puts("usage: hwreport [--full] [-o [FILE] | FILE]\n"
         "  Writes a plain-text report of this machine's hardware (CPU, firmware, PCI devices and their drivers, disks, USB, network, audio ...).\n"
         "  No argument: to the screen. -o: to /HumanVillage/hwreport.txt when the stick's volume is mounted (your home on the stick if you may not write\n"
         "  its top), else /tmp/hwreport.txt. FILE: to that file.\n"
         "  Serial numbers, UUIDs, the last three bytes of MAC addresses and a non-default host name are replaced unless --full is given.\n"
         "  Send the file to whoever collects reports: docs/HARDWARE_DB.md.");
}

APPLET(hwreport) {
    const char *file = NULL;
    int to_default = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--full")) g_full = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else if (!strcmp(argv[i], "-o")) { if (i + 1 < argc && argv[i + 1][0] != '-') file = argv[++i]; else to_default = 1; }
        else if (argv[i][0] != '-' && !file) file = argv[i];
        else { kb_error("unrecognized argument '%s'", argv[i]); usage(); return 2; }
    }
    g_out = stdout;
    char chosen[200] = "";
    if (file) {
        g_out = fopen(file, "w");
        if (!g_out) { kb_error("%s: %s", file, strerror(errno)); return 1; }
        strlcpy(chosen, file, sizeof chosen);
    } else if (to_default) {
        /* The top of the stick's volume belongs to root (docs/STORAGE.md: only the home directories are the residents'): an ordinary user's report goes to
         * the home directory on the stick instead, which is on the same stick. Without the volume, or if neither can be written, /tmp. */
        FILE *f = NULL;
        if (stick_mounted()) {
            f = fopen("/HumanVillage/hwreport.txt", "w");
            if (f) strlcpy(chosen, "/HumanVillage/hwreport.txt", sizeof chosen);
            const char *home = getenv("HOME");
            if (!f && home && !strncmp(home, "/HumanVillage/", 14)) {
                snprintf(chosen, sizeof chosen, "%s/hwreport.txt", home);
                f = fopen(chosen, "w");
            }
        }
        if (!f) {
            f = fopen("/tmp/hwreport.txt", "w");
            strlcpy(chosen, "/tmp/hwreport.txt", sizeof chosen);
        }
        if (!f) { kb_error("/tmp/hwreport.txt: %s", strerror(errno)); return 1; }
        g_out = f;
    }
    int rc = run();
    if (g_out != stdout) {
        fclose(g_out);
        g_out = stdout;
        sync();
        printf("hwreport: written to %s (%d lines)%s\n", chosen, g_lines, g_full ? ", with serial numbers and addresses: keep it to yourself" : "");
    }
    return rc;
}
