/*
 * Sound: what a hardware driver (Intel HDA, AC'97) provides to the audio core (kernel/drivers/audio/audio.c).
 *
 * The core owns /dev/audio, /proc/audio, the software mixer and the thread that keeps the hardware fed. A backend is
 * only a looping DMA ring of 48 kHz signed 16-bit stereo frames and a way to ask where the hardware is in it:
 *
 *     static AudioBackend be = { .name = "Intel HDA", .start = my_start, .stop = my_stop, .position = my_pos };
 *     be.ring = dma_alloc(...); be.ring_frames = 16384;
 *     audio_register(&be);
 *
 * The core zeroes frames behind the play position, so a late refill plays silence and never repeats old sound.
 */
#ifndef K_AUDIODEV_H
#define K_AUDIODEV_H

#include "types.h"
#include "audio_abi.h"

typedef struct AudioBackend AudioBackend;
struct AudioBackend {
    const char *name;                       /* "Intel HDA" */
    char desc[192];                         /* codec and path, for /proc/audio and the log */
    s16 *ring;                              /* interleaved L/R frames the DMA engine loops over (direct-map pointer) */
    u32 ring_frames;                        /* a power of two */
    int  (*start)(AudioBackend *b);         /* begin looping from frame 0; 0 on success */
    void (*stop)(AudioBackend *b);
    u32  (*position)(AudioBackend *b);      /* frame the hardware is reading now, 0 .. ring_frames - 1 */
    void (*housekeeping)(AudioBackend *b);  /* optional; called about four times a second while playing (jack sensing) */
    size_t (*describe)(AudioBackend *b, char *buf, size_t cap);   /* optional extra lines for /proc/audio */
    void *priv;
};

/* Registers the one output device. Creates /dev/audio and /proc/audio. */
int audio_register(AudioBackend *b);

/* Software volume applied to every stream (0..100 each side), exposed so drivers can log it. */
void audio_get_master(u32 *left, u32 *right, bool *mute);

/* True once some backend has registered; a second sound driver should not bother probing. */
bool audio_registered(void);

#endif
