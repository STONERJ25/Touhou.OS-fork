/*
 * GIF decoder: LZW, global and local palettes, transparency, interlacing, and animation (frame list, disposal methods).
 * img_decode() returns the first frame composed on the logical screen; the ImgAnim interface steps through all frames.
 */
#include "imgint.h"

#define GIF_MAX_FRAMES 4096

typedef struct {
    size_t lzw_off;                      /* offset of the LZW minimum code size byte */
    int left, top, w, h, interlaced;
    size_t lct_off;
    int lct_n;
    int transparent;                     /* palette index or -1 */
    int disposal, delay_cs;
} Frame;

struct ImgAnim {
    const uint8_t *d;
    size_t n;
    int w, h, nframes, loops;
    uint32_t gct[256];
    int gct_n;
    Frame *frames;
    Image canvas;
    uint32_t *backup;                    /* canvas before a frame whose disposal is "restore previous" */
    int next, prev_disposal;
    int prev_left, prev_top, prev_w, prev_h;
};

static int skip_sub_blocks(const uint8_t *d, size_t n, size_t *pos) {
    for (;;) {
        if (*pos >= n) return IMG_E_TRUNC;
        size_t len = d[(*pos)++];
        if (len == 0) return 0;
        if (*pos + len > n) return IMG_E_TRUNC;
        *pos += len;
    }
}

/* Parses the header and the block structure (not the pixel data). */
static int gif_scan(const uint8_t *d, size_t n, ImgAnim *a) {
    if (n < 13 || memcmp(d, "GIF8", 4) || (d[4] != '7' && d[4] != '9') || d[5] != 'a') return IMG_E_FORMAT;
    a->d = d;
    a->n = n;
    a->w = (int)rd_le16(d + 6);
    a->h = (int)rd_le16(d + 8);
    int rc = img_check_size(a->w, a->h);
    if (rc) return rc;
    unsigned flags = d[10];
    size_t pos = 13;
    if (flags & 0x80) {
        a->gct_n = 2 << (flags & 7);
        if (pos + (size_t)a->gct_n * 3 > n) return IMG_E_TRUNC;
        for (int i = 0; i < a->gct_n; i++) a->gct[i] = ARGB(255, d[pos + 3 * (size_t)i], d[pos + 3 * (size_t)i + 1], d[pos + 3 * (size_t)i + 2]);
        pos += (size_t)a->gct_n * 3;
    }
    int cap = 8;
    a->frames = malloc((size_t)cap * sizeof(Frame));
    if (!a->frames) return IMG_E_NOMEM;
    int gc_transparent = -1, gc_disposal = 0, gc_delay = 0;
    a->loops = 1;
    for (;;) {
        if (pos >= n) { rc = a->nframes ? 0 : IMG_E_TRUNC; break; }       /* a missing trailer after at least one frame is tolerated */
        uint8_t b = d[pos++];
        if (b == 0x3B) break;
        if (b == 0x21) {
            if (pos >= n) { rc = a->nframes ? 0 : IMG_E_TRUNC; break; }
            uint8_t label = d[pos++];
            if (label == 0xF9 && pos + 6 <= n && d[pos] == 4) {
                unsigned fl = d[pos + 1];
                gc_disposal = (int)((fl >> 2) & 7);
                gc_delay = (int)rd_le16(d + pos + 2);
                gc_transparent = (fl & 1) ? d[pos + 4] : -1;
            } else if (label == 0xFF && pos + 17 <= n && d[pos] == 11 && !memcmp(d + pos + 1, "NETSCAPE2.0", 11) && d[pos + 12] == 3 && d[pos + 13] == 1) {
                a->loops = (int)rd_le16(d + pos + 14);
            }
            rc = skip_sub_blocks(d, n, &pos);
            if (rc) { rc = a->nframes ? 0 : rc; break; }
            continue;
        }
        if (b == 0x2C) {
            if (pos + 9 > n) { rc = a->nframes ? 0 : IMG_E_TRUNC; break; }
            if (a->nframes == cap) {
                if (cap >= GIF_MAX_FRAMES) break;
                cap *= 2;
                Frame *nf = realloc(a->frames, (size_t)cap * sizeof(Frame));
                if (!nf) { rc = IMG_E_NOMEM; break; }
                a->frames = nf;
            }
            Frame *f = &a->frames[a->nframes];
            memset(f, 0, sizeof *f);
            f->left = (int)rd_le16(d + pos);
            f->top = (int)rd_le16(d + pos + 2);
            f->w = (int)rd_le16(d + pos + 4);
            f->h = (int)rd_le16(d + pos + 6);
            unsigned ff = d[pos + 8];
            pos += 9;
            f->interlaced = (ff & 0x40) != 0;
            if (ff & 0x80) {
                f->lct_n = 2 << (ff & 7);
                f->lct_off = pos;
                if (pos + (size_t)f->lct_n * 3 > n) { rc = a->nframes ? 0 : IMG_E_TRUNC; break; }
                pos += (size_t)f->lct_n * 3;
            }
            if (pos >= n) { rc = a->nframes ? 0 : IMG_E_TRUNC; break; }
            f->lzw_off = pos;
            f->transparent = gc_transparent;
            f->disposal = gc_disposal;
            f->delay_cs = gc_delay;
            gc_transparent = -1; gc_disposal = 0; gc_delay = 0;
            pos++;
            int r = skip_sub_blocks(d, n, &pos);
            if (r) { rc = a->nframes ? 0 : r; break; }
            a->nframes++;
            continue;
        }
        rc = a->nframes ? 0 : IMG_E_CORRUPT;                              /* unknown block */
        break;
    }
    if (!rc && a->nframes == 0) rc = IMG_E_CORRUPT;
    return rc;
}

int gif_probe(const uint8_t *d, size_t n, ImgInfo *info) {
    ImgAnim a;
    memset(&a, 0, sizeof a);
    int rc = gif_scan(d, n, &a);
    if (!rc) {
        info->format = IMG_GIF;
        info->width = a.w;
        info->height = a.h;
        info->frames = a.nframes;
        info->has_alpha = 0;
        for (int i = 0; i < a.nframes; i++) if (a.frames[i].transparent >= 0) info->has_alpha = 1;
    }
    free(a.frames);
    return rc;
}

/* LZW-decodes one frame's pixels into idx[w*h] (palette indices, in stored row order). */
static int lzw_decode(const uint8_t *d, size_t n, size_t pos, uint8_t *idx, size_t total) {
    if (pos >= n) return IMG_E_TRUNC;
    int min = d[pos++];
    if (min < 2 || min > 8) return IMG_E_CORRUPT;
    uint16_t prefix[4096];
    uint8_t suffix[4096], stack[4097];
    int clear = 1 << min, eoi = clear + 1, next = eoi + 1, size = min + 1;
    for (int i = 0; i < clear; i++) { prefix[i] = 0; suffix[i] = (uint8_t)i; }
    uint32_t bitbuf = 0;
    int bits = 0, prev = -1, first = 0;
    size_t out = 0, blk_left = 0;
    int ended = 0;
    while (out < total) {
        while (bits < size) {
            if (blk_left == 0) {
                if (pos >= n) { ended = 1; break; }
                blk_left = d[pos++];
                if (blk_left == 0) { ended = 1; break; }
            }
            if (pos >= n) { ended = 1; break; }
            bitbuf |= (uint32_t)d[pos++] << bits;
            bits += 8;
            blk_left--;
        }
        if (ended && bits < size) break;
        int code = (int)(bitbuf & ((1u << size) - 1));
        bitbuf >>= size;
        bits -= size;
        if (code == clear) { size = min + 1; next = eoi + 1; prev = -1; continue; }
        if (code == eoi) break;
        if (prev < 0) {
            if (code >= clear) return IMG_E_CORRUPT;
            idx[out++] = (uint8_t)code;
            prev = code;
            first = code;
            continue;
        }
        int cur = code, sp = 0;
        if (code >= next) {
            if (code > next) return IMG_E_CORRUPT;
            cur = prev;
            stack[sp++] = (uint8_t)first;                            /* KwKwK: the new entry is prev + first(prev) */
        }
        while (cur >= clear) {
            if (sp >= 4096) return IMG_E_CORRUPT;
            stack[sp++] = suffix[cur];
            cur = prefix[cur];
        }
        stack[sp++] = suffix[cur];
        first = stack[sp - 1];
        while (sp > 0 && out < total) idx[out++] = stack[--sp];
        if (next < 4096) {
            prefix[next] = (uint16_t)prev;
            suffix[next] = (uint8_t)first;
            next++;
            if (next == (1 << size) && size < 12) size++;
        }
        prev = code;
    }
    if (out < total) {
        memset(idx + out, 0, total - out);                           /* a short stream leaves the rest at index 0 */
        return IMG_E_TRUNC;
    }
    return 0;
}

/* Stored row r of an interlaced frame is displayed on row interlaced_row(r, h): passes of every 8th row from 0, every 8th from 4,
 * every 4th from 2, every 2nd from 1. */
static int interlaced_row(int r, int h) {
    int n0 = (h + 7) / 8, n1 = (h + 3) / 8, n2 = (h + 1) / 4;
    if (r < n0) return r * 8;
    r -= n0;
    if (r < n1) return 4 + r * 8;
    r -= n1;
    if (r < n2) return 2 + r * 4;
    r -= n2;
    return 1 + r * 2;
}

/* Draws frame number k onto the canvas. */
static int draw_frame(ImgAnim *a, int k, int tolerate_short) {
    Frame *f = &a->frames[k];
    if ((long long)f->w * f->h > (long long)IMG_MAX_PIXELS) return IMG_E_TOOBIG;
    size_t total = (size_t)f->w * (size_t)f->h;
    uint8_t *idx = malloc(total ? total : 1);
    if (!idx) return IMG_E_NOMEM;
    int rc = lzw_decode(a->d, a->n, f->lzw_off, idx, total);
    if (rc && !(tolerate_short && rc == IMG_E_TRUNC)) { free(idx); return rc; }
    const uint32_t *pal = a->gct;
    uint32_t local[256];
    int pal_n = a->gct_n;
    if (f->lct_n) {
        for (int i = 0; i < f->lct_n; i++) local[i] = ARGB(255, a->d[f->lct_off + 3 * (size_t)i], a->d[f->lct_off + 3 * (size_t)i + 1], a->d[f->lct_off + 3 * (size_t)i + 2]);
        pal = local;
        pal_n = f->lct_n;
    }
    for (int r = 0; r < f->h; r++) {
        int dy = f->interlaced ? interlaced_row(r, f->h) : r;
        int cy = f->top + dy;
        if (cy < 0 || cy >= a->h) continue;
        const uint8_t *src = idx + (size_t)r * f->w;
        uint32_t *dst = a->canvas.px + (size_t)cy * a->w;
        for (int x = 0; x < f->w; x++) {
            int cx = f->left + x;
            if (cx >= a->w) break;
            int v = src[x];
            if (v == f->transparent) continue;
            dst[cx] = v < pal_n ? pal[v] : ARGB(255, 0, 0, 0);
        }
    }
    free(idx);
    return 0;
}

static void clear_rect(ImgAnim *a, int left, int top, int w, int h) {
    for (int y = top; y < top + h && y < a->h; y++) {
        if (y < 0) continue;
        for (int x = left; x < left + w && x < a->w; x++) if (x >= 0) a->canvas.px[(size_t)y * a->w + x] = 0;
    }
}

int img_anim_open(const void *data, size_t len, ImgAnim **out) {
    *out = NULL;
    if (img_sniff(data, len) != IMG_GIF) return IMG_E_FORMAT;
    ImgAnim *a = calloc(1, sizeof *a);
    if (!a) return IMG_E_NOMEM;
    int rc = gif_scan(data, len, a);
    if (!rc) rc = img_alloc(&a->canvas, a->w, a->h);
    if (rc) { free(a->frames); free(a); return rc; }
    *out = a;
    return 0;
}

void img_anim_close(ImgAnim *a) {
    if (!a) return;
    free(a->frames);
    free(a->backup);
    img_free(&a->canvas);
    free(a);
}

void img_anim_info(const ImgAnim *a, int *w, int *h, int *frames, int *loops) {
    if (w) *w = a->w;
    if (h) *h = a->h;
    if (frames) *frames = a->nframes;
    if (loops) *loops = a->loops;
}

void img_anim_rewind(ImgAnim *a) {
    a->next = 0;
    a->prev_disposal = 0;
    memset(a->canvas.px, 0, (size_t)a->w * a->h * 4);
}

int img_anim_next(ImgAnim *a, const Image **canvas, int *delay_ms) {
    if (a->next >= a->nframes) img_anim_rewind(a);
    int k = a->next;
    Frame *f = &a->frames[k];
    if (k > 0 || a->prev_disposal) {                                 /* undo the previous frame as it asked */
        if (a->prev_disposal == 2) clear_rect(a, a->prev_left, a->prev_top, a->prev_w, a->prev_h);
        else if (a->prev_disposal == 3 && a->backup) memcpy(a->canvas.px, a->backup, (size_t)a->w * a->h * 4);
    }
    if (f->disposal == 3) {
        if (!a->backup) a->backup = malloc((size_t)a->w * a->h * 4);
        if (a->backup) memcpy(a->backup, a->canvas.px, (size_t)a->w * a->h * 4);
    }
    int rc = draw_frame(a, k, 1);
    if (rc) return rc;
    a->prev_disposal = f->disposal;
    a->prev_left = f->left; a->prev_top = f->top; a->prev_w = f->w; a->prev_h = f->h;
    a->next = k + 1;
    int has_alpha = 0;
    for (size_t i = 0; i < (size_t)a->w * a->h && !has_alpha; i++) if ((a->canvas.px[i] >> 24) != 255) has_alpha = 1;
    a->canvas.has_alpha = has_alpha;
    *canvas = &a->canvas;
    if (delay_ms) *delay_ms = f->delay_cs <= 1 ? 100 : f->delay_cs * 10;       /* like browsers: 0 or 1 centisecond means 0.1 s */
    return 0;
}

int gif_decode(const uint8_t *d, size_t n, Image *out) {
    ImgAnim *a;
    int rc = img_anim_open(d, n, &a);
    if (rc) return rc;
    const Image *c;
    rc = img_anim_next(a, &c, NULL);
    if (!rc) {
        rc = img_alloc(out, c->w, c->h);
        if (!rc) { memcpy(out->px, c->px, (size_t)c->w * c->h * 4); out->has_alpha = c->has_alpha; }
    }
    img_anim_close(a);
    return rc;
}
