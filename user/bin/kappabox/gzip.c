/* gzip gunzip zcat: compress and decompress files with libz (DEFLATE in the gzip wrapper). */
#include "kappabox.h"
#include <z.h>
#include <utime.h>

#define CHUNK 65536

static int g_level = 6;
static int g_stdout, g_decomp, g_force, g_keep, g_quiet, g_test, g_verbose, g_recurse, g_list, g_noname;
static int g_warned;

typedef struct { uint64_t in, out; uint32_t mtime; char name[256]; } Result;

static int write_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static const char *base_of(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* Compresses everything readable from `in` to `out` as one gzip member. */
static int compress_fd(int in, int out, const char *name, uint32_t mtime, Result *r) {
    ZStream s;
    memset(&s, 0, sizeof s);
    int rc = z_deflate_init(&s, g_level, Z_GZIP);
    if (rc) { kb_error("%s", z_strerror(rc)); return -1; }
    if (name && !g_noname) strlcpy(s.gzip.name, base_of(name), sizeof s.gzip.name);
    s.gzip.mtime = g_noname ? 0 : mtime;
    uint8_t *ibuf = malloc(CHUNK), *obuf = malloc(CHUNK);
    if (!ibuf || !obuf) { free(ibuf); free(obuf); z_deflate_end(&s); kb_error("out of memory"); return -1; }
    int result = 0;
    for (;;) {
        ssize_t n = read(in, ibuf, CHUNK);
        if (n < 0) { if (errno == EINTR) continue; kb_error("%s: read error: %s", name ? name : "stdin", strerror(errno)); result = -1; break; }
        int flush = n == 0 ? Z_FINISH : Z_NO_FLUSH;
        s.next_in = ibuf;
        s.avail_in = (size_t)n;
        for (;;) {
            s.next_out = obuf;
            s.avail_out = CHUNK;
            rc = z_deflate(&s, flush);
            if (rc < 0) { kb_error("%s", z_strerror(rc)); result = -1; break; }
            size_t got = CHUNK - s.avail_out;
            if (got && write_all(out, obuf, got) < 0) { kb_error("write error: %s", strerror(errno)); result = -1; break; }
            if (rc == Z_STREAM_END) break;
            if (s.avail_out != 0 && s.avail_in == 0 && flush == Z_NO_FLUSH) break;
        }
        if (result || n == 0) break;
    }
    r->in = s.total_in;
    r->out = s.total_out;
    z_deflate_end(&s);
    free(ibuf);
    free(obuf);
    return result;
}

/* Decompresses every gzip member readable from `in` to `out` (out < 0: only check). */
static int decompress_fd(int in, int out, const char *name, Result *r) {
    ZStream s;
    memset(&s, 0, sizeof s);
    int rc = z_inflate_init(&s, Z_GZIP);
    if (rc) { kb_error("%s", z_strerror(rc)); return -1; }
    uint8_t *ibuf = malloc(CHUNK), *obuf = malloc(CHUNK);
    if (!ibuf || !obuf) { free(ibuf); free(obuf); z_inflate_end(&s); kb_error("out of memory"); return -1; }
    int result = 0, members = 0, fresh = 1;
    uint64_t total_in = 0, total_out = 0;
    r->mtime = 0;
    r->name[0] = 0;
    for (;;) {
        ssize_t n = read(in, ibuf, CHUNK);
        if (n < 0) { if (errno == EINTR) continue; kb_error("%s: read error: %s", name, strerror(errno)); result = -1; break; }
        if (n == 0) {
            if (!fresh || members == 0) { kb_error("%s: unexpected end of file", name); result = -1; }
            break;
        }
        total_in += (uint64_t)n;
        s.next_in = ibuf;
        s.avail_in = (size_t)n;
        while (s.avail_in) {
            s.next_out = obuf;
            s.avail_out = CHUNK;
            uint64_t before = s.total_in;
            rc = z_inflate(&s);
            size_t got = CHUNK - s.avail_out;
            if (s.total_in != before) fresh = 0;
            if (got) {
                total_out += got;
                if (out >= 0 && write_all(out, obuf, got) < 0) { kb_error("write error: %s", strerror(errno)); result = -1; goto done; }
            }
            if (rc == Z_STREAM_END) {
                if (members == 0) { r->mtime = s.gzip.mtime; strlcpy(r->name, s.gzip.name, sizeof r->name); }
                members++;
                z_inflate_reset(&s);
                fresh = 1;
                continue;
            }
            if (rc == Z_ERR_FORMAT && members > 0 && fresh) {
                if (!g_quiet) kb_error("%s: decompression OK, trailing garbage ignored", name);
                g_warned = 1;
                goto done;
            }
            if (rc < 0 && rc != Z_BUF_ERROR) {
                kb_error("%s: %s", name, rc == Z_ERR_FORMAT ? "not in gzip format" : z_strerror(rc));
                result = -1;
                goto done;
            }
            if (rc == Z_BUF_ERROR) break;
        }
    }
done:
    r->in = total_in;
    r->out = total_out;
    z_inflate_end(&s);
    free(ibuf);
    free(obuf);
    return result;
}

static void report(const char *name, const Result *r, int compressing) {
    if (!g_verbose) return;
    uint64_t big = compressing ? r->in : r->out, small = compressing ? r->out : r->in;
    int pct = big ? (int)(100 - (small * 100 + big / 2) / big) : 0;
    fprintf(stderr, "%s:\t%3d.%d%% -- %s\n", name, pct, 0, compressing ? "replaced with gzip file" : "replaced with uncompressed file");
}

static const char *suffix_of(const char *name, size_t *keep_len, const char **append) {
    size_t n = strlen(name);
    static const struct { const char *suf; const char *add; } sufs[] = {
        { ".gz", "" }, { ".tgz", ".tar" }, { ".taz", ".tar" }, { "-gz", "" }, { ".z", "" }, { "_z", "" },
    };
    for (unsigned i = 0; i < sizeof sufs / sizeof sufs[0]; i++) {
        size_t sl = strlen(sufs[i].suf);
        if (n > sl && !strcasecmp(name + n - sl, sufs[i].suf)) { *keep_len = n - sl; *append = sufs[i].add; return sufs[i].suf; }
    }
    return NULL;
}

static void copy_times(const char *path, const struct stat *st) {
    struct utimbuf ut = { st->st_atime, st->st_mtime };
    utime(path, &ut);
}

static int do_file(const char *name);

static int do_dir(const char *name) {
    DIR *d = opendir(name);
    if (!d) { kb_error("%s: %s", name, strerror(errno)); return 1; }
    char **names = NULL;
    int n = 0, rc = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        names = realloc(names, (size_t)(n + 1) * sizeof *names);
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        char *child = kb_join(name, names[i]);
        if (do_file(child)) rc = 1;
        free(child);
        free(names[i]);
    }
    free(names);
    return rc;
}

static int do_file(const char *name) {
    struct stat st;
    if (lstat(name, &st) < 0) { kb_error("%s: %s", name, strerror(errno)); return 1; }
    if (S_ISDIR(st.st_mode)) {
        if (g_recurse) return do_dir(name);
        if (!g_quiet) kb_error("%s is a directory -- ignored", name);
        g_warned = 1;
        return 0;
    }
    if (!S_ISREG(st.st_mode) && !(g_stdout && !g_decomp)) {
        if (!g_quiet) kb_error("%s is not a regular file -- ignored", name);
        g_warned = 1;
        return 0;
    }
    char outname[PATH_MAX];
    Result r;
    memset(&r, 0, sizeof r);
    int in = open(name, O_RDONLY);
    if (in < 0) { kb_error("%s: %s", name, strerror(errno)); return 1; }

    if (g_list) {                                   /* gzip -l: sizes from the trailer of the (last) member */
        struct stat ist;
        uint8_t tail[8];
        fstat(in, &ist);
        if (ist.st_size < 18 || lseek(in, -8, SEEK_END) < 0 || read(in, tail, 8) != 8) { kb_error("%s: not in gzip format", name); close(in); return 1; }
        uint32_t crc = (uint32_t)tail[0] | (uint32_t)tail[1] << 8 | (uint32_t)tail[2] << 16 | (uint32_t)tail[3] << 24;
        uint32_t isz = (uint32_t)tail[4] | (uint32_t)tail[5] << 8 | (uint32_t)tail[6] << 16 | (uint32_t)tail[7] << 24;
        int pct = isz ? (int)(100 - ((uint64_t)ist.st_size * 100 + isz / 2) / isz) : 0;
        printf("%10lld %10u %3d.%d%% %08x %s\n", (long long)ist.st_size, isz, pct, 0, crc, name);
        close(in);
        return 0;
    }

    if (g_decomp || g_test) {
        size_t keep = 0;
        const char *add = "";
        const char *suf = suffix_of(name, &keep, &add);
        if (!suf && !g_stdout && !g_test) {
            if (!g_quiet) kb_error("%s: unknown suffix -- ignored", name);
            close(in);
            g_warned = 1;
            return 0;
        }
        int out = -1;
        int to_file = !g_stdout && !g_test;
        if (to_file) {
            snprintf(outname, sizeof outname, "%.*s%s", (int)keep, name, add);
            if (!g_force && access(outname, F_OK) == 0) { kb_error("%s already exists; not overwritten", outname); close(in); return 1; }
            out = open(outname, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
            if (out < 0) { kb_error("%s: %s", outname, strerror(errno)); close(in); return 1; }
        } else if (g_stdout) out = 1;
        int rc = decompress_fd(in, out, name, &r);
        close(in);
        if (to_file) {
            close(out);
            if (rc) { unlink(outname); return 1; }
            struct stat ost = st;
            if (!g_noname && r.mtime) ost.st_mtime = r.mtime;
            copy_times(outname, &ost);
            if (!g_keep) unlink(name);
        }
        if (!rc && g_test && g_verbose) fprintf(stderr, "%s:\t OK\n", name);
        if (!rc) report(name, &r, 0);
        return rc ? 1 : 0;
    }

    size_t keep = 0;
    const char *add = "";
    if (suffix_of(name, &keep, &add) && !strcasecmp(name + keep, ".gz") && !g_stdout) {
        if (!g_quiet) kb_error("%s already has .gz suffix -- unchanged", name);
        close(in);
        g_warned = 1;
        return 0;
    }
    int out = 1;
    if (!g_stdout) {
        snprintf(outname, sizeof outname, "%s.gz", name);
        if (!g_force && access(outname, F_OK) == 0) { kb_error("%s already exists; not overwritten", outname); close(in); return 1; }
        out = open(outname, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
        if (out < 0) { kb_error("%s: %s", outname, strerror(errno)); close(in); return 1; }
    }
    int rc = compress_fd(in, out, name, (uint32_t)st.st_mtime, &r);
    close(in);
    if (!g_stdout) {
        close(out);
        if (rc) { unlink(outname); return 1; }
        copy_times(outname, &st);
        if (!g_keep) unlink(name);
    }
    if (!rc) report(name, &r, 1);
    return rc ? 1 : 0;
}

static int run(int argc, char **argv, int decomp, int to_stdout) {
    g_level = 6;
    g_stdout = to_stdout;
    g_decomp = decomp;
    g_force = g_keep = g_quiet = g_test = g_verbose = g_recurse = g_list = g_noname = g_warned = 0;
    optind = 1;
    int c;
    while ((c = getopt(argc, argv, "cdfklnNqrtv123456789")) != -1) {
        switch (c) {
        case 'c': g_stdout = 1; break;
        case 'd': g_decomp = 1; break;
        case 'f': g_force = 1; break;
        case 'k': g_keep = 1; break;
        case 'l': g_list = 1; break;
        case 'n': g_noname = 1; break;
        case 'N': g_noname = 0; break;
        case 'q': g_quiet = 1; break;
        case 'r': g_recurse = 1; break;
        case 't': g_test = 1; g_decomp = 1; break;
        case 'v': g_verbose = 1; break;
        case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': g_level = c - '0'; break;
        default:
            fprintf(stderr, "usage: %s [-cdfklnqrtv1-9] [FILE...]\n", kb_name);
            return 2;
        }
    }
    int rc = 0;
    if (optind >= argc) {
        if (isatty(g_decomp ? 0 : 1) && !g_force && !g_decomp) {
            kb_error("compressed data not written to a terminal. Use -f to force compression.");
            return 1;
        }
        Result r;
        memset(&r, 0, sizeof r);
        int out = g_test ? -1 : 1;
        if (g_decomp) rc = decompress_fd(0, out, "stdin", &r) ? 1 : 0;
        else rc = compress_fd(0, 1, NULL, 0, &r) ? 1 : 0;
        return rc ? rc : (g_warned ? 2 : 0);
    }
    for (int i = optind; i < argc; i++) {
        if (!strcmp(argv[i], "-")) {
            Result r;
            memset(&r, 0, sizeof r);
            int e = g_decomp ? decompress_fd(0, g_test ? -1 : 1, "stdin", &r) : compress_fd(0, 1, NULL, 0, &r);
            if (e) rc = 1;
        } else if (do_file(argv[i])) rc = 1;
    }
    return rc ? rc : (g_warned ? 2 : 0);
}

APPLET(gzip) { return run(argc, argv, 0, 0); }
APPLET(gunzip) { return run(argc, argv, 1, 0); }
APPLET(zcat) { return run(argc, argv, 1, 1); }
