/*
 * afile.h: one way to read the sampled-sound files libaudio knows, whatever they are: FLAC, WAV (RIFF/WAVE, RF64 and BW64 too), AIFF and AIFF-C, and Sun .au.
 * The kind is found by the first bytes of the file, never by its name.
 *
 *     AudioFileInfo info;
 *     char err[160];
 *     AudioFile *f;
 *     if (afile_open("tune.flac", AFILE_DOWNMIX, &f, &info, err, sizeof err) != 0) { puts(err); return; }
 *     audio_set_format(dev, info.rate, info.out_channels, info.out_format);       // what afile_read will deliver
 *     uint8_t buf[16384];
 *     for (;;) {
 *         ssize_t n = afile_read(f, buf, sizeof buf / info.out_bytes);            // whole frames, in the device's own sample format
 *         if (n == 0) break;
 *         if (n < 0) { puts(afile_damage_message(f)); continue; }                 // a damaged spot was skipped; carry on
 *         audio_write(dev, buf, (size_t)n * info.out_bytes);
 *     }
 *     afile_close(f);
 *
 * Every format is decoded to integers first (compressed and companded formats to 16 bits, floating point to 32 bits) and then
 * packed into the smallest sample format the sound device has that holds them: 8 bits unsigned, 16, 24 or 32 bits signed; a
 * sample of 12 bits goes into 16 left-justified (shifted up), so nothing is lost or invented. Files with more than two channels
 * are mixed down to stereo when AFILE_DOWNMIX is given (by the speaker positions the file names, or the usual layout for the
 * channel count) and delivered whole otherwise (`play -w` keeps all the channels).
 */
#ifndef LIBAUDIO_AFILE_H
#define LIBAUDIO_AFILE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>
#include <audio_abi.h>

typedef struct AudioFile AudioFile;

#define AFILE_DOWNMIX 1                     /* mix more than two channels down to stereo */

typedef struct AudioFileInfo {
    char     format[12];                    /* "FLAC", "WAV", "RF64", "BW64", "AIFF", "AIFF-C", "AU" */
    char     codec[24];                     /* "PCM", "IEEE float", "A-law", "mu-law", "IMA ADPCM", "MS ADPCM", "FLAC" */
    uint32_t rate;                          /* frames per second */
    uint32_t channels;                      /* in the file */
    uint32_t bits;                          /* significant bits per sample in the file (16 for what is decoded to 16 bits, 32 for floating point) */
    uint32_t channel_mask;                  /* WAVEFORMATEXTENSIBLE speaker positions, 0 if the file does not say */
    uint64_t frames;                        /* 0 when the file does not say */
    bool     length_known;
    bool     big_file;                      /* needed 64-bit sizes (RF64, BW64) */
    char     title[128], artist[128], album[128], track[16], date[32];

    uint32_t out_format;                    /* AUDIO_FMT_* of what afile_read delivers */
    uint32_t out_channels;                  /* 1 or 2 with AFILE_DOWNMIX, else as in the file */
    uint32_t out_bytes;                     /* bytes in one delivered frame */
    uint32_t out_shift;                     /* the left shift that was applied to the samples (container bits - bits), for tests */
} AudioFileInfo;

/* 0 on success; otherwise a negative errno (-ENOENT, -EINVAL for a file that is not one of ours, ...) and a sentence in err. */
int      afile_open(const char *path, unsigned flags, AudioFile **out, AudioFileInfo *info, char *err, size_t errsz);
ssize_t  afile_read(AudioFile *f, void *buf, size_t frames);    /* frames delivered, 0 at the end, -EIO-style negative: one damaged spot skipped (see afile_damage_message) */
int      afile_seek(AudioFile *f, uint64_t frame);              /* 0 or negative errno */
uint64_t afile_tell(const AudioFile *f);
uint32_t afile_damage_count(const AudioFile *f);                /* damaged spots skipped so far */
uint64_t afile_concealed(const AudioFile *f);                   /* frames of silence that stand in for damaged audio */
const char *afile_damage_message(const AudioFile *f);           /* the last one, in words */
const AudioFileInfo *afile_info(const AudioFile *f);
void     afile_close(AudioFile *f);

/* Is this file one of ours, judging by its first bytes only (a regular file; nothing else is touched)? kind gets "FLAC", "WAV", "AIFF" or "AU". A file
 * that is of a kind we know but cannot be played (an unsupported codec) still answers true: afile_open then says why not. */
bool     afile_probe(const char *path, char kind[12]);

/* Reads the file from where it is (afile_seek first to start later) in native form (all channels, no downmix) and writes it as a WAV file: WAVE_FORMAT_PCM when
 * it is mono or stereo of 8 or 16 bits, WAVE_FORMAT_EXTENSIBLE (with the speaker mask and the valid bits) otherwise. At most max_frames frames (0: to the end).
 * Damaged spots are counted in *damage. Returns 0 or a negative errno. */
int      afile_render_wav(AudioFile *f, const char *out_path, uint64_t max_frames, uint32_t *damage);

/* Decodes the whole file to see that it is intact (no sound device is involved). FLAC streams carry an MD5 of the audio and a sample count, which are checked;
 * the other formats have no checksum, so for them this only shows that every frame can be read. Returns 0 when the file is intact; *summary says what was found
 * in a sentence ("MD5 matches, 9482112 samples") or what is wrong. */
int      afile_verify(const char *path, char *summary, size_t summary_size);

/* Writes a WAV header: PCM when plain, EXTENSIBLE when channels > 2, bits_container > 16 or valid_bits != bits_container. out must hold 68 bytes; returns the length (44 or 68). */
size_t   afile_wav_header(uint8_t out[68], unsigned rate, unsigned channels, unsigned container_bits, unsigned valid_bits, uint32_t mask, uint64_t data_bytes);

#endif
