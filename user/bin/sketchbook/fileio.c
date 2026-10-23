/* Picture files: 24-bit BMP and binary PPM (P6) writers, and readers that accept the usual variants.
 *
 * Reading BMP: BITMAPCOREHEADER and the 40/52/56/108/124 byte info headers; 1, 4, 8 bit palettes; 16, 24 and 32 bit;
 * BI_RGB and BI_BITFIELDS; bottom-up and top-down. RLE and JPEG/PNG-in-BMP are refused with a clear message.
 * Reading PPM: P6 with comments in the header, maxval up to 65535.
 * Everything is checked: sizes, truncated data, allocation failures. Errors come back as text, never as a crash. */
#include "sketchbook.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static char g_err[200];

static const char *fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static const char *fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    return g_err;
}

int file_format_for(const char *path) {
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash)) return FMT_UNKNOWN;
    if (!strcasecmp(dot, ".bmp") || !strcasecmp(dot, ".dib")) return FMT_BMP;
    if (!strcasecmp(dot, ".ppm") || !strcasecmp(dot, ".pnm")) return FMT_PPM;
    return FMT_UNKNOWN;
}

/* ---------------------------------------------------------------- buffered reader and writer */

typedef struct { int fd; uint8_t buf[32768]; size_t pos, len; bool eof; } Rd;

static bool rd_fill(Rd *r) {
    if (r->eof) return false;
    ssize_t n;
    do n = read(r->fd, r->buf, sizeof r->buf); while (n < 0 && errno == EINTR);
    if (n <= 0) { r->eof = true; r->pos = r->len = 0; return false; }
    r->pos = 0;
    r->len = (size_t)n;
    return true;
}

static bool rd_bytes(Rd *r, void *dst, size_t n) {
    uint8_t *d = dst;
    while (n) {
        if (r->pos >= r->len && !rd_fill(r)) return false;
        size_t k = r->len - r->pos;
        if (k > n) k = n;
        memcpy(d, r->buf + r->pos, k);
        r->pos += k;
        d += k;
        n -= k;
    }
    return true;
}

static bool rd_skip(Rd *r, size_t n) {
    while (n) {
        if (r->pos >= r->len && !rd_fill(r)) return false;
        size_t k = r->len - r->pos;
        if (k > n) k = n;
        r->pos += k;
        n -= k;
    }
    return true;
}

static int rd_byte(Rd *r) {
    if (r->pos >= r->len && !rd_fill(r)) return -1;
    return r->buf[r->pos++];
}

typedef struct { int fd; uint8_t buf[32768]; size_t len; bool err; } Wr;

static void wr_flush(Wr *w) {
    size_t off = 0;
    while (off < w->len && !w->err) {
        ssize_t n = write(w->fd, w->buf + off, w->len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { w->err = true; break; }
        off += (size_t)n;
    }
    w->len = 0;
}

static void wr_bytes(Wr *w, const void *src, size_t n) {
    const uint8_t *s = src;
    while (n && !w->err) {
        size_t room = sizeof w->buf - w->len;
        size_t k = n < room ? n : room;
        memcpy(w->buf + w->len, s, k);
        w->len += k;
        s += k;
        n -= k;
        if (w->len == sizeof w->buf) wr_flush(w);
    }
}

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

/* ---------------------------------------------------------------- BMP */

static int mask_shift(uint32_t m) { int s = 0; while (m && !(m & 1)) { m >>= 1; s++; } return s; }
static int mask_bits(uint32_t m) { int b = 0; m >>= mask_shift(m); while (m & 1) { b++; m >>= 1; } return b; }
static uint32_t scale_channel(uint32_t v, int bits) {
    if (bits <= 0) return 0;
    if (bits >= 8) return v >> (bits - 8);
    return v * 255 / ((1u << bits) - 1);
}

static const char *bmp_read(Rd *r, uint32_t **out_px, int *out_w, int *out_h) {
    uint8_t fh[14];
    if (!rd_bytes(r, fh, 14) || fh[0] != 'B' || fh[1] != 'M') return fail("this is not a BMP picture");
    uint32_t data_off = le32(fh + 10);
    uint8_t sz4[4];
    if (!rd_bytes(r, sz4, 4)) return fail("the BMP header is truncated");
    uint32_t dib = le32(sz4);
    if (dib != 12 && (dib < 40 || dib > 124)) return fail("unsupported BMP header size (%u)", (unsigned)dib);
    uint8_t h[124];
    if (!rd_bytes(r, h + 4, dib - 4)) return fail("the BMP header is truncated");
    int32_t w, hh;
    int bpp, compression = 0;
    uint32_t ncolors = 0;
    size_t used = 14 + dib;
    if (dib == 12) {
        w = (int32_t)le16(h + 4);
        hh = (int32_t)le16(h + 6);
        bpp = (int)le16(h + 10);
    } else {
        w = (int32_t)le32(h + 4);
        hh = (int32_t)le32(h + 8);
        bpp = (int)le16(h + 14);
        compression = (int)le32(h + 16);
        ncolors = le32(h + 32);
    }
    bool top_down = hh < 0;
    if (top_down) hh = -hh;
    if (w < 1 || hh < 1) return fail("the BMP picture has no pixels");
    if (w > SK_MAX_DIM || hh > SK_MAX_DIM) return fail("the picture is too large (%d x %d, the limit is %d x %d)", (int)w, (int)hh, SK_MAX_DIM, SK_MAX_DIM);
    if (compression == 1 || compression == 2) return fail("compressed (RLE) BMP files are not supported");
    if (compression != 0 && compression != 3) return fail("unsupported BMP compression (%d)", compression);
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return fail("unsupported BMP depth (%d bits per pixel)", bpp);

    uint32_t masks[3] = { 0, 0, 0 };
    if (bpp == 16 || bpp == 32) {
        if (compression == 3) {
            uint8_t mb[12];
            if (dib >= 52) memcpy(mb, h + 40, 12);
            else { if (!rd_bytes(r, mb, 12)) return fail("the BMP header is truncated"); used += 12; }
            for (int i = 0; i < 3; i++) masks[i] = le32(mb + i * 4);
        } else if (bpp == 16) { masks[0] = 0x7C00; masks[1] = 0x03E0; masks[2] = 0x001F; }
        else { masks[0] = 0x00FF0000; masks[1] = 0x0000FF00; masks[2] = 0x000000FF; }
    }
    uint32_t pal[256];
    if (bpp <= 8) {
        uint32_t n = ncolors ? ncolors : 1u << bpp;
        if (n > 256) n = 256;
        size_t esz = dib == 12 ? 3 : 4;
        uint8_t e[4];
        for (uint32_t i = 0; i < n; i++) {
            if (!rd_bytes(r, e, esz)) return fail("the BMP palette is truncated");
            pal[i] = (uint32_t)e[2] << 16 | (uint32_t)e[1] << 8 | e[0];
        }
        for (uint32_t i = n; i < 256; i++) pal[i] = 0;
        used += n * esz;
    }
    if (data_off < used) return fail("the BMP header is inconsistent (picture data starts inside the header)");
    if (!rd_skip(r, data_off - used)) return fail("the BMP file is truncated");

    size_t stride = ((size_t)w * (size_t)bpp + 31) / 32 * 4;
    uint32_t *px = malloc((size_t)w * (size_t)hh * 4);
    uint8_t *row = malloc(stride);
    if (!px || !row) { free(px); free(row); return fail("not enough memory for a %d x %d picture", (int)w, (int)hh); }
    int rs[3] = { mask_shift(masks[0]), mask_shift(masks[1]), mask_shift(masks[2]) };
    int rb[3] = { mask_bits(masks[0]), mask_bits(masks[1]), mask_bits(masks[2]) };
    for (int y = 0; y < hh; y++) {
        if (!rd_bytes(r, row, stride)) { free(px); free(row); return fail("the BMP file is truncated (picture data ends early)"); }
        uint32_t *dst = px + (size_t)(top_down ? y : hh - 1 - y) * w;
        switch (bpp) {
        case 24:
            for (int x = 0; x < w; x++) dst[x] = (uint32_t)row[x * 3 + 2] << 16 | (uint32_t)row[x * 3 + 1] << 8 | row[x * 3];
            break;
        case 32:
            for (int x = 0; x < w; x++) {
                uint32_t v = le32(row + x * 4);
                dst[x] = scale_channel((v & masks[0]) >> rs[0], rb[0]) << 16 | scale_channel((v & masks[1]) >> rs[1], rb[1]) << 8 |
                         scale_channel((v & masks[2]) >> rs[2], rb[2]);
            }
            break;
        case 16:
            for (int x = 0; x < w; x++) {
                uint32_t v = le16(row + x * 2);
                dst[x] = scale_channel((v & masks[0]) >> rs[0], rb[0]) << 16 | scale_channel((v & masks[1]) >> rs[1], rb[1]) << 8 |
                         scale_channel((v & masks[2]) >> rs[2], rb[2]);
            }
            break;
        case 8: for (int x = 0; x < w; x++) dst[x] = pal[row[x]]; break;
        case 4: for (int x = 0; x < w; x++) dst[x] = pal[(row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15]; break;
        default: for (int x = 0; x < w; x++) dst[x] = pal[(row[x >> 3] >> (7 - (x & 7))) & 1]; break;
        }
    }
    free(row);
    *out_px = px;
    *out_w = (int)w;
    *out_h = (int)hh;
    return NULL;
}

static bool bmp_write(Wr *wr, const Doc *d) {
    size_t stride = ((size_t)d->w * 3 + 3) & ~(size_t)3;
    uint32_t image = (uint32_t)(stride * (size_t)d->h);
    uint8_t hdr[54];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B';
    hdr[1] = 'M';
    put32(hdr + 2, 54 + image);
    put32(hdr + 10, 54);
    put32(hdr + 14, 40);
    put32(hdr + 18, (uint32_t)d->w);
    put32(hdr + 22, (uint32_t)d->h);
    put16(hdr + 26, 1);
    put16(hdr + 28, 24);
    put32(hdr + 34, image);
    put32(hdr + 38, 2835);
    put32(hdr + 42, 2835);
    wr_bytes(wr, hdr, sizeof hdr);
    uint8_t *row = calloc(1, stride);
    if (!row) return false;
    for (int y = d->h - 1; y >= 0; y--) {
        const uint32_t *src = d->px + (size_t)y * d->w;
        for (int x = 0; x < d->w; x++) {
            row[x * 3] = (uint8_t)src[x];
            row[x * 3 + 1] = (uint8_t)(src[x] >> 8);
            row[x * 3 + 2] = (uint8_t)(src[x] >> 16);
        }
        wr_bytes(wr, row, stride);
    }
    free(row);
    return true;
}

/* ---------------------------------------------------------------- PPM */

static int ppm_token(Rd *r, bool *eof_hit) {                 /* next unsigned integer, skipping whitespace and # comments; -1 on error */
    int c;
    for (;;) {
        c = rd_byte(r);
        if (c < 0) { *eof_hit = true; return -1; }
        if (c == '#') { while ((c = rd_byte(r)) >= 0 && c != '\n') { } if (c < 0) { *eof_hit = true; return -1; } continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        break;
    }
    if (c < '0' || c > '9') return -1;
    long v = 0;
    while (c >= '0' && c <= '9') {
        v = v * 10 + (c - '0');
        if (v > 1000000) return -1;
        c = rd_byte(r);
        if (c < 0) { *eof_hit = true; return (int)v; }
    }
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return -1;
    return (int)v;                                            /* the single whitespace after maxval is consumed here */
}

static const char *ppm_read(Rd *r, uint32_t **out_px, int *out_w, int *out_h) {
    uint8_t magic[2];
    if (!rd_bytes(r, magic, 2) || magic[0] != 'P') return fail("this is not a PPM picture");
    if (magic[1] != '6') return fail("only binary PPM pictures (P6) can be opened, this one is P%c", magic[1] >= '1' && magic[1] <= '9' ? magic[1] : '?');
    bool eof = false;
    int w = ppm_token(r, &eof), h = ppm_token(r, &eof), maxv = ppm_token(r, &eof);
    if (w < 0 || h < 0 || maxv < 0) return fail(eof ? "the PPM header is truncated" : "the PPM header is damaged");
    if (w < 1 || h < 1) return fail("the PPM picture has no pixels");
    if (w > SK_MAX_DIM || h > SK_MAX_DIM) return fail("the picture is too large (%d x %d, the limit is %d x %d)", w, h, SK_MAX_DIM, SK_MAX_DIM);
    if (maxv < 1 || maxv > 65535) return fail("the PPM maximum colour value (%d) is not valid", maxv);
    int bytes = maxv > 255 ? 2 : 1;
    size_t rowlen = (size_t)w * 3 * (size_t)bytes;
    uint32_t *px = malloc((size_t)w * (size_t)h * 4);
    uint8_t *row = malloc(rowlen);
    if (!px || !row) { free(px); free(row); return fail("not enough memory for a %d x %d picture", w, h); }
    for (int y = 0; y < h; y++) {
        if (!rd_bytes(r, row, rowlen)) { free(px); free(row); return fail("the PPM file is truncated (picture data ends early)"); }
        uint32_t *dst = px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            uint32_t c[3];
            for (int k = 0; k < 3; k++) {
                uint32_t v = bytes == 1 ? row[(size_t)x * 3 + k] : (uint32_t)row[((size_t)x * 3 + k) * 2] << 8 | row[((size_t)x * 3 + k) * 2 + 1];
                if (v > (uint32_t)maxv) v = (uint32_t)maxv;
                c[k] = maxv == 255 ? v : (v * 255 + (uint32_t)maxv / 2) / (uint32_t)maxv;
            }
            dst[x] = c[0] << 16 | c[1] << 8 | c[2];
        }
    }
    free(row);
    *out_px = px;
    *out_w = w;
    *out_h = h;
    return NULL;
}

static bool ppm_write(Wr *wr, const Doc *d) {
    char head[48];
    int n = snprintf(head, sizeof head, "P6\n%d %d\n255\n", d->w, d->h);
    wr_bytes(wr, head, (size_t)n);
    uint8_t *row = malloc((size_t)d->w * 3);
    if (!row) return false;
    for (int y = 0; y < d->h; y++) {
        const uint32_t *src = d->px + (size_t)y * d->w;
        for (int x = 0; x < d->w; x++) {
            row[x * 3] = (uint8_t)(src[x] >> 16);
            row[x * 3 + 1] = (uint8_t)(src[x] >> 8);
            row[x * 3 + 2] = (uint8_t)src[x];
        }
        wr_bytes(wr, row, (size_t)d->w * 3);
    }
    free(row);
    return true;
}

/* ---------------------------------------------------------------- entry points */

const char *file_load(const char *path, uint32_t **px, int *w, int *h) {
    *px = NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return fail("cannot open the file: %s", strerror(errno));
    Rd *r = calloc(1, sizeof *r);
    if (!r) { close(fd); return fail("not enough memory"); }
    r->fd = fd;
    uint8_t first[2] = { 0, 0 };
    ssize_t n = read(fd, first, 2);
    if (n < 2) { close(fd); free(r); return fail(n < 0 ? "cannot read the file: %s" : "the file is empty or too short to be a picture", strerror(errno)); }
    lseek(fd, 0, SEEK_SET);
    const char *err;
    if (first[0] == 'B' && first[1] == 'M') err = bmp_read(r, px, w, h);
    else if (first[0] == 'P') err = ppm_read(r, px, w, h);
    else err = fail("this file is not a BMP or PPM picture");
    close(fd);
    free(r);
    return err;
}

const char *file_save(const char *path, const Doc *d) {
    int fmt = file_format_for(path);
    if (fmt == FMT_UNKNOWN) return fail("Sketchbook saves .bmp and .ppm pictures; please give the file one of those names");
    if (!d->px || d->w < 1 || d->h < 1) return fail("there is nothing to save");
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.sketch~", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fail("cannot create the file: %s", strerror(errno));
    Wr *wr = calloc(1, sizeof *wr);
    if (!wr) { close(fd); unlink(tmp); return fail("not enough memory"); }
    wr->fd = fd;
    bool ok = fmt == FMT_BMP ? bmp_write(wr, d) : ppm_write(wr, d);
    wr_flush(wr);
    int saved_errno = errno;
    bool werr = wr->err;
    free(wr);
    if (close(fd) < 0) werr = true;
    if (!ok || werr) {
        unlink(tmp);
        return fail("cannot write the file: %s", ok ? strerror(saved_errno ? saved_errno : EIO) : "not enough memory");
    }
    if (rename(tmp, path) < 0) {
        if (errno == EEXIST || errno == EPERM || errno == EACCES || errno == ENOTEMPTY) {      /* some filesystems will not replace */
            unlink(path);
            if (rename(tmp, path) == 0) return NULL;
        }
        int e = errno;
        unlink(tmp);
        return fail("cannot replace the file: %s", strerror(e));
    }
    return NULL;
}
