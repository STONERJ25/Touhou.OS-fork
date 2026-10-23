/* TGA decoder: colour-mapped, true-colour and grayscale images, raw and run-length encoded, 8/15/16/24/32 bits, any origin. */
#include "imgint.h"

typedef struct {
    int type, w, h, bpp, desc;
    int idlen, cmap_type, cmap_first, cmap_len, cmap_bits;
    size_t cmap_off, data_off;
    int rle;
} Tga;

static int tga_header(const uint8_t *d, size_t n, Tga *t) {
    memset(t, 0, sizeof *t);
    if (n < 18) return IMG_E_TRUNC;
    t->idlen = d[0];
    t->cmap_type = d[1];
    t->type = d[2];
    if (t->cmap_type > 1) return IMG_E_FORMAT;
    if (!(t->type == 1 || t->type == 2 || t->type == 3 || t->type == 9 || t->type == 10 || t->type == 11)) return IMG_E_FORMAT;
    t->cmap_first = (int)rd_le16(d + 3);
    t->cmap_len = (int)rd_le16(d + 5);
    t->cmap_bits = d[7];
    t->w = (int)rd_le16(d + 12);
    t->h = (int)rd_le16(d + 14);
    t->bpp = d[16];
    t->desc = d[17];
    t->rle = t->type >= 9;
    int rc = img_check_size(t->w, t->h);
    if (rc) return rc;
    int indexed = t->type == 1 || t->type == 9, gray = t->type == 3 || t->type == 11;
    if (indexed) {
        if (t->cmap_type != 1 || t->bpp != 8) return IMG_E_UNSUPPORTED;
        if (!(t->cmap_bits == 15 || t->cmap_bits == 16 || t->cmap_bits == 24 || t->cmap_bits == 32)) return IMG_E_UNSUPPORTED;
    } else if (gray) {
        if (t->bpp != 8) return IMG_E_UNSUPPORTED;
    } else if (!(t->bpp == 15 || t->bpp == 16 || t->bpp == 24 || t->bpp == 32)) return IMG_E_UNSUPPORTED;
    t->cmap_off = 18 + (size_t)t->idlen;
    size_t cmap_bytes = t->cmap_type ? (size_t)t->cmap_len * (size_t)((t->cmap_bits + 7) / 8) : 0;
    t->data_off = t->cmap_off + cmap_bytes;
    if (t->data_off > n) return IMG_E_TRUNC;
    return 0;
}

int tga_probe(const uint8_t *d, size_t n, ImgInfo *info) {
    Tga t;
    int rc = tga_header(d, n, &t);
    if (rc) return rc;
    info->format = IMG_TGA;
    info->width = t.w;
    info->height = t.h;
    info->has_alpha = t.bpp == 32 || (t.type == 1 || t.type == 9 ? t.cmap_bits == 32 : 0) || ((t.bpp == 16 || t.bpp == 15) && (t.desc & 15));
    info->frames = 1;
    return 0;
}

static uint32_t decode_px(const uint8_t *p, int bits, int use_alpha) {
    switch (bits) {
    case 15: case 16: {
        unsigned v = rd_le16(p);
        unsigned r = (v >> 10 & 31) * 255 / 31, g = (v >> 5 & 31) * 255 / 31, b = (v & 31) * 255 / 31;
        return ARGB(use_alpha ? ((v & 0x8000) ? 255 : 0) : 255, r, g, b);
    }
    case 24: return ARGB(255, p[2], p[1], p[0]);
    default: return ARGB(p[3], p[2], p[1], p[0]);
    }
}

int tga_decode(const uint8_t *d, size_t n, Image *out) {
    Tga t;
    int rc = tga_header(d, n, &t);
    if (rc) return rc;
    uint32_t pal[256];
    memset(pal, 0, sizeof pal);
    int indexed = t.type == 1 || t.type == 9, gray = t.type == 3 || t.type == 11;
    int use_alpha16 = (t.desc & 15) != 0;
    if (indexed) {
        size_t eb = (size_t)(t.cmap_bits + 7) / 8;
        for (int i = 0; i < t.cmap_len; i++) {
            int idx = t.cmap_first + i;
            if (idx < 0 || idx > 255) break;
            pal[idx] = decode_px(d + t.cmap_off + (size_t)i * eb, t.cmap_bits, use_alpha16);
        }
    }
    rc = img_alloc(out, t.w, t.h);
    if (rc) return rc;
    size_t bytes_pp = (size_t)(t.bpp + 7) / 8;
    size_t npix = (size_t)t.w * (size_t)t.h;
    const uint8_t *src = d + t.data_off;
    size_t avail = n - t.data_off, pos = 0;
    uint32_t *tmp = out->px;                                         /* decoded in file order, flipped afterwards if needed */
    size_t done = 0;
    uint32_t alpha_or = 0;
    while (done < npix) {
        size_t count = 1;
        int repeat = 0;
        if (t.rle) {
            if (pos >= avail) { img_free(out); return IMG_E_TRUNC; }
            uint8_t hdr = src[pos++];
            count = (size_t)(hdr & 0x7F) + 1;
            repeat = hdr & 0x80;
        }
        size_t need = repeat ? bytes_pp : count * bytes_pp;
        if (pos + need > avail) { img_free(out); return IMG_E_TRUNC; }
        if (count > npix - done) count = npix - done;
        for (size_t k = 0; k < count; k++) {
            const uint8_t *p = src + pos + (repeat ? 0 : k * bytes_pp);
            uint32_t px;
            if (indexed) px = pal[p[0]];
            else if (gray) px = ARGB(255, p[0], p[0], p[0]);
            else px = decode_px(p, t.bpp, use_alpha16);
            tmp[done + k] = px;
            alpha_or |= px >> 24;
        }
        done += count;
        pos += need;
    }
    int has_alpha = t.bpp == 32 || (indexed && t.cmap_bits == 32) || use_alpha16;
    if (has_alpha && (alpha_or & 255) == 0 && t.bpp == 32) {          /* a 32-bit file whose alpha is all zero is treated as opaque */
        for (size_t i = 0; i < npix; i++) tmp[i] |= 0xFF000000u;
        has_alpha = 0;
    }
    out->has_alpha = has_alpha;
    int right_to_left = (t.desc >> 4) & 1, top_down = (t.desc >> 5) & 1;
    if (!top_down) {                                                  /* rows are stored bottom to top unless bit 5 says otherwise */
        for (int y = 0; y < t.h / 2; y++) {
            uint32_t *a = out->px + (size_t)y * t.w, *b = out->px + (size_t)(t.h - 1 - y) * t.w;
            for (int x = 0; x < t.w; x++) { uint32_t tv = a[x]; a[x] = b[x]; b[x] = tv; }
        }
    }
    if (right_to_left) {
        for (int y = 0; y < t.h; y++) {
            uint32_t *row = out->px + (size_t)y * t.w;
            for (int x = 0; x < t.w / 2; x++) { uint32_t tv = row[x]; row[x] = row[t.w - 1 - x]; row[t.w - 1 - x] = tv; }
        }
    }
    return 0;
}
