/*
 * libaudio: sound for Gensokyo programs.
 *
 *   Audio *a = audio_open(0);                       // /dev/audio; NULL if the machine has no sound (errno says why)
 *   audio_set_format(a, 44100, 2, AUDIO_FMT_S16);   // optional: 48 kHz stereo 16-bit is the default
 *   audio_write(a, pcm, bytes);                     // blocks until it is queued; several programs may play at once
 *   audio_drain(a);                                 // wait until it has been heard
 *   audio_close(a);
 *
 * Also here: WAV files (audio_wav_*), tone and noise generators (audio_gen_*), a small chiptune synthesizer and
 * sequencer for procedural music (chip.h functions at the end), and the short system sounds (audio_event).
 * See docs/AUDIO.md.
 */
#ifndef LIBAUDIO_H
#define LIBAUDIO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>
#include <audio_abi.h>

/* ---------------------------------------------------------------- the device */

#define AUDIO_NONBLOCK 1                    /* audio_open flag: writes return what fits instead of waiting */
#define AUDIO_CONTROL  2                    /* audio_open flag: only for volume control, no stream is needed */

typedef struct Audio {
    int fd;
    struct audio_format fmt;
} Audio;

Audio *audio_open(int flags);               /* NULL and errno set when there is no usable sound device */
void   audio_close(Audio *a);
int    audio_fd(const Audio *a);            /* for poll() */
int    audio_set_format(Audio *a, unsigned rate, unsigned channels, unsigned format);   /* 0 or -errno */
size_t audio_frame_bytes(const Audio *a);
ssize_t audio_write(Audio *a, const void *data, size_t bytes);   /* all of it unless AUDIO_NONBLOCK or a signal; or -errno */
int    audio_drain(Audio *a);               /* wait until everything written has been played */
int    audio_flush(Audio *a);               /* throw away what is queued */
int    audio_set_volume(Audio *a, int left, int right, int mute);   /* this stream, 0..100 */
int    audio_status(Audio *a, struct audio_status *st);
int    audio_info(Audio *a, struct audio_info *info);
int    audio_get_master(Audio *a, int *left, int *right, int *mute);
int    audio_set_master(Audio *a, int left, int right, int mute);
const char *audio_strerror(int err);        /* a plain-language reason: "this machine has no sound device" */
bool   audio_present(void);                 /* does /dev/audio exist? */

/* ---------------------------------------------------------------- WAV files */

typedef struct AudioWav {
    int      fd;
    uint32_t rate;
    uint16_t channels, bits;
    uint32_t data_start, data_bytes, pos;   /* byte offsets inside the file / data chunk */
    uint32_t format;                        /* AUDIO_FMT_* of the samples as stored */
} AudioWav;

int     audio_wav_open(AudioWav *w, const char *path);                 /* 0 or -errno (-EINVAL: not a PCM WAV we can play) */
ssize_t audio_wav_read(AudioWav *w, void *buf, size_t bytes);          /* sample bytes, 0 at the end */
int     audio_wav_seek(AudioWav *w, uint32_t frame);
uint32_t audio_wav_frames(const AudioWav *w);
void    audio_wav_close(AudioWav *w);
int     audio_wav_parse_header(const uint8_t *buf, size_t n, AudioWav *w);   /* fills everything but fd; the data starts at data_start */
int     audio_wav_save(const char *path, unsigned rate, unsigned channels, unsigned bits, const void *pcm, size_t bytes);
size_t  audio_wav_make_header(uint8_t out[44], unsigned rate, unsigned channels, unsigned bits, size_t data_bytes);

/* ---------------------------------------------------------------- generators */

typedef enum { AUDIO_SINE, AUDIO_SQUARE, AUDIO_TRIANGLE, AUDIO_SAW } AudioWave;

typedef struct AudioOsc {
    double phase;                           /* 0..1 */
    uint32_t noise;                         /* LFSR state for audio_gen_noise */
} AudioOsc;

/* Fill `frames` frames of interleaved 16-bit stereo at `rate`. amp is 0..1. The phase carries over between calls. */
void audio_gen_tone(AudioOsc *o, int16_t *out, size_t frames, unsigned rate, double freq, double amp, AudioWave wave);
void audio_gen_noise(AudioOsc *o, int16_t *out, size_t frames, double amp);

/* ---------------------------------------------------------------- system sounds */

typedef enum { AUDIO_EV_LOGIN, AUDIO_EV_DIALOG, AUDIO_EV_ERROR, AUDIO_EV_SHUTDOWN, AUDIO_EV_COUNT } AudioEvent;

bool audio_events_enabled(void);            /* $HAKUREI_SOUNDS, /etc/hakurei/sounds, "nosounds" on the kernel command line */
const char *audio_event_name(AudioEvent e);
int  audio_event_by_name(const char *name); /* -1 if unknown */
/* Plays a short original sound and returns at once (what is queued keeps playing after close). `force` ignores the switch. */
int  audio_event_play(AudioEvent e, bool force);
static inline void audio_event(AudioEvent e) { audio_event_play(e, false); }
/* Renders the sound as 16-bit stereo at 48 kHz into a malloc'd buffer; returns the number of frames. */
size_t audio_event_render(AudioEvent e, int16_t **out);

#endif
