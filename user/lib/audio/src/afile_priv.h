/* libaudio, private: what the file readers (afile.c, wavext.c, aiff.c, au.c) share. */
#ifndef LIBAUDIO_AFILE_PRIV_H
#define LIBAUDIO_AFILE_PRIV_H

#include <afile.h>
#include <stdio.h>
#include <string.h>

/* A source of decoded samples: interleaved int32, each right-justified at info->bits (signed). A format that has no such thing as a
 * damaged spot never returns a negative number; FLAC does (see flac.h) and the message says what it was. */
typedef struct AfSource AfSource;
struct AfSource {
    ssize_t (*read)(AfSource *s, int32_t *out, size_t frames);     /* frames; 0 at the end; negative: a damaged spot was skipped */
    int     (*seek)(AfSource *s, uint64_t frame);                  /* 0 or negative errno */
    void    (*close)(AfSource *s);
    uint64_t (*concealed)(const AfSource *s);                      /* frames of silence put in for damaged audio (may be NULL) */
    char     message[100];                                         /* what the last damaged spot was */
    uint32_t damaged;
};

#define AF_MAX_CHANNELS 32

/* Each opener fills *info (format, codec, rate, channels, bits, channel_mask, frames, tags) and returns a source, or a negative errno and a sentence.
 * fd is open for reading and size is the length of the file; the source owns fd afterwards (it closes it). */
int wav_source_open(int fd, uint64_t size, AudioFileInfo *info, AfSource **out, char *err, size_t errsz);
int aiff_source_open(int fd, uint64_t size, AudioFileInfo *info, AfSource **out, char *err, size_t errsz);
int au_source_open(int fd, uint64_t size, AudioFileInfo *info, AfSource **out, char *err, size_t errsz);

/* Helpers shared by the readers. */
uint32_t af_be32(const uint8_t *p);
uint32_t af_le32(const uint8_t *p);
uint16_t af_be16(const uint8_t *p);
uint16_t af_le16(const uint8_t *p);
uint64_t af_le64(const uint8_t *p);
uint64_t af_be64(const uint8_t *p);
int      af_pread(int fd, uint64_t off, void *buf, size_t n);           /* exactly n bytes, or the number that were there (>= 0), or -errno */
void     af_text(char *dst, size_t cap, const uint8_t *src, size_t n);  /* tag text: UTF-8 kept, Latin-1 converted, control characters blanked, cut at a character */
void     af_say(char *err, size_t errsz, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* G.711, 16-bit scaled: the value of one companded byte. */
int  af_ulaw(uint8_t b);
int  af_alaw(uint8_t b);

/* A source of samples stored one after the other in a file: what WAV, AIFF and AU have in common. `frames` of them start at byte `off`;
 * a sample takes `container` bytes (1 to 4, or 8 for double precision) of which the `valid` high ones are significant. */
typedef enum { AF_S_PCM_BE, AF_S_PCM_LE, AF_S_U8, AF_S_FLOAT_BE, AF_S_FLOAT_LE, AF_S_ULAW, AF_S_ALAW } AfCoding;
AfSource *af_raw_source(int fd, uint64_t off, uint64_t frames, unsigned channels, unsigned container, unsigned valid, AfCoding coding);

#endif
