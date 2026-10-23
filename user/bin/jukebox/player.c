/* The player: the loaded track, the stream to /dev/audio, and the analysis that drives the stage. */
#include "jukebox.h"
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TARGET_MS    450                /* keep about this much queued in the kernel; seeking and pausing cut it off at once */
#define CHUNK_FRAMES 1024
#define SONG_RATE    32000
#define FFT_N        512
#define CARRY_MAX    16384

Jukebox J;

void trace(const char *fmt, ...) {
    if (!J.trace) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("jukebox: ", stdout);
    vprintf(fmt, ap);
    putchar('\n');
    fflush(stdout);
    va_end(ap);
}

static double g_pos;                    /* the heard position, refreshed by player_tick */
static uint64_t g_last_master_ms;

void player_init(void) {
    J.cur = -1;
    J.volume = 60;
    J.repeat = true;
    J.carry = malloc(CARRY_MAX);
    J.au = audio_open(AUDIO_NONBLOCK);
    if (!J.au) {
        J.no_device = true;
        snprintf(J.status, sizeof J.status, "No sound device: the concert hall is silent.");
        return;
    }
    int l = 60, r = 60, m = 0;
    if (audio_get_master(J.au, &l, &r, &m) == 0) { J.volume = (l + r) / 2; J.muted = m != 0; }
    snprintf(J.status, sizeof J.status, "Ready.");
}

static void close_source(void) {
    if (J.cp) chip_free(J.cp);
    J.cp = NULL;
    if (J.song) chip_song_free(J.song);
    J.song = NULL;
    if (J.wav_open) audio_wav_close(&J.wav);
    J.wav_open = false;
    J.carry_n = 0;
    J.source_done = false;
    J.rendered = 0;
}

void player_shutdown(void) {
    if (J.au) { audio_flush(J.au); audio_close(J.au); J.au = NULL; }
    close_source();
    free(J.carry);
    J.carry = NULL;
}

double player_length(void) {
    if (J.cur < 0) return 0;
    return J.tr[J.cur].length;
}

double player_position(void) { return J.playing ? g_pos : J.paused_pos; }

/* ------------------------------------------------------------------ sources */

static void push_history(const void *data, size_t frames, unsigned channels, unsigned format) {
    for (size_t i = 0; i < frames; i++) {
        int v = 0;
        const uint8_t *p = (const uint8_t *)data;
        switch (format) {
        case AUDIO_FMT_U8: {
            for (unsigned c = 0; c < channels; c++) v += ((int)p[i * channels + c] - 128) << 8;
            break;
        }
        case AUDIO_FMT_S16: {
            const int16_t *s = (const int16_t *)data;
            for (unsigned c = 0; c < channels; c++) v += s[i * channels + c];
            break;
        }
        case AUDIO_FMT_S24: {
            for (unsigned c = 0; c < channels; c++) { const uint8_t *q = p + (i * channels + c) * 3; v += (int16_t)(q[1] | (q[2] << 8)); }
            break;
        }
        default: {
            for (unsigned c = 0; c < channels; c++) { const uint8_t *q = p + (i * channels + c) * 4; v += (int16_t)(q[2] | (q[3] << 8)); }
            break;
        }
        }
        J.hist[(J.hist_total + i) % HIST_FRAMES] = (int16_t)(v / (int)channels);
    }
    J.hist_total += frames;
}

/* Points the loaded source at `t` seconds and empties the device's queue. */
static void seek_source(double t) {
    if (J.au) audio_flush(J.au);
    J.carry_n = 0;
    J.source_done = false;
    J.rendered = 0;
    J.start_pos = t;
    J.hist_total = 0;
    if (J.cp) {
        chip_seek(J.cp, t);
        double left = chip_length(J.cp) - t;
        J.limit_frames = left > 0 ? (uint64_t)(left * J.rate) : 0;
    } else if (J.wav_open) {
        uint32_t frame = (uint32_t)(t * J.wav.rate);
        if (frame > audio_wav_frames(&J.wav)) frame = audio_wav_frames(&J.wav);
        audio_wav_seek(&J.wav, frame);
        J.limit_frames = audio_wav_frames(&J.wav) - frame;
    }
}

bool player_load(int index, double at, bool start) {
    if (index < 0 || index >= J.ntr) return false;
    Track *t = &J.tr[index];
    if (J.au) audio_flush(J.au);
    close_source();
    J.cur = -1;
    J.playing = J.paused = false;
    if (t->err[0]) {
        snprintf(J.status, sizeof J.status, "%s: %s", t->title, t->err);
        trace("cannot play %s: %s", t->title, t->err);
        return false;
    }
    unsigned channels = 2, format = AUDIO_FMT_S16;
    if (t->kind == TK_SONG) {
        char err[160];
        J.song = chip_song_load(t->path, err, sizeof err);
        if (!J.song) {
            snprintf(t->err, sizeof t->err, "%s", err);
            snprintf(J.status, sizeof J.status, "%s: %s", t->title, err);
            trace("cannot play %s: %s", t->title, err);
            return false;
        }
        J.rate = SONG_RATE;
        J.cp = chip_new(J.song, J.rate);
        if (!J.cp) { snprintf(J.status, sizeof J.status, "Out of memory."); return false; }
        t->length = chip_length(J.cp);
        J.bytes_per_frame = 4;
    } else {
        if (audio_wav_open(&J.wav, t->path) != 0) {
            snprintf(t->err, sizeof t->err, "not a WAV file this player understands");
            snprintf(J.status, sizeof J.status, "%s: %s", t->title, t->err);
            return false;
        }
        J.wav_open = true;
        J.rate = J.wav.rate;
        channels = J.wav.channels;
        format = J.wav.format;
        J.bytes_per_frame = (unsigned)(J.wav.bits / 8) * J.wav.channels;
        t->length = (double)audio_wav_frames(&J.wav) / (double)J.rate;
    }
    if (J.au) {
        int rc = audio_set_format(J.au, J.rate, channels, format);
        if (rc) {
            snprintf(J.status, sizeof J.status, "The sound device cannot play %s (%s).", t->title, audio_strerror(rc));
            close_source();
            return false;
        }
    }
    J.cur = index;
    J.paused_pos = at;
    J.start_pos = at;
    seek_source(at);
    memset(J.hist, 0, sizeof J.hist);
    if (!J.au) {
        snprintf(J.status, sizeof J.status, "No sound device: %s is loaded but cannot be heard.", t->title);
        J.paused = true;
        return true;
    }
    J.playing = start;
    J.paused = !start;
    snprintf(J.status, sizeof J.status, "%s %s", start ? "Playing" : "Loaded", t->title);
    if (start) trace("playing %s", t->title);
    return true;
}

void player_toggle(void) {
    if (J.cur < 0) {
        if (!J.ntr) return;
        int sel = J.list.selected >= 0 ? J.list.selected : 0;
        player_load(sel, 0, true);
        return;
    }
    if (!J.au) return;
    if (J.playing) {
        J.paused_pos = g_pos;
        audio_flush(J.au);
        J.carry_n = 0;
        J.playing = false;
        J.paused = true;
        snprintf(J.status, sizeof J.status, "Paused.");
        trace("paused at %.1f", J.paused_pos);
    } else {
        seek_source(J.paused_pos);
        J.playing = true;
        J.paused = false;
        snprintf(J.status, sizeof J.status, "Playing %s", J.tr[J.cur].title);
        trace("resumed at %.1f", J.paused_pos);
    }
}

void player_stop(void) {
    if (J.cur < 0) return;
    if (J.au) audio_flush(J.au);
    J.playing = false;
    J.paused = false;
    J.paused_pos = 0;
    seek_source(0);
    snprintf(J.status, sizeof J.status, "Stopped.");
    trace("stopped");
}

void player_next(int dir) {
    if (!J.ntr) return;
    if (dir < 0 && J.cur >= 0 && player_position() > 3.0) { player_seek(0); return; }
    int from = J.cur >= 0 ? J.cur : (J.list.selected >= 0 ? J.list.selected : (dir > 0 ? -1 : 0));
    for (int tries = 0; tries < J.ntr; tries++) {
        int next = from + dir;
        if (next >= J.ntr) { if (!J.repeat) { player_stop(); return; } next = 0; }
        if (next < 0) next = J.ntr - 1;
        from = next;
        if (J.tr[next].err[0]) continue;
        J.list.selected = next;
        player_load(next, 0, true);
        return;
    }
}

void player_seek(double seconds) {
    if (J.cur < 0) return;
    double len = player_length();
    if (seconds < 0) seconds = 0;
    if (len > 0 && seconds > len - 0.2) seconds = len > 0.4 ? len - 0.2 : 0;
    trace("seek %.1f", seconds);
    J.paused_pos = seconds;
    g_pos = seconds;
    seek_source(seconds);
}

void player_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    J.volume = percent;
    if (J.au) audio_set_master(J.au, percent, percent, J.muted);
    trace("volume %d", percent);
}

void player_set_mute(bool mute) {
    J.muted = mute;
    if (J.au) audio_set_master(J.au, J.volume, J.volume, mute);
    trace("%s", mute ? "muted" : "unmuted");
}

/* ------------------------------------------------------------------ feeding the device */

static size_t next_chunk(uint8_t *buf, size_t cap) {
    if (J.rendered >= J.limit_frames) return 0;
    uint64_t left = J.limit_frames - J.rendered;
    if (J.cp) {
        size_t want = CHUNK_FRAMES < left ? CHUNK_FRAMES : (size_t)left;
        size_t n = chip_render(J.cp, (int16_t *)buf, want);
        push_history(buf, n, 2, AUDIO_FMT_S16);
        J.rendered += n;
        return n * 4;
    }
    size_t want = cap < left * J.bytes_per_frame ? cap : (size_t)(left * J.bytes_per_frame);
    ssize_t got = audio_wav_read(&J.wav, buf, want);
    if (got <= 0) return 0;
    size_t frames = (size_t)got / J.bytes_per_frame;
    push_history(buf, frames, J.wav.channels, J.wav.format);
    J.rendered += frames;
    return frames * J.bytes_per_frame;
}

static void feed(const struct audio_status *st0) {
    struct audio_status st = *st0;
    uint32_t queued = st.queued_bytes;
    uint8_t chunk[16384] __attribute__((aligned(8)));
    for (int budget = 16; budget > 0; budget--) {
        if (J.carry_n) {
            ssize_t w = audio_write(J.au, J.carry, J.carry_n);
            if (w <= 0) break;                                  /* the queue is full: try again at the next tick */
            memmove(J.carry, J.carry + w, J.carry_n - (size_t)w);
            J.carry_n -= (size_t)w;
            queued += (uint32_t)w;
            if (J.carry_n) break;
            continue;
        }
        if (J.source_done) break;
        double queued_ms = (double)queued / J.bytes_per_frame * 1000.0 / J.rate;
        if (queued_ms >= TARGET_MS) break;
        size_t n = next_chunk(chunk, sizeof chunk);
        if (!n) { J.source_done = true; break; }
        ssize_t w = audio_write(J.au, chunk, n);
        if (w < 0) {
            if (w != -EAGAIN) { snprintf(J.status, sizeof J.status, "The sound device failed: %s", audio_strerror((int)w)); J.source_done = true; break; }
            w = 0;
        }
        if ((size_t)w < n && n - (size_t)w <= CARRY_MAX) {
            memcpy(J.carry, chunk + w, n - (size_t)w);
            J.carry_n = n - (size_t)w;
        }
        queued += (uint32_t)w;
    }
}

/* ------------------------------------------------------------------ the picture of the sound */

static float g_cos[FFT_N / 2], g_sin[FFT_N / 2], g_window[FFT_N];

static void analysis_init(void) {
    for (int i = 0; i < FFT_N / 2; i++) {
        g_cos[i] = (float)cos(2.0 * M_PI * i / FFT_N);
        g_sin[i] = (float)-sin(2.0 * M_PI * i / FFT_N);
    }
    for (int i = 0; i < FFT_N; i++) g_window[i] = 0.5f - 0.5f * (float)cos(2.0 * M_PI * i / (FFT_N - 1));
}

static void fft(float *re, float *im) {
    for (int i = 1, j = 0; i < FFT_N; i++) {
        int bit = FFT_N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= FFT_N; len <<= 1) {
        int half = len / 2, step = FFT_N / len;
        for (int i = 0; i < FFT_N; i += len) {
            for (int j = 0; j < half; j++) {
                float wr = g_cos[j * step], wi = g_sin[j * step];
                float xr = re[i + j + half] * wr - im[i + j + half] * wi;
                float xi = re[i + j + half] * wi + im[i + j + half] * wr;
                re[i + j + half] = re[i + j] - xr;
                im[i + j + half] = im[i + j] - xi;
                re[i + j] += xr;
                im[i + j] += xi;
            }
        }
    }
}

static void decay(void) {
    for (int b = 0; b < SPEC_BANDS; b++) { J.spec[b] *= 0.82f; if (J.spec_peak[b] > J.spec[b]) J.spec_peak[b] -= 0.012f; if (J.spec_peak[b] < J.spec[b]) J.spec_peak[b] = J.spec[b]; }
    for (int k = 0; k < 3; k++) { J.level[k] *= 0.82f; if (J.level_peak[k] > J.level[k]) J.level_peak[k] -= 0.012f; if (J.level_peak[k] < J.level[k]) J.level_peak[k] = J.level[k]; }
}

/* `heard` = source frames of the current track that have reached the speakers. */
static void analyse(int64_t heard) {
    static bool ready;
    if (!ready) { analysis_init(); ready = true; }
    if (heard < FFT_N || (uint64_t)heard > J.hist_total || J.hist_total - (uint64_t)heard > HIST_FRAMES - FFT_N) { decay(); return; }
    static float re[FFT_N], im[FFT_N];
    uint64_t first = (uint64_t)heard - FFT_N;
    for (int i = 0; i < FFT_N; i++) {
        re[i] = (float)J.hist[(first + (uint64_t)i) % HIST_FRAMES] * (1.0f / 32768.0f) * g_window[i];
        im[i] = 0;
    }
    fft(re, im);
    const double bin_hz = (double)J.rate / FFT_N;
    const double f_lo = 60.0, f_hi = J.rate / 2.0 < 9000.0 ? J.rate / 2.0 : 9000.0;
    const double ratio = pow(f_hi / f_lo, 1.0 / SPEC_BANDS);
    double f = f_lo;
    for (int b = 0; b < SPEC_BANDS; b++) {
        double f2 = f * ratio;
        int b0 = (int)(f / bin_hz), b1 = (int)(f2 / bin_hz);
        if (b0 < 1) b0 = 1;
        if (b1 < b0) b1 = b0;
        if (b1 >= FFT_N / 2) b1 = FFT_N / 2 - 1;
        float m = 0;
        for (int k = b0; k <= b1; k++) {
            float mag = sqrtf(re[k] * re[k] + im[k] * im[k]) * (4.0f / FFT_N);
            if (mag > m) m = mag;
        }
        float lvl = (float)((20.0 * log10((double)m + 1e-6) + 62.0) / 62.0);
        if (lvl < 0) lvl = 0;
        if (lvl > 1) lvl = 1;
        J.spec[b] = lvl > J.spec[b] * 0.82f ? lvl : J.spec[b] * 0.82f;
        if (J.spec[b] > J.spec_peak[b]) J.spec_peak[b] = J.spec[b]; else { J.spec_peak[b] -= 0.012f; if (J.spec_peak[b] < J.spec[b]) J.spec_peak[b] = J.spec[b]; }
        f = f2;
    }
    for (int k = 0; k < 3; k++) {
        int a = k * SPEC_BANDS / 3, z = (k + 1) * SPEC_BANDS / 3;
        float s = 0;
        for (int b = a; b < z; b++) s += J.spec[b];
        float lvl = s / (float)(z - a);
        J.level[k] = lvl > J.level[k] * 0.82f ? lvl : J.level[k] * 0.82f;
        if (J.level[k] > J.level_peak[k]) J.level_peak[k] = J.level[k]; else { J.level_peak[k] -= 0.012f; if (J.level_peak[k] < J.level[k]) J.level_peak[k] = J.level[k]; }
    }
}

/* ------------------------------------------------------------------ the tick */

void player_tick(uint64_t now_ms) {
    if (J.au && now_ms - g_last_master_ms > 400) {              /* somebody else may have turned the volume (the volume command) */
        g_last_master_ms = now_ms;
        int l, r, m;
        if (audio_get_master(J.au, &l, &r, &m) == 0) { J.volume = (l + r) / 2; J.muted = m != 0; }
    }
    if (!J.au || J.cur < 0) { decay(); return; }
    struct audio_status st;
    if (audio_status(J.au, &st) != 0) return;
    if (J.playing) {
        feed(&st);
        if (audio_status(J.au, &st) != 0) return;
        int64_t behind = (int64_t)(st.queued_bytes / J.bytes_per_frame) + (int64_t)(J.carry_n / J.bytes_per_frame) +
                         (int64_t)((double)st.pending_hw_frames * J.rate / 48000.0);
        int64_t heard = (int64_t)J.rendered - behind;
        if (heard < 0) heard = 0;
        g_pos = J.start_pos + (double)heard / J.rate;
        double len = player_length();
        if (len > 0 && g_pos > len) g_pos = len;
        analyse(heard);
        if (J.source_done && J.carry_n == 0 && st.queued_bytes == 0 && st.pending_hw_frames < 960) {
            trace("finished %s", J.tr[J.cur].title);
            J.playing = false;
            if (J.repeat || J.cur + 1 < J.ntr) player_next(+1);
            else { J.paused_pos = 0; seek_source(0); snprintf(J.status, sizeof J.status, "Finished."); }
        }
    } else {
        decay();
    }
}
