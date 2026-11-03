/* Encoders: PNG (libz deflate, adaptive row filters, grayscale / RGB / RGBA chosen from the pixels), BMP and PPM. */
#include "imgint.h"
#include <z.h>
#include <stdio.h>
#include <unistd.h>

static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put_le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static int put_chunk(ImgSink sink, void *user, const char *type, const void *data, size_t len) {
    uint8_t hdr[8], crc[4];
    put_be32(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    uint32_t c = z_crc32(0, hdr + 4, 4);
    if (len) c = z_crc32(c, data, len);
    put_be32(crc, c);
    int rc = sink(user, hdr, 8);
    if (!rc && len) rc = sink(user, data, len);
    if (!rc) rc = sink(user, crc, 4);
    return rc;
}

int img_write_png(const Image *im, int level, ImgSink sink, void *user) {
    if (!im || !im->px || im->w <= 0 || im->h <= 0) return IMG_E_ARG;
    int w = im->w, h = im->h;
    size_t npix = (size_t)w * (size_t)h;
    int opaque = 1, gray = 1;
    for (size_t i = 0; i < npix && (opaque || gray); i++) {
        uint32_t p = im->px[i];
        if ((p >> 24) != 255) opaque = 0;
        if (((p >> 16) & 255) != ((p >> 8) & 255) || ((p >> 8) & 255) != (p & 255)) gray = 0;
    }
    int ctype, bpp;
    if (opaque) { ctype = gray ? 0 : 2; bpp = gray ? 1 : 3; }
    else { ctype = gray ? 4 : 6; bpp = gray ? 2 : 4; }
    size_t rowbytes = (size_t)w * (size_t)bpp;

    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    int rc = sink(user, sig, 8);
    if (rc) return rc;
    uint8_t ihdr[13];
    put_be32(ihdr, (uint32_t)w);
    put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = (uint8_t)ctype; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    rc = put_chunk(sink, user, "IHDR", ihdr, 13);
    if (rc) return rc;

    uint8_t *raw = malloc(rowbytes), *prev = calloc(rowbytes, 1), *cand = malloc(5 * (rowbytes + 1)), *obuf = malloc(32768);
    ZStream zs;
    memset(&zs, 0, sizeof zs);
    if (!raw || !prev || !cand || !obuf || z_deflate_init(&zs, level < 0 ? 6 : level, Z_ZLIB)) {
        free(raw); free(prev); free(cand); free(obuf);
        return IMG_E_NOMEM;
    }
    for (int y = 0; y < h && !rc; y++) {
        const uint32_t *src = im->px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            uint32_t p = src[x];
            uint8_t r = (uint8_t)(p >> 16), g = (uint8_t)(p >> 8), b = (uint8_t)p, a = (uint8_t)(p >> 24);
            switch (ctype) {
            case 0: raw[x] = r; break;
            case 4: raw[2 * x] = r; raw[2 * x + 1] = a; break;
            case 2: raw[3 * x] = r; raw[3 * x + 1] = g; raw[3 * x + 2] = b; break;
            default: raw[4 * x] = r; raw[4 * x + 1] = g; raw[4 * x + 2] = b; raw[4 * x + 3] = a; break;
            }
        }
        /* filter candidates: 0 None, 1 Sub, 2 Up, 3 Average, 4 Paeth; pick the one with the smallest sum of absolute values */
        int lo = level <= 1 ? 1 : 0, hi = level <= 1 ? 1 : 4;
        int best = lo;
        unsigned long best_sum = ~0UL;
        for (int f = lo; f <= hi; f++) {
            uint8_t *o = cand + (size_t)f * (rowbytes + 1);
            o[0] = (uint8_t)f;
            unsigned long sum = 0;
            for (size_t i = 0; i < rowbytes; i++) {
                int a = i >= (size_t)bpp ? raw[i - bpp] : 0, b = prev[i], c = i >= (size_t)bpp ? prev[i - bpp] : 0, v;
                switch (f) {
                case 0: v = raw[i]; break;
                case 1: v = raw[i] - a; break;
                case 2: v = raw[i] - b; break;
                case 3: v = raw[i] - ((a + b) >> 1); break;
                default: {
                    int pa = abs(b - c), pb = abs(a - c), pc = abs(a + b - 2 * c);
                    v = raw[i] - ((pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c));
                    break;
                }
                }
                o[1 + i] = (uint8_t)v;
                sum += (unsigned long)((int8_t)(uint8_t)v < 0 ? -(int8_t)(uint8_t)v : (int8_t)(uint8_t)v);
            }
            if (sum < best_sum) { best_sum = sum; best = f; }
        }
        zs.next_in = cand + (size_t)best * (rowbytes + 1);
        zs.avail_in = rowbytes + 1;
        while (zs.avail_in && !rc) {
            zs.next_out = obuf;
            zs.avail_out = 32768;
            int zr = z_deflate(&zs, Z_NO_FLUSH);
            if (zr < 0) { rc = IMG_E_NOMEM; break; }
            if (zs.avail_out != 32768) rc = put_chunk(sink, user, "IDAT", obuf, 32768 - zs.avail_out);
        }
        memcpy(prev, raw, rowbytes);
    }
    while (!rc) {
        zs.next_in = NULL;
        zs.avail_in = 0;
        zs.next_out = obuf;
        zs.avail_out = 32768;
        int zr = z_deflate(&zs, Z_FINISH);
        if (zr < 0) { rc = IMG_E_NOMEM; break; }
        if (zs.avail_out != 32768) rc = put_chunk(sink, user, "IDAT", obuf, 32768 - zs.avail_out);
        if (zr == Z_STREAM_END) break;
    }
    z_deflate_end(&zs);
    free(raw); free(prev); free(cand); free(obuf);
    if (!rc) rc = put_chunk(sink, user, "IEND", NULL, 0);
    return rc;
}

int img_write_bmp(const Image *im, ImgSink sink, void *user) {
    if (!im || !im->px || im->w <= 0 || im->h <= 0) return IMG_E_ARG;
    int alpha = 0;
    if (im->has_alpha) for (size_t i = 0; i < (size_t)im->w * im->h; i++) if ((im->px[i] >> 24) != 255) { alpha = 1; break; }
    int bpp = alpha ? 32 : 24;
    size_t stride = (((size_t)im->w * (size_t)bpp + 31) / 32) * 4;
    uint32_t hdr_size = alpha ? 108 : 40;
    uint32_t off = 14 + hdr_size, size = off + (uint32_t)(stride * (size_t)im->h);
    uint8_t hdr[14 + 108];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    put_le32(hdr + 2, size);
    put_le32(hdr + 10, off);
    put_le32(hdr + 14, hdr_size);
    put_le32(hdr + 18, (uint32_t)im->w);
    put_le32(hdr + 22, (uint32_t)im->h);
    put_le16(hdr + 26, 1);
    put_le16(hdr + 28, (uint32_t)bpp);
    put_le32(hdr + 30, alpha ? 3 : 0);                             /* BI_BITFIELDS for the BGRA layout */
    put_le32(hdr + 34, (uint32_t)(stride * (size_t)im->h));
    put_le32(hdr + 38, 2835); put_le32(hdr + 42, 2835);
    if (alpha) {
        put_le32(hdr + 54, 0x00FF0000u); put_le32(hdr + 58, 0x0000FF00u); put_le32(hdr + 62, 0x000000FFu); put_le32(hdr + 66, 0xFF000000u);
        memcpy(hdr + 70, "sRGB", 4);
    }
    int rc = sink(user, hdr, off);
    uint8_t *row = calloc(stride, 1);
    if (!row) return IMG_E_NOMEM;
    for (int y = im->h - 1; y >= 0 && !rc; y--) {
        const uint32_t *src = im->px + (size_t)y * im->w;
        for (int x = 0; x < im->w; x++) {
            uint32_t p = src[x];
            if (alpha) { row[4 * x] = (uint8_t)p; row[4 * x + 1] = (uint8_t)(p >> 8); row[4 * x + 2] = (uint8_t)(p >> 16); row[4 * x + 3] = (uint8_t)(p >> 24); }
            else { row[3 * x] = (uint8_t)p; row[3 * x + 1] = (uint8_t)(p >> 8); row[3 * x + 2] = (uint8_t)(p >> 16); }
        }
        rc = sink(user, row, stride);
    }
    free(row);
    return rc;
}

int img_write_ppm(const Image *im, uint32_t bg, ImgSink sink, void *user) {
    if (!im || !im->px || im->w <= 0 || im->h <= 0) return IMG_E_ARG;
    char hdr[64];
    int hl = snprintf(hdr, sizeof hdr, "P6\n%d %d\n255\n", im->w, im->h);
    int rc = sink(user, hdr, (size_t)hl);
    uint8_t *row = malloc((size_t)im->w * 3);
    if (!row) return IMG_E_NOMEM;
    uint32_t br = (bg >> 16) & 255, bgc = (bg >> 8) & 255, bb = bg & 255;
    for (int y = 0; y < im->h && !rc; y++) {
        const uint32_t *src = im->px + (size_t)y * im->w;
        for (int x = 0; x < im->w; x++) {
            uint32_t p = src[x], a = p >> 24, r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            if (a != 255) { r = (r * a + br * (255 - a) + 127) / 255; g = (g * a + bgc * (255 - a) + 127) / 255; b = (b * a + bb * (255 - a) + 127) / 255; }
            row[3 * x] = (uint8_t)r; row[3 * x + 1] = (uint8_t)g; row[3 * x + 2] = (uint8_t)b;
        }
        rc = sink(user, row, (size_t)im->w * 3);
    }
    free(row);
    return rc;
}

/* ---------------------------------------------------------------- files */

static int file_sink(void *user, const void *data, size_t n) {
    return fwrite(data, 1, n, (FILE *)user) == n ? 0 : IMG_E_IO;
}

/* Writes through "path.part" and renames, so a full disk or a crash never leaves half a picture under the real name. */
static int save_file(const char *path, int (*writer)(FILE *, const Image *, void *), const Image *im, void *arg) {
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return IMG_E_IO;
    int rc = writer(f, im, arg);
    if (fclose(f) != 0 && !rc) rc = IMG_E_IO;
    if (rc) { unlink(tmp); return rc; }
    if (rename(tmp, path) < 0) { unlink(tmp); return IMG_E_IO; }
    return 0;
}

static int w_png(FILE *f, const Image *im, void *a) { return img_write_png(im, *(int *)a, file_sink, f); }
static int w_bmp(FILE *f, const Image *im, void *a) { (void)a; return img_write_bmp(im, file_sink, f); }
static int w_ppm(FILE *f, const Image *im, void *a) { return img_write_ppm(im, *(uint32_t *)a, file_sink, f); }

int img_save_png(const char *path, const Image *im, int level) { return save_file(path, w_png, im, &level); }
int img_save_bmp(const char *path, const Image *im) { return save_file(path, w_bmp, im, NULL); }
int img_save_ppm(const char *path, const Image *im, uint32_t bg) { return save_file(path, w_ppm, im, &bg); }
