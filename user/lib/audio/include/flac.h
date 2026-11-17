/*
 * flac.h: a FLAC decoder written from RFC 9639 ("Free Lossless Audio Codec (FLAC)") and nothing else.
 *
 * The core (src/flac.c) is plain C with no operating-system calls: it reads through a FlacIO (a read callback and an
 * optional seek callback), so the same source runs in a user program (src/afile.c wraps a file descriptor) and in the host
 * tests (tests/host/flactool.c wraps a FILE or a block of memory, with AddressSanitizer and UBSan on).
 *
 *     FlacIO io = { ctx, my_read, my_seek, file_size };
 *     int err;
 *     Flac *f = flac_open(&io, &err);                 // reads the metadata; NULL and a FLAC_E_* code in err if it is not FLAC
 *     const FlacInfo *in = flac_info(f);              // rate, channels, bits per sample, length, tags, ...
 *     int32_t pcm[4096 * 2];
 *     for (;;) {
 *         ssize_t n = flac_read(f, pcm, 4096, 0);     // interleaved frames; 0 at the end; a negative code is one damaged spot
 *         if (n == 0) break;                          //   (already skipped: call again)
 *         if (n < 0) { log(flac_strerror(n)); continue; }
 *         use(pcm, n);
 *     }
 *     flac_close(f);
 *
 * Memory is bounded: a read buffer of 128 KiB (growing to hold one frame when the stream's frames are big, 8 MiB at the
 * most), the decoded block (at most 65535 samples of 8 channels in 64 bits: 4 MiB, usually a few tens of KiB) and the
 * seek table. A 4 GiB file needs no more than a 4 MiB one. Every value that comes from the stream is checked before it is
 * used (block size, channel count, partition order against the block size, predictor order, shift, escape width, sizes of
 * metadata blocks) and every reconstructed sample is checked against its bit depth, so a damaged or hostile stream gives an
 * error code and never an overrun or an overflow.
 *
 * Damage is survivable: the CRC-8 of every frame header and the CRC-16 of every frame are verified. A frame that fails is
 * reported (flac_read returns a negative code once), the decoder scans for the next frame header that checks out, and the
 * samples that were lost are replaced by silence so that the position stays right (flac_concealed() says how many).
 */
#ifndef LIBAUDIO_FLAC_H
#define LIBAUDIO_FLAC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#define FLAC_MAX_CHANNELS 8
#define FLAC_TAG_MAX      128

typedef struct Flac Flac;                               /* a decoder (opaque) */

typedef struct FlacIO {
    void *ctx;
    ssize_t (*read)(void *ctx, void *buf, size_t n);    /* bytes read (may be fewer than n), 0 at the end of the data, negative on error */
    int (*seek)(void *ctx, uint64_t offset);            /* absolute; 0 on success, negative on error; NULL if the source cannot seek */
    uint64_t size;                                      /* length in bytes, 0 if unknown (the decoder then cannot seek without a seek table) */
} FlacIO;

typedef struct FlacTags {                               /* UTF-8, control characters replaced, cut to FLAC_TAG_MAX - 1 bytes; "" if absent */
    char title[FLAC_TAG_MAX], artist[FLAC_TAG_MAX], album[FLAC_TAG_MAX], date[32], genre[64], tracknumber[16], vendor[FLAC_TAG_MAX];
} FlacTags;

typedef struct FlacInfo {
    uint32_t rate;                                      /* Hz */
    uint32_t channels;                                  /* 1 to 8 */
    uint32_t bits;                                      /* bits per sample, 4 to 32 */
    uint64_t total_samples;                             /* per channel; 0 if the stream does not say */
    uint32_t min_block, max_block;                      /* samples; from STREAMINFO (min == max: fixed block size) */
    uint32_t min_frame, max_frame;                      /* bytes; 0 = unknown */
    uint8_t  md5[16];                                   /* MD5 of the decoded audio as STREAMINFO states it */
    bool     has_md5;                                   /* false if the field is all zero (the encoder did not compute it) */
    uint32_t channel_mask;                              /* WAVEFORMATEXTENSIBLE speaker mask: from the tag or the layout RFC 9639 gives for this count */
    uint32_t seek_points;                               /* real entries of the SEEKTABLE (placeholders are not counted) */
    uint32_t pictures;                                  /* PICTURE blocks skipped */
    uint64_t picture_bytes;                             /* ... and how big they were */
    uint64_t first_frame_offset;                        /* file offset of the first audio frame */
    uint32_t id3v2_bytes;                               /* an ID3v2 tag in front of the stream, skipped */
    FlacTags tags;
} FlacInfo;

/* Negative values: every one except the first six is a damaged spot in the audio that flac_read has already skipped. */
enum {
    FLAC_OK          = 0,
    FLAC_E_IO        = -1,      /* the read or seek callback failed */
    FLAC_E_NOMEM     = -2,
    FLAC_E_NOTFLAC   = -3,      /* does not start with "fLaC" */
    FLAC_E_BADMETA   = -4,      /* STREAMINFO missing or wrong, a metadata block that does not fit in the file, ... */
    FLAC_E_UNSUPPORTED = -5,    /* a stream this decoder does not handle (the sizes are all checked; there is none yet) */
    FLAC_E_ARG       = -6,      /* a bad argument (seeking where there is no seek callback, ...) */
    FLAC_E_SYNC      = -7,      /* bytes that are not a frame were skipped to find the next one */
    FLAC_E_HEADER    = -8,      /* a frame header failed its CRC-8 or holds an impossible value */
    FLAC_E_CRC       = -9,      /* a frame decoded but its CRC-16 does not match: its samples are replaced by silence */
    FLAC_E_FRAME     = -10,     /* a frame body is not valid (bad subframe, partition, predictor, a sample out of range) */
    FLAC_E_TRUNCATED = -11,     /* the stream ends in the middle of a frame */
    FLAC_E_MD5       = -12,     /* only from flac_verify: the decoded audio does not match the MD5 in STREAMINFO */
    FLAC_E_LENGTH    = -13      /* only from flac_verify: the number of samples is not what STREAMINFO says */
};

const char *flac_strerror(int code);

Flac *flac_open(const FlacIO *io, int *err);
void  flac_close(Flac *f);
const FlacInfo *flac_info(const Flac *f);

/* Reads up to `frames` frames (one sample per channel) as interleaved samples: int32_t, each sample the plain integer in
 * [-2^(bits-1), 2^(bits-1)-1]; with FLAC_READ_S16 int16_t instead (scaled to 16 bits: shifted down, truncating, or up). Returns
 * the number of frames, 0 at the end of the stream, or a negative FLAC_E_* code that reports one damaged spot (nothing was
 * written to `out` by that call; the spot has been skipped, the next call carries on). */
#define FLAC_READ_S16 1
ssize_t flac_read(Flac *f, void *out, size_t frames, int flags);

int      flac_seek(Flac *f, uint64_t sample);   /* 0 or a negative code; seeking past the end puts the position at the end */
uint64_t flac_tell(const Flac *f);              /* the number of the next sample flac_read will return */
uint64_t flac_concealed(const Flac *f);         /* frames of silence that stand in for damaged audio so far */
uint32_t flac_errors(const Flac *f);            /* damaged spots reported so far */

typedef struct FlacVerify {
    uint64_t samples;           /* frames decoded */
    uint32_t errors;            /* damaged spots */
    bool     md5_present;       /* STREAMINFO carries an MD5 */
    bool     md5_ok;            /* ... and it equals the MD5 of what was decoded (always false if errors != 0) */
    bool     length_ok;         /* STREAMINFO's sample count is 0 (unknown) or equals the number decoded */
    uint8_t  md5[16];           /* the MD5 of what was decoded */
    int      first_error;       /* the first FLAC_E_* code seen, or 0 */
} FlacVerify;

/* Decodes the whole stream and compares the MD5 of the audio and the sample count with what STREAMINFO says. Returns 0 when
 * the stream is intact: no damaged spot, the MD5 (if there is one) matches and so does the count; FLAC_E_FRAME-style codes or
 * FLAC_E_BADMETA otherwise (details in *v). */
int flac_verify(const FlacIO *io, FlacVerify *v);

/* Does this look like the start of a FLAC file? (the marker, possibly after an ID3v2 tag: give it at least 10 + the tag size bytes, or just 4 for a plain file) */
bool flac_sniff(const uint8_t *buf, size_t n);

/* MD5 (RFC 1321), exported because STREAMINFO holds one and the host tests check it against another implementation. */
typedef struct FlacMd5 { uint32_t a, b, c, d; uint64_t bytes; uint8_t block[64]; } FlacMd5;
void flac_md5_init(FlacMd5 *m);
void flac_md5_update(FlacMd5 *m, const void *data, size_t n);
void flac_md5_final(FlacMd5 *m, uint8_t out[16]);

#endif
