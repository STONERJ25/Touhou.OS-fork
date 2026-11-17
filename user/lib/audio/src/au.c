/*
 * au.c: Sun/NeXT ".snd" (".au") files, written from the format description in Sun's audio_filehdr.h documentation ("Audio File Formats") that every
 * description of the format repeats: a 24-byte big endian header, an annotation up to the data offset, then the samples (big endian).
 *
 *   magic ".snd", data offset (at least 24), data size (0xFFFFFFFF: not known, the data runs to the end of the file), encoding, sample rate, channels
 *   encodings: 1 mu-law 8-bit, 2 linear PCM 8-bit (signed), 3 16-bit, 4 24-bit, 5 32-bit, 6 float 32, 7 float 64, 27 A-law 8-bit
 */
#include "afile_priv.h"
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

int au_source_open(int fd, uint64_t size, AudioFileInfo *info, AfSource **out, char *err, size_t errsz) {
    uint8_t h[24];
    if (af_pread(fd, 0, h, 24) != 24) { af_say(err, errsz, "the file is too short"); return -EINVAL; }
    uint32_t off = af_be32(h + 4), dsize = af_be32(h + 8), enc = af_be32(h + 12), rate = af_be32(h + 16), channels = af_be32(h + 20);
    snprintf(info->format, sizeof info->format, "AU");
    if (off < 24 || off > size) { af_say(err, errsz, "a data offset of %u bytes", off); return -EINVAL; }
    if (channels < 1 || channels > AF_MAX_CHANNELS) { af_say(err, errsz, "%u channels are not supported (1 to %d)", channels, AF_MAX_CHANNELS); return -EINVAL; }
    if (rate == 0 || rate > 0x7FFFFFFFu) { af_say(err, errsz, "a sample rate of %u Hz", rate); return -EINVAL; }

    unsigned container, valid;
    AfCoding coding;
    const char *codec;
    switch (enc) {
    case 1:  container = 1; valid = 16; coding = AF_S_ULAW; codec = "mu-law"; break;
    case 27: container = 1; valid = 16; coding = AF_S_ALAW; codec = "A-law"; break;
    case 2:  container = 1; valid = 8;  coding = AF_S_PCM_BE; codec = "PCM"; break;                         /* signed, unlike WAVE */
    case 3:  container = 2; valid = 16; coding = AF_S_PCM_BE; codec = "PCM"; break;
    case 4:  container = 3; valid = 24; coding = AF_S_PCM_BE; codec = "PCM"; break;
    case 5:  container = 4; valid = 32; coding = AF_S_PCM_BE; codec = "PCM"; break;
    case 6:  container = 4; valid = 32; coding = AF_S_FLOAT_BE; codec = "IEEE float"; break;
    case 7:  container = 8; valid = 32; coding = AF_S_FLOAT_BE; codec = "IEEE float"; break;
    default: af_say(err, errsz, "AU encoding %u is not supported", enc); return -EINVAL;
    }
    uint64_t avail = size - off, len = dsize;
    if (dsize == 0xFFFFFFFFu || len > avail) len = avail;                                                  /* not known, or the file was cut short */
    const uint64_t frames = len / ((uint64_t)channels * container);

    snprintf(info->codec, sizeof info->codec, "%s", codec);
    info->rate = rate;
    info->channels = channels;
    info->bits = valid;
    info->frames = frames;
    info->length_known = true;
    AfSource *src = af_raw_source(fd, off, frames, channels, container, valid, coding);
    if (!src) return -ENOMEM;
    *out = src;
    return 0;
}
