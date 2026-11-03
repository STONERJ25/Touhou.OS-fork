/*
 * zip, unzip: the PKZIP archive format (stored and deflated entries, directories, Unix modes and symbolic links, the
 * "UT" extended timestamp). Reading is bounds-checked against the file size; ZIP64, encryption and multi-disk archives
 * are refused with a clear message.
 */
#include "kappabox.h"
#include <stdarg.h>
#include <z.h>
#include <utime.h>
#include <fnmatch.h>

#define IOBUF 65536
#define MAX_ENTRIES 65535
#define MAX_CD (64u << 20)
#define MAX_UNPACKED (2000000000ULL)

typedef struct {
    char *name;
    uint16_t made, flags, method;
    uint32_t dostime, crc, csize, usize, attr, lh_off;
    uint32_t mtime;                             /* seconds since 1970 (UTC); 0 when unknown */
    int has_ut;
} ZEntry;

static int g_err;

static void zerr(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void zerr(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s: ", kb_name);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    g_err = 1;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

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

static int read_full(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* DOS date and time are local time without a zone. */
static uint32_t to_dos(time_t t) {
    struct tm tm;
    localtime_r(&t, &tm);
    int year = tm.tm_year + 1900;
    if (year < 1980) return (1u << 21) | (1u << 16);                        /* 1980-01-01 00:00 */
    if (year > 2107) year = 2107;
    return ((uint32_t)(year - 1980) << 25) | ((uint32_t)(tm.tm_mon + 1) << 21) | ((uint32_t)tm.tm_mday << 16) |
           ((uint32_t)tm.tm_hour << 11) | ((uint32_t)tm.tm_min << 5) | (uint32_t)(tm.tm_sec / 2);
}

static time_t from_dos(uint32_t d) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = (int)((d >> 25) & 0x7F) + 80;
    tm.tm_mon = (int)((d >> 21) & 15) - 1;
    tm.tm_mday = (int)((d >> 16) & 31);
    tm.tm_hour = (int)((d >> 11) & 31);
    tm.tm_min = (int)((d >> 5) & 63);
    tm.tm_sec = (int)(d & 31) * 2;
    tm.tm_isdst = -1;
    if (tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1) return 0;
    return mktime(&tm);
}

/* ---------------------------------------------------------------- reading the central directory */

static ZEntry *read_directory(int fd, int *count, off_t *size_out) {
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 22) { zerr("not a zip archive (too short)"); return NULL; }
    size_t tail = size < 65557 ? (size_t)size : 65557;
    uint8_t *buf = malloc(tail);
    if (!buf) { zerr("out of memory"); return NULL; }
    if (lseek(fd, size - (off_t)tail, SEEK_SET) < 0 || read_full(fd, buf, tail) < 0) { free(buf); zerr("read error"); return NULL; }
    long eocd = -1;
    for (long i = (long)tail - 22; i >= 0; i--)
        if (buf[i] == 'P' && buf[i + 1] == 'K' && buf[i + 2] == 5 && buf[i + 3] == 6) { eocd = i; break; }
    if (eocd < 0) { free(buf); zerr("not a zip archive (no end-of-central-directory record)"); return NULL; }
    const uint8_t *e = buf + eocd;
    unsigned disk = le16(e + 4), cd_disk = le16(e + 6), n_here = le16(e + 8), n_total = le16(e + 10);
    uint32_t cd_size = le32(e + 12), cd_off = le32(e + 16);
    free(buf);
    if (disk || cd_disk || n_here != n_total) { zerr("multi-disk archives are not supported"); return NULL; }
    if (n_total == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) { zerr("zip64 archives are not supported"); return NULL; }
    if (cd_size > MAX_CD || (uint64_t)cd_off + cd_size > (uint64_t)size) { zerr("damaged archive: central directory is outside the file"); return NULL; }
    uint8_t *cd = malloc(cd_size ? cd_size : 1);
    if (!cd) { zerr("out of memory"); return NULL; }
    if (lseek(fd, cd_off, SEEK_SET) < 0 || read_full(fd, cd, cd_size) < 0) { free(cd); zerr("read error"); return NULL; }
    ZEntry *ents = calloc((size_t)n_total + 1, sizeof *ents);
    if (!ents) { free(cd); zerr("out of memory"); return NULL; }
    size_t pos = 0;
    int n = 0;
    for (; n < (int)n_total; n++) {
        if (pos + 46 > cd_size || le32(cd + pos) != 0x02014b50u) { zerr("damaged archive: bad central directory entry %d", n); goto fail; }
        const uint8_t *h = cd + pos;
        ZEntry *z = &ents[n];
        unsigned nl = le16(h + 28), xl = le16(h + 30), cl = le16(h + 32);
        if (pos + 46 + nl + xl + cl > cd_size) { zerr("damaged archive: entry %d runs past the directory", n); goto fail; }
        z->made = le16(h + 4);
        z->flags = le16(h + 8);
        z->method = le16(h + 10);
        z->dostime = (uint32_t)le16(h + 14) << 16 | le16(h + 12);
        z->crc = le32(h + 16);
        z->csize = le32(h + 20);
        z->usize = le32(h + 24);
        z->attr = le32(h + 38);
        z->lh_off = le32(h + 42);
        z->name = malloc(nl + 1);
        if (!z->name) { zerr("out of memory"); goto fail; }
        memcpy(z->name, h + 46, nl);
        z->name[nl] = 0;
        if (strlen(z->name) != nl) { zerr("damaged archive: name with a NUL byte"); goto fail; }
        if (z->csize == 0xFFFFFFFFu || z->usize == 0xFFFFFFFFu || z->lh_off == 0xFFFFFFFFu) { zerr("zip64 entries are not supported (%s)", z->name); goto fail; }
        const uint8_t *x = h + 46 + nl;                                     /* extra fields: look for the UT timestamp */
        size_t xi = 0;
        while (xi + 4 <= xl) {
            unsigned id = le16(x + xi), len = le16(x + xi + 2);
            if (xi + 4 + len > xl) break;
            if (id == 0x5455 && len >= 5 && (x[xi + 4] & 1)) { z->mtime = le32(x + xi + 5); z->has_ut = 1; }
            xi += 4 + len;
        }
        if (!z->has_ut) { time_t t = from_dos(z->dostime); z->mtime = t > 0 ? (uint32_t)t : 0; }
        pos += 46 + nl + xl + cl;
    }
    free(cd);
    *count = n;
    *size_out = size;
    return ents;
fail:
    for (int i = 0; i <= n && i < (int)n_total; i++) free(ents[i].name);
    free(ents);
    free(cd);
    return NULL;
}

static void free_entries(ZEntry *e, int n) {
    for (int i = 0; i < n; i++) free(e[i].name);
    free(e);
}

/* ---------------------------------------------------------------- unzip */

static int u_list, u_test, u_pipe, u_over, u_never, u_quiet;

static const char *safe_name(const char *name) {
    const char *p = name;
    while (*p == '/') p++;
    if (!*p) return NULL;
    for (const char *q = p; *q;) {
        const char *e = strchr(q, '/');
        size_t l = e ? (size_t)(e - q) : strlen(q);
        if (l == 2 && q[0] == '.' && q[1] == '.') return NULL;
        if (l == 0 && e) { q = e + 1; continue; }
        q += l;
        while (*q == '/') q++;
    }
    return p;
}

static int make_parents(const char *path) {
    char tmp[4096];
    strlcpy(tmp, path, sizeof tmp);
    for (char *s = tmp + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        struct stat st;
        if (lstat(tmp, &st) == 0) {
            if (S_ISLNK(st.st_mode)) { *s = '/'; errno = ELOOP; return -1; }
            if (!S_ISDIR(st.st_mode)) { *s = '/'; errno = ENOTDIR; return -1; }
        } else if (mkdir(tmp, 0777) < 0 && errno != EEXIST) { *s = '/'; return -1; }
        *s = '/';
    }
    return 0;
}

/* Finds the data of an entry and returns its offset, or -1. */
static off_t data_offset(int fd, const ZEntry *z, off_t file_size) {
    uint8_t lh[30];
    if ((off_t)z->lh_off + 30 > file_size || lseek(fd, z->lh_off, SEEK_SET) < 0 || read_full(fd, lh, 30) < 0 || le32(lh) != 0x04034b50u) {
        zerr("%s: damaged archive (bad local header)", z->name);
        return -1;
    }
    off_t off = (off_t)z->lh_off + 30 + le16(lh + 26) + le16(lh + 28);
    if ((uint64_t)off + z->csize > (uint64_t)file_size) { zerr("%s: damaged archive (data runs past the end of the file)", z->name); return -1; }
    return off;
}

/* Decompresses (or copies) the entry to out (out < 0: only verify). Returns 0 when the CRC and size match. */
static int unpack_entry(int fd, const ZEntry *z, off_t file_size, int out, uint8_t **mem, size_t *mem_len) {
    if (z->flags & 1) { zerr("%s: encrypted entries are not supported", z->name); return -1; }
    if (z->method != 0 && z->method != 8) { zerr("%s: compression method %u is not supported", z->name, z->method); return -1; }
    if (z->usize > MAX_UNPACKED) { zerr("%s: too large", z->name); return -1; }
    off_t off = data_offset(fd, z, file_size);
    if (off < 0 || lseek(fd, off, SEEK_SET) < 0) return -1;
    uint8_t *ibuf = malloc(IOBUF), *obuf = malloc(IOBUF);
    uint8_t *collect = NULL;
    size_t collected = 0;
    if (mem) { collect = malloc((size_t)z->usize + 1); *mem = collect; if (!collect) { free(ibuf); free(obuf); zerr("out of memory"); return -1; } }
    if (!ibuf || !obuf) { free(ibuf); free(obuf); free(collect); zerr("out of memory"); return -1; }
    uint32_t crc = 0;
    uint64_t produced = 0, left = z->csize;
    int rc = 0;
    ZStream s;
    memset(&s, 0, sizeof s);
    if (z->method == 8 && z_inflate_init(&s, Z_RAW)) { free(ibuf); free(obuf); free(collect); zerr("out of memory"); return -1; }
    int done = 0;
    while (!done) {
        size_t want = left < IOBUF ? (size_t)left : IOBUF;
        if (want && read_full(fd, ibuf, want) < 0) { zerr("%s: read error", z->name); rc = -1; break; }
        left -= want;
        const uint8_t *chunk = ibuf;
        size_t chunk_len = want;
        if (z->method == 0) {
            if (produced + chunk_len > z->usize) { zerr("%s: stored size is larger than recorded", z->name); rc = -1; break; }
            crc = z_crc32(crc, chunk, chunk_len);
            if (out >= 0 && chunk_len && write_all(out, chunk, chunk_len) < 0) { zerr("%s: write error: %s", z->name, strerror(errno)); rc = -1; break; }
            if (collect) memcpy(collect + collected, chunk, chunk_len), collected += chunk_len;
            produced += chunk_len;
            if (left == 0) done = 1;
            continue;
        }
        s.next_in = chunk;
        s.avail_in = chunk_len;
        for (;;) {
            s.next_out = obuf;
            s.avail_out = IOBUF;
            int r = z_inflate(&s);
            size_t got = IOBUF - s.avail_out;
            if (got) {
                if (produced + got > z->usize) { zerr("%s: data is larger than recorded (damaged archive)", z->name); rc = -1; break; }
                crc = z_crc32(crc, obuf, got);
                if (out >= 0 && write_all(out, obuf, got) < 0) { zerr("%s: write error: %s", z->name, strerror(errno)); rc = -1; break; }
                if (collect) memcpy(collect + collected, obuf, got), collected += got;
                produced += got;
            }
            if (r == Z_STREAM_END) { done = 1; break; }
            if (r < 0 && r != Z_BUF_ERROR) { zerr("%s: %s", z->name, z_strerror(r)); rc = -1; break; }
            if (s.avail_in == 0 && s.avail_out != 0) break;
            if (r == Z_BUF_ERROR) break;
        }
        if (rc) break;
        if (!done && left == 0 && s.avail_in == 0) { zerr("%s: compressed data ends early (truncated archive)", z->name); rc = -1; break; }
    }
    if (z->method == 8) z_inflate_end(&s);
    free(ibuf);
    free(obuf);
    if (!rc && (produced != z->usize || crc != z->crc)) {
        zerr("%s: %s", z->name, produced != z->usize ? "size mismatch (damaged archive)" : "CRC error (damaged archive)");
        rc = -1;
    }
    if (mem) { if (rc) { free(collect); *mem = NULL; } else { collect[collected] = 0; if (mem_len) *mem_len = collected; } }
    return rc;
}

static int extract_one(int fd, const ZEntry *z, off_t file_size, const char *dir) {
    const char *name = safe_name(z->name);
    if (!name) { zerr("%s: skipping a member with an unsafe name", z->name); return -1; }
    char path[4096];
    snprintf(path, sizeof path, "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] != '/' ? "/" : "", name);
    size_t pl = strlen(path);
    int is_dir = pl && path[pl - 1] == '/';
    while (pl > 1 && path[pl - 1] == '/') path[--pl] = 0;
    unsigned unix_mode = (z->made >> 8) == 3 ? (z->attr >> 16) : 0;
    if (make_parents(path) < 0) { zerr("%s: cannot create parent directories: %s", path, strerror(errno)); return -1; }
    struct stat st;
    int exists = lstat(path, &st) == 0;
    if (is_dir || (unix_mode && S_ISDIR(unix_mode))) {
        if (exists && !S_ISDIR(st.st_mode)) { zerr("%s: exists and is not a directory", path); return -1; }
        if (!exists && mkdir(path, 0777) < 0) { zerr("%s: %s", path, strerror(errno)); return -1; }
        if (!u_quiet) {
            size_t nl = strlen(name);
            while (nl > 1 && name[nl - 1] == '/') nl--;
            printf("   creating: %.*s/\n", (int)nl, name);
        }
        return 0;
    }
    if (exists && !u_over) {
        if (!u_never) fprintf(stderr, "%s: %s already exists; skipped (use -o to overwrite)\n", kb_name, path);
        return 0;
    }
    if (exists) { if (S_ISDIR(st.st_mode)) { zerr("%s: is a directory", path); return -1; } unlink(path); }
    if (unix_mode && S_ISLNK(unix_mode)) {                                  /* the data of a symlink entry is its target */
        uint8_t *target = NULL;
        size_t tl = 0;
        if (unpack_entry(fd, z, file_size, -1, &target, &tl) < 0) return -1;
        if (tl > 4000) { free(target); zerr("%s: symbolic link target too long", path); return -1; }
        int r = symlink((char *)target, path);
        if (r < 0) zerr("%s: cannot create symbolic link: %s", path, strerror(errno));
        free(target);
        if (!u_quiet) printf("  linking: %s\n", name);
        return r;
    }
    int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out < 0) { zerr("%s: %s", path, strerror(errno)); return -1; }
    int rc = unpack_entry(fd, z, file_size, out, NULL, NULL);
    close(out);
    if (rc) { unlink(path); return -1; }
    if (unix_mode) chmod(path, unix_mode & 0777);
    else { mode_t um = umask(0); umask(um); chmod(path, 0666 & ~um); }
    if (z->mtime) { struct timeval tv[2] = { { (time_t)z->mtime, 0 }, { (time_t)z->mtime, 0 } }; utimes(path, tv); }
    if (!u_quiet) printf("%s %s\n", z->method == 8 ? "  inflating:" : " extracting:", name);
    return 0;
}

static int member_wanted(const char *name, char **pats, int np) {
    if (!np) return 1;
    for (int i = 0; i < np; i++) if (!fnmatch(pats[i], name, 0)) return 1;
    return 0;
}

APPLET(unzip) {
    u_list = u_test = u_pipe = u_over = u_never = u_quiet = 0;
    g_err = 0;
    const char *dir = "";
    int c;
    optind = 1;
    while ((c = getopt(argc, argv, "ltpond:q")) != -1) {
        switch (c) {
        case 'l': u_list = 1; break;
        case 't': u_test = 1; break;
        case 'p': u_pipe = 1; u_quiet = 1; break;
        case 'o': u_over = 1; break;
        case 'n': u_never = 1; break;
        case 'd': dir = optarg; break;
        case 'q': u_quiet = 1; break;
        default: fprintf(stderr, "usage: unzip [-ltpoqn] [-d DIR] ARCHIVE.zip [MEMBER...]\n"); return 2;
        }
    }
    if (optind >= argc) { fprintf(stderr, "usage: unzip [-ltpoqn] [-d DIR] ARCHIVE.zip [MEMBER...]\n"); return 2; }
    const char *file = argv[optind++];
    char **pats = argv + optind;
    int np = argc - optind;
    int fd = open(file, O_RDONLY);
    if (fd < 0) { kb_error("cannot open %s: %s", file, strerror(errno)); return 2; }
    int n = 0;
    off_t fsize = 0;
    ZEntry *ents = read_directory(fd, &n, &fsize);
    if (!ents) { close(fd); return 2; }
    if (dir[0] && !u_list && !u_test && !u_pipe) {                  /* work inside the directory, like tar -C: the symlink check then only looks below it */
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s", dir);
        for (char *sl = path + 1; *sl; sl++) {
            if (*sl != '/') continue;
            *sl = 0;
            mkdir(path, 0777);
            *sl = '/';
        }
        struct stat st;
        if (stat(path, &st) < 0 && mkdir(path, 0777) < 0) { kb_error("cannot create %s: %s", dir, strerror(errno)); free_entries(ents, n); close(fd); return 2; }
        if (chdir(path) < 0) { kb_error("cannot enter %s: %s", dir, strerror(errno)); free_entries(ents, n); close(fd); return 2; }
        dir = "";
    }
    int shown = 0;
    uint64_t total = 0;
    if (u_list) {
        printf("Archive:  %s\n  Length      Date    Time    Name\n---------  ---------- -----   ----\n", file);
    } else if (!u_quiet && !u_test) printf("Archive:  %s\n", file);
    for (int i = 0; i < n; i++) {
        ZEntry *z = &ents[i];
        if (!member_wanted(z->name, pats, np)) continue;
        shown++;
        if (u_list) {
            time_t t = (time_t)z->mtime;
            struct tm tm;
            char when[32] = "";
            localtime_r(&t, &tm);
            strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
            printf("%9u  %s   %s\n", z->usize, when, z->name);
            total += z->usize;
        } else if (u_test) {
            size_t nl = strlen(z->name);
            if (nl && z->name[nl - 1] == '/') continue;
            if (unpack_entry(fd, z, fsize, -1, NULL, NULL) == 0 && !u_quiet) printf("    testing: %s   OK\n", z->name);
        } else if (u_pipe) {
            size_t nl = strlen(z->name);
            if (nl && z->name[nl - 1] == '/') continue;
            unpack_entry(fd, z, fsize, 1, NULL, NULL);
        } else extract_one(fd, z, fsize, dir);
    }
    if (u_list) printf("---------                     -------\n%9llu                     %d file%s\n", (unsigned long long)total, shown, shown == 1 ? "" : "s");
    if (u_test && !g_err) printf("No errors detected in compressed data of %s.\n", file);
    if (np && !shown) { kb_error("none of the requested members were found"); g_err = 1; }
    free_entries(ents, n);
    close(fd);
    return g_err ? 1 : 0;
}

/* ---------------------------------------------------------------- zip */

typedef struct { ZEntry z; char *extra_ut; } ZOut;

static int z_level = 6, z_recurse, z_junk, z_quiet, z_verbose;
static char **z_excl;
static int z_nexcl;
static ZOut *g_out;
static int g_nout, g_capout;
static int g_zfd;
static uint64_t g_pos;

static int zwrite(const void *buf, size_t n) {
    if (write_all(g_zfd, buf, n) < 0) { zerr("write error: %s", strerror(errno)); return -1; }
    g_pos += n;
    return 0;
}

static int z_excluded(const char *name) {
    for (int i = 0; i < z_nexcl; i++) if (!fnmatch(z_excl[i], name, 0)) return 1;
    return 0;
}

static int add_file(const char *path, const char *arcname, const struct stat *st) {
    if (g_nout >= MAX_ENTRIES) { zerr("too many entries"); return -1; }
    if (g_nout == g_capout) {
        g_capout = g_capout ? g_capout * 2 : 64;
        g_out = realloc(g_out, (size_t)g_capout * sizeof *g_out);
        if (!g_out) { zerr("out of memory"); return -1; }
    }
    ZOut *o = &g_out[g_nout];
    memset(o, 0, sizeof *o);
    ZEntry *z = &o->z;
    size_t nl = strlen(arcname);
    int is_dir = S_ISDIR(st->st_mode), is_link = S_ISLNK(st->st_mode);
    z->name = malloc(nl + 2);
    if (!z->name) { zerr("out of memory"); return -1; }
    strcpy(z->name, arcname);
    if (is_dir && (nl == 0 || z->name[nl - 1] != '/')) { z->name[nl++] = '/'; z->name[nl] = 0; }
    z->made = (3 << 8) | 20;
    z->flags = 0;
    z->mtime = (uint32_t)st->st_mtime;
    z->dostime = to_dos(st->st_mtime);
    z->attr = ((uint32_t)st->st_mode << 16) | (is_dir ? 0x10u : 0u);
    z->lh_off = (uint32_t)g_pos;
    if (g_pos > 0xFFFFFFF0ull) { zerr("archive would exceed 4 GiB (zip64 is not supported)"); free(z->name); return -1; }
    for (const unsigned char *p = (const unsigned char *)z->name; *p; p++) if (*p >= 0x80) z->flags |= 0x0800;     /* UTF-8 names */

    uint8_t *linkdata = NULL;
    size_t linklen = 0;
    int in = -1;
    uint64_t size = 0;
    if (is_link) {
        linkdata = malloc(4097);
        ssize_t l = linkdata ? readlink(path, (char *)linkdata, 4096) : -1;
        if (l < 0) { zerr("%s: %s", path, strerror(errno)); free(linkdata); free(z->name); return -1; }
        linklen = (size_t)l;
        size = linklen;
    } else if (!is_dir) {
        in = open(path, O_RDONLY);
        if (in < 0) { zerr("%s: %s", path, strerror(errno)); free(z->name); return -1; }
        size = (uint64_t)st->st_size;
        if (size > 0xFFFFFFF0ull) { zerr("%s: too large for a zip file without zip64", path); close(in); free(z->name); return -1; }
    }

    /* local header with the sizes still blank; patched after the data is written */
    uint8_t lh[30 + 9];
    uint8_t ut[9];
    put16(ut, 0x5455);
    put16(ut + 2, 5);
    ut[4] = 1;
    put32(ut + 5, z->mtime);
    memset(lh, 0, sizeof lh);
    put32(lh, 0x04034b50u);
    put16(lh + 4, 20);
    put16(lh + 6, z->flags);
    put16(lh + 10, z->dostime & 0xFFFF);
    put16(lh + 12, z->dostime >> 16);
    put16(lh + 26, (unsigned)nl);
    put16(lh + 28, 9);
    uint64_t hdr_pos = g_pos;
    if (zwrite(lh, 30) < 0 || zwrite(z->name, nl) < 0 || zwrite(ut, 9) < 0) { if (in >= 0) close(in); free(linkdata); free(z->name); return -1; }
    uint64_t data_pos = g_pos;

    int method = 0, rc = 0;
    uint32_t crc = 0;
    uint64_t csize = 0;
    if (linkdata) {
        crc = z_crc32(0, linkdata, linklen);
        rc = zwrite(linkdata, linklen);
        csize = linklen;
    } else if (in >= 0) {
        uint8_t *ibuf = malloc(IOBUF), *obuf = malloc(IOBUF);
        ZStream s;
        memset(&s, 0, sizeof s);
        int use_deflate = z_level > 0 && size > 0;
        if (!ibuf || !obuf || (use_deflate && z_deflate_init(&s, z_level, Z_RAW))) { zerr("out of memory"); rc = -1; use_deflate = 0; }
        uint64_t left = size;
        while (!rc) {
            ssize_t r = left ? read(in, ibuf, left < IOBUF ? (size_t)left : IOBUF) : 0;
            if (r < 0) { zerr("%s: read error: %s", path, strerror(errno)); rc = -1; break; }
            int last = r == 0 || (uint64_t)r == left;
            if (r > 0) crc = z_crc32(crc, ibuf, (size_t)r);
            left -= (uint64_t)r;
            if (use_deflate) {
                s.next_in = ibuf;
                s.avail_in = (size_t)r;
                for (;;) {
                    s.next_out = obuf;
                    s.avail_out = IOBUF;
                    int d = z_deflate(&s, last ? Z_FINISH : Z_NO_FLUSH);
                    size_t got = IOBUF - s.avail_out;
                    if (got) { if (zwrite(obuf, got) < 0) { rc = -1; break; } csize += got; }
                    if (d < 0) { zerr("%s", z_strerror(d)); rc = -1; break; }
                    if (d == Z_STREAM_END) break;
                    if (s.avail_out != 0 && s.avail_in == 0) break;
                }
            } else if (r > 0) {
                if (zwrite(ibuf, (size_t)r) < 0) rc = -1;
                csize += (uint64_t)r;
            }
            if (last) break;
        }
        if (use_deflate) {
            z_deflate_end(&s);
            method = 8;
            if (!rc && csize >= size) {                                     /* deflating did not help: store the file instead */
                if (lseek(in, 0, SEEK_SET) < 0 || lseek(g_zfd, (off_t)data_pos, SEEK_SET) < 0) rc = -1;
                else {
                    csize = 0;
                    g_pos = data_pos;
                    uint64_t l2 = size;
                    while (l2 && !rc) {
                        ssize_t r = read(in, ibuf, l2 < IOBUF ? (size_t)l2 : IOBUF);
                        if (r <= 0) { rc = -1; break; }
                        if (zwrite(ibuf, (size_t)r) < 0) rc = -1;
                        csize += (uint64_t)r;
                        l2 -= (uint64_t)r;
                    }
                    if (!rc && ftruncate(g_zfd, (off_t)g_pos) < 0) rc = -1;
                    method = 0;
                }
            }
        }
        free(ibuf);
        free(obuf);
        close(in);
    }
    free(linkdata);
    if (rc) { free(z->name); return -1; }
    z->method = (uint16_t)method;
    z->crc = crc;
    z->csize = (uint32_t)csize;
    z->usize = (uint32_t)size;
    /* patch the local header */
    put16(lh + 8, (unsigned)method);
    put32(lh + 14, crc);
    put32(lh + 18, (uint32_t)csize);
    put32(lh + 22, (uint32_t)size);
    put16(lh + 4, method == 8 ? 20 : 10);
    uint64_t end = g_pos;
    if (lseek(g_zfd, (off_t)hdr_pos, SEEK_SET) < 0 || write_all(g_zfd, lh, 30) < 0 || lseek(g_zfd, (off_t)end, SEEK_SET) < 0) {
        zerr("write error: %s", strerror(errno));
        free(z->name);
        return -1;
    }
    if (z_verbose || !z_quiet) {
        int pct = size ? (int)(100 - csize * 100 / size) : 0;
        printf("  adding: %s (%s %d%%)\n", z->name, method == 8 ? "deflated" : "stored", pct < 0 ? 0 : pct);
    }
    g_nout++;
    return 0;
}

static int walk(const char *path, const char *arcname, int top);

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int walk(const char *path, const char *arcname, int top) {
    struct stat st;
    if (lstat(path, &st) < 0) { zerr("%s: %s", path, strerror(errno)); return -1; }
    if (z_excluded(arcname)) return 0;
    (void)top;
    if (S_ISDIR(st.st_mode)) {
        if (!z_junk && add_file(path, arcname, &st) < 0) return -1;
        if (!z_recurse) return 0;
        DIR *d = opendir(path);
        if (!d) { zerr("%s: %s", path, strerror(errno)); return -1; }
        char **names = NULL;
        int n = 0;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            names = realloc(names, (size_t)(n + 1) * sizeof *names);
            names[n++] = strdup(de->d_name);
        }
        closedir(d);
        qsort(names, (size_t)n, sizeof *names, cmp_names);
        int rc = 0;
        for (int i = 0; i < n; i++) {
            char *child = kb_join(path, names[i]);
            char child_arc[4096];
            if (z_junk) snprintf(child_arc, sizeof child_arc, "%s", names[i]);
            else snprintf(child_arc, sizeof child_arc, "%s%s%s", arcname, arcname[0] && arcname[strlen(arcname) - 1] != '/' ? "/" : "", names[i]);
            if (walk(child, child_arc, 0) < 0) rc = -1;
            free(child);
            free(names[i]);
        }
        free(names);
        return rc;
    }
    if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) { fprintf(stderr, "%s: %s: not a regular file, skipped\n", kb_name, path); return 0; }
    return add_file(path, arcname, &st);
}

static int write_directory(void) {
    uint64_t cd_start = g_pos;
    for (int i = 0; i < g_nout; i++) {
        ZEntry *z = &g_out[i].z;
        size_t nl = strlen(z->name);
        uint8_t h[46 + 9];
        memset(h, 0, sizeof h);
        put32(h, 0x02014b50u);
        put16(h + 4, z->made);
        put16(h + 6, z->method == 8 ? 20 : 10);
        put16(h + 8, z->flags);
        put16(h + 10, z->method);
        put16(h + 12, z->dostime & 0xFFFF);
        put16(h + 14, z->dostime >> 16);
        put32(h + 16, z->crc);
        put32(h + 20, z->csize);
        put32(h + 24, z->usize);
        put16(h + 28, (unsigned)nl);
        put16(h + 30, 9);
        put32(h + 38, z->attr);
        put32(h + 42, z->lh_off);
        put16(h + 46, 0x5455);
        put16(h + 48, 5);
        h[50] = 1;
        put32(h + 51, z->mtime);
        if (zwrite(h, 46) < 0 || zwrite(z->name, nl) < 0 || zwrite(h + 46, 9) < 0) return -1;
    }
    uint8_t e[22];
    memset(e, 0, sizeof e);
    put32(e, 0x06054b50u);
    put16(e + 8, (unsigned)g_nout);
    put16(e + 10, (unsigned)g_nout);
    put32(e + 12, (uint32_t)(g_pos - cd_start));
    put32(e + 16, (uint32_t)cd_start);
    return zwrite(e, 22);
}

APPLET(zip) {
    z_level = 6; z_recurse = z_junk = z_quiet = z_verbose = 0;
    z_nexcl = 0;
    g_err = 0;
    g_out = NULL; g_nout = g_capout = 0; g_pos = 0;
    z_excl = calloc((size_t)argc + 1, sizeof *z_excl);
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if (a[0] != '-' || !a[1]) break;
        for (const char *c = a + 1; *c; c++) {
            if (*c == 'r') z_recurse = 1;
            else if (*c == 'j') z_junk = 1;
            else if (*c == 'q') z_quiet = 1;
            else if (*c == 'v') z_verbose = 1;
            else if (*c >= '0' && *c <= '9') z_level = *c - '0';
            else { fprintf(stderr, "usage: zip [-rjqv0-9] ARCHIVE.zip FILE... [-x PATTERN]\n"); return 2; }
        }
    }
    if (argc - i < 2) { fprintf(stderr, "usage: zip [-rjqv0-9] ARCHIVE.zip FILE... [-x PATTERN]\n"); return 2; }
    const char *archive = argv[i++];
    char path[4096];
    snprintf(path, sizeof path, "%s", archive);
    size_t al = strlen(path);
    {   /* like Info-ZIP: a name without any extension gets ".zip" */
        const char *slash = strrchr(path, '/');
        if (!strchr(slash ? slash : path, '.') && al + 4 < sizeof path) strcat(path, ".zip");
    }
    if (access(path, F_OK) == 0) { kb_error("%s already exists (adding to an existing archive is not supported yet; remove it or choose another name)", path); return 2; }
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmp%d", path, (int)getpid());
    g_zfd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (g_zfd < 0) { kb_error("%s: %s", path, strerror(errno)); return 2; }
    int rc = 0;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-x")) { for (i++; i < argc; i++) z_excl[z_nexcl++] = argv[i]; break; }
        const char *name = argv[i];
        const char *arc = name;
        while (arc[0] == '.' && arc[1] == '/') arc += 2;
        while (*arc == '/') arc++;
        char arcbuf[4096];
        if (z_junk) { const char *b = strrchr(name, '/'); snprintf(arcbuf, sizeof arcbuf, "%s", b && b[1] ? b + 1 : name); arc = arcbuf; }
        size_t l = strlen(arc);
        if (l > 1 && arc[l - 1] == '/') { snprintf(arcbuf, sizeof arcbuf, "%s", arc); while (strlen(arcbuf) > 1 && arcbuf[strlen(arcbuf) - 1] == '/') arcbuf[strlen(arcbuf) - 1] = 0; arc = arcbuf; }
        if (walk(name, arc, 1) < 0) rc = 1;
    }
    if (!g_nout) { kb_error("nothing to do"); rc = 12; }
    if (!rc || g_nout) { if (write_directory() < 0) rc = 1; }
    close(g_zfd);
    if (rc == 12 || rc == 1) {
        if (rc == 12 || !g_nout) { unlink(tmp); return rc == 12 ? 12 : 1; }
    }
    if (rename(tmp, path) < 0) { kb_error("cannot create %s: %s", path, strerror(errno)); unlink(tmp); return 2; }
    for (int k = 0; k < g_nout; k++) free(g_out[k].z.name);
    free(g_out);
    free(z_excl);
    return rc ? 1 : 0;
}
