/*
 * BMP decoder: BITMAPCOREHEADER and the 40..124 byte info headers; 1, 4, 8 bit palettes, 16 (555 or bitfields), 24 and 32 bit
 * pixels; BI_RGB, BI_BITFIELDS, BI_ALPHABITFIELDS, RLE4 and RLE8; bottom-up and top-down. Everything is checked against the
 * size of the file before it is read.
 */
#include "imgint.h"

typedef struct {
    int w, h, topdown, bpp, compression;
    uint32_t off;                       /* start of the pixel data */
    uint32_t colors;                    /* palette entries in the file */
    size_t pal_off, pal_entry;
    uint32_t mask[4];                   /* R G B A for bitfield layouts */
    int has_masks;
    int has_alpha_hdr;                  /* a V4/V5 header or an alpha mask says the alpha channel means something */
} Bmp;

static int bmp_header(const uint8_t *d, size_t n, Bmp *b) {
    memset(b, 0, sizeof *b);
    if (n < 26 || d[0] != 'B' || d[1] != 'M') return IMG_E_FORMAT;
    b->off = rd_le32(d + 10);
    uint32_t hs = rd_le32(d + 14);
    long long w, h;
    if (hs == 12) {
        w = (long long)rd_le16(d + 18);
        h = (long long)rd_le16(d + 20);
        b->bpp = (int)rd_le16(d + 24);
        b->pal_entry = 3;
        b->pal_off = 14 + 12;
        b->colors = b->bpp <= 8 ? 1u << b->bpp : 0;
    } else if (hs >= 40 && hs <= 124) {
        if (n < 14 + hs) return IMG_E_TRUNC;
        w = (int32_t)rd_le32(d + 18);
        h = (int32_t)rd_le32(d + 22);
        b->bpp = (int)rd_le16(d + 28);
        b->compression = (int)rd_le32(d + 30);
        b->colors = rd_le32(d + 46);
        b->pal_entry = 4;
        b->pal_off = 14 + hs;
        if (b->bpp <= 8 && (b->colors == 0 || b->colors > (1u << b->bpp))) b->colors = 1u << b->bpp;
        if (b->bpp > 8) b->colors = 0;
        if (b->compression == 3 || b->compression == 6) {          /* BI_BITFIELDS / BI_ALPHABITFIELDS */
            size_t mo;
            int nm = b->compression == 6 ? 4 : 3;
            if (hs >= 52) mo = 14 + 40;                              /* masks live inside the header */
            else { mo = 14 + hs; b->pal_off += (size_t)nm * 4; }
            if (n < mo + (size_t)nm * 4) return IMG_E_TRUNC;
            for (int i = 0; i < nm; i++) b->mask[i] = rd_le32(d + mo + (size_t)i * 4);
            if (hs >= 56 && nm == 3) b->mask[3] = rd_le32(d + 14 + 52);
            b->has_masks = 1;
        } else if (b->compression == 0 && hs >= 108 && (b->bpp == 16 || b->bpp == 32)) {
            /* V4/V5 headers carry masks even for BI_RGB when the writer meant them */
            uint32_t m[4];
            for (int i = 0; i < 4; i++) m[i] = rd_le32(d + 14 + 40 + (size_t)i * 4);
            if (m[0] | m[1] | m[2]) { memcpy(b->mask, m, sizeof m); b->has_masks = 1; }
        }
        if (b->has_masks && b->mask[3]) b->has_alpha_hdr = 1;
    } else return IMG_E_UNSUPPORTED;
    b->topdown = h < 0;
    if (h < 0) h = -h;
    int rc = img_check_size(w, h);
    if (rc) return rc;
    b->w = (int)w;
    b->h = (int)h;
    switch (b->bpp) {
    case 1: case 4: case 8: case 16: case 24: case 32: break;
    default: return IMG_E_UNSUPPORTED;
    }
    if (b->compression == 1 && b->bpp != 8) return IMG_E_CORRUPT;
    if (b->compression == 2 && b->bpp != 4) return IMG_E_CORRUPT;
    if (b->compression > 3 && b->compression != 6) return IMG_E_UNSUPPORTED;
    if ((b->compression == 3 || b->compression == 6) && b->bpp != 16 && b->bpp != 32) return IMG_E_CORRUPT;
    if (b->off > n) return IMG_E_TRUNC;
    return 0;
}

int bmp_probe(const uint8_t *d, size_t n, ImgInfo *info) {
    Bmp b;
    int rc = bmp_header(d, n, &b);
    if (rc) return rc;
    info->format = IMG_BMP;
    info->width = b.w;
    info->height = b.h;
    info->has_alpha = b.has_alpha_hdr;
    info->frames = 1;
    return 0;
}

static int shift_of(uint32_t m) { int s = 0; while (m && !(m & 1)) { m >>= 1; s++; } return s; }
static int bits_of(uint32_t m) { m >>= shift_of(m); int c = 0; while (m & 1) { c++; m >>= 1; } return c; }

static uint32_t extract(uint32_t v, uint32_t mask) {
    if (!mask) return 255;
    int s = shift_of(mask), nb = bits_of(mask);
    uint32_t x = (v & mask) >> s;
    if (nb >= 8) return x >> (nb - 8);
    if (nb == 0) return 0;
    return x * 255 / ((1u << nb) - 1);
}

int bmp_decode(const uint8_t *d, size_t n, Image *out) {
    Bmp b;
    int rc = bmp_header(d, n, &b);
    if (rc) return rc;
    uint32_t pal[256];
    memset(pal, 0, sizeof pal);
    if (b.bpp <= 8) {
        size_t need = b.pal_off + (size_t)b.colors * b.pal_entry;
        if (need > n) return IMG_E_TRUNC;
        for (uint32_t i = 0; i < b.colors && i < 256; i++) {
            const uint8_t *e = d + b.pal_off + (size_t)i * b.pal_entry;
            pal[i] = ARGB(255, e[2], e[1], e[0]);
        }
    }
    rc = img_alloc(out, b.w, b.h);
    if (rc) return rc;
    size_t stride = (((size_t)b.w * (size_t)b.bpp + 31) / 32) * 4;
    const uint8_t *px = d + b.off;
    size_t avail = n - b.off;

    if (b.compression == 1 || b.compression == 2) {                  /* RLE8 / RLE4 */
        for (size_t i = 0; i < (size_t)b.w * b.h; i++) out->px[i] = pal[0];
        int x = 0, y = 0;                                            /* y counts from the first stored row */
        size_t i = 0;
        int rle4 = b.compression == 2;
        while (y < b.h) {
            if (i + 2 > avail) { rc = IMG_E_TRUNC; break; }
            uint8_t count = px[i], val = px[i + 1];
            i += 2;
            int row = b.topdown ? y : b.h - 1 - y;
            if (count) {                                             /* encoded run */
                for (int k = 0; k < count && x < b.w; k++, x++) {
                    int idx = rle4 ? ((k & 1) ? (val & 15) : (val >> 4)) : val;
                    out->px[(size_t)row * b.w + x] = pal[idx];
                }
                continue;
            }
            if (val == 0) { x = 0; y++; }                            /* end of line */
            else if (val == 1) break;                                /* end of bitmap */
            else if (val == 2) {                                     /* move right and down */
                if (i + 2 > avail) { rc = IMG_E_TRUNC; break; }
                x += px[i];
                y += px[i + 1];
                i += 2;
                if (x > b.w) x = b.w;
            } else {                                                 /* absolute run of val pixels, padded to a 16-bit boundary */
                size_t bytes = rle4 ? ((size_t)val + 1) / 2 : val;
                if (i + bytes > avail) { rc = IMG_E_TRUNC; break; }
                for (int k = 0; k < val && x < b.w; k++, x++) {
                    int idx = rle4 ? ((k & 1) ? (px[i + (size_t)k / 2] & 15) : (px[i + (size_t)k / 2] >> 4)) : px[i + (size_t)k];
                    out->px[(size_t)row * b.w + x] = pal[idx];
                }
                i += bytes + (bytes & 1);
            }
        }
        if (rc) img_free(out);
        return rc;
    }

    if (avail < stride * (size_t)b.h && (stride == 0 || avail / stride < (size_t)b.h)) { img_free(out); return IMG_E_TRUNC; }
    int alpha_seen = 0;
    for (int y = 0; y < b.h; y++) {
        const uint8_t *row = px + stride * (size_t)y;
        uint32_t *dst = out->px + (size_t)(b.topdown ? y : b.h - 1 - y) * b.w;
        switch (b.bpp) {
        case 1: for (int x = 0; x < b.w; x++) dst[x] = pal[(row[x >> 3] >> (7 - (x & 7))) & 1]; break;
        case 4: for (int x = 0; x < b.w; x++) dst[x] = pal[(x & 1) ? (row[x >> 1] & 15) : (row[x >> 1] >> 4)]; break;
        case 8: for (int x = 0; x < b.w; x++) dst[x] = pal[row[x]]; break;
        case 16:
            for (int x = 0; x < b.w; x++) {
                uint32_t v = rd_le16(row + 2 * x);
                if (b.has_masks) dst[x] = ARGB(b.mask[3] ? extract(v, b.mask[3]) : 255, extract(v, b.mask[0]), extract(v, b.mask[1]), extract(v, b.mask[2]));
                else dst[x] = ARGB(255, (v >> 10 & 31) * 255 / 31, (v >> 5 & 31) * 255 / 31, (v & 31) * 255 / 31);
            }
            break;
        case 24: for (int x = 0; x < b.w; x++) dst[x] = ARGB(255, row[3 * x + 2], row[3 * x + 1], row[3 * x]); break;
        default:
            for (int x = 0; x < b.w; x++) {
                uint32_t v = rd_le32(row + 4 * x);
                if (b.has_masks) {
                    uint32_t a = b.mask[3] ? extract(v, b.mask[3]) : 255;
                    dst[x] = ARGB(a, extract(v, b.mask[0]), extract(v, b.mask[1]), extract(v, b.mask[2]));
                } else {
                    dst[x] = ARGB(255, row[4 * x + 2], row[4 * x + 1], row[4 * x]);       /* BI_RGB: the fourth byte is reserved */
                }
            }
            break;
        }
    }
    if (b.bpp == 32 || b.bpp == 16) alpha_seen = b.has_alpha_hdr;
    out->has_alpha = alpha_seen;
    return 0;
}
