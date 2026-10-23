/* Sound tools: beep play volume (Nitori's sound workshop). The real work is in libaudio. */
#include "kappabox.h"
#include <audio.h>
#include <chip.h>
#include <math.h>

static volatile int g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void no_device(const char *what, int err) {
    kb_error("%s: %s", what, audio_strerror(err));
}

/* ---------------------------------------------------------------- beep */

/* Converts 16-bit stereo frames to the requested channel count and sample size. Returns the bytes written to out. */
static size_t convert(const int16_t *in, size_t frames, unsigned channels, unsigned bits, uint8_t *out) {
    size_t o = 0;
    for (size_t i = 0; i < frames; i++) {
        int l = in[2 * i], r = in[2 * i + 1];
        int s[2] = { channels == 1 ? (l + r) / 2 : l, r };
        for (unsigned c = 0; c < channels; c++) {
            int v = s[c];
            switch (bits) {
            case 8:  out[o++] = (uint8_t)((v >> 8) + 128); break;
            case 24: out[o++] = 0; out[o++] = (uint8_t)v; out[o++] = (uint8_t)(v >> 8); break;
            case 32: out[o++] = 0; out[o++] = 0; out[o++] = (uint8_t)v; out[o++] = (uint8_t)(v >> 8); break;
            default: out[o++] = (uint8_t)v; out[o++] = (uint8_t)(v >> 8); break;
            }
        }
    }
    return o;
}

APPLET(beep) {
    double freq = 440, amp = 0.5;
    int ms = 500;
    unsigned rate = 48000, channels = 2, bits = 16;
    AudioWave wave = AUDIO_SINE;
    bool noise = false, wait = true;
    const char *wavfile = NULL, *event = NULL, *polite = NULL;
    int c;
    optind = 1;
    while ((c = getopt(argc, argv, "f:l:a:w:r:c:b:o:e:E:n")) != -1) {
        switch (c) {
        case 'f': freq = atof(optarg); break;
        case 'l': ms = atoi(optarg); break;
        case 'a': amp = atof(optarg) / 100.0; break;
        case 'w':
            if (!strcmp(optarg, "sine")) wave = AUDIO_SINE;
            else if (!strcmp(optarg, "square")) wave = AUDIO_SQUARE;
            else if (!strcmp(optarg, "triangle")) wave = AUDIO_TRIANGLE;
            else if (!strcmp(optarg, "saw")) wave = AUDIO_SAW;
            else if (!strcmp(optarg, "noise")) noise = true;
            else { kb_error("unknown waveform '%s' (sine, square, triangle, saw, noise)", optarg); return 2; }
            break;
        case 'r': rate = (unsigned)atoi(optarg); break;
        case 'c': channels = (unsigned)atoi(optarg); break;
        case 'b': bits = (unsigned)atoi(optarg); break;
        case 'o': wavfile = optarg; break;
        case 'e': event = optarg; break;
        case 'E': polite = optarg; break;
        case 'n': wait = false; break;
        default:
            fprintf(stderr, "usage: beep [-f HZ] [-l MS] [-a PERCENT] [-w sine|square|triangle|saw|noise] [-r RATE] [-c 1|2] [-b 8|16|24|32] [-o FILE.wav] [-n]\n"
                            "       beep -e login|dialog|error|shutdown [-o FILE.wav]     (-E NAME: the same, but silent when the system sounds are off)\n");
            return 2;
        }
    }
    if (amp < 0) amp = 0;
    if (amp > 1) amp = 1;
    if (ms < 1) ms = 1;
    if (ms > 60000) ms = 60000;
    if ((channels != 1 && channels != 2) || (bits != 8 && bits != 16 && bits != 24 && bits != 32) || rate < AUDIO_MIN_RATE || rate > AUDIO_MAX_RATE) {
        kb_error("unsupported format (rate 4000-192000, 1 or 2 channels, 8/16/24/32 bits)");
        return 2;
    }

    if (polite) {                                                   /* what the desktop does: honour the switch */
        int e = audio_event_by_name(polite);
        if (e < 0) { kb_error("no system sound called '%s' (login, dialog, error, shutdown)", polite); return 2; }
        if (!audio_events_enabled()) return 0;
        int rc = audio_event_wait((AudioEvent)e);
        if (rc) { no_device("cannot play", rc); return 1; }
        return 0;
    }
    if (event) {
        int e = audio_event_by_name(event);
        if (e < 0) { kb_error("no system sound called '%s' (login, dialog, error, shutdown)", event); return 2; }
        if (wavfile) {
            int16_t *pcm = NULL;
            size_t frames = audio_event_render((AudioEvent)e, &pcm);
            int rc = frames ? audio_wav_save(wavfile, AUDIO_HW_RATE, 2, 16, pcm, frames * 4) : -ENOMEM;
            free(pcm);
            if (rc) { kb_error("%s: %s", wavfile, strerror(-rc)); return 1; }
            return 0;
        }
        int rc = audio_event_play((AudioEvent)e, true);
        if (rc) { no_device("cannot play", rc); return 1; }
        if (wait) usleep(1200000);
        return 0;
    }

    size_t total = (size_t)rate * (size_t)ms / 1000;
    if (total == 0) total = 1;
    size_t chunk_frames = rate / 20 ? rate / 20 : 1;
    int16_t *tmp = malloc(chunk_frames * 2 * sizeof(int16_t));
    uint8_t *conv = malloc(chunk_frames * 2 * 4);
    if (!tmp || !conv) kb_die("out of memory");

    Audio *a = NULL;
    FILE *wf = NULL;
    uint8_t *whole = NULL;
    size_t whole_len = 0;
    if (wavfile) {
        whole = malloc(total * channels * (bits / 8) + 16);
        if (!whole) kb_die("out of memory");
    } else {
        a = audio_open(0);
        if (!a) { no_device("cannot play", errno); return 1; }
        int rc = audio_set_format(a, rate, channels, bits == 8 ? AUDIO_FMT_U8 : bits == 16 ? AUDIO_FMT_S16 : bits == 24 ? AUDIO_FMT_S24 : AUDIO_FMT_S32);
        if (rc) { no_device("cannot play", rc); return 1; }
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
    }
    (void)wf;

    /* Render the whole sound first when it fits in memory, so the real-time part is only copying (a slow machine must not
     * starve the sound card while this program is busy computing samples). */
    size_t frame_bytes = (size_t)channels * (bits / 8);
    uint8_t *pre = NULL;
    size_t pre_len = 0, pre_pos = 0;
    if (!whole && total * frame_bytes <= 16u * 1024 * 1024) pre = malloc(total * frame_bytes + 16);

    AudioOsc osc = { 0, 0 };
    size_t done = 0;
    int status = 0;
    while (done < total && !g_stop) {
        size_t n = total - done < chunk_frames ? total - done : chunk_frames;
        if (noise) audio_gen_noise(&osc, tmp, n, amp);
        else audio_gen_tone(&osc, tmp, n, rate, freq, amp, wave);
        /* a short fade at both ends so the tone does not click */
        size_t fade = rate / 200;
        for (size_t i = 0; i < n; i++) {
            size_t pos = done + i;
            double g = 1.0;
            if (pos < fade) g = (double)pos / (double)fade;
            else if (pos + fade > total) g = (double)(total - pos) / (double)fade;
            if (g < 1.0) { tmp[2 * i] = (int16_t)(tmp[2 * i] * g); tmp[2 * i + 1] = (int16_t)(tmp[2 * i + 1] * g); }
        }
        size_t bytes = convert(tmp, n, channels, bits, conv);
        if (pre) {
            memcpy(pre + pre_len, conv, bytes);
            pre_len += bytes;
        } else if (whole) {
            memcpy(whole + whole_len, conv, bytes);
            whole_len += bytes;
        } else {
            ssize_t w = audio_write(a, conv, bytes);
            if (w < 0) { no_device("write failed", (int)w); status = 1; break; }
        }
        done += n;
    }
    while (pre && pre_pos < pre_len && !g_stop && !status) {
        size_t n = pre_len - pre_pos < 16384 ? pre_len - pre_pos : 16384;
        ssize_t w = audio_write(a, pre + pre_pos, n);
        if (w < 0) { no_device("write failed", (int)w); status = 1; break; }
        pre_pos += (size_t)w;
    }
    free(pre);
    if (whole) {
        int rc = audio_wav_save(wavfile, rate, channels, bits, whole, whole_len);
        if (rc) { kb_error("%s: %s", wavfile, strerror(-rc)); status = 1; }
        free(whole);
    } else {
        if (g_stop) audio_flush(a);
        else if (wait) audio_drain(a);
        audio_close(a);
    }
    free(tmp);
    free(conv);
    return status;
}

/* ---------------------------------------------------------------- volume */

APPLET(volume) {
    Audio *a = audio_open(AUDIO_CONTROL);
    if (!a) { no_device("the sound device", errno); return 1; }
    int l = 0, r = 0, m = 0;
    int rc = audio_get_master(a, &l, &r, &m);
    if (rc) { no_device("cannot read the volume", rc); return 1; }
    bool change = false;
    if (argc > 3 || (argc > 1 && !strcmp(argv[1], "--help"))) {
        fprintf(stderr, "usage: volume [PERCENT | +PERCENT | -PERCENT | mute | unmute | toggle | LEFT RIGHT]\n");
        return argc > 3 ? 2 : 0;
    }
    if (argc == 3) {
        l = atoi(argv[1]); r = atoi(argv[2]); change = true;
    } else if (argc == 2) {
        const char *s = argv[1];
        if (!strcmp(s, "mute")) { m = 1; change = true; }
        else if (!strcmp(s, "unmute")) { m = 0; change = true; }
        else if (!strcmp(s, "toggle")) { m = !m; change = true; }
        else if ((s[0] == '+' || s[0] == '-') && isdigit((unsigned char)s[1])) { int d = atoi(s); l += d; r += d; change = true; }
        else if (isdigit((unsigned char)s[0])) { l = r = atoi(s); change = true; }
        else { kb_error("'%s' is not a volume (0-100, +N, -N, mute, unmute or toggle)", s); return 2; }
    }
    if (l < 0) l = 0;
    if (r < 0) r = 0;
    if (l > 100) l = 100;
    if (r > 100) r = 100;
    if (change) {
        rc = audio_set_master(a, l, r, m);
        if (rc) { no_device("cannot set the volume", rc); return 1; }
    }
    if (l == r) printf("Master volume: %d%%%s\n", l, m ? " (muted)" : "");
    else printf("Master volume: left %d%%, right %d%%%s\n", l, r, m ? " (muted)" : "");
    audio_close(a);
    return 0;
}

/* ---------------------------------------------------------------- play */

#define SONG_RATE 32000                     /* songs are synthesized at this rate; the kernel converts to 48 kHz */

typedef struct {
    bool quiet, info, loop_forever;
    double start, seconds;
    const char *wavout;
} PlayOpts;

static int play_stream(Audio *a, int fd, size_t chunk) {
    uint8_t *buf = malloc(chunk);
    if (!buf) kb_die("out of memory");
    int rc = 0;
    while (!g_stop) {
        ssize_t n = read(fd, buf, chunk);
        if (n < 0) { if (errno == EINTR) continue; rc = -errno; break; }
        if (n == 0) break;
        ssize_t w = audio_write(a, buf, (size_t)n);
        if (w < 0) { rc = (int)w; break; }
    }
    free(buf);
    return rc;
}

/* A song file starts with the word "tms" (see tms(5)) or is named *.tms. */
static bool is_song(const char *path) {
    size_t n = strlen(path);
    if (n > 4 && !strcmp(path + n - 4, ".tms")) return true;
    if (!strcmp(path, "-")) return false;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    char head[4] = { 0 };
    ssize_t r = read(fd, head, sizeof head);
    close(fd);
    return r == 4 && !memcmp(head, "tms", 3) && (head[3] == ' ' || head[3] == '\n' || head[3] == '\r');
}

static void clock_text(double s, char *out, size_t n) {
    int t = (int)(s + 0.5);
    snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

static void song_summary(const ChipSong *s, double length, bool full) {
    char len[16];
    clock_text(length, len, sizeof len);
    printf("%s%s%s%s\n", s->title, s->mood[0] ? " (" : "", s->mood, s->mood[0] ? ")" : "");
    printf("  %d bpm, %d channels, %s%s\n", s->bpm, s->nchannels, len, s->loop ? ", loops" : "");
    if (s->author[0]) printf("  by %s\n", s->author);
    if (!full) return;
    for (int c = 0; c < s->nchannels; c++) {
        const ChipInstrument *in = &s->inst[s->def[c].instrument];
        printf("  channel %-8s starts with %s (pan %+.2f, gain %.2f)\n", s->def[c].name, in->name, (double)s->def[c].pan, (double)s->def[c].gain);
    }
    if (s->warnings[0]) printf("  note: %s\n", s->warnings);
}

/* Plays (or, with -i, measures; with -w, writes to a WAV file) one song file. *ap is the device, opened on first use. */
static int play_song(Audio **ap, const char *path, const PlayOpts *o, int volume) {
    char err[200];
    ChipSong *song = chip_song_load(path, err, sizeof err);
    if (!song) { kb_error("%s: %s", path, err); return 1; }
    unsigned rate = o->wavout ? 44100 : SONG_RATE;
    ChipPlayer *cp = chip_new(song, rate);
    if (!cp) { kb_error("%s: out of memory", path); chip_song_free(song); return 1; }
    double len = chip_length(cp);
    if (!o->quiet || o->info) song_summary(song, len, o->info);
    if (o->start > 0) chip_seek(cp, o->start);
    double remaining = len - (o->start < len ? o->start : len);
    double limit = o->seconds > 0 && o->seconds < remaining ? o->seconds : remaining;
    if (o->info && o->seconds <= 0 && limit > 30) limit = 30;                  /* measuring is for checking a song, not for waiting */
    uint64_t total = (uint64_t)(limit * rate);
    if (o->loop_forever && !o->info && !o->wavout) total = ~0ull;
    static int16_t buf[2 * 1024];
    int rc = 0;

    if (o->info) {
        uint64_t done = 0, hot = 0, quiet_windows = 0, windows = 0;
        double sumsq = 0;
        int peak = 0, wpeak = 0, wn = 0;
        while (done < total) {
            size_t want = sizeof buf / 4 < total - done ? sizeof buf / 4 : (size_t)(total - done);
            size_t n = chip_render(cp, buf, want);
            if (!n) break;
            for (size_t i = 0; i < n * 2; i++) {
                int v = buf[i] < 0 ? -buf[i] : buf[i];
                if (v > peak) peak = v;
                if (v > 30000) hot++;
                if (v > wpeak) wpeak = v;
                sumsq += (double)buf[i] * (double)buf[i];
            }
            wn += (int)n;
            if (wn >= (int)rate / 4) { windows++; if (wpeak < 30) quiet_windows++; wpeak = 0; wn = 0; }
            done += n;
        }
        printf("  measured %.1f s: peak %.2f, rms %.3f, %llu samples above 0.9, %d%% silent\n", (double)done / rate, peak / 32768.0,
               done ? sqrt(sumsq / (double)(done * 2)) / 32768.0 : 0.0, (unsigned long long)hot, windows ? (int)(quiet_windows * 100 / windows) : 0);
    } else if (o->wavout) {
        size_t cap = (size_t)total * 4;
        if (cap > 64u * 1024 * 1024) { kb_error("%s: too long to render to a file", path); rc = 1; }
        else {
            uint8_t *pcm = malloc(cap ? cap : 1);
            if (!pcm) kb_die("out of memory");
            size_t done = 0;
            while (done < total) {
                size_t want = sizeof buf / 4 < total - done ? sizeof buf / 4 : (size_t)(total - done);
                size_t n = chip_render(cp, buf, want);
                if (!n) break;
                memcpy(pcm + done * 4, buf, n * 4);
                done += n;
            }
            int w = audio_wav_save(o->wavout, rate, 2, 16, pcm, done * 4);
            if (w) { kb_error("%s: %s", o->wavout, strerror(-w)); rc = 1; }
            free(pcm);
        }
    } else {
        if (!*ap) {
            *ap = audio_open(0);
            if (!*ap) { no_device("cannot play", errno); rc = 1; goto out; }
            signal(SIGINT, on_signal);
            signal(SIGTERM, on_signal);
            if (volume >= 0) audio_set_volume(*ap, volume, volume, 0);
        }
        int f = audio_set_format(*ap, rate, 2, AUDIO_FMT_S16);
        if (f) { kb_error("%s: %s", path, audio_strerror(f)); rc = 1; goto out; }
        uint64_t done = 0;
        while (!g_stop && done < total) {
            size_t want = sizeof buf / 4 < total - done ? sizeof buf / 4 : (size_t)(total - done);
            size_t n = chip_render(cp, buf, want);
            if (!n) break;
            ssize_t w = audio_write(*ap, buf, n * 4);
            if (w < 0) { kb_error("%s: %s", path, audio_strerror((int)w)); rc = 1; break; }
            done += n;
        }
    }
out:
    chip_free(cp);
    chip_song_free(song);
    return rc;
}

#define PLAY_USAGE "usage: play [-q] [-v PERCENT] [-g MS] FILE.wav|FILE.tms ...\n" \
                   "       play -r RATE -c 1|2 -b 8|16|24|32 FILE.raw     (raw samples; - is standard input)\n" \
                   "       play [-s SECONDS] [-t SECONDS] [-L] FILE.tms   (start there, play that long, repeat forever)\n" \
                   "       play -i [-t SECONDS] FILE.tms                   (describe and measure a song; no sound device needed)\n" \
                   "       play -w OUT.wav FILE.tms                        (render a song to a WAV file)\n"

APPLET(play) {
    unsigned rate = 44100, channels = 2, bits = 16;
    int volume = -1, gap_ms = 0;
    PlayOpts o;
    memset(&o, 0, sizeof o);
    int c;
    optind = 1;
    while ((c = getopt(argc, argv, "r:c:b:v:g:s:t:w:qiL")) != -1) {
        switch (c) {
        case 'r': rate = (unsigned)atoi(optarg); break;
        case 'c': channels = (unsigned)atoi(optarg); break;
        case 'b': bits = (unsigned)atoi(optarg); break;
        case 'v': volume = atoi(optarg); break;
        case 'g': gap_ms = atoi(optarg); break;
        case 's': o.start = atof(optarg); break;
        case 't': o.seconds = atof(optarg); break;
        case 'w': o.wavout = optarg; break;
        case 'q': o.quiet = true; break;
        case 'i': o.info = true; break;
        case 'L': o.loop_forever = true; break;
        default: fputs(PLAY_USAGE, stderr); return 2;
        }
    }
    if (optind >= argc) { fputs(PLAY_USAGE, stderr); return 2; }
    Audio *a = NULL;
    int status = 0;
    for (int i = optind; i < argc && !g_stop; i++) {
        const char *path = argv[i];
        AudioWav w;
        int rc = 0;
        if (is_song(path)) {
            rc = play_song(&a, path, &o, volume);
            if (rc) status = 1;
            if (a && !g_stop && gap_ms > 0 && i + 1 < argc) usleep((useconds_t)gap_ms * 1000);
            else if (a && !g_stop) audio_drain(a);
            continue;
        }
        if (o.info || o.wavout) { kb_error("%s: -i and -w need a song file (*.tms)", path); status = 1; continue; }
        if (!a) {
            a = audio_open(0);
            if (!a) { no_device("cannot play", errno); return 1; }
            signal(SIGINT, on_signal);
            signal(SIGTERM, on_signal);
            if (volume >= 0) audio_set_volume(a, volume, volume, 0);
        }
        if (strcmp(path, "-") && audio_wav_open(&w, path) == 0) {
            if (!o.quiet) printf("%s: %u Hz, %u-bit, %s, %.1f s\n", path, (unsigned)w.rate, (unsigned)w.bits, w.channels == 1 ? "mono" : "stereo",
                                 (double)audio_wav_frames(&w) / (double)w.rate);
            rc = audio_set_format(a, w.rate, w.channels, w.format);
            if (rc) { kb_error("%s: %s", path, audio_strerror(rc)); status = 1; audio_wav_close(&w); continue; }
            rc = play_stream(a, w.fd, 16384);
            audio_wav_close(&w);
        } else {
            int fd = strcmp(path, "-") ? open(path, O_RDONLY) : 0;
            if (fd < 0) { kb_error("%s: %s", path, strerror(errno)); status = 1; continue; }
            if (bits != 8 && bits != 16 && bits != 24 && bits != 32) { kb_error("unsupported sample size"); return 2; }
            rc = audio_set_format(a, rate, channels, bits == 8 ? AUDIO_FMT_U8 : bits == 16 ? AUDIO_FMT_S16 : bits == 24 ? AUDIO_FMT_S24 : AUDIO_FMT_S32);
            if (rc) { kb_error("%s: %s", path, audio_strerror(rc)); status = 1; if (fd) close(fd); continue; }
            if (!o.quiet) printf("%s: raw, %u Hz, %u-bit, %s\n", path, rate, bits, channels == 1 ? "mono" : "stereo");
            rc = play_stream(a, fd, 16384);
            if (fd) close(fd);
        }
        if (rc) { kb_error("%s: %s", path, audio_strerror(rc)); status = 1; }
        if (!g_stop && gap_ms > 0 && i + 1 < argc) usleep((useconds_t)gap_ms * 1000);     /* the stream stays open and runs dry */
        else if (!g_stop) audio_drain(a);
    }
    if (a) {
        if (g_stop) audio_flush(a);
        audio_close(a);
    }
    return status;
}
