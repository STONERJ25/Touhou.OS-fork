/*
 * PNG decoder: every colour type and bit depth, Adam7 interlacing, tRNS (palette alpha and colour keys), chunk CRCs.
 *
 * The compressed data is inflated straight from the IDAT chunks into one row buffer at a time (libz keeps the history),
 * so memory is the output image plus a few rows. 16-bit samples are reduced to their high byte.
 */
#include "imgint.h"
#include <z.h>

typedef struct {
    int w, h, depth, ctype, interlace;
    int bits_pp;                         /* bits per pixel in the raw data */
    size_t bpp;                          /* bytes per pixel for the filters (at least 1) */
    uint32_t pal[256];
    int npal;
    uint8_t trns[256];
    int ntrns;
    int has_key;
    uint16_t key[3];
    int has_alpha;
} Png;

static int png_header(const uint8_t *d, size_t n, Png *p, size_t *pos) {
    memset(p, 0, sizeof *p);
    if (n < 8 + 12 + 13) return IMG_E_TRUNC;
    if (memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return IMG_E_FORMAT;
    if (rd_be32(d + 8) != 13 || memcmp(d + 12, "IHDR", 4)) return IMG_E_CORRUPT;
    const uint8_t *h = d + 16;
    uint32_t w = rd_be32(h), ht = rd_be32(h + 4);
    p->depth = h[8];
    p->ctype = h[9];
    p->interlace = h[12];
    if (h[10] != 0 || h[11] != 0 || p->interlace > 1) return IMG_E_UNSUPPORTED;
    int ok = 0;
    switch (p->ctype) {
    case 0: ok = p->depth == 1 || p->depth == 2 || p->depth == 4 || p->depth == 8 || p->depth == 16; break;
    case 2: case 4: case 6: ok = p->depth == 8 || p->depth == 16; break;
    case 3: ok = p->depth == 1 || p->depth == 2 || p->depth == 4 || p->depth == 8; break;
    }
    if (!ok) return IMG_E_CORRUPT;
    int rc = img_check_size(w, ht);
    if (rc) return rc;
    p->w = (int)w;
    p->h = (int)ht;
    int channels = p->ctype == 0 || p->ctype == 3 ? 1 : p->ctype == 4 ? 2 : p->ctype == 2 ? 3 : 4;
    p->bits_pp = channels * p->depth;
    p->bpp = (size_t)(p->bits_pp + 7) / 8;
    p->has_alpha = p->ctype == 4 || p->ctype == 6;
    *pos = 8 + 12 + 13;
    return 0;
}

/* Reads the chunk at *pos: returns 1 with its type/data/length, 0 at the end of the data, <0 on damage. */
static int next_chunk(const uint8_t *d, size_t n, size_t *pos, const uint8_t **type, const uint8_t **data, uint32_t *len) {
    if (*pos + 12 > n) return IMG_E_TRUNC;
    uint32_t l = rd_be32(d + *pos);
    if (l > 0x7FFFFFFFu || (size_t)l > n - *pos - 12) return IMG_E_TRUNC;
    *type = d + *pos + 4;
    *data = d + *pos + 8;
    *len = l;
    if (!(d[*pos + 4] & 0x20)) {                                     /* critical chunk: the CRC must match */
        if (z_crc32(0, d + *pos + 4, (size_t)l + 4) != rd_be32(d + *pos + 8 + l)) return IMG_E_CORRUPT;
    }
    *pos += 12 + (size_t)l;
    return 1;
}

static void ancillary(Png *p, const uint8_t *type, const uint8_t *data, uint32_t len) {
    if (!memcmp(type, "PLTE", 4)) {
        if (len % 3 == 0 && len <= 768 && p->ctype == 3) {
            p->npal = (int)(len / 3);
            for (int i = 0; i < p->npal; i++) p->pal[i] = ARGB(255, data[i * 3], data[i * 3 + 1], data[i * 3 + 2]);
        }
    } else if (!memcmp(type, "tRNS", 4)) {
        if (p->ctype == 3) {
            p->ntrns = len > 256 ? 256 : (int)len;
            memcpy(p->trns, data, (size_t)p->ntrns);
            p->has_alpha = 1;
        } else if (p->ctype == 0 && len >= 2) {
            p->key[0] = (uint16_t)rd_be16(data);
            p->has_key = 1;
            p->has_alpha = 1;
        } else if (p->ctype == 2 && len >= 6) {
            for (int i = 0; i < 3; i++) p->key[i] = (uint16_t)rd_be16(data + 2 * i);
            p->has_key = 1;
            p->has_alpha = 1;
        }
    }
}

int png_probe(const uint8_t *d, size_t n, ImgInfo *info) {
    Png p;
    size_t pos;
    int rc = png_header(d, n, &p, &pos);
    if (rc) return rc;
    for (;;) {                                                       /* tRNS comes before the first IDAT */
        const uint8_t *type, *data;
        uint32_t len;
        int r = next_chunk(d, n, &pos, &type, &data, &len);
        if (r <= 0) break;
        if (!memcmp(type, "IDAT", 4) || !memcmp(type, "IEND", 4)) break;
        ancillary(&p, type, data, len);
    }
    info->format = IMG_PNG;
    info->width = p.w;
    info->height = p.h;
    info->has_alpha = p.has_alpha;
    info->frames = 1;
    return 0;
}

static void unfilter(int type, uint8_t *cur, const uint8_t *prev, size_t n, size_t bpp) {
    switch (type) {
    case 1: for (size_t i = bpp; i < n; i++) cur[i] = (uint8_t)(cur[i] + cur[i - bpp]); break;
    case 2: for (size_t i = 0; i < n; i++) cur[i] = (uint8_t)(cur[i] + prev[i]); break;
    case 3:
        for (size_t i = 0; i < n; i++) {
            int left = i >= bpp ? cur[i - bpp] : 0;
            cur[i] = (uint8_t)(cur[i] + ((left + prev[i]) >> 1));
        }
        break;
    case 4:
        for (size_t i = 0; i < n; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
            int pa = abs(b - c), pb = abs(a - c), pc = abs(a + b - 2 * c);
            int pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
            cur[i] = (uint8_t)(cur[i] + pred);
        }
        break;
    default: break;
    }
}

static void convert_row(const Png *p, const uint8_t *row, int npix, uint32_t *dst, int step) {
    int depth = p->depth;
    switch (p->ctype) {
    case 0: {
        if (depth == 16) {
            for (int i = 0; i < npix; i++) {
                unsigned v = rd_be16(row + 2 * i), g = v >> 8;
                dst[i * step] = ARGB(p->has_key && v == p->key[0] ? 0 : 255, g, g, g);
            }
        } else if (depth == 8) {
            for (int i = 0; i < npix; i++) {
                unsigned g = row[i];
                dst[i * step] = ARGB(p->has_key && g == p->key[0] ? 0 : 255, g, g, g);
            }
        } else {
            unsigned mask = (1u << depth) - 1;
            for (int i = 0; i < npix; i++) {
                int bit = i * depth;
                unsigned v = (row[bit >> 3] >> (8 - depth - (bit & 7))) & mask, g = v * 255 / mask;
                dst[i * step] = ARGB(p->has_key && v == p->key[0] ? 0 : 255, g, g, g);
            }
        }
        break;
    }
    case 2:
        for (int i = 0; i < npix; i++) {
            unsigned r, g, b;
            int transparent = 0;
            if (depth == 8) {
                r = row[3 * i]; g = row[3 * i + 1]; b = row[3 * i + 2];
                transparent = p->has_key && r == p->key[0] && g == p->key[1] && b == p->key[2];
            } else {
                unsigned r16 = rd_be16(row + 6 * i), g16 = rd_be16(row + 6 * i + 2), b16 = rd_be16(row + 6 * i + 4);
                r = r16 >> 8; g = g16 >> 8; b = b16 >> 8;
                transparent = p->has_key && r16 == p->key[0] && g16 == p->key[1] && b16 == p->key[2];
            }
            dst[i * step] = ARGB(transparent ? 0 : 255, r, g, b);
        }
        break;
    case 3: {
        unsigned mask = (1u << depth) - 1;
        for (int i = 0; i < npix; i++) {
            int bit = i * depth;
            unsigned idx = depth == 8 ? row[i] : (unsigned)((row[bit >> 3] >> (8 - depth - (bit & 7))) & mask);
            uint32_t c = (int)idx < p->npal ? p->pal[idx] : ARGB(255, 0, 0, 0);
            if ((int)idx < p->ntrns) c = (c & 0x00FFFFFFu) | (uint32_t)p->trns[idx] << 24;
            dst[i * step] = c;
        }
        break;
    }
    case 4:
        for (int i = 0; i < npix; i++) {
            unsigned g = depth == 8 ? row[2 * i] : row[4 * i], a = depth == 8 ? row[2 * i + 1] : row[4 * i + 2];
            dst[i * step] = ARGB(a, g, g, g);
        }
        break;
    default:
        for (int i = 0; i < npix; i++) {
            if (depth == 8) dst[i * step] = ARGB(row[4 * i + 3], row[4 * i], row[4 * i + 1], row[4 * i + 2]);
            else dst[i * step] = ARGB(row[8 * i + 6], row[8 * i], row[8 * i + 2], row[8 * i + 4]);
        }
        break;
    }
}

int png_decode(const uint8_t *d, size_t n, Image *out) {
    Png p;
    size_t pos;
    int rc = png_header(d, n, &p, &pos);
    if (rc) return rc;
    /* gather palette / transparency, find the first IDAT */
    const uint8_t *type = NULL, *data = NULL;
    uint32_t len = 0;
    for (;;) {
        rc = next_chunk(d, n, &pos, &type, &data, &len);
        if (rc <= 0) return rc ? rc : IMG_E_TRUNC;
        if (!memcmp(type, "IDAT", 4)) break;
        if (!memcmp(type, "IEND", 4)) return IMG_E_TRUNC;
        ancillary(&p, type, data, len);
    }
    if (p.ctype == 3 && p.npal == 0) return IMG_E_CORRUPT;
    rc = img_alloc(out, p.w, p.h);
    if (rc) return rc;
    out->has_alpha = p.has_alpha;
    size_t full_row = ((size_t)p.w * (size_t)p.bits_pp + 7) / 8;
    uint8_t *cur = malloc(full_row + 1), *prev = malloc(full_row + 1);
    ZStream zs;
    memset(&zs, 0, sizeof zs);
    if (!cur || !prev || z_inflate_init(&zs, Z_ZLIB)) { free(cur); free(prev); return IMG_E_NOMEM; }

    static const int xs[7] = { 0, 4, 0, 2, 0, 1, 0 }, ys[7] = { 0, 0, 4, 0, 2, 0, 1 };
    static const int dx[7] = { 8, 8, 4, 4, 2, 2, 1 }, dy[7] = { 8, 8, 8, 4, 4, 2, 2 };
    int npass = p.interlace ? 7 : 1;
    int finished = 0;
    rc = 0;
    zs.next_in = data;
    zs.avail_in = len;
    for (int pass = 0; pass < npass && !rc; pass++) {
        int pw = p.interlace ? (p.w > xs[pass] ? (p.w - xs[pass] + dx[pass] - 1) / dx[pass] : 0) : p.w;
        int ph = p.interlace ? (p.h > ys[pass] ? (p.h - ys[pass] + dy[pass] - 1) / dy[pass] : 0) : p.h;
        if (pw == 0 || ph == 0) continue;
        size_t rowbytes = ((size_t)pw * (size_t)p.bits_pp + 7) / 8;
        memset(prev, 0, rowbytes + 1);
        for (int y = 0; y < ph && !rc; y++) {
            size_t need = rowbytes + 1, have = 0;
            while (have < need) {
                if (finished) { rc = IMG_E_TRUNC; break; }
                if (zs.avail_in == 0) {                                  /* next IDAT chunk */
                    for (;;) {
                        int r = next_chunk(d, n, &pos, &type, &data, &len);
                        if (r <= 0) { rc = r ? r : IMG_E_TRUNC; break; }
                        if (!memcmp(type, "IDAT", 4)) { zs.next_in = data; zs.avail_in = len; break; }
                        if (!memcmp(type, "IEND", 4)) { rc = IMG_E_TRUNC; break; }
                    }
                    if (rc) break;
                    if (zs.avail_in == 0) continue;
                }
                zs.next_out = cur + have;
                zs.avail_out = need - have;
                int zr = z_inflate(&zs);
                have = need - zs.avail_out;
                if (zr == Z_STREAM_END) finished = 1;
                else if (zr == Z_BUF_ERROR && zs.avail_in > 0) { rc = IMG_E_CORRUPT; break; }   /* cannot happen; never loop forever */
                else if (zr < 0 && zr != Z_BUF_ERROR) { rc = zr == Z_ERR_TRUNC ? IMG_E_TRUNC : IMG_E_CORRUPT; break; }
            }
            if (rc) break;
            if (cur[0] > 4) { rc = IMG_E_CORRUPT; break; }
            unfilter(cur[0], cur + 1, prev + 1, rowbytes, p.bpp);
            int py = p.interlace ? ys[pass] + y * dy[pass] : y;
            uint32_t *dst = out->px + (size_t)py * p.w + (p.interlace ? xs[pass] : 0);
            convert_row(&p, cur + 1, pw, dst, p.interlace ? dx[pass] : 1);
            memcpy(prev + 1, cur + 1, rowbytes);
        }
    }
    z_inflate_end(&zs);
    free(cur);
    free(prev);
    return rc;
}
