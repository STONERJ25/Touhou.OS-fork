/*
 * Host driver for kernel/lib/inflate.c (the kernel's gunzip), built by tests/host/test_inflate.py with AddressSanitizer and UBSan.
 *
 *   inflatetool run [-w WORK] [-l LIMIT] [-f N] [-o OUTFILE] FILE
 *       decodes FILE; one result line on stdout:  rc=-3 in=1234 out=5678 crc=89abcdef adler=0123abcd calls=7 why=...
 *       -w WORK   size of the work area in bytes (default 131072 = GZ_WORK_128K; GZ_MIN_WORK is the smallest)
 *       -l LIMIT  the most output bytes allowed (default 256 MiB)
 *       -f N      the N-th call of the sink fails (1-based): the result must be GZ_E_SINK
 *       -o FILE   write the delivered output there
 *       -q        quiet sink: only count (to time the decoder alone; the checksums in the result line are not computed)
 *   inflatetool batch [-w WORK] [-l LIMIT]
 *       stdin is a sequence of records (u32 length, little-endian, then that many bytes); one result line per record, flushed at once. Every input
 *       is copied into a heap block of exactly its size, so AddressSanitizer sees a read past either end of it.
 *
 * The CRC and the Adler-32 in the result line are computed here, from the bytes the sink received, by code that has nothing to do with the decoder's;
 * the Python side compares them with zlib's for the data it expects.
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inflate.h"

typedef struct {
    uint32_t crc;                   /* bitwise CRC-32 (RFC 1952's definition, no table) */
    uint32_t a, b;                  /* Adler-32 (RFC 1950) */
    uint64_t total;
    unsigned calls, fail_at;
    FILE *out;
    uint8_t *lo, *hi;               /* the window: every piece the sink gets must lie inside it */
    uint64_t limit;
} Probe;

static int g_quiet;                 /* -q: the sink only counts (for timing the decoder; the crc and adler in the result line are then meaningless) */

static int sink(void *ctx, const uint8_t *data, size_t n) {
    Probe *p = ctx;
    p->calls++;
    if (n == 0) { fprintf(stderr, "inflatetool: the sink got an empty piece\n"); abort(); }
    if (p->fail_at && p->calls == p->fail_at) return 1;
    if (p->lo && (data < p->lo || data + n > p->hi)) { fprintf(stderr, "inflatetool: a piece outside the window\n"); abort(); }
    for (size_t i = 0; !g_quiet && i < n; i++) {
        p->crc ^= data[i];
        for (int k = 0; k < 8; k++) p->crc = (p->crc & 1) ? (p->crc >> 1) ^ 0xEDB88320u : p->crc >> 1;
        p->a = (p->a + data[i]) % 65521;
        p->b = (p->b + p->a) % 65521;
    }
    p->total += n;
    if (p->total > p->limit) { fprintf(stderr, "inflatetool: the sink got more than the limit\n"); abort(); }
    if (p->out && fwrite(data, 1, n, p->out) != n) { perror("write"); exit(2); }
    return 0;
}

static void one(const uint8_t *in, size_t n, size_t work_size, uint64_t limit, unsigned fail_at, FILE *out) {
    uint8_t *work = aligned_alloc(8, (work_size + 7) & ~(size_t)7);
    if (!work) { fprintf(stderr, "out of memory\n"); exit(2); }
    Probe p = { .crc = 0xFFFFFFFFu, .a = 1, .fail_at = fail_at, .out = out, .limit = limit,
                .lo = work + GZ_STATE_BYTES, .hi = work + work_size };
    GzInfo info;
    memset(&info, 0xAA, sizeof info);
    int rc = gunzip(in, n, work, work_size, limit, sink, &p, &info);
    if (rc != GZ_E_ARG && (info.out_len != p.total || (!g_quiet && info.crc != ~p.crc))) {      /* the decoder's own count and CRC are of what the sink took: they must agree always */
        fprintf(stderr, "inflatetool: the decoder's count (%" PRIu64 ", %08x) is not what the sink saw (%" PRIu64 ", %08x)\n", info.out_len, info.crc, p.total, ~p.crc);
        abort();
    }
    printf("rc=%d in=%" PRIu64 " out=%" PRIu64 " crc=%08x adler=%08x calls=%u why=%s\n", rc, info.in_pos, p.total, ~p.crc, (p.b << 16) | p.a, p.calls, info.why ? info.why : "-");
    fflush(stdout);
    free(work);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: inflatetool run|batch [-w WORK] [-l LIMIT] [-f N] [-o OUT] [FILE]\n"); return 2; }
    size_t work = GZ_WORK_128K;
    uint64_t limit = 256ull << 20;
    unsigned fail_at = 0;
    const char *outname = NULL, *file = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-w") && i + 1 < argc) work = (size_t)strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) limit = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) fail_at = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outname = argv[++i];
        else if (!strcmp(argv[i], "-q")) g_quiet = 1;
        else if (argv[i][0] != '-' && !file) file = argv[i];
        else { fprintf(stderr, "inflatetool: bad argument %s\n", argv[i]); return 2; }
    }
    if (!strcmp(argv[1], "run")) {
        if (!file) { fprintf(stderr, "inflatetool: run needs a file\n"); return 2; }
        FILE *f = fopen(file, "rb");
        if (!f) { perror(file); return 2; }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc(len ? (size_t)len : 1);       /* exactly the size of the file: a read past the end is an ASan error */
        if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) { fprintf(stderr, "cannot read %s\n", file); return 2; }
        fclose(f);
        FILE *out = outname ? fopen(outname, "wb") : NULL;
        if (outname && !out) { perror(outname); return 2; }
        one(buf, (size_t)len, work, limit, fail_at, out);
        if (out) fclose(out);
        free(buf);
        return 0;
    }
    if (!strcmp(argv[1], "batch")) {
        for (;;) {
            uint8_t lenb[4];
            size_t got = fread(lenb, 1, 4, stdin);
            if (got == 0) break;
            if (got != 4) { fprintf(stderr, "inflatetool: a record header was cut short\n"); return 2; }
            size_t n = (size_t)lenb[0] | (size_t)lenb[1] << 8 | (size_t)lenb[2] << 16 | (size_t)lenb[3] << 24;
            uint8_t *buf = malloc(n);                           /* exactly n bytes (malloc(0) is still a block with red zones around it) */
            if ((!buf && n) || fread(buf, 1, n, stdin) != n) { fprintf(stderr, "inflatetool: a record was cut short\n"); return 2; }
            one(buf, n, work, limit, fail_at, NULL);
            free(buf);
        }
        return 0;
    }
    fprintf(stderr, "inflatetool: unknown mode %s\n", argv[1]);
    return 2;
}
