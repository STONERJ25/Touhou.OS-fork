/*
 * Text services: UTF-8, font sets with per-character fallback, and one-line layout (advances, kerning, ligatures).
 *
 * The layout is a single pass over the code points. Each one is resolved to (font, glyph) through the set; glyphs of the same font
 * collect in a short window so that a ligature can look a few glyphs ahead; every glyph that leaves the window is held back one step
 * so that the kerning against its successor can still be added to its advance. Nothing is allocated and the caller's output array may
 * be shorter than the result (the return value says how long it would have to be).
 *
 * Not done, on purpose (future work): bidirectional reordering, complex-script shaping (Arabic joining, Indic reordering, Thai),
 * mark positioning, vertical text, line breaking. The browser breaks lines itself and asks for widths.
 */
#include "ttfint.h"

/* ---------------------------------------------------------------- UTF-8 */

uint32_t ttf_utf8_next(const char **pp, const char *end) {
    const uint8_t *p = (const uint8_t *)*pp, *e = (const uint8_t *)end;
    uint32_t c = *p++, cp;
    int need;
    if (c < 0x80) { *pp = (const char *)p; return c; }
    if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; }
    else { *pp = (const char *)p; return 0xFFFD; }                /* a stray continuation byte, C0/C1, or F5..FF */
    for (int i = 0; i < need; i++) {
        if (p >= e || (*p & 0xC0) != 0x80) { *pp = (const char *)p; return 0xFFFD; }      /* truncated: one U+FFFD for what was read */
        if (i == 0 && ((c == 0xE0 && *p < 0xA0) || (c == 0xED && *p > 0x9F) || (c == 0xF0 && *p < 0x90) || (c == 0xF4 && *p > 0x8F))) {
            *pp = (const char *)p;                                  /* overlong, surrogate or beyond 10FFFF */
            return 0xFFFD;
        }
        cp = (cp << 6) | (*p & 0x3F);
        p++;
    }
    *pp = (const char *)p;
    return cp;
}

int ttf_utf8_encode(uint32_t cp, char out[4]) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) cp = 0xFFFD;
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ---------------------------------------------------------------- font sets */

void ttf_set_init(TtfFontSet *s) { memset(s, 0, sizeof *s); }

int ttf_set_add(TtfFontSet *s, const TtfFont *f) {
    if (!s || !f) return TTF_E_ARG;
    if (s->count >= TTF_SET_MAX) return TTF_E_TOOBIG;
    s->fonts[s->count++] = f;
    return TTF_OK;
}

int ttf_set_lookup(const TtfFontSet *s, uint32_t cp, uint32_t *glyph) {
    for (int i = 0; i < s->count; i++) {
        uint32_t g = ttf_glyph_for(s->fonts[i], cp);
        if (g) { *glyph = g; return i; }
    }
    *glyph = 0;
    return -1;
}

int ttf_is_ignorable(uint32_t cp) {
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 1;
    if (cp == 0xAD || cp == 0x34F || cp == 0x61C || cp == 0x180E || cp == 0xFEFF) return 1;
    if (cp >= 0x200B && cp <= 0x200F) return 1;                    /* zero-width space, joiners, direction marks */
    if (cp >= 0x202A && cp <= 0x202E) return 1;                    /* embedding and override controls */
    if (cp >= 0x2060 && cp <= 0x206F) return 1;                    /* word joiner, invisible operators, deprecated format controls */
    if (cp >= 0xFE00 && cp <= 0xFE0F) return 1;                    /* variation selectors */
    if (cp == 0xE0001 || (cp >= 0xE0020 && cp <= 0xE007F)) return 1;
    if (cp >= 0xE0100 && cp <= 0xE01EF) return 1;                  /* variation selectors supplement */
    return 0;
}

/* Space separators: when no font has the exact character they are shown as a plain space. */
static int is_space_like(uint32_t cp) {
    return cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

/* ---------------------------------------------------------------- layout */

#define WINDOW 8                                                    /* glyphs a ligature may look ahead (and the longest one it can match) */

typedef struct Layout {
    const TtfFontSet *set;
    float scale[TTF_SET_MAX];
    unsigned flags;
    TtfGlyphPos *out;
    size_t cap, count;
    float pen;
    int have_prev;
    TtfGlyphPos prev;                                               /* waiting for its successor, to learn its kerning */
    uint32_t wg[WINDOW], wc[WINDOW];                                /* the lookahead window: glyphs and clusters */
    uint16_t wf[WINDOW];                                            /* their TTF_GP_* flags */
    int wn, wfont;
} Layout;

static float round_px(float v) { return (float)(int)(v + (v < 0 ? -0.5f : 0.5f)); }

static void finish_prev(Layout *l, float kern) {
    float a = l->prev.advance + kern;
    if (l->flags & TTF_LAYOUT_ROUND) a = round_px(a);
    l->prev.advance = a;
    if (l->out && l->count < l->cap) l->out[l->count] = l->prev;
    l->count++;
    l->pen += a;
    l->have_prev = 0;
}

static void push_glyph(Layout *l, int font, uint32_t glyph, uint32_t cluster, unsigned gflags) {
    const TtfFont *f = l->set->fonts[font];
    if (l->have_prev) {
        float kern = 0;
        if ((l->flags & TTF_LAYOUT_KERN) && l->prev.font == font)
            kern = (float)ttf_kern_units(f, l->prev.glyph, glyph) * l->scale[font];
        finish_prev(l, kern);
    }
    l->prev.glyph = glyph;
    l->prev.font = (uint16_t)font;
    l->prev.flags = (uint16_t)gflags;
    l->prev.x = l->pen;
    l->prev.advance = (float)ttf_advance_units(f, glyph) * l->scale[font];
    l->prev.cluster = cluster;
    l->have_prev = 1;
}

/* Sends the first glyph of the window on its way, as a ligature if the font has one that starts here. */
static void window_emit(Layout *l) {
    const TtfFont *f = l->set->fonts[l->wfont];
    int used = 1;
    uint32_t glyph = l->wg[0];
    unsigned fl = l->wf[0];
    if ((l->flags & TTF_LAYOUT_LIGA) && f->ngsub_liga) {
        uint32_t lg;
        int k = ttf_ligature(f, l->wg, (size_t)l->wn, &lg);
        if (k >= 2 && k <= l->wn) { used = k; glyph = lg; fl = TTF_GP_LIGATURE; }
    }
    push_glyph(l, l->wfont, glyph, l->wc[0], fl);
    l->wn -= used;
    for (int i = 0; i < l->wn; i++) { l->wg[i] = l->wg[i + used]; l->wc[i] = l->wc[i + used]; l->wf[i] = l->wf[i + used]; }
}

size_t ttf_layout(const TtfFontSet *set, float size_px, const char *utf8, size_t len, unsigned flags,
                  TtfGlyphPos *out, size_t cap, float *width) {
    if (width) *width = 0;
    if (!set || set->count < 1 || !utf8 || len == 0) return 0;
    if (!(size_px > 0)) size_px = 0;
    if (size_px > 100000.0f) size_px = 100000.0f;
    Layout l;
    memset(&l, 0, sizeof l);
    l.set = set;
    l.flags = flags;
    l.out = out;
    l.cap = out ? cap : 0;
    for (int i = 0; i < set->count; i++) l.scale[i] = ttf_scale(set->fonts[i], size_px);
    l.wfont = -1;

    const char *p = utf8, *end = utf8 + len;
    while (p < end) {
        uint32_t cluster = (uint32_t)(p - utf8);
        uint32_t cp = ttf_utf8_next(&p, end), g;
        if (ttf_is_ignorable(cp)) continue;
        int font = ttf_set_lookup(set, cp, &g);
        unsigned gf = 0;
        if (font < 0) {
            if (is_space_like(cp)) font = ttf_set_lookup(set, 0x20, &g);
            if (font < 0) { font = 0; g = 0; gf = TTF_GP_NOTDEF; }
        }
        if (font != l.wfont) {                                      /* kerning and ligatures never cross from one font to another */
            while (l.wn > 0) window_emit(&l);
            l.wfont = font;
        }
        l.wg[l.wn] = g;
        l.wc[l.wn] = cluster;
        l.wf[l.wn] = (uint16_t)gf;
        l.wn++;
        while (l.wn >= WINDOW) window_emit(&l);
    }
    while (l.wn > 0) window_emit(&l);
    if (l.have_prev) finish_prev(&l, 0);
    if (width) *width = l.pen;
    return l.count;
}

float ttf_text_width(const TtfFontSet *set, float size_px, const char *utf8, size_t len, unsigned flags) {
    float w = 0;
    ttf_layout(set, size_px, utf8, len, flags, NULL, 0, &w);
    return w;
}
