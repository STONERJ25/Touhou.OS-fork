/* Sound tools: beep play volume (Nitori's sound workshop). The real work is in libaudio. */
#include "kappabox.h"
#include <audio.h>
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
    const char *wavfile = NULL, *event = NULL;
    int c;
    optind = 1;
    while ((c = getopt(argc, argv, "f:l:a:w:r:c:b:o:e:n")) != -1) {
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
        case 'n': wait = false; break;
        default:
            fprintf(stderr, "usage: beep [-f HZ] [-l MS] [-a PERCENT] [-w sine|square|triangle|saw|noise] [-r RATE] [-c 1|2] [-b 8|16|24|32] [-o FILE.wav] [-n]\n"
                            "       beep -e login|dialog|error|shutdown\n");
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

static int play_stream(Audio *a, int fd, size_t chunk, bool quiet, const char *name) {
    (void)quiet; (void)name;
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

APPLET(play) {
    unsigned rate = 44100, channels = 2, bits = 16;
    int volume = -1, gap_ms = 0;
    bool quiet = false, forced = false;
    int c;
    optind = 1;
    while ((c = getopt(argc, argv, "r:c:b:v:g:q")) != -1) {
        switch (c) {
        case 'r': rate = (unsigned)atoi(optarg); forced = true; break;
        case 'c': channels = (unsigned)atoi(optarg); forced = true; break;
        case 'b': bits = (unsigned)atoi(optarg); forced = true; break;
        case 'v': volume = atoi(optarg); break;
        case 'g': gap_ms = atoi(optarg); break;
        case 'q': quiet = true; break;
        default:
            fprintf(stderr, "usage: play [-q] [-v PERCENT] [-g MS] FILE.wav ...\n       play -r RATE -c 1|2 -b 8|16|24|32 FILE.raw   (raw samples; - is standard input)\n");
            return 2;
        }
    }
    (void)forced;
    if (optind >= argc) { fprintf(stderr, "usage: play [-q] [-v PERCENT] [-g MS] FILE.wav ...\n"); return 2; }
    Audio *a = audio_open(0);
    if (!a) { no_device("cannot play", errno); return 1; }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (volume >= 0) audio_set_volume(a, volume, volume, 0);
    int status = 0;
    for (int i = optind; i < argc && !g_stop; i++) {
        const char *path = argv[i];
        AudioWav w;
        int rc;
        if (strcmp(path, "-") && audio_wav_open(&w, path) == 0) {
            if (!quiet) printf("%s: %u Hz, %u-bit, %s, %.1f s\n", path, (unsigned)w.rate, (unsigned)w.bits, w.channels == 1 ? "mono" : "stereo",
                               (double)audio_wav_frames(&w) / (double)w.rate);
            rc = audio_set_format(a, w.rate, w.channels, w.format);
            if (rc) { kb_error("%s: %s", path, audio_strerror(rc)); status = 1; audio_wav_close(&w); continue; }
            rc = play_stream(a, w.fd, 16384, quiet, path);
            audio_wav_close(&w);
        } else {
            int fd = strcmp(path, "-") ? open(path, O_RDONLY) : 0;
            if (fd < 0) { kb_error("%s: %s", path, strerror(errno)); status = 1; continue; }
            if (bits != 8 && bits != 16 && bits != 24 && bits != 32) { kb_error("unsupported sample size"); return 2; }
            rc = audio_set_format(a, rate, channels, bits == 8 ? AUDIO_FMT_U8 : bits == 16 ? AUDIO_FMT_S16 : bits == 24 ? AUDIO_FMT_S24 : AUDIO_FMT_S32);
            if (rc) { kb_error("%s: %s", path, audio_strerror(rc)); status = 1; if (fd) close(fd); continue; }
            if (!quiet) printf("%s: raw, %u Hz, %u-bit, %s\n", path, rate, bits, channels == 1 ? "mono" : "stereo");
            rc = play_stream(a, fd, 16384, quiet, path);
            if (fd) close(fd);
        }
        if (rc) { kb_error("%s: %s", path, audio_strerror(rc)); status = 1; }
        if (!g_stop && gap_ms > 0 && i + 1 < argc) usleep((useconds_t)gap_ms * 1000);     /* the stream stays open and runs dry */
        else if (!g_stop) audio_drain(a);
    }
    if (g_stop) audio_flush(a);
    audio_close(a);
    return status;
}
