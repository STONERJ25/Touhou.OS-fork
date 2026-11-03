/*
 * JPEG decoder: baseline, extended-sequential 8-bit and progressive Huffman JPEG; grayscale, YCbCr, RGB and (Adobe) CMYK/YCCK;
 * any sampling factors that divide evenly (4:4:4, 4:2:2, 4:2:0, 4:4:0, 4:1:1 ...); restart intervals; EXIF orientation.
 *
 * The integer "islow" IDCT, the fixed-point colour conversion and the "fancy" (triangle filter) chroma upsampling are the
 * ones libjpeg uses, so the output agrees with libjpeg-based decoders to within a rounding step.
 * Not supported: arithmetic coding, lossless, 12-bit samples, hierarchical mode, a frame height of 0 (DNL).
 */
#include "imgint.h"

#define MAXCOMP 4
#define MAX_COEF_BYTES (256u << 20)
#define MAX_SCANS 64      /* real files have about ten; with EOB runs a repeated progressive scan costs a few bytes to store and a full pass over the picture to decode */

typedef struct {
    uint16_t fast[512];                  /* (length << 8) | symbol for codes of up to 9 bits, 0 = longer */
    uint8_t  bits[17];
    uint8_t  sym[256];
    int32_t  maxcode[18];
    int32_t  valoff[17];
    int      built;
} Huff;

typedef struct {
    int id, h, v, tq;
    int bw, bh;                          /* block grid padded to whole MCUs */
    int cw, ch;                          /* real size in samples */
    int tbw, tbh;                        /* real size in blocks */
    int dc_tbl, ac_tbl;
    int pred;
    int16_t *coef;                       /* progressive: bw * bh * 64 coefficients in natural order */
    uint8_t *plane;                      /* (bw * 8) x (bh * 8) samples */
} Comp;

typedef struct {
    const uint8_t *d;
    size_t n, pos;
    int w, h, ncomp, progressive, have_frame;
    Comp c[MAXCOMP];
    int hmax, vmax, mcux, mcuy;
    uint16_t qt[4][64];                  /* natural order */
    int qt_ok[4];
    Huff dc[4], ac[4];
    int restart_interval;
    int adobe, adobe_transform, jfif;
    int orientation;
    /* entropy-coded segment reader */
    uint32_t bitbuf;
    int bitcnt;
    int marker;                          /* a marker byte met inside the entropy data */
    int fake_bits;                       /* bits invented after the end of the data or a marker */
    int over;                            /* some of those invented bits were consumed */
    int eobrun;
    int scan_done;
} Jpeg;

static const uint8_t g_zig[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* ---------------------------------------------------------------- bits and Huffman codes */

static void fill_bits(Jpeg *j) {
    while (j->bitcnt <= 24) {
        uint32_t b = 0;
        if (j->marker || j->pos >= j->n) {
            j->fake_bits += 8;
        } else {
            b = j->d[j->pos];
            if (b == 0xFF) {
                size_t q = j->pos + 1;
                while (q < j->n && j->d[q] == 0xFF) q++;             /* fill bytes */
                if (q >= j->n) { j->pos = j->n; b = 0; j->fake_bits += 8; }
                else if (j->d[q] == 0) { j->pos = q + 1; }            /* stuffed zero: a literal 0xFF */
                else { j->marker = j->d[q]; j->pos = q - 1; b = 0; j->fake_bits += 8; }
            } else j->pos++;
        }
        j->bitbuf |= b << (24 - j->bitcnt);
        j->bitcnt += 8;
    }
}

static inline void consume(Jpeg *j, int n) {
    j->bitbuf <<= n;
    j->bitcnt -= n;
    if (j->fake_bits > j->bitcnt) { j->over = 1; j->fake_bits = j->bitcnt; }
}

static inline int get_bits(Jpeg *j, int n) {
    if (n == 0) return 0;
    fill_bits(j);
    int v = (int)(j->bitbuf >> (32 - n));
    consume(j, n);
    return v;
}

static inline int get_bit(Jpeg *j) { return get_bits(j, 1); }

static inline int extend(int v, int t) { return v < (1 << (t - 1)) ? v - (1 << t) + 1 : v; }

static int huff_build(Huff *h, const uint8_t *counts, const uint8_t *syms, int nsyms) {
    memset(h, 0, sizeof *h);
    memcpy(h->bits, counts, 17);
    memcpy(h->sym, syms, (size_t)nsyms);
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        h->valoff[l] = k - code;
        if (code + h->bits[l] > (1 << l)) return IMG_E_CORRUPT;      /* over-subscribed */
        for (int i = 0; i < h->bits[l]; i++, code++, k++) {
            if (l <= 9) {
                int base = code << (9 - l);
                for (int f = 0; f < (1 << (9 - l)); f++) h->fast[base + f] = (uint16_t)((l << 8) | h->sym[k]);
            }
        }
        h->maxcode[l] = h->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
    h->built = 1;
    return 0;
}

static int huff_decode(Jpeg *j, const Huff *h) {
    fill_bits(j);
    uint16_t f = h->fast[j->bitbuf >> 23];
    if (f) { consume(j, f >> 8); return f & 255; }
    for (int l = 10; l <= 16; l++) {
        int32_t code = (int32_t)(j->bitbuf >> (32 - l));
        if (h->maxcode[l] >= 0 && code <= h->maxcode[l]) {
            int idx = code + h->valoff[l];
            if (idx < 0 || idx > 255) return -1;
            consume(j, l);
            return h->sym[idx];
        }
    }
    return -1;
}

/* ---------------------------------------------------------------- IDCT (libjpeg "islow") */

#define CONST_BITS 13
#define PASS1_BITS 2
#define F_0_298 2446
#define F_0_390 3196
#define F_0_541 4433
#define F_0_765 6270
#define F_0_899 7373
#define F_1_175 9633
#define F_1_501 12299
#define F_1_847 15137
#define F_1_961 16069
#define F_2_053 16819
#define F_2_562 20995
#define F_3_072 25172
#define DESCALE(x, n) (((x) + (1 << ((n) - 1))) >> (n))

static inline uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

typedef int64_t I64;                     /* hostile coefficients must not overflow: do the arithmetic in 64 bits */

static inline I64 dq(int16_t c, uint16_t q) {
    I64 v = (I64)c * q;
    return v < -8192 ? -8192 : v > 8192 ? 8192 : v;      /* real images stay far inside this range */
}

static void idct_block(const int16_t *in, const uint16_t *q, uint8_t *out, int stride) {
    I64 ws[64];
    for (int c = 0; c < 8; c++) {
        const int16_t *ip = in + c;
        I64 *wp = ws + c;
        if (!ip[8] && !ip[16] && !ip[24] && !ip[32] && !ip[40] && !ip[48] && !ip[56]) {
            I64 dc = dq(ip[0], q[c]) * (1 << PASS1_BITS);
            for (int r = 0; r < 8; r++) wp[r * 8] = dc;
            continue;
        }
        I64 z2 = dq(ip[16], q[c + 16]), z3 = dq(ip[48], q[c + 48]);
        I64 z1 = (z2 + z3) * F_0_541;
        I64 tmp2 = z1 + z3 * (-F_1_847), tmp3 = z1 + z2 * F_0_765;
        z2 = dq(ip[0], q[c]); z3 = dq(ip[32], q[c + 32]);
        I64 tmp0 = (z2 + z3) * (1 << CONST_BITS), tmp1 = (z2 - z3) * (1 << CONST_BITS);
        I64 tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3, tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
        tmp0 = dq(ip[56], q[c + 56]); tmp1 = dq(ip[40], q[c + 40]); tmp2 = dq(ip[24], q[c + 24]); tmp3 = dq(ip[8], q[c + 8]);
        z1 = tmp0 + tmp3; z2 = tmp1 + tmp2; z3 = tmp0 + tmp2;
        I64 z4 = tmp1 + tmp3, z5 = (z3 + z4) * F_1_175;
        tmp0 *= F_0_298; tmp1 *= F_2_053; tmp2 *= F_3_072; tmp3 *= F_1_501;
        z1 *= -F_0_899; z2 *= -F_2_562; z3 *= -F_1_961; z4 *= -F_0_390;
        z3 += z5; z4 += z5;
        tmp0 += z1 + z3; tmp1 += z2 + z4; tmp2 += z2 + z3; tmp3 += z1 + z4;
        wp[0] = DESCALE(tmp10 + tmp3, CONST_BITS - PASS1_BITS);
        wp[56] = DESCALE(tmp10 - tmp3, CONST_BITS - PASS1_BITS);
        wp[8] = DESCALE(tmp11 + tmp2, CONST_BITS - PASS1_BITS);
        wp[48] = DESCALE(tmp11 - tmp2, CONST_BITS - PASS1_BITS);
        wp[16] = DESCALE(tmp12 + tmp1, CONST_BITS - PASS1_BITS);
        wp[40] = DESCALE(tmp12 - tmp1, CONST_BITS - PASS1_BITS);
        wp[24] = DESCALE(tmp13 + tmp0, CONST_BITS - PASS1_BITS);
        wp[32] = DESCALE(tmp13 - tmp0, CONST_BITS - PASS1_BITS);
    }
    for (int r = 0; r < 8; r++) {
        const I64 *wp = ws + r * 8;
        uint8_t *op = out + r * stride;
        if (!wp[1] && !wp[2] && !wp[3] && !wp[4] && !wp[5] && !wp[6] && !wp[7]) {
            uint8_t dc = clamp8((int)(DESCALE(wp[0], PASS1_BITS + 3) + 128));
            for (int x = 0; x < 8; x++) op[x] = dc;
            continue;
        }
        I64 z2 = wp[2], z3 = wp[6];
        I64 z1 = (z2 + z3) * F_0_541;
        I64 tmp2 = z1 + z3 * (-F_1_847), tmp3 = z1 + z2 * F_0_765;
        I64 tmp0 = (wp[0] + wp[4]) * (1 << CONST_BITS), tmp1 = (wp[0] - wp[4]) * (1 << CONST_BITS);
        I64 tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3, tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
        tmp0 = wp[7]; tmp1 = wp[5]; tmp2 = wp[3]; tmp3 = wp[1];
        z1 = tmp0 + tmp3; z2 = tmp1 + tmp2; z3 = tmp0 + tmp2;
        I64 z4 = tmp1 + tmp3, z5 = (z3 + z4) * F_1_175;
        tmp0 *= F_0_298; tmp1 *= F_2_053; tmp2 *= F_3_072; tmp3 *= F_1_501;
        z1 *= -F_0_899; z2 *= -F_2_562; z3 *= -F_1_961; z4 *= -F_0_390;
        z3 += z5; z4 += z5;
        tmp0 += z1 + z3; tmp1 += z2 + z4; tmp2 += z2 + z3; tmp3 += z1 + z4;
        const int sh = CONST_BITS + PASS1_BITS + 3;
        op[0] = clamp8((int)(DESCALE(tmp10 + tmp3, sh) + 128));
        op[7] = clamp8((int)(DESCALE(tmp10 - tmp3, sh) + 128));
        op[1] = clamp8((int)(DESCALE(tmp11 + tmp2, sh) + 128));
        op[6] = clamp8((int)(DESCALE(tmp11 - tmp2, sh) + 128));
        op[2] = clamp8((int)(DESCALE(tmp12 + tmp1, sh) + 128));
        op[5] = clamp8((int)(DESCALE(tmp12 - tmp1, sh) + 128));
        op[3] = clamp8((int)(DESCALE(tmp13 + tmp0, sh) + 128));
        op[4] = clamp8((int)(DESCALE(tmp13 - tmp0, sh) + 128));
    }
}

/* ---------------------------------------------------------------- segment parsing */

static int read_exif(Jpeg *j, const uint8_t *p, size_t len) {
    if (len < 14 || memcmp(p, "Exif\0\0", 6)) return 0;
    const uint8_t *t = p + 6;
    size_t tl = len - 6;
    int le = t[0] == 'I' && t[1] == 'I';
    if (!le && !(t[0] == 'M' && t[1] == 'M')) return 0;
#define R16(o) ((o) + 2 <= tl ? (le ? rd_le16(t + (o)) : rd_be16(t + (o))) : 0)
#define R32(o) ((o) + 4 <= tl ? (le ? rd_le32(t + (o)) : rd_be32(t + (o))) : 0)
    if (R16(2) != 42) return 0;
    uint32_t ifd = R32(4);
    if (ifd < 8 || ifd + 2 > tl) return 0;
    unsigned cnt = R16(ifd);
    for (unsigned i = 0; i < cnt && i < 64; i++) {
        size_t e = ifd + 2 + (size_t)i * 12;
        if (e + 12 > tl) break;
        if (R16(e) == 0x0112 && R16(e + 2) == 3) {
            unsigned o = R16(e + 8);
            if (o >= 1 && o <= 8) j->orientation = (int)o;
        }
    }
#undef R16
#undef R32
    return 0;
}

static int parse_dqt(Jpeg *j, const uint8_t *p, size_t len) {
    size_t i = 0;
    while (i < len) {
        int pq = p[i] >> 4, tq = p[i] & 15;
        i++;
        if (tq > 3 || pq > 1) return IMG_E_CORRUPT;
        size_t need = pq ? 128 : 64;
        if (i + need > len) return IMG_E_CORRUPT;
        for (int k = 0; k < 64; k++) {
            j->qt[tq][g_zig[k]] = pq ? (uint16_t)rd_be16(p + i + (size_t)k * 2) : p[i + (size_t)k];
        }
        j->qt_ok[tq] = 1;
        i += need;
    }
    return 0;
}

static int parse_dht(Jpeg *j, const uint8_t *p, size_t len) {
    size_t i = 0;
    while (i + 17 <= len) {
        int tc = p[i] >> 4, th = p[i] & 15;
        if (tc > 1 || th > 3) return IMG_E_CORRUPT;
        uint8_t counts[17];
        counts[0] = 0;
        int total = 0;
        for (int l = 1; l <= 16; l++) { counts[l] = p[i + (size_t)l]; total += counts[l]; }
        i += 17;
        if (total > 256 || i + (size_t)total > len) return IMG_E_CORRUPT;
        uint8_t syms[256];
        memset(syms, 0, sizeof syms);
        memcpy(syms, p + i, (size_t)total);
        i += (size_t)total;
        int rc = huff_build(tc ? &j->ac[th] : &j->dc[th], counts, syms, total);
        if (rc) return rc;
    }
    return 0;
}

static int parse_sof(Jpeg *j, const uint8_t *p, size_t len, int progressive) {
    if (j->have_frame || len < 6) return IMG_E_CORRUPT;
    if (p[0] != 8) return IMG_E_UNSUPPORTED;
    j->h = (int)rd_be16(p + 1);
    j->w = (int)rd_be16(p + 3);
    j->ncomp = p[5];
    if (j->h == 0) return IMG_E_UNSUPPORTED;
    int rc = img_check_size(j->w, j->h);
    if (rc) return rc;
    if (j->ncomp < 1 || j->ncomp > MAXCOMP || len < 6 + (size_t)j->ncomp * 3) return IMG_E_CORRUPT;
    j->progressive = progressive;
    j->hmax = j->vmax = 1;
    for (int i = 0; i < j->ncomp; i++) {
        Comp *c = &j->c[i];
        c->id = p[6 + i * 3];
        c->h = p[7 + i * 3] >> 4;
        c->v = p[7 + i * 3] & 15;
        c->tq = p[8 + i * 3];
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3) return IMG_E_CORRUPT;
        if (j->ncomp == 1) c->h = c->v = 1;                          /* a lone component is never subsampled */
        if (c->h > j->hmax) j->hmax = c->h;
        if (c->v > j->vmax) j->vmax = c->v;
    }
    for (int i = 0; i < j->ncomp; i++)
        if (j->hmax % j->c[i].h || j->vmax % j->c[i].v) return IMG_E_UNSUPPORTED;
    j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
    j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
    j->have_frame = 1;
    return 0;
}

static int alloc_planes(Jpeg *j) {
    size_t coef_total = 0;
    for (int i = 0; i < j->ncomp; i++) {
        Comp *c = &j->c[i];
        c->bw = j->mcux * c->h;
        c->bh = j->mcuy * c->v;
        c->cw = (j->w * c->h + j->hmax - 1) / j->hmax;
        c->ch = (j->h * c->v + j->vmax - 1) / j->vmax;
        c->tbw = (c->cw + 7) / 8;
        c->tbh = (c->ch + 7) / 8;
        if (j->progressive) coef_total += (size_t)c->bw * (size_t)c->bh * 64 * sizeof(int16_t);
    }
    if (coef_total > MAX_COEF_BYTES) return IMG_E_TOOBIG;
    for (int i = 0; i < j->ncomp; i++) {
        Comp *c = &j->c[i];
        c->plane = calloc((size_t)c->bw * 8 * (size_t)c->bh * 8, 1);
        if (!c->plane) return IMG_E_NOMEM;
        if (j->progressive) {
            c->coef = calloc((size_t)c->bw * (size_t)c->bh * 64, sizeof(int16_t));
            if (!c->coef) return IMG_E_NOMEM;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- scans */

static int decode_baseline_block(Jpeg *j, Comp *c, int16_t *blk) {
    memset(blk, 0, 64 * sizeof(int16_t));
    int t = huff_decode(j, &j->dc[c->dc_tbl]);
    if (t < 0 || t > 15) return IMG_E_CORRUPT;
    int diff = t ? extend(get_bits(j, t), t) : 0;
    c->pred += diff;
    blk[0] = (int16_t)c->pred;
    for (int k = 1; k < 64;) {
        int rs = huff_decode(j, &j->ac[c->ac_tbl]);
        if (rs < 0) return IMG_E_CORRUPT;
        int r = rs >> 4, s = rs & 15;
        if (s == 0) {
            if (r == 15) { k += 16; continue; }
            break;
        }
        k += r;
        if (k > 63) return IMG_E_CORRUPT;
        blk[g_zig[k]] = (int16_t)extend(get_bits(j, s), s);
        k++;
    }
    return 0;
}

static int decode_dc_first(Jpeg *j, Comp *c, int16_t *blk, int al) {
    int t = huff_decode(j, &j->dc[c->dc_tbl]);
    if (t < 0 || t > 15) return IMG_E_CORRUPT;
    int diff = t ? extend(get_bits(j, t), t) : 0;
    c->pred += diff;
    blk[0] = (int16_t)(c->pred * (1 << al));
    return 0;
}

static int decode_ac_first(Jpeg *j, Comp *c, int16_t *blk, int ss, int se, int al) {
    if (j->eobrun > 0) { j->eobrun--; return 0; }
    for (int k = ss; k <= se; k++) {
        int rs = huff_decode(j, &j->ac[c->ac_tbl]);
        if (rs < 0) return IMG_E_CORRUPT;
        int r = rs >> 4, s = rs & 15;
        if (s) {
            k += r;
            if (k > 63) return IMG_E_CORRUPT;
            blk[g_zig[k]] = (int16_t)(extend(get_bits(j, s), s) * (1 << al));
        } else if (r == 15) k += 15;
        else {
            j->eobrun = 1 << r;
            if (r) j->eobrun += get_bits(j, r);
            j->eobrun--;
            break;
        }
    }
    return 0;
}

static int decode_ac_refine(Jpeg *j, Comp *c, int16_t *blk, int ss, int se, int al) {
    int p1 = 1 << al, m1 = -(1 << al);
    int k = ss;
    if (j->eobrun <= 0) {
        for (; k <= se; k++) {
            int rs = huff_decode(j, &j->ac[c->ac_tbl]);
            if (rs < 0) return IMG_E_CORRUPT;
            int r = rs >> 4, s = rs & 15, val = 0;
            if (s) {
                if (s != 1) return IMG_E_CORRUPT;
                val = get_bit(j) ? p1 : m1;
            } else if (r != 15) {
                j->eobrun = 1 << r;
                if (r) j->eobrun += get_bits(j, r);
                break;
            }
            do {                                                      /* skip over already-nonzero coefficients and r zero ones */
                int16_t *coef = &blk[g_zig[k]];
                if (*coef != 0) {
                    if (get_bit(j) && (*coef & p1) == 0) *coef = (int16_t)(*coef >= 0 ? *coef + p1 : *coef + m1);
                } else if (--r < 0) break;
                k++;
            } while (k <= se);
            if (val && k <= se) blk[g_zig[k]] = (int16_t)val;
        }
    }
    if (j->eobrun > 0) {
        for (; k <= se; k++) {
            int16_t *coef = &blk[g_zig[k]];
            if (*coef != 0 && get_bit(j) && (*coef & p1) == 0) *coef = (int16_t)(*coef >= 0 ? *coef + p1 : *coef + m1);
        }
        j->eobrun--;
    }
    return 0;
}

/* After a restart interval: realign, expect RSTn, reset the predictors. */
static int do_restart(Jpeg *j, int ncomps_in_scan, const int *idx) {
    j->bitbuf = 0;
    j->bitcnt = 0;
    j->fake_bits = 0;
    j->eobrun = 0;
    /* find the RSTn marker (skipping any stray bytes before it) */
    if (!j->marker) {
        while (j->pos + 1 < j->n && !(j->d[j->pos] == 0xFF && j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7)) {
            if (j->d[j->pos] == 0xFF && j->d[j->pos + 1] != 0 && j->d[j->pos + 1] != 0xFF) break;   /* some other marker: the scan is over */
            j->pos++;
        }
        if (j->pos + 1 < j->n && j->d[j->pos] == 0xFF) j->marker = j->d[j->pos + 1];
    }
    if (j->marker >= 0xD0 && j->marker <= 0xD7) {
        j->pos += 2;
        j->marker = 0;
    } else return j->pos >= j->n ? IMG_E_TRUNC : IMG_E_CORRUPT;
    for (int i = 0; i < ncomps_in_scan; i++) j->c[idx[i]].pred = 0;
    return 0;
}

static int decode_scan(Jpeg *j, const uint8_t *hdr, size_t len) {
    if (!j->have_frame || len < 1) return IMG_E_CORRUPT;
    int ns = hdr[0];
    if (ns < 1 || ns > j->ncomp || len < (size_t)1 + (size_t)ns * 2 + 3) return IMG_E_CORRUPT;
    int idx[MAXCOMP];
    for (int i = 0; i < ns; i++) {
        int id = hdr[1 + i * 2], found = -1;
        for (int k = 0; k < j->ncomp; k++) if (j->c[k].id == id) found = k;
        if (found < 0) return IMG_E_CORRUPT;
        for (int k = 0; k < i; k++) if (idx[k] == found) return IMG_E_CORRUPT;
        idx[i] = found;
        j->c[found].dc_tbl = hdr[2 + i * 2] >> 4;
        j->c[found].ac_tbl = hdr[2 + i * 2] & 15;
        if (j->c[found].dc_tbl > 3 || j->c[found].ac_tbl > 3) return IMG_E_CORRUPT;
    }
    int ss = hdr[1 + ns * 2], se = hdr[2 + ns * 2], ah = hdr[3 + ns * 2] >> 4, al = hdr[3 + ns * 2] & 15;
    if (!j->progressive) { ss = 0; se = 63; ah = al = 0; }
    else if (ss > se || se > 63 || (ss == 0 && se != 0) || (ss > 0 && ns != 1) || al > 13 || ah > 13) return IMG_E_CORRUPT;
    for (int i = 0; i < ns; i++) {
        Comp *c = &j->c[idx[i]];
        if (ss == 0 && !j->dc[c->dc_tbl].built && ah == 0) return IMG_E_CORRUPT;
        if (se > 0 && !j->ac[c->ac_tbl].built) return IMG_E_CORRUPT;
        if (!j->qt_ok[c->tq]) return IMG_E_CORRUPT;
        c->pred = 0;
    }
    j->bitbuf = 0;
    j->bitcnt = 0;
    j->fake_bits = 0;
    j->over = 0;
    j->marker = 0;
    j->eobrun = 0;

    int interleaved = ns > 1;
    int cols = interleaved ? j->mcux : j->c[idx[0]].tbw;
    int rows = interleaved ? j->mcuy : j->c[idx[0]].tbh;
    int todo = j->restart_interval;
    int16_t blk[64];
    for (int my = 0; my < rows; my++) {
        for (int mx = 0; mx < cols; mx++) {
            if (j->restart_interval && todo == 0) {
                int rc = do_restart(j, ns, idx);
                if (rc) return rc;
                todo = j->restart_interval;
            }
            for (int ci = 0; ci < ns; ci++) {
                Comp *c = &j->c[idx[ci]];
                int bh = interleaved ? c->v : 1, bwid = interleaved ? c->h : 1;
                for (int y = 0; y < bh; y++) {
                    for (int x = 0; x < bwid; x++) {
                        int bx = interleaved ? mx * c->h + x : mx, by = interleaved ? my * c->v + y : my;
                        int rc;
                        if (!j->progressive) {
                            rc = decode_baseline_block(j, c, blk);
                            if (!rc) idct_block(blk, j->qt[c->tq], c->plane + (size_t)by * 8 * (size_t)c->bw * 8 + (size_t)bx * 8, c->bw * 8);
                        } else {
                            int16_t *b = c->coef + ((size_t)by * c->bw + (size_t)bx) * 64;
                            if (ss == 0) {
                                if (ah == 0) rc = decode_dc_first(j, c, b, al);
                                else { if (get_bit(j)) b[0] = (int16_t)(b[0] | (1 << al)); rc = 0; }
                            } else if (ah == 0) rc = decode_ac_first(j, c, b, ss, se, al);
                            else rc = decode_ac_refine(j, c, b, ss, se, al);
                        }
                        if (rc) return rc;
                        if (j->over) return j->pos >= j->n ? IMG_E_TRUNC : IMG_E_CORRUPT;
                    }
                }
            }
            if (j->restart_interval) todo--;
        }
    }
    return 0;
}

/* Skips to the next marker after a scan; returns its code or -1. */
static int next_marker(Jpeg *j) {
    while (j->pos + 1 < j->n) {
        if (j->d[j->pos] == 0xFF && j->d[j->pos + 1] != 0 && j->d[j->pos + 1] != 0xFF && !(j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7)) {
            int m = j->d[j->pos + 1];
            j->pos += 2;
            return m;
        }
        j->pos++;
    }
    return -1;
}

static int parse(Jpeg *j, int headers_only) {
    if (j->n < 4 || j->d[0] != 0xFF || j->d[1] != 0xD8) return IMG_E_FORMAT;
    j->pos = 2;
    int scans = 0, rc = 0;
    for (;;) {
        int m = next_marker(j);
        if (m < 0) { rc = scans ? 0 : IMG_E_TRUNC; break; }
        if (m == 0xD9) break;                                        /* EOI */
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) continue;       /* markers without a length */
        if (j->pos + 2 > j->n) { rc = scans ? 0 : IMG_E_TRUNC; break; }
        size_t len = rd_be16(j->d + j->pos);
        if (len < 2 || j->pos + len > j->n) { rc = scans ? 0 : IMG_E_TRUNC; break; }
        const uint8_t *p = j->d + j->pos + 2;
        size_t pl = len - 2;
        j->pos += len;
        switch (m) {
        case 0xE1: read_exif(j, p, pl); break;
        case 0xE0: if (pl >= 5 && !memcmp(p, "JFIF", 5)) j->jfif = 1; break;
        case 0xEE: if (pl >= 12 && !memcmp(p, "Adobe", 5)) { j->adobe = 1; j->adobe_transform = p[11]; } break;
        case 0xDB: rc = parse_dqt(j, p, pl); break;
        case 0xC4: rc = parse_dht(j, p, pl); break;
        case 0xDD: if (pl >= 2) j->restart_interval = (int)rd_be16(p); break;
        case 0xC0: case 0xC1: rc = parse_sof(j, p, pl, 0); break;
        case 0xC2: rc = parse_sof(j, p, pl, 1); break;
        case 0xC3: case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
            rc = IMG_E_UNSUPPORTED;
            break;
        case 0xDA:
            if (headers_only) return 0;
            if (!j->have_frame || scans >= MAX_SCANS) { rc = IMG_E_CORRUPT; break; }
            if (!j->c[0].plane) { rc = alloc_planes(j); if (rc) break; }
            rc = decode_scan(j, p, pl);
            if (!rc) scans++;
            break;
        default: break;                                              /* APPn, COM, ... */
        }
        if (rc) break;
        if (headers_only && j->have_frame) return 0;
    }
    if (!rc && !scans) rc = IMG_E_CORRUPT;
    return rc;
}

/* ---------------------------------------------------------------- upsampling and colour */

/* Produces row `y` (0 <= y < image height) of component c at full resolution into out[0..w). */
static void component_row(const Jpeg *j, const Comp *c, int y, uint8_t *out) {
    int fx = j->hmax / c->h, fy = j->vmax / c->v;
    int stride = c->bw * 8;
    int w = j->w;
    if (fx == 1 && fy == 1) { memcpy(out, c->plane + (size_t)y * stride, (size_t)w); return; }
    if (fx == 2 && fy == 1 && c->cw > 2) {                            /* h2v1 fancy */
        const uint8_t *in = c->plane + (size_t)y * stride;
        int cw = c->cw;
        for (int x = 0; x < w; x++) {
            int i = x >> 1, v;
            if (x & 1) v = i + 1 < cw ? (in[i] * 3 + in[i + 1] + 2) >> 2 : in[i];
            else v = i > 0 ? (in[i] * 3 + in[i - 1] + 1) >> 2 : in[i];
            out[x] = (uint8_t)v;
        }
        return;
    }
    if (fx == 2 && fy == 2 && c->cw > 2) {                            /* h2v2 fancy */
        int iy = y >> 1, ny = (y & 1) ? iy + 1 : iy - 1;
        if (ny < 0) ny = 0;
        if (ny >= c->ch) ny = c->ch - 1;
        if (iy >= c->ch) iy = c->ch - 1;
        const uint8_t *r0 = c->plane + (size_t)iy * stride, *r1 = c->plane + (size_t)ny * stride;
        int cw = c->cw;
        for (int x = 0; x < w; x++) {
            int i = x >> 1;
            int cur = r0[i] * 3 + r1[i], v;
            if (x & 1) {
                int nxt = i + 1 < cw ? r0[i + 1] * 3 + r1[i + 1] : cur;
                v = i + 1 < cw ? (cur * 3 + nxt + 7) >> 4 : (cur * 4 + 7) >> 4;
            } else {
                int lst = i > 0 ? r0[i - 1] * 3 + r1[i - 1] : cur;
                v = i > 0 ? (cur * 3 + lst + 8) >> 4 : (cur * 4 + 8) >> 4;
            }
            out[x] = (uint8_t)v;
        }
        return;
    }
    if (fx == 1 && fy == 2) {                                         /* h1v2 fancy */
        int iy = y >> 1, ny = (y & 1) ? iy + 1 : iy - 1;
        if (ny < 0) ny = 0;
        if (ny >= c->ch) ny = c->ch - 1;
        if (iy >= c->ch) iy = c->ch - 1;
        const uint8_t *r0 = c->plane + (size_t)iy * stride, *r1 = c->plane + (size_t)ny * stride;
        int bias = (y & 1) ? 2 : 1;
        for (int x = 0; x < w; x++) out[x] = (uint8_t)((r0[x] * 3 + r1[x] + bias) >> 2);
        return;
    }
    {                                                                 /* anything else: replicate */
        int iy = y / fy;
        if (iy >= c->ch) iy = c->ch - 1;
        const uint8_t *in = c->plane + (size_t)iy * stride;
        for (int x = 0; x < w; x++) out[x] = in[x / fx];
    }
}

static int g_cr_r[256], g_cb_b[256], g_cr_g[256], g_cb_g[256];
static int g_ycc_ready;

static void ycc_tables(void) {
    for (int i = 0; i < 256; i++) {
        int x = i - 128;
        g_cr_r[i] = (91881 * x + 32768) >> 16;
        g_cb_b[i] = (116130 * x + 32768) >> 16;
        g_cr_g[i] = -46802 * x;
        g_cb_g[i] = -22554 * x + 32768;
    }
    g_ycc_ready = 1;
}

static int apply_orientation(Image *im, int o) {
    if (o <= 1 || o > 8) return 0;
    Image t;
    int rc;
    switch (o) {
    case 2: rc = img_flip(im, 1, &t); break;
    case 3: rc = img_rotate90(im, 2, &t); break;
    case 4: rc = img_flip(im, 0, &t); break;
    case 6: rc = img_rotate90(im, 1, &t); break;
    case 8: rc = img_rotate90(im, 3, &t); break;
    case 5: case 7: {                                                 /* transpose / transverse = rotate + mirror */
        Image r;
        rc = img_rotate90(im, o == 5 ? 1 : 3, &r);
        if (rc) return rc;
        rc = img_flip(&r, 1, &t);
        img_free(&r);
        break;
    }
    default: return 0;
    }
    if (rc) return rc;
    img_free(im);
    *im = t;
    return 0;
}

int jpeg_probe(const uint8_t *d, size_t n, ImgInfo *info) {
    Jpeg *j = calloc(1, sizeof *j);
    if (!j) return IMG_E_NOMEM;
    j->d = d;
    j->n = n;
    j->orientation = 1;
    int rc = parse(j, 1);
    if (!rc && !j->have_frame) rc = IMG_E_CORRUPT;
    if (!rc) {
        info->format = IMG_JPEG;
        info->width = j->orientation >= 5 ? j->h : j->w;
        info->height = j->orientation >= 5 ? j->w : j->h;
        info->has_alpha = 0;
        info->frames = 1;
    }
    free(j);
    return rc;
}

int jpeg_decode(const uint8_t *d, size_t n, Image *out) {
    Jpeg *j = calloc(1, sizeof *j);
    if (!j) return IMG_E_NOMEM;
    j->d = d;
    j->n = n;
    j->orientation = 1;
    if (!g_ycc_ready) ycc_tables();
    int rc = parse(j, 0);
    uint8_t *rows[MAXCOMP] = { 0 };
    if (!rc && !j->have_frame) rc = IMG_E_CORRUPT;
    if (!rc && j->progressive) {                                      /* all scans are in: transform the coefficients */
        for (int i = 0; i < j->ncomp; i++) {
            Comp *c = &j->c[i];
            if (!j->qt_ok[c->tq]) { rc = IMG_E_CORRUPT; break; }
            for (int by = 0; by < c->bh; by++)
                for (int bx = 0; bx < c->bw; bx++)
                    idct_block(c->coef + ((size_t)by * c->bw + (size_t)bx) * 64, j->qt[c->tq],
                               c->plane + (size_t)by * 8 * (size_t)c->bw * 8 + (size_t)bx * 8, c->bw * 8);
            free(c->coef);
            c->coef = NULL;
        }
    }
    if (!rc) rc = img_alloc(out, j->w, j->h);
    if (!rc) {
        for (int i = 0; i < j->ncomp; i++) { rows[i] = malloc((size_t)j->w); if (!rows[i]) rc = IMG_E_NOMEM; }
    }
    if (!rc) {
        int mode;                                                     /* 0 gray, 1 YCbCr, 2 RGB, 3 CMYK, 4 YCCK */
        if (j->ncomp == 1) mode = 0;
        else if (j->ncomp == 3) {
            if (j->jfif) mode = 1;
            else if (j->adobe) mode = j->adobe_transform == 0 ? 2 : 1;
            else mode = (j->c[0].id == 'R' && j->c[1].id == 'G' && j->c[2].id == 'B') ? 2 : 1;
        } else if (j->ncomp == 4) mode = (j->adobe && j->adobe_transform == 2) ? 4 : 3;
        else mode = 0;                                                /* two components: show the first */
        for (int y = 0; y < j->h; y++) {
            for (int i = 0; i < j->ncomp; i++) component_row(j, &j->c[i], y, rows[i]);
            uint32_t *dst = out->px + (size_t)y * j->w;
            for (int x = 0; x < j->w; x++) {
                int r, g, b;
                if (mode == 0) { r = g = b = rows[0][x]; }
                else if (mode == 2) { r = rows[0][x]; g = rows[1][x]; b = rows[2][x]; }
                else if (mode == 1 || mode == 4) {
                    int yy = rows[0][x], cb = rows[1][x], cr = rows[2][x];
                    r = clamp8(yy + g_cr_r[cr]);
                    g = clamp8(yy + ((g_cb_g[cb] + g_cr_g[cr]) >> 16));
                    b = clamp8(yy + g_cb_b[cb]);
                    if (mode == 4) {                                  /* YCCK: the RGB so far is inverted CMY, K is stored inverted */
                        int k = rows[3][x];
                        r = (255 - r) * k / 255; g = (255 - g) * k / 255; b = (255 - b) * k / 255;
                        r = clamp8(r); g = clamp8(g); b = clamp8(b);
                    }
                } else {
                    int cc = rows[0][x], mm = rows[1][x], yy = rows[2][x], kk = rows[3][x];
                    if (j->adobe) { r = cc * kk / 255; g = mm * kk / 255; b = yy * kk / 255; }      /* Adobe stores CMYK inverted */
                    else { r = (255 - cc) * (255 - kk) / 255; g = (255 - mm) * (255 - kk) / 255; b = (255 - yy) * (255 - kk) / 255; }
                }
                dst[x] = ARGB(255, r, g, b);
            }
        }
        out->has_alpha = 0;
        rc = apply_orientation(out, j->orientation);
    }
    for (int i = 0; i < MAXCOMP; i++) { free(rows[i]); free(j->c[i].plane); free(j->c[i].coef); }
    free(j);
    return rc;
}
