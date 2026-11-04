/*
 * Kappa TrueType text: draws UTF-8 strings with libttf into a KCanvas. Compiled only with KAPPA_TTF (see kappa_ttf.h); otherwise this
 * file is empty and the toolkit's bitmap text is all there is.
 *
 * What it adds to libttf's ttf_draw_text: the canvas' scale (text is rendered at the physical size, not enlarged), the clip rectangle, the
 * line box (y is the top, as for kc_text), a cached coverage curve per text colour, and "..." truncation. At the end of the file is the system UI
 * font (kt_ui_*): Gensokyo Sans at 13 px with the bitmap font for the characters it lacks, which canvas.c puts behind kc_text and its relatives.
 */
#include <kappa_ttf.h>

#if KAPPA_TTF

#include <font8x16.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct KTtf {
    TtfFont fonts[TTF_SET_MAX];
    void *owned[TTF_SET_MAX];                   /* file contents this context read itself */
    TtfFontSet set;
    TtfRaster *raster;
    TtfCache *cache;
    int gamma, contrast, lcd;
    uint8_t lut[256];
    uint32_t lut_fg;
    int lut_valid;
};

KTtf *kt_new(void) {
    KTtf *t = (KTtf *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->raster = ttf_raster_new();
    t->cache = ttf_cache_new(1u << 20, 2048);
    if (!t->raster || !t->cache) { kt_free(t); return NULL; }
    ttf_set_init(&t->set);
    t->gamma = 50;
    t->contrast = 30;
    return t;
}

void kt_free(KTtf *t) {
    if (!t) return;
    ttf_cache_free(t->cache);
    ttf_raster_free(t->raster);
    for (int i = 0; i < TTF_SET_MAX; i++) free(t->owned[i]);
    free(t);
}

int kt_add_font(KTtf *t, const void *data, size_t len, int face) {
    if (!t) return TTF_E_ARG;
    int n = t->set.count;
    if (n >= TTF_SET_MAX) return TTF_E_TOOBIG;
    int rc = ttf_open(&t->fonts[n], data, len, face);
    if (rc) return rc;
    return ttf_set_add(&t->set, &t->fonts[n]);
}

int kt_add_file(KTtf *t, const char *path, int face) {
    if (!t || !path) return TTF_E_ARG;
    int n = t->set.count;
    if (n >= TTF_SET_MAX) return TTF_E_TOOBIG;
    FILE *f = fopen(path, "rb");
    if (!f) return TTF_E_FORMAT;
    size_t cap = 1 << 16, len = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    while (buf) {
        if (len == cap) {
            if (cap > (size_t)256 << 20) { free(buf); buf = NULL; break; }               /* no font is that large */
            uint8_t *nb = (uint8_t *)realloc(buf, cap * 2);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
            cap *= 2;
        }
        size_t r = fread(buf + len, 1, cap - len, f);
        if (r == 0) break;
        len += r;
    }
    fclose(f);
    if (!buf) return TTF_E_NOMEM;
    int rc = kt_add_font(t, buf, len, face);
    if (rc) { free(buf); return rc; }
    t->owned[n] = buf;
    return TTF_OK;
}

void kt_set_blend(KTtf *t, int gamma, int contrast) {
    if (!t) return;
    t->gamma = gamma;
    t->contrast = contrast;
    t->lut_valid = 0;
}

void kt_set_lcd(KTtf *t, int lcd) {
    if (t) t->lcd = lcd < 0 || lcd > 2 ? 0 : lcd;
}

int kt_ascent(KTtf *t, float size) {
    if (!t || t->set.count == 0) return 0;
    TtfMetrics m;
    ttf_metrics(t->set.fonts[0], size, &m);
    return (int)(m.ascent + 0.5f);
}

int kt_line_height(KTtf *t, float size) {
    if (!t || t->set.count == 0) return 0;
    TtfMetrics m;
    ttf_metrics(t->set.fonts[0], size, &m);
    return (int)(m.line_height + 0.5f);
}

int kt_text_width(KTtf *t, float size, const char *utf8) {
    if (!t || !utf8 || t->set.count == 0) return 0;
    float w = ttf_text_width(&t->set, size, utf8, strlen(utf8), TTF_LAYOUT_DEFAULT);
    return (int)(w + 0.999f);
}

/* The coverage curve for a text colour. The ground is not known to the canvas: assume the opposite extreme of the text's own brightness, which
 * is what text normally sits on (dark ink on light paper, light text on a dark bar). */
static const uint8_t *lut_for(KTtf *t, uint32_t fg) {
    if (t->gamma <= 0 && t->contrast <= 0) return NULL;
    if (!t->lut_valid || t->lut_fg != fg) {
        uint32_t luma = (54 * ((fg >> 16) & 255) + 183 * ((fg >> 8) & 255) + 19 * (fg & 255)) >> 8;
        ttf_coverage_lut(t->lut, fg, luma < 128 ? 0xFFFFFF : 0x000000, t->gamma, t->contrast);
        t->lut_fg = fg;
        t->lut_valid = 1;
    }
    return t->lut;
}

int kt_text(KCanvas *c, KTtf *t, float size, int x, int y, const char *utf8, uint32_t fg) {
    if (!c || !t || !utf8 || t->set.count == 0 || !(size > 0)) return 0;
    int s = kc_scale(c);
    float psize = size * (float)s;                                   /* the size in pixels of px */
    size_t len = strlen(utf8);
    KCanvas v = kc_native_view(c);                                   /* the same pixels, in physical coordinates */
    if (kr_empty(v.clip)) return (int)(ttf_text_width(&t->set, psize, utf8, len, TTF_LAYOUT_DEFAULT) / (float)s + 0.999f);
    TtfMetrics m;
    ttf_metrics(t->set.fonts[0], psize, &m);
    int baseline = y * s + (int)(m.ascent + 0.5f);
    TtfSurface surf = { v.px, v.w, v.h, v.stride };
    TtfDrawStyle st;
    memset(&st, 0, sizeof st);
    st.color = fg & 0xFFFFFFu;
    st.mode = t->lcd ? TTF_MODE_LCD : TTF_MODE_GRAY;
    st.lcd_bgr = t->lcd == 2;
    st.layout_flags = TTF_LAYOUT_DEFAULT;
    st.lut = lut_for(t, st.color);
    st.clip_x0 = v.clip.x;
    st.clip_y0 = v.clip.y;
    st.clip_x1 = v.clip.x + v.clip.w;
    st.clip_y1 = v.clip.y + v.clip.h;
    float w = ttf_draw_text(t->cache, t->raster, &t->set, psize, utf8, len, (float)(x * s), baseline, &st, &surf);
    return (int)(w / (float)s + 0.999f);
}

void kt_text_in(KCanvas *c, KTtf *t, float size, KRect r, const char *utf8, uint32_t fg, KAlign align) {
    if (!c || !t || !utf8 || r.w <= 0 || r.h <= 0 || t->set.count == 0) return;
    char buf[512];
    const char *show = utf8;
    int w = kt_text_width(t, size, utf8);
    if (w > r.w) {                                                   /* too wide: the longest prefix that fits with "..." after it */
        int dots = kt_text_width(t, size, "...");
        size_t total = strlen(utf8), keep = 0, i = 0;
        while (i < total && i < sizeof buf - 8) {                    /* grow the prefix one character at a time (labels are short) */
            size_t j = i + 1;
            while (j < total && ((unsigned char)utf8[j] & 0xC0) == 0x80) j++;
            memcpy(buf, utf8, j);
            buf[j] = 0;
            if (kt_text_width(t, size, buf) + dots > r.w) break;
            keep = i = j;
        }
        memcpy(buf, utf8, keep);
        memcpy(buf + keep, "...", 4);
        show = buf;
        w = kt_text_width(t, size, show);
    }
    int x = align == KA_LEFT ? r.x : align == KA_RIGHT ? r.x + r.w - w : r.x + (r.w - w) / 2;
    int y = r.y + (r.h - kt_line_height(t, size)) / 2;
    KRect old = c->clip;
    kc_set_clip(c, kr_intersect(old, r));
    kt_text(c, t, size, x, y, show, fg);
    c->clip = old;
}

/* ---------------------------------------------------------------- the system UI font (see kappa_ttf.h) */

static KTtf *g_ui;                                              /* this process's copy of Gensokyo Sans */
static int g_ui_state;                                          /* 0 not tried yet, 1 loaded, -1 could not be loaded */
static KTtf *g_ui_bold;                                         /* and of Gensokyo Sans Bold, read the first time bold text is drawn */
static int g_bold_state;                                        /* the same: 0, 1, -1 (then bold text is the regular face drawn twice, half a pixel apart) */

/* The environment variable of the UI font (for tests and for trying another face; not in a setuid program), or NULL. */
static const char *env_font(const char *name) {
    const char *v = getuid() == geteuid() && getgid() == getegid() ? getenv(name) : NULL;
    return v && v[0] ? v : NULL;
}

/* Reads one face and sets the blend. Plain alpha blending (gamma 0) with a little contrast: the amount of ink per character then equals the bitmap font's
 * within 3% for dark text on paper and for light text on red or night blue (measured, docs/TTF.md); the default (50, 30) is 11% lighter than the bitmap font
 * for dark text and 15% heavier for light text. The Bold is blended the same way. */
static KTtf *load_ui_face(const char *path) {
    KTtf *t = kt_new();
    if (t && kt_add_file(t, path, 0) == TTF_OK) {
        kt_set_blend(t, 0, 30);
        return t;
    }
    kt_free(t);
    return NULL;
}

int kt_ui_ready(void) {
    if (g_ui_state == 0) {
        const char *path = env_font("KAPPA_UI_FONT");
        g_ui = load_ui_face(path ? path : KT_UI_FONT_PATH);
        g_ui_state = g_ui ? 1 : -1;
    }
    return g_ui_state > 0;
}

/* The Bold face, when there is one: $KAPPA_UI_BOLD_FONT names it; with $KAPPA_UI_FONT set and no bold named the face in use is another one and the system's Bold
 * would not match it, so bold text stays the double strike. */
static int bold_ready(void) {
    if (g_bold_state == 0) {
        const char *path = env_font("KAPPA_UI_BOLD_FONT");
        if (!path && !env_font("KAPPA_UI_FONT")) path = KT_UI_BOLD_FONT_PATH;
        g_ui_bold = path ? load_ui_face(path) : NULL;
        g_bold_state = g_ui_bold ? 1 : -1;
    }
    return g_bold_state > 0;
}

/* Does the bitmap font have a picture for cp (it answers an unknown code point with its hollow box)? */
static int bitmap_has(uint32_t cp) { return font8x16_glyph(cp) != font8x16_glyph(0x10FFFFu); }

/* Draws (c != NULL) and measures one run of text that the UI font has, starting at logical pen x. Returns the run's advance in logical pixels. */
static float ui_run(KTtf *t, KCanvas *c, float x, int y, const char *run, size_t len, uint32_t fg) {
    int s = c ? kc_scale(c) : 1;
    float psize = KT_UI_SIZE * (float)s;                         /* glyphs are rendered at the physical size, positions come from the logical pen */
    if (!c) return ttf_text_width(&t->set, psize, run, len, TTF_LAYOUT_DEFAULT) / (float)s;
    KCanvas v = kc_native_view(c);
    if (kr_empty(v.clip)) return ttf_text_width(&t->set, psize, run, len, TTF_LAYOUT_DEFAULT) / (float)s;
    TtfSurface surf = { v.px, v.w, v.h, v.stride };
    TtfDrawStyle st;
    memset(&st, 0, sizeof st);
    st.color = fg & 0xFFFFFFu;
    st.mode = t->lcd ? TTF_MODE_LCD : TTF_MODE_GRAY;
    st.lcd_bgr = t->lcd == 2;
    st.layout_flags = TTF_LAYOUT_DEFAULT;
    st.lut = lut_for(t, st.color);
    st.clip_x0 = v.clip.x;
    st.clip_y0 = v.clip.y;
    st.clip_x1 = v.clip.x + v.clip.w;
    st.clip_y1 = v.clip.y + v.clip.h;
    float w = ttf_draw_text(t->cache, t->raster, &t->set, psize, run, len, x * (float)s, (y + KT_UI_BASELINE) * s, &st, &surf);
    return w / (float)s;
}

float kt_ui_walk(KCanvas *c, float x, int y, const char *s, size_t bytes, uint32_t fg, int bold) {
    if (!s || !kt_ui_ready()) return 0;
    int real_bold = bold && bold_ready();                        /* a real weight: its own advances, nothing is drawn twice */
    KTtf *t = real_bold ? g_ui_bold : g_ui;
    const TtfFont *font = t->set.fonts[0];
    bytes = strnlen(s, bytes);                                   /* never past the NUL, whatever length the caller has in mind ((size_t)-1: all of it) */
    const char *p = s, *end = s + bytes, *run = NULL;
    float pen = 0;
    while (p < end) {
        const char *q = p;
        uint32_t cp = ttf_utf8_next(&q, end);
        int kind = 0;                                            /* 0: the UI font's (or skipped) text, 1: a bitmap cell, 2: a tab */
        if (cp == '\t') kind = 2;
        else if (!ttf_is_ignorable(cp) && ttf_glyph_for(font, cp) == 0 && bitmap_has(cp)) kind = 1;
        if (kind == 0) {
            if (!run) run = p;
            p = q;
            continue;
        }
        if (run) {                                               /* the run before this character */
            float w = ui_run(t, c, x + pen, y, run, (size_t)(p - run), fg);
            if (bold && !real_bold && c) ui_run(t, c, x + pen + 0.5f, y, run, (size_t)(p - run), fg);
            pen += w;
            run = NULL;
        }
        if (kind == 1) {
            if (c) {
                int bx = (int)(x + pen + 0.5f);
                kc_glyph(c, bx, y, cp, fg, 1);
                if (bold) kc_glyph(c, bx + 1, y, cp, fg, 1);
            }
            pen += 8.0f;
        } else {
            pen += (float)ttf_advance_units(font, ttf_glyph_for(font, ' ')) * ttf_scale(font, KT_UI_SIZE);
        }
        p = q;
    }
    if (run) {
        float w = ui_run(t, c, x + pen, y, run, (size_t)(end - run), fg);
        if (bold && !real_bold && c) ui_run(t, c, x + pen + 0.5f, y, run, (size_t)(end - run), fg);
        pen += w;
    }
    return bold && !real_bold ? pen + 1.0f : pen;
}

#endif /* KAPPA_TTF */
