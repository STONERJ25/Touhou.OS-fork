/*
 * Opening a font: the sfnt table directory (OpenType specification, "Organization of an OpenType Font"), TrueType collections,
 * and the small fixed-layout tables head, hhea, maxp, hmtx, OS/2, post and name.
 *
 * ttf_open validates only what every later call relies on (the required tables exist and are long enough, the glyph count fits the
 * loca table, the metrics tables hold what they claim); nothing is allocated. Table lengths are clamped to the file, because a
 * length that points past the end is a common fault of real fonts and the data before the end is still good.
 */
#include "ttfint.h"

static uint32_t g_next_font_id;

const char *ttf_strerror(int rc) {
    switch (rc) {
    case TTF_OK: return "ok";
    case TTF_E_FORMAT: return "not a TrueType/OpenType font";
    case TTF_E_TRUNC: return "the font file is cut short";
    case TTF_E_CORRUPT: return "the font's tables contradict themselves";
    case TTF_E_UNSUPPORTED: return "a kind of font this library cannot use";
    case TTF_E_NOMEM: return "out of memory";
    case TTF_E_TOOBIG: return "glyph or bitmap too large";
    case TTF_E_ARG: return "bad argument";
    case TTF_E_GLYPH: return "bad glyph";
    default: return "unknown error";
    }
}

static int sfnt_tag_ok(uint32_t t) { return t == 0x00010000u || t == TAG('O', 'T', 'T', 'O') || t == TAG('t', 'r', 'u', 'e'); }

int ttf_face_count(const void *data, size_t len) {
    if (!data || len < 12) return 0;
    Span s = sp_make((const uint8_t *)data, len);
    uint32_t tag = sp_u32(s, 0);
    if (tag == TAG('t', 't', 'c', 'f')) {
        uint32_t n = sp_u32(s, 8);
        return n >= 1 && n <= TTF_MAX_FACES ? (int)n : 0;
    }
    return sfnt_tag_ok(tag) ? 1 : 0;
}

static TtfTable *table_slot(TtfFont *f, uint32_t tag) {
    switch (tag) {
    case TAG('h', 'e', 'a', 'd'): return &f->head;
    case TAG('h', 'h', 'e', 'a'): return &f->hhea;
    case TAG('h', 'm', 't', 'x'): return &f->hmtx;
    case TAG('m', 'a', 'x', 'p'): return &f->maxp;
    case TAG('l', 'o', 'c', 'a'): return &f->loca;
    case TAG('g', 'l', 'y', 'f'): return &f->glyf;
    case TAG('c', 'm', 'a', 'p'): return &f->cmap;
    case TAG('n', 'a', 'm', 'e'): return &f->name;
    case TAG('O', 'S', '/', '2'): return &f->os2;
    case TAG('p', 'o', 's', 't'): return &f->post;
    case TAG('k', 'e', 'r', 'n'): return &f->kern;
    case TAG('G', 'P', 'O', 'S'): return &f->gpos;
    case TAG('G', 'S', 'U', 'B'): return &f->gsub;
    case TAG('C', 'F', 'F', ' '): return &f->cff;
    default: return NULL;
    }
}

int ttf_open(TtfFont *f, const void *data, size_t len, int face_index) {
    if (!f) return TTF_E_ARG;
    memset(f, 0, sizeof *f);
    if (!data) return TTF_E_ARG;
    Span s = sp_make((const uint8_t *)data, len);
    int nfaces = ttf_face_count(data, len);
    if (nfaces <= 0) return len < 12 && len >= 4 && sfnt_tag_ok(sp_u32(s, 0)) ? TTF_E_TRUNC : TTF_E_FORMAT;
    if (face_index < 0 || face_index >= nfaces) return TTF_E_FORMAT;

    size_t dir = 0;
    if (sp_u32(s, 0) == TAG('t', 't', 'c', 'f')) {
        if (!sp_has(s, 12, 4u * (uint32_t)nfaces)) return TTF_E_TRUNC;
        dir = sp_u32(s, 12 + 4u * (uint32_t)face_index);
        if (!sp_has(s, dir, 12) || !sfnt_tag_ok(sp_u32(s, dir))) return TTF_E_FORMAT;
    }
    uint32_t sfnt = sp_u32(s, dir);
    uint32_t ntab = sp_u16(s, dir + 4);
    if (!sp_has(s, dir + 12, 16u * ntab)) return TTF_E_TRUNC;

    for (uint32_t i = 0; i < ntab; i++) {
        size_t o = dir + 12 + 16u * i;
        uint32_t tag = sp_u32(s, o), off = sp_u32(s, o + 8), tlen = sp_u32(s, o + 12);
        TtfTable *t = table_slot(f, tag);
        if (!t || t->off != 0) continue;                           /* not one we use, or a duplicate: the first wins */
        if (off < 12 || off >= len) continue;                      /* starts inside the header or past the end: treat as absent */
        if (tlen > len - off) tlen = (uint32_t)(len - off);
        t->off = off;
        t->len = tlen;
    }
    f->data = (const uint8_t *)data;
    f->size = len;
    f->face_index = face_index;

    if (!f->head.off || !f->hhea.off || !f->maxp.off || !f->hmtx.off) return TTF_E_CORRUPT;

    Span head = sp_table(f, f->head);
    if (head.n < 54) return TTF_E_TRUNC;
    if (sp_u32(head, 12) != 0x5F0F3CF5u) return TTF_E_CORRUPT;
    f->units_per_em = (int)sp_u16(head, 18);
    if (f->units_per_em < 16 || f->units_per_em > 16384) return TTF_E_CORRUPT;
    for (int i = 0; i < 4; i++) f->bbox[i] = sp_i16(head, 36 + 2u * (uint32_t)i);
    uint32_t mac_style = sp_u16(head, 44);
    f->loca_long = sp_i16(head, 50);
    if (f->loca_long != 0 && f->loca_long != 1) return TTF_E_CORRUPT;

    Span maxp = sp_table(f, f->maxp);
    if (maxp.n < 6) return TTF_E_TRUNC;
    f->num_glyphs = (int)sp_u16(maxp, 4);

    Span hhea = sp_table(f, f->hhea);
    if (hhea.n < 36) return TTF_E_TRUNC;
    f->hhea_ascent = sp_i16(hhea, 4);
    f->hhea_descent = sp_i16(hhea, 6);
    f->hhea_line_gap = sp_i16(hhea, 8);
    f->num_hmetrics = (int)sp_u16(hhea, 34);
    if (f->num_hmetrics > (int)(f->hmtx.len / 4)) f->num_hmetrics = (int)(f->hmtx.len / 4);       /* a table shorter than it says */
    if (f->num_hmetrics < 1) return TTF_E_CORRUPT;

    /* Where the outlines are. 'OTTO' fonts have CFF; others have glyf. */
    if (f->cff.off && (sfnt == TAG('O', 'T', 'T', 'O') || !(f->glyf.off && f->loca.off))) {
        f->is_cff = 1;
    } else if (f->glyf.off && f->loca.off) {
        f->is_cff = 0;
    } else {
        return TTF_E_UNSUPPORTED;                                  /* bitmap-only fonts, or CFF2 (variable) fonts: no 'glyf' and no 'CFF ' */
    }
    if (f->is_cff) {
        int rc = ttfi_cff_open(f);
        if (rc) return rc;
        if (f->num_glyphs > (int)f->cff_charstrings_count) f->num_glyphs = (int)f->cff_charstrings_count;
    } else {
        size_t entry = f->loca_long ? 4u : 2u;
        size_t fit = f->loca.len / entry;                           /* loca has num_glyphs + 1 entries */
        if (fit == 0 || (size_t)f->num_glyphs + 1 > fit) f->num_glyphs = (int)(fit ? fit - 1 : 0);
    }
    if (f->num_glyphs < 1) return TTF_E_CORRUPT;

    /* OS/2 */
    f->weight_class = 400;
    f->width_class = 5;
    if (f->os2.off) {
        Span o = sp_table(f, f->os2);
        if (o.n >= 78) {
            f->has_os2 = 1;
            f->weight_class = (int)sp_u16(o, 4);
            f->width_class = (int)sp_u16(o, 6);
            f->strike_thick = sp_i16(o, 26);
            f->strike_pos = sp_i16(o, 28);
            uint32_t sel = sp_u16(o, 62);
            f->use_typo_metrics = (sel >> 7) & 1;
            f->typo_ascent = sp_i16(o, 68);
            f->typo_descent = sp_i16(o, 70);
            f->typo_line_gap = sp_i16(o, 72);
            f->win_ascent = (int)sp_u16(o, 74);
            f->win_descent = (int)sp_u16(o, 76);
            if (sp_u16(o, 0) >= 2 && o.n >= 90) {
                f->x_height = sp_i16(o, 86);
                f->cap_height = sp_i16(o, 88);
            }
            f->is_italic = (int)(sel & 1);
            f->is_bold = (int)((sel >> 5) & 1);
        }
    }
    if (mac_style & 2) f->is_italic = 1;
    if (mac_style & 1) f->is_bold = 1;
    if (f->weight_class >= 600) f->is_bold = 1;

    if (f->post.off) {
        Span p = sp_table(f, f->post);
        if (p.n >= 16) {
            f->underline_pos = sp_i16(p, 8);
            f->underline_thick = sp_i16(p, 10);
            f->is_fixed_pitch = sp_u32(p, 12) != 0;
        }
    }

    ttfi_cmap_select(f);
    ttfi_otl_open(f);
    f->id = __atomic_add_fetch(&g_next_font_id, 1, __ATOMIC_RELAXED);
    return TTF_OK;
}

/* ---------------------------------------------------------------- metrics */

void ttf_metrics(const TtfFont *f, float size_px, TtfMetrics *m) {
    float s = ttf_scale(f, size_px);
    int asc, desc, gap;
    if (f->has_os2 && f->use_typo_metrics && (f->typo_ascent || f->typo_descent)) {
        asc = f->typo_ascent; desc = -f->typo_descent; gap = f->typo_line_gap;
    } else if (f->hhea_ascent || f->hhea_descent) {
        asc = f->hhea_ascent; desc = -f->hhea_descent; gap = f->hhea_line_gap;
    } else if (f->has_os2 && (f->win_ascent || f->win_descent)) {
        asc = f->win_ascent; desc = f->win_descent; gap = 0;
    } else if (f->has_os2 && (f->typo_ascent || f->typo_descent)) {
        asc = f->typo_ascent; desc = -f->typo_descent; gap = f->typo_line_gap;
    } else {
        asc = f->bbox[3]; desc = -f->bbox[1]; gap = 0;
    }
    if (gap < 0) gap = 0;
    m->ascent = (float)asc * s;
    m->descent = (float)desc * s;
    m->line_gap = (float)gap * s;
    m->line_height = m->ascent + m->descent + m->line_gap;
    m->underline_pos = (float)f->underline_pos * s;
    m->underline_thick = (float)f->underline_thick * s;
    m->strike_pos = (float)f->strike_pos * s;
    m->strike_thick = (float)f->strike_thick * s;
    m->x_height = (float)f->x_height * s;
    m->cap_height = (float)f->cap_height * s;
}

int ttf_advance_units(const TtfFont *f, uint32_t g) {
    if (g >= (uint32_t)f->num_glyphs) return 0;
    uint32_t i = g < (uint32_t)f->num_hmetrics ? g : (uint32_t)f->num_hmetrics - 1;     /* glyphs past the long list repeat the last advance */
    return (int)sp_u16(sp_table(f, f->hmtx), 4u * i);
}

int ttf_lsb_units(const TtfFont *f, uint32_t g) {
    if (g >= (uint32_t)f->num_glyphs) return 0;
    Span h = sp_table(f, f->hmtx);
    if (g < (uint32_t)f->num_hmetrics) return sp_i16(h, 4u * g + 2);
    return sp_i16(h, 4u * (uint32_t)f->num_hmetrics + 2u * (g - (uint32_t)f->num_hmetrics));
}

/* ---------------------------------------------------------------- names */

/* Rank of a name record for picking: English Windows entries first, then any Windows Unicode entry, Unicode-platform entries, and
 * Macintosh Roman English. -1: not a record we can decode. */
static int name_rank(uint32_t plat, uint32_t enc, uint32_t lang) {
    if (plat == 3 && (enc == 1 || enc == 10 || enc == 0)) return (lang & 0xFF) == 0x09 ? 100 : 80;
    if (plat == 0) return 70;
    if (plat == 1 && enc == 0) return lang == 0 ? 50 : 20;
    return -1;
}

int ttf_name(const TtfFont *f, int name_id, char *buf, size_t cap) {
    if (cap) buf[0] = 0;
    Span n = sp_table(f, f->name);
    if (n.n < 6) return -1;
    uint32_t count = sp_u16(n, 2), str = sp_u16(n, 4);
    if (!sp_has(n, 6, (size_t)count * 12)) count = (uint32_t)((n.n - 6) / 12);
    int best = -1, best_rank = -1;
    for (uint32_t i = 0; i < count; i++) {
        size_t r = 6 + 12u * i;
        if ((int)sp_u16(n, r + 6) != name_id) continue;
        uint32_t plat = sp_u16(n, r), enc = sp_u16(n, r + 2), lang = sp_u16(n, r + 4);
        int rank = name_rank(plat, enc, lang);
        if (rank <= best_rank) continue;
        if (!sp_has(n, (size_t)str + sp_u16(n, r + 10), sp_u16(n, r + 8))) continue;
        best = (int)i;
        best_rank = rank;
    }
    if (best < 0) return -1;
    size_t r = 6 + 12u * (uint32_t)best;
    uint32_t plat = sp_u16(n, r), slen = sp_u16(n, r + 8);
    Span sv = sp_from(n, (size_t)str + sp_u16(n, r + 10));
    size_t total = 0, pos = 0;
    int stopped = 0;
    for (uint32_t i = 0; i < slen;) {
        uint32_t cp;
        if (plat == 1) {                                            /* Mac Roman: ASCII is exact, the rest becomes '?' */
            uint32_t b = sp_u8(sv, i++);
            cp = b < 0x80 ? b : '?';
        } else {
            if (slen - i < 2) break;
            uint32_t u = sp_u16(sv, i);
            i += 2;
            cp = u;
            if (u >= 0xD800 && u < 0xDC00) {
                uint32_t lo = slen - i >= 2 ? sp_u16(sv, i) : 0;
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
                else cp = 0xFFFD;
            } else if (u >= 0xDC00 && u < 0xE000) cp = 0xFFFD;
        }
        char tmp[4];
        int k = ttf_utf8_encode(cp, tmp);
        total += (size_t)k;
        if (!stopped && cap && pos + (size_t)k < cap) { memcpy(buf + pos, tmp, (size_t)k); pos += (size_t)k; }
        else stopped = 1;                                           /* once one does not fit, nothing after it is written either */
    }
    if (cap) buf[pos] = 0;
    return (int)total;
}
