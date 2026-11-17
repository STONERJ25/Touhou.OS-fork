/*
 * Host build of libaudio's file readers (user/lib/audio/src/afile.c, wavext.c, aiff.c, au.c and flac.c) for tests/host/test_audio_formats.py. The very same
 * sources run in the guest (`play`, the Jukebox); here they are built with AddressSanitizer and UBSan so that a mutation fuzz can show that a damaged file
 * costs an error and nothing worse.
 *
 *   audiotool probe FILE                    the kind of the file by its first bytes (nothing is printed if it is not one of ours)
 *   audiotool info FILE                     what afile_open says: format, codec, rate, channels, bits, frames, mask, tags, the delivered format
 *   audiotool pcm [--down] FILE OUT         every frame in the delivered (device) format to OUT, then "frames= damage= concealed="
 *   audiotool wav FILE OUT.wav [START [MAX]]  afile_render_wav: the native decode as a WAV file (from frame START, at most MAX frames)
 *   audiotool seek [--down] FILE FRAME COUNT OUT    seek, then COUNT frames in the delivered format to OUT
 *   audiotool fuzz SEED COUNT FILE...       COUNT random mutations of the given files, each opened, read to the end and seeked in
 */
#define _FILE_OFFSET_BITS 64
#include <afile.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_info(const AudioFileInfo *i, const char *err) {
    if (err) { printf("error=%s\n", err); return; }
    printf("format=%s\ncodec=%s\nrate=%u\nchannels=%u\nbits=%u\nchannel_mask=0x%x\nframes=%llu\nlength_known=%d\nbig_file=%d\n", i->format, i->codec, i->rate, i->channels,
           i->bits, i->channel_mask, (unsigned long long)i->frames, i->length_known, i->big_file);
    printf("title=%s\nartist=%s\nalbum=%s\ntrack=%s\ndate=%s\nout_format=%u\nout_channels=%u\nout_bytes=%u\nout_shift=%u\n", i->title, i->artist, i->album, i->track, i->date,
           i->out_format, i->out_channels, i->out_bytes, i->out_shift);
}

static int cmd_pcm(int argc, char **argv) {
    unsigned flags = 0;
    if (argc > 0 && !strcmp(argv[0], "--down")) { flags = AFILE_DOWNMIX; argc--; argv++; }
    if (argc < 2) return 2;
    AudioFile *f;
    AudioFileInfo info;
    char err[200];
    int rc = afile_open(argv[0], flags, &f, &info, err, sizeof err);
    if (rc) { printf("error=%s\n", err); return 1; }
    FILE *out = fopen(argv[1], "wb");
    if (!out) { perror(argv[1]); return 2; }
    uint8_t *buf = malloc(1000 * (size_t)info.out_bytes);
    uint64_t frames = 0;
    uint32_t events = 0;
    for (;;) {
        ssize_t n = afile_read(f, buf, 1000);
        if (n == 0) break;
        if (n < 0) { events++; printf("damage: %s\n", afile_damage_message(f)); continue; }
        fwrite(buf, info.out_bytes, (size_t)n, out);
        frames += (uint64_t)n;
    }
    printf("frames=%llu damage=%u concealed=%llu tell=%llu\n", (unsigned long long)frames, afile_damage_count(f), (unsigned long long)afile_concealed(f),
           (unsigned long long)afile_tell(f));
    (void)events;
    free(buf);
    fclose(out);
    afile_close(f);
    return 0;
}

static int cmd_seek(int argc, char **argv) {
    unsigned flags = 0;
    if (argc > 0 && !strcmp(argv[0], "--down")) { flags = AFILE_DOWNMIX; argc--; argv++; }
    if (argc < 4) return 2;
    AudioFile *f;
    AudioFileInfo info;
    char err[200];
    if (afile_open(argv[0], flags, &f, &info, err, sizeof err)) { printf("error=%s\n", err); return 1; }
    uint64_t target = strtoull(argv[1], NULL, 10);
    size_t count = (size_t)strtoull(argv[2], NULL, 10);
    int rc = afile_seek(f, target);
    printf("seek=%d tell=%llu\n", rc, (unsigned long long)afile_tell(f));
    uint8_t *buf = malloc((count ? count : 1) * (size_t)info.out_bytes);
    size_t got = 0;
    while (got < count) {
        ssize_t n = afile_read(f, buf + got * info.out_bytes, count - got);
        if (n == 0) break;
        if (n < 0) continue;
        got += (size_t)n;
    }
    FILE *out = fopen(argv[3], "wb");
    fwrite(buf, info.out_bytes, got, out);
    fclose(out);
    printf("read=%zu tell=%llu\n", got, (unsigned long long)afile_tell(f));
    free(buf);
    afile_close(f);
    return 0;
}

static uint64_t g_rng = 88172645463325252ull;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 11);
}

static void on_alarm(int sig) { (void)sig; fputs("audiotool fuzz: HANG (alarm)\n", stderr); abort(); }

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(size > 0 ? (size_t)size : 1);
    if (size > 0 && fread(p, 1, (size_t)size, f) != (size_t)size) exit(2);
    fclose(f);
    *n = (size_t)size;
    return p;
}

static int cmd_fuzz(int argc, char **argv) {
    if (argc < 3) return 2;
    g_rng ^= strtoull(argv[0], NULL, 10) * 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 8; i++) rnd();
    long count = strtol(argv[1], NULL, 10);
    int nfiles = argc - 2;
    uint8_t **corpus = malloc(sizeof(uint8_t *) * (size_t)nfiles);
    size_t *sizes = malloc(sizeof(size_t) * (size_t)nfiles);
    for (int i = 0; i < nfiles; i++) corpus[i] = slurp(argv[2 + i], &sizes[i]);
    signal(SIGALRM, on_alarm);
    char tmp[64];
    snprintf(tmp, sizeof tmp, "/tmp/audiotool-fuzz-%d.bin", (int)getpid());
    long opened = 0, refused = 0, damaged = 0;
    for (long it = 0; it < count; it++) {
        alarm(120);
        int which = (int)(rnd() % (uint32_t)nfiles);
        size_t n = sizes[which], cap = n * 2 + 64;
        uint8_t *buf = malloc(cap);
        memcpy(buf, corpus[which], n);
        int muts = 1 + (int)(rnd() % 4);
        for (int k = 0; k < muts && n > 0; k++) {
            uint32_t kind = rnd() % 9;
            size_t at = (rnd() & 1) ? rnd() % (n < 200 ? n : 200) : rnd() % n;                  /* half of them land in the header */
            switch (kind) {
            case 0: case 1: buf[at] ^= (uint8_t)(1u << (rnd() & 7)); break;
            case 2: buf[at] = (uint8_t)rnd(); break;
            case 3: buf[at] = (rnd() & 1) ? 0xFF : 0x00; break;
            case 4: n = at ? at : 1; break;
            case 5: { size_t len = 1 + rnd() % 40; if (at + len > n) len = n - at; memmove(buf + at, buf + at + len, n - at - len); n -= len; break; }
            case 6: { size_t len = 1 + rnd() % 40; if (at + len > n) len = n - at; if (n + len <= cap) { memmove(buf + at + len, buf + at, n - at); n += len; } break; }
            case 7: {
                static const uint32_t ex[] = { 0, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 0x0000FFFFu, 0x00010000u, 0x01000000u };
                uint32_t v = ex[rnd() % 7];
                for (int b = 0; b < 4 && at + (size_t)b < n; b++) buf[at + (size_t)b] = (uint8_t)(v >> (8 * b));
                break;
            }
            default: { size_t o = rnd() % n; uint8_t t = buf[at]; buf[at] = buf[o]; buf[o] = t; break; }
            }
        }
        FILE *o = fopen(tmp, "wb");
        fwrite(buf, 1, n, o);
        fclose(o);
        free(buf);
        AudioFile *f;
        AudioFileInfo info;
        char err[200];
        if (afile_open(tmp, (rnd() & 1) ? AFILE_DOWNMIX : 0, &f, &info, err, sizeof err)) { refused++; continue; }
        opened++;
        uint8_t *pcm = malloc(777 * (size_t)info.out_bytes);
        uint64_t total = 0;
        for (;;) {
            ssize_t r = afile_read(f, pcm, 777);
            if (r == 0) break;
            if (r < 0) { damaged++; continue; }
            total += (uint64_t)r;
            if (total > (1ull << 26)) break;
        }
        for (int k = 0; k < 4; k++) {
            uint64_t t = info.frames ? (uint64_t)rnd() % (info.frames + 2) : (uint64_t)rnd() % 100000;
            if (afile_seek(f, t) == 0) afile_read(f, pcm, 777);
        }
        free(pcm);
        afile_close(f);
    }
    unlink(tmp);
    alarm(0);
    printf("fuzz iterations=%ld opened=%ld refused=%ld damaged=%ld\n", count, opened, refused, damaged);
    for (int i = 0; i < nfiles; i++) free(corpus[i]);
    free(corpus);
    free(sizes);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: audiotool probe|info|pcm|wav|seek|fuzz ...\n"); return 2; }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "probe")) {
        char kind[12];
        if (afile_probe(argv[2], kind)) printf("%s\n", kind);
        return 0;
    }
    if (!strcmp(cmd, "info")) {
        AudioFile *f;
        AudioFileInfo info;
        char err[200];
        if (afile_open(argv[2], 0, &f, &info, err, sizeof err)) { print_info(NULL, err); return 1; }
        print_info(&info, NULL);
        afile_close(f);
        return 0;
    }
    if (!strcmp(cmd, "pcm")) return cmd_pcm(argc - 2, argv + 2);
    if (!strcmp(cmd, "seek")) return cmd_seek(argc - 2, argv + 2);
    if (!strcmp(cmd, "fuzz")) return cmd_fuzz(argc - 2, argv + 2);
    if (!strcmp(cmd, "wav") && argc >= 4) {
        AudioFile *f;
        AudioFileInfo info;
        char err[200];
        if (afile_open(argv[2], 0, &f, &info, err, sizeof err)) { printf("error=%s\n", err); return 1; }
        uint32_t damage = 0;
        if (argc >= 5) afile_seek(f, strtoull(argv[4], NULL, 10));                                          /* wav FILE OUT [START [MAX]] */
        int rc = afile_render_wav(f, argv[3], argc >= 6 ? strtoull(argv[5], NULL, 10) : 0, &damage);
        printf("render=%d damage=%u\n", rc, damage);
        afile_close(f);
        return rc ? 1 : 0;
    }
    fprintf(stderr, "audiotool: unknown command %s\n", cmd);
    return 2;
}
