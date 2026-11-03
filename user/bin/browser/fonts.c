/*
 * The browser's fonts: WebFontProvider on libttf plus text drawing (see fonts.h). Written from the engine's interface
 * (user/lib/web/include/web.h) and libttf's (user/lib/ttf/include/ttf.h); the fallback glyphs come from the toolkit's bitmap
 * font (shared/font8x16.h).
 *
 * Measuring and drawing share one rule for splitting a run into segments: a stretch of characters the TrueType face has
 * glyphs for (laid out by libttf: kerning, ligatures, fractional advances, all linear in the size so that a measure at the
 * CSS size matches a drawing at the device size), and a stretch of characters it lacks (drawn from the 8x16 bitmap font:
 * half the size wide, a whole size for East Asian wide characters). Zero-width characters and controls advance by nothing.
 */
#include "fonts.h"

#include <ttf.h>
#include <font8x16.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

enum { SL_SANS, SL_SANS_B, SL_SANS_I, SL_SANS_BI, SL_MONO, SL_MONO_B, SL_MONO_I, SL_MONO_BI, SL_COUNT };

static const char *const slot_file[SL_COUNT] = {
    "GensokyoSans-Regular.ttf", "GensokyoSans-Bold.ttf", "GensokyoSans-Italic.ttf", "GensokyoSans-BoldItalic.ttf",
    "GensokyoMono-Regular.ttf", "GensokyoMono-Bold.ttf", "GensokyoMono-Italic.ttf", "GensokyoMono-BoldItalic.ttf",
};

typedef struct BrFile {
    TtfFont font;
    uint8_t *data;
    size_t len;
    bool ok;
} BrFile;

/* What a WebFontHandle stands for. */
typedef struct BrFace {
    int slot;                       /* >= 0: the TrueType file; -1: the bitmap font */
    float size;                     /* CSS px (for a monospace bitmap request a multiple of 16) */
    bool bold, italic;              /* made up: the file has no such face */
    bool mono;                      /* bitmap font at a whole multiple of its 8x16 cell */
    TtfFontSet set;
    WebFontMetrics m;
} BrFace;

struct BrFonts {
    BrFile files[SL_COUNT];
    int nfiles;
    BrFace *faces;
    int nfaces, capfaces;
    WebFontProvider prov;
    TtfRaster *raster;
    TtfCache *cache;
    TtfGlyphPos *glyphs;
    size_t capglyphs;
    uint8_t lut_dark[256], lut_light[256];
};

#define U(x) ((WebUnit)((x) * 64.0f + 0.5f))

/* ---------------------------------------------------------------- loading */

static bool load_file(BrFile *bf, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (32 << 20)) { close(fd); return false; }
    size_t n = (size_t)st.st_size, got = 0;
    uint8_t *buf = malloc(n);
    if (!buf) { close(fd); return false; }
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (got != n || ttf_open(&bf->font, buf, n, 0) != TTF_OK) { free(buf); return false; }
    bf->data = buf;
    bf->len = n;
    bf->ok = true;
    return true;
}

static WebFontHandle match(void *user, const WebFontDesc *d);
static void metrics(void *user, WebFontHandle h, WebFontMetrics *out);
static WebUnit measure(void *user, WebFontHandle h, const char *s, size_t n);

BrFonts *brf_new(const char *dir) {
    BrFonts *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    if (!dir) dir = getenv("BROWSER_FONT_DIR");
    if (!dir || !*dir) dir = BRF_DEFAULT_DIR;
    for (int i = 0; i < SL_COUNT; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, slot_file[i]);
        if (load_file(&f->files[i], path)) f->nfiles++;
    }
    f->raster = ttf_raster_new();
    f->cache = ttf_cache_new(2u << 20, 4096);
    ttf_coverage_lut(f->lut_dark, 0x000000, 0xFFFFFF, 50, 30);          /* dark text on a light page: what most pages are */
    ttf_coverage_lut(f->lut_light, 0xFFFFFF, 0x000000, 50, 30);
    f->prov.user = f;
    f->prov.match = match;
    f->prov.metrics = metrics;
    f->prov.measure = measure;
    if (!f->raster || !f->cache) { brf_free(f); return NULL; }
    return f;
}

void brf_free(BrFonts *f) {
    if (!f) return;
    for (int i = 0; i < SL_COUNT; i++) free(f->files[i].data);
    if (f->raster) ttf_raster_free(f->raster);
    if (f->cache) ttf_cache_free(f->cache);
    free(f->faces);
    free(f->glyphs);
    free(f);
}

int brf_file_count(const BrFonts *f) { return f->nfiles; }
const WebFontProvider *brf_provider(BrFonts *f) { return &f->prov; }

/* ---------------------------------------------------------------- families */

enum { FAM_UNKNOWN, FAM_SANS, FAM_MONO };

static bool listed(const char *const *list, const char *name) {
    for (; *list; list++)
        if (strcasecmp(*list, name) == 0) return true;
    return false;
}

/* Which of the two kinds of face a family name asks for; unknown names are skipped so that the next one in the list decides
 * (font-family: "Some Web Font", monospace). Serif and the other generic families have no face of their own here: they are
 * set in Gensokyo Sans. */
static int classify(const char *name) {
    static const char *const sans[] = {
        "sans-serif", "serif", "system-ui", "ui-sans-serif", "ui-serif", "ui-rounded", "-apple-system", "blinkmacsystemfont", "cursive",
        "fantasy", "gensokyo sans", "arial", "arial black", "helvetica", "helvetica neue", "verdana", "tahoma", "trebuchet ms", "segoe ui",
        "roboto", "open sans", "inter", "noto sans", "noto sans jp", "liberation sans", "dejavu sans", "georgia", "times", "times new roman",
        "palatino", "palatino linotype", "garamond", "cambria", "noto serif", "liberation serif", "dejavu serif", "book antiqua",
        "lucida grande", "lucida sans", "calibri", "ubuntu", "cantarell", "droid sans", "source sans pro", "lato", "montserrat",
        "impact", "comic sans ms", NULL };
    static const char *const mono[] = {
        "monospace", "ui-monospace", "gensokyo mono", "courier", "courier new", "consolas", "menlo", "monaco", "lucida console",
        "dejavu sans mono", "liberation mono", "ubuntu mono", "source code pro", "fira code", "fira mono", "sf mono", "andale mono",
        "cascadia code", "cascadia mono", "roboto mono", "noto sans mono", "droid sans mono", NULL };
    if (listed(mono, name)) return FAM_MONO;
    if (listed(sans, name)) return FAM_SANS;
    return FAM_UNKNOWN;
}

/* ---------------------------------------------------------------- faces */

static bool wide_cp(uint32_t c) {                  /* East Asian wide and fullwidth: the same table the engine's built-in font uses */
    return (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF) ||
           (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) ||
           (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
           (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
           (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x20000 && c <= 0x3FFFD);
}

static bool zero_width(uint32_t c) { return ttf_is_ignorable(c) || (c >= 0x0300 && c <= 0x036F) || (c >= 0x2060 && c <= 0x2064); }

static bool space_sep(uint32_t c) { return (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == 0x3000; }

static BrFace *face_of(BrFonts *f, WebFontHandle h) {
    if (h == 0 || h > (WebFontHandle)f->nfaces) return NULL;
    return &f->faces[h - 1];
}

static void ttf_face_metrics(const BrFile *file, float size, WebFontMetrics *m) {
    TtfMetrics tm;
    ttf_metrics(&file->font, size, &tm);
    memset(m, 0, sizeof *m);
    m->ascent = U(tm.ascent);
    m->descent = U(tm.descent);
    m->line_gap = U(tm.line_gap);
    m->x_height = U(tm.x_height > 0 ? tm.x_height : size * 0.5f);
    float upem = (float)file->font.units_per_em;
    m->zero_advance = U((float)ttf_advance_units(&file->font, ttf_glyph_for(&file->font, '0')) * size / upem);
    m->space_advance = U((float)ttf_advance_units(&file->font, ttf_glyph_for(&file->font, ' ')) * size / upem);
    float up = -tm.underline_pos > 0 ? -tm.underline_pos : size * 0.1f;
    float thick = tm.underline_thick > 0 ? tm.underline_thick : (size / 16 > 1.0f ? size / 16 : 1.0f);
    m->underline_offset = U(up);
    m->underline_thickness = U(thick);
}

static void bitmap_face_metrics(float size, WebFontMetrics *m) {          /* three quarters above the baseline, as the engine's own font */
    memset(m, 0, sizeof *m);
    m->ascent = U(size * 0.75f);
    m->descent = U(size * 0.25f);
    m->x_height = U(size * 0.5f);
    m->zero_advance = m->space_advance = U(size * 0.5f);
    float t = size / 16 > 1.0f ? size / 16 : 1.0f;
    m->underline_thickness = U(t);
    m->underline_offset = U(t < 1.5f ? 1.0f : t);
}

/* The handle of a face for (slot, size, made-up flags), creating the record on first use. */
static WebFontHandle face_handle(BrFonts *f, int slot, float size, bool bold, bool italic, bool mono) {
    for (int i = 0; i < f->nfaces; i++) {
        const BrFace *x = &f->faces[i];
        if (x->slot == slot && x->size == size && x->bold == bold && x->italic == italic && x->mono == mono) return (WebFontHandle)(i + 1);
    }
    if (f->nfaces >= 8192) return (WebFontHandle)f->nfaces;                 /* absurd page: reuse the last face */
    if (f->nfaces == f->capfaces) {
        int cap = f->capfaces ? f->capfaces * 2 : 32;
        BrFace *nf = realloc(f->faces, (size_t)cap * sizeof *nf);
        if (!nf) return f->nfaces ? (WebFontHandle)f->nfaces : 0;
        f->faces = nf;
        f->capfaces = cap;
    }
    BrFace *x = &f->faces[f->nfaces];
    memset(x, 0, sizeof *x);
    x->slot = slot;
    x->size = size;
    x->bold = bold;
    x->italic = italic;
    x->mono = mono;
    if (slot >= 0) {
        ttf_set_init(&x->set);
        ttf_set_add(&x->set, &f->files[slot].font);
        ttf_face_metrics(&f->files[slot], size, &x->m);
    } else bitmap_face_metrics(size, &x->m);
    return (WebFontHandle)(++f->nfaces);
}

static WebFontHandle match(void *user, const WebFontDesc *d) {
    BrFonts *f = user;
    int cls = FAM_SANS;
    for (int i = 0; i < d->nfamilies; i++) {
        int c = d->families[i] ? classify(d->families[i]) : FAM_UNKNOWN;
        if (c != FAM_UNKNOWN) { cls = c; break; }
    }
    bool bold = d->weight >= 600, italic = d->style != 0;
    float size = (float)d->size / 64.0f;
    if (size < 1.0f) size = 1.0f;
    if (size > 8192.0f) size = 8192.0f;                              /* a page that asks for a font a mile high gets one that is merely huge */
    int base = cls == FAM_MONO ? SL_MONO : SL_SANS;
    /* the face asked for, else the same without bold, without italic, plain: what is missing is made up */
    int want[4] = { base + (bold ? 1 : 0) + (italic ? 2 : 0), base + (italic ? 2 : 0), base + (bold ? 1 : 0), base };
    for (int i = 0; i < 4; i++) {
        int s = want[i];
        if (!f->files[s].ok) continue;
        bool has_bold = ((s - base) & 1) != 0, has_italic = ((s - base) & 2) != 0;
        return face_handle(f, s, size, bold && !has_bold, italic && !has_italic, false);
    }
    if (cls == FAM_MONO) {                                          /* no mono file: whole cells of the bitmap font */
        int k = (int)((size + 8.0f) / 16.0f);
        if (k < 1) k = 1;
        return face_handle(f, -1, 16.0f * (float)k, false, false, true);
    }
    return face_handle(f, -1, size, false, false, false);
}

static void metrics(void *user, WebFontHandle h, WebFontMetrics *out) {
    BrFace *x = face_of(user, h);
    if (x) *out = x->m;
    else memset(out, 0, sizeof *out);
}

bool brf_face_info(BrFonts *f, WebFontHandle h, BrFaceInfo *out) {
    BrFace *x = face_of(f, h);
    if (!x) return false;
    memset(out, 0, sizeof *out);
    out->kind = x->slot >= 0 ? 0 : 1;
    out->size = x->size;
    out->synth_bold = x->bold;
    out->synth_italic = x->italic;
    out->mono = x->mono;
    snprintf(out->file, sizeof out->file, "%s", x->slot >= 0 ? slot_file[x->slot] : "bitmap");
    return true;
}

/* ---------------------------------------------------------------- segments and measuring */

static bool ttf_covers(const BrFace *x, uint32_t cp) {
    if (x->slot < 0) return false;
    if (zero_width(cp) || space_sep(cp)) return true;
    return ttf_glyph_for(x->set.fonts[0], cp) != 0;
}

typedef struct Seg { const char *p; size_t n; bool ttf; } Seg;

static bool seg_next(const BrFace *x, const char **pp, const char *end, Seg *seg) {
    const char *p = *pp;
    if (p >= end) return false;
    const char *q = p;
    bool kind = ttf_covers(x, ttf_utf8_next(&q, end));
    while (q < end) {
        const char *r = q;
        if (ttf_covers(x, ttf_utf8_next(&r, end)) != kind) break;
        q = r;
    }
    seg->p = p;
    seg->n = (size_t)(q - p);
    seg->ttf = kind;
    *pp = q;
    return true;
}

#define BOLD_EXTRA 0.03f                    /* made-up bold: each glyph is this many ems wider ... */
#define BOLD_SMEAR 0.035f                   /* ... and is drawn twice this many ems apart */

static float bitmap_adv(const BrFace *x, uint32_t cp, float size) {
    if (zero_width(cp)) return 0;
    (void)x;
    return wide_cp(cp) ? size : size * 0.5f;
}

/* the advance of text in CSS px at `size` (device px for the painter's own sizes) */
static float run_width(BrFonts *f, const BrFace *x, const char *s, size_t n, float size) {
    (void)f;
    float w = 0;
    const char *p = s, *end = s + n;
    Seg seg;
    while (seg_next(x, &p, end, &seg)) {
        if (seg.ttf) {
            float sw = 0;
            size_t cnt = ttf_layout(&x->set, size, seg.p, seg.n, TTF_LAYOUT_DEFAULT, NULL, 0, &sw);
            w += sw + (x->bold ? (float)cnt * size * BOLD_EXTRA : 0.0f);
        } else {
            const char *q = seg.p, *qe = seg.p + seg.n;
            while (q < qe) w += bitmap_adv(x, ttf_utf8_next(&q, qe), size);
        }
    }
    return w;
}

static WebUnit measure(void *user, WebFontHandle h, const char *s, size_t n) {
    BrFonts *f = user;
    BrFace *x = face_of(f, h);
    if (!x || !n) return 0;
    return U(run_width(f, x, s, n, x->size));
}

WebUnit brf_measure(BrFonts *f, WebFontHandle h, const char *s, size_t n) { return measure(f, h, s, n); }

int brf_prefix_width(BrFonts *f, WebFontHandle h, int scale, const char *s, size_t len, size_t at) {
    BrFace *x = face_of(f, h);
    if (!x) return 0;
    if (at > len) at = len;
    if (scale < 1) scale = 1;
    return (int)(run_width(f, x, s, at, x->size * (float)scale) + 0.5f);
}

/* ---------------------------------------------------------------- drawing */

static int ifloor(float v) {
    int i = (int)v;
    return (float)i > v ? i - 1 : i;
}

static unsigned luma(uint32_t rgb) { return (((rgb >> 16) & 255) * 77 + ((rgb >> 8) & 255) * 150 + (rgb & 255) * 29) >> 8; }

static void blit_gray(BrSurface *dst, BrRect c, int ox, int oy, const TtfBitmap *bm, uint32_t rgb, unsigned alpha, const uint8_t *lut) {
    if (!bm->data || bm->width <= 0 || bm->height <= 0) return;
    int x0 = ox + bm->left, y0 = oy - bm->top;
    int xs = c.x0 > x0 ? c.x0 - x0 : 0, xe = c.x1 - x0 < bm->width ? c.x1 - x0 : bm->width;
    int ys = c.y0 > y0 ? c.y0 - y0 : 0, ye = c.y1 - y0 < bm->height ? c.y1 - y0 : bm->height;
    for (int j = ys; j < ye; j++) {
        const uint8_t *src = bm->data + (size_t)j * (size_t)bm->stride;
        uint32_t *d = dst->px + (size_t)(y0 + j) * (size_t)dst->stride + x0;
        for (int i = xs; i < xe; i++) {
            unsigned cov = src[i];
            if (!cov) continue;
            unsigned a = lut[cov];
            if (alpha != 255) a = (a * alpha + 127) / 255;
            d[i] = br_mix(d[i], rgb, a);
        }
    }
}

static const TtfBitmap *glyph_bitmap(BrFonts *f, const BrFace *x, uint32_t glyph, float size, int step, TtfBitmap *tmp) {
    if (!x->italic) return ttf_cache_get(f->cache, f->raster, x->set.fonts[0], glyph, size, step, TTF_MODE_GRAY);
    TtfRenderParams p = { .size_px = size, .sub_x = (float)step / 4.0f, .skew = 0.2f, .mode = TTF_MODE_GRAY };
    if (ttf_render(f->raster, x->set.fonts[0], glyph, &p, tmp) != TTF_OK) return NULL;
    return tmp;
}

static float draw_ttf_seg(BrFonts *f, const BrFace *x, BrSurface *dst, BrRect c, const char *s, size_t n, float pen, int base, float size,
                          uint32_t rgb, unsigned alpha) {
    float w = 0;
    size_t cnt = ttf_layout(&x->set, size, s, n, TTF_LAYOUT_DEFAULT, f->glyphs, f->capglyphs, &w);
    if (cnt > f->capglyphs) {
        size_t cap = cnt + 64;
        TtfGlyphPos *ng = realloc(f->glyphs, cap * sizeof *ng);
        if (!ng) return w;
        f->glyphs = ng;
        f->capglyphs = cap;
        cnt = ttf_layout(&x->set, size, s, n, TTF_LAYOUT_DEFAULT, f->glyphs, f->capglyphs, &w);
    }
    float extra = x->bold ? size * BOLD_EXTRA : 0.0f;
    const uint8_t *lut = luma(rgb) < 150 ? f->lut_dark : f->lut_light;
    TtfBitmap tmp;
    for (size_t i = 0; i < cnt; i++) {
        const TtfGlyphPos *g = &f->glyphs[i];
        float fx = pen + g->x + (float)i * extra;
        if (fx > (float)c.x1 + 2.0f) break;
        if (fx + size * 3.0f < (float)c.x0) continue;
        for (int pass = 0; pass < (x->bold ? 2 : 1); pass++) {
            float px = fx + (pass ? size * BOLD_SMEAR : 0.0f);
            int ix = ifloor(px), step = (int)((px - (float)ix) * 4.0f + 0.5f);
            if (step >= 4) { ix++; step = 0; }
            const TtfBitmap *bm = glyph_bitmap(f, x, g->glyph, size, step, &tmp);
            if (bm) blit_gray(dst, c, ix, base, bm, rgb, alpha, lut);
        }
    }
    return w + (float)cnt * extra;
}

static float draw_bitmap_seg(const BrFace *x, BrSurface *dst, BrRect c, const char *s, size_t n, float pen, int base, float size,
                             uint32_t rgb, unsigned alpha) {
    const char *p = s, *end = s + n;
    float start = pen;
    int gh = (int)(size + 0.5f);
    if (gh < 1) gh = 1;
    int top = base - (int)(size * 0.75f + 0.5f);
    while (p < end) {
        uint32_t cp = ttf_utf8_next(&p, end);
        float adv = bitmap_adv(x, cp, size);
        if (adv <= 0) continue;
        int x0 = (int)(pen + 0.5f), x1 = (int)(pen + adv + 0.5f);
        int gw = x1 - x0;
        pen += adv;
        if (gw < 1 || x0 >= c.x1 || x1 <= c.x0 || top >= c.y1 || top + gh <= c.y0) continue;
        const unsigned char *g = font8x16_glyph(cp);
        int jlo = c.y0 > top ? c.y0 - top : 0, jhi = c.y1 - top < gh ? c.y1 - top : gh;           /* only the rows and columns inside the clip */
        int ilo = c.x0 > x0 ? c.x0 - x0 : 0, ihi = c.x1 - x0 < gw ? c.x1 - x0 : gw;
        for (int j = jlo; j < jhi; j++) {
            int y = top + j;
            unsigned row = g[(int)((int64_t)j * 16 / gh)];
            if (!row) continue;
            uint32_t *d = dst->px + (size_t)y * (size_t)dst->stride;
            for (int i = ilo; i < ihi; i++) {
                int px = x0 + i;
                if (row & (0x80u >> (int)((int64_t)i * 8 / gw))) d[px] = br_mix(d[px], rgb, alpha);
            }
        }
    }
    return pen - start;
}

void brf_draw_text(BrFonts *f, BrSurface *dst, BrRect clip, WebFontHandle h, int scale, const char *s, size_t n, int32_t x64, int32_t y64,
                   uint32_t argb, unsigned group_alpha) {
    BrFace *x = face_of(f, h);
    if (!x || !n || !dst || !dst->px) return;
    unsigned alpha = (unsigned)(((argb >> 24) * group_alpha + 127) / 255);
    if (!alpha) return;
    BrRect c = br_rect_and(clip, br_rect(0, 0, dst->w, dst->h));
    if (br_rect_empty(c)) return;
    if (scale < 1) scale = 1;
    float size = x->size * (float)scale;
    int base = (int)((y64 + 32) >> 6);
    if ((float)base - size * 1.4f >= (float)c.y1 || (float)base + size * 0.6f <= (float)c.y0) return;
    float pen = (float)x64 / 64.0f;
    uint32_t rgb = argb & 0xFFFFFFu;
    const char *p = s, *end = s + n;
    Seg seg;
    while (seg_next(x, &p, end, &seg)) {
        if (pen > (float)c.x1 + size * 2.0f) break;
        if (seg.ttf) pen += draw_ttf_seg(f, x, dst, c, seg.p, seg.n, pen, base, size, rgb, alpha);
        else pen += draw_bitmap_seg(x, dst, c, seg.p, seg.n, pen, base, size, rgb, alpha);
    }
}
