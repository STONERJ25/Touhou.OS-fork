/* libaudio: tone and noise generators. */
#include <audio.h>
#include <math.h>

static int16_t clip16(double v) {
    if (v > 32767.0) return 32767;
    if (v < -32768.0) return -32768;
    return (int16_t)(v < 0 ? v - 0.5 : v + 0.5);
}

void audio_gen_tone(AudioOsc *o, int16_t *out, size_t frames, unsigned rate, double freq, double amp, AudioWave wave) {
    double step = freq / (double)rate;
    for (size_t i = 0; i < frames; i++) {
        double p = o->phase, s;
        switch (wave) {
        case AUDIO_SQUARE:   s = p < 0.5 ? 1.0 : -1.0; break;
        case AUDIO_TRIANGLE: s = p < 0.5 ? 4.0 * p - 1.0 : 3.0 - 4.0 * p; break;
        case AUDIO_SAW:      s = 2.0 * p - 1.0; break;
        default:             s = sin(2.0 * M_PI * p); break;
        }
        int16_t v = clip16(s * amp * 32767.0);
        out[2 * i] = v;
        out[2 * i + 1] = v;
        o->phase += step;
        if (o->phase >= 1.0) o->phase -= (double)(int)o->phase;
    }
}

void audio_gen_noise(AudioOsc *o, int16_t *out, size_t frames, double amp) {
    if (!o->noise) o->noise = 0xACE1u;
    for (size_t i = 0; i < frames; i++) {
        /* 32-bit xorshift: white enough for hiss and percussion */
        uint32_t x = o->noise;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        o->noise = x;
        int16_t v = clip16(((double)(int32_t)x / 2147483648.0) * amp * 32767.0);
        out[2 * i] = v;
        out[2 * i + 1] = v;
    }
}
