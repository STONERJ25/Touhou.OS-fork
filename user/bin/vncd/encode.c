/*
 * Pixel formats and the encodings of RFC 6143 section 7.7 (Raw, Hextile, ZRLE) plus the Cursor pseudo-encoding (7.8.1).
 * CopyRect is only a header and two numbers and lives in client.c.
 *
 * The frame is 0x00RRGGBB. A viewer may ask for any true-colour format of 8, 16 or 32 bits per pixel (any channel sizes and
 * positions, either byte order) or for an 8-bit colour map; three tables per format turn a frame pixel into the viewer's value
 * (for the colour map the server's own BGR233 cube, sent once with SetColourMapEntries).
 *
 * Every encoder reads its pixels through fetch_pixels(), which also draws the pointer into them for a viewer that does not take
 * the Cursor pseudo-encoding; the frame itself never contains the pointer.
 */
#include "vncd.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------------ output buffers */

void ob_reserve(OutBuf *b, size_t more) {
    if (b->failed) return;
    if (b->len + more <= b->cap) return;
    if (b->off && b->off == b->len) b->off = b->len = 0;
    if (b->len + more <= b->cap) return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + more) cap *= 2;
    uint8_t *d = realloc(b->d, cap);
    if (!d) { b->failed = true; return; }
    b->d = d;
    b->cap = cap;
}

void ob_put(OutBuf *b, const void *p, size_t n) {
    ob_reserve(b, n);
    if (b->failed) return;
    memcpy(b->d + b->len, p, n);
    b->len += n;
}

void ob_u8(OutBuf *b, unsigned v) { uint8_t x = (uint8_t)v; ob_put(b, &x, 1); }
void ob_u16(OutBuf *b, unsigned v) { uint8_t x[2] = { (uint8_t)(v >> 8), (uint8_t)v }; ob_put(b, x, 2); }
void ob_u32(OutBuf *b, uint32_t v) { uint8_t x[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; ob_put(b, x, 4); }
void ob_s32(OutBuf *b, int32_t v) { ob_u32(b, (uint32_t)v); }
size_t ob_pending(const OutBuf *b) { return b->len - b->off; }

void ob_compact(OutBuf *b) {
    if (!b->off) return;
    memmove(b->d, b->d + b->off, b->len - b->off);
    b->len -= b->off;
    b->off = 0;
}

void ob_free(OutBuf *b) { free(b->d); memset(b, 0, sizeof *b); }

/* ------------------------------------------------------------------------------------------------ pixel formats */

const PixelFormat VNC_NATIVE_FORMAT = { 32, 24, 0, 1, 255, 255, 255, 16, 8, 0 };

void pixfmt_parse(PixelFormat *pf, const uint8_t b[16]) {
    pf->bpp = b[0];
    pf->depth = b[1];
    pf->big_endian = b[2] != 0;
    pf->true_colour = b[3] != 0;
    pf->rmax = (uint16_t)(b[4] << 8 | b[5]);
    pf->gmax = (uint16_t)(b[6] << 8 | b[7]);
    pf->bmax = (uint16_t)(b[8] << 8 | b[9]);
    pf->rshift = b[10];
    pf->gshift = b[11];
    pf->bshift = b[12];
}

void pixfmt_write(const PixelFormat *pf, uint8_t b[16]) {
    memset(b, 0, 16);
    b[0] = pf->bpp; b[1] = pf->depth; b[2] = pf->big_endian; b[3] = pf->true_colour;
    b[4] = (uint8_t)(pf->rmax >> 8); b[5] = (uint8_t)pf->rmax;
    b[6] = (uint8_t)(pf->gmax >> 8); b[7] = (uint8_t)pf->gmax;
    b[8] = (uint8_t)(pf->bmax >> 8); b[9] = (uint8_t)pf->bmax;
    b[10] = pf->rshift; b[11] = pf->gshift; b[12] = pf->bshift;
}

static int bits_of(unsigned max) { int n = 0; while (max & 1) { n++; max >>= 1; } return max ? -1 : n; }   /* 2^n - 1 -> n, else -1 */

const char *pixfmt_check(const PixelFormat *pf) {
    if (pf->bpp != 8 && pf->bpp != 16 && pf->bpp != 32) return "bits-per-pixel must be 8, 16 or 32";
    if (pf->depth == 0 || pf->depth > pf->bpp) return "the depth must be between 1 and bits-per-pixel";
    if (!pf->true_colour) return pf->bpp == 8 ? "" : "a colour map is offered with 8 bits per pixel only";
    const unsigned max[3] = { pf->rmax, pf->gmax, pf->bmax }, shift[3] = { pf->rshift, pf->gshift, pf->bshift };
    uint32_t used = 0;
    for (int i = 0; i < 3; i++) {
        int n = bits_of(max[i]);
        if (n <= 0) return "a colour maximum is not of the form 2^N - 1";
        if (shift[i] + (unsigned)n > pf->bpp) return "a colour does not fit in the pixel";
        uint32_t mask = (uint32_t)(((uint64_t)max[i]) << shift[i]);
        if (used & mask) return "two colours share bits";
        used |= mask;
    }
    return "";
}

void pixconv_init(PixConv *c, const PixelFormat *pf) {
    memset(c, 0, sizeof *c);
    c->pf = *pf;
    c->bytes = pf->bpp / 8;
    c->cbytes = c->bytes;
    if (!pf->true_colour) {                             /* the BGR233 cube: index = r(3 bits) g(3) b(2) */
        c->colour_map = true;
        for (int v = 0; v < 256; v++) { c->rt[v] = (uint32_t)(v >> 5) << 5; c->gt[v] = (uint32_t)(v >> 5) << 2; c->bt[v] = (uint32_t)v >> 6; }
        return;
    }
    for (int v = 0; v < 256; v++) {
        c->rt[v] = ((uint32_t)((v * pf->rmax + 127) / 255)) << pf->rshift;
        c->gt[v] = ((uint32_t)((v * pf->gmax + 127) / 255)) << pf->gshift;
        c->bt[v] = ((uint32_t)((v * pf->bmax + 127) / 255)) << pf->bshift;
    }
    /* ZRLE's CPIXEL (RFC 6143 7.7.6): a 32-bit true-colour pixel of depth 24 or less whose colours all sit in the low or the high
     * three bytes travels as those three bytes, in the pixel's byte order. */
    if (pf->bpp == 32 && pf->depth <= 24) {
        uint32_t mask = ((uint32_t)pf->rmax << pf->rshift) | ((uint32_t)pf->gmax << pf->gshift) | ((uint32_t)pf->bmax << pf->bshift);
        if (!(mask & 0xFF000000u)) { c->cbytes = 3; c->cskip = pf->big_endian ? 1 : 0; }
        else if (!(mask & 0xFFu)) { c->cbytes = 3; c->cskip = pf->big_endian ? 0 : 1; }
    }
}

void pix_put(const PixConv *c, uint8_t *out, uint32_t v) {
    switch (c->bytes) {
    case 1: out[0] = (uint8_t)v; break;
    case 2: if (c->pf.big_endian) { out[0] = (uint8_t)(v >> 8); out[1] = (uint8_t)v; } else { out[0] = (uint8_t)v; out[1] = (uint8_t)(v >> 8); } break;
    default:
        if (c->pf.big_endian) { out[0] = (uint8_t)(v >> 24); out[1] = (uint8_t)(v >> 16); out[2] = (uint8_t)(v >> 8); out[3] = (uint8_t)v; }
        else { out[0] = (uint8_t)v; out[1] = (uint8_t)(v >> 8); out[2] = (uint8_t)(v >> 16); out[3] = (uint8_t)(v >> 24); }
    }
}

void pix_put_c(const PixConv *c, uint8_t *out, uint32_t v) {
    if (c->cbytes == c->bytes) { pix_put(c, out, v); return; }
    uint8_t w[4];
    pix_put(c, w, v);
    memcpy(out, w + c->cskip, 3);
}

/* ------------------------------------------------------------------------------------------------ reading the frame */

VRect cursor_rect(const Frame *f) {
    if (f->cur_w <= 0 || f->cur_h <= 0 || !f->cur) return (VRect){ 0, 0, 0, 0 };
    VRect r = { f->ptr_x - f->cur_hotx, f->ptr_y - f->cur_hoty, f->cur_w, f->cur_h };
    return vrect_intersect(r, (VRect){ 0, 0, f->w, f->h });
}

void fetch_pixels(const PixSrc *s, int x, int y, int w, int h, uint32_t *out) {
    const Frame *f = s->f;
    for (int j = 0; j < h; j++) {
        if (f->px) memcpy(out + (size_t)j * w, f->px + (size_t)(y + j) * f->stride + x, (size_t)w * 4);
        else memset(out + (size_t)j * w, 0, (size_t)w * 4);
    }
    if (!s->overlay) return;
    VRect cr = cursor_rect(f);
    VRect v = vrect_intersect(cr, (VRect){ x, y, w, h });
    if (vrect_empty(v)) return;
    int ox = f->ptr_x - f->cur_hotx, oy = f->ptr_y - f->cur_hoty;
    for (int j = v.y; j < v.y + v.h; j++)
        for (int i = v.x; i < v.x + v.w; i++) {
            uint32_t p = f->cur[(size_t)(j - oy) * f->cur_w + (i - ox)];
            if (p >> 24) out[(size_t)(j - y) * w + (i - x)] = p & 0xFFFFFFu;
        }
}

static uint32_t *scratch_pixels(EncState *e, size_t n) {
    if (e->tile_cap < n) {
        uint32_t *t = realloc(e->tile, n * 4);
        if (!t) return NULL;
        e->tile = t;
        e->tile_cap = n;
    }
    return e->tile;
}

void enc_state_free(EncState *e) {
    if (e->z_ready) z_deflate_end(&e->z);
    free(e->tmp);
    free(e->tile);
    memset(e, 0, sizeof *e);
}

void enc_rect_header(OutBuf *o, VRect r, int32_t encoding) {
    ob_u16(o, (unsigned)r.x); ob_u16(o, (unsigned)r.y); ob_u16(o, (unsigned)r.w); ob_u16(o, (unsigned)r.h);
    ob_s32(o, encoding);
}

/* ------------------------------------------------------------------------------------------------ Raw */

void enc_raw(OutBuf *o, const PixConv *c, const PixSrc *s, VRect r, EncState *e) {
    enc_rect_header(o, r, ENC_RAW);
    uint32_t *row = scratch_pixels(e, (size_t)r.w);
    ob_reserve(o, (size_t)r.w * r.h * c->bytes);
    if (!row || o->failed) { o->failed = true; return; }
    for (int y = r.y; y < r.y + r.h; y++) {
        fetch_pixels(s, r.x, y, r.w, 1, row);
        uint8_t *dst = o->d + o->len;
        for (int x = 0; x < r.w; x++) pix_put(c, dst + (size_t)x * c->bytes, pix_value(c, row[x]));
        o->len += (size_t)r.w * c->bytes;
    }
}

/* ------------------------------------------------------------------------------------------------ Hextile */

#define HX_RAW        1
#define HX_BG         2
#define HX_FG         4
#define HX_SUBRECTS   8
#define HX_COLOURED  16

static int cmp_u32(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }

/* The most frequent value of n pixels (n <= 256). */
static uint32_t most_frequent(const uint32_t *px, int n) {
    uint32_t t[256];
    memcpy(t, px, (size_t)n * 4);
    qsort(t, (size_t)n, 4, cmp_u32);
    uint32_t best = t[0];
    int best_n = 0;
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && t[j] == t[i]) j++;
        if (j - i > best_n) { best_n = j - i; best = t[i]; }
        i = j;
    }
    return best;
}

typedef struct { uint8_t x, y, w, h; uint32_t colour; } SubRect;

/* Covers every pixel that is not `bg` with single-coloured rectangles (greedy: the longest run to the right, then as many rows
 * down as are the same). Returns the count, or -1 when there would be more than `max`. */
static int find_subrects(const uint32_t *px, int tw, int th, uint32_t bg, SubRect *out, int max) {
    uint8_t done[256];
    memset(done, 0, sizeof done);
    int n = 0;
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            int i = y * tw + x;
            if (done[i] || px[i] == bg) continue;
            uint32_t c = px[i];
            int w = 1;
            while (x + w < tw && !done[i + w] && px[i + w] == c) w++;
            int h = 1;
            for (; y + h < th; h++) {
                int k = 0;
                while (k < w && !done[(y + h) * tw + x + k] && px[(y + h) * tw + x + k] == c) k++;
                if (k < w) break;
            }
            for (int yy = y; yy < y + h; yy++) memset(done + yy * tw + x, 1, (size_t)w);
            if (n == max) return -1;
            out[n++] = (SubRect){ (uint8_t)x, (uint8_t)y, (uint8_t)w, (uint8_t)h, c };
        }
    return n;
}

void enc_hextile(OutBuf *o, const PixConv *c, const PixSrc *s, VRect r, EncState *e) {
    enc_rect_header(o, r, ENC_HEXTILE);
    uint32_t px[256];
    SubRect sub[255];
    bool have_bg = false, have_fg = false;                  /* only within this rectangle, and never across a raw tile */
    uint32_t bg = 0, fg = 0;
    const int B = c->bytes;
    (void)e;
    for (int ty = r.y; ty < r.y + r.h; ty += 16) {
        int th = r.y + r.h - ty < 16 ? r.y + r.h - ty : 16;
        for (int tx = r.x; tx < r.x + r.w; tx += 16) {
            int tw = r.x + r.w - tx < 16 ? r.x + r.w - tx : 16;
            int n = tw * th;
            fetch_pixels(s, tx, ty, tw, th, px);
            for (int i = 0; i < n; i++) px[i] = pix_value(c, px[i]);      /* compare in the viewer's values: they are what it sees */
            uint32_t c0 = px[0], c1 = c0;
            bool two = false, more = false;
            for (int i = 1; i < n && !more; i++) {
                if (px[i] == c0) continue;
                if (!two) { c1 = px[i]; two = true; }
                else if (px[i] != c1) more = true;
            }
            size_t raw_size = 1 + (size_t)n * B;
            uint8_t hdr[1 + 4 + 4 + 1];
            if (!two) {                                       /* one colour */
                if (have_bg && bg == c0) { ob_u8(o, 0); continue; }
                hdr[0] = HX_BG;
                pix_put(c, hdr + 1, c0);
                ob_put(o, hdr, 1 + (size_t)B);
                bg = c0; have_bg = true;
                continue;
            }
            uint32_t tbg = more ? most_frequent(px, n) : 0;
            if (!more) {                                      /* two colours: the commoner one is the background */
                int k0 = 0;
                for (int i = 0; i < n; i++) k0 += px[i] == c0;
                tbg = k0 * 2 >= n ? c0 : c1;
            }
            int ns = find_subrects(px, tw, th, tbg, sub, 255);
            size_t size = 1;
            unsigned se = HX_SUBRECTS;
            bool set_bg = !have_bg || bg != tbg;
            if (set_bg) { se |= HX_BG; size += (size_t)B; }
            uint32_t tfg = more ? 0 : (tbg == c0 ? c1 : c0);
            bool set_fg = false;
            if (ns > 0) {
                if (more) { se |= HX_COLOURED; size += 1 + (size_t)ns * (B + 2); }
                else {
                    set_fg = !have_fg || fg != tfg;
                    if (set_fg) { se |= HX_FG; size += (size_t)B; }
                    size += 1 + (size_t)ns * 2;
                }
            }
            if (ns < 0 || size >= raw_size) {                 /* raw is no bigger: send it as it is */
                ob_reserve(o, raw_size);
                if (o->failed) return;
                o->d[o->len++] = HX_RAW;
                for (int i = 0; i < n; i++) pix_put(c, o->d + o->len + (size_t)i * B, px[i]);
                o->len += (size_t)n * B;
                have_bg = have_fg = false;
                continue;
            }
            ob_reserve(o, size);
            if (o->failed) return;
            uint8_t *d = o->d + o->len;
            *d++ = (uint8_t)se;
            if (set_bg) { pix_put(c, d, tbg); d += B; bg = tbg; have_bg = true; }
            if (set_fg) { pix_put(c, d, tfg); d += B; fg = tfg; have_fg = true; }
            *d++ = (uint8_t)ns;
            for (int i = 0; i < ns; i++) {
                if (more) { pix_put(c, d, sub[i].colour); d += B; }
                *d++ = (uint8_t)(sub[i].x << 4 | sub[i].y);
                *d++ = (uint8_t)((sub[i].w - 1) << 4 | (sub[i].h - 1));
            }
            if (more) have_fg = false;                        /* SubrectsColoured leaves the foreground undefined */
            o->len = (size_t)(d - o->d);
        }
    }
}

/* ------------------------------------------------------------------------------------------------ ZRLE */

static bool tmp_reserve(EncState *e, size_t need) {
    if (e->tmp_cap >= need) return true;
    size_t cap = e->tmp_cap ? e->tmp_cap : 65536;
    while (cap < need) cap *= 2;
    uint8_t *t = realloc(e->tmp, cap);
    if (!t) return false;
    e->tmp = t;
    e->tmp_cap = cap;
    return true;
}

/* Palette of one tile: up to 127 colours in a small open-addressing table. Returns the count, or 128 for "too many". */
typedef struct { uint32_t key[256]; uint8_t idx[256]; bool used[256]; uint32_t colours[128]; int n; } Palette;

static int pal_find(const Palette *p, uint32_t v) {
    unsigned h = (v * 2654435761u) >> 24;
    while (p->used[h]) { if (p->key[h] == v) return p->idx[h]; h = (h + 1) & 255; }
    return -1;
}

static int build_palette(Palette *p, const uint32_t *px, int n) {
    memset(p->used, 0, sizeof p->used);
    p->n = 0;
    for (int i = 0; i < n; i++) {
        uint32_t v = px[i];
        unsigned h = (v * 2654435761u) >> 24;
        while (p->used[h] && p->key[h] != v) h = (h + 1) & 255;
        if (p->used[h]) continue;
        if (p->n == 127) return p->n = 128;
        p->used[h] = true;
        p->key[h] = v;
        p->idx[h] = (uint8_t)p->n;
        p->colours[p->n++] = v;
    }
    return p->n;
}

static size_t run_bytes(int len) { return (size_t)((len - 1) / 255 + 1); }

static uint8_t *put_run(uint8_t *d, int len) {
    len -= 1;
    while (len >= 255) { *d++ = 255; len -= 255; }
    *d++ = (uint8_t)len;
    return d;
}

/* One 64x64 (or smaller) tile, pixels already in the viewer's values, appended to e->tmp. */
static bool zrle_tile(EncState *e, size_t *pos, const PixConv *c, const uint32_t *px, int tw, int th, Palette *pal) {
    const int n = tw * th, cb = c->cbytes;
    int np = build_palette(pal, px, n);
    int runs = 0, single = 0;
    size_t rle_lens = 0, prle_lens = 0;
    for (int i = 0; i < n;) {
        int j = i + 1;
        while (j < n && px[j] == px[i]) j++;
        int len = j - i;
        runs++;
        rle_lens += run_bytes(len);
        if (len == 1) single++; else prle_lens += run_bytes(len);
        i = j;
    }
    size_t cost_raw = 1 + (size_t)n * cb;
    size_t best = cost_raw;
    int kind = 0;                                               /* 0 raw, 1 solid, 2 packed palette, 128 plain RLE, 130 palette RLE */
    if (np == 1) { best = 1 + (size_t)cb; kind = 1; }
    else {
        if (np <= 16) {
            int bits = np == 2 ? 1 : np <= 4 ? 2 : 4;
            size_t cost = 1 + (size_t)np * cb + (size_t)th * (size_t)((tw * bits + 7) / 8);
            if (cost < best) { best = cost; kind = 2; }
        }
        size_t cost_rle = 1 + (size_t)runs * cb + rle_lens;
        if (cost_rle < best) { best = cost_rle; kind = 128; }
        if (np <= 127) {
            size_t cost_prle = 1 + (size_t)np * cb + (size_t)runs + prle_lens;
            if (cost_prle < best) { best = cost_prle; kind = 130; }
        }
    }
    (void)single;
    if (!tmp_reserve(e, *pos + best + 16)) return false;
    uint8_t *d = e->tmp + *pos;
    switch (kind) {
    case 1:
        *d++ = 1;
        pix_put_c(c, d, px[0]); d += cb;
        break;
    case 2: {
        *d++ = (uint8_t)np;
        for (int i = 0; i < np; i++) { pix_put_c(c, d, pal->colours[i]); d += cb; }
        int bits = np == 2 ? 1 : np <= 4 ? 2 : 4;
        for (int y = 0; y < th; y++) {
            unsigned acc = 0;
            int nb = 0;
            for (int x = 0; x < tw; x++) {
                acc = (acc << bits) | (unsigned)pal_find(pal, px[y * tw + x]);
                nb += bits;
                if (nb == 8) { *d++ = (uint8_t)acc; acc = 0; nb = 0; }
            }
            if (nb) *d++ = (uint8_t)(acc << (8 - nb));         /* every row starts on a byte boundary */
        }
        break;
    }
    case 128:
        *d++ = 128;
        for (int i = 0; i < n;) {
            int j = i + 1;
            while (j < n && px[j] == px[i]) j++;
            pix_put_c(c, d, px[i]); d += cb;
            d = put_run(d, j - i);
            i = j;
        }
        break;
    case 130:
        *d++ = (uint8_t)(128 + np);
        for (int i = 0; i < np; i++) { pix_put_c(c, d, pal->colours[i]); d += cb; }
        for (int i = 0; i < n;) {
            int j = i + 1;
            while (j < n && px[j] == px[i]) j++;
            int idx = pal_find(pal, px[i]);
            if (j - i == 1) *d++ = (uint8_t)idx;
            else { *d++ = (uint8_t)(idx | 128); d = put_run(d, j - i); }
            i = j;
        }
        break;
    default:
        *d++ = 0;
        for (int i = 0; i < n; i++) { pix_put_c(c, d, px[i]); d += cb; }
        break;
    }
    *pos = (size_t)(d - e->tmp);
    return true;
}

int enc_zrle(OutBuf *o, const PixConv *c, const PixSrc *s, VRect r, EncState *e) {
    if (!e->z_ready) {
        memset(&e->z, 0, sizeof e->z);
        if (z_deflate_init(&e->z, e->z_level, Z_ZLIB) != Z_OK) return -1;
        e->z_ready = true;
    }
    uint32_t *px = scratch_pixels(e, 64 * 64);
    Palette *pal = malloc(sizeof *pal);
    if (!px || !pal) { free(pal); return -1; }
    size_t pos = 0;
    for (int ty = r.y; ty < r.y + r.h; ty += 64) {
        int th = r.y + r.h - ty < 64 ? r.y + r.h - ty : 64;
        for (int tx = r.x; tx < r.x + r.w; tx += 64) {
            int tw = r.x + r.w - tx < 64 ? r.x + r.w - tx : 64;
            fetch_pixels(s, tx, ty, tw, th, px);
            for (int i = 0; i < tw * th; i++) px[i] = pix_value(c, px[i]);
            if (!zrle_tile(e, &pos, c, px, tw, th, pal)) { free(pal); return -1; }
        }
    }
    free(pal);
    enc_rect_header(o, r, ENC_ZRLE);
    size_t len_at = o->len;
    ob_u32(o, 0);
    e->z.next_in = e->tmp;
    e->z.avail_in = pos;
    for (;;) {
        ob_reserve(o, 65536);
        if (o->failed) return -1;
        e->z.next_out = o->d + o->len;
        e->z.avail_out = o->cap - o->len;
        size_t before = e->z.avail_out;
        int rc = z_deflate(&e->z, Z_SYNC_FLUSH);
        o->len += before - e->z.avail_out;
        if (rc < 0) return -1;
        if (e->z.avail_in == 0 && e->z.avail_out > 0) break;    /* everything went in and the flush fitted */
    }
    uint32_t zlen = (uint32_t)(o->len - len_at - 4);
    o->d[len_at] = (uint8_t)(zlen >> 24); o->d[len_at + 1] = (uint8_t)(zlen >> 16);
    o->d[len_at + 2] = (uint8_t)(zlen >> 8); o->d[len_at + 3] = (uint8_t)zlen;
    return 0;
}

/* ------------------------------------------------------------------------------------------------ the pointer */

void enc_cursor(OutBuf *o, const PixConv *c, const Frame *f) {
    int w = f->cur_w, h = f->cur_h;
    if (w <= 0 || h <= 0 || !f->cur) {                         /* hidden: one transparent pixel (some viewers dislike 0 x 0) */
        enc_rect_header(o, (VRect){ 0, 0, 1, 1 }, ENC_CURSOR);
        uint8_t z[5] = { 0 };
        ob_put(o, z, (size_t)c->bytes + 1);
        return;
    }
    enc_rect_header(o, (VRect){ f->cur_hotx, f->cur_hoty, w, h }, ENC_CURSOR);
    size_t mask_row = (size_t)(w + 7) / 8;
    ob_reserve(o, (size_t)w * h * c->bytes + mask_row * h);
    if (o->failed) return;
    for (int i = 0; i < w * h; i++) {
        pix_put(c, o->d + o->len, pix_value(c, f->cur[i] & 0xFFFFFFu));
        o->len += (size_t)c->bytes;
    }
    for (int y = 0; y < h; y++) {
        uint8_t *m = o->d + o->len;
        memset(m, 0, mask_row);
        for (int x = 0; x < w; x++) if (f->cur[y * w + x] >> 24) m[x / 8] |= (uint8_t)(0x80 >> (x % 8));
        o->len += mask_row;
    }
}
