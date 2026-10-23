/*
 * Streaming deflate: LZ77 with hash chains and lazy matching, dynamic / fixed / stored blocks (whichever is smallest),
 * levels 0..9, raw / zlib / gzip framing.
 *
 * Input is gathered into blocks of up to 32 KiB that sit behind up to 32 KiB of history in one 64 KiB buffer, so every
 * position fits in 16 bits. A block is tokenised, its Huffman trees are built (length limited to 15 bits with the
 * Kraft-sum repair), its three possible encodings are costed exactly, and the cheapest is written.
 */
#include <z.h>
#include <stdlib.h>
#include <string.h>

#define MIN_MATCH   3u
#define MAX_MATCH   258u
#define MAX_DIST    32768u
#define BLOCK_BYTES 32768u
#define HIST_BYTES  32768u
#define BUF_SIZE    (HIST_BYTES + BLOCK_BYTES)
#define HASH_BITS   15
#define HASH_SIZE   (1u << HASH_BITS)
#define OBUF_SIZE   49152u

typedef struct {
    int level, format;
    uint32_t good_len, max_lazy, nice_len, max_chain;
    int lazy;
    uint8_t *buf;
    uint32_t hist, fill;
    uint16_t *head, *prev;
    uint16_t *tdist;
    uint8_t *tlen;
    uint32_t ntok;
    uint32_t lfreq[288], dfreq[32];
    uint8_t *obuf;
    uint32_t olen, opos;
    uint64_t bb;
    int bc;
    int stage;                                  /* 0 header not written, 1 body, 2 finished */
    uint8_t fl_len[288], fd_len[32];            /* fixed block code lengths and (bit-reversed) codes */
    uint16_t fl_code[288], fd_code[32];
} Deflate;

static const uint16_t g_lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t  g_lext[29]  = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint8_t  g_dext[30]  = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const uint8_t  g_clorder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

static const struct { uint16_t good, lazy, nice, chain; int lazy_mode; } g_cfg[10] = {
    { 0, 0, 0, 0, 0 },
    { 4, 4, 8, 4, 0 }, { 4, 5, 16, 8, 0 }, { 4, 6, 32, 32, 0 },
    { 4, 4, 16, 16, 1 }, { 8, 16, 32, 32, 1 }, { 8, 16, 128, 128, 1 }, { 8, 32, 128, 256, 1 }, { 32, 128, 258, 1024, 1 }, { 32, 258, 258, 4096, 1 },
};

static uint8_t g_len_code[256];                 /* length - 3 -> length code 0..28 */
static int g_tables_ready;

static void init_tables(void) {
    for (int c = 0; c < 28; c++)
        for (int k = 0; k < (1 << g_lext[c]); k++) g_len_code[g_lbase[c] - 3 + k] = (uint8_t)c;
    g_len_code[255] = 28;                       /* 258 has its own code */
    g_tables_ready = 1;
}

static inline unsigned dist_code(unsigned dist) {
    unsigned v = dist - 1;
    if (v < 4) return v;
    unsigned msb = 31u - (unsigned)__builtin_clz(v);
    return 2 * msb + ((v >> (msb - 1)) & 1);
}

static inline uint32_t dist_base(unsigned code) {
    if (code < 4) return code + 1;
    unsigned msb = code / 2;
    return (1u << msb) + ((code & 1u) << (msb - 1)) + 1;
}

/* ---------------------------------------------------------------- Huffman code construction */

static unsigned bitrev(unsigned code, int len) {
    unsigned r = 0;
    while (len--) { r = (r << 1) | (code & 1); code >>= 1; }
    return r;
}

/* Canonical codes for the given lengths, bit-reversed so they can be written LSB first. */
static void assign_codes(const uint8_t *lens, int n, uint16_t *codes) {
    unsigned count[16] = { 0 }, next[16];
    for (int i = 0; i < n; i++) count[lens[i]]++;
    count[0] = 0;
    unsigned code = 0;
    for (int b = 1; b < 16; b++) { code = (code + count[b - 1]) << 1; next[b] = code; }
    for (int i = 0; i < n; i++) if (lens[i]) codes[i] = (uint16_t)bitrev(next[lens[i]]++, lens[i]);
}

/* Optimal code lengths (Huffman) for freq[0..n), none longer than maxbits. Needs at least two used symbols. */
static void build_lengths(const uint32_t *freq, int n, int maxbits, uint8_t *lens) {
    uint16_t sym[288];
    uint32_t weight[2 * 288];
    int parent[2 * 288], depth[2 * 288];
    int m = 0;
    for (int i = 0; i < n; i++) { lens[i] = 0; if (freq[i]) sym[m++] = (uint16_t)i; }
    if (m == 0) return;
    if (m == 1) { lens[sym[0]] = 1; return; }
    for (int i = 1; i < m; i++) {                               /* insertion sort by frequency, ties by symbol */
        uint16_t s = sym[i];
        int j = i - 1;
        while (j >= 0 && freq[sym[j]] > freq[s]) { sym[j + 1] = sym[j]; j--; }
        sym[j + 1] = s;
    }
    for (int i = 0; i < m; i++) weight[i] = freq[sym[i]];
    int li = 0, ni = m, nn = m;
    for (int k = 0; k < m - 1; k++) {
        int pick[2];
        for (int t = 0; t < 2; t++) {
            if (li < m && (ni >= nn || weight[li] <= weight[ni])) pick[t] = li++;
            else pick[t] = ni++;
        }
        weight[nn] = weight[pick[0]] + weight[pick[1]];
        parent[pick[0]] = parent[pick[1]] = nn;
        nn++;
    }
    depth[nn - 1] = 0;
    for (int i = nn - 2; i >= 0; i--) depth[i] = depth[parent[i]] + 1;
    unsigned cnt[32] = { 0 };
    for (int i = 0; i < m; i++) { int d = depth[i]; if (d > maxbits) d = maxbits; cnt[d]++; }
    uint32_t total = 0;
    for (int i = 1; i <= maxbits; i++) total += cnt[i] << (maxbits - i);
    while (total > (1u << maxbits)) {                           /* clamping made the code over-subscribed: lengthen some codes */
        cnt[maxbits]--;
        for (int i = maxbits - 1; i > 0; i--) if (cnt[i]) { cnt[i]--; cnt[i + 1] += 2; break; }
        total--;
    }
    int idx = 0;                                                /* the rarest symbols get the longest codes */
    for (int len = maxbits; len >= 1; len--)
        for (unsigned k = 0; k < cnt[len]; k++) lens[sym[idx++]] = (uint8_t)len;
}

/* ---------------------------------------------------------------- bit output */

static inline void put_bits(Deflate *d, uint32_t v, int n) {
    d->bb |= (uint64_t)v << d->bc;
    d->bc += n;
    while (d->bc >= 8) { d->obuf[d->olen++] = (uint8_t)d->bb; d->bb >>= 8; d->bc -= 8; }
}

static void align_byte(Deflate *d) { if (d->bc) put_bits(d, 0, 8 - d->bc); }

static void write_stored(Deflate *d, const uint8_t *data, uint32_t n, int final) {
    put_bits(d, final ? 1 : 0, 1);
    put_bits(d, 0, 2);
    align_byte(d);
    d->obuf[d->olen++] = (uint8_t)n;
    d->obuf[d->olen++] = (uint8_t)(n >> 8);
    d->obuf[d->olen++] = (uint8_t)~n;
    d->obuf[d->olen++] = (uint8_t)(~n >> 8);
    memcpy(d->obuf + d->olen, data, n);
    d->olen += n;
}

/* ---------------------------------------------------------------- tokenising */

static inline uint32_t hash3(const uint8_t *p) {
    uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
    return (v * 2654435761u) >> (32 - HASH_BITS);
}

static inline uint32_t insert_pos(Deflate *d, uint32_t pos) {
    uint32_t h = hash3(d->buf + pos);
    uint32_t head = d->head[h];
    d->prev[pos] = (uint16_t)head;
    d->head[h] = (uint16_t)pos;
    return head;
}

static uint32_t longest_match(Deflate *d, uint32_t pos, uint32_t cur, uint32_t best, uint32_t avail, uint32_t *mstart) {
    uint32_t maxlen = avail < MAX_MATCH ? avail : MAX_MATCH;
    if (best >= maxlen) return best;
    uint32_t chain = d->max_chain;
    if (best >= d->good_len) chain >>= 2;
    uint32_t nice = d->nice_len < maxlen ? d->nice_len : maxlen;
    uint32_t limit = pos > MAX_DIST ? pos - MAX_DIST : 0;
    const uint8_t *scan = d->buf + pos;
    do {
        const uint8_t *m = d->buf + cur;
        if (m[best] == scan[best] && m[0] == scan[0] && m[1] == scan[1]) {
            uint32_t len = 2;
            while (len < maxlen && m[len] == scan[len]) len++;
            if (len > best) {
                best = len;
                *mstart = cur;
                if (len >= nice || len >= maxlen) break;
            }
        }
    } while ((cur = d->prev[cur]) > limit && --chain);
    return best;
}

static inline void emit_lit(Deflate *d, uint8_t c) {
    d->tdist[d->ntok] = 0;
    d->tlen[d->ntok] = c;
    d->ntok++;
    d->lfreq[c]++;
}

static inline void emit_match(Deflate *d, uint32_t len, uint32_t dist) {
    d->tdist[d->ntok] = (uint16_t)dist;
    d->tlen[d->ntok] = (uint8_t)(len - 3);
    d->ntok++;
    d->lfreq[257 + g_len_code[len - 3]]++;
    d->dfreq[dist_code(dist)]++;
}

static void tokenize(Deflate *d, uint32_t start, uint32_t end) {
    const uint8_t *buf = d->buf;
    uint32_t pos = start;
    if (!d->lazy) {
        while (pos < end) {
            uint32_t avail = end - pos, len = 0, mstart = 0;
            if (avail >= MIN_MATCH) {
                uint32_t head = insert_pos(d, pos);
                if (head && pos - head <= MAX_DIST) {
                    len = longest_match(d, pos, head, MIN_MATCH - 1, avail, &mstart);
                    if (len == MIN_MATCH && pos - mstart > 4096) len = 0;
                }
            }
            if (len >= MIN_MATCH) {
                emit_match(d, len, pos - mstart);
                if (len <= d->max_lazy)                         /* the "max insert length" of the fast levels */
                    for (uint32_t p = pos + 1; p < pos + len; p++) if (end - p >= MIN_MATCH) insert_pos(d, p);
                pos += len;
            } else {
                emit_lit(d, buf[pos]);
                pos++;
            }
        }
        return;
    }
    uint32_t match_len = MIN_MATCH - 1, match_start = 0;
    int have_prev_lit = 0;
    while (pos < end) {
        uint32_t avail = end - pos, head = 0;
        if (avail >= MIN_MATCH) head = insert_pos(d, pos);
        uint32_t prev_len = match_len, prev_match = match_start;
        match_len = MIN_MATCH - 1;
        if (head && prev_len < d->max_lazy && pos - head <= MAX_DIST) {
            match_len = longest_match(d, pos, head, prev_len, avail, &match_start);
            if (match_len == MIN_MATCH && pos - match_start > 4096) match_len = MIN_MATCH - 1;
        }
        if (prev_len >= MIN_MATCH && match_len <= prev_len) {
            emit_match(d, prev_len, pos - 1 - prev_match);
            uint32_t stop = pos - 1 + prev_len;
            for (uint32_t p = pos + 1; p < stop; p++) if (end - p >= MIN_MATCH) insert_pos(d, p);
            pos = stop;
            match_len = MIN_MATCH - 1;
            have_prev_lit = 0;
        } else if (have_prev_lit) {
            emit_lit(d, buf[pos - 1]);
            pos++;
        } else {
            have_prev_lit = 1;
            pos++;
        }
    }
    if (have_prev_lit) emit_lit(d, buf[pos - 1]);
}

/* ---------------------------------------------------------------- block output */

typedef struct { uint8_t sym, extra, bits; } RleItem;

static int rle_lengths(const uint8_t *seq, int total, RleItem *out) {
    int n = 0;
    for (int i = 0; i < total;) {
        uint8_t v = seq[i];
        int run = 1;
        while (i + run < total && seq[i + run] == v) run++;
        i += run;
        if (v == 0) {
            int r = run;
            while (r > 0) {
                if (r >= 11) { int k = r > 138 ? 138 : r; out[n++] = (RleItem){ 18, (uint8_t)(k - 11), 7 }; r -= k; }
                else if (r >= 3) { out[n++] = (RleItem){ 17, (uint8_t)(r - 3), 3 }; r = 0; }
                else { out[n++] = (RleItem){ 0, 0, 0 }; r--; }
            }
        } else {
            out[n++] = (RleItem){ v, 0, 0 };
            int r = run - 1;
            while (r >= 3) { int k = r > 6 ? 6 : r; out[n++] = (RleItem){ 16, (uint8_t)(k - 3), 2 }; r -= k; }
            while (r-- > 0) out[n++] = (RleItem){ v, 0, 0 };
        }
    }
    return n;
}

static void write_tokens(Deflate *d, const uint16_t *lcode, const uint8_t *llen, const uint16_t *dcode, const uint8_t *dlen) {
    for (uint32_t i = 0; i < d->ntok; i++) {
        unsigned dist = d->tdist[i];
        if (!dist) {
            put_bits(d, lcode[d->tlen[i]], llen[d->tlen[i]]);
        } else {
            unsigned lc = g_len_code[d->tlen[i]];
            put_bits(d, lcode[257 + lc], llen[257 + lc]);
            if (g_lext[lc]) put_bits(d, (unsigned)(d->tlen[i] + 3 - g_lbase[lc]), g_lext[lc]);
            unsigned dc = dist_code(dist);
            put_bits(d, dcode[dc], dlen[dc]);
            if (g_dext[dc]) put_bits(d, dist - dist_base(dc), g_dext[dc]);
        }
    }
    put_bits(d, lcode[256], llen[256]);
}

/* Compresses buf[start, end) as one block. */
static void write_block(Deflate *d, uint32_t start, uint32_t end, int final) {
    uint32_t n = end - start;
    if (d->level == 0) { write_stored(d, d->buf + start, n, final); return; }
    d->lfreq[256] = 1;
    uint64_t extra_bits = 0;
    for (int c = 0; c < 29; c++) extra_bits += (uint64_t)d->lfreq[257 + c] * g_lext[c];
    for (int c = 0; c < 30; c++) extra_bits += (uint64_t)d->dfreq[c] * g_dext[c];

    uint64_t fixed_bits = 3 + extra_bits;
    for (int i = 0; i < 288; i++) fixed_bits += (uint64_t)d->lfreq[i] * d->fl_len[i];
    for (int i = 0; i < 30; i++) fixed_bits += (uint64_t)d->dfreq[i] * 5;

    /* dynamic trees: force two used symbols in each tree so that both codes are complete */
    uint32_t lf[288], df[32];
    memcpy(lf, d->lfreq, sizeof lf);
    memcpy(df, d->dfreq, sizeof df);
    int dist_used = 0;
    for (int i = 0; i < 30; i++) if (df[i]) dist_used++;
    for (int i = 0; i < 2 && dist_used < 2; i++) if (!df[i]) { df[i] = 1; dist_used++; }
    int lit_used = 0;
    for (int i = 0; i < 286; i++) if (lf[i]) lit_used++;
    for (int i = 0; i < 2 && lit_used < 2; i++) if (!lf[i]) { lf[i] = 1; lit_used++; }
    uint8_t ll[288], dl[32];
    memset(ll, 0, sizeof ll);
    memset(dl, 0, sizeof dl);
    build_lengths(lf, 286, 15, ll);
    build_lengths(df, 30, 15, dl);
    int nlit = 286;
    while (nlit > 257 && !ll[nlit - 1]) nlit--;
    int ndist = 30;
    while (ndist > 1 && !dl[ndist - 1]) ndist--;
    uint8_t seq[320];
    memcpy(seq, ll, (size_t)nlit);
    memcpy(seq + nlit, dl, (size_t)ndist);
    RleItem rle[320];
    int nrle = rle_lengths(seq, nlit + ndist, rle);
    uint32_t cf[19] = { 0 };
    for (int i = 0; i < nrle; i++) cf[rle[i].sym]++;
    uint8_t cl[19];
    int cused = 0;
    for (int i = 0; i < 19; i++) if (cf[i]) cused++;
    for (int i = 0; i < 2 && cused < 2; i++) if (!cf[i]) { cf[i] = 1; cused++; }
    build_lengths(cf, 19, 7, cl);
    int ncl = 19;
    while (ncl > 4 && !cl[g_clorder[ncl - 1]]) ncl--;

    uint64_t dyn_bits = 3 + 5 + 5 + 4 + 3 * (uint64_t)ncl + extra_bits;
    for (int i = 0; i < nrle; i++) dyn_bits += cl[rle[i].sym] + rle[i].bits;
    for (int i = 0; i < 286; i++) dyn_bits += (uint64_t)d->lfreq[i] * ll[i];
    for (int i = 0; i < 30; i++) dyn_bits += (uint64_t)d->dfreq[i] * dl[i];

    uint64_t stored_bits = 3 + ((8 - ((d->bc + 3) & 7)) & 7) + 32 + 8 * (uint64_t)n;

    if (stored_bits <= fixed_bits && stored_bits <= dyn_bits) {
        write_stored(d, d->buf + start, n, final);
    } else if (fixed_bits <= dyn_bits) {
        put_bits(d, final ? 1 : 0, 1);
        put_bits(d, 1, 2);
        write_tokens(d, d->fl_code, d->fl_len, d->fd_code, d->fd_len);
    } else {
        uint16_t lcode[288], dcode[32], ccode[19];
        assign_codes(ll, 286, lcode);
        assign_codes(dl, 30, dcode);
        assign_codes(cl, 19, ccode);
        put_bits(d, final ? 1 : 0, 1);
        put_bits(d, 2, 2);
        put_bits(d, (uint32_t)(nlit - 257), 5);
        put_bits(d, (uint32_t)(ndist - 1), 5);
        put_bits(d, (uint32_t)(ncl - 4), 4);
        for (int i = 0; i < ncl; i++) put_bits(d, cl[g_clorder[i]], 3);
        for (int i = 0; i < nrle; i++) {
            put_bits(d, ccode[rle[i].sym], cl[rle[i].sym]);
            if (rle[i].bits) put_bits(d, rle[i].extra, rle[i].bits);
        }
        write_tokens(d, lcode, ll, dcode, dl);
    }
}

static void compress_block(Deflate *d, int final) {
    uint32_t start = d->hist, end = d->fill;
    d->ntok = 0;
    memset(d->lfreq, 0, sizeof d->lfreq);
    memset(d->dfreq, 0, sizeof d->dfreq);
    if (d->level > 0 && end > start) tokenize(d, start, end);
    write_block(d, start, end, final);
}

static void slide(Deflate *d) {
    uint32_t keep = d->fill < HIST_BYTES ? d->fill : HIST_BYTES;
    uint32_t shift = d->fill - keep;
    if (shift) {
        memmove(d->buf, d->buf + shift, keep);
        for (uint32_t i = 0; i < HASH_SIZE; i++) { uint32_t e = d->head[i]; d->head[i] = (uint16_t)(e > shift ? e - shift : 0); }
        for (uint32_t i = 0; i < keep; i++) { uint32_t e = d->prev[i + shift]; d->prev[i] = (uint16_t)(e > shift ? e - shift : 0); }
    }
    d->hist = keep;
    d->fill = keep;
}

/* ---------------------------------------------------------------- the stream interface */

int z_deflate_init(ZStream *s, int level, int format) {
    if (level < 0) level = Z_DEFAULT_LEVEL;
    if (level > 9 || format < Z_RAW || format > Z_GZIP) return Z_ERR_ARG;
    if (!g_tables_ready) init_tables();
    Deflate *d = calloc(1, sizeof *d);
    if (!d) return Z_ERR_MEM;
    d->buf = malloc(BUF_SIZE + MAX_MATCH + 8);
    d->head = calloc(HASH_SIZE, sizeof(uint16_t));
    d->prev = calloc(BUF_SIZE, sizeof(uint16_t));
    d->tdist = malloc((BLOCK_BYTES + 1) * sizeof(uint16_t));
    d->tlen = malloc(BLOCK_BYTES + 1);
    d->obuf = malloc(OBUF_SIZE);
    if (!d->buf || !d->head || !d->prev || !d->tdist || !d->tlen || !d->obuf) {
        free(d->buf); free(d->head); free(d->prev); free(d->tdist); free(d->tlen); free(d->obuf); free(d);
        return Z_ERR_MEM;
    }
    memset(d->buf, 0, BUF_SIZE + MAX_MATCH + 8);
    d->level = level;
    d->format = format;
    d->good_len = g_cfg[level].good;
    d->max_lazy = g_cfg[level].lazy;
    d->nice_len = g_cfg[level].nice;
    d->max_chain = g_cfg[level].chain;
    d->lazy = g_cfg[level].lazy_mode;
    int i = 0;
    for (; i < 144; i++) d->fl_len[i] = 8;
    for (; i < 256; i++) d->fl_len[i] = 9;
    for (; i < 280; i++) d->fl_len[i] = 7;
    for (; i < 288; i++) d->fl_len[i] = 8;
    for (i = 0; i < 32; i++) d->fd_len[i] = 5;
    assign_codes(d->fl_len, 288, d->fl_code);
    assign_codes(d->fd_len, 32, d->fd_code);
    s->state = d;
    s->total_in = s->total_out = 0;
    s->check = format == Z_ZLIB ? 1 : 0;
    if (format != Z_GZIP) memset(&s->gzip, 0, sizeof s->gzip);
    return Z_OK;
}

void z_deflate_end(ZStream *s) {
    Deflate *d = s->state;
    if (!d) return;
    free(d->buf); free(d->head); free(d->prev); free(d->tdist); free(d->tlen); free(d->obuf);
    free(d);
    s->state = NULL;
}

size_t z_deflate_bound(size_t n) { return n + (n >> 11) + 600; }

static void write_header(ZStream *s, Deflate *d) {
    if (d->format == Z_ZLIB) {
        unsigned flevel = d->level < 2 ? 0 : d->level < 6 ? 1 : d->level == 6 ? 2 : 3;
        unsigned h = (0x78u << 8) | (flevel << 6);
        h += 31 - (h % 31);
        d->obuf[d->olen++] = (uint8_t)(h >> 8);
        d->obuf[d->olen++] = (uint8_t)h;
    } else if (d->format == Z_GZIP) {
        int has_name = s->gzip.name[0] != 0;
        d->obuf[d->olen++] = 0x1F;
        d->obuf[d->olen++] = 0x8B;
        d->obuf[d->olen++] = 8;
        d->obuf[d->olen++] = has_name ? 8 : 0;
        for (int i = 0; i < 4; i++) d->obuf[d->olen++] = (uint8_t)(s->gzip.mtime >> (8 * i));
        d->obuf[d->olen++] = d->level == 9 ? 2 : d->level == 1 ? 4 : 0;
        d->obuf[d->olen++] = s->gzip.os ? s->gzip.os : 3;
        if (has_name) {
            size_t nl = strnlen(s->gzip.name, sizeof s->gzip.name - 1);
            memcpy(d->obuf + d->olen, s->gzip.name, nl);
            d->olen += (uint32_t)nl;
            d->obuf[d->olen++] = 0;
        }
    }
}

static void write_trailer(ZStream *s, Deflate *d) {
    align_byte(d);
    if (d->format == Z_ZLIB) {
        uint32_t a = s->check;
        for (int i = 3; i >= 0; i--) d->obuf[d->olen++] = (uint8_t)(a >> (8 * i));
    } else if (d->format == Z_GZIP) {
        for (int i = 0; i < 4; i++) d->obuf[d->olen++] = (uint8_t)(s->check >> (8 * i));
        for (int i = 0; i < 4; i++) d->obuf[d->olen++] = (uint8_t)(s->total_in >> (8 * i));
    }
}

int z_deflate(ZStream *s, int flush) {
    Deflate *d = s ? s->state : NULL;
    if (!d) return Z_ERR_ARG;
    if (flush != Z_NO_FLUSH && flush != Z_SYNC_FLUSH && flush != Z_FINISH) return Z_ERR_ARG;
    int rc;
    for (;;) {
        uint32_t n = d->olen - d->opos;
        if (n > s->avail_out) n = (uint32_t)s->avail_out;
        if (n) {
            memcpy(s->next_out, d->obuf + d->opos, n);
            s->next_out += n;
            s->avail_out -= n;
            s->total_out += n;
            d->opos += n;
        }
        if (d->opos < d->olen) { rc = Z_OK; break; }                    /* the caller must make room */
        d->opos = d->olen = 0;
        if (d->stage == 2) { rc = Z_STREAM_END; break; }
        if (d->stage == 0) { write_header(s, d); d->stage = 1; continue; }
        uint32_t room = BLOCK_BYTES - (d->fill - d->hist);
        if (s->avail_in && room) {
            size_t take = s->avail_in < room ? s->avail_in : room;
            memcpy(d->buf + d->fill, s->next_in, take);
            if (d->format == Z_GZIP) s->check = z_crc32(s->check, s->next_in, take);
            else if (d->format == Z_ZLIB) s->check = z_adler32(s->check, s->next_in, take);
            d->fill += (uint32_t)take;
            s->next_in += take;
            s->avail_in -= take;
            s->total_in += take;
            continue;
        }
        int full = d->fill - d->hist == BLOCK_BYTES;
        int input_done = s->avail_in == 0;
        if (full || (input_done && flush != Z_NO_FLUSH)) {
            int final = flush == Z_FINISH && input_done;
            int had_data = d->fill > d->hist;
            if (!had_data && !final) { rc = Z_OK; break; }                /* nothing to flush */
            compress_block(d, final);
            if (flush == Z_SYNC_FLUSH && !final && input_done) {
                put_bits(d, 0, 3);
                align_byte(d);
                d->obuf[d->olen++] = 0; d->obuf[d->olen++] = 0; d->obuf[d->olen++] = 0xFF; d->obuf[d->olen++] = 0xFF;
            }
            slide(d);
            if (final) { write_trailer(s, d); d->stage = 2; }
            continue;
        }
        rc = Z_OK;                                                      /* waiting for more input */
        break;
    }
    return rc;
}

int z_compress(const void *src, size_t n, int level, int format, uint8_t **out, size_t *out_len) {
    ZStream s;
    memset(&s, 0, sizeof s);
    int rc = z_deflate_init(&s, level, format);
    if (rc) return rc;
    size_t cap = z_deflate_bound(n);
    uint8_t *buf = malloc(cap);
    if (!buf) { z_deflate_end(&s); return Z_ERR_MEM; }
    s.next_in = src;
    s.avail_in = n;
    s.next_out = buf;
    s.avail_out = cap;
    rc = z_deflate(&s, Z_FINISH);
    if (rc != Z_STREAM_END) { free(buf); z_deflate_end(&s); return rc < 0 ? rc : Z_ERR_DATA; }
    *out = buf;
    *out_len = (size_t)s.total_out;
    z_deflate_end(&s);
    return Z_OK;
}
