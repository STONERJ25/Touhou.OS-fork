/*
 * audiopipe: what the machine sounds like, as 48 kHz stereo frames waiting for the sound card. Two sources are mixed on the
 * machine's own timeline:
 *   - the FM board (opna_take(): 55,466.67 Hz frames), resampled by the library's windowed-sinc resampler;
 *   - the buzzer (the 8255's bit 3 gating the 8253's counter 1: a square wave of pc98_beep_hz()).
 * The emulation calls apipe_run() for every stretch of machine time it ran; the output side (the program's sound pump) takes
 * frames out with apipe_take() whenever the sound device has room. The ring between the two is a third of a second long:
 *   - an underrun (the pump finds it empty, the emulation is behind) is silence on the device and is counted here;
 *   - an overrun (the pump cannot keep up: the device queue is full, or the machine runs faster than real time) drops the OLDEST
 *     frames, so that what is heard stays close to what is on the screen, and is counted.
 * Volume is a gain of 0 to 100 on a square curve (50 is a quarter of the amplitude), applied when frames enter the ring, so a
 * change is heard within the ring's latency, not at once; apipe_clear() empties the ring (pause, reset, seek).
 *
 * No dependencies beyond opna.h's resampler (freestanding); the host tests compile it as it is.
 */
#ifndef PC98APP_AUDIOPIPE_H
#define PC98APP_AUDIOPIPE_H

#include <stddef.h>
#include <stdint.h>
#include <opna.h>

#define APIPE_RING 16384                /* frames: 341 ms at 48 kHz */

typedef struct {
    OpnaResampler rs;
    int have_fm;                        /* a chip feeds the pipe: its resampled frames set the length of every stretch */
    uint32_t out_rate, cpu_hz;
    int32_t gain;                       /* 0..256 */
    uint64_t clock_frac;                /* no chip: machine clocks not yet turned into frames, times out_rate */
    uint32_t beep_phase;                /* the square wave's phase, 0..2^32 per period */
    int16_t ring[APIPE_RING][2];
    uint32_t head, len;                 /* oldest frame, frames queued */
    uint64_t produced, taken;           /* frames, in all */
    uint64_t dropped;                   /* frames discarded by an overrun */
    uint64_t underruns;                 /* apipe_take() calls that wanted frames and found the ring empty */
} AudioPipe;

/* chip_num/chip_den: the FM chip's output rate as a fraction (opna_rate_num/den), or 0/0 when there is no chip. */
void apipe_init(AudioPipe *p, uint32_t chip_num, uint32_t chip_den, uint32_t out_rate, uint32_t cpu_hz);
void apipe_set_volume(AudioPipe *p, int percent);
/* The chip's frames `fm` (nfm of them, stereo interleaved at the chip rate; NULL/0 without a chip) for a stretch of `clocks`
 * machine clocks, and the buzzer's tone for that stretch (0 = silent). */
void apipe_run(AudioPipe *p, uint64_t clocks, const int16_t *fm, size_t nfm, uint32_t beep_hz);
/* Copies up to max frames (stereo interleaved) out of the ring and returns how many. */
size_t apipe_take(AudioPipe *p, int16_t *out, size_t max);
void apipe_clear(AudioPipe *p);
/* Frames waiting, and as milliseconds. */
size_t apipe_queued(const AudioPipe *p);

#endif
