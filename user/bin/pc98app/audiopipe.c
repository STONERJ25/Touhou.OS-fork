/* audiopipe: the FM board and the buzzer on one timeline, in a ring for the sound pump (audiopipe.h). */
#include "audiopipe.h"
#include <string.h>

#define BEEP_AMPLITUDE 6000             /* the buzzer is a flat square wave: much quieter than full scale */

void apipe_init(AudioPipe *p, uint32_t chip_num, uint32_t chip_den, uint32_t out_rate, uint32_t cpu_hz) {
    memset(p, 0, sizeof *p);
    p->out_rate = out_rate;
    p->cpu_hz = cpu_hz ? cpu_hz : 1;
    p->gain = 256;
    p->have_fm = chip_num && chip_den;
    if (p->have_fm) opna_resampler_init(&p->rs, chip_num, chip_den, out_rate);
}

void apipe_set_volume(AudioPipe *p, int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    p->gain = percent * percent * 256 / 10000;          /* a square curve: 50 % is a quarter of the amplitude */
}

/* One frame into the ring; a full ring loses its oldest frame. */
static void push(AudioPipe *p, int32_t l, int32_t r) {
    if (l > 32767) l = 32767;
    if (l < -32768) l = -32768;
    if (r > 32767) r = 32767;
    if (r < -32768) r = -32768;
    if (p->len == APIPE_RING) {
        p->head = (p->head + 1) % APIPE_RING;
        p->len--;
        p->dropped++;
    }
    uint32_t at = (p->head + p->len) % APIPE_RING;
    p->ring[at][0] = (int16_t)l;
    p->ring[at][1] = (int16_t)r;
    p->len++;
    p->produced++;
}

/* The buzzer's sample for the next output frame: a square wave from a 32-bit phase accumulator. */
static int32_t beep_sample(AudioPipe *p, uint32_t hz) {
    p->beep_phase += (uint32_t)(((uint64_t)hz << 32) / p->out_rate);
    return (p->beep_phase & 0x80000000u) ? BEEP_AMPLITUDE : -BEEP_AMPLITUDE;
}

void apipe_run(AudioPipe *p, uint64_t clocks, const int16_t *fm, size_t nfm, uint32_t beep_hz) {
    if (!beep_hz) p->beep_phase = 0;                    /* a new tone starts at the same point of its period every time */
    int32_t gain = p->gain;
    if (p->have_fm) {
        /* The chip's frames set the length of the stretch: the chip counts machine time exactly (opna_set_host_clock), so over a
         * second it delivers 55,466.67 frames and the resampler makes 48,000 of them. */
        int16_t out[512][2];
        size_t left = nfm;
        const int16_t *in = fm;
        while (left) {
            size_t used = left, made = opna_resample(&p->rs, in, &used, &out[0][0], 512);
            for (size_t i = 0; i < made; i++) {
                int32_t l = out[i][0], r = out[i][1];
                if (beep_hz) { int32_t b = beep_sample(p, beep_hz); l += b; r += b; }
                push(p, l * gain / 256, r * gain / 256);
            }
            in += 2 * used;
            left -= used;
            if (!used && !made) break;
        }
        return;
    }
    /* No chip: the stretch is as long as the machine time says. The fraction is carried so that nothing drifts. */
    p->clock_frac += clocks * p->out_rate;
    uint64_t frames = p->clock_frac / p->cpu_hz;
    p->clock_frac %= p->cpu_hz;
    for (uint64_t i = 0; i < frames; i++) {
        int32_t b = beep_hz ? beep_sample(p, beep_hz) : 0;
        push(p, b * gain / 256, b * gain / 256);
    }
}

size_t apipe_take(AudioPipe *p, int16_t *out, size_t max) {
    size_t n = p->len < max ? p->len : max;
    if (!n && max) p->underruns++;                      /* asked for sound and there is none: the device plays silence */
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = p->ring[p->head][0];
        out[2 * i + 1] = p->ring[p->head][1];
        p->head = (p->head + 1) % APIPE_RING;
    }
    p->len -= (uint32_t)n;
    p->taken += n;
    return n;
}

void apipe_clear(AudioPipe *p) {
    p->head = p->len = 0;
    p->clock_frac = 0;
}

size_t apipe_queued(const AudioPipe *p) { return p->len; }
