/*
 * SMBIOS structure walker and /proc/dmi formatter, the half of the DMI code that needs no machine (see kernel/include/dmi.h).
 *
 * Written from the DMTF's SMBIOS Reference Specification (DSP0134), sections 6 (the structure format: a formatted area of `length`
 * bytes, then a set of NUL-terminated strings ended by one more NUL; "string 0" means none) and 7 (the structures): type 0 BIOS
 * Information, 1 System Information, 2 Baseboard, 3 System Enclosure, 4 Processor, 17 Memory Device, 127 End-Of-Table. Fields that
 * a version of the specification added are read only when the structure's own length says they are there. Nothing here trusts the
 * table: every offset is checked against the length of the structure and every structure against the length of the table.
 */
#include "dmi.h"

/* ------------------------------------------------------------------ reading the table */

typedef struct {
    const u8 *h;            /* the formatted area */
    u8        len;          /* its length */
    const u8 *strs;         /* the string set */
    const u8 *end;          /* one past the last byte of the string set (and of the table, if it is cut) */
} Rec;

static u8  b8(const Rec *r, unsigned off)  { return off < r->len ? r->h[off] : 0; }
static u16 b16(const Rec *r, unsigned off) { return (u16)(b8(r, off) | b8(r, off + 1) << 8); }
static u32 b32(const Rec *r, unsigned off) { return (u32)b16(r, off) | (u32)b16(r, off + 2) << 16; }
static bool has(const Rec *r, unsigned off, unsigned size) { return off + size <= r->len; }

/* String number n (1-based) of the structure, printable ASCII only and trimmed; empty for 0 or a number past the set. */
static void get_str(const Rec *r, u8 n, char *out, size_t cap) {
    size_t k = 0;
    out[0] = 0;
    if (!n || !cap) return;
    const u8 *s = r->strs;
    for (unsigned i = 1; i < n; i++) {
        while (s < r->end && *s) s++;
        s++;
        if (s >= r->end) return;
    }
    while (s < r->end && *s && k + 1 < cap) {
        u8 c = *s++;
        out[k++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    out[k] = 0;
    while (k && out[k - 1] == ' ') out[--k] = 0;
    size_t lead = 0;
    while (out[lead] == ' ') lead++;
    if (lead) {
        for (size_t i = 0; i <= k - lead; i++) out[i] = out[i + lead];
    }
}

static void str_field(const Rec *r, unsigned off, char *out, size_t cap) {
    if (has(r, off, 1)) get_str(r, r->h[off], out, cap);
    else out[0] = 0;
}

/* ------------------------------------------------------------------ the structures */

static void do_bios(DmiInfo *d, const Rec *r) {
    str_field(r, 4, d->bios_vendor, sizeof d->bios_vendor);
    str_field(r, 5, d->bios_version, sizeof d->bios_version);
    str_field(r, 8, d->bios_date, sizeof d->bios_date);
    if (has(r, 0x14, 4)) {
        d->bios_major = b8(r, 0x14); d->bios_minor = b8(r, 0x15);
        d->ec_major = b8(r, 0x16); d->ec_minor = b8(r, 0x17);
    }
}

static void do_system(DmiInfo *d, const Rec *r) {
    str_field(r, 4, d->sys_manufacturer, sizeof d->sys_manufacturer);
    str_field(r, 5, d->sys_product, sizeof d->sys_product);
    str_field(r, 6, d->sys_version, sizeof d->sys_version);
    str_field(r, 7, d->sys_serial, sizeof d->sys_serial);
    if (has(r, 8, 16)) {
        bool zero = true, ones = true;
        for (unsigned i = 0; i < 16; i++) {
            d->uuid[i] = r->h[8 + i];
            if (d->uuid[i] != 0x00) zero = false;
            if (d->uuid[i] != 0xFF) ones = false;
        }
        d->have_uuid = !zero && !ones;                  /* all zeros: not set; all ones: the system does not have one */
    }
    str_field(r, 0x19, d->sys_sku, sizeof d->sys_sku);
    str_field(r, 0x1A, d->sys_family, sizeof d->sys_family);
}

static void do_board(DmiInfo *d, const Rec *r) {
    str_field(r, 4, d->bb_manufacturer, sizeof d->bb_manufacturer);
    str_field(r, 5, d->bb_product, sizeof d->bb_product);
    str_field(r, 6, d->bb_version, sizeof d->bb_version);
    str_field(r, 7, d->bb_serial, sizeof d->bb_serial);
    str_field(r, 8, d->bb_asset, sizeof d->bb_asset);
}

static void do_chassis(DmiInfo *d, const Rec *r) {
    if (d->have_chassis) return;                        /* the first enclosure is the machine's; later ones are sub-chassis */
    d->have_chassis = true;
    d->chassis_type = (u8)(b8(r, 5) & 0x7F);            /* bit 7 is the chassis lock */
    str_field(r, 4, d->ch_manufacturer, sizeof d->ch_manufacturer);
    str_field(r, 6, d->ch_version, sizeof d->ch_version);
    str_field(r, 7, d->ch_serial, sizeof d->ch_serial);
    str_field(r, 8, d->ch_asset, sizeof d->ch_asset);
}

static void do_cpu(DmiInfo *d, const Rec *r) {
    int i = d->ncpu++;
    if (i >= DMI_MAX_CPU) return;
    DmiCpu *c = &d->cpu[i];
    str_field(r, 4, c->socket, sizeof c->socket);
    str_field(r, 7, c->manufacturer, sizeof c->manufacturer);
    str_field(r, 0x10, c->version, sizeof c->version);
    c->max_mhz = b16(r, 0x14);
    c->cur_mhz = b16(r, 0x16);
    if (has(r, 0x26, 0)) {                               /* 2.5: core count, cores enabled, thread count, one byte each */
        c->cores = b8(r, 0x23); c->enabled = b8(r, 0x24); c->threads = b8(r, 0x25);
    }
    if (has(r, 0x30, 0)) {                               /* 3.0: the same in 16 bits, for parts with more than 255 */
        if (c->cores == 0xFF) c->cores = b16(r, 0x2A);
        if (c->enabled == 0xFF) c->enabled = b16(r, 0x2C);
        if (c->threads == 0xFF) c->threads = b16(r, 0x2E);
    }
}

static void do_mem(DmiInfo *d, const Rec *r) {
    int i = d->nmem++;
    if (i >= DMI_MAX_MEM) return;
    DmiMem *m = &d->mem[i];
    u16 sz = b16(r, 0x0C);
    if (sz == 0x7FFF && has(r, 0x1C, 4)) m->size_mib = b32(r, 0x1C) & 0x7FFFFFFFu;     /* 32 GiB and more: the extended size field */
    else if (sz == 0 || sz == 0xFFFF) m->size_mib = 0;                                  /* empty slot / not known */
    else if (sz & 0x8000) m->size_mib = (u32)((sz & 0x7FFF) + 1023) / 1024;             /* the field is in KiB */
    else m->size_mib = sz;
    m->form = b8(r, 0x0E);
    str_field(r, 0x10, m->locator, sizeof m->locator);
    str_field(r, 0x11, m->bank, sizeof m->bank);
    m->type = b8(r, 0x12);
    m->speed = has(r, 0x15, 2) ? b16(r, 0x15) : 0;
    str_field(r, 0x17, m->manufacturer, sizeof m->manufacturer);
    str_field(r, 0x18, m->serial, sizeof m->serial);
    str_field(r, 0x1A, m->part, sizeof m->part);
    m->configured = has(r, 0x20, 2) ? b16(r, 0x20) : 0;
}

static void zero(void *p, size_t n) {
    u8 *b = p;
    while (n--) *b++ = 0;
}

void dmi_parse(DmiInfo *d, const u8 *table, u32 len, u16 ver) {
    zero(d, sizeof *d);
    d->bios_major = d->bios_minor = d->ec_major = d->ec_minor = 0xFF;
    d->ver = ver;
    d->bytes = len;
    if (!table || len < 4) return;
    d->ok = true;
    u32 off = 0;
    bool ended = false;
    while (off + 4 <= len && d->structures < 8192) {
        const u8 *h = table + off;
        u8 type = h[0], hlen = h[1];
        if (hlen < 4 || off + hlen > len) { d->cut = true; break; }
        /* the string set: ends at the first pair of NULs (a structure without strings has just those two bytes) */
        const u8 *strs = h + hlen, *lim = table + len, *p = strs;
        while (p + 1 < lim && !(p[0] == 0 && p[1] == 0)) p++;
        Rec r = { h, hlen, strs, p + 1 < lim ? p + 1 : lim };
        if (p + 1 >= lim) d->cut = true;
        d->structures++;
        switch (type) {
        case 0:   do_bios(d, &r); break;
        case 1:   do_system(d, &r); break;
        case 2:   do_board(d, &r); break;
        case 3:   do_chassis(d, &r); break;
        case 4:   do_cpu(d, &r); break;
        case 17:  do_mem(d, &r); break;
        default:  break;
        }
        if (type == 127) { ended = true; break; }
        if (d->cut) break;
        off = (u32)(p + 2 - table);
    }
    if (!ended) d->cut = true;
}

/* ------------------------------------------------------------------ names */

const char *dmi_chassis_name(u8 t) {
    static const char *const n[] = {
        0, "Other", "Unknown", "Desktop", "Low Profile Desktop", "Pizza Box", "Mini Tower", "Tower", "Portable", "Laptop", "Notebook",
        "Hand Held", "Docking Station", "All in One", "Sub Notebook", "Space-saving", "Lunch Box", "Main Server Chassis", "Expansion Chassis",
        "SubChassis", "Bus Expansion Chassis", "Peripheral Chassis", "RAID Chassis", "Rack Mount Chassis", "Sealed-case PC", "Multi-system chassis",
        "Compact PCI", "Advanced TCA", "Blade", "Blade Enclosure", "Tablet", "Convertible", "Detachable", "IoT Gateway", "Embedded PC", "Mini PC",
        "Stick PC",
    };
    return t < sizeof n / sizeof n[0] ? n[t] : 0;
}

const char *dmi_memory_type_name(u8 t) {
    switch (t) {
    case 0x01: return "Other";      case 0x02: return "Unknown";    case 0x03: return "DRAM";       case 0x04: return "EDRAM";
    case 0x05: return "VRAM";       case 0x06: return "SRAM";       case 0x07: return "RAM";        case 0x0F: return "SDRAM";
    case 0x10: return "SGRAM";      case 0x11: return "RDRAM";      case 0x12: return "DDR";        case 0x13: return "DDR2";
    case 0x14: return "DDR2 FB-DIMM"; case 0x18: return "DDR3";     case 0x19: return "FBD2";       case 0x1A: return "DDR4";
    case 0x1B: return "LPDDR";      case 0x1C: return "LPDDR2";     case 0x1D: return "LPDDR3";     case 0x1E: return "LPDDR4";
    case 0x1F: return "Logical non-volatile"; case 0x20: return "HBM"; case 0x21: return "HBM2";    case 0x22: return "DDR5";
    case 0x23: return "LPDDR5";     case 0x24: return "HBM3";
    default: return 0;
    }
}

/* ------------------------------------------------------------------ the text */

typedef struct { char *b; size_t cap, n; } Out;

static void put_ch(Out *o, char c) {
    if (o->n + 1 < o->cap) o->b[o->n++] = c;
    o->b[o->n < o->cap ? o->n : o->cap - 1] = 0;
}

static void put_s(Out *o, const char *s) {
    while (*s) put_ch(o, *s++);
}

static void put_u(Out *o, u64 v) {
    char t[24];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) put_ch(o, t[--n]);
}

static void put_h2(Out *o, u8 v) {
    put_ch(o, "0123456789abcdef"[v >> 4]);
    put_ch(o, "0123456789abcdef"[v & 15]);
}

static void kv(Out *o, const char *key, const char *val) {
    if (!val[0]) return;
    put_s(o, key); put_s(o, ": "); put_s(o, val); put_ch(o, '\n');
}

static void kvn(Out *o, const char *key, u64 v) {
    put_s(o, key); put_s(o, ": "); put_u(o, v); put_ch(o, '\n');
}

/* "cpu.0.socket" style keys */
static void ikey(char *out, const char *pre, int i, const char *post) {
    Out k = { out, 48, 0 };
    k.b[0] = 0;
    put_s(&k, pre); put_u(&k, (u64)i); put_ch(&k, '.'); put_s(&k, post);
}

static void put_uuid(Out *o, const DmiInfo *d) {
    /* DSP0134 2.6 and later: the first three fields are little-endian, so the text is the usual UUID text with those bytes reversed */
    static const u8 le[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    for (int i = 0; i < 16; i++) {
        put_h2(o, d->uuid[d->ver >= 0x0206 ? le[i] : i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) put_ch(o, '-');
    }
}

size_t dmi_format(const DmiInfo *d, char *buf, size_t cap) {
    Out o = { buf, cap, 0 };
    char key[48], t[96];
    if (!cap) return 0;
    buf[0] = 0;
    if (!d->ok) {
        put_s(&o, "smbios: not provided\n");
        return o.n;
    }
    put_s(&o, "smbios: "); put_u(&o, d->ver >> 8); put_ch(&o, '.'); put_u(&o, d->ver & 0xFF);
    put_s(&o, d->kind == 3 ? " (64-bit entry point), " : " (32-bit entry point), ");
    put_u(&o, d->structures); put_s(&o, " structures, "); put_u(&o, d->bytes); put_s(&o, " bytes");
    if (d->flags & 1) put_s(&o, ", entry point checksum bad (used anyway)");
    if (d->cut) put_s(&o, ", table cut short");
    put_ch(&o, '\n');
    kv(&o, "bios.vendor", d->bios_vendor);
    kv(&o, "bios.version", d->bios_version);
    kv(&o, "bios.date", d->bios_date);
    if (d->bios_major != 0xFF) { Out r = { t, sizeof t, 0 }; t[0] = 0; put_u(&r, d->bios_major); put_ch(&r, '.'); put_u(&r, d->bios_minor); kv(&o, "bios.release", t); }
    if (d->ec_major != 0xFF) { Out r = { t, sizeof t, 0 }; t[0] = 0; put_u(&r, d->ec_major); put_ch(&r, '.'); put_u(&r, d->ec_minor); kv(&o, "bios.ec_release", t); }
    kv(&o, "system.manufacturer", d->sys_manufacturer);
    kv(&o, "system.product", d->sys_product);
    kv(&o, "system.version", d->sys_version);
    kv(&o, "system.serial", d->sys_serial);
    if (d->have_uuid) { Out r = { t, sizeof t, 0 }; t[0] = 0; put_uuid(&r, d); kv(&o, "system.uuid", t); }
    kv(&o, "system.sku", d->sys_sku);
    kv(&o, "system.family", d->sys_family);
    kv(&o, "baseboard.manufacturer", d->bb_manufacturer);
    kv(&o, "baseboard.product", d->bb_product);
    kv(&o, "baseboard.version", d->bb_version);
    kv(&o, "baseboard.serial", d->bb_serial);
    kv(&o, "baseboard.asset", d->bb_asset);
    if (d->have_chassis) {
        Out r = { t, sizeof t, 0 };
        const char *nm = dmi_chassis_name(d->chassis_type);
        t[0] = 0;
        put_u(&r, d->chassis_type);
        if (nm) { put_s(&r, " ("); put_s(&r, nm); put_ch(&r, ')'); }
        kv(&o, "chassis.type", t);
        kv(&o, "chassis.manufacturer", d->ch_manufacturer);
        kv(&o, "chassis.version", d->ch_version);
        kv(&o, "chassis.serial", d->ch_serial);
        kv(&o, "chassis.asset", d->ch_asset);
    }
    kvn(&o, "processor.count", (u64)d->ncpu);
    for (int i = 0; i < d->ncpu && i < DMI_MAX_CPU; i++) {
        const DmiCpu *c = &d->cpu[i];
        ikey(key, "processor.", i, "socket"); kv(&o, key, c->socket);
        ikey(key, "processor.", i, "manufacturer"); kv(&o, key, c->manufacturer);
        ikey(key, "processor.", i, "version"); kv(&o, key, c->version);
        if (c->max_mhz) { ikey(key, "processor.", i, "max_mhz"); kvn(&o, key, c->max_mhz); }
        if (c->cur_mhz) { ikey(key, "processor.", i, "current_mhz"); kvn(&o, key, c->cur_mhz); }
        if (c->cores) { ikey(key, "processor.", i, "cores"); kvn(&o, key, c->cores); }
        if (c->threads) { ikey(key, "processor.", i, "threads"); kvn(&o, key, c->threads); }
    }
    u64 filled = 0;
    for (int i = 0; i < d->nmem && i < DMI_MAX_MEM; i++) if (d->mem[i].size_mib) filled++;
    put_s(&o, "memory.slots: "); put_u(&o, (u64)d->nmem); put_s(&o, " ("); put_u(&o, filled); put_s(&o, " filled)\n");
    for (int i = 0; i < d->nmem && i < DMI_MAX_MEM; i++) {
        const DmiMem *m = &d->mem[i];
        ikey(key, "memory.", i, "locator"); kv(&o, key, m->locator);
        ikey(key, "memory.", i, "size_mib"); kvn(&o, key, m->size_mib);
        if (!m->size_mib) continue;
        const char *tn = dmi_memory_type_name(m->type);
        if (tn) { ikey(key, "memory.", i, "type"); kv(&o, key, tn); }
        if (m->speed) { ikey(key, "memory.", i, "speed_mts"); kvn(&o, key, m->speed); }
        if (m->configured) { ikey(key, "memory.", i, "configured_mts"); kvn(&o, key, m->configured); }
        ikey(key, "memory.", i, "manufacturer"); kv(&o, key, m->manufacturer);
        ikey(key, "memory.", i, "part"); kv(&o, key, m->part);
        ikey(key, "memory.", i, "serial"); kv(&o, key, m->serial);
    }
    return o.n;
}
