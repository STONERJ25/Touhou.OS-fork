/* PNM decoder: P1..P6 (ASCII and binary bitmaps, graymaps, pixmaps; maxval up to 65535) and P7 (PAM). */
#include "imgint.h"

typedef struct {
    int type, w, h, depth;               /* depth: samples per pixel */
    unsigned maxval;
    size_t off;                          /* start of the raster */
    int has_alpha;
} Pnm;

typedef struct { const uint8_t *d; size_t n, pos; } Cur;

static void skip_ws(Cur *c) {
    for (;;) {
        while (c->pos < c->n && (c->d[c->pos] == ' ' || c->d[c->pos] == '\t' || c->d[c->pos] == '\n' || c->d[c->pos] == '\r' || c->d[c->pos] == '\v' || c->d[c->pos] == '\f')) c->pos++;
        if (c->pos < c->n && c->d[c->pos] == '#') { while (c->pos < c->n && c->d[c->pos] != '\n') c->pos++; continue; }
        return;
    }
}

/* An unsigned decimal; returns -1 when there is none. Values above 2^31 saturate (the callers reject them). */
static long long read_uint(Cur *c) {
    skip_ws(c);
    if (c->pos >= c->n || c->d[c->pos] < '0' || c->d[c->pos] > '9') return -1;
    long long v = 0;
    while (c->pos < c->n && c->d[c->pos] >= '0' && c->d[c->pos] <= '9') {
        v = v * 10 + (c->d[c->pos++] - '0');
        if (v > 0x7FFFFFFFLL) v = 0x7FFFFFFFLL;
    }
    return v;
}

static int pnm_header(const uint8_t *d, size_t n, Pnm *p) {
    memset(p, 0, sizeof *p);
    if (n < 3 || d[0] != 'P') return IMG_E_FORMAT;
    Cur c = { d, n, 2 };
    p->type = d[1] - '0';
    if (p->type < 1 || p->type > 7) return IMG_E_FORMAT;
    long long w, h, mv = 1;
    if (p->type == 7) {                                              /* PAM: "KEY value" lines up to ENDHDR */
        if (d[2] != '\n') return IMG_E_FORMAT;
        long long depth = 0;
        w = h = mv = -1;
        for (int lines = 0; lines < 64; lines++) {
            while (c.pos < n && (d[c.pos] == '\n' || d[c.pos] == '\r' || d[c.pos] == ' ')) c.pos++;
            if (c.pos >= n) return IMG_E_TRUNC;
            if (d[c.pos] == '#') { while (c.pos < n && d[c.pos] != '\n') c.pos++; continue; }
            size_t s = c.pos;
            while (c.pos < n && d[c.pos] != ' ' && d[c.pos] != '\n' && d[c.pos] != '\r') c.pos++;
            size_t kl = c.pos - s;
            if (kl == 6 && !memcmp(d + s, "ENDHDR", 6)) {
                while (c.pos < n && d[c.pos] != '\n') c.pos++;
                c.pos++;
                break;
            }
            if (kl == 5 && !memcmp(d + s, "WIDTH", 5)) w = read_uint(&c);
            else if (kl == 6 && !memcmp(d + s, "HEIGHT", 6)) h = read_uint(&c);
            else if (kl == 5 && !memcmp(d + s, "DEPTH", 5)) depth = read_uint(&c);
            else if (kl == 6 && !memcmp(d + s, "MAXVAL", 6)) mv = read_uint(&c);
            else while (c.pos < n && d[c.pos] != '\n') c.pos++;       /* TUPLTYPE and anything else */
        }
        if (w < 0 || h < 0 || depth < 1 || depth > 4 || mv < 1) return IMG_E_CORRUPT;
        p->depth = (int)depth;
        p->has_alpha = depth == 2 || depth == 4;
    } else {
        w = read_uint(&c);
        h = read_uint(&c);
        if (p->type != 1 && p->type != 4) mv = read_uint(&c);
        if (w < 0 || h < 0 || mv < 0) return IMG_E_CORRUPT;
        p->depth = (p->type == 3 || p->type == 6) ? 3 : 1;
        if (p->type >= 4 && c.pos < n) c.pos++;                      /* exactly one whitespace byte before binary data */
    }
    if (mv < 1 || mv > 65535) return IMG_E_CORRUPT;
    int rc = img_check_size(w, h);
    if (rc) return rc;
    p->w = (int)w;
    p->h = (int)h;
    p->maxval = (unsigned)mv;
    p->off = c.pos;
    return 0;
}

int pnm_probe(const uint8_t *d, size_t n, ImgInfo *info) {
    Pnm p;
    int rc = pnm_header(d, n, &p);
    if (rc) return rc;
    info->format = IMG_PNM;
    info->width = p.w;
    info->height = p.h;
    info->has_alpha = p.has_alpha;
    info->frames = 1;
    return 0;
}

static unsigned scale8(unsigned v, unsigned maxval) {
    if (maxval == 255) return v;
    if (maxval == 65535) return v >> 8;
    if (v > maxval) v = maxval;
    return (v * 255 + maxval / 2) / maxval;
}

int pnm_decode(const uint8_t *d, size_t n, Image *out) {
    Pnm p;
    int rc = pnm_header(d, n, &p);
    if (rc) return rc;
    rc = img_alloc(out, p.w, p.h);
    if (rc) return rc;
    out->has_alpha = p.has_alpha;
    size_t npix = (size_t)p.w * (size_t)p.h;
    Cur c = { d, n, p.off };
    int binary = p.type >= 4;
    if (p.type == 4) {                                               /* packed bits, rows padded to a byte, 1 = black */
        size_t stride = ((size_t)p.w + 7) / 8;
        if (n - p.off < stride * (size_t)p.h) { img_free(out); return IMG_E_TRUNC; }
        for (int y = 0; y < p.h; y++)
            for (int x = 0; x < p.w; x++)
                out->px[(size_t)y * p.w + x] = (d[p.off + stride * (size_t)y + (size_t)x / 8] >> (7 - (x & 7))) & 1 ? 0xFF000000u : 0xFFFFFFFFu;
        return 0;
    }
    if (p.type == 1) {                                               /* ASCII bits */
        for (size_t i = 0; i < npix; i++) {
            skip_ws(&c);
            if (c.pos >= n) { img_free(out); return IMG_E_TRUNC; }
            char ch = (char)d[c.pos++];
            if (ch != '0' && ch != '1') { img_free(out); return IMG_E_CORRUPT; }
            out->px[i] = ch == '1' ? 0xFF000000u : 0xFFFFFFFFu;
        }
        return 0;
    }
    int wide = p.maxval > 255;
    size_t bps = wide ? 2 : 1;
    if (binary && n - p.off < npix * (size_t)p.depth * bps) { img_free(out); return IMG_E_TRUNC; }
    const uint8_t *r = d + p.off;
    for (size_t i = 0; i < npix; i++) {
        unsigned s[4] = { 0, 0, 0, 0 };
        for (int k = 0; k < p.depth; k++) {
            if (binary) {
                s[k] = wide ? rd_be16(r) : r[0];
                r += bps;
            } else {
                long long v = read_uint(&c);
                if (v < 0) { img_free(out); return c.pos >= n ? IMG_E_TRUNC : IMG_E_CORRUPT; }
                s[k] = (unsigned)v;
            }
            s[k] = scale8(s[k], p.maxval);
        }
        switch (p.depth) {
        case 1: out->px[i] = ARGB(255, s[0], s[0], s[0]); break;
        case 2: out->px[i] = ARGB(s[1], s[0], s[0], s[0]); break;
        case 3: out->px[i] = ARGB(255, s[0], s[1], s[2]); break;
        default: out->px[i] = ARGB(s[3], s[0], s[1], s[2]); break;
        }
    }
    return 0;
}
