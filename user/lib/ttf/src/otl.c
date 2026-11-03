/*
 * Kerning and ligatures: the 'kern' table (format 0, both the Microsoft and the Apple header), the GPOS pair adjustment lookup
 * (type 2, formats 1 and 2, reached directly or through an Extension lookup) and the GSUB ligature lookup (type 4, again directly
 * or through an Extension). OpenType specification, "GPOS", "GSUB" and "Common Table Formats"; Apple's 'kern' chapter.
 *
 * This is deliberately the small, useful corner of OpenType Layout: horizontal pair kerning and 'liga' for Latin-style text.
 * The script is picked once (Latin, else DFLT, else the first one) and its default language system names the features;
 * contextual lookups, mark positioning, other substitution types and language-specific features are not applied.
 * When a font has both a GPOS 'kern' feature and a 'kern' table, GPOS wins.
 */
#include "ttfint.h"

/* ---------------------------------------------------------------- coverage and class definition tables */

static int coverage_index(Span cov, uint32_t gid) {
    uint32_t fmt = sp_u16(cov, 0), count = sp_u16(cov, 2);
    uint32_t lo = 0, hi = count;
    if (fmt == 1) {
        if (!sp_has(cov, 4, 2u * count)) return -1;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2, v = sp_u16(cov, 4 + 2u * mid);
            if (gid == v) return (int)mid;
            if (gid < v) hi = mid; else lo = mid + 1;
        }
    } else if (fmt == 2) {
        if (!sp_has(cov, 4, 6u * count)) return -1;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            size_t r = 4 + 6u * mid;
            uint32_t start = sp_u16(cov, r), end = sp_u16(cov, r + 2);
            if (gid < start) hi = mid;
            else if (gid > end) lo = mid + 1;
            else return (int)(sp_u16(cov, r + 4) + (gid - start));
        }
    }
    return -1;
}

static uint32_t class_of(Span cd, uint32_t gid) {
    uint32_t fmt = sp_u16(cd, 0);
    if (fmt == 1) {
        uint32_t start = sp_u16(cd, 2), count = sp_u16(cd, 4);
        return gid >= start && gid - start < count ? sp_u16(cd, 6 + 2u * (gid - start)) : 0;
    }
    if (fmt == 2) {
        uint32_t count = sp_u16(cd, 2), lo = 0, hi = count;
        if (!sp_has(cd, 4, 6u * count)) return 0;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            size_t r = 4 + 6u * mid;
            uint32_t start = sp_u16(cd, r), end = sp_u16(cd, r + 2);
            if (gid < start) hi = mid;
            else if (gid > end) lo = mid + 1;
            else return sp_u16(cd, r + 4);
        }
    }
    return 0;                                                       /* glyphs not listed are in class 0 */
}

/* ---------------------------------------------------------------- finding the lookups of a feature */

static void add_lookup(Span t, uint32_t lookup_list, uint32_t idx, uint32_t *out, int *n, int max) {
    if (idx >= sp_u16(t, lookup_list)) return;
    uint32_t off = lookup_list + sp_u16(t, lookup_list + 2 + 2u * idx);
    if (!sp_has(t, off, 6)) return;
    for (int i = 0; i < *n; i++) if (out[i] == off) return;         /* the same lookup reached twice is applied once */
    if (*n < max) out[(*n)++] = off;
}

static void add_feature(Span t, uint32_t feature_list, uint32_t lookup_list, uint32_t idx, uint32_t tag, uint32_t *out, int *n, int max) {
    if (idx >= sp_u16(t, feature_list)) return;
    size_t rec = feature_list + 2 + 6u * idx;
    if (sp_u32(t, rec) != tag) return;
    size_t ft = (size_t)feature_list + sp_u16(t, rec + 4);
    uint32_t nl = sp_u16(t, ft + 2);
    for (uint32_t k = 0; k < nl && k < 64; k++) add_lookup(t, lookup_list, sp_u16(t, ft + 4 + 2u * k), out, n, max);
}

static int find_lookups(Span t, uint32_t tag, uint32_t *out, int max) {
    int n = 0;
    if (t.n < 10 || sp_u16(t, 0) != 1) return 0;
    uint32_t script_list = sp_u16(t, 4), feature_list = sp_u16(t, 6), lookup_list = sp_u16(t, 8);
    if (!script_list || !feature_list || !lookup_list) return 0;
    uint32_t scount = sp_u16(t, script_list);
    size_t script = 0;
    int rank = 0;
    for (uint32_t i = 0; i < scount && i < 64; i++) {
        size_t r = script_list + 2 + 6u * i;
        uint32_t stag = sp_u32(t, r);
        int rk = stag == TAG('l', 'a', 't', 'n') ? 3 : stag == TAG('D', 'F', 'L', 'T') ? 2 : 1;
        if (rk > rank) { rank = rk; script = (size_t)script_list + sp_u16(t, r + 4); }
    }
    if (rank) {
        size_t ls = 0;
        if (sp_u16(t, script)) ls = script + sp_u16(t, script);                      /* the default language system */
        else if (sp_u16(t, script + 2)) ls = script + sp_u16(t, script + 4 + 4);     /* else the first listed one */
        if (ls) {
            uint32_t nf = sp_u16(t, ls + 4);
            for (uint32_t k = 0; k < nf && k < 256; k++)
                add_feature(t, feature_list, lookup_list, sp_u16(t, ls + 6 + 2u * k), tag, out, &n, max);
        }
    }
    if (n == 0) {                                                   /* no usable script: take every feature of that name */
        uint32_t nf = sp_u16(t, feature_list);
        for (uint32_t i = 0; i < nf && i < 512; i++) add_feature(t, feature_list, lookup_list, i, tag, out, &n, max);
    }
    return n;
}

/* ---------------------------------------------------------------- the 'kern' table */

static void open_kern(TtfFont *f) {
    Span k = sp_table(f, f->kern);
    f->nkern = 0;
    if (k.n < 4) return;
    size_t pos;
    uint32_t n;
    int apple = 0;
    if (sp_u16(k, 0) == 0) { n = sp_u16(k, 2); pos = 4; }
    else if (sp_u32(k, 0) == 0x00010000u) { n = sp_u32(k, 4); pos = 8; apple = 1; }
    else return;
    for (uint32_t i = 0; i < n && i < 64 && f->nkern < TTF_MAX_KERN_TABLES; i++) {
        size_t hdr = apple ? 8 : 6;
        if (!sp_has(k, pos, hdr)) break;
        uint32_t length, cov;
        int horizontal, format, cross, override_flag;
        if (apple) {
            length = sp_u32(k, pos);
            cov = sp_u16(k, pos + 4);
            format = (int)(cov & 0xFF);
            horizontal = !(cov & 0x8000);
            cross = (cov & 0x4000) != 0;
            override_flag = 0;
        } else {
            length = sp_u16(k, pos + 2);
            cov = sp_u16(k, pos + 4);
            format = (int)(cov >> 8);
            horizontal = (cov & 1) != 0;
            cross = (cov & 4) != 0;
            override_flag = (cov & 8) != 0;
        }
        if (format == 0 && horizontal && !cross && sp_has(k, pos + hdr, 8)) {
            f->kern_off[f->nkern] = (uint32_t)(pos + hdr);
            f->kern_override[f->nkern] = (uint8_t)override_flag;
            f->nkern++;
        }
        if (n == 1) break;                                         /* one subtable: its length field is not needed (and overflows in big fonts) */
        if (length < hdr) break;
        pos += length;
    }
}

static int kern_table_value(const TtfFont *f, uint32_t l, uint32_t r) {
    Span k = sp_table(f, f->kern);
    uint32_t key = (l << 16) | r;
    int total = 0;
    for (int t = 0; t < f->nkern; t++) {
        size_t body = f->kern_off[t];
        uint32_t n = sp_u16(k, body);
        size_t avail = k.n > body + 8 ? (k.n - body - 8) / 6 : 0;
        if (n > avail) n = (uint32_t)avail;                        /* an overstated pair count: use what is there */
        uint32_t lo = 0, hi = n;
        int found = 0, v = 0;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            size_t p = body + 8 + 6u * (size_t)mid;
            uint32_t pk = (sp_u16(k, p) << 16) | sp_u16(k, p + 2);
            if (pk == key) { found = 1; v = sp_i16(k, p + 4); break; }
            if (key < pk) hi = mid; else lo = mid + 1;
        }
        if (found) total = f->kern_override[t] ? v : total + v;
    }
    return total;
}

/* ---------------------------------------------------------------- GPOS pair adjustment */

static size_t vrec_size(uint32_t fmt) {
    size_t n = 0;
    for (uint32_t b = fmt & 0xFFu; b; b >>= 1) n += b & 1;
    return 2 * n;
}

/* The XAdvance of a value record, 0 if the format has none. */
static int vrec_xadvance(Span s, size_t off, uint32_t fmt) {
    if (!(fmt & 4)) return 0;
    size_t o = off + ((fmt & 1) ? 2 : 0) + ((fmt & 2) ? 2 : 0);
    return sp_i16(s, o);
}

/* One PairPos subtable. *found says whether the subtable decided this pair (so that later subtables are not consulted). */
static int pairpos(Span sub, uint32_t l, uint32_t r, int *found) {
    uint32_t fmt = sp_u16(sub, 0);
    int ci = coverage_index(sp_from(sub, sp_u16(sub, 2)), l);
    if (ci < 0 || (fmt != 1 && fmt != 2)) return 0;
    uint32_t vf1 = sp_u16(sub, 4), vf2 = sp_u16(sub, 6);
    size_t s1 = vrec_size(vf1), s2 = vrec_size(vf2);
    if (fmt == 1) {
        if ((uint32_t)ci >= sp_u16(sub, 8)) return 0;
        Span ps = sp_from(sub, sp_u16(sub, 10 + 2u * (uint32_t)ci));
        size_t rec = 2 + s1 + s2;
        uint32_t n = sp_u16(ps, 0);
        size_t avail = ps.n >= 2 ? (ps.n - 2) / rec : 0;
        if (n > avail) n = (uint32_t)avail;
        uint32_t lo = 0, hi = n;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            size_t p = 2 + rec * mid;
            uint32_t second = sp_u16(ps, p);
            if (second == r) { *found = 1; return vrec_xadvance(ps, p + 2, vf1); }
            if (r < second) hi = mid; else lo = mid + 1;
        }
        return 0;                                                   /* not in this pair set: a later subtable may know it */
    }
    uint32_t n1 = sp_u16(sub, 12), n2 = sp_u16(sub, 14);
    uint32_t c1 = class_of(sp_from(sub, sp_u16(sub, 8)), l), c2 = class_of(sp_from(sub, sp_u16(sub, 10)), r);
    *found = 1;                                                     /* the first glyph is covered: this subtable answers, even with 0 */
    if (c1 >= n1 || c2 >= n2) return 0;
    return vrec_xadvance(sub, 16 + ((size_t)c1 * n2 + c2) * (s1 + s2), vf1);
}

static int gpos_pair(const TtfFont *f, uint32_t l, uint32_t r) {
    Span g = sp_table(f, f->gpos);
    int total = 0;
    for (int i = 0; i < f->ngpos_kern; i++) {
        Span lk = sp_from(g, f->gpos_kern[i]);
        uint32_t type = sp_u16(lk, 0), nsub = sp_u16(lk, 4);
        for (uint32_t s = 0; s < nsub && s < 256; s++) {
            Span sub = sp_from(lk, sp_u16(lk, 6 + 2u * s));
            uint32_t t = type;
            if (t == 9) {                                           /* Extension: the real subtable is elsewhere */
                if (sp_u16(sub, 0) != 1) continue;
                t = sp_u16(sub, 2);
                sub = sp_from(sub, sp_u32(sub, 4));
            }
            if (t != 2) continue;
            int found = 0;
            int v = pairpos(sub, l, r, &found);
            if (found) { total += v; break; }
        }
    }
    return total;
}

int ttf_kern_units(const TtfFont *f, uint32_t left, uint32_t right) {
    if (left >= (uint32_t)f->num_glyphs || right >= (uint32_t)f->num_glyphs) return 0;
    if (f->ngpos_kern) return gpos_pair(f, left, right);
    if (f->nkern) return kern_table_value(f, left, right);
    return 0;
}

/* ---------------------------------------------------------------- GSUB ligatures */

int ttf_ligature(const TtfFont *f, const uint32_t *glyphs, size_t n, uint32_t *out) {
    if (!f->ngsub_liga || n < 2) return 0;
    Span g = sp_table(f, f->gsub);
    for (int i = 0; i < f->ngsub_liga; i++) {
        Span lk = sp_from(g, f->gsub_liga[i]);
        uint32_t type = sp_u16(lk, 0), nsub = sp_u16(lk, 4);
        for (uint32_t s = 0; s < nsub && s < 256; s++) {
            Span sub = sp_from(lk, sp_u16(lk, 6 + 2u * s));
            uint32_t t = type;
            if (t == 7) {
                if (sp_u16(sub, 0) != 1) continue;
                t = sp_u16(sub, 2);
                sub = sp_from(sub, sp_u32(sub, 4));
            }
            if (t != 4 || sp_u16(sub, 0) != 1) continue;
            int ci = coverage_index(sp_from(sub, sp_u16(sub, 2)), glyphs[0]);
            if (ci < 0 || (uint32_t)ci >= sp_u16(sub, 4)) continue;
            Span set = sp_from(sub, sp_u16(sub, 6 + 2u * (uint32_t)ci));
            uint32_t nlig = sp_u16(set, 0);
            for (uint32_t k = 0; k < nlig && k < 512; k++) {         /* in the font's order: the first that matches wins */
                Span lig = sp_from(set, sp_u16(set, 2 + 2u * k));
                uint32_t ncomp = sp_u16(lig, 2);                      /* counts the first glyph too */
                if (ncomp < 2 || ncomp > n || !sp_has(lig, 4, 2u * (ncomp - 1))) continue;
                uint32_t j = 1;
                while (j < ncomp && sp_u16(lig, 4 + 2u * (j - 1)) == glyphs[j]) j++;
                if (j == ncomp) { *out = sp_u16(lig, 0); return (int)ncomp; }
            }
        }
    }
    return 0;
}

void ttfi_otl_open(TtfFont *f) {
    f->nkern = 0;
    f->ngpos_kern = f->ngsub_liga = 0;
    if (f->kern.off) open_kern(f);
    if (f->gpos.off) f->ngpos_kern = find_lookups(sp_table(f, f->gpos), TAG('k', 'e', 'r', 'n'), f->gpos_kern, TTF_MAX_LOOKUPS);
    if (f->gsub.off) f->ngsub_liga = find_lookups(sp_table(f, f->gsub), TAG('l', 'i', 'g', 'a'), f->gsub_liga, TTF_MAX_LOOKUPS);
}
