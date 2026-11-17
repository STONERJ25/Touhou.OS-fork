/*
 * afile.c: the front of libaudio's file readers (see afile.h): finds the kind of a file by its first bytes, opens it, turns whatever it
 * decodes into the samples the sound device takes, mixes many channels down to two, and renders a file to a WAV.
 *
 * The formats themselves are in flac.c (FLAC, RFC 9639), wavext.c (RIFF/WAVE, RF64, BW64), aiff.c (AIFF, AIFF-C) and au.c (Sun .au); the
 * G.711 expansions (ITU-T G.711, the A-law and mu-law tables as algorithms) are here because three of them need them.
 */
#include "afile_priv.h"
#include <flac.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------------------------------------------------ helpers */

uint32_t af_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint32_t af_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t af_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint16_t af_le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
uint64_t af_le64(const uint8_t *p) { return (uint64_t)af_le32(p) | (uint64_t)af_le32(p + 4) << 32; }
uint64_t af_be64(const uint8_t *p) { return (uint64_t)af_be32(p) << 32 | af_be32(p + 4); }

int af_pread(int fd, uint64_t off, void *buf, size_t n) {
    if (lseek(fd, (off_t)off, SEEK_SET) < 0) return -errno;
    uint8_t *p = buf;
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, p + done, n - done);
        if (r < 0) { if (errno == EINTR) continue; return -errno; }
        if (r == 0) break;
        done += (size_t)r;
    }
    return (int)done;
}

void af_say(char *err, size_t errsz, const char *fmt, ...) {
    if (!err || !errsz) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

/* Tag text: valid UTF-8 is kept, anything else is taken for Latin-1 and converted; control characters become spaces; cut at a character. */
void af_text(char *dst, size_t cap, const uint8_t *src, size_t n) {
    size_t len = 0;
    while (len < n && src[len]) len++;                                          /* up to the first NUL */
    bool utf8 = true;
    for (size_t i = 0; i < len && utf8;) {
        unsigned c = src[i], k = c < 0x80 ? 1 : c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        if (!k || i + k > len) { utf8 = false; break; }
        for (unsigned j = 1; j < k; j++) if ((src[i + j] & 0xC0) != 0x80) utf8 = false;
        i += k;
    }
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        unsigned c = src[i], k = 1;
        if (utf8 && c >= 0x80) k = c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (!utf8 && c >= 0x80) {                                               /* Latin-1 to UTF-8 */
            if (o + 2 >= cap) break;
            dst[o++] = (char)(0xC0 | (c >> 6));
            dst[o++] = (char)(0x80 | (c & 0x3F));
            i++;
            continue;
        }
        if (o + k >= cap) break;
        if (k == 1 && (c < 0x20 || c == 0x7F)) dst[o++] = ' ';
        else { memcpy(dst + o, src + i, k); o += k; }
        i += k;
    }
    while (o > 0 && dst[o - 1] == ' ') o--;                                     /* a trailing blank from a padded field */
    dst[o] = 0;
}

/* ITU-T G.711, 16-bit scaled (what Sun's g711 and Python's audioop give: the largest magnitudes are 32124 and 32256). */
int af_ulaw(uint8_t b) {
    b = (uint8_t)~b;
    int t = (((b & 0x0F) << 3) + 0x84) << ((b & 0x70) >> 4);
    return (b & 0x80) ? 0x84 - t : t - 0x84;
}

int af_alaw(uint8_t b) {
    b ^= 0x55;
    int t = (b & 0x0F) << 4;
    int seg = (b & 0x70) >> 4;
    if (seg == 0) t += 8;
    else { t += 0x108; t <<= seg - 1; }
    return (b & 0x80) ? t : -t;
}

/* A floating point sample (-1 to 1) as a 32-bit integer: rounded to the nearest, clipped, NaN is silence. */
static int32_t float_to_s32(double x) {
    if (x != x) return 0;
    double t = x * 2147483648.0 + 0.5;
    if (t >= 2147483647.0) return INT32_MAX;
    if (t <= -2147483648.0) return INT32_MIN;
    int64_t i = (int64_t)t;
    if ((double)i > t) i--;                                                     /* floor */
    return (int32_t)i;
}

/* ------------------------------------------------------------------------------------------------------- raw samples */

typedef struct {
    AfSource src;
    int fd;
    uint64_t off, frames, pos;
    unsigned channels, container, valid, frame_bytes;
    AfCoding coding;
    uint8_t buf[32768];
} RawSource;

static void raw_convert(const RawSource *s, const uint8_t *p, size_t n, int32_t *out) {
    const unsigned c = s->container;
    switch (s->coding) {
    case AF_S_U8: {                                                             /* offset binary; fewer than 8 significant bits are the high ones */
        const unsigned drop = 8 - s->valid;
        for (size_t i = 0; i < n; i++) out[i] = ((int32_t)p[i] - 128) >> drop;
        break;
    }
    case AF_S_ULAW:
        for (size_t i = 0; i < n; i++) out[i] = af_ulaw(p[i]);
        break;
    case AF_S_ALAW:
        for (size_t i = 0; i < n; i++) out[i] = af_alaw(p[i]);
        break;
    case AF_S_FLOAT_LE:
    case AF_S_FLOAT_BE:
        for (size_t i = 0; i < n; i++, p += c) {
            uint8_t b[8];
            for (unsigned k = 0; k < c; k++) b[k] = s->coding == AF_S_FLOAT_LE ? p[k] : p[c - 1 - k];     /* to the machine's order (little endian) */
            if (c == 4) { float f; memcpy(&f, b, 4); out[i] = float_to_s32((double)f); }
            else { double d; memcpy(&d, b, 8); out[i] = float_to_s32(d); }
        }
        break;
    default: {                                                                  /* two's complement integers, left-justified in their bytes */
        const unsigned drop = c * 8 - s->valid;
        const bool be = s->coding == AF_S_PCM_BE;
        for (size_t i = 0; i < n; i++, p += c) {
            uint32_t u = 0;
            for (unsigned k = 0; k < c; k++) u = u << 8 | p[be ? k : c - 1 - k];
            int64_t v = (int64_t)u;
            if (c < 4 && (u & (1u << (c * 8 - 1)))) v -= (int64_t)1 << (c * 8);
            else if (c == 4) v = (int32_t)u;
            out[i] = (int32_t)(v >> drop);
        }
        break;
    }
    }
}

static ssize_t raw_read(AfSource *as, int32_t *out, size_t frames) {
    RawSource *s = (RawSource *)as;
    if (s->pos >= s->frames) return 0;
    size_t want = sizeof s->buf / s->frame_bytes;
    if (want > frames) want = frames;
    if (want > s->frames - s->pos) want = (size_t)(s->frames - s->pos);
    int got = af_pread(s->fd, s->off + s->pos * s->frame_bytes, s->buf, want * s->frame_bytes);
    if (got < 0) { snprintf(as->message, sizeof as->message, "read error: %s", strerror(-got)); as->damaged++; s->frames = s->pos; return -EIO; }
    size_t n = (size_t)got / s->frame_bytes;
    if (n == 0) { s->frames = s->pos; return 0; }                               /* the file is shorter than its header says */
    raw_convert(s, s->buf, n * s->channels, out);
    s->pos += n;
    return (ssize_t)n;
}

static int raw_seek(AfSource *as, uint64_t frame) {
    RawSource *s = (RawSource *)as;
    s->pos = frame > s->frames ? s->frames : frame;
    return 0;
}

static void raw_close(AfSource *as) {
    RawSource *s = (RawSource *)as;
    if (s->fd >= 0) close(s->fd);
    free(s);
}

AfSource *af_raw_source(int fd, uint64_t off, uint64_t frames, unsigned channels, unsigned container, unsigned valid, AfCoding coding) {
    RawSource *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->src.read = raw_read;
    s->src.seek = raw_seek;
    s->src.close = raw_close;
    s->fd = fd;
    s->off = off;
    s->frames = frames;
    s->channels = channels;
    s->container = container;
    s->valid = valid;
    s->coding = coding;
    s->frame_bytes = channels * container;
    return &s->src;
}

/* ------------------------------------------------------------------------------------------------------------- FLAC */

typedef struct {
    AfSource src;
    Flac *f;
    int fd;
} FlacSource;

static ssize_t flac_io_read(void *ctx, void *buf, size_t n) {
    ssize_t r;
    do r = read(*(int *)ctx, buf, n); while (r < 0 && errno == EINTR);
    return r;
}

static int flac_io_seek(void *ctx, uint64_t off) { return lseek(*(int *)ctx, (off_t)off, SEEK_SET) < 0 ? -1 : 0; }

static ssize_t flac_src_read(AfSource *as, int32_t *out, size_t frames) {
    FlacSource *s = (FlacSource *)as;
    uint64_t at = flac_tell(s->f);
    ssize_t n = flac_read(s->f, out, frames, 0);
    if (n < 0) {
        const FlacInfo *in = flac_info(s->f);
        unsigned sec = in->rate ? (unsigned)(at / in->rate) : 0;
        snprintf(as->message, sizeof as->message, "%s (near %u:%02u)", flac_strerror((int)n), sec / 60, sec % 60);
        as->damaged++;
        return -EIO;
    }
    return n;
}

static int flac_src_seek(AfSource *as, uint64_t frame) {
    FlacSource *s = (FlacSource *)as;
    int rc = flac_seek(s->f, frame);
    return rc ? -EIO : 0;
}

static uint64_t flac_src_concealed(const AfSource *as) { return flac_concealed(((const FlacSource *)as)->f); }

static void flac_src_close(AfSource *as) {
    FlacSource *s = (FlacSource *)as;
    flac_close(s->f);
    if (s->fd >= 0) close(s->fd);
    free(s);
}

static int flac_source_open(int fd, uint64_t size, AudioFileInfo *info, AfSource **out, char *err, size_t errsz) {
    FlacSource *s = calloc(1, sizeof *s);
    if (!s) return -ENOMEM;
    s->fd = fd;
    FlacIO io = { &s->fd, flac_io_read, flac_io_seek, size };
    int rc;
    lseek(fd, 0, SEEK_SET);
    s->f = flac_open(&io, &rc);
    if (!s->f) {
        af_say(err, errsz, "%s", flac_strerror(rc));
        s->fd = -1;                                                             /* the caller closes fd on failure */
        free(s);
        return rc == FLAC_E_NOMEM ? -ENOMEM : -EINVAL;
    }
    const FlacInfo *in = flac_info(s->f);
    snprintf(info->format, sizeof info->format, "FLAC");
    snprintf(info->codec, sizeof info->codec, "FLAC");
    info->rate = in->rate;
    info->channels = in->channels;
    info->bits = in->bits;
    info->channel_mask = in->channel_mask;
    info->frames = in->total_samples;
    info->length_known = in->total_samples != 0;
    snprintf(info->title, sizeof info->title, "%s", in->tags.title);
    snprintf(info->artist, sizeof info->artist, "%s", in->tags.artist);
    snprintf(info->album, sizeof info->album, "%s", in->tags.album);
    snprintf(info->track, sizeof info->track, "%s", in->tags.tracknumber);
    snprintf(info->date, sizeof info->date, "%s", in->tags.date);
    s->src.read = flac_src_read;
    s->src.seek = flac_src_seek;
    s->src.close = flac_src_close;
    s->src.concealed = flac_src_concealed;
    *out = &s->src;
    return 0;
}

/* ---------------------------------------------------------------------------------------------------------- the front */

#define TMP_FRAMES 2048

struct AudioFile {
    AudioFileInfo info;
    AfSource *src;
    uint64_t pos;                           /* the frame afile_read will deliver next */
    unsigned container;                     /* bits of a delivered sample: 8, 16, 24 or 32 */
    int32_t *tmp;                           /* decoded frames, in the channels of the file */
    bool downmix;
    int32_t mix[2][AF_MAX_CHANNELS];        /* Q16 gains of each channel into left and right */
    ssize_t pending;                        /* a damage report waiting for its turn */
    char message[100];
};

/* WAVEFORMATEXTENSIBLE speaker positions, in bit order: how loudly each reaches the left and the right ear. */
static void speaker_gain(unsigned bit, double *l, double *r) {
    static const double c = 0.7071067811865476, h = 0.5;
    *l = *r = 0;
    switch (bit) {
    case 0: *l = 1; break;                                                      /* front left */
    case 1: *r = 1; break;                                                      /* front right */
    case 2: *l = *r = c; break;                                                 /* front centre */
    case 3: break;                                                              /* low frequency effects: left out, as stereo downmixes do */
    case 4: *l = c; break;                                                      /* back left */
    case 5: *r = c; break;                                                      /* back right */
    case 6: *l = c; break;                                                      /* front left of centre */
    case 7: *r = c; break;                                                      /* front right of centre */
    case 8: *l = *r = h; break;                                                 /* back centre */
    case 9: *l = c; break;                                                      /* side left */
    case 10: *r = c; break;                                                     /* side right */
    case 11: *l = *r = h; break;                                                /* top centre */
    case 12: *l = c; break;                                                     /* top front left */
    case 13: *l = *r = h; break;                                                /* top front centre */
    case 14: *r = c; break;                                                     /* top front right */
    case 15: *l = h; break;                                                     /* top back left */
    case 16: *l = *r = 0.35; break;                                             /* top back centre */
    case 17: *r = h; break;                                                     /* top back right */
    default: break;
    }
}

static uint32_t layout_for(unsigned channels) {
    static const uint32_t m[9] = { 0, 0x4, 0x3, 0x7, 0x33, 0x37, 0x3F, 0x70F, 0x63F };
    return channels <= 8 ? m[channels] : 0x3;                                  /* more than eight and no mask: the first two are left and right */
}

static void build_downmix(AudioFile *f) {
    uint32_t mask = f->info.channel_mask;
    unsigned n = f->info.channels;
    if (!mask || (unsigned)__builtin_popcount(mask) != n) mask = layout_for(n);
    double gl[AF_MAX_CHANNELS], gr[AF_MAX_CHANNELS], sl = 0, sr = 0;
    unsigned ch = 0;
    for (unsigned bit = 0; bit < 32 && ch < n; bit++) {
        if (!(mask & (1u << bit))) continue;
        speaker_gain(bit, &gl[ch], &gr[ch]);
        sl += gl[ch];
        sr += gr[ch];
        ch++;
    }
    for (; ch < n; ch++) gl[ch] = gr[ch] = 0;                                   /* channels the mask does not account for are dropped */
    double norm = sl > sr ? sl : sr;
    if (norm < 1) norm = 1;                                                     /* never louder than the loudest input: no clipping */
    for (unsigned i = 0; i < n; i++) {
        f->mix[0][i] = (int32_t)(gl[i] / norm * 65536.0 + 0.5);
        f->mix[1][i] = (int32_t)(gr[i] / norm * 65536.0 + 0.5);
    }
}

/* The kind of a file from its first bytes (an ID3v2 tag in front of a FLAC stream is looked through). */
static const char *detect(int fd, uint64_t size) {
    uint8_t h[16];
    int n = af_pread(fd, 0, h, sizeof h);
    if (n < 4) return NULL;
    if (!memcmp(h, "fLaC", 4)) return "FLAC";
    if (n >= 10 && !memcmp(h, "ID3", 3)) {
        uint64_t skip = 10 + ((uint64_t)(h[6] & 0x7F) << 21 | (uint64_t)(h[7] & 0x7F) << 14 | (uint64_t)(h[8] & 0x7F) << 7 | (h[9] & 0x7F)) + ((h[5] & 0x10) ? 10 : 0);
        uint8_t m[4];
        if (skip + 4 <= size && af_pread(fd, skip, m, 4) == 4 && !memcmp(m, "fLaC", 4)) return "FLAC";
        return NULL;
    }
    if (n >= 12 && (!memcmp(h, "RIFF", 4) || !memcmp(h, "RF64", 4) || !memcmp(h, "BW64", 4)) && !memcmp(h + 8, "WAVE", 4)) return "WAV";
    if (n >= 12 && !memcmp(h, "FORM", 4) && (!memcmp(h + 8, "AIFF", 4) || !memcmp(h + 8, "AIFC", 4))) return "AIFF";
    if (!memcmp(h, ".snd", 4)) return "AU";
    return NULL;
}

bool afile_probe(const char *path, char kind[12]) {
    if (kind) kind[0] = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    const char *k = NULL;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) k = detect(fd, (uint64_t)st.st_size);
    close(fd);
    if (kind && k) snprintf(kind, 12, "%s", k);
    return k != NULL;
}

int afile_open(const char *path, unsigned flags, AudioFile **out, AudioFileInfo *info_out, char *err, size_t errsz) {
    *out = NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0) { int e = errno; af_say(err, errsz, "%s", strerror(e)); return -e; }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); af_say(err, errsz, "not a regular file"); return -EINVAL; }
    uint64_t size = (uint64_t)st.st_size;
    const char *kind = detect(fd, size);
    if (!kind) { close(fd); af_say(err, errsz, "not a FLAC, WAV, AIFF or AU file"); return -EINVAL; }

    AudioFile *f = calloc(1, sizeof *f);
    if (!f) { close(fd); return -ENOMEM; }
    AfSource *src = NULL;
    int rc;
    if (!strcmp(kind, "FLAC")) rc = flac_source_open(fd, size, &f->info, &src, err, errsz);
    else if (!strcmp(kind, "WAV")) rc = wav_source_open(fd, size, &f->info, &src, err, errsz);
    else if (!strcmp(kind, "AIFF")) rc = aiff_source_open(fd, size, &f->info, &src, err, errsz);
    else rc = au_source_open(fd, size, &f->info, &src, err, errsz);
    if (rc) { close(fd); free(f); return rc; }                                  /* an opener that fails leaves fd to us */
    AudioFileInfo *in = &f->info;
    if (in->channels < 1 || in->channels > AF_MAX_CHANNELS || in->bits < 1 || in->bits > 32 || in->rate == 0) {
        af_say(err, errsz, "unusable format (%u channels, %u bits, %u Hz)", in->channels, in->bits, in->rate);
        src->close(src);
        free(f);
        return -EINVAL;
    }
    f->src = src;
    f->container = in->bits <= 8 ? 8 : in->bits <= 16 ? 16 : in->bits <= 24 ? 24 : 32;
    in->out_format = f->container == 8 ? AUDIO_FMT_U8 : f->container == 16 ? AUDIO_FMT_S16 : f->container == 24 ? AUDIO_FMT_S24 : AUDIO_FMT_S32;
    in->out_shift = f->container - in->bits;
    f->downmix = (flags & AFILE_DOWNMIX) && in->channels > 2;
    in->out_channels = f->downmix ? 2 : in->channels;
    in->out_bytes = in->out_channels * (f->container / 8);
    f->tmp = malloc((size_t)TMP_FRAMES * in->channels * sizeof(int32_t));
    if (!f->tmp) { src->close(src); free(f); return -ENOMEM; }
    if (f->downmix) build_downmix(f);
    if (info_out) *info_out = f->info;
    *out = f;
    return 0;
}

/* Packs n samples (already right-justified at `bits`) into the sample format of `container` bits. */
static void pack(unsigned container, unsigned bits, const int32_t *in, size_t n, uint8_t *out) {
    const int64_t mul = (int64_t)1 << (container - bits);
    for (size_t i = 0; i < n; i++) {
        int64_t v = (int64_t)in[i] * mul;
        switch (container) {
        case 8:  *out++ = (uint8_t)(v + 128); break;
        case 16: *out++ = (uint8_t)v; *out++ = (uint8_t)(v >> 8); break;
        case 24: *out++ = (uint8_t)v; *out++ = (uint8_t)(v >> 8); *out++ = (uint8_t)(v >> 16); break;
        default: *out++ = (uint8_t)v; *out++ = (uint8_t)(v >> 8); *out++ = (uint8_t)(v >> 16); *out++ = (uint8_t)(v >> 24); break;
        }
    }
}

ssize_t afile_read(AudioFile *f, void *buf, size_t frames) {
    if (f->pending) { ssize_t e = f->pending; f->pending = 0; return e; }
    uint8_t *out = buf;
    const unsigned chans = f->info.channels, ochans = f->info.out_channels;
    size_t done = 0;
    while (done < frames) {
        size_t want = frames - done < TMP_FRAMES ? frames - done : TMP_FRAMES;
        ssize_t n = f->src->read(f->src, f->tmp, want);
        if (n == 0) break;
        if (n < 0) {
            snprintf(f->message, sizeof f->message, "%s", f->src->message);
            if (done) { f->pending = n; break; }
            return n;
        }
        if (f->downmix) {
            for (ssize_t i = 0; i < n; i++) {
                int64_t l = 0, r = 0;
                for (unsigned c = 0; c < chans; c++) {
                    int64_t s = f->tmp[(size_t)i * chans + c];
                    l += s * f->mix[0][c];
                    r += s * f->mix[1][c];
                }
                f->tmp[(size_t)i * 2] = (int32_t)((l + 32768) >> 16);
                f->tmp[(size_t)i * 2 + 1] = (int32_t)((r + 32768) >> 16);
            }
        }
        pack(f->container, f->info.bits, f->tmp, (size_t)n * ochans, out + done * f->info.out_bytes);
        done += (size_t)n;
    }
    f->pos += done;
    return (ssize_t)done;
}

int afile_seek(AudioFile *f, uint64_t frame) {
    f->pending = 0;
    if (f->info.length_known && frame > f->info.frames) frame = f->info.frames;            /* past the end is the end */
    int rc = f->src->seek(f->src, frame);
    if (rc == 0) f->pos = frame;
    return rc;
}

uint64_t afile_tell(const AudioFile *f) { return f->pos; }
uint32_t afile_damage_count(const AudioFile *f) { return f->src->damaged; }
uint64_t afile_concealed(const AudioFile *f) { return f->src->concealed ? f->src->concealed(f->src) : 0; }
const char *afile_damage_message(const AudioFile *f) { return f->message; }
const AudioFileInfo *afile_info(const AudioFile *f) { return &f->info; }

void afile_close(AudioFile *f) {
    if (!f) return;
    f->src->close(f->src);
    free(f->tmp);
    free(f);
}

/* ------------------------------------------------------------------------------------------------------------- verifying */

int afile_verify(const char *path, char *summary, size_t summary_size) {
    char kind[12];
    if (!afile_probe(path, kind)) { af_say(summary, summary_size, "not a FLAC, WAV, AIFF or AU file"); return -EINVAL; }
    if (!strcmp(kind, "FLAC")) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) { int e = errno; af_say(summary, summary_size, "%s", strerror(e)); return -e; }
        struct stat st;
        fstat(fd, &st);
        FlacIO io = { &fd, flac_io_read, flac_io_seek, (uint64_t)st.st_size };
        FlacVerify v;
        int rc = flac_verify(&io, &v);
        close(fd);
        if (rc == 0 && v.md5_ok) af_say(summary, summary_size, "MD5 matches, %llu samples", (unsigned long long)v.samples);
        else if (rc == 0) af_say(summary, summary_size, "no MD5 in the file, no damage found, %llu samples", (unsigned long long)v.samples);
        else if (v.errors) af_say(summary, summary_size, "%u damaged spot%s (%s), %llu samples decoded", v.errors, v.errors == 1 ? "" : "s", flac_strerror(v.first_error), (unsigned long long)v.samples);
        else af_say(summary, summary_size, "%s (%llu samples decoded)", flac_strerror(rc), (unsigned long long)v.samples);
        return rc ? -EIO : 0;
    }
    AudioFile *f;
    AudioFileInfo info;
    int rc = afile_open(path, 0, &f, &info, summary, summary_size);
    if (rc) return rc;
    uint64_t frames = 0;
    uint32_t damage = 0;
    uint8_t *buf = malloc((size_t)TMP_FRAMES * info.out_bytes);
    if (!buf) { afile_close(f); return -ENOMEM; }
    for (;;) {
        ssize_t n = afile_read(f, buf, TMP_FRAMES);
        if (n == 0) break;
        if (n < 0) { damage++; continue; }
        frames += (uint64_t)n;
    }
    free(buf);
    afile_close(f);
    if (damage) { af_say(summary, summary_size, "%u damaged spot%s, %llu frames read", damage, damage == 1 ? "" : "s", (unsigned long long)frames); return -EIO; }
    if (info.length_known && frames != info.frames) { af_say(summary, summary_size, "%llu frames read, the header says %llu", (unsigned long long)frames, (unsigned long long)info.frames); return -EIO; }
    af_say(summary, summary_size, "%s has no checksum; every frame reads, %llu frames", info.format, (unsigned long long)frames);
    return 0;
}

/* ----------------------------------------------------------------------------------------------------- WAV rendering */

size_t afile_wav_header(uint8_t out[68], unsigned rate, unsigned channels, unsigned container, unsigned valid, uint32_t mask, uint64_t data_bytes) {
    const bool ext = channels > 2 || container > 16 || valid != container;
    const uint32_t block = channels * container / 8;
    uint8_t *p = out;
    uint32_t v;
    uint16_t s;
#define PUT32(x) do { v = (uint32_t)(x); memcpy(p, &v, 4); p += 4; } while (0)
#define PUT16(x) do { s = (uint16_t)(x); memcpy(p, &s, 2); p += 2; } while (0)
    memcpy(p, "RIFF", 4); p += 4;
    PUT32((ext ? 60 : 36) + data_bytes + (data_bytes & 1));
    memcpy(p, "WAVEfmt ", 8); p += 8;
    PUT32(ext ? 40 : 16);
    PUT16(ext ? 0xFFFE : 1);
    PUT16(channels);
    PUT32(rate);
    PUT32((uint64_t)rate * block);
    PUT16(block);
    PUT16(container);
    if (ext) {
        PUT16(22);
        PUT16(valid);
        PUT32(mask);
        PUT16(1);                                                               /* the sub format: PCM */
        static const uint8_t guid_tail[14] = { 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71 };
        memcpy(p, guid_tail, 14);
        p += 14;
    }
    memcpy(p, "data", 4); p += 4;
    PUT32(data_bytes);
#undef PUT32
#undef PUT16
    return (size_t)(p - out);
}

int afile_render_wav(AudioFile *f, const char *out_path, uint64_t max_frames, uint32_t *damage) {
    const AudioFileInfo *in = &f->info;
    const unsigned chans = in->channels, container = f->container;
    const size_t fb = (size_t)chans * (container / 8);
    int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -errno;
    uint8_t head[68];
    size_t hl = afile_wav_header(head, in->rate, chans, container, in->bits, in->channel_mask, 0);
    int rc = write(fd, head, hl) == (ssize_t)hl ? 0 : -EIO;
    uint8_t *bytes = malloc((size_t)TMP_FRAMES * fb);
    if (!bytes) { close(fd); return -ENOMEM; }
    uint64_t total = 0, frames_done = 0;
    uint32_t dmg = 0;
    while (!rc && (!max_frames || frames_done < max_frames)) {
        size_t want = TMP_FRAMES;
        if (max_frames && max_frames - frames_done < want) want = (size_t)(max_frames - frames_done);
        ssize_t n = f->src->read(f->src, f->tmp, want);
        if (n == 0) break;
        if (n < 0) { dmg++; snprintf(f->message, sizeof f->message, "%s", f->src->message); continue; }
        pack(container, in->bits, f->tmp, (size_t)n * chans, bytes);
        size_t len = (size_t)n * fb;
        size_t at = 0;
        while (at < len) {
            ssize_t w = write(fd, bytes + at, len - at);
            if (w <= 0) { rc = -EIO; break; }
            at += (size_t)w;
        }
        total += len;
        frames_done += (uint64_t)n;
        if (total > 0xFFFFFF00ull) { rc = -EFBIG; break; }
    }
    if (!rc && (total & 1)) { uint8_t z = 0; if (write(fd, &z, 1) != 1) rc = -EIO; }       /* chunks are padded to an even length */
    if (!rc) {
        hl = afile_wav_header(head, in->rate, chans, container, in->bits, in->channel_mask, total);
        if (lseek(fd, 0, SEEK_SET) < 0 || write(fd, head, hl) != (ssize_t)hl) rc = -EIO;
    }
    free(bytes);
    close(fd);
    if (damage) *damage = dmg;
    return rc;
}
