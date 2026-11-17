/*
 * Host build of the kernel's ext2/ext3/ext4 parser (kernel/fs/ext4_core.c), for tests/host/test_ext4.py. The very same source runs in the kernel; here it is linked against
 * a file or a block of memory and built with AddressSanitizer and UBSan, so that a mutation fuzz (`fuzz`) can show that a damaged volume costs an error code and nothing worse.
 *
 *   ext4tool IMAGE [--offset N] [--chunk N] info
 *   ext4tool IMAGE ... ls PATH               one line per entry: type size ino mode uid gid mtime name
 *   ext4tool IMAGE ... stat PATH             key=value lines
 *   ext4tool IMAGE ... cat PATH [OFFSET LEN]
 *   ext4tool IMAGE ... readlink PATH
 *   ext4tool IMAGE ... walk [PATH]           the whole tree, one tab-separated line per entry (kind path size mode uid gid nlink ino mtime mtime_ns rdev crc|target), reads every file
 *   ext4tool IMAGE ... map PATH              the mapping of the file's logical blocks (lblk kind pblk run)
 *   ext4tool IMAGE ... groups                per block group: inode table, free blocks, free inodes
 *   ext4tool IMAGE ... statfs
 *   ext4tool IMAGE ... reads                 the same walk twice: how many device reads did the second one need
 *   ext4tool IMAGE ... fuzz SEED COUNT [MUTATIONS]
 */
#include "ext4_priv.h"
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

typedef struct { FILE *f; u8 *mem; u64 base, size; } Src;

static int src_read(void *ctx, u64 off, void *buf, size_t len) {
    Src *s = ctx;
    if (off > s->size || len > s->size - off) return -EIO;
    if (s->mem) { memcpy(buf, s->mem + s->base + off, len); return 0; }
    if (fseeko(s->f, (off_t)(s->base + off), SEEK_SET)) return -EIO;
    return fread(buf, 1, len, s->f) == len ? 0 : -EIO;
}

/* the fuzz needs to know which blocks of the volume the parser treats as metadata: bget() calls this for every block it reads (ext4_core.c, host build only) */
void (*ext4_meta_hook)(u64 blk, u32 bsize);

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

typedef struct { Ext4Vol *v; Src src; Ext4Dev dev; u32 chunk; } Ctx;

static int open_path(Ctx *c, const char *path, Ext4File **out) {
    Ext4File *cur;
    int rc = ext4_file_open(c->v, E4_ROOT_INO, &cur);
    if (rc) return rc;
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = strtok(buf, "/"); p; p = strtok(NULL, "/")) {
        u32 ino;
        rc = ext4_dir_lookup(cur, p, &ino);
        ext4_file_close(cur);
        if (rc) return rc;
        rc = ext4_file_open(c->v, ino, &cur);
        if (rc) return rc;
    }
    *out = cur;
    return 0;
}

static char kind_of(const Ext4File *f) {
    switch (f->mode & E4_S_IFMT) {
    case E4_S_IFREG: return 'f';
    case E4_S_IFDIR: return 'd';
    case E4_S_IFLNK: return 'l';
    case E4_S_IFCHR: return 'c';
    case E4_S_IFBLK: return 'b';
    case E4_S_IFIFO: return 'p';
    case E4_S_IFSOCK: return 's';
    default: return '?';
    }
}

/* The crc of a stretch of zero bytes without making the bytes: every byte changes the register linearly, so n bytes are a 32x32 matrix over GF(2) to the n-th power, made by
 * squaring (zmat[k]: the change for 2^k zero bytes, column i for bit i). A file with 5 GiB of holes is checked in seconds this way. */
static u32 zmat[48][32];
static bool zmat_ready;

static u32 mat_vec(const u32 *m, u32 v) {
    u32 r = 0;
    for (int i = 0; v; i++, v >>= 1) if (v & 1) r ^= m[i];
    return r;
}

static u32 crc_zeros(u32 crc, u64 n) {
    if (!zmat_ready) {
        for (int i = 0; i < 32; i++) { u32 x = 1u << i; zmat[0][i] = (x >> 8) ^ crc_table[x & 255]; }
        for (int k = 1; k < 48; k++) for (int i = 0; i < 32; i++) zmat[k][i] = mat_vec(zmat[k - 1], zmat[k - 1][i]);
        zmat_ready = true;
    }
    u32 r = ~crc;
    for (int k = 0; n; k++, n >>= 1) if (n & 1) r = mat_vec(zmat[k], r);
    return ~r;
}

static bool all_zero(const u8 *p, size_t n) {
    static const u8 z[4096];
    for (; n >= sizeof z; n -= sizeof z, p += sizeof z) if (memcmp(p, z, sizeof z)) return false;
    return !n || !memcmp(p, z, n);
}

/* A file's content as a crc, reading at most `cap` bytes in pieces of `chunk` bytes; returns 0 or -errno. */
static int file_crc(Ext4File *f, u64 cap, u32 chunk, u32 *crc, u64 *total) {
    u8 *buf = malloc(chunk);
    u64 off = 0;
    *crc = 0;
    int rc = 0;
    while (off < cap) {
        ssize_t n = ext4_read(f, off, buf, chunk);
        if (n < 0) { rc = (int)n; break; }
        if (n == 0) break;
        *crc = all_zero(buf, (size_t)n) ? crc_zeros(*crc, (u64)n) : crc_update(*crc, buf, (size_t)n);
        off += (u64)n;
    }
    *total = off;
    free(buf);
    return rc;
}

static int cmd_walk(Ctx *c, const char *start, u64 cap, bool quiet, u64 *count) {
    struct Frame { Ext4File *dir; u64 cookie; size_t plen; } stack[48];
    int depth = 0;
    char path[2048];
    Ext4File *top;
    int rc = open_path(c, start, &top);
    if (rc) return rc;
    snprintf(path, sizeof path, "%s", strcmp(start, "/") ? start : "");
    stack[0].dir = top;
    stack[0].cookie = 0;
    stack[0].plen = strlen(path);
    depth = 1;
    u64 entries = 0;
    int worst = 0;
    /* a damaged volume can have a directory inside itself, or a file that many names share: the walk enters each directory once, and (in the quiet walk of the fuzz) reads at most
     * 256 MiB of file content in all, so that the harness's own work stays bounded (the parser is asked questions one at a time and cannot loop on its own) */
    u8 *seen = calloc(c->v->inodes / 8 + 1, 1);
    u64 budget = quiet ? (256ull << 20) : ~0ull;
    if (top->ino <= c->v->inodes) seen[top->ino / 8] |= (u8)(1u << (top->ino % 8));
    while (depth > 0 && entries < 200000) {
        struct Frame *fr = &stack[depth - 1];
        Ext4DirEnt e;
        rc = ext4_dir_next(fr->dir, &fr->cookie, &e);
        if (rc < 0) { if (!quiet) printf("E\t%s\t%d\n", path, rc); worst = rc; ext4_file_close(fr->dir); depth--; path[fr->plen] = 0; continue; }
        if (rc == 0) { ext4_file_close(fr->dir); depth--; path[fr->plen] = 0; continue; }
        entries++;
        size_t base = fr->plen;
        snprintf(path + base, sizeof path - base, "/%s", e.name);
        /* the same name must be found again by a lookup (this also exercises the lookup path on every entry) */
        u32 again = 0;
        int lr = ext4_dir_lookup(fr->dir, e.name, &again);
        if (lr || again != e.ino) { if (!quiet) printf("E\t%s\tlookup %d ino %u/%u\n", path, lr, again, e.ino); worst = lr ? lr : -EIO; }
        Ext4File *f;
        rc = ext4_file_open(c->v, e.ino, &f);
        if (rc) { if (!quiet) printf("E\t%s\t%d\n", path, rc); worst = rc; path[base] = 0; continue; }
        if (!quiet) {
            char k = kind_of(f);
            printf("%c\t%s\t%llu\t%o\t%u\t%u\t%u\t%u\t%lld\t%u\t%llu\t", k, path, (unsigned long long)f->size, f->mode & 07777, f->uid, f->gid, f->nlink, f->ino,
                   (long long)f->mtime, f->mtime_ns, (unsigned long long)f->rdev);
            if (k == 'l') {
                char tgt[4200];
                int n = ext4_readlink(f, tgt, sizeof tgt - 1);
                if (n >= 0) { tgt[n] = 0; printf("%s\n", tgt); } else printf("ERR=%d\n", n);
            } else if (k == 'f') {
                u32 crc;
                u64 total;
                int rr = file_crc(f, cap, c->chunk, &crc, &total);
                if (rr) printf("ERR=%d\n", rr); else printf("%08x\n", crc);
            } else {
                printf("-\n");
            }
        } else if (kind_of(f) == 'f') {
            u32 crc;
            u64 total;
            file_crc(f, cap < budget ? cap : budget, c->chunk, &crc, &total);
            budget = total < budget ? budget - total : 0;
        } else if (kind_of(f) == 'l') {
            char tgt[4200];
            ext4_readlink(f, tgt, sizeof tgt - 1);
        }
        bool fresh = (f->mode & E4_S_IFMT) == E4_S_IFDIR && f->ino <= c->v->inodes && !(seen[f->ino / 8] & (1u << (f->ino % 8)));
        if (fresh && depth < 40) {
            seen[f->ino / 8] |= (u8)(1u << (f->ino % 8));
            stack[depth].dir = f;
            stack[depth].cookie = 0;
            stack[depth].plen = strlen(path);
            depth++;
        } else {
            ext4_file_close(f);
            path[base] = 0;
        }
    }
    while (depth > 0) { ext4_file_close(stack[depth - 1].dir); depth--; }
    free(seen);
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

static u8 *g_work;                                              /* the damaged image of the iteration that is running: kept when it hangs, for the next look */
static u64 g_work_size;

static void on_alarm(int sig) {
    (void)sig;
    static const char m[] = "FUZZ HANG: an iteration ran for more than 20 seconds (the damaged image is in /tmp/ext4-hang.img)\n";
    if (write(2, m, sizeof m - 1)) {}
    int fd = open("/tmp/ext4-hang.img", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        u64 done = 0;
        while (done < g_work_size) {
            ssize_t n = write(fd, g_work + done, g_work_size - done);
            if (n <= 0) break;
            done += (u64)n;
        }
        close(fd);
    }
    _exit(99);
}

static u64 rng_state;
static u64 rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* The metadata blocks of the pristine volume: filled by a walk with the hook set. */
static u64 *meta_list;
static u32 *meta_live;                                          /* per block: how much of it is in use (up to its last non-zero byte), where damage matters */
static size_t meta_n, meta_cap;
static void meta_note(u64 blk, u32 bsize) {
    (void)bsize;
    for (size_t i = 0; i < meta_n; i++) if (meta_list[i] == blk) return;
    if (meta_n == meta_cap) { meta_cap = meta_cap ? meta_cap * 2 : 256; meta_list = realloc(meta_list, meta_cap * sizeof *meta_list); }
    meta_list[meta_n++] = blk;
}

/* Damage a copy of the image in memory in ways that matter for a parser: bit flips, bytes and whole fields set to extremes, copies of one place over another, and (rarely)
 * truncation. Three quarters of the damage lands in the volume's metadata: the superblock, the group descriptors and the blocks the walk of the intact volume read as
 * inode tables, directories, extent trees and indirect blocks. Then the damaged volume is opened and walked, every file read, every name looked up, with a time limit. */
static int cmd_fuzz(const char *imgpath, u64 offset, u64 seed, u64 count, u32 nmut) {
    u8 *orig;
    u64 size;
    if (load_all(imgpath, &orig, &size)) { fprintf(stderr, "cannot read %s\n", imgpath); return 2; }
    size -= offset;
    u8 *work = malloc(size + 64);
    /* which blocks matter: walk the intact volume once */
    u32 bsize = 1024;
    {
        Ctx c;
        memset(&c, 0, sizeof c);
        c.src.mem = orig;
        c.src.base = offset;
        c.src.size = size;
        c.dev.read = src_read;
        c.dev.ctx = &c.src;
        c.dev.size = size;
        c.dev.sector = 512;
        c.chunk = 65536;
        char why[200];
        ext4_meta_hook = meta_note;
        if (ext4_vol_open(&c.v, &c.dev, why, sizeof why)) { fprintf(stderr, "the image to fuzz does not open: %s\n", why); return 2; }
        bsize = c.v->bsize;
        u64 n;
        cmd_walk(&c, "/", 1 << 20, true, &n);
        ext4_vol_close(c.v);
        ext4_meta_hook = NULL;
    }
    meta_live = malloc((meta_n ? meta_n : 1) * sizeof *meta_live);
    for (size_t k = 0; k < meta_n; k++) {
        u32 live = bsize;
        while (live > 1 && orig[offset + meta_list[k] * bsize + live - 1] == 0) live--;
        meta_live[k] = live;
    }
    signal(SIGALRM, on_alarm);
    rng_state = seed * 0x9E3779B97F4A7C15ull + 1;
    u64 mounted = 0, refused = 0, entries_total = 0, maxms = 0;
    u64 last = size;
    while (last > 4096) {                                       /* the used part: the end of the volume is zeros in a sparse image */
        bool zero = true;
        for (u64 i = 0; i < 4096 && zero; i++) zero = orig[offset + last - 4096 + i] == 0;
        if (!zero) break;
        last -= 4096;
    }
    for (u64 it = 0; it < count; it++) {
        memcpy(work, orig + offset, size);
        u64 cur_size = size;
        for (u32 m = 0; m < nmut; m++) {
            u64 where, r = rnd() % 100;
            if (r < 12) where = 1024 + rnd() % 1024;                                          /* the superblock */
            else if (r < 75 && meta_n) { size_t k = rnd() % meta_n; where = meta_list[k] * bsize + rnd() % meta_live[k]; }     /* the used part of a metadata block the walk read */
            else where = rnd() % last;
            if (where >= cur_size) where = rnd() % cur_size;
            switch (rnd() % 6) {
            case 0: work[where] ^= (u8)(1u << (rnd() % 8)); break;
            case 1: work[where] = (u8)rnd(); break;
            case 2: {                                               /* a 2, 4 or 8 byte field set to an extreme */
                static const u64 ext[] = { 0, 1, 0x7FFFFFFFull, 0xFFFFFFFFull, 0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0x10000, 0xFFFF, 0xF30A, 0xEF53 };
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
            default: if (rnd() % 16 == 0 && cur_size > 65536) cur_size = cur_size - (rnd() % (cur_size / 4)) / 512 * 512; break;
            }
        }
        alarm(20);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        Ctx c;
        memset(&c, 0, sizeof c);
        c.src.mem = work;
        g_work = work;
        g_work_size = cur_size;
        c.src.size = cur_size;
        c.dev.read = src_read;
        c.dev.ctx = &c.src;
        c.dev.size = cur_size;
        c.dev.sector = 512;
        c.chunk = (it & 1) ? 4096 : 65536;                          /* both the windowed and the direct read path */
        char why[200];
        int rc = ext4_vol_open(&c.v, &c.dev, why, sizeof why);
        if (rc == 0) {
            mounted++;
            u64 n = 0;
            cmd_walk(&c, "/", 1 << 20, true, &n);
            entries_total += n;
            Ext4Stat st;
            ext4_statfs(c.v, &st);
            char fl[256];
            ext4_feature_text(c.v, fl, sizeof fl);
            /* seeks: directory positions that are not entry boundaries, and reads at odd offsets */
            Ext4File *root;
            if (ext4_file_open(c.v, E4_ROOT_INO, &root) == 0) {
                for (int k = 0; k < 6; k++) {
                    u64 cookie = rnd() % (root->size + 64);
                    Ext4DirEnt e;
                    for (int j = 0; j < 8 && ext4_dir_next(root, &cookie, &e) > 0; j++) {}
                }
                ext4_file_close(root);
            }
            ext4_vol_close(c.v);
        } else {
            refused++;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        alarm(0);
        u64 ms = (u64)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
        if (ms > maxms) maxms = ms;
    }
    printf("fuzz: %lu iterations, %lu mounted, %lu refused, %lu entries walked, slowest %lu ms, %zu metadata blocks in the intact volume\n", (unsigned long)count,
           (unsigned long)mounted, (unsigned long)refused, (unsigned long)entries_total, (unsigned long)maxms, meta_n);
    free(orig);
    free(work);
    free(meta_live);
    return 0;
}

int main(int argc, char **argv) {
    crc_init();
    if (argc < 3) { fprintf(stderr, "usage: see the header of tests/host/ext4tool.c\n"); return 2; }
    const char *image = argv[1];
    u64 offset = 0;
    u32 chunk = 65536;
    int i = 2;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; i++) {
        if (!strcmp(argv[i], "--offset") && i + 1 < argc) offset = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--chunk") && i + 1 < argc) chunk = (u32)strtoul(argv[++i], NULL, 0);
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
    c.chunk = chunk;
    char why[256];
    int rc = ext4_vol_open(&c.v, &c.dev, why, sizeof why);
    if (rc) {
        printf("open failed: %d %s\n", rc, why);
        return 1;
    }
    Ext4Vol *v = c.v;
    int ret = 0;
    if (!strcmp(cmd, "info")) {
        char fl[512];
        ext4_feature_text(v, fl, sizeof fl);
        printf("flavour=%s\nlabel=%s\nrev=%u\nblock_size=%u\nblocks=%llu\nreserved_blocks=%llu\nfree_blocks_sb=%llu\ninodes=%u\nfree_inodes_sb=%u\nfirst_ino=%u\ninode_size=%u\n"
               "blocks_per_group=%u\ninodes_per_group=%u\ngroups=%u\nfirst_data_block=%u\ndesc_size=%u\nratio=%u\nfirst_meta_bg=%u\nstate=%u\ncompat=%#x\nincompat=%#x\nro_compat=%#x\n"
               "features=%s\nuuid=%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
               ext4_flavour(v), v->label, v->rev, v->bsize, (unsigned long long)v->blocks, (unsigned long long)v->r_blocks, (unsigned long long)v->free_blocks_sb, v->inodes,
               v->free_inodes_sb, v->first_ino, v->inode_size, v->bpg, v->ipg, v->ngroups, v->first_data_block, v->desc_size, v->ratio, v->first_meta_bg, v->state, v->compat,
               v->incompat, v->ro_compat, fl, v->uuid[0], v->uuid[1], v->uuid[2], v->uuid[3], v->uuid[4], v->uuid[5], v->uuid[6], v->uuid[7], v->uuid[8], v->uuid[9], v->uuid[10],
               v->uuid[11], v->uuid[12], v->uuid[13], v->uuid[14], v->uuid[15]);
    } else if (!strcmp(cmd, "ls")) {
        Ext4File *d;
        rc = open_path(&c, i < argc ? argv[i] : "/", &d);
        if (rc) { printf("error %d\n", rc); ret = 1; }
        else {
            u64 cookie = 0;
            Ext4DirEnt e;
            while ((rc = ext4_dir_next(d, &cookie, &e)) > 0) {
                Ext4File *f;
                if (ext4_file_open(v, e.ino, &f)) { printf("? 0 %u 0 0 0 0 %s\n", e.ino, e.name); continue; }
                printf("%c %llu %u %o %u %u %lld %s\n", kind_of(f), (unsigned long long)f->size, f->ino, f->mode & 07777, f->uid, f->gid, (long long)f->mtime, e.name);
                ext4_file_close(f);
            }
            if (rc < 0) { printf("error %d\n", rc); ret = 1; }
            ext4_file_close(d);
        }
    } else if (!strcmp(cmd, "stat")) {
        Ext4File *f;
        rc = open_path(&c, argv[i], &f);
        if (rc) { printf("error %d\n", rc); ret = 1; }
        else {
            printf("kind=%c\nino=%u\nmode=%o\nuid=%u\ngid=%u\nnlink=%u\nsize=%llu\nblocks512=%llu\natime=%lld\natime_ns=%u\nctime=%lld\nctime_ns=%u\nmtime=%lld\nmtime_ns=%u\n"
                   "crtime=%lld\ncrtime_ns=%u\nflags=%#x\nextents=%d\ninline=%d\nencrypted=%d\nfast_link=%d\nrdev=%llu\ngeneration=%u\n",
                   kind_of(f), f->ino, f->mode & 07777, f->uid, f->gid, f->nlink, (unsigned long long)f->size, (unsigned long long)f->blocks512, (long long)f->atime, f->atime_ns,
                   (long long)f->ctime, f->ctime_ns, (long long)f->mtime, f->mtime_ns, (long long)f->crtime, f->crtime_ns, f->flags, f->extents, f->inline_data, f->encrypted,
                   f->fast_link, (unsigned long long)f->rdev, f->generation);
            ext4_file_close(f);
        }
    } else if (!strcmp(cmd, "cat")) {
        Ext4File *f;
        rc = open_path(&c, argv[i], &f);
        if (rc) { fprintf(stderr, "error %d\n", rc); ret = 1; }
        else {
            u64 off = i + 1 < argc ? strtoull(argv[i + 1], NULL, 0) : 0, len = i + 2 < argc ? strtoull(argv[i + 2], NULL, 0) : ~0ull;
            u8 *buf = malloc(c.chunk);
            for (;;) {
                size_t want = len > c.chunk ? c.chunk : (size_t)len;
                ssize_t n = ext4_read(f, off, buf, want);
                if (n < 0) { fprintf(stderr, "read error %d\n", (int)n); ret = 1; break; }
                if (n == 0) break;
                fwrite(buf, 1, (size_t)n, stdout);
                off += (u64)n;
                if (len != ~0ull) { len -= (u64)n; if (!len) break; }
            }
            free(buf);
            ext4_file_close(f);
        }
    } else if (!strcmp(cmd, "readlink")) {
        Ext4File *f;
        rc = open_path(&c, argv[i], &f);
        if (rc) { printf("error %d\n", rc); ret = 1; }
        else {
            char tgt[4200];
            int n = ext4_readlink(f, tgt, sizeof tgt - 1);
            if (n < 0) { printf("error %d\n", n); ret = 1; } else { tgt[n] = 0; printf("%s\n", tgt); }
            ext4_file_close(f);
        }
    } else if (!strcmp(cmd, "walk")) {
        u64 n = 0;
        ret = cmd_walk(&c, i < argc ? argv[i] : "/", ~0ull, false, &n) ? 1 : 0;
    } else if (!strcmp(cmd, "lookups")) {                            /* lookups DIR [SEED]: every name of DIR looked up once in a random order (what ls -l does to a directory in sorted order) */
        Ext4File *d;
        rc = open_path(&c, i < argc ? argv[i] : "/", &d);
        if (rc) { printf("error %d\n", rc); ret = 1; }
        else {
            size_t cap = 1024, n = 0;
            char (*names)[E4_NAME_MAX + 1] = malloc(cap * sizeof *names);
            u64 cookie = 0;
            Ext4DirEnt e;
            while (ext4_dir_next(d, &cookie, &e) > 0) {
                if (n == cap) { cap *= 2; names = realloc(names, cap * sizeof *names); }
                memcpy(names[n++], e.name, e.namelen + 1);
            }
            rng_state = (i + 1 < argc ? strtoull(argv[i + 1], NULL, 0) : 1) * 0x9E3779B97F4A7C15ull + 1;
            for (size_t k = n; k > 1; k--) {                           /* Fisher-Yates */
                size_t j = rnd() % k;
                char tmp[E4_NAME_MAX + 1];
                memcpy(tmp, names[k - 1], sizeof tmp);
                memcpy(names[k - 1], names[j], sizeof tmp);
                memcpy(names[j], tmp, sizeof tmp);
            }
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            u64 r0 = v->n_reads, found = 0;
            for (size_t k = 0; k < n; k++) {
                u32 ino;
                if (ext4_dir_lookup(d, names[k], &ino) == 0) found++;
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            printf("names=%zu found=%lu ms=%ld device_reads=%lu\n", n, (unsigned long)found, (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000),
                   (unsigned long)(v->n_reads - r0));
            free(names);
            ext4_file_close(d);
        }
    } else if (!strcmp(cmd, "reads")) {
        u64 n = 0;
        cmd_walk(&c, i < argc ? argv[i] : "/", ~0ull, true, &n);
        u64 r1 = v->n_reads, h1 = v->n_hits;
        cmd_walk(&c, i < argc ? argv[i] : "/", ~0ull, true, &n);
        printf("entries=%lu first_reads=%lu second_reads=%lu second_hits=%lu\n", (unsigned long)n, (unsigned long)r1, (unsigned long)(v->n_reads - r1), (unsigned long)(v->n_hits - h1));
    } else if (!strcmp(cmd, "map")) {
        Ext4File *f;
        rc = open_path(&c, argv[i], &f);
        if (rc) { printf("error %d\n", rc); ret = 1; }
        else {
            u64 nb = (f->size + v->bsize - 1) / v->bsize;
            for (u64 l = 0; l < nb;) {
                Ext4Map m;
                rc = ext4_map_block(f, l, &m);
                if (rc) { printf("error %d at %llu\n", rc, (unsigned long long)l); ret = 1; break; }
                u64 run = m.run > nb - l ? nb - l : m.run;
                printf("%llu %s %llu %llu\n", (unsigned long long)l, m.kind == E4_MAP_MAPPED ? "mapped" : m.kind == E4_MAP_UNWRITTEN ? "unwritten" : "hole",
                       (unsigned long long)m.pblk, (unsigned long long)run);
                l += run;
            }
            ext4_file_close(f);
        }
    } else if (!strcmp(cmd, "groups")) {
        for (u32 g = 0; g < v->ngroups; g++) {
            u64 it;
            u32 fb, fi;
            rc = ext4_group_info(v, g, &it, &fb, &fi);
            if (rc) { printf("error %d at group %u\n", rc, g); ret = 1; break; }
            printf("%u %llu %u %u\n", g, (unsigned long long)it, fb, fi);
        }
    } else if (!strcmp(cmd, "statfs")) {
        Ext4Stat st;
        rc = ext4_statfs(v, &st);
        if (rc) { printf("error %d\n", rc); ret = 1; }
        else printf("bsize=%llu blocks=%llu bfree=%llu bavail=%llu files=%llu ffree=%llu\n", (unsigned long long)st.bsize, (unsigned long long)st.blocks, (unsigned long long)st.bfree,
                    (unsigned long long)st.bavail, (unsigned long long)st.files, (unsigned long long)st.ffree);
    } else {
        fprintf(stderr, "unknown command %s\n", cmd);
        ret = 2;
    }
    ext4_vol_close(v);
    return ret;
}
