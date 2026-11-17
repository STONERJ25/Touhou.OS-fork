/*
 * aiff.c: AIFF and AIFF-C files, written from Apple's "Audio Interchange File Format: AIFF" (1989) and the AIFF-C draft (1991).
 *
 * IFF chunks (a four-letter id, a big endian size, the data, padded to an even length) inside a FORM chunk of type AIFF or AIFC. COMM holds the channel
 * count, the number of sample frames, the sample size and the sample rate as an 80-bit IEEE extended number; for AIFF-C it adds a four-letter compression
 * type. SSND holds an offset and a block size and then the samples. Samples are big endian two's complement, left-justified in ceil(size / 8) bytes (so a
 * 12-bit sample is two bytes with the low four bits zero); 8-bit samples are signed (unlike WAVE).
 *
 *   NONE, twos, in24, in32   big endian PCM (1 to 32 bits)        sowt   little endian PCM
 *   raw                      unsigned 8-bit                        fl32, FL32, fl64, FL64   IEEE floating point
 *   ulaw, ULAW, alaw, ALAW   G.711, decoded to 16 bits
 * NAME and AUTH become the title and the artist.
 */
#include "afile_priv.h"
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

/* The 10-byte IEEE 754 extended number of AIFF's sample rate, as a whole number (rounded). 0 for anything that is not a sensible rate. */
static uint32_t extended_to_rate(const uint8_t *p) {
    unsigned se = af_be16(p);
    uint64_t mant = af_be64(p + 2);
    if (se & 0x8000) return 0;                                                  /* negative */
    int e = (int)(se & 0x7FFF) - 16383;                                         /* value = mant * 2^(e - 63) */
    if (e < 0 || e > 30 || !(mant >> 63)) return 0;                             /* below 1 Hz, above 2 GHz, or not normalized */
    unsigned shift = 63 - (unsigned)e;
    uint64_t v = (mant + ((uint64_t)1 << (shift - 1))) >> shift;
    return v > 0x7FFFFFFFu ? 0 : (uint32_t)v;
}

int aiff_source_open(int fd, uint64_t size, AudioFileInfo *info, AfSource **out, char *err, size_t errsz) {
    uint8_t h[12];
    if (af_pread(fd, 0, h, 12) != 12) { af_say(err, errsz, "the file is too short"); return -EINVAL; }
    const bool aifc = !memcmp(h + 8, "AIFC", 4);
    snprintf(info->format, sizeof info->format, "%s", aifc ? "AIFF-C" : "AIFF");

    bool have_comm = false, have_ssnd = false;
    unsigned channels = 0, bits = 0;
    uint32_t declared = 0, rate = 0;
    char ctype[5] = "NONE";
    uint64_t data_off = 0, data_len = 0;
    uint64_t pos = 12;
    for (int guard = 0; guard < 4096 && pos + 8 <= size; guard++) {
        uint8_t ch[8];
        if (af_pread(fd, pos, ch, 8) != 8) break;
        uint64_t sz = af_be32(ch + 4), body = pos + 8;
        if (!memcmp(ch, "COMM", 4) && sz >= 18 && !have_comm) {
            uint8_t c[24];
            size_t take = sz < sizeof c ? (size_t)sz : sizeof c;
            if (af_pread(fd, body, c, take) == (int)take) {
                channels = af_be16(c);
                declared = af_be32(c + 2);
                bits = af_be16(c + 6);
                rate = extended_to_rate(c + 8);
                if (aifc) {
                    if (take < 22) { af_say(err, errsz, "an AIFF-C COMM chunk without a compression type"); return -EINVAL; }
                    memcpy(ctype, c + 18, 4);
                    ctype[4] = 0;
                }
                have_comm = true;
            }
        } else if (!memcmp(ch, "SSND", 4) && !have_ssnd) {
            uint8_t s[8];
            if (sz >= 8 && af_pread(fd, body, s, 8) == 8) {
                uint64_t off = af_be32(s);
                data_off = body + 8 + off;
                uint64_t len = sz >= 8 + off ? sz - 8 - off : 0;
                if (data_off > size) len = 0;
                else if (data_off + len > size || sz == 0xFFFFFFFFull) len = size - data_off;                 /* cut short, or a size that was never filled in */
                data_len = len;
                have_ssnd = true;
            }
        } else if (!memcmp(ch, "NAME", 4) || !memcmp(ch, "AUTH", 4)) {
            uint8_t t[256];
            size_t take = sz < sizeof t ? (size_t)sz : sizeof t;
            if (body + take <= size && af_pread(fd, body, t, take) == (int)take)
                af_text(!memcmp(ch, "NAME", 4) ? info->title : info->artist, !memcmp(ch, "NAME", 4) ? sizeof info->title : sizeof info->artist, t, take);
        }
        if (body + sz > size) break;
        pos = body + sz + (sz & 1);
    }
    if (!have_comm) { af_say(err, errsz, "no COMM chunk"); return -EINVAL; }
    if (!have_ssnd) { af_say(err, errsz, "no SSND chunk"); return -EINVAL; }
    if (channels == 0 || channels > AF_MAX_CHANNELS) { af_say(err, errsz, "%u channels are not supported (1 to %d)", channels, AF_MAX_CHANNELS); return -EINVAL; }
    if (rate == 0) { af_say(err, errsz, "an unusable sample rate"); return -EINVAL; }

    AfCoding coding;
    unsigned container, valid;
    const char *codec;
    if (!strcmp(ctype, "NONE") || !strcmp(ctype, "twos") || !strcmp(ctype, "in24") || !strcmp(ctype, "in32") || !strcmp(ctype, "sowt")) {
        if (bits < 1 || bits > 32) { af_say(err, errsz, "a sample size of %u bits", bits); return -EINVAL; }
        container = (bits + 7) / 8;
        valid = bits;
        coding = !strcmp(ctype, "sowt") ? AF_S_PCM_LE : AF_S_PCM_BE;
        codec = "PCM";
    } else if (!strcmp(ctype, "raw ")) {
        container = 1; valid = 8; coding = AF_S_U8; codec = "PCM";
    } else if (!strcmp(ctype, "fl32") || !strcmp(ctype, "FL32")) {
        container = 4; valid = 32; coding = AF_S_FLOAT_BE; codec = "IEEE float";
    } else if (!strcmp(ctype, "fl64") || !strcmp(ctype, "FL64")) {
        container = 8; valid = 32; coding = AF_S_FLOAT_BE; codec = "IEEE float";
    } else if (!strcmp(ctype, "ulaw") || !strcmp(ctype, "ULAW")) {
        container = 1; valid = 16; coding = AF_S_ULAW; codec = "mu-law";
    } else if (!strcmp(ctype, "alaw") || !strcmp(ctype, "ALAW")) {
        container = 1; valid = 16; coding = AF_S_ALAW; codec = "A-law";
    } else {
        af_say(err, errsz, "AIFF-C compression '%s' is not supported", ctype);
        return -EINVAL;
    }
    const uint64_t frame_bytes = (uint64_t)channels * container;
    uint64_t frames = data_len / frame_bytes;
    if (declared && declared < frames) frames = declared;                                                   /* more data than the header counts: the header wins */

    snprintf(info->codec, sizeof info->codec, "%s", codec);
    info->rate = rate;
    info->channels = channels;
    info->bits = valid;
    info->frames = frames;
    info->length_known = true;
    AfSource *src = af_raw_source(fd, data_off, frames, channels, container, valid, coding);
    if (!src) return -ENOMEM;
    *out = src;
    return 0;
}
