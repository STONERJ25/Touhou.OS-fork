/*
 * wavext.c: the WAVE files of every kind libaudio plays (the older, plain-PCM reader is wav.c, which the Jukebox and `play` used before and which stays).
 * Written from Microsoft's multimedia documents (RIFF, WAVEFORMATEX, WAVEFORMATEXTENSIBLE, the IMA ADPCM and Microsoft ADPCM block formats of the
 * "Multimedia Standards Update"), ITU-T G.711 for A-law and mu-law, the EBU's Tech 3306 (RF64) and ITU-R BS.2088 (BW64) for the 64-bit sizes.
 *
 *   WAVE_FORMAT_PCM        8 (unsigned), 16, 24 and 32 bits; containers wider than the significant bits (12 bits in 16, 20 or 24 in 32) are shifted back
 *   WAVE_FORMAT_IEEE_FLOAT 32 and 64 bits
 *   WAVE_FORMAT_ALAW / _MULAW, WAVE_FORMAT_IMA_ADPCM, WAVE_FORMAT_ADPCM (Microsoft)
 *   WAVE_FORMAT_EXTENSIBLE with the sub formats above, the speaker mask and the valid bits
 *   RF64 and BW64 (a ds64 chunk carries the 64-bit sizes; a data chunk of size 0xFFFFFFFF takes its size from there)
 *   a data chunk of size 0 or 0xFFFFFFFF in a plain RIFF file is a stream that was being written: the data is whatever follows, up to the end of the file
 *   chunks padded to an even length, LIST/INFO tags (INAM, IART, IPRD, ITRK or IPRT, ICRD), the fact chunk (the real length of an ADPCM stream)
 *
 * The ADPCM decoders work a block at a time, so seeking is by block. Microsoft ADPCM's predictor is (sample1 * coef1 + sample2 * coef2) >> 8: an arithmetic
 * shift, which rounds toward minus infinity. (The specification's pseudo code writes "/ 256"; read as C division, which rounds toward zero, it gives a different
 * sample for a quarter of the samples of a file. Windows' own codec, reached through Media Foundation, decodes mono and stereo test files exactly as the shift
 * does: tests/host/flac_reference_check.py.)
 */
#include "afile_priv.h"
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

enum { WT_PCM = 1, WT_MSADPCM = 2, WT_FLOAT = 3, WT_ALAW = 6, WT_ULAW = 7, WT_IMA = 0x11, WT_EXT = 0xFFFE };

static const char *tag_name(unsigned tag) {
    switch (tag) {
    case 0x0010: return "OKI ADPCM";
    case 0x0031: return "GSM 6.10";
    case 0x0040: return "G.721 ADPCM";
    case 0x0050: return "MPEG layer 1/2";
    case 0x0055: return "MP3";
    case 0x00FF: case 0x1610: return "AAC";
    case 0x0160: case 0x0161: case 0x0162: case 0x0163: return "Windows Media Audio";
    case 0x2000: return "AC-3";
    case 0xF1AC: return "FLAC";
    default: return "an unknown codec";
    }
}

typedef struct {
    unsigned tag, channels, block_align, bits, valid, cb_size;
    uint32_t rate, mask;
    unsigned spb;
    unsigned ncoef;
    int16_t coef[32][2];
    bool have;
} WavFmt;

/* --------------------------------------------------------------------------------------------------------------- ADPCM */

typedef struct {
    AfSource src;
    int fd;
    uint64_t data_off, data_bytes, frames, pos, cached;
    unsigned channels, block_align, spb;
    bool ms;
    unsigned ncoef;
    int16_t coef[32][2];
    uint8_t *blk;
    int32_t *pcm;
    unsigned blk_len;
} Adpcm;

static const int16_t ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t ima_adjust[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

static int ima_nibble(int *pred, int *index, unsigned nib) {
    int step = ima_step[*index];
    int diff = step >> 3;
    if (nib & 4) diff += step;
    if (nib & 2) diff += step >> 1;
    if (nib & 1) diff += step >> 2;
    int p = *pred + ((nib & 8) ? -diff : diff);
    if (p > 32767) p = 32767;
    if (p < -32768) p = -32768;
    *pred = p;
    int idx = *index + ima_adjust[nib & 7];
    *index = idx < 0 ? 0 : idx > 88 ? 88 : idx;
    return p;
}

/* One IMA ADPCM block of `avail` bytes: a 4-byte header per channel (the first sample and the step index), then 4-bit codes, 8 samples of one channel (4 bytes)
 * at a time, the channels taking turns, the earlier sample in the low nibble. Returns the number of frames. */
static unsigned ima_block(const Adpcm *a, const uint8_t *b, size_t avail, int32_t *out) {
    const unsigned ch = a->channels;
    if (avail < 4u * ch) return 0;
    int pred[AF_MAX_CHANNELS], index[AF_MAX_CHANNELS];
    for (unsigned c = 0; c < ch; c++) {
        pred[c] = (int16_t)af_le16(b + 4 * c);
        index[c] = b[4 * c + 2] > 88 ? 88 : b[4 * c + 2];
        out[c] = pred[c];
    }
    const uint8_t *d = b + 4 * ch;
    unsigned groups = (unsigned)((avail - 4u * ch) / (4u * ch));
    if (groups * 8 + 1 > a->spb) groups = (a->spb - 1) / 8;
    for (unsigned g = 0; g < groups; g++)
        for (unsigned c = 0; c < ch; c++) {
            const uint8_t *q = d + ((size_t)g * ch + c) * 4;
            for (unsigned j = 0; j < 8; j++) {
                unsigned nib = (j & 1) ? q[j >> 1] >> 4 : q[j >> 1] & 15;
                out[(size_t)(1 + g * 8 + j) * ch + c] = ima_nibble(&pred[c], &index[c], nib);
            }
        }
    return 1 + groups * 8;
}

static const int ms_adapt[16] = { 230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230 };

/* One Microsoft ADPCM block: per channel a predictor index (1 byte each, all together), a delta, the newest and the one before (16 bits each, grouped the
 * same way), then 4-bit codes, the high nibble first, the channels taking turns. The first two frames are the two samples of the header (older first).
 * Returns the number of frames, or 0 when the header is not valid. */
static unsigned ms_block(const Adpcm *a, const uint8_t *b, size_t avail, int32_t *out) {
    const unsigned ch = a->channels;
    if (avail < 7u * ch) return 0;
    int pi[AF_MAX_CHANNELS], delta[AF_MAX_CHANNELS], s1[AF_MAX_CHANNELS], s2[AF_MAX_CHANNELS];
    for (unsigned c = 0; c < ch; c++) {
        pi[c] = b[c];
        if ((unsigned)pi[c] >= a->ncoef) return 0;
        delta[c] = (int16_t)af_le16(b + ch + 2 * c);
        s1[c] = (int16_t)af_le16(b + 3 * ch + 2 * c);
        s2[c] = (int16_t)af_le16(b + 5 * ch + 2 * c);
        out[c] = s2[c];
        out[ch + c] = s1[c];
    }
    const uint8_t *d = b + 7 * ch;
    size_t nibbles = (avail - 7u * ch) * 2;
    size_t frames = nibbles / ch;
    if (frames + 2 > a->spb) frames = a->spb - 2;
    size_t n = 0;
    for (size_t f = 0; f < frames; f++)
        for (unsigned c = 0; c < ch; c++, n++) {
            unsigned nib = (n & 1) ? d[n >> 1] & 15 : d[n >> 1] >> 4;
            int64_t num = (int64_t)s1[c] * a->coef[pi[c]][0] + (int64_t)s2[c] * a->coef[pi[c]][1];   /* two 32-bit products can add up to 2^31 */
            int64_t pred = num >> 8;                                                    /* an arithmetic shift: toward minus infinity (not C's division) */
            int err = (nib & 8) ? (int)nib - 16 : (int)nib;
            int64_t v = pred + err * (int64_t)delta[c];
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            out[(f + 2) * ch + c] = (int32_t)v;
            s2[c] = s1[c];
            s1[c] = (int)v;
            int64_t nd = ((int64_t)ms_adapt[nib] * delta[c]) >> 8;
            delta[c] = nd < 16 ? 16 : nd > 0x7FFFFF ? 0x7FFFFF : (int)nd;               /* the step never gets out of hand (a damaged block would otherwise overflow it) */
        }
    return (unsigned)(frames + 2);
}

static ssize_t adpcm_read(AfSource *as, int32_t *out, size_t frames) {
    Adpcm *a = (Adpcm *)as;
    const unsigned ch = a->channels;
    size_t done = 0;
    while (done < frames && a->pos < a->frames) {
        uint64_t blk = a->pos / a->spb;
        unsigned within = (unsigned)(a->pos % a->spb);
        if (a->cached != blk) {
            uint64_t off = blk * a->block_align;
            size_t want = a->block_align;
            if (off + want > a->data_bytes) want = (size_t)(a->data_bytes - off);
            int got = af_pread(a->fd, a->data_off + off, a->blk, want);
            a->cached = blk;
            a->blk_len = got > 0 ? (a->ms ? ms_block(a, a->blk, (size_t)got, a->pcm) : ima_block(a, a->blk, (size_t)got, a->pcm)) : 0;
            if (a->blk_len == 0) {                                                      /* a block we cannot decode: silence for its length, reported once */
                unsigned len = a->spb;
                if (len > a->frames - blk * a->spb) len = (unsigned)(a->frames - blk * a->spb);
                memset(a->pcm, 0, (size_t)len * ch * sizeof(int32_t));
                a->blk_len = len;
                if (done) { a->cached = ~0ull; return (ssize_t)done; }                 /* report it on the next call, after what was decoded */
                snprintf(as->message, sizeof as->message, "a damaged ADPCM block near frame %llu", (unsigned long long)(blk * a->spb));
                as->damaged++;
                return -EIO;
            }
        }
        if (within >= a->blk_len) {                                                     /* a short last block: the file ends here */
            a->frames = a->pos;
            break;
        }
        size_t n = a->blk_len - within;
        if (n > frames - done) n = frames - done;
        if (n > a->frames - a->pos) n = (size_t)(a->frames - a->pos);
        memcpy(out + done * ch, a->pcm + (size_t)within * ch, n * ch * sizeof(int32_t));
        done += n;
        a->pos += n;
    }
    return (ssize_t)done;
}

static int adpcm_seek(AfSource *as, uint64_t frame) {
    Adpcm *a = (Adpcm *)as;
    a->pos = frame > a->frames ? a->frames : frame;
    return 0;
}

static void adpcm_close(AfSource *as) {
    Adpcm *a = (Adpcm *)as;
    if (a->fd >= 0) close(a->fd);
    free(a->blk);
    free(a->pcm);
    free(a);
}

/* ------------------------------------------------------------------------------------------------------------ the file */

typedef struct { uint64_t riff, data, samples; bool have; } Ds64;

static void info_tag(AudioFileInfo *info, const uint8_t *id, const uint8_t *p, size_t n) {
    if (!memcmp(id, "INAM", 4)) af_text(info->title, sizeof info->title, p, n);
    else if (!memcmp(id, "IART", 4)) af_text(info->artist, sizeof info->artist, p, n);
    else if (!memcmp(id, "IPRD", 4)) af_text(info->album, sizeof info->album, p, n);
    else if (!memcmp(id, "ITRK", 4) || (!memcmp(id, "IPRT", 4) && !info->track[0])) af_text(info->track, sizeof info->track, p, n);
    else if (!memcmp(id, "ICRD", 4)) af_text(info->date, sizeof info->date, p, n);
}

/* A LIST chunk of type INFO: sub chunks, each an id, a size, text, padded to an even length. */
static void read_list(int fd, uint64_t body, uint64_t len, AudioFileInfo *info) {
    if (len < 12 || len > (1u << 20)) return;
    uint8_t *buf = malloc((size_t)len);
    if (!buf) return;
    if (af_pread(fd, body, buf, (size_t)len) == (int)len && !memcmp(buf, "INFO", 4)) {
        size_t at = 4;
        while (at + 8 <= len) {
            uint32_t sz = af_le32(buf + at + 4);
            if (sz > len - at - 8) break;
            info_tag(info, buf + at, buf + at + 8, sz);
            at += 8 + (size_t)sz + (sz & 1);
        }
    }
    free(buf);
}

int wav_source_open(int fd, uint64_t size, AudioFileInfo *info, AfSource **out, char *err, size_t errsz) {
    uint8_t h[12];
    if (af_pread(fd, 0, h, 12) != 12) { af_say(err, errsz, "the file is too short"); return -EINVAL; }
    const bool rf64 = !memcmp(h, "RF64", 4) || !memcmp(h, "BW64", 4);
    snprintf(info->format, sizeof info->format, "%s", rf64 ? (h[0] == 'R' ? "RF64" : "BW64") : "WAV");
    info->big_file = rf64;

    WavFmt fmt;
    memset(&fmt, 0, sizeof fmt);
    Ds64 ds = { 0, 0, 0, false };
    uint64_t data_off = 0, data_bytes = 0, fact = 0;
    bool have_data = false, have_fact = false;
    uint64_t pos = 12;
    for (int guard = 0; guard < 4096 && pos + 8 <= size; guard++) {
        uint8_t ch[8];
        if (af_pread(fd, pos, ch, 8) != 8) break;
        uint64_t sz = af_le32(ch + 4), body = pos + 8;
        if (!memcmp(ch, "data", 4)) {
            bool unknown = false;
            if (rf64 && sz == 0xFFFFFFFFull && ds.have) sz = ds.data;
            else if (sz == 0 || sz == 0xFFFFFFFFull) { sz = size - body; unknown = true; }                  /* a stream that was being written */
            if (body + sz > size) { sz = size - body; unknown = true; }                                    /* a file that was cut short */
            if (!have_data) { data_off = body; data_bytes = sz; have_data = true; }
            if (unknown) break;
        } else if (!memcmp(ch, "fmt ", 4)) {
            uint8_t f[1024];
            size_t take = sz < sizeof f ? (size_t)sz : sizeof f;
            if (body + take > size) take = (size_t)(size - body);
            if (take >= 16 && !fmt.have && af_pread(fd, body, f, take) == (int)take) {
                fmt.have = true;
                fmt.tag = af_le16(f);
                fmt.channels = af_le16(f + 2);
                fmt.rate = af_le32(f + 4);
                fmt.block_align = af_le16(f + 12);
                fmt.bits = af_le16(f + 14);
                fmt.valid = fmt.bits;
                fmt.cb_size = take >= 18 ? af_le16(f + 16) : 0;
                if (fmt.cb_size > take - 18 && take >= 18) fmt.cb_size = (unsigned)(take - 18);
                const uint8_t *e = f + 18;
                if (fmt.tag == WT_EXT) {
                    if (fmt.cb_size < 22) { af_say(err, errsz, "a WAVE_FORMAT_EXTENSIBLE header that is too short"); return -EINVAL; }
                    fmt.valid = af_le16(e);
                    fmt.mask = af_le32(e + 2);
                    static const uint8_t base[14] = { 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71 };
                    if (memcmp(e + 8, base, 14)) { af_say(err, errsz, "a WAVE_FORMAT_EXTENSIBLE sub format that is not a known codec"); return -EINVAL; }
                    fmt.tag = af_le16(e + 6);
                    fmt.cb_size = 0;                                                                        /* the codec's own extra bytes are not there */
                } else if (fmt.tag == WT_IMA && fmt.cb_size >= 2) {
                    fmt.spb = af_le16(e);
                } else if (fmt.tag == WT_MSADPCM && fmt.cb_size >= 4) {
                    fmt.spb = af_le16(e);
                    fmt.ncoef = af_le16(e + 2);
                    if (fmt.ncoef > 32 || (size_t)fmt.cb_size < 4 + 4u * fmt.ncoef) { af_say(err, errsz, "a Microsoft ADPCM header with %u coefficient pairs", fmt.ncoef); return -EINVAL; }
                    for (unsigned i = 0; i < fmt.ncoef; i++) {
                        fmt.coef[i][0] = (int16_t)af_le16(e + 4 + 4 * i);
                        fmt.coef[i][1] = (int16_t)af_le16(e + 6 + 4 * i);
                    }
                }
            }
        } else if (!memcmp(ch, "ds64", 4)) {
            uint8_t d[24];
            if (sz >= 24 && af_pread(fd, body, d, 24) == 24) { ds.riff = af_le64(d); ds.data = af_le64(d + 8); ds.samples = af_le64(d + 16); ds.have = true; }
        } else if (!memcmp(ch, "fact", 4)) {
            uint8_t d[4];
            if (sz >= 4 && af_pread(fd, body, d, 4) == 4) { fact = af_le32(d); have_fact = true; }
        } else if (!memcmp(ch, "LIST", 4)) {
            if (body + sz <= size) read_list(fd, body, sz, info);
        }
        if (body + sz > size) break;
        pos = body + sz + (sz & 1);
    }
    if (!fmt.have) { af_say(err, errsz, "no fmt chunk"); return -EINVAL; }
    if (!have_data) { af_say(err, errsz, "no data chunk"); return -EINVAL; }
    if (fmt.channels == 0 || fmt.channels > AF_MAX_CHANNELS) { af_say(err, errsz, "%u channels are not supported (1 to %d)", fmt.channels, AF_MAX_CHANNELS); return -EINVAL; }
    if (fmt.rate == 0 || fmt.rate > 0x7FFFFFFFu) { af_say(err, errsz, "a sample rate of %u Hz", fmt.rate); return -EINVAL; }

    info->rate = fmt.rate;
    info->channels = fmt.channels;
    info->channel_mask = fmt.mask && (unsigned)__builtin_popcount(fmt.mask) == fmt.channels ? fmt.mask : 0;
    const unsigned ch = fmt.channels;

    if (fmt.tag == WT_IMA || fmt.tag == WT_MSADPCM) {
        const bool ms = fmt.tag == WT_MSADPCM;
        if (fmt.bits != 4) { af_say(err, errsz, "%s with %u bits per sample", ms ? "Microsoft ADPCM" : "IMA ADPCM", fmt.bits); return -EINVAL; }
        const unsigned header = (ms ? 7u : 4u) * ch;
        if (fmt.block_align < header + 1 || fmt.block_align > 65535 || (ms && fmt.ncoef == 0)) { af_say(err, errsz, "an ADPCM block size of %u bytes", fmt.block_align); return -EINVAL; }
        unsigned spb = ms ? (fmt.block_align - header) * 2 / ch + 2 : (fmt.block_align - header) / (4 * ch) * 8 + 1;
        if (fmt.spb && fmt.spb < spb) spb = fmt.spb;                                                         /* the header may promise fewer, never more */
        if (spb < 2) { af_say(err, errsz, "an ADPCM block that holds no samples"); return -EINVAL; }
        Adpcm *a = calloc(1, sizeof *a);
        if (!a) return -ENOMEM;
        a->blk = malloc(fmt.block_align);
        a->pcm = malloc((size_t)spb * ch * sizeof(int32_t));
        if (!a->blk || !a->pcm) { free(a->blk); free(a->pcm); free(a); return -ENOMEM; }
        a->fd = fd; a->data_off = data_off; a->data_bytes = data_bytes; a->channels = ch; a->block_align = fmt.block_align; a->spb = spb; a->ms = ms;
        a->ncoef = fmt.ncoef; memcpy(a->coef, fmt.coef, sizeof a->coef); a->cached = ~0ull;
        uint64_t full = data_bytes / fmt.block_align, rest = data_bytes % fmt.block_align, frames = full * spb;
        if (rest >= header + (ms ? 1u : 4u * ch)) {                                                          /* a short last block */
            frames += ms ? 2 + (rest - header) * 2 / ch : 1 + (rest - header) / (4 * ch) * 8;
            if (frames > (full + 1) * spb) frames = (full + 1) * spb;
        }
        if (have_fact && fact < frames) frames = fact;                                                       /* the encoder padded the last block */
        a->frames = frames;
        a->src.read = adpcm_read; a->src.seek = adpcm_seek; a->src.close = adpcm_close;
        snprintf(info->codec, sizeof info->codec, "%s", ms ? "MS ADPCM" : "IMA ADPCM");
        info->bits = 16;
        info->frames = frames;
        info->length_known = true;
        *out = &a->src;
        return 0;
    }

    unsigned container, valid;
    AfCoding coding;
    switch (fmt.tag) {
    case WT_PCM:
        container = fmt.block_align / ch;
        if (fmt.block_align % ch || container < 1 || container > 4) { af_say(err, errsz, "PCM with %u bytes per frame for %u channels", fmt.block_align, ch); return -EINVAL; }
        valid = fmt.valid ? fmt.valid : container * 8;
        if (valid > container * 8) valid = container * 8;
        if (valid < 1) valid = container * 8;
        coding = container == 1 ? AF_S_U8 : AF_S_PCM_LE;                                                     /* 8-bit WAVE is unsigned, the wider ones signed */
        snprintf(info->codec, sizeof info->codec, "PCM");
        break;
    case WT_FLOAT:
        container = fmt.block_align / ch;
        if (fmt.block_align % ch || (container != 4 && container != 8)) { af_say(err, errsz, "floating point with %u bytes per sample", fmt.block_align / ch); return -EINVAL; }
        valid = 32;
        coding = AF_S_FLOAT_LE;
        snprintf(info->codec, sizeof info->codec, "IEEE float");
        break;
    case WT_ALAW:
    case WT_ULAW:
        container = 1;
        if (fmt.block_align != ch) { af_say(err, errsz, "G.711 with %u bytes per frame", fmt.block_align); return -EINVAL; }
        valid = 16;
        coding = fmt.tag == WT_ALAW ? AF_S_ALAW : AF_S_ULAW;
        snprintf(info->codec, sizeof info->codec, "%s", fmt.tag == WT_ALAW ? "A-law" : "mu-law");
        break;
    default:
        af_say(err, errsz, "WAVE format 0x%04X (%s) is not supported", fmt.tag, tag_name(fmt.tag));
        return -EINVAL;
    }
    info->bits = valid;
    const uint64_t frames = data_bytes / ((uint64_t)ch * container);
    info->frames = frames;
    info->length_known = true;
    AfSource *src = af_raw_source(fd, data_off, frames, ch, container, valid, coding);
    if (!src) return -ENOMEM;
    *out = src;
    return 0;
}
