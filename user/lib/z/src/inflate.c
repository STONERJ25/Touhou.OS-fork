/*
 * Streaming inflate: raw DEFLATE, zlib and gzip.
 *
 * The decoder is a state machine, so input and output can arrive in pieces of any size. Output goes through a 64 KiB
 * window (32 KiB of history plus up to 32 KiB not yet handed to the caller); the caller's buffers are never read back.
 * Bits are held in a 64-bit accumulator. At every exit the whole bytes that are still in the accumulator are given
 * back to next_in, so trailing data after the end of a stream is exactly where the caller expects it.
 */
#include <z.h>
#include <stdlib.h>
#include <string.h>

#define WIN_SIZE  65536u
#define WIN_MASK  (WIN_SIZE - 1)
#define HISTORY   32768u
#define MAX_MATCH 258u
#define PEND_MAX  (WIN_SIZE - HISTORY - MAX_MATCH)      /* undelivered bytes allowed before the next symbol is decoded */
#define FASTBITS  10

typedef struct {
    uint16_t fast[1 << FASTBITS];       /* (symbol << 4) | length for codes up to FASTBITS long, 0 = use the slow path */
    uint16_t count[16];                 /* codes per length */
    uint16_t symbol[320];               /* symbols in canonical order */
} Huff;

enum {
    M_HEAD, M_GZ_FIXED, M_GZ_XLEN, M_GZ_EXTRA, M_GZ_NAME, M_GZ_COMMENT, M_GZ_HCRC, M_ZL_HEAD,
    M_BLOCK, M_STORED_LEN, M_STORED, M_DYN_COUNTS, M_DYN_CLEN, M_DYN_LENS, M_LEN, M_DIST, M_COPY, M_TRAILER, M_DONE, M_ERROR
};

typedef struct {
    int format, wrap, mode, err;
    uint64_t bits;
    int nbits;
    uint8_t *win;
    uint32_t wpos, rpos;
    uint64_t written;
    int last;
    uint32_t stored;
    int nlen, ndist, ncode, idx;
    uint8_t clens[19];
    uint8_t lens[320];
    Huff clen, lit, dist;
    uint32_t mlen, mdist;
    uint8_t hbuf[10];
    uint32_t gz_flags, gz_xlen, hcrc;
    uint32_t name_len;
    uint64_t out_total;
} Inflate;

static const uint16_t g_lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t  g_lext[29]  = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t g_dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t  g_dext[30]  = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const uint8_t  g_clorder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

static unsigned bitrev(unsigned code, int len) {
    unsigned r = 0;
    while (len--) { r = (r << 1) | (code & 1); code >>= 1; }
    return r;
}

/* Builds the decoding tables for n code lengths. Returns -1 for an over-subscribed set, and for an incomplete one unless
 * it is the single one-bit code (or no code at all) that DEFLATE allows for distances. strict: always complete (code length codes). */
static int huff_build(Huff *h, const uint8_t *lens, int n, int strict) {
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    int used = n - h->count[0];
    h->count[0] = 0;
    int left = 1;
    for (int len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }
    if (left > 0 && used > 0 && (strict || !(used == 1 && h->count[1] == 1))) return -1;
    uint16_t offs[17], start[17];
    offs[1] = 0;
    for (int len = 1; len < 16; len++) offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    memcpy(start, offs, sizeof start);
    for (int i = 0; i < n; i++) if (lens[i]) h->symbol[offs[lens[i]]++] = (uint16_t)i;
    memset(h->fast, 0, sizeof h->fast);
    unsigned code = 0;
    for (int len = 1; len <= FASTBITS; len++) {
        for (int k = 0; k < h->count[len]; k++, code++) {
            unsigned sym = h->symbol[start[len] + k];
            unsigned r = bitrev(code, len);
            for (unsigned j = r; j < (1u << FASTBITS); j += 1u << len) h->fast[j] = (uint16_t)((sym << 4) | (unsigned)len);
        }
        code <<= 1;
    }
    return 0;
}

/* Decodes one symbol from the low bits of `bits`. Returns the symbol (length in *used), -1 if more bits are needed, -2 if
 * the bits are not a valid code. */
static int huff_decode(const Huff *h, uint64_t bits, int nbits, int *used) {
    unsigned e = h->fast[bits & ((1u << FASTBITS) - 1)];
    if (e) {
        int l = (int)(e & 15);
        if (l > nbits) return -1;
        *used = l;
        return (int)(e >> 4);
    }
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        if (len > nbits) return -1;
        code |= (int)((bits >> (len - 1)) & 1);
        int count = h->count[len];
        if (code - count < first) {
            *used = len;
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -2;
}

static void fixed_tables(Inflate *st) {
    uint8_t lens[288];
    int i = 0;
    for (; i < 144; i++) lens[i] = 8;
    for (; i < 256; i++) lens[i] = 9;
    for (; i < 280; i++) lens[i] = 7;
    for (; i < 288; i++) lens[i] = 8;
    huff_build(&st->lit, lens, 288, 0);
    for (i = 0; i < 32; i++) lens[i] = 5;
    huff_build(&st->dist, lens, 32, 0);
}

static void reset_state(Inflate *st) {
    st->wrap = st->format;
    st->mode = st->wrap == Z_RAW ? M_BLOCK : M_HEAD;
    st->err = 0;
    st->bits = 0;
    st->nbits = 0;
    st->wpos = st->rpos = 0;
    st->written = 0;
    st->out_total = 0;
    st->last = 0;
    st->idx = 0;
    st->hcrc = 0;
    st->name_len = 0;
}

int z_inflate_init(ZStream *s, int format) {
    if (format < Z_RAW || format > Z_AUTO) return Z_ERR_ARG;
    Inflate *st = calloc(1, sizeof *st);
    if (!st) return Z_ERR_MEM;
    st->win = malloc(WIN_SIZE);
    if (!st->win) { free(st); return Z_ERR_MEM; }
    st->format = format;
    reset_state(st);
    s->state = st;
    s->total_in = s->total_out = 0;
    s->check = format == Z_ZLIB ? 1 : 0;
    memset(&s->gzip, 0, sizeof s->gzip);
    return Z_OK;
}

int z_inflate_reset(ZStream *s) {
    Inflate *st = s->state;
    if (!st) return Z_ERR_ARG;
    reset_state(st);
    s->check = 0;
    memset(&s->gzip, 0, sizeof s->gzip);
    return Z_OK;
}

void z_inflate_end(ZStream *s) {
    Inflate *st = s->state;
    if (!st) return;
    free(st->win);
    free(st);
    s->state = NULL;
}

/* Hands as much of the window as fits to the caller and updates the running checksum. */
static void flush_out(ZStream *s, Inflate *st) {
    while (st->rpos != st->wpos && s->avail_out) {
        uint32_t from = st->rpos & WIN_MASK;
        size_t n = st->wpos - st->rpos;
        if (n > WIN_SIZE - from) n = WIN_SIZE - from;
        if (n > s->avail_out) n = s->avail_out;
        memcpy(s->next_out, st->win + from, n);
        if (st->wrap == Z_GZIP) s->check = z_crc32(s->check, s->next_out, n);
        else if (st->wrap == Z_ZLIB) s->check = z_adler32(s->check, s->next_out, n);
        s->next_out += n;
        s->avail_out -= n;
        s->total_out += n;
        st->out_total += n;
        st->rpos += (uint32_t)n;
    }
}

int z_inflate(ZStream *s) {
    Inflate *st = s ? s->state : NULL;
    if (!st) return Z_ERR_ARG;
    if (st->mode == M_ERROR) return st->err;
    if (st->mode == M_DONE) return Z_STREAM_END;

    uint64_t bits = st->bits;
    int nbits = st->nbits;
    const uint8_t *in = s->next_in;
    size_t avail = s->avail_in;
    size_t in_start = avail, out_start = s->avail_out;
    int rc = Z_OK;

#define FILL() do { while (nbits <= 56 && avail) { bits |= (uint64_t)*in++ << nbits; nbits += 8; avail--; } } while (0)
#define NEED(n) do { if (nbits < (n)) { FILL(); if (nbits < (n)) goto out; } } while (0)
#define GETBITS(n) ((uint32_t)(bits & ((1ULL << (n)) - 1)))
#define DROP(n) do { bits >>= (n); nbits -= (n); } while (0)
#define FAIL(code) do { st->mode = M_ERROR; st->err = (code); rc = (code); goto out; } while (0)
#define PENDING() (st->wpos - st->rpos)
#define PUT(b) (st->win[st->wpos++ & WIN_MASK] = (uint8_t)(b))

    s->state = st;
    for (;;) {
        switch (st->mode) {
        case M_HEAD:
            if (st->wrap == Z_AUTO) {
                NEED(16);
                st->wrap = (bits & 0xFFFF) == 0x8B1F ? Z_GZIP : Z_ZLIB;
                s->check = st->wrap == Z_ZLIB ? 1 : 0;
            }
            if (st->wrap == Z_GZIP) { st->mode = M_GZ_FIXED; st->idx = 0; st->hcrc = 0; }
            else st->mode = M_ZL_HEAD;
            break;

        case M_ZL_HEAD: {
            NEED(16);
            unsigned cmf = (unsigned)(bits & 0xFF), flg = (unsigned)((bits >> 8) & 0xFF);
            if (((cmf << 8) | flg) % 31 || (cmf & 15) != 8 || (cmf >> 4) > 7 || (flg & 0x20)) FAIL(Z_ERR_FORMAT);
            DROP(16);
            st->mode = M_BLOCK;
            break;
        }

        case M_GZ_FIXED:
            while (st->idx < 10) {
                NEED(8);
                uint8_t b = (uint8_t)(bits & 0xFF);
                DROP(8);
                st->hbuf[st->idx++] = b;
                st->hcrc = z_crc32(st->hcrc, &b, 1);
            }
            if (st->hbuf[0] != 0x1F || st->hbuf[1] != 0x8B) FAIL(Z_ERR_FORMAT);
            if (st->hbuf[2] != 8 || (st->hbuf[3] & 0xE0)) FAIL(Z_ERR_FORMAT);
            st->gz_flags = st->hbuf[3];
            s->gzip.mtime = (uint32_t)st->hbuf[4] | (uint32_t)st->hbuf[5] << 8 | (uint32_t)st->hbuf[6] << 16 | (uint32_t)st->hbuf[7] << 24;
            s->gzip.xfl = st->hbuf[8];
            s->gzip.os = st->hbuf[9];
            s->gzip.name[0] = 0;
            st->idx = 0;
            st->gz_xlen = 0;
            st->name_len = 0;
            st->mode = M_GZ_XLEN;
            break;

        case M_GZ_XLEN:
            if (st->gz_flags & 4) {
                while (st->idx < 2) {
                    NEED(8);
                    uint8_t b = (uint8_t)(bits & 0xFF);
                    DROP(8);
                    st->hcrc = z_crc32(st->hcrc, &b, 1);
                    st->gz_xlen |= (uint32_t)b << (8 * st->idx);
                    st->idx++;
                }
            }
            st->mode = M_GZ_EXTRA;
            break;

        case M_GZ_EXTRA:
            while (st->gz_xlen) {
                NEED(8);
                uint8_t b = (uint8_t)(bits & 0xFF);
                DROP(8);
                st->hcrc = z_crc32(st->hcrc, &b, 1);
                st->gz_xlen--;
            }
            st->mode = M_GZ_NAME;
            break;

        case M_GZ_NAME:
        case M_GZ_COMMENT: {
            int is_name = st->mode == M_GZ_NAME;
            if (st->gz_flags & (is_name ? 8u : 16u)) {
                for (;;) {
                    NEED(8);
                    uint8_t b = (uint8_t)(bits & 0xFF);
                    DROP(8);
                    st->hcrc = z_crc32(st->hcrc, &b, 1);
                    if (b == 0) break;
                    if (is_name && st->name_len < sizeof s->gzip.name - 1) { s->gzip.name[st->name_len++] = (char)b; s->gzip.name[st->name_len] = 0; }
                }
            }
            st->mode = is_name ? M_GZ_COMMENT : M_GZ_HCRC;
            st->idx = 0;
            break;
        }

        case M_GZ_HCRC:
            if (st->gz_flags & 2) {
                NEED(16);
                if ((uint32_t)(bits & 0xFFFF) != (st->hcrc & 0xFFFF)) FAIL(Z_ERR_CHECK);
                DROP(16);
            }
            st->mode = M_BLOCK;
            break;

        case M_BLOCK: {
            NEED(3);
            st->last = (int)GETBITS(1);
            unsigned type = (unsigned)((bits >> 1) & 3);
            DROP(3);
            if (type == 0) st->mode = M_STORED_LEN;
            else if (type == 1) { fixed_tables(st); st->mode = M_LEN; }
            else if (type == 2) st->mode = M_DYN_COUNTS;
            else FAIL(Z_ERR_DATA);
            break;
        }

        case M_STORED_LEN: {
            int drop = nbits & 7;
            DROP(drop);
            NEED(32);
            uint32_t len = GETBITS(16), nlen = (uint32_t)((bits >> 16) & 0xFFFF);
            if (len != (~nlen & 0xFFFF)) FAIL(Z_ERR_DATA);
            DROP(32);
            st->stored = len;
            st->mode = M_STORED;
            break;
        }

        case M_STORED:
            while (st->stored) {
                uint32_t room = (WIN_SIZE - HISTORY) - PENDING();
                if (room == 0) { flush_out(s, st); room = (WIN_SIZE - HISTORY) - PENDING(); if (room == 0) goto out; }
                size_t n = st->stored < room ? st->stored : room;
                if (nbits >= 8) {                                   /* whole bytes still in the accumulator come first */
                    PUT(bits & 0xFF);
                    DROP(8);
                    st->written++;
                    st->stored--;
                    continue;
                }
                if (!avail) goto out;
                if (n > avail) n = avail;
                for (size_t i = 0; i < n; i++) PUT(in[i]);
                in += n;
                avail -= n;
                st->written += n;
                st->stored -= (uint32_t)n;
            }
            st->mode = st->last ? M_TRAILER : M_BLOCK;
            break;

        case M_DYN_COUNTS:
            NEED(14);
            st->nlen = (int)GETBITS(5) + 257;
            st->ndist = (int)((bits >> 5) & 31) + 1;
            st->ncode = (int)((bits >> 10) & 15) + 4;
            DROP(14);
            if (st->nlen > 286 || st->ndist > 30) FAIL(Z_ERR_DATA);
            memset(st->clens, 0, sizeof st->clens);
            st->idx = 0;
            st->mode = M_DYN_CLEN;
            break;

        case M_DYN_CLEN:
            while (st->idx < st->ncode) {
                NEED(3);
                st->clens[g_clorder[st->idx++]] = (uint8_t)GETBITS(3);
                DROP(3);
            }
            if (huff_build(&st->clen, st->clens, 19, 1) < 0) FAIL(Z_ERR_DATA);
            st->idx = 0;
            st->mode = M_DYN_LENS;
            break;

        case M_DYN_LENS: {
            int total = st->nlen + st->ndist;
            while (st->idx < total) {
                FILL();
                int used = 0;
                int sym = huff_decode(&st->clen, bits, nbits, &used);
                if (sym == -1) goto out;
                if (sym < 0) FAIL(Z_ERR_DATA);
                if (sym < 16) {
                    DROP(used);
                    st->lens[st->idx++] = (uint8_t)sym;
                    continue;
                }
                int extra = sym == 16 ? 2 : sym == 17 ? 3 : 7;
                if (nbits < used + extra) goto out;
                uint32_t rep;
                uint8_t val = 0;
                if (sym == 16) {
                    if (st->idx == 0) FAIL(Z_ERR_DATA);
                    val = st->lens[st->idx - 1];
                    rep = 3 + (uint32_t)((bits >> used) & 3);
                } else if (sym == 17) rep = 3 + (uint32_t)((bits >> used) & 7);
                else rep = 11 + (uint32_t)((bits >> used) & 127);
                if (st->idx + (int)rep > total) FAIL(Z_ERR_DATA);
                DROP(used + extra);
                while (rep--) st->lens[st->idx++] = val;
            }
            if (st->lens[256] == 0) FAIL(Z_ERR_DATA);
            if (huff_build(&st->lit, st->lens, st->nlen, 0) < 0) FAIL(Z_ERR_DATA);
            if (huff_build(&st->dist, st->lens + st->nlen, st->ndist, 0) < 0) FAIL(Z_ERR_DATA);
            st->mode = M_LEN;
            break;
        }

        case M_LEN:
            for (;;) {
                if (PENDING() > PEND_MAX) { flush_out(s, st); if (PENDING() > PEND_MAX) goto out; }
                FILL();
                int used = 0;
                int sym = huff_decode(&st->lit, bits, nbits, &used);
                if (sym == -1) goto out;
                if (sym < 0) FAIL(Z_ERR_DATA);
                if (sym < 256) {
                    DROP(used);
                    PUT(sym);
                    st->written++;
                    continue;
                }
                if (sym == 256) {
                    DROP(used);
                    st->mode = st->last ? M_TRAILER : M_BLOCK;
                    break;
                }
                sym -= 257;
                if (sym >= 29) FAIL(Z_ERR_DATA);
                int eb = g_lext[sym];
                if (nbits < used + eb) goto out;
                st->mlen = g_lbase[sym] + (eb ? (uint32_t)((bits >> used) & ((1u << eb) - 1)) : 0);
                DROP(used + eb);
                st->mode = M_DIST;
                break;
            }
            break;

        case M_DIST: {
            FILL();
            int used = 0;
            int sym = huff_decode(&st->dist, bits, nbits, &used);
            if (sym == -1) goto out;
            if (sym < 0 || sym >= 30) FAIL(Z_ERR_DATA);
            int eb = g_dext[sym];
            if (nbits < used + eb) goto out;
            st->mdist = g_dbase[sym] + (eb ? (uint32_t)((bits >> used) & ((1u << eb) - 1)) : 0);
            DROP(used + eb);
            if (st->mdist > st->written) FAIL(Z_ERR_DATA);          /* reaches back before the start of the data */
            st->mode = M_COPY;
            break;
        }

        case M_COPY:
            if (PENDING() > PEND_MAX) { flush_out(s, st); if (PENDING() > PEND_MAX) goto out; }
            for (uint32_t i = 0; i < st->mlen; i++) {
                uint8_t b = st->win[(st->wpos - st->mdist) & WIN_MASK];
                PUT(b);
            }
            st->written += st->mlen;
            st->mode = M_LEN;
            break;

        case M_TRAILER:
            flush_out(s, st);
            if (PENDING()) goto out;                                /* the caller must take the rest of the output first */
            DROP(nbits & 7);
            if (st->wrap == Z_GZIP) {
                NEED(64);
                uint32_t crc = (uint32_t)(bits & 0xFFFFFFFFu), isize = (uint32_t)(bits >> 32);
                if (crc != s->check || isize != (uint32_t)st->out_total) FAIL(Z_ERR_CHECK);
                bits = 0;
                nbits = 0;
            } else if (st->wrap == Z_ZLIB) {
                NEED(32);
                uint32_t a = (uint32_t)(bits & 0xFFFFFFFFu);
                a = (a >> 24) | ((a >> 8) & 0xFF00u) | ((a << 8) & 0xFF0000u) | (a << 24);
                if (a != s->check) FAIL(Z_ERR_CHECK);
                DROP(32);
            }
            st->mode = M_DONE;
            rc = Z_STREAM_END;
            goto out;

        case M_DONE:
            rc = Z_STREAM_END;
            goto out;

        default:
            FAIL(Z_ERR_DATA);
        }
    }

out:
    flush_out(s, st);
    /* give back the whole bytes that were read ahead */
    {
        int k = nbits >> 3;
        nbits -= 8 * k;
        if (nbits) bits &= (1ULL << nbits) - 1; else bits = 0;
        in -= k;
        avail += (size_t)k;
    }
    st->bits = bits;
    st->nbits = nbits;
    s->total_in += in_start - avail;
    s->next_in = in;
    s->avail_in = avail;
    if (rc == Z_OK && avail == in_start && s->avail_out == out_start) rc = Z_BUF_ERROR;
    return rc;
}

#undef FILL
#undef NEED
#undef GETBITS
#undef DROP
#undef FAIL
#undef PENDING
#undef PUT

int z_uncompress(const void *src, size_t n, int format, size_t max_out, uint8_t **out, size_t *out_len) {
    if (max_out == 0) max_out = (size_t)256 << 20;
    ZStream s;
    int rc = z_inflate_init(&s, format);
    if (rc) return rc;
    size_t cap = n * 3 + 4096;
    if (cap > max_out) cap = max_out;
    uint8_t *buf = malloc(cap ? cap : 1);
    if (!buf) { z_inflate_end(&s); return Z_ERR_MEM; }
    size_t len = 0;
    s.next_in = src;
    s.avail_in = n;
    for (;;) {
        s.next_out = buf + len;
        s.avail_out = cap - len;
        rc = z_inflate(&s);
        len = cap - s.avail_out;
        if (rc == Z_STREAM_END) {
            if (format == Z_GZIP || format == Z_AUTO) {             /* more gzip members may follow */
                if (s.avail_in >= 2 && s.next_in[0] == 0x1F && s.next_in[1] == 0x8B) { z_inflate_reset(&s); continue; }
            }
            break;
        }
        if (rc < 0 && rc != Z_BUF_ERROR) break;
        if (s.avail_out == 0) {
            if (cap >= max_out) { rc = Z_ERR_LIMIT; break; }
            size_t ncap = cap * 2 < max_out ? cap * 2 : max_out;
            if (ncap < 4096) ncap = 4096;
            uint8_t *nb = realloc(buf, ncap);
            if (!nb) { rc = Z_ERR_MEM; break; }
            buf = nb;
            cap = ncap;
            continue;
        }
        if (s.avail_in == 0) { rc = Z_ERR_TRUNC; break; }           /* input used up, stream not finished */
        if (rc == Z_BUF_ERROR) { rc = Z_ERR_DATA; break; }
    }
    z_inflate_end(&s);
    if (rc != Z_STREAM_END) { free(buf); return rc; }
    *out = buf;
    *out_len = len;
    return Z_OK;
}
