/*
 * CPU thermal and power registers: decoding, units, and the test hook's table (see kernel/include/thermal.h). No machine access here.
 *
 * Sources: Intel 64 and IA-32 Architectures Software Developer's Manual, volume 3 (14.7 thermal monitoring, 14.9 package level
 * thermal management, 15.10 platform specific power management support: RAPL) and volume 4 (the model-specific register tables).
 */
#include "thermal.h"

void thermal_decode_status(u64 m, ThermalStatus *o) {
    o->valid = (m >> 31) & 1;
    o->readout = (int)((m >> 16) & 0x7F);
    o->resolution = (int)((m >> 27) & 0xF);
    o->status = o->log = 0;
    for (int k = 0; k < 8; k++) {
        if ((m >> (2 * k)) & 1) o->status |= 1u << k;                  /* bits 0, 2, 4, ... 14 */
        if ((m >> (2 * k + 1)) & 1) o->log |= 1u << k;                 /* bits 1, 3, 5, ... 15 */
    }
}

int thermal_decode_tjmax(u64 m, int *offset) {
    if (offset) *offset = (int)((m >> 24) & 0x3F);
    return (int)((m >> 16) & 0xFF);
}

size_t thermal_flag_names(u32 bits, char *buf, size_t cap) {
    static const char *const names[8] = { "hot", "prochot", "critical", "threshold1", "threshold2", "power_limit", "current_limit", "cross_domain" };
    size_t n = 0;
    if (cap == 0) return 0;
    buf[0] = 0;
    for (int k = 0; k < 8; k++) {
        if (!(bits & (1u << k))) continue;
        const char *s = names[k];
        if (n && n + 1 < cap) buf[n++] = ' ';
        while (*s && n + 1 < cap) buf[n++] = *s++;
        buf[n] = 0;
    }
    if (n == 0 && cap > 1) { buf[0] = '-'; buf[1] = 0; n = 1; }
    return n;
}

/* The multiplier in IA32_PERF_STATUS counts a reference clock: 133.33 MHz on Nehalem and Westmere, 100 MHz from Sandy Bridge on. The Atom
 * family has its own (83.3, 100, 133 MHz depending on the model), and nothing about it is promised for other vendors. */
int thermal_bus_khz(const char *vendor, u32 family, u32 model) {
    const char *intel = "GenuineIntel";
    for (int i = 0;; i++) {
        if (vendor[i] != intel[i]) return 0;
        if (!intel[i]) break;
    }
    if (family != 6) return 0;
    switch (model) {
    case 0x1A: case 0x1E: case 0x1F: case 0x25: case 0x2C: case 0x2E: case 0x2F:
        return 133333;
    case 0x1C: case 0x26: case 0x27: case 0x35: case 0x36: case 0x37: case 0x4A: case 0x4D: case 0x4C: case 0x5A: case 0x5D: case 0x5C: case 0x5F:
    case 0x7A: case 0x86: case 0x96: case 0x9C: case 0xBE:
        return 0;                                                       /* Atom and Tremont: not the same clock */
    default:
        return model >= 0x2A ? 100000 : 0;
    }
}

bool thermal_rapl_units(u64 m, int *esu) {
    int e = (int)((m >> 8) & 0x1F);                                     /* bits 12:8: energy status units, 1 / 2^e joule */
    if (e < 1 || e > 30) return false;
    if (esu) *esu = e;
    return true;
}

u64 thermal_energy_uj(u32 raw, int esu) { return ((u64)raw * 1000000ULL) >> esu; }

u64 thermal_power_mw(u32 from, u32 to, int esu, u64 interval_ms) {
    if (interval_ms == 0) return 0;
    u32 d = to - from;                                                  /* modulo 2^32: the counter wraps */
    return thermal_energy_uj(d, esu) / interval_ms;                     /* microjoules per millisecond are milliwatts */
}

/* ------------------------------------------------------------------ thermal.fake= */

static bool hex(const char **p, u64 *out) {
    const char *s = *p;
    u64 v = 0;
    int n = 0;
    for (;; s++, n++) {
        char c = *s;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) break;
        if (n >= 16) return false;
        v = v << 4 | (u64)d;
    }
    if (n == 0) return false;
    *out = v;
    *p = s;
    return true;
}

bool thermal_fake_parse(const char *spec, ThermalFake *out) {
    out->n = 0;
    const char *s = spec;
    while (*s) {
        u64 msr, val, step = 0;
        if (out->n >= 16) return false;
        if (!hex(&s, &msr) || msr > 0xFFFFFFFFu || *s++ != ':' || !hex(&s, &val)) return false;
        if (*s == '+') { s++; if (!hex(&s, &step)) return false; }
        if (*s == ',') s++;
        else if (*s) return false;
        ThermalFakeEntry *e = &out->e[out->n++];
        e->msr = (u32)msr;
        e->val = val;
        e->step = step;
        e->reads = 0;
    }
    return out->n > 0;
}

bool thermal_fake_read(ThermalFake *t, u32 msr, u64 *val) {
    for (int i = 0; i < t->n; i++) {
        if (t->e[i].msr != msr) continue;
        *val = t->e[i].val + t->e[i].step * t->e[i].reads++;
        return true;
    }
    return false;
}
