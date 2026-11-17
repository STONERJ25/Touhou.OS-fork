/*
 * Host build of the FLAC decoder (user/lib/audio/src/flac.c) for tests/host/test_flac.py. The very same source runs in the guest; here it
 * is linked against a FILE or a block of memory and built with AddressSanitizer and UBSan, so that a mutation fuzz can show that a damaged
 * stream costs an error code and nothing worse.
 *
 *   flactool info FILE                      the STREAMINFO fields, the tags and what the metadata held, as key=value lines
 *   flactool decode [--s16] FILE OUT        every frame to OUT (int32 or int16 little endian, interleaved); one "error ..." line per damaged spot,
 *                                           then "frames= errors= concealed= tell="
 *   flactool verify FILE                    flac_verify: prints samples, errors, md5 results; exit status 0 when intact
 *   flactool seek FILE SAMPLE COUNT OUT     seek, then read COUNT frames (int32) to OUT; prints "tell=" before and after
 *   flactool seektest FILE SEED COUNT      COUNT random seeks in one open stream, each read checked against a full decode made first
 *   flactool md5 FILE                       the MD5 of the file's bytes (flac_md5_*, for checking it against another implementation)
 *   flactool fuzz SEED COUNT FILE...        COUNT random mutations of the given streams, each opened, read to the end, seeked in and verified;
 *                                           the sanitizers abort on a fault, alarm(120) on a hang
 */
#define _FILE_OFFSET_BITS 64
#include <flac.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint64_t g_rng = 88172645463325252ull;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 11);
}

typedef struct { FILE *f; unsigned long long bytes; } FileIO;

static ssize_t file_read(void *ctx, void *buf, size_t n) {
    FileIO *io = ctx;
    size_t got = fread(buf, 1, n, io->f);
    io->bytes += got;
    if (got == 0 && ferror(io->f)) return -1;
    return (ssize_t)got;
}

static int file_seek(void *ctx, uint64_t off) {
    FileIO *io = ctx;
    return fseeko(io->f, (off_t)off, SEEK_SET) ? -1 : 0;
}

typedef struct { const uint8_t *p; size_t n, pos; } MemIO;

static ssize_t mem_read(void *ctx, void *buf, size_t n) {
    MemIO *io = ctx;
    size_t left = io->n - io->pos;
    if (n > left) n = left;
    memcpy(buf, io->p + io->pos, n);
    io->pos += n;
    return (ssize_t)n;
}

static int mem_seek(void *ctx, uint64_t off) {
    MemIO *io = ctx;
    if (off > io->n) return -1;
    io->pos = (size_t)off;
    return 0;
}

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(size > 0 ? (size_t)size : 1);
    if (size > 0 && fread(p, 1, (size_t)size, f) != (size_t)size) { perror(path); exit(2); }
    fclose(f);
    *n = (size_t)size;
    return p;
}

static void hex(const uint8_t *p, int n) { for (int i = 0; i < n; i++) printf("%02x", p[i]); }

static int open_file(const char *path, FileIO *io, FlacIO *fio) {
    io->f = fopen(path, "rb");
    if (!io->f) { perror(path); return 2; }
    fseeko(io->f, 0, SEEK_END);
    fio->size = (uint64_t)ftello(io->f);
    fseeko(io->f, 0, SEEK_SET);
    fio->ctx = io;
    fio->read = file_read;
    fio->seek = file_seek;
    return 0;
}

static int cmd_info(const char *path) {
    FileIO io;
    FlacIO fio;
    if (open_file(path, &io, &fio)) return 2;
    int err;
    Flac *f = flac_open(&fio, &err);
    if (!f) { printf("error=%d\nmessage=%s\n", err, flac_strerror(err)); return 1; }
    const FlacInfo *in = flac_info(f);
    printf("open_bytes=%llu\n", io.bytes);                                            /* what flac_open read: a playlist opens hundreds of files */
    printf("rate=%u\nchannels=%u\nbits=%u\ntotal_samples=%llu\nmin_block=%u\nmax_block=%u\nmin_frame=%u\nmax_frame=%u\n", in->rate, in->channels, in->bits,
           (unsigned long long)in->total_samples, in->min_block, in->max_block, in->min_frame, in->max_frame);
    printf("md5=");
    hex(in->md5, 16);
    printf("\nhas_md5=%d\nchannel_mask=0x%x\nseek_points=%u\npictures=%u\npicture_bytes=%llu\nfirst_frame_offset=%llu\nid3v2_bytes=%u\n", in->has_md5, in->channel_mask,
           in->seek_points, in->pictures, (unsigned long long)in->picture_bytes, (unsigned long long)in->first_frame_offset, in->id3v2_bytes);
    printf("title=%s\nartist=%s\nalbum=%s\ndate=%s\ngenre=%s\ntracknumber=%s\nvendor=%s\n", in->tags.title, in->tags.artist, in->tags.album, in->tags.date, in->tags.genre,
           in->tags.tracknumber, in->tags.vendor);
    flac_close(f);
    fclose(io.f);
    return 0;
}

static int cmd_decode(int argc, char **argv) {
    int s16 = 0, i = 0;
    if (argc > 0 && !strcmp(argv[0], "--s16")) { s16 = 1; i = 1; }
    if (argc - i < 2) { fprintf(stderr, "usage: flactool decode [--s16] FILE OUT\n"); return 2; }
    FileIO io;
    FlacIO fio;
    if (open_file(argv[i], &io, &fio)) return 2;
    int err;
    Flac *f = flac_open(&fio, &err);
    if (!f) { printf("open error=%d %s\n", err, flac_strerror(err)); return 1; }
    FILE *out = fopen(argv[i + 1], "wb");
    if (!out) { perror(argv[i + 1]); return 2; }
    const FlacInfo *in = flac_info(f);
    enum { CHUNK = 1000 };                                                     /* not a power of two or a block size: blocks are cut anywhere */
    int32_t *pcm = malloc((size_t)CHUNK * in->channels * sizeof(int32_t));
    uint64_t frames = 0;
    for (;;) {
        uint64_t before = flac_tell(f);
        ssize_t n = flac_read(f, pcm, CHUNK, s16 ? FLAC_READ_S16 : 0);
        if (n == 0) break;
        if (n < 0) {
            printf("error at=%llu code=%d message=%s\n", (unsigned long long)before, (int)n, flac_strerror((int)n));
            if (n == FLAC_E_IO || n == FLAC_E_NOMEM) break;
            continue;
        }
        fwrite(pcm, s16 ? 2 : 4, (size_t)n * in->channels, out);
        frames += (uint64_t)n;
    }
    printf("frames=%llu errors=%u concealed=%llu tell=%llu\n", (unsigned long long)frames, flac_errors(f), (unsigned long long)flac_concealed(f),
           (unsigned long long)flac_tell(f));
    free(pcm);
    fclose(out);
    flac_close(f);
    fclose(io.f);
    return 0;
}

static int cmd_verify(const char *path) {
    FileIO io;
    FlacIO fio;
    if (open_file(path, &io, &fio)) return 2;
    FlacVerify v;
    int rc = flac_verify(&fio, &v);
    printf("result=%d samples=%llu errors=%u md5_present=%d md5_ok=%d length_ok=%d first_error=%d md5=", rc, (unsigned long long)v.samples, v.errors,
           v.md5_present, v.md5_ok, v.length_ok, v.first_error);
    hex(v.md5, 16);
    printf(" message=%s\n", flac_strerror(rc));
    fclose(io.f);
    return rc == 0 ? 0 : 1;
}

/* Seeks around in one open stream and checks every read against a full decode made first: the state machine of seeking and reading on. */
static int cmd_seektest(const char *path, uint64_t seed, long count) {
    FileIO io;
    FlacIO fio;
    if (open_file(path, &io, &fio)) return 2;
    int err;
    Flac *f = flac_open(&fio, &err);
    if (!f) { printf("open error=%d %s\n", err, flac_strerror(err)); return 1; }
    const FlacInfo *in = flac_info(f);
    const uint32_t ch = in->channels;
    size_t cap = 1 << 16, total = 0;
    int32_t *all = malloc(cap * ch * sizeof(int32_t));
    for (;;) {
        if (total == cap) { cap *= 2; all = realloc(all, cap * ch * sizeof(int32_t)); }
        ssize_t n = flac_read(f, all + total * ch, cap - total, 0);
        if (n == 0) break;
        if (n < 0) { printf("seektest: the stream is damaged (%s)\n", flac_strerror((int)n)); return 1; }
        total += (size_t)n;
    }
    g_rng ^= seed * 0x9E3779B97F4A7C15ull;
    int32_t *buf = malloc(5000 * ch * sizeof(int32_t));
    long bad = 0;
    for (long i = 0; i < count; i++) {
        uint64_t t;
        switch (rnd() % 5) {
        case 0: t = total ? rnd() % total : 0; break;
        case 1: t = in->max_block ? (uint64_t)(rnd() % 40) * in->max_block + (rnd() % 3) - 1 : 0; break;      /* around block boundaries */
        case 2: t = total - (rnd() % 10 < total ? rnd() % 10 : 0); break;                                      /* near the end */
        case 3: t = rnd() % 4; break;
        default: t = total + rnd() % 3; break;                                                                 /* at and past the end */
        }
        if (t > total) t = total;
        int rc = flac_seek(f, t);
        size_t want = 1 + rnd() % 4000, got = 0;
        while (got < want) {
            ssize_t n = flac_read(f, buf + got * ch, want - got, 0);
            if (n <= 0) break;
            got += (size_t)n;
        }
        size_t expect = t >= total ? 0 : (total - t < want ? total - t : want);
        if (rc != 0 || got != expect || memcmp(buf, all + t * ch, got * ch * sizeof(int32_t)) || flac_tell(f) != t + got) {
            printf("seektest: seek %llu (read %zu): rc=%d got=%zu expected=%zu tell=%llu\n", (unsigned long long)t, want, rc, got, expect, (unsigned long long)flac_tell(f));
            bad++;
        }
    }
    printf("seektest seeks=%ld bad=%ld samples=%zu\n", count, bad, total);
    free(buf);
    free(all);
    flac_close(f);
    fclose(io.f);
    return bad ? 1 : 0;
}

static int cmd_seek(char **argv) {
    FileIO io;
    FlacIO fio;
    if (open_file(argv[0], &io, &fio)) return 2;
    int err;
    Flac *f = flac_open(&fio, &err);
    if (!f) { printf("open error=%d %s\n", err, flac_strerror(err)); return 1; }
    uint64_t target = strtoull(argv[1], NULL, 10);
    size_t count = (size_t)strtoull(argv[2], NULL, 10);
    int rc = flac_seek(f, target);
    printf("seek=%d tell=%llu\n", rc, (unsigned long long)flac_tell(f));
    const FlacInfo *in = flac_info(f);
    FILE *out = fopen(argv[3], "wb");
    int32_t *pcm = malloc((count ? count : 1) * in->channels * sizeof(int32_t));
    size_t got = 0;
    while (got < count) {
        ssize_t n = flac_read(f, pcm + got * in->channels, count - got, 0);
        if (n == 0) break;
        if (n < 0) { printf("error code=%d\n", (int)n); continue; }
        got += (size_t)n;
    }
    fwrite(pcm, 4, got * in->channels, out);
    printf("read=%zu tell=%llu\n", got, (unsigned long long)flac_tell(f));
    free(pcm);
    fclose(out);
    flac_close(f);
    fclose(io.f);
    return 0;
}

static int cmd_md5(const char *path) {
    size_t n;
    uint8_t *p = slurp(path, &n);
    FlacMd5 m;
    uint8_t d[16];
    flac_md5_init(&m);
    size_t at = 0, step = 1;
    while (at < n) {                                                           /* in pieces of odd sizes, so that the partial-block code runs */
        size_t take = step < n - at ? step : n - at;
        flac_md5_update(&m, p + at, take);
        at += take;
        step = step * 3 % 211 + 1;
    }
    flac_md5_final(&m, d);
    hex(d, 16);
    printf("\n");
    free(p);
    return 0;
}

static void on_alarm(int sig) { (void)sig; fputs("flactool fuzz: HANG (alarm)\n", stderr); abort(); }

/* One full pass over a (possibly damaged) stream: open, read everything, seek around, verify. Returns 0 if it opened. */
static int exercise(const uint8_t *data, size_t n, uint64_t *frames, uint32_t *errors) {
    MemIO m = { data, n, 0 };
    FlacIO fio = { &m, mem_read, mem_seek, n };
    int err;
    Flac *f = flac_open(&fio, &err);
    if (!f) return 1;
    const FlacInfo *in = flac_info(f);
    size_t chunk = 777;
    int32_t *pcm = malloc(chunk * in->channels * sizeof(int32_t));
    uint64_t total = 0;
    for (;;) {
        ssize_t r = flac_read(f, pcm, chunk, (rnd() & 3) == 0 ? FLAC_READ_S16 : 0);
        if (r == 0) break;
        if (r < 0) { (*errors)++; if (r == FLAC_E_IO || r == FLAC_E_NOMEM) break; continue; }
        total += (uint64_t)r;
        if (total > (1ull << 28)) break;                                       /* a hostile frame count: enough */
    }
    *frames += total;
    for (int k = 0; k < 4; k++) {                                              /* seeking in a damaged file must be safe too */
        uint64_t t = in->total_samples ? (uint64_t)rnd() % (in->total_samples + 2) : (uint64_t)rnd() % 100000;
        if (flac_seek(f, t) == 0) {
            ssize_t r = flac_read(f, pcm, chunk, 0);
            (void)r;
        }
    }
    free(pcm);
    flac_close(f);
    FlacVerify v;
    MemIO m2 = { data, n, 0 };
    FlacIO fio2 = { &m2, mem_read, mem_seek, n };
    flac_verify(&fio2, &v);
    return 0;
}

static int cmd_fuzz(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: flactool fuzz SEED COUNT FILE...\n"); return 2; }
    g_rng ^= strtoull(argv[0], NULL, 10) * 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 8; i++) rnd();
    long count = strtol(argv[1], NULL, 10);
    int nfiles = argc - 2;
    uint8_t **corpus = malloc(sizeof(uint8_t *) * (size_t)nfiles);
    size_t *sizes = malloc(sizeof(size_t) * (size_t)nfiles);
    for (int i = 0; i < nfiles; i++) corpus[i] = slurp(argv[2 + i], &sizes[i]);
    signal(SIGALRM, on_alarm);
    long opened = 0, refused = 0, with_errors = 0;
    uint64_t frames = 0;
    for (long it = 0; it < count; it++) {
        alarm(120);
        int which = (int)(rnd() % (uint32_t)nfiles);
        size_t n = sizes[which];
        size_t cap = n * 2 + 64;
        uint8_t *buf = malloc(cap);
        memcpy(buf, corpus[which], n);
        int muts = 1 + (int)(rnd() % 4);
        for (int k = 0; k < muts && n > 0; k++) {
            uint32_t kind = rnd() % 9;
            size_t at = (rnd() & 1) ? rnd() % (n < 200 ? n : 200) : rnd() % n;       /* half of them land in the first 200 bytes: the metadata and the first frame header */
            switch (kind) {
            case 0: case 1: buf[at] ^= (uint8_t)(1u << (rnd() & 7)); break;
            case 2: buf[at] = (uint8_t)rnd(); break;
            case 3: buf[at] = (rnd() & 1) ? 0xFF : 0x00; break;
            case 4: n = at ? at : 1; break;                                          /* truncate */
            case 5: {                                                                /* delete a chunk */
                size_t len = 1 + rnd() % 40;
                if (at + len > n) len = n - at;
                memmove(buf + at, buf + at + len, n - at - len);
                n -= len;
                break;
            }
            case 6: {                                                                /* duplicate a chunk */
                size_t len = 1 + rnd() % 40;
                if (at + len > n) len = n - at;
                if (n + len <= cap) {
                    memmove(buf + at + len, buf + at, n - at);
                    n += len;
                }
                break;
            }
            case 7: {                                                                /* an extreme value over up to four bytes */
                static const uint32_t ex[] = { 0, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 0x0000FFFFu, 0x00010000u, 0x01000000u };
                uint32_t v = ex[rnd() % 7];
                for (int b = 0; b < 4 && at + (size_t)b < n; b++) buf[at + (size_t)b] = (uint8_t)(v >> (8 * b));
                break;
            }
            default: {                                                               /* swap two bytes */
                size_t other = rnd() % n;
                uint8_t t = buf[at];
                buf[at] = buf[other];
                buf[other] = t;
                break;
            }
            }
        }
        uint32_t errors = 0;
        if (exercise(buf, n, &frames, &errors)) refused++;
        else { opened++; if (errors) with_errors++; }
        free(buf);
    }
    alarm(0);
    printf("fuzz iterations=%ld opened=%ld refused=%ld with_errors=%ld frames=%llu\n", count, opened, refused, with_errors, (unsigned long long)frames);
    for (int i = 0; i < nfiles; i++) free(corpus[i]);
    free(corpus);
    free(sizes);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: flactool info|decode|verify|seek|seektest|md5|fuzz ...\n"); return 2; }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "info")) return cmd_info(argv[2]);
    if (!strcmp(cmd, "decode")) return cmd_decode(argc - 2, argv + 2);
    if (!strcmp(cmd, "verify")) return cmd_verify(argv[2]);
    if (!strcmp(cmd, "seek") && argc >= 6) return cmd_seek(argv + 2);
    if (!strcmp(cmd, "md5")) return cmd_md5(argv[2]);
    if (!strcmp(cmd, "fuzz")) return cmd_fuzz(argc - 2, argv + 2);
    if (!strcmp(cmd, "seektest") && argc >= 5) return cmd_seektest(argv[2], strtoull(argv[3], NULL, 10), strtol(argv[4], NULL, 10));
    fprintf(stderr, "flactool: unknown command %s\n", cmd);
    return 2;
}
