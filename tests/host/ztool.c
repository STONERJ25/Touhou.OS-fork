// Host test driver for libz: ztool deflate LEVEL FORMAT INCHUNK OUTCHUNK  |  ztool inflate FORMAT INCHUNK OUTCHUNK
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <z.h>

static uint8_t *slurp(FILE *f, size_t *n) {
    size_t cap = 1 << 16, len = 0;
    uint8_t *b = malloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; b = realloc(b, cap); }
        size_t r = fread(b + len, 1, cap - len, f);
        if (r == 0) break;
        len += r;
    }
    *n = len;
    return b;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    size_t n;
    uint8_t *in = slurp(stdin, &n);
    ZStream s;
    memset(&s, 0, sizeof s);
    if (!strcmp(argv[1], "deflate")) {
        int level = atoi(argv[2]), fmt = atoi(argv[3]);
        size_t ic = (size_t)atol(argv[4]), oc = (size_t)atol(argv[5]);
        int rc = z_deflate_init(&s, level, fmt);
        if (rc) { fprintf(stderr, "init %d\n", rc); return 1; }
        if (fmt == Z_GZIP) { strcpy(s.gzip.name, "test.txt"); s.gzip.mtime = 1700000000; }
        uint8_t *out = malloc(oc);
        size_t pos = 0;
        for (;;) {
            size_t take = n - pos < ic ? n - pos : ic;
            s.next_in = in + pos;
            s.avail_in = take;
            int flush = (pos + take == n) ? Z_FINISH : Z_NO_FLUSH;
            for (;;) {
                s.next_out = out;
                s.avail_out = oc;
                rc = z_deflate(&s, flush);
                fwrite(out, 1, oc - s.avail_out, stdout);
                if (rc < 0) { fprintf(stderr, "deflate %d\n", rc); return 1; }
                if (rc == Z_STREAM_END) goto done_d;
                if (s.avail_out != 0 && s.avail_in == 0) break;
            }
            pos += take - s.avail_in;
            if (pos >= n && flush != Z_FINISH) flush = Z_FINISH;
        }
    done_d:
        z_deflate_end(&s);
        return 0;
    }
    if (!strcmp(argv[1], "inflate")) {
        int fmt = atoi(argv[2]);
        size_t ic = (size_t)atol(argv[3]), oc = (size_t)atol(argv[4]);
        int rc = z_inflate_init(&s, fmt);
        if (rc) return 1;
        uint8_t *out = malloc(oc);
        size_t pos = 0;
        for (;;) {
            size_t take = n - pos < ic ? n - pos : ic;
            s.next_in = in + pos;
            s.avail_in = take;
            for (;;) {
                s.next_out = out;
                s.avail_out = oc;
                rc = z_inflate(&s);
                fwrite(out, 1, oc - s.avail_out, stdout);
                if (rc == Z_STREAM_END) {
                    fprintf(stderr, "end, %zu bytes left\n", (size_t)s.avail_in + (n - pos - take));
                    return 0;
                }
                if (rc < 0 && rc != Z_BUF_ERROR) { fprintf(stderr, "inflate error %d (%s)\n", rc, z_strerror(rc)); return 1; }
                if (s.avail_out != 0 && s.avail_in == 0) break;
                if (rc == Z_BUF_ERROR) break;
            }
            pos += take - s.avail_in;
            if (take == 0) { fprintf(stderr, "truncated\n"); return 3; }
        }
    }
    return 2;
}
