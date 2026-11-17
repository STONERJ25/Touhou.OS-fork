/*
 * flac.c: a FLAC decoder, written from RFC 9639 ("Free Lossless Audio Codec (FLAC)") and from the MD5 description of RFC 1321.
 * No code from libFLAC or any other decoder was used or looked at; the specification is the only source.
 *
 * Layout of this file: CRCs and MD5; a bit reader that works on a block of memory and never reads outside it; the subframe
 * decoders (constant, verbatim, fixed, LPC, with the Rice-coded residual); the frame header and the frame; the stream (the read
 * buffer, the metadata blocks, finding frames, recovering from damage); reading; seeking; verifying.
 *
 * How a frame is decoded: the whole frame has to be in the read buffer first. STREAMINFO says how big a frame can get, so the
 * buffer is topped up to that before every frame; if a stream lies about it (or does not say), a frame that runs off the end
 * of the buffer is tried again with more of the file in the buffer. Decoding a frame is then a pure function of a byte range,
 * with every read checked against the end of that range, which is what makes the bit reader safe on hostile input.
 *
 * Everything the stream says is checked before it is used: the frame header against STREAMINFO (channels, bit depth, rate), the
 * block size against 65535, the partition order against the block size, the predictor order against the block size, the
 * coefficient precision and shift, the escape width, the quotient of a Rice code against 32 bits, and every reconstructed sample
 * against its bit depth (which also keeps the 64-bit arithmetic of the predictors from ever overflowing).
 */
#include <flac.h>
#include <stdlib.h>
#include <string.h>

#define BUF_INIT      (128u * 1024)             /* the read buffer; grows when frames are bigger than it can hold */
#define BUF_MAX       (8u * 1024 * 1024)        /* the biggest frame a legal stream can have is about 2.2 MB */
#define BUF_PAD       16                        /* the bit reader loads 8 bytes at a time: never past the end of the allocation */
#define VC_MAX        (1u << 20)                /* of a VORBIS_COMMENT block only this much is read (the tags we want come first) */
#define SEEKTABLE_MAX (4u << 20)                /* a SEEKTABLE bigger than this is ignored (seeking then searches) */
#define MAX_CONCEAL   (1u << 21)                /* the longest stretch of silence put in for lost frames: about 43 s at 48 kHz */
#define MAX_BLOCK     65535u

/* ------------------------------------------------------------------------------------------------------------- CRCs */

static uint8_t  crc8_tab[256];
static uint16_t crc16_tab[256];
static bool     crc_ready;

static void crc_init(void) {
    if (crc_ready) return;
    for (unsigned i = 0; i < 256; i++) {
        uint8_t c8 = (uint8_t)i;
        uint16_t c16 = (uint16_t)(i << 8);
        for (int k = 0; k < 8; k++) {
            c8 = (c8 & 0x80) ? (uint8_t)((c8 << 1) ^ 0x07) : (uint8_t)(c8 << 1);               /* x^8 + x^2 + x + 1 */
            c16 = (c16 & 0x8000) ? (uint16_t)((c16 << 1) ^ 0x8005) : (uint16_t)(c16 << 1);     /* x^16 + x^15 + x^2 + 1 */
        }
        crc8_tab[i] = c8;
        crc16_tab[i] = c16;
    }
    crc_ready = true;
}

static uint8_t crc8(const uint8_t *p, size_t n) {
    uint8_t c = 0;
    while (n--) c = crc8_tab[c ^ *p++];
    return c;
}

static uint16_t crc16(const uint8_t *p, size_t n) {
    uint16_t c = 0;
    while (n--) c = (uint16_t)((c << 8) ^ crc16_tab[(c >> 8) ^ *p++]);
    return c;
}

/* ------------------------------------------------------------------------------------------------------------- MD5 */

static const uint32_t md5_k[64] = {                 /* floor(2^32 * |sin(i + 1)|) */
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const uint8_t md5_s[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,  5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void md5_block(FlacMd5 *m, const uint8_t *p) {
    uint32_t w[16];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 | (uint32_t)p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
    uint32_t a = m->a, b = m->b, c = m->c, d = m->d;
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16)      { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) & 15; }
        else             { f = c ^ (b | ~d);       g = (7 * i) & 15; }
        uint32_t t = d;
        d = c;
        c = b;
        uint32_t x = a + f + md5_k[i] + w[g];
        b = b + ((x << md5_s[i]) | (x >> (32 - md5_s[i])));
        a = t;
    }
    m->a += a; m->b += b; m->c += c; m->d += d;
}

void flac_md5_init(FlacMd5 *m) {
    m->a = 0x67452301u; m->b = 0xefcdab89u; m->c = 0x98badcfeu; m->d = 0x10325476u;
    m->bytes = 0;
}

void flac_md5_update(FlacMd5 *m, const void *data, size_t n) {
    const uint8_t *p = data;
    size_t have = (size_t)(m->bytes & 63);
    m->bytes += n;
    if (have) {
        size_t take = 64 - have;
        if (take > n) take = n;
        memcpy(m->block + have, p, take);
        p += take;
        n -= take;
        if (have + take < 64) return;
        md5_block(m, m->block);
    }
    while (n >= 64) { md5_block(m, p); p += 64; n -= 64; }
    if (n) memcpy(m->block, p, n);
}

void flac_md5_final(FlacMd5 *m, uint8_t out[16]) {
    uint64_t bits = m->bytes * 8;
    size_t have = (size_t)(m->bytes & 63);
    uint8_t pad[72];
    size_t padlen = have < 56 ? 56 - have : 120 - have;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    flac_md5_update(m, pad, padlen);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (8 * i));
    flac_md5_update(m, len, 8);
    const uint32_t st[4] = { m->a, m->b, m->c, m->d };
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++) out[4 * i + k] = (uint8_t)(st[i] >> (8 * k));
}

/* ---------------------------------------------------------------------------------------------------- bit reader */

/* Reads MSB first from a block of memory. `total` is the number of valid bits; a read that would go past it sets `bad` and
 * returns 0, and from then on every read does the same. The 8-byte loads may look at up to 7 bytes past the valid data, so
 * the allocation behind the block has BUF_PAD spare bytes. */
typedef struct { const uint8_t *p; uint64_t total, bit; bool bad; } Bits;

static inline uint64_t load_be64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}

static inline uint64_t get_bits(Bits *b, unsigned n) {                     /* n <= 57 */
    if (n == 0) return 0;
    if (b->bad || b->bit + n > b->total) { b->bad = true; return 0; }
    uint64_t w = load_be64(b->p + (b->bit >> 3)) << (b->bit & 7);
    b->bit += n;
    return w >> (64 - n);
}

static inline int64_t get_sbits(Bits *b, unsigned n) {                     /* two's complement, 1 <= n <= 57 */
    uint64_t v = get_bits(b, n);
    uint64_t m = 1ull << (n - 1);
    return (int64_t)(v ^ m) - (int64_t)m;
}

/* The number of 0 bits before the next 1 bit (which is consumed). */
static inline uint64_t get_unary(Bits *b) {
    uint64_t count = 0;
    for (;;) {
        if (b->bad || b->bit >= b->total) { b->bad = true; return 0; }
        unsigned off = (unsigned)(b->bit & 7);
        uint64_t w = load_be64(b->p + (b->bit >> 3)) << off;
        unsigned valid = 64 - off;
        if (w) {
            unsigned z = (unsigned)__builtin_clzll(w);
            b->bit += z + 1;
            if (b->bit > b->total) { b->bad = true; return 0; }
            return count + z;
        }
        b->bit += valid;
        count += valid;
    }
}

/* -------------------------------------------------------------------------------------------------------- subframes */

enum { FD_OK = 0, FD_SHORT, FD_BAD_HEADER, FD_BAD_CRC, FD_BAD_DATA, FD_NOMEM };

/* The residual (RFC 9639 section 9.2.7): the coding method, the partitions, each with a Rice parameter or an escape. The first
 * `order` samples of x are the warm-up samples (already read); x[order..n) receives the residual. */
static int read_residual(Bits *b, uint32_t n, uint32_t order, int64_t *x) {
    unsigned method = (unsigned)get_bits(b, 2);
    if (method > 1) return b->bad ? FD_SHORT : FD_BAD_DATA;                     /* 2 and 3 are reserved */
    unsigned porder = (unsigned)get_bits(b, 4);
    uint32_t parts = 1u << porder;
    if (b->bad) return FD_SHORT;
    if (n & (parts - 1)) return FD_BAD_DATA;                                    /* the block size must divide into the partitions */
    uint32_t psize = n >> porder;
    if (psize < order) return FD_BAD_DATA;                                      /* the first partition would hold a negative count */
    const unsigned pbits = method ? 5 : 4, esc = method ? 31 : 15;
    uint32_t at = order;
    for (uint32_t p = 0; p < parts; p++) {
        uint32_t count = p == 0 ? psize - order : psize;
        unsigned k = (unsigned)get_bits(b, pbits);
        if (b->bad) return FD_SHORT;
        if (k == esc) {                                                         /* escape: `bits` plain two's complement values per sample */
            unsigned nb = (unsigned)get_bits(b, 5);
            for (uint32_t i = 0; i < count; i++) x[at + i] = nb ? get_sbits(b, nb) : 0;
            if (b->bad) return FD_SHORT;
        } else {
            const uint64_t qmax = 0xFFFFFFFFull >> k;                           /* the folded value must fit in 32 bits */
            for (uint32_t i = 0; i < count; i++) {
                uint64_t q = get_unary(b);
                if (b->bad) return FD_SHORT;
                if (q > qmax) return FD_BAD_DATA;
                uint64_t u = (q << k) | (k ? get_bits(b, k) : 0);
                int64_t v = (int64_t)(u >> 1);
                x[at + i] = (u & 1) ? ~v : v;
            }
            if (b->bad) return FD_SHORT;
        }
        at += count;
    }
    return FD_OK;
}

static bool restore_fixed(int64_t *x, uint32_t n, unsigned order, int64_t lo, int64_t hi) {
    for (uint32_t i = order; i < n; i++) {
        int64_t p = 0;
        switch (order) {
        case 1: p = x[i - 1]; break;
        case 2: p = 2 * x[i - 1] - x[i - 2]; break;
        case 3: p = 3 * x[i - 1] - 3 * x[i - 2] + x[i - 3]; break;
        case 4: p = 4 * x[i - 1] - 6 * x[i - 2] + 4 * x[i - 3] - x[i - 4]; break;
        default: break;
        }
        int64_t v = x[i] + p;
        if (v < lo || v > hi) return false;
        x[i] = v;
    }
    return true;
}

static bool restore_lpc(int64_t *x, uint32_t n, unsigned order, const int32_t *coef, unsigned shift, int64_t lo, int64_t hi) {
    for (uint32_t i = order; i < n; i++) {
        const int64_t *s = x + i;
        int64_t sum = 0;
        for (unsigned j = 0; j < order; j++) sum += (int64_t)coef[j] * s[-1 - (int)j];
        int64_t v = x[i] + (sum >> shift);
        if (v < lo || v > hi) return false;
        x[i] = v;
    }
    return true;
}

/* One subframe of n samples at `bps` bits (the side channel of a stereo pair has one bit more) into x[0..n). */
static int read_subframe(Bits *b, uint32_t n, unsigned bps, int64_t *x) {
    if (get_bits(b, 1)) return b->bad ? FD_SHORT : FD_BAD_DATA;                 /* the padding bit is zero */
    unsigned type = (unsigned)get_bits(b, 6);
    unsigned wasted = 0;
    if (get_bits(b, 1)) {
        uint64_t z = get_unary(b);
        if (b->bad) return FD_SHORT;
        if (z + 1 >= bps) return FD_BAD_DATA;                                   /* nothing would be left of the samples */
        wasted = (unsigned)z + 1;
    }
    if (b->bad) return FD_SHORT;
    const unsigned sbps = bps - wasted;
    const int64_t hi = (int64_t)((1ull << (sbps - 1)) - 1), lo = -hi - 1;
    int rc = FD_OK;

    if (type == 0) {                                                            /* CONSTANT */
        int64_t v = get_sbits(b, sbps);
        for (uint32_t i = 0; i < n; i++) x[i] = v;
    } else if (type == 1) {                                                     /* VERBATIM */
        for (uint32_t i = 0; i < n; i++) x[i] = get_sbits(b, sbps);
    } else if (type >= 8 && type <= 12) {                                       /* FIXED, order 0 to 4 */
        unsigned order = type - 8;
        if (order > n) return FD_BAD_DATA;
        for (unsigned i = 0; i < order; i++) x[i] = get_sbits(b, sbps);
        if (b->bad) return FD_SHORT;
        rc = read_residual(b, n, order, x);
        if (rc) return rc;
        if (!restore_fixed(x, n, order, lo, hi)) return FD_BAD_DATA;
    } else if (type >= 32) {                                                    /* LPC, order 1 to 32 */
        unsigned order = type - 31;
        if (order > n) return FD_BAD_DATA;
        for (unsigned i = 0; i < order; i++) x[i] = get_sbits(b, sbps);
        unsigned pm1 = (unsigned)get_bits(b, 4);
        if (pm1 == 15) return b->bad ? FD_SHORT : FD_BAD_DATA;                  /* precision 16 is not allowed */
        unsigned precision = pm1 + 1;
        int64_t shift = get_sbits(b, 5);
        if (b->bad) return FD_SHORT;
        if (shift < 0) return FD_BAD_DATA;                                      /* a negative shift is not allowed */
        int32_t coef[32];
        for (unsigned i = 0; i < order; i++) coef[i] = (int32_t)get_sbits(b, precision);
        if (b->bad) return FD_SHORT;
        rc = read_residual(b, n, order, x);
        if (rc) return rc;
        if (!restore_lpc(x, n, order, coef, (unsigned)shift, lo, hi)) return FD_BAD_DATA;
    } else {
        return FD_BAD_DATA;                                                     /* reserved subframe types */
    }
    if (b->bad) return FD_SHORT;
    if (wasted)
        for (uint32_t i = 0; i < n; i++) x[i] = (int64_t)((uint64_t)x[i] << wasted);
    return FD_OK;
}

/* ----------------------------------------------------------------------------------------------------------- frames */

enum { MODE_INDEP = 0, MODE_LEFT_SIDE, MODE_SIDE_RIGHT, MODE_MID_SIDE };

typedef struct {
    uint32_t blocksize, rate, channels, mode, bits;
    bool     variable;                  /* variable-blocksize stream: `number` is a sample number, else a frame number */
    uint64_t number;
    size_t   len;                       /* header bytes, CRC-8 included */
} FrameHeader;

/* The UTF-8 style number of the frame header: 1 to 7 bytes, up to 36 bits. Returns the length, 0 if invalid, -1 if the bytes are not all there. */
static int read_coded_number(const uint8_t *p, size_t avail, uint64_t *out) {
    if (avail < 1) return -1;
    unsigned b = p[0], len;
    uint64_t v;
    if (b < 0x80) { *out = b; return 1; }
    if ((b & 0xE0) == 0xC0)      { len = 2; v = b & 0x1F; }
    else if ((b & 0xF0) == 0xE0) { len = 3; v = b & 0x0F; }
    else if ((b & 0xF8) == 0xF0) { len = 4; v = b & 0x07; }
    else if ((b & 0xFC) == 0xF8) { len = 5; v = b & 0x03; }
    else if ((b & 0xFE) == 0xFC) { len = 6; v = b & 0x01; }
    else if (b == 0xFE)          { len = 7; v = 0; }
    else return 0;                                                              /* a continuation byte or 0xFF */
    if (avail < len) return -1;
    for (unsigned i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        v = (v << 6) | (p[i] & 0x3F);
    }
    static const uint64_t least[8] = { 0, 0, 0x80, 0x800, 0x10000, 0x200000, 0x4000000, 0x80000000ull };
    if (v < least[len]) return 0;                                               /* not the shortest coding */
    *out = v;
    return (int)len;
}

static int parse_header(const FlacInfo *in, const uint8_t *p, size_t avail, FrameHeader *h) {
    if (avail < 2) return FD_SHORT;
    if (p[0] != 0xFF || (p[1] & 0xFE) != 0xF8) return FD_BAD_HEADER;            /* sync code 11111111111110, reserved bit 0, blocking strategy */
    if (avail < 4) return FD_SHORT;
    h->variable = (p[1] & 1) != 0;
    unsigned bs_code = p[2] >> 4, sr_code = p[2] & 15, ch_code = p[3] >> 4, sz_code = (p[3] >> 1) & 7;
    if (p[3] & 1) return FD_BAD_HEADER;                                         /* reserved bit */
    if (bs_code == 0 || sr_code == 15 || ch_code > 10 || sz_code == 3) return FD_BAD_HEADER;

    if (ch_code < 8) { h->channels = ch_code + 1; h->mode = MODE_INDEP; }
    else { h->channels = 2; h->mode = ch_code == 8 ? MODE_LEFT_SIDE : ch_code == 9 ? MODE_SIDE_RIGHT : MODE_MID_SIDE; }
    static const uint8_t sizes[8] = { 0, 8, 12, 0, 16, 20, 24, 32 };
    h->bits = sz_code ? sizes[sz_code] : in->bits;
    if (h->channels != in->channels || h->bits != in->bits) return FD_BAD_HEADER;

    size_t at = 4;
    int nl = read_coded_number(p + at, avail - at, &h->number);
    if (nl == 0) return FD_BAD_HEADER;
    if (nl < 0) return FD_SHORT;
    if (!h->variable && (nl > 6 || h->number >= (1ull << 31))) return FD_BAD_HEADER;    /* a frame number has 31 bits at most */
    at += (size_t)nl;

    uint32_t bs;
    if (bs_code == 1) bs = 192;
    else if (bs_code <= 5) bs = 576u << (bs_code - 2);
    else if (bs_code >= 8) bs = 256u << (bs_code - 8);
    else if (bs_code == 6) {
        if (avail < at + 1) return FD_SHORT;
        bs = (uint32_t)p[at] + 1;
        at += 1;
    } else {
        if (avail < at + 2) return FD_SHORT;
        bs = ((uint32_t)p[at] << 8 | p[at + 1]) + 1;
        at += 2;
    }
    if (bs > MAX_BLOCK) return FD_BAD_HEADER;
    h->blocksize = bs;

    static const uint32_t rates[16] = { 0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000, 0, 0, 0, 0 };
    uint32_t rate;
    if (sr_code == 0) rate = in->rate;
    else if (sr_code < 12) rate = rates[sr_code];
    else if (sr_code == 12) {
        if (avail < at + 1) return FD_SHORT;
        rate = (uint32_t)p[at] * 1000;
        at += 1;
    } else {
        if (avail < at + 2) return FD_SHORT;
        rate = (uint32_t)p[at] << 8 | p[at + 1];
        if (sr_code == 14) rate *= 10;
        at += 2;
    }
    if (rate == 0 || rate != in->rate) return FD_BAD_HEADER;                    /* the rate of a stream does not change */
    h->rate = rate;

    if (avail < at + 1) return FD_SHORT;
    if (crc8(p, at) != p[at]) return FD_BAD_HEADER;
    h->len = at + 1;
    return FD_OK;
}

/* ------------------------------------------------------------------------------------------------------------ stream */

typedef struct { uint64_t sample, offset; uint32_t samples; } SeekPoint;

struct Flac {
    FlacIO io;
    FlacInfo info;

    uint8_t *buf;                       /* read buffer: buf[pos..len) is unread; buf[0] is at file offset `base` */
    size_t cap, len, pos;
    uint64_t base;
    bool eof;                           /* the read callback has returned 0: nothing follows buf[len] */
    size_t hint;                        /* how much of the file to have in the buffer before decoding a frame */
    size_t chunk;                       /* the most one read callback is asked for: small while the metadata is read (a playlist opens hundreds of files), the whole buffer after */

    SeekPoint *seek;
    uint32_t nseek;                     /* entries in `seek`, placeholders included */

    int64_t *chan_mem;                  /* the decoded block: channels * chan_cap samples */
    int64_t *chan[FLAC_MAX_CHANNELS];
    uint32_t chan_cap;
    uint32_t blk_len, blk_pos;          /* samples in the current block; how many have been handed out */

    uint64_t out_pos;                   /* the number of the next sample flac_read returns */
    uint64_t frame_next;                /* where the next frame should start, by the frames decoded so far */
    uint64_t silence_left, concealed;
    int      pending;                   /* an error to report before going on */
    uint32_t nerrors;
    bool     ended;
    uint64_t fail_end;                  /* where the furthest failed decode attempt of the current damaged stretch ended (file offset) */
    unsigned overlap;
};

const char *flac_strerror(int code) {
    switch (code) {
    case FLAC_OK:            return "ok";
    case FLAC_E_IO:          return "read error";
    case FLAC_E_NOMEM:       return "out of memory";
    case FLAC_E_NOTFLAC:     return "not a FLAC file";
    case FLAC_E_BADMETA:     return "damaged FLAC metadata";
    case FLAC_E_UNSUPPORTED: return "not supported";
    case FLAC_E_ARG:         return "cannot do that with this stream";
    case FLAC_E_SYNC:        return "bytes that are not audio frames were skipped";
    case FLAC_E_HEADER:      return "a frame header is damaged";
    case FLAC_E_CRC:         return "a frame failed its checksum";
    case FLAC_E_FRAME:       return "a frame is damaged";
    case FLAC_E_TRUNCATED:   return "the file ends in the middle of a frame";
    case FLAC_E_MD5:         return "the audio does not match the checksum in the file";
    case FLAC_E_LENGTH:      return "the audio has not the length the file header says";
    default:                 return "unknown error";
    }
}

/* Makes sure buf[pos..len) holds at least `want` bytes unless the file ends first. */
static int fill(Flac *f, size_t want) {
    if (f->len - f->pos >= want || f->eof) return 0;
    if (f->pos) {
        memmove(f->buf, f->buf + f->pos, f->len - f->pos);
        f->base += f->pos;
        f->len -= f->pos;
        f->pos = 0;
    }
    if (want > f->cap) {
        size_t ncap = f->cap;
        while (ncap < want) ncap *= 2;
        if (ncap > BUF_MAX) ncap = BUF_MAX;
        if (ncap < want) want = ncap;
        if (ncap > f->cap) {
            uint8_t *nb = realloc(f->buf, ncap + BUF_PAD);
            if (!nb) return FLAC_E_NOMEM;
            memset(nb + f->cap, 0, ncap + BUF_PAD - f->cap);
            f->buf = nb;
            f->cap = ncap;
        }
    }
    while (f->len < want && !f->eof) {
        size_t room = f->cap - f->len;
        if (f->chunk && room > f->chunk) room = f->chunk;
        ssize_t n = f->io.read(f->io.ctx, f->buf + f->len, room);
        if (n < 0) return FLAC_E_IO;
        if (n == 0) f->eof = true;
        else f->len += (size_t)n;
    }
    return 0;
}

/* Discards n bytes of the stream. */
static int skip_bytes(Flac *f, uint64_t n) {
    uint64_t have = f->len - f->pos;
    if (n <= have) { f->pos += (size_t)n; return 0; }
    n -= have;
    f->pos = f->len;
    uint64_t target = f->base + f->len + n;
    if (f->io.seek && f->io.size && target <= f->io.size) {
        if (f->io.seek(f->io.ctx, target) < 0) return FLAC_E_IO;
        f->base = target;
        f->pos = f->len = 0;
        f->eof = false;
        return 0;
    }
    while (n) {                                                                 /* no seeking: read it and throw it away */
        f->base += f->len;
        f->pos = f->len = 0;
        int rc = fill(f, 1);
        if (rc) return rc;
        if (f->len == 0) return 0;                                              /* the file ended: the caller notices */
        size_t take = f->len < n ? f->len : (size_t)n;
        f->pos = take;
        n -= take;
    }
    return 0;
}

static int ensure_chan(Flac *f, uint32_t n) {
    if (n <= f->chan_cap) return 0;
    uint32_t cap = f->chan_cap ? f->chan_cap : 4096;
    while (cap < n) cap *= 2;
    if (cap > MAX_BLOCK) cap = MAX_BLOCK;
    int64_t *m = malloc((size_t)cap * f->info.channels * sizeof(int64_t));
    if (!m) return FLAC_E_NOMEM;
    free(f->chan_mem);
    f->chan_mem = m;
    f->chan_cap = cap;
    for (uint32_t c = 0; c < f->info.channels; c++) f->chan[c] = m + (size_t)c * cap;
    return 0;
}

/* Decodes the frame that starts at p (avail bytes follow). On FD_OK the samples are in f->chan[], *used is the frame's length in
 * bytes; on any failure *used says how many bytes were looked at (at least 1). */
static int decode_frame(Flac *f, const uint8_t *p, size_t avail, FrameHeader *h, size_t *used) {
    *used = 1;
    int rc = parse_header(&f->info, p, avail, h);
    if (rc == FD_SHORT) { *used = avail; return rc; }
    if (rc) return rc;
    if (ensure_chan(f, h->blocksize)) return FD_NOMEM;

    Bits b = { p, (uint64_t)avail * 8, (uint64_t)h->len * 8, false };
    const uint32_t n = h->blocksize;
    for (uint32_t c = 0; c < h->channels; c++) {
        unsigned bps = h->bits;
        if ((h->mode == MODE_LEFT_SIDE && c == 1) || (h->mode == MODE_SIDE_RIGHT && c == 0) || (h->mode == MODE_MID_SIDE && c == 1)) bps++;
        rc = read_subframe(&b, n, bps, f->chan[c]);
        if (rc) { *used = rc == FD_SHORT ? avail : (size_t)(b.bit >> 3) + 1; return rc; }
    }

    if (h->mode != MODE_INDEP) {                                                /* undo the stereo decorrelation (the side channel has 33 bits for 32-bit audio) */
        int64_t *a = f->chan[0], *s = f->chan[1];
        const int64_t hi = (int64_t)((1ull << (h->bits - 1)) - 1), lo = -hi - 1;
        for (uint32_t i = 0; i < n; i++) {
            int64_t l, r;
            if (h->mode == MODE_LEFT_SIDE)       { l = a[i]; r = a[i] - s[i]; }
            else if (h->mode == MODE_SIDE_RIGHT) { l = s[i] + a[i]; r = s[i]; }
            else {
                int64_t side = s[i], twice = a[i] * 2 + (side & 1);             /* mid = (l + r) >> 1; the lost bit is the parity of the side */
                l = (twice + side) >> 1;
                r = (twice - side) >> 1;
            }
            if (l < lo || l > hi || r < lo || r > hi) { *used = (size_t)(b.bit >> 3) + 1; return FD_BAD_DATA; }
            a[i] = l;
            s[i] = r;
        }
    }

    size_t end = (size_t)((b.bit + 7) >> 3);                                    /* zero padding up to a byte boundary, then the CRC-16 */
    if (end + 2 > avail) { *used = avail; return FD_SHORT; }
    uint16_t want = (uint16_t)(p[end] << 8 | p[end + 1]);
    *used = end + 2;
    if (crc16(p, end) != want) return FD_BAD_CRC;
    return FD_OK;
}

/* After a failed attempt that started at the read position and looked at `looked` bytes: step forward by one byte, which finds a frame
 * that starts inside the bad one, unless we keep failing inside the same stretch (a crafted file full of frame-like headers would
 * make every byte cost a whole frame): then skip what was looked at. */
static void step_after_failure(Flac *f, size_t looked) {
    uint64_t start = f->base + f->pos;
    if (start < f->fail_end) f->overlap++;
    else f->overlap = 0;
    if (start + looked > f->fail_end) f->fail_end = start + looked;
    uint64_t next = f->overlap > 4 ? f->fail_end : start + 1;
    uint64_t room = f->base + f->len;
    if (next > room) next = room;
    f->pos = (size_t)(next - f->base);
}

/* Finds the next frame at or after the read position and decodes it (into f->chan). Returns 1 when there is one: f->pos is at its
 * start and *used is its length. 0: no frame before the data ends (or before maxscan bytes were passed). Negative: I/O or memory error.
 * *event gets the first kind of damage that was skipped on the way, if any. */
static int locate_frame(Flac *f, FrameHeader *h, size_t *used, int *event, uint64_t maxscan) {
    uint64_t start = f->base + f->pos;
    for (;;) {
        int rc = fill(f, f->hint);
        if (rc) return rc;
        size_t avail = f->len - f->pos;
        if (avail == 0) return 0;
        if (f->base + f->pos - start > maxscan) return 0;
        const uint8_t *p = f->buf + f->pos;
        if (avail >= 2 && p[0] == 0xFF && (p[1] & 0xFE) == 0xF8) {
            size_t u = 0;
            int st = decode_frame(f, p, avail, h, &u);
            if (st == FD_OK) { *used = u; return 1; }
            if (st == FD_NOMEM) return FLAC_E_NOMEM;
            if (st == FD_SHORT && !f->eof && f->hint < BUF_MAX) {               /* the frame is bigger than what STREAMINFO promised: look at more of the file */
                f->hint = f->hint * 2 > BUF_MAX ? BUF_MAX : f->hint * 2;
                continue;
            }
            int code = st == FD_BAD_HEADER ? FLAC_E_HEADER : st == FD_BAD_CRC ? FLAC_E_CRC : st == FD_SHORT && f->eof ? FLAC_E_TRUNCATED : FLAC_E_FRAME;
            if (!*event) *event = code;
            step_after_failure(f, u);
        } else {
            if (!*event) *event = FLAC_E_SYNC;
            const uint8_t *q = avail > 1 ? memchr(p + 1, 0xFF, avail - 1) : NULL;
            f->pos += q ? (size_t)(q - p) : avail;
        }
    }
}

/* Decodes the next frame as the current block. Returns 1 for a frame; 0 at the end of the stream; a negative FLAC_E_* for damage that
 * was skipped (the block that follows it is loaded unless the stream ended: f->blk_len says). */
static int next_block(Flac *f) {
    FrameHeader h;
    size_t used = 0;
    int event = 0;
    int rc = locate_frame(f, &h, &used, &event, UINT64_MAX);
    if (rc < 0) { f->ended = true; return rc; }
    if (rc == 0) {
        f->ended = true;
        if (event && f->info.total_samples && f->frame_next >= f->info.total_samples) event = 0;      /* what follows the last frame (a tag) is not our business */
        return event;
    }
    f->pos += used;
    f->fail_end = 0;
    f->overlap = 0;
    const bool known = h.variable || (f->info.min_block == f->info.max_block && f->info.max_block != 0);
    const uint64_t first = h.variable ? h.number : h.number * f->info.max_block;
    if (event && known) {                                                       /* frames were lost: stand in for them with silence so that the position stays right */
        if (first > f->frame_next && first - f->frame_next <= MAX_CONCEAL) {
            f->silence_left += first - f->frame_next;
            f->concealed += first - f->frame_next;
        }
        f->frame_next = first;
    }
    f->frame_next += h.blocksize;
    f->blk_len = h.blocksize;
    f->blk_pos = 0;
    return event ? event : 1;
}

/* ---------------------------------------------------------------------------------------------------------- metadata */

static uint32_t be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

/* Copies a tag value: valid UTF-8 only (a bad byte becomes '?'), control characters become spaces, cut at a character boundary. */
static void copy_text(char *dst, size_t cap, const uint8_t *src, size_t n) {
    size_t o = 0, i = 0;
    while (i < n) {
        unsigned c = src[i], len = 1;
        if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c >= 0xE0 && c <= 0xEF) len = 3;
        else if (c >= 0xF0 && c <= 0xF4) len = 4;
        else if (c >= 0x80) len = 0;
        bool ok = len >= 1 && i + len <= n;
        for (unsigned k = 1; ok && k < len; k++) ok = (src[i + k] & 0xC0) == 0x80;
        if (len == 0 || !ok) { if (o + 1 >= cap) break; dst[o++] = '?'; i++; continue; }
        if (o + len >= cap) break;
        if (len == 1 && (c < 0x20 || c == 0x7F)) dst[o++] = ' ';
        else { memcpy(dst + o, src + i, len); o += len; }
        i += len;
    }
    dst[o] = 0;
}

static bool tag_is(const uint8_t *e, size_t n, const char *name, const uint8_t **value, size_t *vlen) {
    size_t k = strlen(name);
    if (n <= k || e[k] != '=') return false;
    for (size_t i = 0; i < k; i++) {
        unsigned c = e[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c != (unsigned)name[i]) return false;
    }
    *value = e + k + 1;
    *vlen = n - k - 1;
    return true;
}

static void read_vorbis_comment(FlacInfo *in, const uint8_t *p, size_t n) {
    if (n < 4) return;
    uint32_t vlen = le32(p);
    p += 4; n -= 4;
    if (vlen > n) vlen = (uint32_t)n;
    copy_text(in->tags.vendor, sizeof in->tags.vendor, p, vlen);
    p += vlen; n -= vlen;
    if (n < 4) return;
    uint32_t count = le32(p);
    p += 4; n -= 4;
    for (uint32_t i = 0; i < count && n >= 4; i++) {
        uint32_t l = le32(p);
        p += 4; n -= 4;
        if (l > n) break;
        const uint8_t *v;
        size_t vl;
        if (tag_is(p, l, "TITLE", &v, &vl)) copy_text(in->tags.title, sizeof in->tags.title, v, vl);
        else if (tag_is(p, l, "ARTIST", &v, &vl)) copy_text(in->tags.artist, sizeof in->tags.artist, v, vl);
        else if (tag_is(p, l, "ALBUM", &v, &vl)) copy_text(in->tags.album, sizeof in->tags.album, v, vl);
        else if (tag_is(p, l, "DATE", &v, &vl)) copy_text(in->tags.date, sizeof in->tags.date, v, vl);
        else if (tag_is(p, l, "GENRE", &v, &vl)) copy_text(in->tags.genre, sizeof in->tags.genre, v, vl);
        else if (tag_is(p, l, "TRACKNUMBER", &v, &vl)) copy_text(in->tags.tracknumber, sizeof in->tags.tracknumber, v, vl);
        else if (tag_is(p, l, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK", &v, &vl)) {
            uint32_t mask = 0;
            size_t k = 0;
            if (vl > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) k = 2;
            for (; k < vl && k < 10; k++) {
                unsigned c = v[k], d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
                if (d > 15) { mask = 0; break; }
                mask = mask << 4 | d;
            }
            if (mask && (unsigned)__builtin_popcount(mask) == in->channels) in->channel_mask = mask;
        }
        p += l; n -= l;
    }
}

/* The speaker layout RFC 9639 gives for each channel count, as a WAVEFORMATEXTENSIBLE mask. */
static uint32_t default_mask(uint32_t channels) {
    static const uint32_t m[9] = { 0, 0x4, 0x3, 0x7, 0x33, 0x37, 0x3F, 0x70F, 0x63F };
    return channels <= 8 ? m[channels] : 0;
}

static int read_metadata(Flac *f) {
    FlacInfo *in = &f->info;
    int rc = fill(f, 10);
    if (rc) return rc;
    const uint8_t *p = f->buf + f->pos;
    if (f->len - f->pos >= 10 && !memcmp(p, "ID3", 3) && p[3] != 0xFF && p[4] != 0xFF && !((p[6] | p[7] | p[8] | p[9]) & 0x80)) {
        uint32_t size = (uint32_t)p[6] << 21 | (uint32_t)p[7] << 14 | (uint32_t)p[8] << 7 | p[9];       /* ID3v2: a tag in front of the stream, skipped */
        uint64_t total = 10 + (uint64_t)size + ((p[5] & 0x10) ? 10 : 0);
        if (f->io.size && total > f->io.size) return FLAC_E_NOTFLAC;
        rc = skip_bytes(f, total);
        if (rc) return rc;
        in->id3v2_bytes = (uint32_t)total;
    }
    rc = fill(f, 4);
    if (rc) return rc;
    if (f->len - f->pos < 4 || memcmp(f->buf + f->pos, "fLaC", 4)) return FLAC_E_NOTFLAC;
    f->pos += 4;

    bool first = true, have_info = false;
    for (;;) {
        rc = fill(f, 4);
        if (rc) return rc;
        if (f->len - f->pos < 4) return FLAC_E_BADMETA;                         /* the file ends inside the metadata */
        const uint8_t *h = f->buf + f->pos;
        bool last = (h[0] & 0x80) != 0;
        unsigned type = h[0] & 0x7F;
        uint32_t len = be24(h + 1);
        f->pos += 4;
        if (type == 127) return FLAC_E_BADMETA;                                 /* would look like a frame sync */
        if (first && type != 0) return FLAC_E_BADMETA;                          /* STREAMINFO comes first */
        first = false;
        uint64_t at = f->base + f->pos;
        if (f->io.size && at + len > f->io.size) return FLAC_E_BADMETA;         /* a block that does not fit in the file */

        if (type == 0) {
            if (len != 34 || have_info) return FLAC_E_BADMETA;
            rc = fill(f, 34);
            if (rc) return rc;
            if (f->len - f->pos < 34) return FLAC_E_BADMETA;
            const uint8_t *s = f->buf + f->pos;
            in->min_block = (uint32_t)s[0] << 8 | s[1];
            in->max_block = (uint32_t)s[2] << 8 | s[3];
            in->min_frame = be24(s + 4);
            in->max_frame = be24(s + 7);
            uint64_t v = be64(s + 10);
            in->rate = (uint32_t)(v >> 44);
            in->channels = (uint32_t)((v >> 41) & 7) + 1;
            in->bits = (uint32_t)((v >> 36) & 31) + 1;
            in->total_samples = v & ((1ull << 36) - 1);
            memcpy(in->md5, s + 18, 16);
            for (int i = 0; i < 16; i++) if (in->md5[i]) in->has_md5 = true;
            if (in->rate == 0 || in->bits < 4) return FLAC_E_BADMETA;
            in->channel_mask = default_mask(in->channels);
            have_info = true;
            f->pos += 34;
        } else if (type == 3 && len % 18 == 0 && len <= SEEKTABLE_MAX && len > 0) {
            rc = fill(f, len);
            if (rc) return rc;
            if (f->len - f->pos < len) return FLAC_E_BADMETA;
            uint32_t n = len / 18;
            SeekPoint *sp = malloc((size_t)n * sizeof *sp);
            if (!sp) return FLAC_E_NOMEM;
            const uint8_t *s = f->buf + f->pos;
            uint32_t real = 0;
            for (uint32_t i = 0; i < n; i++, s += 18) {
                sp[i].sample = be64(s);
                sp[i].offset = be64(s + 8);
                sp[i].samples = (uint32_t)s[16] << 8 | s[17];
                if (sp[i].sample != ~0ull) real++;
            }
            free(f->seek);
            f->seek = sp;
            f->nseek = n;
            in->seek_points = real;
            f->pos += len;
        } else if (type == 4) {
            uint32_t take = len < VC_MAX ? len : VC_MAX;
            rc = fill(f, take);
            if (rc) return rc;
            if (f->len - f->pos < take) return FLAC_E_BADMETA;
            if (have_info) read_vorbis_comment(in, f->buf + f->pos, take);
            rc = skip_bytes(f, len);
            if (rc) return rc;
        } else {
            if (type == 6) { in->pictures++; in->picture_bytes += len; }
            rc = skip_bytes(f, len);
            if (rc) return rc;
        }
        if (last) break;
    }
    if (!have_info) return FLAC_E_BADMETA;
    in->first_frame_offset = f->base + f->pos;
    return 0;
}

bool flac_sniff(const uint8_t *buf, size_t n) {
    size_t at = 0;
    if (n >= 10 && !memcmp(buf, "ID3", 3) && !((buf[6] | buf[7] | buf[8] | buf[9]) & 0x80))
        at = 10 + ((size_t)buf[6] << 21 | (size_t)buf[7] << 14 | (size_t)buf[8] << 7 | buf[9]) + ((buf[5] & 0x10) ? 10 : 0);
    return n >= at + 4 && !memcmp(buf + at, "fLaC", 4);
}

Flac *flac_open(const FlacIO *io, int *err) {
    int dummy;
    if (!err) err = &dummy;
    *err = 0;
    if (!io || !io->read) { *err = FLAC_E_ARG; return NULL; }
    crc_init();
    Flac *f = calloc(1, sizeof *f);
    if (!f) { *err = FLAC_E_NOMEM; return NULL; }
    f->io = *io;
    f->cap = BUF_INIT;
    f->buf = calloc(1, f->cap + BUF_PAD);
    if (!f->buf) { free(f); *err = FLAC_E_NOMEM; return NULL; }
    f->chunk = 4096;
    int rc = read_metadata(f);
    f->chunk = 0;
    if (rc) { flac_close(f); *err = rc; return NULL; }
    f->hint = f->info.max_frame ? (size_t)f->info.max_frame + 16 : 65536;
    if (f->hint < 4096) f->hint = 4096;
    if (f->hint > BUF_MAX / 2) f->hint = BUF_MAX / 2;
    return f;
}

void flac_close(Flac *f) {
    if (!f) return;
    free(f->buf);
    free(f->seek);
    free(f->chan_mem);
    free(f);
}

const FlacInfo *flac_info(const Flac *f) { return &f->info; }
uint64_t flac_tell(const Flac *f) { return f->out_pos; }
uint64_t flac_concealed(const Flac *f) { return f->concealed; }
uint32_t flac_errors(const Flac *f) { return f->nerrors; }

/* --------------------------------------------------------------------------------------------------------- reading */

/* Hands out up to `max` frames; out == NULL drops them (seeking). */
static size_t take_frames(Flac *f, void *out, size_t max, int flags) {
    const uint32_t ch = f->info.channels;
    size_t done = 0;
    while (done < max) {
        if (f->silence_left) {
            size_t n = f->silence_left < max - done ? (size_t)f->silence_left : max - done;
            if (out) memset((uint8_t *)out + done * ch * ((flags & FLAC_READ_S16) ? 2 : 4), 0, n * ch * ((flags & FLAC_READ_S16) ? 2 : 4));
            f->silence_left -= n;
            done += n;
        } else if (f->blk_pos < f->blk_len) {
            size_t n = f->blk_len - f->blk_pos < max - done ? f->blk_len - f->blk_pos : max - done;
            if (out) {
                if (flags & FLAC_READ_S16) {
                    int16_t *o = (int16_t *)out + done * ch;
                    const unsigned bits = f->info.bits;
                    for (size_t i = 0; i < n; i++)
                        for (uint32_t c = 0; c < ch; c++) {
                            int64_t v = f->chan[c][f->blk_pos + i];
                            o[i * ch + c] = (int16_t)(bits >= 16 ? v >> (bits - 16) : v * (1 << (16 - bits)));
                        }
                } else {
                    int32_t *o = (int32_t *)out + done * ch;
                    for (size_t i = 0; i < n; i++)
                        for (uint32_t c = 0; c < ch; c++) o[i * ch + c] = (int32_t)f->chan[c][f->blk_pos + i];
                }
            }
            f->blk_pos += (uint32_t)n;
            done += n;
        } else {
            break;
        }
    }
    f->out_pos += done;
    return done;
}

ssize_t flac_read(Flac *f, void *out, size_t frames, int flags) {
    if (!f || !out) return FLAC_E_ARG;
    if (f->pending) { int e = f->pending; f->pending = 0; return e; }
    size_t done = 0;
    while (done < frames) {
        done += take_frames(f, (uint8_t *)out + done * f->info.channels * ((flags & FLAC_READ_S16) ? 2 : 4), frames - done, flags);
        if (done == frames || f->ended) break;                                  /* take_frames only stops short when the block and the silence are used up */
        int r = next_block(f);
        if (r == 1) continue;
        if (r == 0) break;
        if (r != FLAC_E_IO && r != FLAC_E_NOMEM) f->nerrors++;
        if (done) { f->pending = r; break; }
        return r;
    }
    return (ssize_t)done;
}

/* --------------------------------------------------------------------------------------------------------- seeking */

static int reposition(Flac *f, uint64_t offset, uint64_t sample) {
    if (!f->io.seek) return FLAC_E_ARG;
    if (offset >= f->base && offset <= f->base + f->len) {
        f->pos = (size_t)(offset - f->base);
    } else {
        if (f->io.seek(f->io.ctx, offset) < 0) return FLAC_E_IO;
        f->base = offset;
        f->pos = f->len = 0;
        f->eof = false;
    }
    f->frame_next = sample;
    f->blk_len = f->blk_pos = 0;
    f->silence_left = 0;
    f->pending = 0;
    f->ended = false;
    f->fail_end = 0;
    f->overlap = 0;
    return 0;
}

static bool sample_known(const Flac *f, const FrameHeader *h) {
    return h->variable || (f->info.min_block == f->info.max_block && f->info.max_block != 0);
}

static uint64_t frame_first(const Flac *f, const FrameHeader *h) {
    return h->variable ? h->number : h->number * f->info.max_block;
}

/* With no usable seek table: probes the file for frames (interpolating by sample count, then bisecting) to find one at or before
 * `target` that is not far from it. Returns the file offset and first sample of the frame (the start of the stream if it finds nothing better). */
static void search_start(Flac *f, uint64_t target, uint64_t *off, uint64_t *sample) {
    uint64_t lo_off = f->info.first_frame_offset, lo_pos = 0;
    uint64_t hi_off = f->io.size, hi_pos = f->info.total_samples;
    *off = lo_off;
    *sample = lo_pos;
    if (!hi_off || !hi_pos || hi_off <= lo_off || target >= hi_pos) return;
    for (int iter = 0; iter < 48 && hi_off - lo_off > 4096; iter++) {
        uint64_t guess;
        if (iter & 1) guess = lo_off + (hi_off - lo_off) / 2;
        else {
            double frac = hi_pos > lo_pos ? (double)(target - lo_pos) / (double)(hi_pos - lo_pos) : 0.5;
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            guess = lo_off + (uint64_t)(frac * (double)(hi_off - lo_off));
        }
        if (guess <= lo_off) guess = lo_off + 1;
        if (guess >= hi_off) guess = hi_off - 1;
        if (reposition(f, guess, 0)) return;
        FrameHeader h;
        size_t used = 0;
        int ev = 0;
        int rc = locate_frame(f, &h, &used, &ev, 1u << 20);
        if (rc < 0) return;
        uint64_t at = f->base + f->pos;
        if (rc == 0 || at >= hi_off) { hi_off = guess; continue; }              /* no frame starts between the guess and the upper bound */
        if (!sample_known(f, &h)) return;
        uint64_t first = frame_first(f, &h);
        if (first <= target && target < first + h.blocksize) { *off = at; *sample = first; return; }
        if (first < target) { lo_off = at; lo_pos = first; *off = at; *sample = first; }
        else { hi_off = at; hi_pos = first; }
    }
}

int flac_seek(Flac *f, uint64_t target) {
    if (!f) return FLAC_E_ARG;
    if (!f->io.seek) return FLAC_E_ARG;
    if (f->info.total_samples && target >= f->info.total_samples) {             /* the end: nothing to find */
        target = f->info.total_samples;
        f->blk_len = f->blk_pos = 0;
        f->silence_left = 0;
        f->pending = 0;
        f->ended = true;
        f->out_pos = f->frame_next = target;
        return 0;
    }

    uint64_t off = f->info.first_frame_offset, pos0 = 0;
    bool from_table = false;
    for (uint32_t i = 0; i < f->nseek; i++) {                                   /* the last real seek point at or before the target */
        const SeekPoint *sp = &f->seek[i];
        if (sp->sample == ~0ull || sp->sample > target || sp->sample < pos0) continue;
        if (f->info.first_frame_offset + sp->offset >= (f->io.size ? f->io.size : ~0ull)) continue;
        off = f->info.first_frame_offset + sp->offset;
        pos0 = sp->sample;
        from_table = true;
    }
    if (!from_table) search_start(f, target, &off, &pos0);

    for (int attempt = 0; attempt < 2; attempt++) {
        int rc = reposition(f, off, pos0);
        if (rc) return rc;
        uint64_t run = pos0;
        for (;;) {
            FrameHeader h;
            size_t used = 0;
            int ev = 0;
            rc = locate_frame(f, &h, &used, &ev, UINT64_MAX);
            if (rc < 0) { f->ended = true; return rc; }
            if (rc == 0 && (off != f->info.first_frame_offset || pos0 != 0)) {  /* no frame after the start point (a seek point that lies): begin again from the start of the stream */
                off = f->info.first_frame_offset;
                pos0 = 0;
                break;
            }
            if (rc == 0) {                                                      /* the target is past the last frame we can decode */
                f->ended = true;
                f->out_pos = target;
                f->frame_next = target;
                return 0;
            }
            uint64_t first = sample_known(f, &h) ? frame_first(f, &h) : run;
            if (first > target && (off != f->info.first_frame_offset || pos0 != 0)) {   /* the start point was past the target: begin again from the start of the stream */
                off = f->info.first_frame_offset;
                pos0 = 0;
                break;
            }
            f->pos += used;
            if (first + h.blocksize > target || first > target) {              /* this is the frame (or, in a stream that starts later than the target, the first one) */
                uint64_t skip = target > first ? target - first : 0;
                f->blk_len = h.blocksize;
                f->blk_pos = (uint32_t)skip;
                f->frame_next = first + h.blocksize;
                f->out_pos = first + skip;
                f->fail_end = 0;
                f->overlap = 0;
                return 0;
            }
            run = first + h.blocksize;
            f->frame_next = run;
        }
    }
    return FLAC_E_FRAME;
}

/* ---------------------------------------------------------------------------------------------------------- verify */

int flac_verify(const FlacIO *io, FlacVerify *v) {
    memset(v, 0, sizeof *v);
    int err = 0;
    Flac *f = flac_open(io, &err);
    if (!f) { v->first_error = err; return err; }
    const FlacInfo *in = flac_info(f);
    const uint32_t ch = in->channels, nb = (in->bits + 7) / 8;
    enum { CHUNK = 4096 };
    int32_t *pcm = malloc((size_t)CHUNK * ch * sizeof(int32_t));
    uint8_t *bytes = malloc((size_t)CHUNK * ch * 4);
    if (!pcm || !bytes) { free(pcm); free(bytes); flac_close(f); v->first_error = FLAC_E_NOMEM; return FLAC_E_NOMEM; }
    FlacMd5 md;
    flac_md5_init(&md);
    for (;;) {
        ssize_t n = flac_read(f, pcm, CHUNK, 0);
        if (n == 0) break;
        if (n < 0) {
            v->errors++;
            if (!v->first_error) v->first_error = (int)n;
            if (n == FLAC_E_IO || n == FLAC_E_NOMEM) break;
            continue;
        }
        size_t o = 0;                                                           /* the MD5 covers the samples as little-endian integers of ceil(bits / 8) bytes */
        for (size_t i = 0; i < (size_t)n * ch; i++) {
            uint32_t s = (uint32_t)pcm[i];
            for (uint32_t k = 0; k < nb; k++) bytes[o++] = (uint8_t)(s >> (8 * k));
        }
        flac_md5_update(&md, bytes, o);
        v->samples += (uint64_t)n;
    }
    flac_md5_final(&md, v->md5);
    v->md5_present = in->has_md5;
    v->md5_ok = in->has_md5 && !v->errors && !memcmp(v->md5, in->md5, 16);
    v->length_ok = in->total_samples == 0 || in->total_samples == v->samples;
    free(pcm);
    free(bytes);
    flac_close(f);
    if (v->errors) return v->first_error ? v->first_error : FLAC_E_FRAME;
    if (v->md5_present && !v->md5_ok) return FLAC_E_MD5;
    if (!v->length_ok) return FLAC_E_LENGTH;
    return 0;
}
