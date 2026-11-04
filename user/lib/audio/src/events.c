/* libaudio: the short system sounds (login chime, dialog ding, error, shutdown). Original, synthesized on the spot. */
#include <audio.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static const char *const g_names[AUDIO_EV_COUNT] = { "login", "dialog", "error", "shutdown" };

const char *audio_event_name(AudioEvent e) { return e >= 0 && e < AUDIO_EV_COUNT ? g_names[e] : "?"; }

int audio_event_by_name(const char *name) {
    for (int i = 0; i < AUDIO_EV_COUNT; i++) if (!strcmp(name, g_names[i])) return i;
    return -1;
}

static bool word_is(const char *s, const char *const *words) {
    for (; *words; words++) if (!strncasecmp(s, *words, strlen(*words))) return true;
    return false;
}

/* The switch, in order: $HAKUREI_SOUNDS, the file /etc/hakurei/sounds ("on" or "off"), "nosounds" on the kernel command line. */
bool audio_events_enabled(void) {
    static const char *const off[] = { "0", "off", "no", "false", NULL }, *const on[] = { "1", "on", "yes", "true", NULL };
    const char *env = getenv("HAKUREI_SOUNDS");
    if (env && *env) return !word_is(env, off);
    int fd = open("/etc/hakurei/sounds", O_RDONLY);
    if (fd >= 0) {
        char buf[32];
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n > 0) {
            buf[n] = 0;
            char *p = buf;
            while (*p == ' ' || *p == '\t' || *p == '\n') p++;
            if (word_is(p, off)) return false;
            if (word_is(p, on)) return true;
        }
    }
    fd = open("/proc/cmdline", O_RDONLY);
    if (fd >= 0) {
        char cmd[512];
        ssize_t n = read(fd, cmd, sizeof cmd - 1);
        close(fd);
        if (n > 0) {
            cmd[n] = 0;
            for (char *p = strtok(cmd, " \n"); p; p = strtok(NULL, " \n"))
                if (!strcmp(p, "nosounds") || !strcmp(p, "sounds=off")) return false;
        }
    }
    return true;
}

typedef struct { double hz; int ms; AudioWave wave; double amp; } Note;

/* Each note is a plucked tone: a fast attack and an exponential decay; a note may ring into the next one. */
static size_t render(const Note *notes, int n, double tail_ms, int16_t **out) {
    const unsigned rate = AUDIO_HW_RATE;
    size_t total = 0;
    for (int i = 0; i < n; i++) total += (size_t)notes[i].ms * rate / 1000;
    size_t frames = total + (size_t)(tail_ms * rate / 1000);
    int16_t *buf = calloc(frames * 2, sizeof(int16_t));
    if (!buf) return 0;
    size_t start = 0;
    for (int i = 0; i < n; i++) {
        size_t len = (size_t)notes[i].ms * rate / 1000 + (i + 1 == n ? (size_t)(tail_ms * rate / 1000) : (size_t)(tail_ms * rate / 3000));
        double phase = 0, step = notes[i].hz / rate;
        double decay = 5.0 / ((double)len / rate);            /* about -43 dB by the end of the note */
        for (size_t k = 0; k < len && start + k < frames; k++) {
            double t = (double)k / rate, p = phase, s;
            switch (notes[i].wave) {
            case AUDIO_SQUARE:   s = p < 0.5 ? 0.6 : -0.6; break;
            case AUDIO_TRIANGLE: s = p < 0.5 ? 4 * p - 1 : 3 - 4 * p; break;
            default:             s = sin(2 * M_PI * p) + 0.25 * sin(4 * M_PI * p) * exp(-8 * t); break;
            }
            double attack = k < 96 ? (double)k / 96 : 1.0;
            double v = s * attack * exp(-decay * t) * notes[i].amp * 32767.0;
            int sum = buf[2 * (start + k)] + (int)v;
            if (sum > 32767) sum = 32767;
            if (sum < -32768) sum = -32768;
            buf[2 * (start + k)] = buf[2 * (start + k) + 1] = (int16_t)sum;
            phase += step;
            if (phase >= 1.0) phase -= 1.0;
        }
        start += (size_t)notes[i].ms * rate / 1000;
    }
    *out = buf;
    return frames;
}

size_t audio_event_render(AudioEvent e, int16_t **out) {
    switch (e) {
    case AUDIO_EV_LOGIN: {        /* a rising shrine-bell arpeggio: C E G, then a high C that rings */
        static const Note n[] = { { 523.25, 130, AUDIO_SINE, 0.30 }, { 659.25, 130, AUDIO_SINE, 0.30 }, { 783.99, 130, AUDIO_SINE, 0.30 },
                                  { 1046.50, 360, AUDIO_SINE, 0.34 } };
        return render(n, 4, 420, out);
    }
    case AUDIO_EV_DIALOG: {       /* a single small bell */
        static const Note n[] = { { 1318.51, 140, AUDIO_SINE, 0.30 } };
        return render(n, 1, 160, out);
    }
    case AUDIO_EV_ERROR: {        /* two low buzzes, falling */
        static const Note n[] = { { 220.00, 120, AUDIO_SQUARE, 0.20 }, { 164.81, 220, AUDIO_SQUARE, 0.20 } };
        return render(n, 2, 140, out);
    }
    case AUDIO_EV_SHUTDOWN: {     /* the login arpeggio backwards */
        static const Note n[] = { { 1046.50, 150, AUDIO_SINE, 0.30 }, { 783.99, 150, AUDIO_SINE, 0.30 }, { 659.25, 150, AUDIO_SINE, 0.30 },
                                  { 523.25, 300, AUDIO_SINE, 0.34 } };
        return render(n, 4, 360, out);
    }
    default:
        return 0;
    }
}

static int event_play(AudioEvent e, bool force, bool wait) {
    if (!force && !audio_events_enabled()) return 0;
    Audio *a = audio_open(wait ? 0 : AUDIO_NONBLOCK);
    if (!a) return -errno;
    int16_t *pcm = NULL;
    size_t frames = audio_event_render(e, &pcm);
    int rc = frames ? 0 : -ENOMEM;
    if (!rc) rc = audio_set_format(a, AUDIO_HW_RATE, 2, AUDIO_FMT_S16);
    if (!rc) {
        ssize_t n = audio_write(a, pcm, frames * 4);       /* fits in the stream's queue; the kernel plays it out after close */
        if (n < 0) rc = (int)n;
        else if (wait) audio_drain(a);                      /* for the shutdown tune: the machine is about to go */
    }
    free(pcm);
    audio_close(a);
    return rc;
}

int audio_event_play(AudioEvent e, bool force) { return event_play(e, force, false); }
int audio_event_wait(AudioEvent e) { return event_play(e, false, true); }
