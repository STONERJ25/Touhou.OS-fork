/*
 * Fonts as layout sees them: a cache of the embedder's fonts (WebFontProvider) per (family list, size, weight, style), and
 * the built-in fixed-width font used when the embedder gives none (tests, tools): every character is half the font size
 * wide (a full size for East Asian wide and fullwidth characters, nothing for combining marks and zero-width ones), the
 * ascent is three quarters of the size and the descent a quarter, so at 16 px it measures like the toolkit's 8x16
 * bitmap font and layouts can be computed by hand.
 */
#include "css.h"

typedef struct FontCache { FontRef *slots[128]; } FontCache;

/* ------------------------------------------------------------------------------------------------ the built-in font */

static bool wide_cp(uint32_t c) {
    return (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF) ||
           (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) ||
           (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
           (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
           (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x20000 && c <= 0x3FFFD);
}

static bool zero_cp(uint32_t c) {
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x200B && c <= 0x200F) || c == 0xFEFF || (c >= 0xFE00 && c <= 0xFE0F) ||
           c == 0x00AD || (c >= 0x2060 && c <= 0x2064);
}

WebUnit builtin_advance(WebUnit size, uint32_t cp) {
    if (zero_cp(cp)) return 0;
    return wide_cp(cp) ? size : size / 2;
}

static WebUnit builtin_measure(WebUnit size, const char *s, size_t n) {
    WebUnit w = 0;
    for (size_t i = 0; i < n;) {
        size_t len;
        uint32_t cp = utf8_decode(s + i, n - i, &len);
        i += len;
        w += builtin_advance(size, cp);
    }
    return w;
}

static void builtin_metrics(WebUnit size, WebFontMetrics *m) {
    memset(m, 0, sizeof *m);
    m->ascent = size * 3 / 4;
    m->descent = size - m->ascent;
    m->line_gap = 0;
    m->x_height = size / 2;
    m->zero_advance = size / 2;
    m->space_advance = size / 2;
    m->underline_offset = size / 16 > 64 ? size / 16 : 64;
    m->underline_thickness = size / 16 > 64 ? size / 16 : 64;
}

/* the built-in font's handles encode the size and the style: size << 8 | flags, never 0 */
WebFontHandle builtin_handle(WebUnit size, int weight, int style) {
    return ((uintptr_t)size << 8) | 0x80 | (weight >= 600 ? 1 : 0) | (style ? 2 : 0);
}

WebUnit builtin_handle_size(WebFontHandle h) { return (WebUnit)(h >> 8); }

/* ------------------------------------------------------------------------------------------------ the cache */

static uint32_t key_hash(const FontList *f, WebUnit size, int weight, int style) {
    uint32_t h = f ? f->hash ^ (uint32_t)(uintptr_t)f : 0;
    h = h * 31 + (uint32_t)size;
    h = h * 31 + (uint32_t)weight;
    h = h * 31 + (uint32_t)style;
    return h;
}

static bool same_family(const FontList *a, const FontList *b) {
    if (a == b) return true;
    if (!a || !b || a->n != b->n) return false;
    for (int i = 0; i < a->n; i++) if (strcasecmp(a->names[i], b->names[i])) return false;
    return true;
}

FontRef *font_get(WebDoc *d, const CInh *inh) {
    WebUnit size = (WebUnit)(inh->font_size * 64 + 0.5f);
    if (size < 1) size = 1;
    int weight = inh->font_weight, style = inh->font_style;
    FontRef *f = inh->font;
    if (f && f->size == inh->font_size && f->weight == weight && f->style == style && same_family(f->family, inh->family)) return f;
    if (!d->fonts) {
        d->fonts = wa_alloc(&d->arena, sizeof(FontCache));
        if (!d->fonts) return NULL;
    }
    uint32_t h = key_hash(inh->family, size, weight, style) & 127;
    for (f = d->fonts->slots[h]; f; f = f->next)
        if (f->size == inh->font_size && f->weight == weight && f->style == style && same_family(f->family, inh->family)) break;
    if (!f) {
        f = wa_alloc(&d->arena, sizeof *f);
        if (!f) return NULL;
        f->size = inh->font_size;
        f->weight = (uint16_t)weight;
        f->style = (uint8_t)style;
        f->family = inh->family;
        const WebFontProvider *p = d->cfg.fonts;
        if (p && p->match) {
            WebFontDesc desc;
            memset(&desc, 0, sizeof desc);
            desc.families = inh->family ? inh->family->names : NULL;
            desc.nfamilies = inh->family ? inh->family->n : 0;
            desc.size = size;
            desc.weight = weight;
            desc.style = style;
            desc.small_caps = inh->small_caps;
            f->h = p->match(p->user, &desc);
            if (p->metrics) p->metrics(p->user, f->h, &f->m);
            else builtin_metrics(size, &f->m);
        } else {
            f->h = builtin_handle(size, weight, style);
            builtin_metrics(size, &f->m);
        }
        f->space_w = font_measure(d, f, " ", 1);
        f->next = d->fonts->slots[h];
        d->fonts->slots[h] = f;
    }
    ((CInh *)inh)->font = f;                                 /* a cache inside a shared group: harmless */
    return f;
}

WebUnit font_measure(WebDoc *d, const FontRef *f, const char *s, size_t n) {
    const WebFontProvider *p = d->cfg.fonts;
    if (p && p->measure) return p->measure(p->user, f->h, s, n);
    return builtin_measure(builtin_handle_size(f->h), s, n);
}
