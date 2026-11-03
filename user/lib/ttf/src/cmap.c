/*
 * Character to glyph mapping: the 'cmap' table (OpenType specification, "cmap"). Formats 0 (byte table), 4 (segments, the BMP
 * workhorse), 6 (trimmed table), 12 (segmented coverage of all of Unicode) and 13 (many characters, one glyph) are read; format 14
 * (variation selectors) and the legacy formats are skipped, so a font that has them next to a usable table opens normally.
 *
 * One subtable is chosen when the font is opened: the best Unicode one by platform and encoding (full repertoire before BMP-only,
 * Windows before the Unicode platform's older encodings), then a Windows symbol table, then Macintosh Roman restricted to ASCII.
 */
#include "ttfint.h"

/* Is the subtable at `off` one we can read, and entirely inside the table? 0 if so, -1 if not. The `length` fields of subtables are
 * not trusted: real fonts get them wrong, and the table's own end is what bounds every read. */
static int check_format(Span cm, size_t off) {
    uint32_t fmt = sp_u16(cm, off);
    switch (fmt) {
    case 0:
        return sp_has(cm, off, 6 + 256) ? 0 : -1;
    case 4: {
        uint32_t x2 = sp_u16(cm, off + 6);
        if (x2 == 0 || (x2 & 1)) return -1;
        return sp_has(cm, off, 16 + 4u * x2) ? 0 : -1;               /* 14 + 4 arrays of segCount + the pad word */
    }
    case 6:
        return sp_has(cm, off, 10 + 2u * sp_u16(cm, off + 8)) ? 0 : -1;
    case 12:
    case 13:
        return sp_has(cm, off, 16 + 12u * (size_t)sp_u32(cm, off + 12)) ? 0 : -1;
    default:
        return -1;
    }
}

/* Preference of a (platform, encoding) pair; 0 = never use. */
static int score(uint32_t plat, uint32_t enc) {
    if (plat == 3 && enc == 10) return 100;
    if (plat == 0 && (enc == 4 || enc == 6)) return 95;
    if (plat == 3 && enc == 1) return 80;
    if (plat == 0 && enc <= 3) return 70;
    if (plat == 3 && enc == 0) return 40;
    if (plat == 1 && enc == 0) return 20;
    return 0;
}

void ttfi_cmap_select(TtfFont *f) {
    f->cmap_off = 0;
    f->cmap_format = 0;
    f->cmap_symbol = 0;
    if (!f->cmap.off) return;
    Span cm = sp_table(f, f->cmap);
    uint32_t n = sp_u16(cm, 2);
    int best = 0;
    for (uint32_t i = 0; i < n; i++) {
        size_t r = 4 + 8u * i;
        if (!sp_has(cm, r, 8)) break;
        uint32_t plat = sp_u16(cm, r), enc = sp_u16(cm, r + 2), off = sp_u32(cm, r + 4);
        int sc = score(plat, enc);
        if (sc <= best || off >= cm.n || check_format(cm, off) < 0) continue;
        best = sc;
        f->cmap_off = off;
        f->cmap_format = (int)sp_u16(cm, off);
        f->cmap_symbol = plat == 3 && enc == 0 ? 1 : plat == 1 ? 2 : 0;
    }
}

static uint32_t lookup(const TtfFont *f, uint32_t cp) {
    Span cm = sp_table(f, f->cmap);
    size_t o = f->cmap_off;
    switch (f->cmap_format) {
    case 0:
        return cp < 256 ? sp_u8(cm, o + 6 + cp) : 0;
    case 4: {
        if (cp > 0xFFFF) return 0;
        uint32_t segs = sp_u16(cm, o + 6) / 2;
        size_t end_o = o + 14, start_o = o + 16 + 2u * segs, delta_o = o + 16 + 4u * segs, range_o = o + 16 + 6u * segs;
        uint32_t lo = 0, hi = segs;                                  /* the first segment whose end code is >= cp */
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (sp_u16(cm, end_o + 2u * mid) < cp) lo = mid + 1; else hi = mid;
        }
        if (lo >= segs) return 0;
        uint32_t start = sp_u16(cm, start_o + 2u * lo);
        if (cp < start) return 0;
        uint32_t delta = sp_u16(cm, delta_o + 2u * lo), ro = sp_u16(cm, range_o + 2u * lo);
        if (ro == 0) return (cp + delta) & 0xFFFF;
        /* idRangeOffset counts bytes from its own position to the glyph index array entry for `start` */
        uint32_t g = sp_u16(cm, range_o + 2u * lo + ro + 2u * (cp - start));
        return g ? (g + delta) & 0xFFFF : 0;
    }
    case 6: {
        uint32_t first = sp_u16(cm, o + 6), count = sp_u16(cm, o + 8);
        return cp >= first && cp - first < count ? sp_u16(cm, o + 10 + 2u * (cp - first)) : 0;
    }
    case 12:
    case 13: {
        uint32_t lo = 0, hi = sp_u32(cm, o + 12);
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            size_t g = o + 16 + 12u * (size_t)mid;
            uint32_t start = sp_u32(cm, g), end = sp_u32(cm, g + 4);
            if (cp < start) hi = mid;
            else if (cp > end) lo = mid + 1;
            else {
                uint32_t sg = sp_u32(cm, g + 8);
                return f->cmap_format == 12 ? sg + (cp - start) : sg;
            }
        }
        return 0;
    }
    default:
        return 0;
    }
}

uint32_t ttf_glyph_for(const TtfFont *f, uint32_t cp) {
    if (!f->cmap_off) return 0;                                             /* (format 0 is a format, so cmap_format cannot say "no table") */
    uint32_t g;
    if (f->cmap_symbol == 2) g = cp < 0x80 ? lookup(f, cp) : 0;             /* Macintosh Roman: only ASCII means what Unicode says */
    else {
        g = lookup(f, cp);
        if (!g && f->cmap_symbol == 1 && cp < 0x100) g = lookup(f, 0xF000u + cp);   /* symbol fonts keep their characters in the private area */
    }
    return g < (uint32_t)f->num_glyphs ? g : 0;
}
