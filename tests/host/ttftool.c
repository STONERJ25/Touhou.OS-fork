/*
 * ttftool: command-line driver for libttf, used by tests/host/test_ttf.py (built with AddressSanitizer and UBSan) and by hand.
 *
 *   ttftool draw OUT.ppm|OUT.pgm FONT[,FONT...] SIZE "text"        draw a line of text (font file[:face], comma separated fallbacks) to a PPM or PGM
 *   ttftool CMD args...                                            run one command
 *   ttftool < script                                               run one command per line (words separated by blanks)
 *
 * Commands print plain text lines that the tests parse; a failing command prints "err CODE message". Text arguments may be written
 * as hex: HEXBYTES to avoid any quoting. The commands are listed in `usage` below.
 *
 * The library is compiled with -DTTF_ALLOC_HOOKS so that every allocation goes through ttf_hook_*: the fuzzer uses them to prove that
 * hostile fonts never make the library ask for unreasonable amounts of memory, and `leaks` proves that everything is freed.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "ttf.h"

/* ------------------------------------------------------------------ allocation accounting */

typedef struct { size_t size; size_t pad; } AllocHdr;
static size_t g_cur, g_peak, g_max_request, g_limit = (size_t)-1;
static unsigned long g_calls;
static long g_fail_in;                  /* >0: the allocation request that brings this to zero fails (`failat N`), once */

static int fail_now(void) { return g_fail_in > 0 && --g_fail_in == 0; }

void *ttf_hook_malloc(size_t n) {
    g_calls++;
    if (n > g_max_request) g_max_request = n;
    if (fail_now() || n > g_limit || g_cur + n > g_limit) return NULL;
    AllocHdr *h = malloc(sizeof *h + n);
    if (!h) return NULL;
    h->size = n;
    g_cur += n;
    if (g_cur > g_peak) g_peak = g_cur;
    return h + 1;
}

void ttf_hook_free(void *p) {
    if (!p) return;
    AllocHdr *h = (AllocHdr *)p - 1;
    g_cur -= h->size;
    free(h);
}

void *ttf_hook_realloc(void *p, size_t n) {
    if (!p) return ttf_hook_malloc(n);
    g_calls++;
    AllocHdr *h = (AllocHdr *)p - 1;
    if (n > g_max_request) g_max_request = n;
    if (fail_now() || n > g_limit || g_cur - h->size + n > g_limit) return NULL;
    size_t old = h->size;
    AllocHdr *nh = realloc(h, sizeof *h + n);
    if (!nh) return NULL;
    nh->size = n;
    g_cur = g_cur - old + n;
    if (g_cur > g_peak) g_peak = g_cur;
    return nh + 1;
}

/* ------------------------------------------------------------------ state */

#define MAX_SLOTS 8

typedef struct Slot {
    char name[32];
    uint8_t *data;
    size_t size;
    TtfFont font;
    int open;
} Slot;

static Slot g_slots[MAX_SLOTS];
static TtfRaster *g_raster;
static TtfCache *g_cache;
static TtfFontSet g_set;
static int g_use_cache = 1;

static Slot *find_slot(const char *name) {
    for (int i = 0; i < MAX_SLOTS; i++) if (g_slots[i].name[0] && !strcmp(g_slots[i].name, name)) return &g_slots[i];
    return NULL;
}

static Slot *new_slot(const char *name) {
    Slot *s = find_slot(name);
    if (s) { free(s->data); memset(s, 0, sizeof *s); }
    else for (int i = 0; i < MAX_SLOTS && !s; i++) if (!g_slots[i].name[0]) s = &g_slots[i];
    if (s) snprintf(s->name, sizeof s->name, "%s", name);
    return s;
}

static uint8_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, len = 0;
    uint8_t *b = malloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; b = realloc(b, cap); }
        size_t r = fread(b + len, 1, cap - len, f);
        if (r == 0) break;
        len += r;
    }
    fclose(f);
    *n = len;
    return b;
}

static unsigned long parse_num(const char *s) { return strtoul(s, NULL, 0); }

/* hex:... -> bytes; anything else is taken literally */
static char *text_arg(const char *s, size_t *len) {
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (!strncmp(s, "hex:", 4)) {
        size_t k = 0;
        for (size_t i = 4; i + 1 < n + 0 && s[i] && s[i + 1]; i += 2) {
            char b[3] = { s[i], s[i + 1], 0 };
            out[k++] = (char)strtoul(b, NULL, 16);
        }
        out[k] = 0;
        *len = k;
    } else {
        memcpy(out, s, n + 1);
        *len = n;
    }
    return out;
}

static int load_font(const char *name, const char *path, int face) {
    size_t n;
    uint8_t *d = read_file(path, &n);
    if (!d) { printf("err -99 cannot read %s\n", path); return 1; }
    Slot *s = new_slot(name);
    if (!s) { free(d); printf("err -98 too many fonts\n"); return 1; }
    s->data = d;
    s->size = n;
    int rc = ttf_open(&s->font, d, n, face);
    if (rc) { printf("err %d %s\n", rc, ttf_strerror(rc)); s->open = 0; return 1; }
    s->open = 1;
    printf("ok faces=%d glyphs=%d upem=%d cff=%d\n", ttf_face_count(d, n), s->font.num_glyphs, s->font.units_per_em, s->font.is_cff);
    return 0;
}

static Slot *need_slot(const char *name) {
    Slot *s = find_slot(name);
    if (!s || !s->open) { printf("err -97 no font %s\n", name); return NULL; }
    return s;
}

static void fmt_num(char *out, size_t cap, float v) {
    snprintf(out, cap, "%.3f", (double)v);
    char *p = out + strlen(out);
    while (p > out && p[-1] == '0') *--p = 0;
    if (p > out && p[-1] == '.') *--p = 0;
    if (!strcmp(out, "-0")) strcpy(out, "0");
}

static void print_outline(const TtfOutline *o) {
    const float *p = o->pts;
    char a[32], b[32], c[32], d[32], e[32], f[32];
    for (uint32_t i = 0; i < o->nops; i++) {
        switch (o->ops[i]) {
        case TTF_OP_MOVE: fmt_num(a, 32, p[0]); fmt_num(b, 32, p[1]); printf("M %s %s\n", a, b); p += 2; break;
        case TTF_OP_LINE: fmt_num(a, 32, p[0]); fmt_num(b, 32, p[1]); printf("L %s %s\n", a, b); p += 2; break;
        case TTF_OP_QUAD:
            fmt_num(a, 32, p[0]); fmt_num(b, 32, p[1]); fmt_num(c, 32, p[2]); fmt_num(d, 32, p[3]);
            printf("Q %s %s %s %s\n", a, b, c, d); p += 4; break;
        case TTF_OP_CUBIC:
            fmt_num(a, 32, p[0]); fmt_num(b, 32, p[1]); fmt_num(c, 32, p[2]); fmt_num(d, 32, p[3]); fmt_num(e, 32, p[4]); fmt_num(f, 32, p[5]);
            printf("C %s %s %s %s %s %s\n", a, b, c, d, e, f); p += 6; break;
        default: printf("Z\n"); break;
        }
    }
}

static void print_bitmap(const TtfBitmap *bm) {
    printf("bitmap %d %d %d %d %d %d %.4f\n", bm->width, bm->height, bm->left, bm->top, bm->bpp, bm->stride, (double)bm->advance);
    for (int y = 0; y < bm->height; y++) {
        for (int x = 0; x < bm->width * bm->bpp; x++) printf("%02x", bm->data[(size_t)y * (size_t)bm->stride + (size_t)x]);
        printf("\n");
    }
    printf("end\n");
}

static unsigned long hash_bitmap(const TtfBitmap *bm) {
    unsigned long h = 1469598103934665603ul;
    for (int y = 0; y < bm->height; y++)
        for (int x = 0; x < bm->width * bm->bpp; x++) { h ^= bm->data[(size_t)y * (size_t)bm->stride + (size_t)x]; h *= 1099511628211ul; }
    return h;
}

static void write_pnm(const char *path, const uint32_t *px, int w, int h, int gray) {
    FILE *f = fopen(path, "wb");
    if (!f) { printf("err -99 cannot write %s\n", path); return; }
    fprintf(f, "%s\n%d %d\n255\n", gray ? "P5" : "P6", w, h);
    for (int i = 0; i < w * h; i++) {
        if (gray) fputc((px[i] >> 16) & 255, f);
        else { fputc((px[i] >> 16) & 255, f); fputc((px[i] >> 8) & 255, f); fputc(px[i] & 255, f); }
    }
    fclose(f);
}

/* ------------------------------------------------------------------ checks over whole fonts */

/* Area enclosed by an outline in font units^2, from a fine flattening that shares nothing with the rasteriser. */
static double outline_area(const TtfOutline *o) {
    const float *p = o->pts;
    double area = 0, cx = 0, cy = 0, sx = 0, sy = 0;
    int open = 0;
    for (uint32_t i = 0; i < o->nops; i++) {
        double nx = cx, ny = cy;
        switch (o->ops[i]) {
        case TTF_OP_MOVE:
            if (open) area += cx * sy - sx * cy;
            cx = sx = p[0]; cy = sy = p[1]; open = 1; p += 2;
            continue;
        case TTF_OP_LINE:
            nx = p[0]; ny = p[1]; area += cx * ny - nx * cy; p += 2; break;
        case TTF_OP_QUAD:
            for (int k = 1; k <= 64; k++) {
                double t = k / 64.0, m = 1 - t;
                double x = m * m * cx + 2 * m * t * p[0] + t * t * p[2], y = m * m * cy + 2 * m * t * p[1] + t * t * p[3];
                area += nx * y - x * ny;
                nx = x; ny = y;
            }
            p += 4;
            break;
        case TTF_OP_CUBIC:
            for (int k = 1; k <= 64; k++) {
                double t = k / 64.0, m = 1 - t;
                double x = m * m * m * cx + 3 * m * m * t * p[0] + 3 * m * t * t * p[2] + t * t * t * p[4];
                double y = m * m * m * cy + 3 * m * m * t * p[1] + 3 * m * t * t * p[3] + t * t * t * p[5];
                area += nx * y - x * ny;
                nx = x; ny = y;
            }
            p += 6;
            break;
        default:
            if (open) area += cx * sy - sx * cy;
            cx = sx; cy = sy; open = 0;
            continue;
        }
        cx = nx; cy = ny;
    }
    if (open) area += cx * sy - sx * cy;
    return area / 2.0;
}

static int cmd_areacheck(Slot *s, float size, int limit) {
    int n = s->font.num_glyphs < limit ? s->font.num_glyphs : limit, bad = 0, errs = 0, checked = 0;
    double maxrel = 0;
    double scale = (double)size / s->font.units_per_em;
    for (int g = 0; g < n; g++) {
        const TtfOutline *o;
        if (ttf_glyph_outline(g_raster, &s->font, (uint32_t)g, &o)) { errs++; continue; }
        if (o->nops == 0) continue;
        double area = outline_area(o);
        if (area < 0) area = -area;
        area *= scale * scale;
        TtfRenderParams p = { size, 0, 0, 0, TTF_MODE_GRAY };
        TtfBitmap bm;
        if (ttf_render(g_raster, &s->font, (uint32_t)g, &p, &bm)) { errs++; continue; }
        double ink = 0;
        for (int y = 0; y < bm.height; y++) for (int x = 0; x < bm.width; x++) ink += bm.data[(size_t)y * (size_t)bm.stride + (size_t)x];
        ink /= 255.0;
        checked++;
        double diff = ink > area ? ink - area : area - ink;
        double rel = diff / (area > 1 ? area : 1);
        /* the ink is the sum of pixels rounded to 1/255 and of curves flattened to 0.02 px: both errors grow with the outline's length,
         * so allow a little per pixel of width and height. The point is to catch missing contours and wrong winding, not 1% noise. */
        double allow = 0.05 + 0.02 * (double)(bm.width + bm.height);
        if (diff > allow && rel > 0.02) { bad++; if (bad <= 200) printf("bad gid=%d ink=%.3f area=%.3f\n", g, ink, area); }
        if (rel > maxrel) maxrel = rel;
    }
    printf("areacheck checked=%d bad=%d errors=%d maxrel=%.4f\n", checked, bad, errs, maxrel);
    return 0;
}

/* The decoded outline's bounds against the box in the glyph header. Fonts differ in what they store there: most give the bounds of all
 * points (control points too), some the bounds of the curve itself, which can be smaller. So "larger" (our control-point bounds exceed the
 * header's) is normal for those fonts, while "smaller" (we found less than the font says is there) would mean geometry went missing. */
static int cmd_boxcheck(Slot *s) {
    int mismatch = 0, errs = 0, compared = 0, smaller = 0;
    Slot *sl = s;
    if (sl->font.is_cff) { printf("boxcheck skipped cff\n"); return 0; }
    for (int g = 0; g < sl->font.num_glyphs; g++) {
        const TtfOutline *o;
        if (ttf_glyph_outline(g_raster, &sl->font, (uint32_t)g, &o)) { errs++; continue; }
        /* header box: loca -> glyf */
        const uint8_t *loca = sl->font.data + sl->font.loca.off, *glyf = sl->font.data + sl->font.glyf.off;
        uint32_t a, b;
        if (sl->font.loca_long) {
            a = (uint32_t)loca[4 * g] << 24 | (uint32_t)loca[4 * g + 1] << 16 | (uint32_t)loca[4 * g + 2] << 8 | loca[4 * g + 3];
            b = (uint32_t)loca[4 * g + 4] << 24 | (uint32_t)loca[4 * g + 5] << 16 | (uint32_t)loca[4 * g + 6] << 8 | loca[4 * g + 7];
        } else {
            a = 2u * ((uint32_t)loca[2 * g] << 8 | loca[2 * g + 1]);
            b = 2u * ((uint32_t)loca[2 * g + 2] << 8 | loca[2 * g + 3]);
        }
        if (b <= a || a + 10 > sl->font.glyf.len) { if (o->nops) mismatch++; continue; }
        const uint8_t *h = glyf + a;
        int hb[4];
        for (int i = 0; i < 4; i++) hb[i] = (int16_t)((h[2 + 2 * i] << 8) | h[3 + 2 * i]);
        if (o->nops == 0) continue;
        compared++;
        float got[4] = { o->xmin, o->ymin, o->xmax, o->ymax };
        int diff = 0, small = 0;
        for (int i = 0; i < 4; i++) { float d = got[i] - (float)hb[i]; if (d > 1.01f || d < -1.01f) diff = 1; }
        if (got[0] > (float)hb[0] + 1.01f || got[1] > (float)hb[1] + 1.01f || got[2] < (float)hb[2] - 1.01f || got[3] < (float)hb[3] - 1.01f) small = 1;
        if (diff) {
            mismatch++;
            smaller += small;
            if (mismatch <= 10 || small) printf("box gid=%d header=%d,%d,%d,%d got=%.1f,%.1f,%.1f,%.1f%s\n", g, hb[0], hb[1], hb[2], hb[3], got[0], got[1], got[2], got[3], small ? " SMALLER" : "");
        }
    }
    printf("boxcheck glyphs=%d compared=%d mismatch=%d smaller=%d errors=%d\n", sl->font.num_glyphs, compared, mismatch, smaller, errs);
    return 0;
}

/* The outlines of glyphs first..first+count-1, as "g GID NOPS" and then the path lines (so a test can compare a whole font with another reader). */
static int cmd_dump(Slot *s, int first, int count) {
    int n = s->font.num_glyphs;
    if (first < 0) first = 0;
    int last = count > 0 && first + count < n ? first + count : n;
    for (int g = first; g < last; g++) {
        const TtfOutline *o;
        int rc = ttf_glyph_outline(g_raster, &s->font, (uint32_t)g, &o);
        if (rc) { printf("g %d err %d\n", g, rc); continue; }
        printf("g %d %u\n", g, o->nops);
        print_outline(o);
    }
    printf("end\n");
    return 0;
}

static int cmd_sweep(Slot *s, float size, int mode, int first, int count) {
    int n = s->font.num_glyphs, errs = 0, empty = 0, drawn = 0;
    unsigned long long ink = 0;
    if (first < 0) first = 0;
    int last = count > 0 && first + count < n ? first + count : n;
    for (int g = first; g < last; g++) {
        TtfRenderParams p = { size, 0, 0, 0, mode };
        TtfBitmap bm;
        int rc = ttf_render(g_raster, &s->font, (uint32_t)g, &p, &bm);
        if (rc) { errs++; continue; }
        if (bm.width == 0) { empty++; continue; }
        drawn++;
        for (int y = 0; y < bm.height; y++) for (int x = 0; x < bm.width * bm.bpp; x++) ink += bm.data[(size_t)y * (size_t)bm.stride + (size_t)x];
    }
    printf("sweep glyphs=%d drawn=%d empty=%d errors=%d ink=%llu\n", last - first, drawn, empty, errs, ink);
    return 0;
}

/* ------------------------------------------------------------------ the fuzzer */

static uint64_t g_rng;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 11); }

static void on_alarm(int sig) {
    (void)sig;
    static const char m[] = "HANG: an iteration took too long\n";
    ssize_t r = write(1, m, sizeof m - 1);
    (void)r;
    _exit(99);
}

static void mutate(uint8_t *d, size_t *n, size_t orig) {
    int rounds = 1 + (int)(rnd() % 4);
    for (int k = 0; k < rounds && *n > 12; k++) {
        size_t lo = 0, hi = *n;
        if (rnd() & 1) {                                           /* aim inside one table of the directory */
            uint32_t nt = (d[4] << 8) | d[5];
            if (nt && 12 + 16u * nt <= *n) {
                uint32_t t = rnd() % nt;
                const uint8_t *e = d + 12 + 16 * t;
                uint32_t off = (uint32_t)e[8] << 24 | (uint32_t)e[9] << 16 | (uint32_t)e[10] << 8 | e[11];
                uint32_t len = (uint32_t)e[12] << 24 | (uint32_t)e[13] << 16 | (uint32_t)e[14] << 8 | e[15];
                if (off < *n && len) { lo = off; hi = off + len < *n ? off + len : *n; }
            }
        }
        if (hi <= lo) { lo = 0; hi = *n; }
        size_t span = hi - lo;
        switch (rnd() % 8) {
        case 0: for (int i = 0, c = 1 + rnd() % 8; i < c; i++) d[lo + rnd() % span] ^= (uint8_t)(1u << (rnd() & 7)); break;
        case 1: for (int i = 0, c = 1 + rnd() % 8; i < c; i++) d[lo + rnd() % span] = (uint8_t)rnd(); break;
        case 2: {                                                  /* a 16-bit field becomes an extreme */
            static const uint16_t ext[] = { 0, 1, 0x7FFF, 0x8000, 0xFFFF, 0x00FF, 0x0100, 0xFFFE };
            size_t p = lo + rnd() % span;
            if (p + 1 < *n) { uint16_t v = ext[rnd() % 8]; d[p] = (uint8_t)(v >> 8); d[p + 1] = (uint8_t)v; }
            break;
        }
        case 3: {                                                  /* a 32-bit field becomes an extreme */
            static const uint32_t ext[] = { 0, 1, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu, 0x0000FFFF, 0x00010000, 0xFFFFFFFEu };
            size_t p = lo + rnd() % span;
            if (p + 3 < *n) { uint32_t v = ext[rnd() % 8]; d[p] = (uint8_t)(v >> 24); d[p + 1] = (uint8_t)(v >> 16); d[p + 2] = (uint8_t)(v >> 8); d[p + 3] = (uint8_t)v; }
            break;
        }
        case 4: if (rnd() % 4 == 0) *n = 12 + rnd() % (*n - 12); break;                         /* truncate */
        case 5: { size_t p = lo + rnd() % span, c = 1 + rnd() % 64; for (size_t i = 0; i < c && p + i < *n; i++) d[p + i] = 0; break; }
        case 6: { size_t p = lo + rnd() % span, c = 1 + rnd() % 64; for (size_t i = 0; i < c && p + i < *n; i++) d[p + i] = 0xFF; break; }
        default: { size_t a = lo + rnd() % span, b = lo + rnd() % span, c = 1 + rnd() % 32;           /* copy a block over another */
            if (a + c <= *n && b + c <= *n) memmove(d + b, d + a, c);
            break; }
        }
    }
    (void)orig;
}

static void exercise(const TtfFont *f, TtfRaster *r, TtfCache *cache) {
    static const uint32_t cps[] = { 0x20, 0x41, 0x56, 0x66, 0x69, 0x7E, 0xE9, 0x3042, 0x4E00, 0xFFFD, 0x1F600, 0x10FFFF, 0xF041 };
    uint32_t gl[16];
    int ng = 0;
    for (size_t i = 0; i < sizeof cps / sizeof *cps; i++) {
        uint32_t g = ttf_glyph_for(f, cps[i]);
        if (ng < 16) gl[ng++] = g;
    }
    for (int i = 0; i < 6; i++) {
        uint32_t g = ttf_glyph_for(f, rnd());
        if (ng < 16) gl[ng++] = g;
    }
    char nm[64];
    for (int id = 0; id < 8; id++) ttf_name(f, id, nm, sizeof nm);
    TtfMetrics m;
    ttf_metrics(f, 16.0f, &m);
    uint32_t seq[8];
    for (int i = 0; i + 1 < ng; i++) {
        (void)ttf_advance_units(f, gl[i]);
        (void)ttf_lsb_units(f, gl[i]);
        (void)ttf_kern_units(f, gl[i], gl[i + 1]);
        seq[0] = gl[i]; seq[1] = gl[i + 1]; seq[2] = gl[i]; seq[3] = gl[(i + 2) % ng];
        uint32_t out;
        (void)ttf_ligature(f, seq, 4, &out);
    }
    (void)ttf_kern_units(f, rnd() % 70000, rnd() % 70000);
    /* glyphs: the mapped ones, a few random ones, and the last (index arithmetic at the end of every table) */
    int nglyphs = f->num_glyphs;
    for (int i = 0; i < ng + 8; i++) {
        uint32_t g = i < ng ? gl[i] : (i == ng ? (uint32_t)(nglyphs - 1) : rnd() % (uint32_t)(nglyphs + 3));
        TtfRenderParams p = { (i & 1) ? 41.5f : 13.0f, (float)(rnd() % 100) / 100.0f, (float)(rnd() % 100) / 100.0f, (i & 2) ? 0.2f : 0.0f, (i & 4) ? TTF_MODE_LCD : TTF_MODE_GRAY };
        TtfBitmap bm;
        int rc = ttf_render(r, f, g, &p, &bm);
        if (rc == TTF_OK && bm.width > 0) {
            if (bm.width > TTF_MAX_DIM || bm.height > TTF_MAX_DIM || !bm.data || bm.stride != bm.width * bm.bpp) { printf("BAD BITMAP\n"); abort(); }
            volatile uint8_t sink = bm.data[(size_t)(bm.height - 1) * (size_t)bm.stride + (size_t)bm.stride - 1];
            (void)sink;
        }
        (void)ttf_cache_get(cache, r, f, g, 14.0f, (int)(rnd() % 4), (i & 1) ? TTF_MODE_LCD : TTF_MODE_GRAY);
    }
    TtfFontSet set;
    ttf_set_init(&set);
    ttf_set_add(&set, f);
    static const char text[] = "Afi fi AV \xE3\x81\x82\xE4\xB8\x80 \xEF\xBF\xBD x\xCC\x81 office";
    TtfGlyphPos pos[64];
    (void)ttf_layout(&set, 18.0f, text, sizeof text - 1, TTF_LAYOUT_DEFAULT, pos, 64, NULL);
    uint32_t px[40 * 24];
    memset(px, 0, sizeof px);
    TtfSurface surf = { px, 40, 24, 40 };
    TtfDrawStyle st;
    memset(&st, 0, sizeof st);
    st.color = 0xFFFFFF;
    ttf_draw_text(cache, r, &set, 12.0f, "AVfi", 4, 1.25f, 16, &st, &surf);
}

static int cmd_fuzz(Slot *s, unsigned long seed, int iters) {
    uint8_t *work = malloc(s->size + 16);
    int opened = 0, failed = 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    size_t base_alloc = g_cur;
    g_max_request = 0;
    g_peak = g_cur;
    for (int it = 0; it < iters; it++) {
        g_rng = seed * 0x9E3779B97F4A7C15ull + (uint64_t)it * 0xD1B54A32D192ED03ull + 88172645463325252ull;
        for (int i = 0; i < 4; i++) rnd();
        size_t n = s->size;
        memcpy(work, s->data, n);
        if (it > 0) mutate(work, &n, s->size);                    /* iteration 0 is the unmodified font */
        alarm(20);
        TtfFont f;
        int faces = ttf_face_count(work, n);
        int rc = ttf_open(&f, work, n, faces > 1 ? (int)(rnd() % (uint32_t)faces) : 0);
        if (rc == TTF_OK) {
            opened++;
            TtfRaster *r = ttf_raster_new();
            TtfCache *c = ttf_cache_new(64 * 1024, 64);
            exercise(&f, r, c);
            ttf_cache_free(c);
            ttf_raster_free(r);
        } else failed++;
        alarm(0);
        if (g_cur != base_alloc) { printf("LEAK iteration %d: %zu bytes outstanding\n", it, g_cur - base_alloc); return 3; }
    }
    free(work);
    printf("fuzz iterations=%d opened=%d refused=%d peak_alloc=%zu max_request=%zu\n", iters, opened, failed, g_peak - base_alloc, g_max_request);
    return 0;
}

/* ------------------------------------------------------------------ speed */

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

static int cmd_bench(Slot *s, float size, int ms, int mode) {
    /* the glyphs of the printable ASCII range, as a text would use them */
    uint32_t gl[96];
    int n = 0;
    for (uint32_t c = 0x21; c < 0x7F; c++) { uint32_t g = ttf_glyph_for(&s->font, c); if (g) gl[n++] = g; }
    if (n == 0) for (uint32_t g = 1; g < (uint32_t)s->font.num_glyphs && n < 96; g++) gl[n++] = g;
    if (n == 0) { printf("err -1 no glyphs\n"); return 1; }
    long count = 0;
    double t0 = now_ms(), t1 = t0;
    TtfBitmap bm;
    while (t1 - t0 < ms) {
        for (int i = 0; i < n; i++) {
            TtfRenderParams p = { size, (float)(i & 3) * 0.25f, 0, 0, mode };
            ttf_render(g_raster, &s->font, gl[i], &p, &bm);
        }
        count += n;
        t1 = now_ms();
    }
    printf("bench size=%.1f glyphs=%ld seconds=%.3f glyphs_per_second=%.0f\n", (double)size, count, (t1 - t0) / 1000.0, (double)count / ((t1 - t0) / 1000.0));
    return 0;
}

/* ------------------------------------------------------------------ the Kappa shim (user/lib/kappa/src/ttftext.c), built in with -DTTFTOOL_KAPPA */

#ifdef TTFTOOL_KAPPA
#include <kappa_ttf.h>

/* ktext OUT SCALE W H X Y SIZE FG BG FONTFILE TEXT [CX CY CW CH]   draws with kt_text on a W x H (logical) canvas of the given scale
 * ktextin OUT SCALE W H RX RY RW RH SIZE FG BG FONTFILE ALIGN TEXT  draws with kt_text_in inside a rectangle (ALIGN 0 left, 1 centre, 2 right) */
static int cmd_ktext(int in_rect, int argc, char **argv) {
    int need = in_rect ? 15 : 12;
    if (argc < need) return printf("err -1 usage\n"), 1;
    int scale = atoi(argv[2]), w = atoi(argv[3]), h = atoi(argv[4]);
    if (scale < 1 || w < 1 || h < 1 || w > 4000 || h > 4000) return printf("err -7 size\n"), 1;
    int i = 5, rx = 0, ry = 0, rw = 0, rh = 0, x = 0, y = 0;
    if (in_rect) { rx = atoi(argv[i++]); ry = atoi(argv[i++]); rw = atoi(argv[i++]); rh = atoi(argv[i++]); }
    else { x = atoi(argv[i++]); y = atoi(argv[i++]); }
    float size = (float)atof(argv[i++]);
    uint32_t fg = (uint32_t)strtoul(argv[i++], NULL, 16), bg = (uint32_t)strtoul(argv[i++], NULL, 16);
    const char *font = argv[i++];
    int align = in_rect ? atoi(argv[i++]) : 0;
    size_t len;
    char *text = text_arg(argv[i++], &len);
    size_t pw = (size_t)w * (size_t)scale, ph = (size_t)h * (size_t)scale;
    uint32_t *px = malloc(pw * ph * 4);
    for (size_t k = 0; k < pw * ph; k++) px[k] = bg;
    KCanvas c;
    kc_init_scaled(&c, px, w, h, (int)pw, scale);
    KTtf *t = kt_new();
    int rc = kt_add_file(t, font, 0);
    if (rc) { printf("err %d %s\n", rc, ttf_strerror(rc)); free(px); free(text); kt_free(t); return 1; }
    if (!in_rect && argc >= i + 4) kc_set_clip(&c, kr(atoi(argv[i]), atoi(argv[i + 1]), atoi(argv[i + 2]), atoi(argv[i + 3])));
    if (in_rect) {
        kt_text_in(&c, t, size, kr(rx, ry, rw, rh), text, fg, (KAlign)align);
        printf("ktextin width=%d lineheight=%d ascent=%d\n", kt_text_width(t, size, text), kt_line_height(t, size), kt_ascent(t, size));
    } else {
        int adv = kt_text(&c, t, size, x, y, text, fg);
        float psize = size * (float)scale;
        printf("ktext advance=%d lineheight=%d ascent=%d physical_ascent=%d\n", adv, kt_line_height(t, size), kt_ascent(t, size), kt_ascent(t, psize));
    }
    write_pnm(argv[1], px, (int)pw, (int)ph, 0);
    kt_free(t);
    free(px);
    free(text);
    return 0;
}

/* The toolkit's own text API (canvas.c: kc_text and friends) with the system UI font, which kt_ui_ready() loads from $KAPPA_UI_FONT (the tests point it at a
 * font they made, or at nothing). One tool process, one font: the environment is read when the first text is drawn.
 *
 *   kui draw OUT SCALE W H X Y FG BG MODE TEXT [CX CY CW CH]   MODE 0 kc_text, 1 kc_text_bold, 2 kc_text_shadow (shadow black), 3 kc_text_scaled x1,
 *                                                               4 kc_text_scaled x2, 5 kc_text_n with the first BYTES bytes (TEXT then "BYTES:TEXT")
 *   kui glyph OUT SCALE W H X Y FG BG CODEPOINT               the bitmap cell, for reference
 *   kui in OUT SCALE W H RX RY RW RH FG BG ALIGN TEXT         kc_text_in
 *   kui measure TEXT [BYTES [X...]]                           width, width of the first BYTES bytes, kc_text_hit for each X
 *   kui bitmap 0|1                                            kc_text_use_bitmap */
static int cmd_kui(int argc, char **argv) {
    if (argc < 2) return printf("err -1 usage: kui draw|glyph|in|measure|bitmap ...\n"), 1;
    const char *sub = argv[1];
    if (!strcmp(sub, "bitmap")) { kc_text_use_bitmap(argc > 2 && atoi(argv[2])); printf("ok\n"); return 0; }
    if (!strcmp(sub, "bench")) {                                    /* kui bench MS SCALE TEXT: microseconds per kc_text call (glyph cache warm) on a 480 x 32 canvas */
        if (argc < 5) return printf("err -1 usage: kui bench MS SCALE TEXT\n"), 1;
        double ms = atof(argv[2]);
        int scale = atoi(argv[3]);
        size_t len;
        char *text = text_arg(argv[4], &len);
        if (scale < 1 || scale > 4) return printf("err -7 scale\n"), 1;
        size_t pw = 480u * (unsigned)scale, ph = 32u * (unsigned)scale;
        uint32_t *px = calloc(pw * ph, 4);
        KCanvas c;
        kc_init_scaled(&c, px, 480, 32, (int)pw, scale);
        for (int i = 0; i < 50; i++) kc_text(&c, 4, 8, text, 0x1A1A24);        /* warm the cache */
        long n = 0;
        double t0 = now_ms(), t1 = t0;
        while (t1 - t0 < ms) {
            for (int i = 0; i < 200; i++) kc_text(&c, 4 + (i & 7), 8, text, 0x1A1A24);
            n += 200;
            t1 = now_ms();
        }
        printf("kui bench calls=%ld microseconds_per_call=%.2f chars=%zu proportional=%d\n", n, (t1 - t0) * 1000.0 / (double)n, strlen(text), kc_text_is_proportional());
        free(px);
        free(text);
        return 0;
    }
    if (!strcmp(sub, "measure")) {
        if (argc < 3) return printf("err -1 usage\n"), 1;
        size_t len;
        char *text = text_arg(argv[2], &len);
        size_t bytes = argc > 3 ? (size_t)atol(argv[3]) : len;
        printf("kui width=%d prefix=%d proportional=%d boldwidth=%d hit=", kc_text_width(text), kc_text_width_n(text, bytes), kc_text_is_proportional(), kc_text_width_bold(text));
        for (int i = 4; i < argc; i++) printf("%s%zu", i > 4 ? "," : "", kc_text_hit(text, atoi(argv[i])));
        printf("\n");
        free(text);
        return 0;
    }
    int in_rect = !strcmp(sub, "in");
    int need = in_rect ? 14 : !strcmp(sub, "glyph") ? 11 : 12;
    if ((strcmp(sub, "draw") && strcmp(sub, "glyph") && !in_rect) || argc < need) return printf("err -1 usage\n"), 1;
    int scale = atoi(argv[3]), w = atoi(argv[4]), h = atoi(argv[5]);
    if (scale < 1 || w < 1 || h < 1 || w > 4000 || h > 4000) return printf("err -7 size\n"), 1;
    int i = 6, x = 0, y = 0, rx = 0, ry = 0, rw = 0, rh = 0;
    if (in_rect) { rx = atoi(argv[i++]); ry = atoi(argv[i++]); rw = atoi(argv[i++]); rh = atoi(argv[i++]); }
    else { x = atoi(argv[i++]); y = atoi(argv[i++]); }
    uint32_t fg = (uint32_t)strtoul(argv[i++], NULL, 16), bg = (uint32_t)strtoul(argv[i++], NULL, 16);
    int mode = 0, align = 0;
    if (in_rect) align = atoi(argv[i++]);
    else if (strcmp(sub, "glyph")) mode = atoi(argv[i++]);
    size_t pw = (size_t)w * (size_t)scale, ph = (size_t)h * (size_t)scale;
    uint32_t *px = malloc(pw * ph * 4);
    for (size_t k = 0; k < pw * ph; k++) px[k] = bg;
    KCanvas c;
    kc_init_scaled(&c, px, w, h, (int)pw, scale);
    if (!strcmp(sub, "glyph")) {
        kc_glyph(&c, x, y, (uint32_t)strtoul(argv[i], NULL, 0), fg, 1);
        printf("kui glyph\n");
    } else {
        size_t len;
        char *text = text_arg(argv[i++], &len);
        if (!in_rect && argc >= i + 4) kc_set_clip(&c, kr(atoi(argv[i]), atoi(argv[i + 1]), atoi(argv[i + 2]), atoi(argv[i + 3])));
        if (in_rect) {
            kc_text_in(&c, kr(rx, ry, rw, rh), text, fg, (KAlign)align);
            printf("kui in width=%d\n", kc_text_width(text));
        } else {
            int adv = 0;
            if (mode == 0) adv = kc_text(&c, x, y, text, fg);
            else if (mode == 1) adv = kc_text_bold(&c, x, y, text, fg);
            else if (mode == 2) adv = kc_text_shadow(&c, x, y, text, fg, 0x000000);
            else if (mode == 3) adv = kc_text_scaled(&c, x, y, text, fg, 1);
            else if (mode == 4) adv = kc_text_scaled(&c, x, y, text, fg, 2);
            else {
                char *colon = strchr(text, ':');
                size_t n = colon ? (size_t)atol(text) : len;
                adv = kc_text_n(&c, x, y, colon ? colon + 1 : text, n, fg);
            }
            printf("kui draw advance=%d proportional=%d\n", adv, kc_text_is_proportional());
        }
        free(text);
    }
    write_pnm(argv[2], px, (int)pw, (int)ph, 0);
    free(px);
    return 0;
}
#endif

/* ------------------------------------------------------------------ the command interpreter */

static const char usage[] =
    "commands: load NAME PATH [FACE] | info NAME | glyph NAME CP... | outline NAME GID | render NAME GID SIZE [SUBX [MODE [SKEW [SUBY]]]]\n"
    "          kern NAME L R | lig NAME G... | set NAME... | layout SIZE FLAGS TEXT | width SIZE FLAGS TEXT | utf8dec HEX | utf8enc CP\n"
    "          cache new BYTES ENTRIES | cache get NAME GID SIZE SUB MODE | cache stats | cache clear | cache free\n"
    "          draw OUT FONTS SIZE TEXT | drawx OUT SIZE W H X BASELINE FG BG MODE FLAGS GAMMA CONTRAST TEXT | lut FG BG GAMMA CONTRAST\n"
    "          areacheck NAME SIZE [LIMIT] | boxcheck NAME | dump NAME [FIRST [COUNT]] | metrics NAME [FIRST [COUNT]] | sweep NAME SIZE [MODE [FIRST [COUNT]]]\n"
    "          fuzz NAME SEED ITERS | bench NAME SIZE MS [MODE]\n"
    "          alloclimit BYTES | allocroom BYTES | failat N | allocstats | leaks | rasternew | usecache 0|1 | name NAME ID [CAP] | echo ...\n"
    "          (with -DTTFTOOL_KAPPA: ktext, ktextin, kui draw|glyph|in|measure|bitmap, see cmd_ktext and cmd_kui)\n";

static int cmd_info(Slot *s) {
    const TtfFont *f = &s->font;
    char fam[128] = "", sty[128] = "", full[128] = "", ps[128] = "";
    ttf_name(f, TTF_NAME_FAMILY, fam, sizeof fam);
    ttf_name(f, TTF_NAME_SUBFAMILY, sty, sizeof sty);
    ttf_name(f, TTF_NAME_FULL, full, sizeof full);
    ttf_name(f, TTF_NAME_POSTSCRIPT, ps, sizeof ps);
    printf("family=%s|style=%s|full=%s|ps=%s\n", fam, sty, full, ps);
    printf("upem=%d glyphs=%d cff=%d cid=%d bbox=%d,%d,%d,%d\n", f->units_per_em, f->num_glyphs, f->is_cff, f->cff_cid, f->bbox[0], f->bbox[1], f->bbox[2], f->bbox[3]);
    printf("hhea=%d,%d,%d typo=%d,%d,%d win=%d,%d os2=%d usetypo=%d xh=%d caph=%d\n", f->hhea_ascent, f->hhea_descent, f->hhea_line_gap, f->typo_ascent,
           f->typo_descent, f->typo_line_gap, f->win_ascent, f->win_descent, f->has_os2, f->use_typo_metrics, f->x_height, f->cap_height);
    printf("underline=%d,%d strike=%d,%d weight=%d width=%d bold=%d italic=%d fixed=%d\n", f->underline_pos, f->underline_thick, f->strike_pos, f->strike_thick,
           f->weight_class, f->width_class, f->is_bold, f->is_italic, f->is_fixed_pitch);
    printf("cmap=%d symbol=%d kern=%d gposkern=%d gsubliga=%d hmetrics=%d\n", f->cmap_format, f->cmap_symbol, f->nkern, f->ngpos_kern, f->ngsub_liga, f->num_hmetrics);
    TtfMetrics m;
    ttf_metrics(f, 100.0f, &m);
    printf("metrics100 ascent=%.3f descent=%.3f gap=%.3f height=%.3f ul=%.3f,%.3f st=%.3f,%.3f xh=%.3f caph=%.3f\n", (double)m.ascent, (double)m.descent, (double)m.line_gap,
           (double)m.line_height, (double)m.underline_pos, (double)m.underline_thick, (double)m.strike_pos, (double)m.strike_thick, (double)m.x_height, (double)m.cap_height);
    return 0;
}

static int run(int argc, char **argv) {
    if (argc < 1) return 0;
    const char *c = argv[0];
    if (!g_raster) g_raster = ttf_raster_new();
    if (!strcmp(c, "load")) {
        if (argc < 3) return printf("err -1 usage: load NAME PATH [FACE]\n"), 1;
        return load_font(argv[1], argv[2], argc > 3 ? atoi(argv[3]) : 0);
    }
    if (!strcmp(c, "info")) { Slot *s = argc > 1 ? need_slot(argv[1]) : NULL; return s ? cmd_info(s) : 1; }
    if (!strcmp(c, "echo")) {
        for (int i = 1; i < argc; i++) printf("%s%c", argv[i], i + 1 < argc ? ' ' : '\n');
        if (argc == 1) printf("\n");
        return 0;
    }
    if (!strcmp(c, "name")) {                                       /* name NAME ID [CAP]: the string as UTF-8 hex, and its full length */
        Slot *s = argc > 2 ? need_slot(argv[1]) : NULL;
        if (!s) return 1;
        char buf[256];
        size_t cap = argc > 3 ? (size_t)atoi(argv[3]) : sizeof buf;
        if (cap > sizeof buf) cap = sizeof buf;
        int n = ttf_name(&s->font, atoi(argv[2]), buf, cap);
        if (n < 0) { printf("none\n"); return 0; }
        printf("name %d ", n);
        for (size_t i = 0; cap && buf[i]; i++) printf("%02x", (unsigned char)buf[i]);
        printf("\n");
        return 0;
    }
    if (!strcmp(c, "glyph")) {
        Slot *s = argc > 2 ? need_slot(argv[1]) : NULL;
        if (!s) return 1;
        for (int i = 2; i < argc; i++) {
            uint32_t cp = (uint32_t)parse_num(argv[i]), g = ttf_glyph_for(&s->font, cp);
            printf("cp=%u gid=%u adv=%d lsb=%d\n", cp, g, ttf_advance_units(&s->font, g), ttf_lsb_units(&s->font, g));
        }
        return 0;
    }
    if (!strcmp(c, "outline")) {
        Slot *s = argc > 2 ? need_slot(argv[1]) : NULL;
        if (!s) return 1;
        const TtfOutline *o;
        int rc = ttf_glyph_outline(g_raster, &s->font, (uint32_t)parse_num(argv[2]), &o);
        if (rc) { printf("err %d %s\n", rc, ttf_strerror(rc)); return 1; }
        print_outline(o);
        printf("end\n");
        return 0;
    }
    if (!strcmp(c, "render")) {
        Slot *s = argc > 3 ? need_slot(argv[1]) : NULL;
        if (!s) return 1;
        TtfRenderParams p;
        memset(&p, 0, sizeof p);
        p.size_px = (float)atof(argv[3]);
        p.sub_x = argc > 4 ? (float)atof(argv[4]) : 0;
        p.mode = argc > 5 ? atoi(argv[5]) : TTF_MODE_GRAY;
        p.skew = argc > 6 ? (float)atof(argv[6]) : 0;
        p.sub_y = argc > 7 ? (float)atof(argv[7]) : 0;
        TtfBitmap bm;
        int rc = ttf_render(g_raster, &s->font, (uint32_t)parse_num(argv[2]), &p, &bm);
        if (rc) { printf("err %d %s\n", rc, ttf_strerror(rc)); return 1; }
        print_bitmap(&bm);
        return 0;
    }
    if (!strcmp(c, "kern")) {
        Slot *s = argc > 3 ? need_slot(argv[1]) : NULL;
        if (!s) return 1;
        printf("kern %d\n", ttf_kern_units(&s->font, (uint32_t)parse_num(argv[2]), (uint32_t)parse_num(argv[3])));
        return 0;
    }
    if (!strcmp(c, "lig")) {
        Slot *s = argc > 2 ? need_slot(argv[1]) : NULL;
        if (!s) return 1;
        uint32_t g[16], out = 0;
        int n = 0;
        for (int i = 2; i < argc && n < 16; i++) g[n++] = (uint32_t)parse_num(argv[i]);
        int k = ttf_ligature(&s->font, g, (size_t)n, &out);
        if (k) printf("lig %u %d\n", out, k); else printf("none\n");
        return 0;
    }
    if (!strcmp(c, "set")) {
        ttf_set_init(&g_set);
        for (int i = 1; i < argc; i++) {
            Slot *s = need_slot(argv[i]);
            if (!s) return 1;
            ttf_set_add(&g_set, &s->font);
        }
        printf("ok %d\n", g_set.count);
        return 0;
    }
    if (!strcmp(c, "layout") || !strcmp(c, "width")) {
        if (argc < 4) return printf("err -1 usage\n"), 1;
        size_t len;
        char *text = text_arg(argv[3], &len);
        float size = (float)atof(argv[1]);
        unsigned flags = (unsigned)parse_num(argv[2]);
        TtfGlyphPos pos[256];
        float w;
        size_t n = ttf_layout(&g_set, size, text, len, flags, pos, 256, &w);
        if (!strcmp(c, "layout")) {
            for (size_t i = 0; i < n && i < 256; i++)
                printf("g %u %u %.4f %.4f %u %u\n", pos[i].glyph, pos[i].font, (double)pos[i].x, (double)pos[i].advance, pos[i].cluster, pos[i].flags);
            printf("count %zu\n", n);
        }
        printf("width %.4f\n", (double)ttf_text_width(&g_set, size, text, len, flags));
        (void)w;
        free(text);
        return 0;
    }
    if (!strcmp(c, "utf8dec")) {
        size_t len;
        char *t = text_arg(argc > 1 ? argv[1] : "hex:", &len);
        const char *p = t, *e = t + len;
        while (p < e) printf("%X ", ttf_utf8_next(&p, e));
        printf("\n");
        free(t);
        return 0;
    }
    if (!strcmp(c, "utf8enc")) {
        char b[4];
        int n = ttf_utf8_encode((uint32_t)parse_num(argc > 1 ? argv[1] : "0"), b);
        for (int i = 0; i < n; i++) printf("%02x", (unsigned char)b[i]);
        printf("\n");
        return 0;
    }
    if (!strcmp(c, "cache")) {
        const char *sub = argc > 1 ? argv[1] : "";
        if (!strcmp(sub, "new")) {
            if (g_cache) ttf_cache_free(g_cache);
            g_cache = ttf_cache_new((size_t)parse_num(argv[2]), (size_t)parse_num(argv[3]));
            printf("ok\n");
        } else if (!strcmp(sub, "get") && g_cache && argc > 6) {
            Slot *s = need_slot(argv[2]);
            if (!s) return 1;
            TtfCacheStats a, b;
            ttf_cache_stats(g_cache, &a);
            const TtfBitmap *bm = ttf_cache_get(g_cache, g_raster, &s->font, (uint32_t)parse_num(argv[3]), (float)atof(argv[4]), atoi(argv[5]), atoi(argv[6]));
            ttf_cache_stats(g_cache, &b);
            if (!bm) { printf("err -5 cache get failed\n"); return 1; }
            printf("%s %d %d %d %d %lx\n", b.hits > a.hits ? "hit" : "miss", bm->width, bm->height, bm->left, bm->top, hash_bitmap(bm));
        } else if (!strcmp(sub, "stats") && g_cache) {
            TtfCacheStats st;
            ttf_cache_stats(g_cache, &st);
            printf("hits=%llu misses=%llu evictions=%llu bytes=%zu entries=%zu peak=%zu\n", (unsigned long long)st.hits, (unsigned long long)st.misses,
                   (unsigned long long)st.evictions, st.bytes, st.entries, st.peak_bytes);
        } else if (!strcmp(sub, "clear") && g_cache) { ttf_cache_clear(g_cache); printf("ok\n"); }
        else if (!strcmp(sub, "free") && g_cache) { ttf_cache_free(g_cache); g_cache = NULL; printf("ok\n"); }
        else { printf("err -1 bad cache command\n"); return 1; }
        return 0;
    }
    if (!strcmp(c, "lut")) {
        uint8_t lut[256];
        if (argc < 5) return printf("err -1 usage\n"), 1;
        ttf_coverage_lut(lut, (uint32_t)strtoul(argv[1], NULL, 16), (uint32_t)strtoul(argv[2], NULL, 16), atoi(argv[3]), atoi(argv[4]));
        for (int i = 0; i < 256; i++) printf("%d%c", lut[i], i == 255 ? '\n' : ' ');
        return 0;
    }
    if (!strcmp(c, "draw") || !strcmp(c, "drawx")) {
        /* draw OUT FONTS SIZE TEXT  |  drawx OUT SIZE W H X BASELINE FG BG MODE FLAGS GAMMA CONTRAST TEXT (uses the font set from `set`) */
        TtfFontSet set;
        ttf_set_init(&set);
        int w = 0, h = 0;
        float size, x;
        int baseline;
        uint32_t fg = 0x000000, bg = 0xFFFFFF;
        int mode = TTF_MODE_GRAY, gamma = 0, contrast = 0;
        unsigned flags = TTF_LAYOUT_DEFAULT;
        const char *text;
        if (!strcmp(c, "draw")) {
            if (argc < 5) return printf("err -1 usage: draw OUT FONTS SIZE TEXT\n"), 1;
            char *list = strdup(argv[2]);
            int slot = 0;
            for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
                char *colon = strrchr(tok, ':');
                int face = 0;
                if (colon && isdigit((unsigned char)colon[1])) { face = atoi(colon + 1); *colon = 0; }
                char nm[16];
                snprintf(nm, sizeof nm, "d%d", slot++);
                if (load_font(nm, tok, face)) return 1;
                ttf_set_add(&set, &find_slot(nm)->font);
            }
            free(list);
            size = (float)atof(argv[3]);
            text = argv[4];
            baseline = 0;
            x = 4;
        } else {
            if (argc < 14) return printf("err -1 usage\n"), 1;
            set = g_set;
            size = (float)atof(argv[2]);
            w = atoi(argv[3]); h = atoi(argv[4]);
            x = (float)atof(argv[5]); baseline = atoi(argv[6]);
            fg = (uint32_t)strtoul(argv[7], NULL, 16); bg = (uint32_t)strtoul(argv[8], NULL, 16);
            mode = atoi(argv[9]); flags = (unsigned)parse_num(argv[10]); gamma = atoi(argv[11]); contrast = atoi(argv[12]);
            text = argv[13];
        }
        size_t len;
        char *t = text_arg(text, &len);
        if (!strcmp(c, "draw")) {                                  /* size the picture to the text */
            TtfMetrics m;
            ttf_metrics(set.fonts[0], size, &m);
            float tw = ttf_text_width(&set, size, t, len, flags);
            w = (int)(tw + 8.5f);
            h = (int)(m.ascent + m.descent + 8.5f);
            baseline = (int)(m.ascent + 4.5f);
        }
        uint32_t *px = malloc((size_t)w * (size_t)h * 4);
        for (int i = 0; i < w * h; i++) px[i] = bg;
        TtfSurface surf = { px, w, h, w };
        TtfDrawStyle st;
        memset(&st, 0, sizeof st);
        st.color = fg; st.mode = mode ? TTF_MODE_LCD : TTF_MODE_GRAY; st.lcd_bgr = mode == 2; st.layout_flags = flags;
        uint8_t lut[256];
        if (gamma || contrast) { ttf_coverage_lut(lut, fg, bg, gamma, contrast); st.lut = lut; }
        if (g_use_cache && !g_cache) g_cache = ttf_cache_new(1 << 20, 1024);
        float adv = ttf_draw_text(g_use_cache ? g_cache : NULL, g_raster, &set, size, t, len, x, baseline, &st, &surf);
        const char *out = argv[1];
        size_t ol = strlen(out);
        int gray = ol > 4 && !strcmp(out + ol - 4, ".pgm");
        write_pnm(out, px, w, h, gray);
        printf("drew %d %d advance=%.3f\n", w, h, (double)adv);
        free(px);
        free(t);
        return 0;
    }
    if (!strcmp(c, "areacheck")) { Slot *s = argc > 2 ? need_slot(argv[1]) : NULL; return s ? cmd_areacheck(s, (float)atof(argv[2]), argc > 3 ? atoi(argv[3]) : 1 << 30) : 1; }
    if (!strcmp(c, "metrics")) {                                    /* metrics NAME [FIRST [COUNT]]: "m GID ADVANCE LSB" for each glyph */
        Slot *s = argc > 1 ? need_slot(argv[1]) : NULL;
        if (!s) return 1;
        int first = argc > 2 ? atoi(argv[2]) : 0, count = argc > 3 ? atoi(argv[3]) : 0, n = s->font.num_glyphs;
        int last = count > 0 && first + count < n ? first + count : n;
        for (int g = first < 0 ? 0 : first; g < last; g++) printf("m %d %d %d\n", g, ttf_advance_units(&s->font, (uint32_t)g), ttf_lsb_units(&s->font, (uint32_t)g));
        printf("end\n");
        return 0;
    }
    if (!strcmp(c, "dump")) { Slot *s = argc > 1 ? need_slot(argv[1]) : NULL; return s ? cmd_dump(s, argc > 2 ? atoi(argv[2]) : 0, argc > 3 ? atoi(argv[3]) : 0) : 1; }
    if (!strcmp(c, "boxcheck")) { Slot *s = argc > 1 ? need_slot(argv[1]) : NULL; return s ? cmd_boxcheck(s) : 1; }
    if (!strcmp(c, "sweep")) {
        Slot *s = argc > 2 ? need_slot(argv[1]) : NULL;
        return s ? cmd_sweep(s, (float)atof(argv[2]), argc > 3 ? atoi(argv[3]) : 0, argc > 4 ? atoi(argv[4]) : 0, argc > 5 ? atoi(argv[5]) : 0) : 1;
    }
    if (!strcmp(c, "fuzz")) { Slot *s = argc > 3 ? need_slot(argv[1]) : NULL; return s ? cmd_fuzz(s, parse_num(argv[2]), atoi(argv[3])) : 1; }
    if (!strcmp(c, "bench")) { Slot *s = argc > 3 ? need_slot(argv[1]) : NULL; return s ? cmd_bench(s, (float)atof(argv[2]), atoi(argv[3]), argc > 4 ? atoi(argv[4]) : 0) : 1; }
#ifdef TTFTOOL_KAPPA
    if (!strcmp(c, "ktext")) return cmd_ktext(0, argc, argv);
    if (!strcmp(c, "ktextin")) return cmd_ktext(1, argc, argv);
    if (!strcmp(c, "kui")) return cmd_kui(argc, argv);
#endif
    if (!strcmp(c, "usecache")) { g_use_cache = argc > 1 ? atoi(argv[1]) : 1; printf("ok\n"); return 0; }
    if (!strcmp(c, "rasternew")) {                                  /* a fresh scratch context: its buffers have to be allocated again */
        ttf_raster_free(g_raster);
        g_raster = ttf_raster_new();
        printf(g_raster ? "ok\n" : "err -5 no memory\n");
        return g_raster ? 0 : 1;
    }
    if (!strcmp(c, "failat")) { g_fail_in = argc > 1 ? atol(argv[1]) : 0; printf("ok\n"); return 0; }
    if (!strcmp(c, "allocroom")) { g_limit = g_cur + (argc > 1 ? (size_t)parse_num(argv[1]) : 0); printf("ok\n"); return 0; }
    if (!strcmp(c, "alloclimit")) { g_limit = argc > 1 ? (size_t)parse_num(argv[1]) : (size_t)-1; printf("ok\n"); return 0; }
    if (!strcmp(c, "allocstats")) { printf("cur=%zu peak=%zu max_request=%zu calls=%lu\n", g_cur, g_peak, g_max_request, g_calls); return 0; }
    if (!strcmp(c, "leaks")) {
        if (g_cache) { ttf_cache_free(g_cache); g_cache = NULL; }
        ttf_raster_free(g_raster);
        g_raster = NULL;
        printf("outstanding=%zu\n", g_cur);
        return 0;
    }
    fputs(usage, stdout);
    return 2;
}

int main(int argc, char **argv) {
    int rc = 0;
    if (argc > 1) {
        rc = run(argc - 1, argv + 1);
    } else {
        static char line[1 << 17];
        while (fgets(line, sizeof line, stdin)) {
            char *tok[4096];
            int n = 0;
            for (char *p = line; *p && n < 4096;) {                  /* words separated by blanks; "double quotes" keep blanks inside a word */
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
                if (!*p) break;
                if (*p == '"') {
                    tok[n++] = ++p;
                    while (*p && *p != '"') p++;
                } else {
                    tok[n++] = p;
                    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
                }
                if (*p) *p++ = 0;
            }
            if (n == 0 || tok[0][0] == '#') continue;
            rc |= run(n, tok);
            fflush(stdout);
        }
    }
    for (int i = 0; i < MAX_SLOTS; i++) free(g_slots[i].data);
    if (g_cache) ttf_cache_free(g_cache);
    ttf_raster_free(g_raster);
    return rc ? 1 : 0;
}
