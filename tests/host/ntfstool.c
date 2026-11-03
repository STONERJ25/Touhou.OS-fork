/*
 * Host build of the kernel's NTFS parser (kernel/fs/ntfs_core.c, ntfs_name.c, ntfs_lznt1.c, and bitlocker.c when -DWITH_BDE), for tests/host/test_ntfs.py.
 * The very same sources run in the kernel; here they are linked against a file or a block of memory, and built with AddressSanitizer and UBSan so that a
 * mutation fuzz (`fuzz`) can show that a damaged volume costs an error code and nothing worse.
 *
 *   ntfstool IMAGE [--offset N] [--bde] info
 *   ntfstool IMAGE ... ls PATH               one line per visible entry: type size inode mtime name
 *   ntfstool IMAGE ... stat PATH
 *   ntfstool IMAGE ... cat PATH [OFFSET LEN]
 *   ntfstool IMAGE ... readlink PATH [MOUNT]
 *   ntfstool IMAGE ... walk [PATH]           the whole tree: type, path, size, inode, mtime, crc32 of the content (a link: its target); reads every file
 *   ntfstool IMAGE ... rec N                 a dump of MFT record N
 *   ntfstool IMAGE ... free                  free clusters
 *   ntfstool IMAGE ... fuzz SEED COUNT [MUTATIONS]
 *   ntfstool lznt1 IN OUT SIZE               decompress a raw LZNT1 stream (OUT receives the bytes, SIZE is its capacity)
 *   ntfstool IMAGE bde-info | bde-read OFFSET LEN      the BitLocker layer alone (needs -DWITH_BDE)
 */
#include "ntfs_priv.h"
#include <signal.h>
#include <time.h>
#include <unistd.h>
#ifdef WITH_BDE
#include "bitlocker.h"
#endif

typedef struct { FILE *f; u8 *mem; u64 base, size; } Src;

static int src_read(void *ctx, u64 off, void *buf, size_t len) {
    Src *s = ctx;
    if (off > s->size || len > s->size - off) return -EIO;
    if (s->mem) { memcpy(buf, s->mem + s->base + off, len); return 0; }
    if (fseeko(s->f, (off_t)(s->base + off), SEEK_SET)) return -EIO;
    return fread(buf, 1, len, s->f) == len ? 0 : -EIO;
}

static u32 crc_table[256];

static void crc_init(void) {
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static u32 crc_update(u32 crc, const u8 *p, size_t n) {
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 255] ^ (crc >> 8);
    return ~crc;
}

typedef struct { NtfsVol *v; Src src; NtfsDev dev; char mount[64]; } Ctx;

static int open_path(Ctx *c, const char *path, NtfsFile **out) {
    NtfsFile *cur;
    int rc = ntfs_file_open(c->v, 5, &cur);
    if (rc) return rc;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = strtok(buf, "/"); p; p = strtok(NULL, "/")) {
        u64 ref;
        rc = ntfs_dir_lookup(cur, p, &ref);
        ntfs_file_close(cur);
        if (rc) return rc;
        rc = ntfs_file_open(c->v, ref, &cur);
        if (rc) return rc;
    }
    *out = cur;
    return 0;
}

static const char *kind_of(NtfsFile *f) { return f->is_dir ? (ntfs_is_link(f) ? "l" : "d") : (ntfs_is_link(f) ? "l" : "f"); }

/* A file's content as a crc, reading at most `cap` bytes in 64 KiB pieces; returns 0 or -errno. */
static int file_crc(NtfsFile *f, u64 cap, u32 *crc, u64 *total) {
    u8 *buf = malloc(65536);
    u64 off = 0;
    *crc = 0;
    int rc = 0;
    while (off < cap) {
        ssize_t n = ntfs_read(f, off, buf, 65536);
        if (n < 0) { rc = (int)n; break; }
        if (n == 0) break;
        *crc = crc_update(*crc, buf, (size_t)n);
        off += (u64)n;
    }
    *total = off;
    free(buf);
    return rc;
}

static int cmd_walk(Ctx *c, const char *start, u64 cap, bool quiet, u64 *count) {
    struct Frame { NtfsFile *dir; NtfsDirCursor cur; size_t plen; } stack[48];
    int depth = 0;
    char path[2048];
    NtfsFile *top;
    int rc = open_path(c, start, &top);
    if (rc) return rc;
    snprintf(path, sizeof path, "%s", strcmp(start, "/") ? start : "");
    stack[0].dir = top;
    ntfs_dir_cursor_reset(&stack[0].cur);
    stack[0].plen = strlen(path);
    depth = 1;
    u64 entries = 0;
    int worst = 0;
    while (depth > 0 && entries < 200000) {
        struct Frame *fr = &stack[depth - 1];
        NtfsVisible e;
        u32 skipped = 0;
        rc = ntfs_dir_next_visible(fr->dir, &fr->cur, &e, &skipped);
        if (rc < 0) { if (!quiet) printf("E\t%s\t%d\n", path, rc); worst = rc; rc = 0; ntfs_file_close(fr->dir); depth--; path[fr->plen] = 0; continue; }
        if (rc == 0) { ntfs_file_close(fr->dir); depth--; path[fr->plen] = 0; continue; }
        entries++;
        size_t base = fr->plen;
        snprintf(path + base, sizeof path - base, "/%s", e.name);
        NtfsFile *f;
        rc = ntfs_file_open(c->v, e.ino, &f);
        if (rc) { if (!quiet) printf("E\t%s\t%d\n", path, rc); worst = rc; path[base] = 0; continue; }
        if (ntfs_is_link(f)) {
            char tgt[1024];
            int n = ntfs_readlink(f, c->mount, tgt, sizeof tgt);
            if (!quiet) { if (n >= 0) printf("l\t%s\t0\t%lu\t%lu\t%s\n", path, (unsigned long)f->no, (unsigned long)f->mtime, tgt); else printf("l\t%s\t0\t%lu\t%lu\tERR=%d\n", path, (unsigned long)f->no, (unsigned long)f->mtime, n); }
            ntfs_file_close(f);
            path[base] = 0;
        } else if (f->is_dir) {
            if (!quiet) printf("d\t%s\t0\t%lu\t%lu\t-\n", path, (unsigned long)f->no, (unsigned long)f->mtime);
            if (depth < 40) {
                stack[depth].dir = f;
                ntfs_dir_cursor_reset(&stack[depth].cur);
                stack[depth].plen = strlen(path);
                depth++;
            } else {
                ntfs_file_close(f);
                path[base] = 0;
            }
        } else {
            u32 crc;
            u64 total;
            int rr = file_crc(f, cap, &crc, &total);
            if (!quiet) {
                if (rr) printf("f\t%s\t%lu\t%lu\t%lu\tERR=%d\n", path, (unsigned long)f->data.size, (unsigned long)f->no, (unsigned long)f->mtime, rr);
                else printf("f\t%s\t%lu\t%lu\t%lu\t%08x\n", path, (unsigned long)f->data.size, (unsigned long)f->no, (unsigned long)f->mtime, crc);
            }
            ntfs_file_close(f);
            path[base] = 0;
        }
    }
    while (depth > 0) { ntfs_file_close(stack[depth - 1].dir); depth--; }
    if (count) *count = entries;
    return worst;
}

static int load_all(const char *path, u8 **mem, u64 *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -errno;
    fseeko(f, 0, SEEK_END);
    off_t n = ftello(f);
    fseeko(f, 0, SEEK_SET);
    *mem = malloc((size_t)n + 1);
    if (fread(*mem, 1, (size_t)n, f) != (size_t)n) { fclose(f); return -EIO; }
    fclose(f);
    *size = (u64)n;
    return 0;
}

static void on_alarm(int sig) {
    (void)sig;
    static const char m[] = "FUZZ HANG: an iteration ran for more than 20 seconds\n";
    if (write(2, m, sizeof m - 1)) {}
    _exit(99);
}

static u64 rng_state;
static u64 rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* Damage a copy of the image in memory in ways that matter for a parser: bit flips, whole fields set to extremes, copies of one place over another, and
 * truncation; biased towards the boot sector, the MFT and the first megabytes, where the metadata lives. */
static int cmd_fuzz(const char *imgpath, u64 offset, u64 seed, u64 count, u32 nmut) {
    u8 *orig;
    u64 size;
    if (load_all(imgpath, &orig, &size)) { fprintf(stderr, "cannot read %s\n", imgpath); return 2; }
    size -= offset;
    u8 *work = malloc(size + 64);
    u64 last = size;
    while (last > 4096 && !memcmp(orig + offset + last - 4096, "\0\0\0\0\0\0\0\0", 8) && orig[offset + last - 1] == 0) {          /* the used part */
        bool zero = true;
        for (u64 i = 0; i < 4096 && zero; i++) zero = orig[offset + last - 4096 + i] == 0;
        if (!zero) break;
        last -= 4096;
    }
    u64 mft_lo = 0, mft_hi = 0;
    {
        u32 bps = orig[offset + 11] | orig[offset + 12] << 8, spc = orig[offset + 13];
        u64 mft = 0;
        for (int i = 0; i < 8; i++) mft |= (u64)orig[offset + 48 + i] << (8 * i);
        mft_lo = mft * bps * spc;
        mft_hi = mft_lo + 512 * 1024;
        if (mft_hi > size) mft_hi = size;
        if (mft_lo >= mft_hi) mft_lo = mft_hi = 0;
    }
    signal(SIGALRM, on_alarm);
    rng_state = seed * 0x9E3779B97F4A7C15ull + 1;
    u64 mounted = 0, refused = 0, entries_total = 0, maxms = 0;
    for (u64 it = 0; it < count; it++) {
        memcpy(work, orig + offset, size);
        u64 cur_size = size;
        for (u32 m = 0; m < nmut; m++) {
            u64 where, r = rnd() % 100;
            if (r < 45 && mft_hi > mft_lo) where = mft_lo + rnd() % (mft_hi - mft_lo);
            else if (r < 55) where = rnd() % 512;
            else where = rnd() % last;
            switch (rnd() % 6) {
            case 0: work[where] ^= (u8)(1u << (rnd() % 8)); break;
            case 1: work[where] = (u8)rnd(); break;
            case 2: {                                               /* a 2, 4 or 8 byte field set to an extreme */
                static const u64 ext[] = { 0, 1, 0x7FFFFFFFull, 0xFFFFFFFFull, 0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0x10000, 0xFFFF };
                u64 val = ext[rnd() % (sizeof ext / sizeof *ext)];
                u32 w = 1u << (1 + rnd() % 3);
                for (u32 i = 0; i < w && where + i < cur_size; i++) work[where + i] = (u8)(val >> (8 * i));
                break;
            }
            case 3: {                                               /* copy a block over another place */
                u64 from = rnd() % last, n = 1 + rnd() % 256;
                if (from + n <= cur_size && where + n <= cur_size) memmove(work + where, work + from, n);
                break;
            }
            case 4: if (where + 8 <= cur_size) { u64 v = rnd(); memcpy(work + where, &v, 8); } break;
            default: if (rnd() % 8 == 0 && cur_size > 65536) cur_size = cur_size - (rnd() % (cur_size / 4)) / 512 * 512; break;
            }
        }
        alarm(20);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        Ctx c;
        memset(&c, 0, sizeof c);
        c.src.mem = work;
        c.src.size = cur_size;
        c.dev.read = src_read;
        c.dev.ctx = &c.src;
        c.dev.size = cur_size;
        c.dev.sector = 512;
        snprintf(c.mount, sizeof c.mount, "/mnt");
        char why[128];
        NtfsOpts opts = { 0 };
        opts.show_system = (it & 1) != 0;
        int rc = ntfs_vol_open(&c.v, &c.dev, &opts, why, sizeof why);
        if (rc == 0) {
            mounted++;
            u64 n = 0;
            cmd_walk(&c, "/", 1 << 20, true, &n);
            entries_total += n;
            u64 fr;
            ntfs_free_clusters(c.v, &fr);
            ntfs_vol_close(c.v);
        } else {
            refused++;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        alarm(0);
        u64 ms = (u64)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
        if (ms > maxms) maxms = ms;
    }
    printf("fuzz: %lu iterations, %lu mounted, %lu refused, %lu entries walked, slowest %lu ms\n", (unsigned long)count, (unsigned long)mounted,
           (unsigned long)refused, (unsigned long)entries_total, (unsigned long)maxms);
    free(orig);
    free(work);
    return 0;
}

#ifdef WITH_BDE
void bde_test_aes(int decrypt, const u8 *key, unsigned keylen, const u8 in[16], u8 out[16]);
int bde_test_ccm(const u8 *key, unsigned keylen, const u8 nonce[12], u8 *data, size_t n, const u8 tag[16]);
int bde_test_sector(unsigned method, const u8 *key, u32 klen, u64 byte_off, u32 ss, u8 *data, int variant);

static size_t unhex(const char *s, u8 *out, size_t cap) {
    size_t n = 0;
    for (; s[0] && s[1] && n < cap; s += 2) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (u8)v;
    }
    return n;
}

static void puthex(const u8 *p, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\n");
}

/* cipher self-tests driven by hex strings:
 *   aes enc|dec KEY BLOCK                       one AES block
 *   ccm KEY NONCE12 TAG16 DATA                  opens: prints ok and the plaintext, or fail
 *   sector METHOD KEY OFFSET SECTORSIZE DATA [VARIANT]    decrypts one sector of a BitLocker volume */
static int cipher_cmd(int argc, char **argv) {
    u8 key[128], a[16], b[16], c[16], data[4096];
    if (!strcmp(argv[1], "aes") && argc >= 5) {
        size_t kl = unhex(argv[3], key, sizeof key);
        unhex(argv[4], a, 16);
        bde_test_aes(!strcmp(argv[2], "dec"), key, (unsigned)kl, a, b);
        puthex(b, 16);
        return 0;
    }
    if (!strcmp(argv[1], "ccm") && argc >= 6) {
        size_t kl = unhex(argv[2], key, sizeof key);
        unhex(argv[3], a, 12);
        unhex(argv[4], c, 16);
        size_t n = unhex(argv[5], data, sizeof data);
        int rc = bde_test_ccm(key, (unsigned)kl, a, data, n, c);
        if (rc) { printf("fail %d\n", rc); return 0; }
        printf("ok ");
        puthex(data, n);
        return 0;
    }
    if (!strcmp(argv[1], "sector") && argc >= 7) {
        size_t kl = unhex(argv[3], key, sizeof key);
        u32 ss = (u32)strtoul(argv[5], NULL, 0);
        size_t n = unhex(argv[6], data, sizeof data);
        if (n != ss) return 2;
        int rc = bde_test_sector((unsigned)strtoul(argv[2], NULL, 0), key, (u32)kl, strtoull(argv[4], NULL, 0), ss, data, argc > 7 ? atoi(argv[7]) : 0);
        if (rc) { printf("fail %d\n", rc); return 0; }
        puthex(data, n);
        return 0;
    }
    return 2;
}
#endif

int main(int argc, char **argv) {
    crc_init();
#ifdef WITH_BDE
    if (argc >= 3 && (!strcmp(argv[1], "aes") || !strcmp(argv[1], "ccm") || !strcmp(argv[1], "sector"))) return cipher_cmd(argc, argv);
#endif
    if (argc >= 5 && !strcmp(argv[1], "lznt1")) {
        u8 *in;
        u64 n;
        if (load_all(argv[2], &in, &n)) return 2;
        size_t cap = (size_t)strtoul(argv[4], NULL, 0), got = 0;
        u8 *out = malloc(cap + 1);
        int rc = ntfs_lznt1(in, (size_t)n, out, cap, &got);
        if (!rc) {
            FILE *f = fopen(argv[3], "wb");
            fwrite(out, 1, got, f);
            fclose(f);
            printf("%zu\n", got);
        } else {
            fprintf(stderr, "lznt1: damaged stream (%d)\n", rc);
        }
        free(out);
        free(in);
        return rc ? 1 : 0;
    }
    if (argc < 3) { fprintf(stderr, "usage: see the header of tests/host/ntfstool.c\n"); return 2; }
    const char *image = argv[1];
    u64 offset = 0;
    bool use_bde = false;
    NtfsOpts opts = { 0 };
    int i = 2;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; i++) {
        if (!strcmp(argv[i], "--offset") && i + 1 < argc) offset = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--bde")) use_bde = true;
        else if (!strcmp(argv[i], "--system")) opts.show_system = true;
        else if (!strcmp(argv[i], "--dos")) opts.show_dos = true;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (i >= argc) return 2;
    const char *cmd = argv[i++];
    if (!strcmp(cmd, "fuzz")) {
        if (i + 1 >= argc) return 2;
        return cmd_fuzz(image, offset, strtoull(argv[i], NULL, 0), strtoull(argv[i + 1], NULL, 0), i + 2 < argc ? (u32)strtoul(argv[i + 2], NULL, 0) : 4);
    }
    Ctx c;
    memset(&c, 0, sizeof c);
    c.src.f = fopen(image, "rb");
    if (!c.src.f) { perror(image); return 2; }
    fseeko(c.src.f, 0, SEEK_END);
    c.src.size = (u64)ftello(c.src.f) - offset;
    c.src.base = offset;
    c.dev.read = src_read;
    c.dev.ctx = &c.src;
    c.dev.size = c.src.size;
    c.dev.sector = 512;
    snprintf(c.mount, sizeof c.mount, "/mnt/windows");
#ifdef WITH_BDE
    BdeVol *bde = NULL;
    BdeIo io = { src_read, &c.src, c.src.size, 512 };
    if (use_bde || !strcmp(cmd, "bde-info") || !strcmp(cmd, "bde-read")) {
        BdeReport rep;
        int rc = bde_open(&io, &bde, &rep);
        FILE *rf = !strcmp(cmd, "bde-read") ? stderr : stdout;          /* bde-read writes the bytes themselves to stdout */
        fprintf(rf, "%s\n", rep.text);
        if (rc) { fprintf(rf, "bde: not unlocked (%d)\n", rc); return 1; }
        if (!strcmp(cmd, "bde-info")) { bde_close(bde); return 0; }
        if (!strcmp(cmd, "bde-read")) {
            u64 off = strtoull(argv[i], NULL, 0), len = strtoull(argv[i + 1], NULL, 0);
            u8 *buf = malloc(len);
            rc = bde_read(bde, off, buf, len);
            if (rc) fprintf(stderr, "bde_read failed (%d)\n", rc);
            else fwrite(buf, 1, len, stdout);
            free(buf);
            bde_close(bde);
            return rc ? 1 : 0;
        }
        c.dev.read = bde_dev_read;
        c.dev.ctx = bde;
        c.dev.size = bde_size(bde);
    }
#else
    if (use_bde) { fprintf(stderr, "built without BitLocker support\n"); return 2; }
#endif
    char why[160];
    int rc = ntfs_vol_open(&c.v, &c.dev, &opts, why, sizeof why);
    if (rc) {
        printf("open failed: %d %s\n", rc, why);
#ifdef WITH_BDE
        bde_close(bde);
#endif
        return 1;
    }
    NtfsVol *v = c.v;
    int ret = 0;
    if (!strcmp(cmd, "info")) {
        printf("label=%s\nserial=%016lx\nversion=%u.%u\nvol_flags=%#x\nbps=%u\ncluster=%u\nrec_size=%u\nidx_size=%u\nnclusters=%lu\nmft_lcn=%lu\nmft_size=%lu\nnrecords=%lu\nmft_runs=%u\nupcase_builtin=%d\n",
               v->label, (unsigned long)v->serial, v->ver_major, v->ver_minor, v->vol_flags, v->bps, v->cluster, v->rec_size, v->idx_size,
               (unsigned long)v->nclusters, (unsigned long)v->mft_lcn, (unsigned long)v->mft_size, (unsigned long)v->nrecords, v->mft.n, v->upcase_builtin);
    } else if (!strcmp(cmd, "ls")) {
        NtfsFile *d;
        rc = open_path(&c, i < argc ? argv[i] : "/", &d);
        if (rc) { printf("error %d\n", rc); ret = 1; }
        else {
            NtfsDirCursor cur;
            ntfs_dir_cursor_reset(&cur);
            NtfsVisible e;
            u32 skipped = 0;
            while ((rc = ntfs_dir_next_visible(d, &cur, &e, &skipped)) > 0) {
                NtfsFile *f;
                if (ntfs_file_open(v, e.ino, &f)) { printf("? 0 %lu 0 %s\n", (unsigned long)e.ino, e.name); continue; }
                printf("%s %lu %lu %lu %s\n", kind_of(f), (unsigned long)(f->is_dir ? 0 : f->data.size), (unsigned long)f->no, (unsigned long)f->mtime, e.name);
                ntfs_file_close(f);
            }
            if (rc < 0) { printf("error %d\n", rc); ret = 1; }
            if (skipped) printf("skipped %u\n", skipped);
            ntfs_file_close(d);
        }
    } else if (!strcmp(cmd, "stat")) {
        NtfsFile *f;
        rc = open_path(&c, argv[i], &f);
        if (rc) { printf("error %d\n", rc); ret = 1; }
        else {
            printf("kind=%s\nino=%lu\nsize=%lu\nalloc=%lu\nnlink=%u\nattrs=%#x\nctime=%lu\nmtime=%lu\natime=%lu\nreparse=%#x\nresident=%d\nflags=%#x\ncunit=%u\nencrypted=%d\n",
                   kind_of(f), (unsigned long)f->no, (unsigned long)f->data.size, (unsigned long)f->data.alloc, f->nlink, f->attrs, (unsigned long)f->ctime,
                   (unsigned long)f->mtime, (unsigned long)f->atime, f->reparse_tag, f->data.resident, f->data.flags, f->data.cunit, (f->data.flags & NT_AF_ENCRYPTED) != 0);
            ntfs_file_close(f);
        }
    } else if (!strcmp(cmd, "cat")) {
        NtfsFile *f;
        rc = open_path(&c, argv[i], &f);
        if (rc) { fprintf(stderr, "error %d\n", rc); ret = 1; }
        else {
            u64 off = i + 1 < argc ? strtoull(argv[i + 1], NULL, 0) : 0, len = i + 2 < argc ? strtoull(argv[i + 2], NULL, 0) : ~0ull;
            u8 *buf = malloc(65536);
            for (;;) {
                size_t want = len > 65536 ? 65536 : (size_t)len;
                ssize_t n = ntfs_read(f, off, buf, want);
                if (n < 0) { fprintf(stderr, "read error %d\n", (int)n); ret = 1; break; }
                if (n == 0) break;
                fwrite(buf, 1, (size_t)n, stdout);
                off += (u64)n;
                if (len != ~0ull) { len -= (u64)n; if (!len) break; }
            }
            free(buf);
            ntfs_file_close(f);
        }
    } else if (!strcmp(cmd, "readlink")) {
        NtfsFile *f;
        rc = open_path(&c, argv[i], &f);
        if (rc) { printf("error %d\n", rc); ret = 1; }
        else {
            char tgt[1024];
            int n = ntfs_readlink(f, i + 1 < argc ? argv[i + 1] : "/mnt/windows", tgt, sizeof tgt);
            if (n < 0) { printf("error %d\n", n); ret = 1; } else printf("%s\n", tgt);
            ntfs_file_close(f);
        }
    } else if (!strcmp(cmd, "walk")) {
        u64 n = 0;
        ret = cmd_walk(&c, i < argc ? argv[i] : "/", ~0ull, false, &n) ? 1 : 0;
    } else if (!strcmp(cmd, "cache")) {                              /* the same walk twice: what did the second one read from the device? */
        u64 n = 0;
        cmd_walk(&c, i < argc ? argv[i] : "/", ~0ull, true, &n);
        u64 r1 = v->n_reads, hits1 = v->n_rec_hits + v->n_idx_hits;
        cmd_walk(&c, i < argc ? argv[i] : "/", ~0ull, true, &n);
        printf("entries=%lu first_reads=%lu second_reads=%lu second_hits=%lu\n", (unsigned long)n, (unsigned long)r1, (unsigned long)(v->n_reads - r1),
               (unsigned long)(v->n_rec_hits + v->n_idx_hits - hits1));
    } else if (!strcmp(cmd, "rec")) {
        char *text = malloc(65536);
        rc = ntfs_dump_record(v, strtoull(argv[i], NULL, 0), text, 65536);
        if (rc < 0) { printf("error %d\n", rc); ret = 1; } else fwrite(text, 1, (size_t)rc, stdout);
        free(text);
    } else if (!strcmp(cmd, "free")) {
        u64 fr;
        rc = ntfs_free_clusters(v, &fr);
        if (rc) { printf("error %d\n", rc); ret = 1; } else printf("free_clusters=%lu total=%lu\n", (unsigned long)fr, (unsigned long)v->nclusters);
    } else {
        fprintf(stderr, "unknown command %s\n", cmd);
        ret = 2;
    }
    ntfs_vol_close(v);
#ifdef WITH_BDE
    bde_close(bde);
#endif
    return ret;
}
