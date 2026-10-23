/* Format detection, loading, and the pixel transformations (scale, rotate, flip, composite).
 *
 * WORK IN PROGRESS: written but never run. The decoders it dispatches to (png_decode, bmp_decode, ...) do not exist yet,
 * and scale_area() is a first draft that needs a careful re-read and tests (see docs/MEDIA.md, "State when paused"). */
#include "imgint.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

const char *img_strerror(int rc) {
    switch (rc) {
    case IMG_OK: return "ok";
    case IMG_E_FORMAT: return "not an image this system can read";
    case IMG_E_TRUNC: return "the file ends too early (truncated image)";
    case IMG_E_CORRUPT: return "the image data is damaged";
    case IMG_E_TOOBIG: return "the image is too large";
    case IMG_E_NOMEM: return "out of memory";
    case IMG_E_UNSUPPORTED: return "an image feature that is not supported";
    case IMG_E_IO: return "cannot read the file";
    case IMG_E_ARG: return "bad argument";
    default: return "unknown error";
    }
}

const char *img_format_name(ImgFormat f) {
    switch (f) {
    case IMG_PNG: return "PNG";
    case IMG_JPEG: return "JPEG";
    case IMG_GIF: return "GIF";
    case IMG_BMP: return "BMP";
    case IMG_PNM: return "PNM";
    case IMG_TGA: return "TGA";
    default: return "unknown";
    }
}

ImgFormat img_sniff(const void *data, size_t len) {
    const uint8_t *d = data;
    if (len >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return IMG_PNG;
    if (len >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return IMG_JPEG;
    if (len >= 6 && (!memcmp(d, "GIF87a", 6) || !memcmp(d, "GIF89a", 6))) return IMG_GIF;
    if (len >= 2 && d[0] == 'B' && d[1] == 'M') return IMG_BMP;
    if (len >= 2 && d[0] == 'P' && d[1] >= '1' && d[1] <= '6' && (len < 3 || d[2] == ' ' || d[2] == '\t' || d[2] == '\n' || d[2] == '\r' || d[2] == '#')) return IMG_PNM;
    if (len >= 3 && d[0] == 'P' && d[1] == '7' && d[2] == '\n') return IMG_PNM;
    if (len >= 18 && d[1] <= 1 && (d[2] == 1 || d[2] == 2 || d[2] == 3 || d[2] == 9 || d[2] == 10 || d[2] == 11)) return IMG_TGA;     /* weakest test: last */
    return IMG_UNKNOWN;
}

int img_check_size(long long w, long long h) {
    if (w <= 0 || h <= 0) return IMG_E_CORRUPT;
    if (w > IMG_MAX_DIM || h > IMG_MAX_DIM || w * h > (long long)IMG_MAX_PIXELS) return IMG_E_TOOBIG;
    return 0;
}

int img_alloc(Image *im, int w, int h) {
    memset(im, 0, sizeof *im);
    int rc = img_check_size(w, h);
    if (rc) return rc;
    im->px = calloc((size_t)w * (size_t)h, 4);
    if (!im->px) return IMG_E_NOMEM;
    im->w = w;
    im->h = h;
    return 0;
}

void img_free(Image *im) {
    if (!im) return;
    free(im->px);
    memset(im, 0, sizeof *im);
}

int img_probe(const void *data, size_t len, ImgInfo *info) {
    memset(info, 0, sizeof *info);
    info->format = img_sniff(data, len);
    info->frames = 1;
    switch (info->format) {
    case IMG_PNG: return png_probe(data, len, info);
    case IMG_BMP: return bmp_probe(data, len, info);
    case IMG_UNKNOWN: return IMG_E_FORMAT;
    default: return IMG_E_UNSUPPORTED;
    }
}

int img_decode(const void *data, size_t len, Image *out) {
    memset(out, 0, sizeof *out);
    int rc;
    switch (img_sniff(data, len)) {
    case IMG_PNG: rc = png_decode(data, len, out); break;
    case IMG_BMP: rc = bmp_decode(data, len, out); break;
    case IMG_UNKNOWN: return IMG_E_FORMAT;
    default: return IMG_E_UNSUPPORTED;
    }
    if (rc) img_free(out);
    return rc;
}

/* Reads a whole file (at most limit bytes). */
static int read_file(const char *path, size_t limit, uint8_t **data, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return IMG_E_IO;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) { close(fd); return IMG_E_IO; }
    if (st.st_size > (long long)limit) { close(fd); return IMG_E_TOOBIG; }
    size_t cap = st.st_size > 0 ? (size_t)st.st_size : 4096, n = 0;
    uint8_t *buf = malloc(cap + 1);
    if (!buf) { close(fd); return IMG_E_NOMEM; }
    for (;;) {
        if (n == cap) {
            if (cap >= limit) { free(buf); close(fd); return IMG_E_TOOBIG; }
            cap *= 2;
            uint8_t *nb = realloc(buf, cap + 1);
            if (!nb) { free(buf); close(fd); return IMG_E_NOMEM; }
            buf = nb;
        }
        ssize_t r = read(fd, buf + n, cap - n);
        if (r < 0) { free(buf); close(fd); return IMG_E_IO; }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(fd);
    *data = buf;
    *len = n;
    return 0;
}

int img_load(const char *path, Image *out) {
    memset(out, 0, sizeof *out);
    uint8_t *data;
    size_t len;
    int rc = read_file(path, IMG_MAX_FILE, &data, &len);
    if (rc) return rc;
    rc = img_decode(data, len, out);
    free(data);
    return rc;
}

int img_probe_file(const char *path, ImgInfo *info) {
    memset(info, 0, sizeof *info);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return IMG_E_IO;
    /* headers are small, but PNG may need to see tRNS and BMP/JPEG markers: read generously */
    size_t cap = 256 * 1024, n = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { close(fd); return IMG_E_NOMEM; }
    while (n < cap) {
        ssize_t r = read(fd, buf + n, cap - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    int rc = img_probe(buf, n, info);
    free(buf);
    return rc;
}

/* ---------------------------------------------------------------- scaling */

static inline uint32_t lerp_px(uint32_t a, uint32_t b, int t) {                       /* t in 0..256 */
    uint32_t ag = a & 0xFF00FF00u, ab = a & 0x00FF00FFu, bg = b & 0xFF00FF00u, bb = b & 0x00FF00FFu;
    uint32_t g = ((ag >> 8) * (uint32_t)(256 - t) + (bg >> 8) * (uint32_t)t) & 0xFF00FF00u;
    uint32_t r = ((ab * (uint32_t)(256 - t) + bb * (uint32_t)t) >> 8) & 0x00FF00FFu;
    return g | r;
}

static int scale_nearest(const Image *s, Image *o) {
    for (int y = 0; y < o->h; y++) {
        int sy = (int)(((2LL * y + 1) * s->h) / (2LL * o->h));
        const uint32_t *row = s->px + (size_t)sy * s->w;
        uint32_t *dst = o->px + (size_t)y * o->w;
        for (int x = 0; x < o->w; x++) dst[x] = row[(int)(((2LL * x + 1) * s->w) / (2LL * o->w))];
    }
    return 0;
}

static int scale_bilinear(const Image *s, Image *o) {
    int *x0 = malloc((size_t)o->w * 3 * sizeof(int));
    if (!x0) return IMG_E_NOMEM;
    int *x1 = x0 + o->w, *xf = x1 + o->w;
    for (int x = 0; x < o->w; x++) {
        long long pos = ((2LL * x + 1) * s->w * 256) / (2LL * o->w) - 128;
        int i = (int)(pos >> 8), f = (int)(pos & 255);
        int a = i < 0 ? 0 : i > s->w - 1 ? s->w - 1 : i, b = i + 1 < 0 ? 0 : i + 1 > s->w - 1 ? s->w - 1 : i + 1;
        x0[x] = a; x1[x] = b; xf[x] = f;
    }
    for (int y = 0; y < o->h; y++) {
        long long pos = ((2LL * y + 1) * s->h * 256) / (2LL * o->h) - 128;
        int i = (int)(pos >> 8), f = (int)(pos & 255);
        int ya = i < 0 ? 0 : i > s->h - 1 ? s->h - 1 : i, yb = i + 1 < 0 ? 0 : i + 1 > s->h - 1 ? s->h - 1 : i + 1;
        const uint32_t *ra = s->px + (size_t)ya * s->w, *rb = s->px + (size_t)yb * s->w;
        uint32_t *dst = o->px + (size_t)y * o->w;
        for (int x = 0; x < o->w; x++) {
            uint32_t top = lerp_px(ra[x0[x]], ra[x1[x]], xf[x]);
            uint32_t bot = lerp_px(rb[x0[x]], rb[x1[x]], xf[x]);
            dst[x] = lerp_px(top, bot, f);
        }
    }
    free(x0);
    return 0;
}

/* Box filter: every destination pixel is the exact average of the source area it covers (fractional edges included).
 * Streaming: one source row at a time, O(destination width) memory. Colours are averaged weighted by alpha. */
static int scale_area(const Image *s, Image *o) {
    int dw = o->w, dh = o->h, sw = s->w, sh = s->h;
    uint64_t *hrow = malloc((size_t)dw * 4 * sizeof(uint64_t));
    uint64_t *acc = calloc((size_t)dw * 4, sizeof(uint64_t));
    uint64_t *nxt = calloc((size_t)dw * 4, sizeof(uint64_t));
    if (!hrow || !acc || !nxt) { free(hrow); free(acc); free(nxt); return IMG_E_NOMEM; }
    uint64_t total = (uint64_t)sw * (uint64_t)sh;
    int j = 0;                                                  /* destination row being accumulated */
    for (int sy = 0; sy < sh; sy++) {
        const uint32_t *row = s->px + (size_t)sy * sw;
        /* horizontal pass: weights are overlaps in a grid of sw*dw cells */
        memset(hrow, 0, (size_t)dw * 4 * sizeof(uint64_t));
        int dx = 0;
        for (int sx = 0; sx < sw; sx++) {
            uint32_t p = row[sx];
            uint64_t a = p >> 24, r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            if (s->has_alpha) { r *= a; g *= a; b *= a; } else a = 255;
            int64_t left = (int64_t)sx * dw, right = left + dw;          /* the source pixel in grid units */
            while (dx < dw) {
                int64_t dl = (int64_t)dx * sw, dr = dl + sw;
                int64_t lo = left > dl ? left : dl, hi = right < dr ? right : dr;
                if (hi > lo) {
                    uint64_t wgt = (uint64_t)(hi - lo);
                    hrow[dx * 4 + 0] += a * wgt; hrow[dx * 4 + 1] += r * wgt; hrow[dx * 4 + 2] += g * wgt; hrow[dx * 4 + 3] += b * wgt;
                }
                if (dr <= right) dx++; else break;                       /* this destination column is done with this source pixel */
                if (dr == right) break;
            }
            if (right >= (int64_t)(dx) * sw + sw && dx < dw && (int64_t)(dx) * sw + sw <= right) { /* defensive: skip finished columns */ }
        }
        /* vertical pass: this source row covers [sy*dh, (sy+1)*dh) in a grid of sh*dh cells */
        int64_t top = (int64_t)sy * dh, bottom = top + dh;
        for (;;) {
            int64_t yl = (int64_t)j * sh, yr = yl + sh;
            int64_t lo = top > yl ? top : yl, hi = bottom < yr ? bottom : yr;
            if (hi > lo) {
                uint64_t wgt = (uint64_t)(hi - lo);
                uint64_t *target = acc;
                for (int i = 0; i < dw * 4; i++) target[i] += hrow[i] * wgt;
            }
            if (yr <= bottom) {                                          /* destination row j is complete */
                uint32_t *dst = o->px + (size_t)j * dw;
                for (int x = 0; x < dw; x++) {
                    uint64_t a = acc[x * 4], r = acc[x * 4 + 1], g = acc[x * 4 + 2], b = acc[x * 4 + 3];
                    uint64_t half = total * 255 / 2;                     /* a, r, g, b are scaled by 255 (alpha) or 1 */
                    if (s->has_alpha) {
                        uint32_t oa = (uint32_t)((a + total / 2) / total);
                        uint32_t orr = 0, og = 0, ob = 0;
                        if (a) { orr = (uint32_t)((r + a / 2) / a); og = (uint32_t)((g + a / 2) / a); ob = (uint32_t)((b + a / 2) / a); }
                        if (orr > 255) orr = 255;
                        if (og > 255) og = 255;
                        if (ob > 255) ob = 255;
                        dst[x] = ARGB(oa, orr, og, ob);
                    } else {
                        (void)half;
                        uint32_t orr = (uint32_t)((r / 255 + total / 2) / total), og = (uint32_t)((g / 255 + total / 2) / total), ob = (uint32_t)((b / 255 + total / 2) / total);
                        dst[x] = ARGB(255, orr > 255 ? 255 : orr, og > 255 ? 255 : og, ob > 255 ? 255 : ob);
                    }
                }
                memset(acc, 0, (size_t)dw * 4 * sizeof(uint64_t));
                uint64_t *t = acc; acc = nxt; nxt = t;
                j++;
                if (j >= dh) break;
                if (yr == bottom) break;
                continue;                                                 /* the rest of this source row belongs to the next destination row */
            }
            break;
        }
        if (j >= dh) break;
    }
    free(hrow);
    free(acc);
    free(nxt);
    return 0;
}

int img_scale(const Image *src, int nw, int nh, ImgScale mode, Image *out) {
    memset(out, 0, sizeof *out);
    if (!src || !src->px || src->w <= 0 || src->h <= 0) return IMG_E_ARG;
    int rc = img_alloc(out, nw, nh);
    if (rc) return rc;
    out->has_alpha = src->has_alpha;
    if (nw == src->w && nh == src->h) { memcpy(out->px, src->px, (size_t)nw * nh * 4); return 0; }
    if (mode == IMG_AREA && (nw > src->w || nh > src->h)) mode = IMG_BILINEAR;             /* a box filter only makes sense for shrinking */
    switch (mode) {
    case IMG_NEAREST: rc = scale_nearest(src, out); break;
    case IMG_BILINEAR: rc = scale_bilinear(src, out); break;
    default: rc = scale_area(src, out); break;
    }
    if (rc) img_free(out);
    return rc;
}

void img_fit(int sw, int sh, int bw, int bh, int upscale, int *ow, int *oh) {
    if (sw <= 0 || sh <= 0 || bw <= 0 || bh <= 0) { *ow = sw > 0 ? sw : 1; *oh = sh > 0 ? sh : 1; return; }
    if (!upscale && sw <= bw && sh <= bh) { *ow = sw; *oh = sh; return; }
    long long w = bw, h = ((long long)sh * bw + sw / 2) / sw;
    if (h > bh) { h = bh; w = ((long long)sw * bh + sh / 2) / sh; }
    *ow = w < 1 ? 1 : (int)w;
    *oh = h < 1 ? 1 : (int)h;
}

/* ---------------------------------------------------------------- rotate, flip, composite */

int img_rotate90(const Image *s, int turns, Image *o) {
    memset(o, 0, sizeof *o);
    turns = ((turns % 4) + 4) % 4;
    int rc = img_alloc(o, turns & 1 ? s->h : s->w, turns & 1 ? s->w : s->h);
    if (rc) return rc;
    o->has_alpha = s->has_alpha;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            uint32_t p = s->px[(size_t)y * s->w + x];
            int nx, ny;
            switch (turns) {
            case 0: nx = x; ny = y; break;
            case 1: nx = s->h - 1 - y; ny = x; break;
            case 2: nx = s->w - 1 - x; ny = s->h - 1 - y; break;
            default: nx = y; ny = s->w - 1 - x; break;
            }
            o->px[(size_t)ny * o->w + nx] = p;
        }
    return 0;
}

int img_flip(const Image *s, int horizontal, Image *o) {
    memset(o, 0, sizeof *o);
    int rc = img_alloc(o, s->w, s->h);
    if (rc) return rc;
    o->has_alpha = s->has_alpha;
    for (int y = 0; y < s->h; y++) {
        const uint32_t *src = s->px + (size_t)y * s->w;
        uint32_t *dst = o->px + (size_t)(horizontal ? y : s->h - 1 - y) * s->w;
        if (horizontal) for (int x = 0; x < s->w; x++) dst[x] = src[s->w - 1 - x];
        else memcpy(dst, src, (size_t)s->w * 4);
    }
    return 0;
}

void img_composite_over(Image *im, uint32_t bg) {
    if (!im || !im->px) return;
    uint32_t br = (bg >> 16) & 255, bgc = (bg >> 8) & 255, bb = bg & 255;
    size_t n = (size_t)im->w * (size_t)im->h;
    if (im->has_alpha) {
        for (size_t i = 0; i < n; i++) {
            uint32_t p = im->px[i], a = p >> 24;
            if (a == 255) continue;
            uint32_t r = (((p >> 16) & 255) * a + br * (255 - a) + 127) / 255;
            uint32_t g = (((p >> 8) & 255) * a + bgc * (255 - a) + 127) / 255;
            uint32_t b = ((p & 255) * a + bb * (255 - a) + 127) / 255;
            im->px[i] = ARGB(255, r, g, b);
        }
    }
    im->has_alpha = 0;
}
