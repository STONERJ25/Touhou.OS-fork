/*
 * Kernel <-> userland ABI of the sound system: the ioctls and structures of /dev/audio (see docs/AUDIO.md).
 * Every structure is plain 32/64-bit integers so it is the same for the kernel and libaudio; ioctl arguments are at most
 * 256 bytes (the system call layer copies that much).
 */
#ifndef SHARED_AUDIO_ABI_H
#define SHARED_AUDIO_ABI_H

#include <stdint.h>

/* Sample formats a client may write (little endian, interleaved; 24-bit samples are packed into three bytes). */
#define AUDIO_FMT_U8   1
#define AUDIO_FMT_S16  2
#define AUDIO_FMT_S24  3
#define AUDIO_FMT_S32  4

#define AUDIO_HW_RATE  48000            /* what the hardware is always set to; clients at other rates are resampled */
#define AUDIO_MIN_RATE 4000
#define AUDIO_MAX_RATE 192000

struct audio_format {
    uint32_t rate;                      /* frames per second */
    uint32_t channels;                  /* 1 or 2 */
    uint32_t format;                    /* AUDIO_FMT_* */
};

struct audio_volume {
    uint32_t left, right;               /* 0..100 */
    uint32_t mute;                      /* 0 or 1 */
};

struct audio_status {                   /* per open stream */
    uint32_t queue_bytes;               /* capacity of the stream's queue */
    uint32_t queued_bytes;              /* written but not mixed yet */
    uint32_t queued_ms;                 /* the same in milliseconds of audio in the stream's own format */
    uint32_t space_bytes;
    uint64_t frames_written;            /* the stream's own frames, since open */
    uint64_t frames_played;             /* frames of this stream that have been mixed into the hardware ring */
    uint32_t underruns;                 /* times the stream ran dry while the hardware kept playing */
    uint32_t hw_running;                /* 1 while the DMA engine is running */
    uint64_t hw_frames;                 /* frames the hardware has consumed since it was last started */
    uint64_t pending_hw_frames;         /* mixed but not yet played: the latency in 48 kHz frames */
};

struct audio_info {
    char     name[64];                  /* "Intel HD Audio (8086:2668)" */
    char     desc[128];                 /* codec and output path */
    uint32_t hw_rate;
    uint32_t ring_frames;
    uint32_t lead_frames;
    uint32_t streams;                   /* open streams */
    uint32_t state;                     /* AUDIO_STATE_* */
    uint32_t reserved;
    uint64_t frames_mixed;
    uint64_t underruns;
};
#define AUDIO_STATE_IDLE    0
#define AUDIO_STATE_RUNNING 1
#define AUDIO_STATE_STALLED 2           /* the hardware position stopped moving: data is discarded at the right speed */

#define AUDIO_SET_FORMAT 0x4101         /* arg: struct audio_format, in and out (the format actually used) */
#define AUDIO_GET_FORMAT 0x4102
#define AUDIO_SET_VOLUME 0x4103         /* arg: struct audio_volume, this stream only */
#define AUDIO_GET_VOLUME 0x4104
#define AUDIO_SET_MASTER 0x4105         /* arg: struct audio_volume, the whole machine (lasts until the next boot) */
#define AUDIO_GET_MASTER 0x4106
#define AUDIO_GET_INFO   0x4107         /* arg: struct audio_info */
#define AUDIO_GET_STATUS 0x4108         /* arg: struct audio_status */
#define AUDIO_DRAIN      0x4109         /* wait until everything written has been played */
#define AUDIO_FLUSH      0x410A         /* throw away what is queued in this stream */

#endif
