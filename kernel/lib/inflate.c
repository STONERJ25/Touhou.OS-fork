/*
 * gunzip: RFC 1952 (the gzip member: header, trailer) around RFC 1951 (DEFLATE), from memory to a callback. See kernel/include/inflate.h for what it
 * promises and refuses. Written from the two RFCs; the userland's libz (user/lib/z) is a separate, streaming implementation for programs that feed
 * and drain in small pieces and may allocate; this one is one-shot over memory that is already there, allocates nothing, uses no floating point or
 * vector registers (the kernel is built -mgeneral-regs-only) and includes nothing of the kernel, so tests/host/test_inflate.py compiles this very
 * file for the host with the sanitizers.
 *
 * How it works, and why it is shaped like this:
 *
 *  - The output goes into a window in the caller's work area. DEFLATE only ever looks back 32 KiB, so when the window is full everything in it
 *    is handed to the sink, the last 32 KiB move to its start, and decoding goes on. The window is therefore linear (a match is a plain copy
 *    from `op - distance`, never a wrap-around) and its size is the caller's choice. Nothing the size of the whole output is ever held.
 *  - Bits are read through a 64-bit accumulator, filled eight bytes at a time while at least eight input bytes remain. The word that is read
 *    holds some bytes beyond those counted: they are the real next bytes of the stream, and the next refill puts the very same bits in the same
 *    place, so they are harmless; at the end of the input the accumulator holds zeros beyond what is counted, and every code is checked against
 *    the bits that are really there (n > bc below), so a stream that is cut in the middle of a code is reported as cut, not decoded from zeros.
 *  - Huffman codes are decoded with a 10-bit lookup table; the rare longer codes (up to 15 bits) fall back to the canonical decode of RFC 1951
 *    section 3.2.2 (compare the code read so far with the first code of each length). The tables are built from the code lengths by the
 *    construction of the same section. Sets of lengths that over-subscribe, or leave a gap (except the one case DEFLATE allows, a single code of
 *    length 1, and the code-length code which must be complete), are refused, as zlib refuses them.
 *  - The CRC-32 is computed over what is handed to the sink, four bytes at a time.
 */
#include "inflate.h"

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "inflate.c reads its input in little-endian words"
#endif

#define FAST_BITS 10u
#define FAST_MASK ((1u << FAST_BITS) - 1u)
#define MAX_MATCH 258u
#define MAXRUN    (MAX_MATCH + 8u)      /* room that must be free before a symbol is decoded: a longest match, plus what the 8-byte copy may overshoot */

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

typedef struct {
    uint16_t fast[1u << FAST_BITS];     /* indexed by the next FAST_BITS input bits: (symbol << 4) | code length, or 0 when the code is longer (or not assigned) */
    uint16_t count[16];                 /* how many codes have each length (count[0] is not used) */
    uint16_t symbol[288];               /* the symbols, in canonical order: by code length, then by value */
} Huff;

typedef struct {
    const uint8_t *src, *ip, *ie;       /* the input, the next byte to read, the end */
    uint64_t bb;                        /* bit accumulator, next bit in bit 0 */
    unsigned bc;                        /* how many bits of it are counted (whole bytes already taken from ip) */
    uint8_t *win;                       /* the window */
    size_t cap, op, flushed;            /* its size, the next byte to write, how much of it has been delivered already */
    uint64_t out_total, limit;          /* delivered so far (never more than limit), the ceiling */
    uint32_t crc;                       /* CRC-32 of what has been delivered */
    GzSink sink;
    void *ctx;
    const char *why;
    Huff lit, dist;
    uint8_t lens[320];                  /* code lengths of a dynamic block: up to 286 + 30 */
    uint32_t crct[4][256];              /* CRC-32 tables for four bytes at a time */
} State;

_Static_assert(sizeof(State) <= GZ_STATE_BYTES, "GZ_STATE_BYTES in inflate.h is too small for the decoder's state");

/* RFC 1951 section 3.2.5: base values and extra bits of the length codes 257..285 and of the distance codes 0..29. */
static const uint16_t g_lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t  g_lext[29]  = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t g_dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t  g_dext[30]  = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
/* section 3.2.7: the order in which the lengths of the code-length code are stored */
static const uint8_t  g_clorder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

const char *gunzip_strerror(int rc) {
    switch (rc) {
    case GZ_OK:         return "ok";
    case GZ_E_FORMAT:   return "not a gzip stream (bad magic, method or flags)";
    case GZ_E_HCRC:     return "the gzip header's own checksum does not match";
    case GZ_E_DATA:     return "invalid compressed data";
    case GZ_E_TRUNC:    return "the stream ends too early";
    case GZ_E_CRC:      return "CRC-32 mismatch (the data is damaged)";
    case GZ_E_SIZE:     return "length mismatch (the data is damaged)";
    case GZ_E_TRAILING: return "bytes after the end of the stream";
    case GZ_E_LIMIT:    return "more output than allowed";
    case GZ_E_SINK:     return "the consumer of the output failed";
    case GZ_E_ARG:      return "bad arguments (work area too small or misaligned)";
    default:            return "unknown error";
    }
}

/* ------------------------------------------------------------------------------------------------ CRC-32 (RFC 1952 section 8) */

static void crc_init(uint32_t t[4][256]) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = t[0][i];
        for (int j = 1; j < 4; j++) {
            c = t[0][c & 0xFF] ^ (c >> 8);
            t[j][i] = c;
        }
    }
}

static uint32_t crc_update(const uint32_t t[4][256], uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n >= 4) {
        uint32_t w;
        __builtin_memcpy(&w, p, 4);
        crc ^= w;
        crc = t[3][crc & 0xFF] ^ t[2][(crc >> 8) & 0xFF] ^ t[1][(crc >> 16) & 0xFF] ^ t[0][crc >> 24];
        p += 4;
        n -= 4;
    }
    while (n--) crc = t[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ------------------------------------------------------------------------------------------------ Huffman codes */

static unsigned bitrev(unsigned v, unsigned n) {
    unsigned r = 0;
    while (n--) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}

/* Builds the decoding tables for n code lengths (each 0..15). Returns -1 for a set that over-subscribes the code space, and for an incomplete one unless it is
 * empty or (not `strict`) the single code of length 1 that DEFLATE allows for the distances. strict: the code-length code, which must be complete. */
static int huff_build(Huff *h, const uint8_t *lens, unsigned n, int strict) {
    unsigned len;
    for (len = 0; len < 16; len++) h->count[len] = 0;
    for (unsigned i = 0; i < n; i++) h->count[lens[i]]++;
    unsigned used = n - h->count[0];
    h->count[0] = 0;
    int left = 1;                                       /* codes of the next length that are still unassigned, in the units of that length */
    for (len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }
    if (left > 0 && used > 0 && (strict || !(used == 1 && h->count[1] == 1))) return -1;

    uint16_t start[16], offs[16];                       /* where the symbols of each length begin in symbol[] */
    start[1] = offs[1] = 0;
    for (len = 1; len < 15; len++) start[len + 1] = offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (unsigned i = 0; i < n; i++)
        if (lens[i]) h->symbol[offs[lens[i]]++] = (uint16_t)i;

    for (unsigned i = 0; i < (1u << FAST_BITS); i++) h->fast[i] = 0;
    unsigned code = 0;                                  /* RFC 1951 3.2.2: the first code of each length follows the last code of the one before, shifted */
    for (len = 1; len <= FAST_BITS; len++) {
        for (unsigned k = 0; k < h->count[len]; k++, code++) {
            unsigned sym = h->symbol[start[len] + k];
            for (unsigned j = bitrev(code, len); j < (1u << FAST_BITS); j += 1u << len) h->fast[j] = (uint16_t)((sym << 4) | len);
        }
        code <<= 1;
    }
    return 0;
}

/* The canonical decode, for codes that the lookup table does not hold (longer than FAST_BITS, or no code at all). bb: the next input bits. Returns the symbol
 * and its code length in *len, or -1 when the bits are no code. */
static int decode_slow(const Huff *h, uint64_t bb, unsigned *len) {
    int code = 0, first = 0, index = 0;                 /* the code read so far; the first code of this length; where its symbols begin */
    for (unsigned l = 1; l <= 15; l++) {
        code |= (int)(bb & 1);
        bb >>= 1;
        int count = h->count[l];
        if (code - count < first) { *len = l; return h->symbol[index + (code - first)]; }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static void build_fixed(State *s) {                     /* RFC 1951 3.2.6 */
    uint8_t l[288];
    unsigned i = 0;
    for (; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    huff_build(&s->lit, l, 288, 0);
    for (i = 0; i < 32; i++) l[i] = 5;                  /* 32 codes of 5 bits: the last two are never valid, the symbol checks refuse them */
    huff_build(&s->dist, l, 32, 0);
}

/* ------------------------------------------------------------------------------------------------ input bits */

/* Fills the accumulator to at least 56 bits, or with everything that is left of the input. Works on the caller's copies of ip, bb and bc. */
#define REFILL() do { \
    if (LIKELY(ie - ip >= 8)) { \
        uint64_t w_; \
        __builtin_memcpy(&w_, ip, 8); \
        bb |= w_ << bc; \
        ip += (63u - bc) >> 3; \
        bc |= 56u; \
    } else { \
        while (bc < 56 && ip < ie) { bb |= (uint64_t)*ip++ << bc; bc += 8; } \
    } \
} while (0)

static void fill(State *s) {
    const uint8_t *ip = s->ip, *const ie = s->ie;
    uint64_t bb = s->bb;
    unsigned bc = s->bc;
    REFILL();
    s->ip = ip;
    s->bb = bb;
    s->bc = bc;
}

static int need(State *s, unsigned n) {                 /* n <= 32 bits available? (reads more input if it can) */
    if (s->bc < n) {
        fill(s);
        if (s->bc < n) return GZ_E_TRUNC;
    }
    return GZ_OK;
}

static unsigned take(State *s, unsigned n) {            /* n <= 32, after need() */
    unsigned v = (unsigned)(s->bb & ((1ull << n) - 1));
    s->bb >>= n;
    s->bc -= n;
    return v;
}

static int fail(State *s, int rc, const char *why) {
    s->why = why;
    return rc;
}

/* ------------------------------------------------------------------------------------------------ output window */

/* Delivers what has been decoded and not yet delivered; unless this is the last call, keeps the last GZ_HISTORY bytes at the start of the window. */
static int flush(State *s, int final) {
    size_t n = s->op - s->flushed;
    if (n) {
        if (n > s->limit - s->out_total) return fail(s, GZ_E_LIMIT, "the output is longer than the limit");
        if (s->sink && s->sink(s->ctx, s->win + s->flushed, n)) return GZ_E_SINK;
        s->crc = crc_update(s->crct, s->crc, s->win + s->flushed, n);       /* (the sink may not change the window, and what it refused was not delivered) */
        s->out_total += n;
        s->flushed = s->op;
    }
    if (!final && s->op > GZ_HISTORY) {
        __builtin_memmove(s->win, s->win + s->op - GZ_HISTORY, GZ_HISTORY);
        s->op = s->flushed = GZ_HISTORY;
    }
    return GZ_OK;
}

/* ------------------------------------------------------------------------------------------------ DEFLATE blocks */

/* The symbols of one Huffman block, up to its end-of-block code. */
static int codes(State *s) {
    const Huff *const lit = &s->lit, *const dist = &s->dist;
    const uint8_t *ip = s->ip, *const ie = s->ie;
    uint64_t bb = s->bb;
    unsigned bc = s->bc;
    uint8_t *const win = s->win;
    const size_t cap = s->cap;
    size_t op = s->op;
    int rc = GZ_OK;

    for (;;) {
        if (UNLIKELY(op + MAXRUN > cap)) {
            s->op = op;
            rc = flush(s, 0);
            if (rc) goto out;
            op = s->op;
        }
        REFILL();                                       /* at least 56 bits now, unless the input is used up: a length symbol with everything it needs is at most 48 */

        unsigned e = lit->fast[bb & FAST_MASK], sym, n;
        if (LIKELY(e)) { n = e & 15; sym = e >> 4; }
        else {
            int r = decode_slow(lit, bb, &n);
            if (r < 0) { rc = fail(s, GZ_E_DATA, "invalid literal/length code"); goto out; }
            sym = (unsigned)r;
        }
        if (UNLIKELY(n > bc)) { rc = GZ_E_TRUNC; goto out; }
        bb >>= n;
        bc -= n;
        if (sym < 256) { win[op++] = (uint8_t)sym; continue; }
        if (sym == 256) break;                          /* end of block */

        sym -= 257;
        if (UNLIKELY(sym >= 29)) { rc = fail(s, GZ_E_DATA, "invalid literal/length code (286 or 287)"); goto out; }
        unsigned eb = g_lext[sym];
        if (UNLIKELY(eb > bc)) { rc = GZ_E_TRUNC; goto out; }
        unsigned length = g_lbase[sym] + (unsigned)(bb & ((1u << eb) - 1));
        bb >>= eb;
        bc -= eb;

        e = dist->fast[bb & FAST_MASK];
        if (LIKELY(e)) { n = e & 15; sym = e >> 4; }
        else {
            int r = decode_slow(dist, bb, &n);
            if (r < 0) { rc = fail(s, GZ_E_DATA, "invalid distance code"); goto out; }
            sym = (unsigned)r;
        }
        if (UNLIKELY(n > bc)) { rc = GZ_E_TRUNC; goto out; }
        bb >>= n;
        bc -= n;
        if (UNLIKELY(sym >= 30)) { rc = fail(s, GZ_E_DATA, "invalid distance code (30 or 31)"); goto out; }
        eb = g_dext[sym];
        if (UNLIKELY(eb > bc)) { rc = GZ_E_TRUNC; goto out; }
        unsigned distance = g_dbase[sym] + (unsigned)(bb & ((1u << eb) - 1));
        bb >>= eb;
        bc -= eb;
        if (UNLIKELY(distance > op)) { rc = fail(s, GZ_E_DATA, "a distance that reaches back before the start of the output"); goto out; }

        uint8_t *d = win + op;
        const uint8_t *m = d - distance;
        op += length;
        if (distance >= 8) {                            /* eight bytes at a time; the copy never reads what it has not written yet, and may write up to 7 bytes beyond the end */
            const uint8_t *const end = win + op;
            do { __builtin_memcpy(d, m, 8); d += 8; m += 8; } while (d < end);
        } else if (distance == 1) {
            __builtin_memset(d, m[0], length);
        } else {                                        /* overlapping, byte by byte: the copy feeds itself */
            for (unsigned i = 0; i < length; i++) d[i] = m[i];
        }
    }
out:
    s->ip = ip;
    s->bb = bb;
    s->bc = bc;
    s->op = op;
    return rc;
}

/* A stored block (section 3.2.4): skip to the byte boundary, LEN and its complement, then LEN raw bytes. */
static int stored_block(State *s) {
    unsigned drop = s->bc & 7;
    s->bb >>= drop;
    s->bc -= drop;
    s->ip -= s->bc >> 3;                                /* the whole bytes read ahead go back to the input */
    s->bb = 0;
    s->bc = 0;
    if (s->ie - s->ip < 4) return GZ_E_TRUNC;
    unsigned len = s->ip[0] | (unsigned)s->ip[1] << 8, nlen = s->ip[2] | (unsigned)s->ip[3] << 8;
    s->ip += 4;
    if ((len ^ nlen) != 0xFFFFu) return fail(s, GZ_E_DATA, "a stored block whose length check fails");
    while (len) {
        if (s->cap == s->op) {                          /* (only possible with a window that is exactly full: flush() slides it) */
            int rc = flush(s, 0);
            if (rc) return rc;
        }
        size_t n = len, room = s->cap - s->op, avail = (size_t)(s->ie - s->ip);
        if (n > room) n = room;
        if (n > avail) n = avail;
        if (n == 0) return GZ_E_TRUNC;
        __builtin_memcpy(s->win + s->op, s->ip, n);
        s->op += n;
        s->ip += n;
        len -= (unsigned)n;
    }
    return GZ_OK;
}

/* The header of a dynamic block (section 3.2.7): how many codes, the code-length code, the code lengths of both alphabets, then the two tables. */
static int dynamic_tables(State *s) {
    int rc = need(s, 14);
    if (rc) return rc;
    unsigned hlit = take(s, 5) + 257, hdist = take(s, 5) + 1, hclen = take(s, 4) + 4;
    if (hlit > 286 || hdist > 30) return fail(s, GZ_E_DATA, "too many length or distance symbols");
    uint8_t cl[19];
    for (unsigned i = 0; i < 19; i++) cl[i] = 0;
    for (unsigned i = 0; i < hclen; i++) {
        rc = need(s, 3);
        if (rc) return rc;
        cl[g_clorder[i]] = (uint8_t)take(s, 3);
    }
    if (huff_build(&s->dist, cl, 19, 1) < 0) return fail(s, GZ_E_DATA, "invalid code lengths set");     /* (s->dist is scratch until the real tables are built) */

    const unsigned total = hlit + hdist;
    unsigned i = 0;
    while (i < total) {
        fill(s);
        unsigned e = s->dist.fast[s->bb & FAST_MASK];       /* the code-length codes are at most 7 bits: the lookup table holds every one of them */
        if (!e) return fail(s, GZ_E_DATA, "invalid code lengths set");
        unsigned n = e & 15, sym = e >> 4;
        if (n > s->bc) return GZ_E_TRUNC;
        s->bb >>= n;
        s->bc -= n;
        if (sym < 16) { s->lens[i++] = (uint8_t)sym; continue; }
        unsigned eb = sym == 16 ? 2 : sym == 17 ? 3 : 7, rep = sym == 18 ? 11 : 3;
        uint8_t val = 0;
        if (sym == 16) {
            if (i == 0) return fail(s, GZ_E_DATA, "invalid bit length repeat");
            val = s->lens[i - 1];
        }
        if (s->bc < eb) return GZ_E_TRUNC;
        rep += take(s, eb);
        if (i + rep > total) return fail(s, GZ_E_DATA, "invalid bit length repeat");
        while (rep--) s->lens[i++] = val;
    }
    if (s->lens[256] == 0) return fail(s, GZ_E_DATA, "invalid code -- missing end-of-block");
    if (huff_build(&s->lit, s->lens, hlit, 0) < 0) return fail(s, GZ_E_DATA, "invalid literal/lengths set");
    if (huff_build(&s->dist, s->lens + hlit, hdist, 0) < 0) return fail(s, GZ_E_DATA, "invalid distances set");
    return GZ_OK;
}

static int deflate_stream(State *s) {
    unsigned last;
    do {
        int rc = need(s, 3);
        if (rc) return rc;
        last = take(s, 1);
        unsigned type = take(s, 2);
        if (type == 0) rc = stored_block(s);
        else if (type == 1) { build_fixed(s); rc = codes(s); }
        else if (type == 2) { rc = dynamic_tables(s); if (!rc) rc = codes(s); }
        else rc = fail(s, GZ_E_DATA, "invalid block type");
        if (rc) return rc;
    } while (!last);
    return GZ_OK;
}

/* ------------------------------------------------------------------------------------------------ the gzip member */

#define FHCRC     2
#define FEXTRA    4
#define FNAME     8
#define FCOMMENT 16

/* RFC 1952 section 2.3: ID1 ID2 CM FLG MTIME(4) XFL OS, then the optional parts in the order extra field, name, comment, header CRC-16. */
static int read_header(State *s) {
    const uint8_t *p = s->ip, *const ie = s->ie, *const begin = p;
    size_t have = (size_t)(ie - p);
    if (have >= 1 && p[0] != 0x1F) return fail(s, GZ_E_FORMAT, "the magic number is not 1f 8b");
    if (have >= 2 && p[1] != 0x8B) return fail(s, GZ_E_FORMAT, "the magic number is not 1f 8b");
    if (have >= 3 && p[2] != 8) return fail(s, GZ_E_FORMAT, "the compression method is not deflate");
    if (have >= 4 && (p[3] & 0xE0)) return fail(s, GZ_E_FORMAT, "a reserved flag is set");
    if (have < 10) return fail(s, GZ_E_FORMAT, "the header is cut short");
    unsigned flg = p[3];
    p += 10;
    if (flg & FEXTRA) {
        if (ie - p < 2) return fail(s, GZ_E_FORMAT, "the header is cut short");
        size_t xlen = (size_t)p[0] | (size_t)p[1] << 8;
        p += 2;
        if ((size_t)(ie - p) < xlen) return fail(s, GZ_E_FORMAT, "the header is cut short");
        p += xlen;
    }
    for (unsigned f = FNAME; f <= FCOMMENT; f <<= 1) {  /* the file name, then the comment: zero-terminated */
        if (!(flg & f)) continue;
        while (p < ie && *p) p++;
        if (p == ie) return fail(s, GZ_E_FORMAT, "the header is cut short");
        p++;
    }
    if (flg & FHCRC) {
        if (ie - p < 2) return fail(s, GZ_E_FORMAT, "the header is cut short");
        uint32_t want = (uint32_t)p[0] | (uint32_t)p[1] << 8, got = crc_update(s->crct, 0, begin, (size_t)(p - begin)) & 0xFFFFu;
        if (want != got) return GZ_E_HCRC;
        p += 2;
    }
    s->ip = p;
    return GZ_OK;
}

/* After the last block: the CRC-32 and the length (mod 2^32) of the output, little-endian, and then the end of the input. */
static int read_trailer(State *s) {
    unsigned drop = s->bc & 7;                          /* the rest of the byte the last block ended in */
    s->bb >>= drop;
    s->bc -= drop;
    s->ip -= s->bc >> 3;
    s->bb = 0;
    s->bc = 0;
    if (s->ie - s->ip < 8) return GZ_E_TRUNC;
    uint32_t crc = (uint32_t)s->ip[0] | (uint32_t)s->ip[1] << 8 | (uint32_t)s->ip[2] << 16 | (uint32_t)s->ip[3] << 24;
    uint32_t isize = (uint32_t)s->ip[4] | (uint32_t)s->ip[5] << 8 | (uint32_t)s->ip[6] << 16 | (uint32_t)s->ip[7] << 24;
    s->ip += 8;
    if (crc != s->crc) return GZ_E_CRC;
    if (isize != (uint32_t)s->out_total) return GZ_E_SIZE;
    if (s->ip != s->ie) return GZ_E_TRAILING;
    return GZ_OK;
}

int gunzip(const uint8_t *src, size_t srclen, void *work, size_t work_size, uint64_t limit, GzSink sink, void *ctx, GzInfo *info) {
    int rc;
    State *s = work;
    if (!src || !work || ((uintptr_t)work & 7) || work_size < GZ_MIN_WORK) {
        if (info) { info->in_pos = 0; info->out_len = 0; info->crc = 0; info->why = NULL; }
        return GZ_E_ARG;
    }
    s->src = s->ip = src;
    s->ie = src + srclen;
    s->bb = 0;
    s->bc = 0;
    s->win = (uint8_t *)work + GZ_STATE_BYTES;
    s->cap = (work_size - GZ_STATE_BYTES) & ~(size_t)7;
    s->op = s->flushed = 0;
    s->out_total = 0;
    s->limit = limit;
    s->crc = 0;
    s->sink = sink;
    s->ctx = ctx;
    s->why = NULL;
    crc_init(s->crct);

    rc = read_header(s);
    if (!rc) rc = deflate_stream(s);
    if (!rc) rc = flush(s, 1);                          /* (a limit that the last piece breaks is found here) */
    if (!rc) rc = read_trailer(s);
    if (info) {
        info->in_pos = (uint64_t)(s->ip - s->src);
        info->out_len = s->out_total;
        info->crc = s->crc;
        info->why = s->why;
    }
    return rc;
}
