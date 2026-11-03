/*
 * tar: create, extract and list archives. Writes ustar (with GNU long-name entries for long paths); reads ustar, GNU
 * (long names and links, base-256 numbers) and pax extended headers. -z (and, when reading, a gzip signature) puts libz
 * in the middle. Modes, symbolic links and modification times are kept; "..", absolute names and extraction through
 * symbolic links are refused.
 */
#include "kappabox.h"
#include <stdarg.h>
#include <z.h>
#include <utime.h>
#include <fnmatch.h>

#define BLOCK 512
#define IOBUF 65536
#define META_MAX (1 << 20)

/* ---------------------------------------------------------------- byte stream with optional gzip */

typedef struct {
    int fd, gz, writing, eof, err;
    ZStream z;
    uint8_t *ibuf;
    size_t ipos, ilen;
    uint8_t *obuf;
} TarIO;

static int io_open_read(TarIO *io, int fd, int force_gz) {
    memset(io, 0, sizeof *io);
    io->fd = fd;
    io->ibuf = malloc(IOBUF);
    if (!io->ibuf) return -1;
    ssize_t n;
    do n = read(fd, io->ibuf, IOBUF); while (n < 0 && errno == EINTR);
    if (n < 0) return -1;
    io->ilen = (size_t)n;
    if (n == 0) io->eof = 1;
    io->gz = force_gz || (n >= 2 && io->ibuf[0] == 0x1F && io->ibuf[1] == 0x8B);
    if (io->gz && z_inflate_init(&io->z, Z_GZIP)) return -1;
    return 0;
}

static int io_refill(TarIO *io) {
    if (io->eof) return 0;
    ssize_t n;
    do n = read(io->fd, io->ibuf, IOBUF); while (n < 0 && errno == EINTR);
    if (n < 0) { io->err = errno; return -1; }
    io->ipos = 0;
    io->ilen = (size_t)n;
    if (n == 0) io->eof = 1;
    return (int)n;
}

/* Reads up to n bytes; fewer only at the end of the archive. Returns -1 on a read or decompression error. */
static ssize_t io_read(TarIO *io, void *buf, size_t n) {
    uint8_t *out = buf;
    size_t got = 0;
    if (!io->gz) {
        while (got < n) {
            if (io->ipos == io->ilen && io_refill(io) <= 0) break;
            size_t take = io->ilen - io->ipos < n - got ? io->ilen - io->ipos : n - got;
            memcpy(out + got, io->ibuf + io->ipos, take);
            io->ipos += take;
            got += take;
        }
        return io->err ? -1 : (ssize_t)got;
    }
    while (got < n) {
        if (io->z.avail_in == 0) {
            if (io->ipos == io->ilen && io_refill(io) <= 0) break;
            io->z.next_in = io->ibuf + io->ipos;
            io->z.avail_in = io->ilen - io->ipos;
            io->ipos = io->ilen;
        }
        io->z.next_out = out + got;
        io->z.avail_out = n - got;
        int rc = z_inflate(&io->z);
        got = n - io->z.avail_out;
        if (rc == Z_STREAM_END) {
            z_inflate_reset(&io->z);
            if (io->z.avail_in == 0 && io->ipos == io->ilen && io->eof) break;
            continue;
        }
        if (rc < 0 && rc != Z_BUF_ERROR) { io->err = EIO; kb_error("gzip: %s", z_strerror(rc)); return -1; }
    }
    return (ssize_t)got;
}

static int io_open_write(TarIO *io, int fd, int gz, int level) {
    memset(io, 0, sizeof *io);
    io->fd = fd;
    io->writing = 1;
    io->gz = gz;
    io->obuf = malloc(IOBUF);
    if (!io->obuf) return -1;
    if (gz && z_deflate_init(&io->z, level, Z_GZIP)) return -1;
    return 0;
}

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

static int io_write(TarIO *io, const void *buf, size_t n) {
    if (!io->gz) return write_all(io->fd, buf, n);
    io->z.next_in = buf;
    io->z.avail_in = n;
    while (io->z.avail_in) {
        io->z.next_out = io->obuf;
        io->z.avail_out = IOBUF;
        int rc = z_deflate(&io->z, Z_NO_FLUSH);
        if (rc < 0) return -1;
        if (write_all(io->fd, io->obuf, IOBUF - io->z.avail_out) < 0) return -1;
    }
    return 0;
}

static int io_close(TarIO *io) {
    int rc = 0;
    if (io->writing && io->gz) {
        int r;
        io->z.next_in = NULL;
        io->z.avail_in = 0;
        do {
            io->z.next_out = io->obuf;
            io->z.avail_out = IOBUF;
            r = z_deflate(&io->z, Z_FINISH);
            if (r < 0 || write_all(io->fd, io->obuf, IOBUF - io->z.avail_out) < 0) { rc = -1; break; }
        } while (r != Z_STREAM_END);
        z_deflate_end(&io->z);
    } else if (io->gz) z_inflate_end(&io->z);
    free(io->ibuf);
    free(io->obuf);
    return rc;
}

/* ---------------------------------------------------------------- options */

static int o_create, o_extract, o_list, o_gz, o_verbose, o_keep_old, o_stdout, o_perm, o_deref, o_touch, o_level = 6;
static int o_strip;
static char **o_exclude;
static int n_exclude;
static char **g_select;
static int g_nselect;
static char g_selected_seen[64];
static int g_errors;
static int g_root;
static mode_t g_umask;

static void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void warn(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s: ", kb_name);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    g_errors = 1;
}

/* ---------------------------------------------------------------- header fields */

typedef struct {
    char name[4096], link[4096];
    unsigned mode;
    uint64_t size;
    int64_t mtime;
    uint32_t uid, gid;
    char uname[33], gname[33];
    char type;
} Entry;

static uint64_t parse_num(const uint8_t *p, size_t n) {
    if (p[0] & 0x80) {                                  /* GNU base-256 */
        uint64_t v = p[0] & 0x3F;
        for (size_t i = 1; i < n; i++) v = (v << 8) | p[i];
        return v;
    }
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == 0)) i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = (v << 3) | (uint64_t)(p[i] - '0');
    return v;
}

static void put_num(uint8_t *p, size_t n, uint64_t v) {
    uint64_t limit = (n >= 12) ? (1ULL << 33) : (1ULL << (3 * (n - 1)));
    if (v < limit) {
        p[n - 1] = 0;
        for (size_t i = n - 1; i-- > 0;) { p[i] = (uint8_t)('0' + (v & 7)); v >>= 3; }
    } else {
        p[0] = 0x80;
        for (size_t i = n; i-- > 1;) { p[i] = (uint8_t)v; v >>= 8; }
    }
}

static unsigned header_sum(const uint8_t *h) {
    unsigned s = 0;
    for (int i = 0; i < BLOCK; i++) s += (i >= 148 && i < 156) ? ' ' : h[i];
    return s;
}

static int all_zero(const uint8_t *b) {
    for (int i = 0; i < BLOCK; i++) if (b[i]) return 0;
    return 1;
}

static void field_str(char *dst, size_t dn, const uint8_t *p, size_t n) {
    size_t l = 0;
    while (l < n && p[l]) l++;
    if (l >= dn) l = dn - 1;
    memcpy(dst, p, l);
    dst[l] = 0;
}

/* pax records: "LEN key=value\n" */
static void parse_pax(const uint8_t *data, size_t n, Entry *e, int *have_name, int *have_link, int *have_size) {
    size_t i = 0;
    while (i < n) {
        size_t len = 0, j = i;
        while (j < n && data[j] >= '0' && data[j] <= '9') len = len * 10 + (size_t)(data[j++] - '0');
        if (j >= n || data[j] != ' ' || len == 0 || i + len > n || len < (j - i) + 2) break;
        const char *rec = (const char *)data + j + 1;
        size_t rl = len - (j - i) - 1;
        if (rl && rec[rl - 1] == '\n') rl--;
        const char *eq = memchr(rec, '=', rl);
        if (eq) {
            size_t kl = (size_t)(eq - rec), vl = rl - kl - 1;
            char val[4096];
            if (vl >= sizeof val) vl = sizeof val - 1;
            memcpy(val, eq + 1, vl);
            val[vl] = 0;
            if (kl == 4 && !memcmp(rec, "path", 4)) { strlcpy(e->name, val, sizeof e->name); *have_name = 1; }
            else if (kl == 8 && !memcmp(rec, "linkpath", 8)) { strlcpy(e->link, val, sizeof e->link); *have_link = 1; }
            else if (kl == 4 && !memcmp(rec, "size", 4)) { e->size = strtoull(val, NULL, 10); *have_size = 1; }
            else if (kl == 5 && !memcmp(rec, "mtime", 5)) e->mtime = (int64_t)strtoll(val, NULL, 10);
            else if (kl == 3 && !memcmp(rec, "uid", 3)) e->uid = (uint32_t)strtoul(val, NULL, 10);
            else if (kl == 3 && !memcmp(rec, "gid", 3)) e->gid = (uint32_t)strtoul(val, NULL, 10);
            else if (kl == 5 && !memcmp(rec, "uname", 5)) strlcpy(e->uname, val, sizeof e->uname);
            else if (kl == 5 && !memcmp(rec, "gname", 5)) strlcpy(e->gname, val, sizeof e->gname);
        }
        i += len;
    }
}

/* Reads the next entry (resolving long-name and pax headers). Returns 1 with *e filled, 0 at the end of the archive, -1 on error. */
static int next_entry(TarIO *io, Entry *e) {
    uint8_t h[BLOCK];
    char long_name[4096] = "", long_link[4096] = "";
    int have_ln = 0, have_ll = 0, have_pname = 0, have_plink = 0, have_psize = 0;
    Entry px;
    memset(&px, 0, sizeof px);
    int have_px = 0;
    for (;;) {
        ssize_t n = io_read(io, h, BLOCK);
        if (n < 0) return -1;
        if (n == 0) return 0;                                   /* no end-of-archive blocks: tolerate */
        if (n < BLOCK) { warn("unexpected end of archive"); return -1; }
        if (all_zero(h)) return 0;
        if (header_sum(h) != parse_num(h + 148, 8)) { warn("corrupt archive header (checksum mismatch)"); return -1; }
        char type = (char)h[156];
        uint64_t size = parse_num(h + 124, 12);
        if (type == 'L' || type == 'K' || type == 'x' || type == 'g') {
            if (size > META_MAX) { warn("oversized %c header (%llu bytes)", type, (unsigned long long)size); return -1; }
            size_t padded = (size_t)((size + BLOCK - 1) & ~(uint64_t)(BLOCK - 1));
            uint8_t *data = malloc(padded ? padded : 1);
            if (!data) { warn("out of memory"); return -1; }
            if (io_read(io, data, padded) != (ssize_t)padded) { free(data); warn("unexpected end of archive"); return -1; }
            if (type == 'L') { size_t l = size < sizeof long_name - 1 ? size : sizeof long_name - 1; memcpy(long_name, data, l); long_name[l] = 0; have_ln = 1; }
            else if (type == 'K') { size_t l = size < sizeof long_link - 1 ? size : sizeof long_link - 1; memcpy(long_link, data, l); long_link[l] = 0; have_ll = 1; }
            else if (type == 'x') { parse_pax(data, size, &px, &have_pname, &have_plink, &have_psize); have_px = 1; }
            free(data);                                         /* 'g' (global pax defaults) is read and ignored */
            continue;
        }
        memset(e, 0, sizeof *e);
        e->type = type ? type : '0';
        e->mode = (unsigned)parse_num(h + 100, 8) & 07777;
        e->uid = (uint32_t)parse_num(h + 108, 8);
        e->gid = (uint32_t)parse_num(h + 116, 8);
        e->size = size;
        e->mtime = (int64_t)parse_num(h + 136, 12);
        field_str(e->link, sizeof e->link, h + 157, 100);
        field_str(e->uname, sizeof e->uname, h + 265, 32);
        field_str(e->gname, sizeof e->gname, h + 297, 32);
        char nm[101], prefix[156];
        field_str(nm, sizeof nm, h, 100);
        prefix[0] = 0;
        if (!memcmp(h + 257, "ustar", 5) && h[262] != ' ') field_str(prefix, sizeof prefix, h + 345, 155);
        if (prefix[0]) snprintf(e->name, sizeof e->name, "%s/%s", prefix, nm);
        else strlcpy(e->name, nm, sizeof e->name);
        if (have_ln) strlcpy(e->name, long_name, sizeof e->name);
        if (have_ll) strlcpy(e->link, long_link, sizeof e->link);
        if (have_px) {
            if (have_pname) strlcpy(e->name, px.name, sizeof e->name);
            if (have_plink) strlcpy(e->link, px.link, sizeof e->link);
            if (have_psize) e->size = px.size;
            if (px.mtime) e->mtime = px.mtime;
            if (px.uid) e->uid = px.uid;
            if (px.gid) e->gid = px.gid;
            if (px.uname[0]) strlcpy(e->uname, px.uname, sizeof e->uname);
            if (px.gname[0]) strlcpy(e->gname, px.gname, sizeof e->gname);
        }
        return 1;
    }
}

static int skip_data(TarIO *io, uint64_t size) {
    uint64_t left = (size + BLOCK - 1) & ~(uint64_t)(BLOCK - 1);
    uint8_t buf[4096];
    while (left) {
        size_t n = left < sizeof buf ? (size_t)left : sizeof buf;
        ssize_t r = io_read(io, buf, n);
        if (r != (ssize_t)n) { warn("unexpected end of archive"); return -1; }
        left -= n;
    }
    return 0;
}

/* ---------------------------------------------------------------- names */

static int excluded(const char *name) {
    for (int i = 0; i < n_exclude; i++) {
        if (!fnmatch(o_exclude[i], name, 0)) return 1;
        const char *s = name;
        while ((s = strchr(s, '/'))) { s++; if (!fnmatch(o_exclude[i], s, 0)) return 1; }
    }
    return 0;
}

/* Strips leading slashes and --strip-components; returns NULL for names that must not be extracted. */
static const char *safe_name(const char *name) {
    const char *p = name;
    while (*p == '/') p++;
    for (int i = 0; i < o_strip; i++) {
        const char *s = strchr(p, '/');
        if (!s) return NULL;
        p = s + 1;
        while (*p == '/') p++;
    }
    if (!*p) return NULL;
    for (const char *q = p; *q;) {
        const char *e = strchr(q, '/');
        size_t l = e ? (size_t)(e - q) : strlen(q);
        if (l == 2 && q[0] == '.' && q[1] == '.') return NULL;
        q += l;
        while (*q == '/') q++;
    }
    return p;
}

/* Creates the parent directories of path; refuses to go through a symbolic link. */
static int make_parents(const char *path) {
    char tmp[4096];
    strlcpy(tmp, path, sizeof tmp);
    for (char *s = tmp + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        struct stat st;
        if (lstat(tmp, &st) == 0) {
            if (S_ISLNK(st.st_mode)) { *s = '/'; errno = 0; return -1; }       /* errno 0: the caller says "refusing to extract through a symbolic link" */
            if (!S_ISDIR(st.st_mode)) { *s = '/'; errno = ENOTDIR; return -1; }
        } else if (mkdir(tmp, 0777 & ~g_umask) < 0 && errno != EEXIST) { *s = '/'; return -1; }
        *s = '/';
    }
    return 0;
}

/* ---------------------------------------------------------------- listing */

static void list_entry(const Entry *e) {
    if (!o_verbose) { puts(e->name); return; }
    char ms[11], when[24] = "-";
    unsigned type = e->type == '5' ? S_IFDIR : e->type == '2' ? S_IFLNK : S_IFREG;
    kb_mode_string(type | e->mode, ms);
    time_t t = (time_t)e->mtime;
    struct tm tm;
    if (gmtime_r(&t, &tm)) strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
    char who[80];
    if (e->uname[0]) snprintf(who, sizeof who, "%s/%s", e->uname, e->gname[0] ? e->gname : "-");
    else snprintf(who, sizeof who, "%u/%u", e->uid, e->gid);
    printf("%s %s %8llu %s %s", ms, who, (unsigned long long)e->size, when, e->name);
    if (e->type == '2') printf(" -> %s", e->link);
    else if (e->type == '1') printf(" link to %s", e->link);
    putchar('\n');
}

/* ---------------------------------------------------------------- extraction */

typedef struct { char path[4096]; unsigned mode; int64_t mtime; } DirFix;
static DirFix *g_dirs;
static int g_ndirs;

static void set_times(const char *path, int64_t mtime, int link) {
    if (o_touch) return;
    struct timeval tv[2] = { { (time_t)mtime, 0 }, { (time_t)mtime, 0 } };
    if (link) lutimes(path, tv); else utimes(path, tv);
}

static unsigned final_mode(unsigned mode) { return (g_root || o_perm) ? (mode & 07777) : (mode & 0777 & ~g_umask); }

static int extract_data(TarIO *io, const Entry *e, int fd) {
    uint64_t left = e->size, padded = (e->size + BLOCK - 1) & ~(uint64_t)(BLOCK - 1);
    uint8_t *buf = malloc(IOBUF);
    if (!buf) { warn("out of memory"); return -1; }
    int rc = 0;
    while (padded) {
        size_t want = padded < IOBUF ? (size_t)padded : IOBUF;
        ssize_t r = io_read(io, buf, want);
        if (r != (ssize_t)want) { warn("unexpected end of archive"); rc = -1; break; }
        size_t useful = left < want ? (size_t)left : want;
        if (fd >= 0 && useful && write_all(fd, buf, useful) < 0) { warn("write error: %s", strerror(errno)); rc = -1; fd = -1; }
        left -= useful;
        padded -= want;
    }
    free(buf);
    return rc;
}

static int extract_entry(TarIO *io, const Entry *e) {
    const char *name = safe_name(e->name);
    if (!name) {
        if (!(e->type == '5' && o_strip)) warn("%s: skipping member with an unsafe or empty name", e->name);
        return skip_data(io, e->size);
    }
    if (excluded(e->name)) return skip_data(io, e->size);
    if (o_verbose) puts(e->name);
    char path[4096];
    strlcpy(path, name, sizeof path);
    size_t pl = strlen(path);
    while (pl > 1 && path[pl - 1] == '/') path[--pl] = 0;

    if (o_stdout) {                                             /* -O: file contents to standard output */
        if (e->type == '0' || e->type == '7') return extract_data(io, e, 1);
        return skip_data(io, e->size);
    }
    if (make_parents(path) < 0) {
        warn("%s: cannot create parent directories: %s", path, errno ? strerror(errno) : "refusing to extract through a symbolic link");
        return skip_data(io, e->size);
    }
    struct stat st;
    int exists = lstat(path, &st) == 0;
    if (e->type == '5') {
        if (exists && !S_ISDIR(st.st_mode)) { if (o_keep_old) return 0; unlink(path); exists = 0; }
        if (!exists && mkdir(path, 0700) < 0) { warn("%s: %s", path, strerror(errno)); return 0; }
        if (g_ndirs < 1 << 16) {
            g_dirs = realloc(g_dirs, (size_t)(g_ndirs + 1) * sizeof *g_dirs);
            if (!g_dirs) { warn("out of memory"); return -1; }
            strlcpy(g_dirs[g_ndirs].path, path, sizeof g_dirs[0].path);
            g_dirs[g_ndirs].mode = e->mode;
            g_dirs[g_ndirs].mtime = e->mtime;
            g_ndirs++;
        }
        chmod(path, 0700 | final_mode(e->mode));               /* writable until everything below it is in place */
        return 0;
    }
    if (exists && o_keep_old) { if (!o_keep_old) return 0; return skip_data(io, e->size); }
    if (exists && S_ISDIR(st.st_mode)) { warn("%s: is a directory; not replaced", path); return skip_data(io, e->size); }
    if (exists) unlink(path);

    if (e->type == '2') {
        if (symlink(e->link, path) < 0) warn("%s: cannot create symbolic link: %s", path, strerror(errno));
        else set_times(path, e->mtime, 1);
        return 0;
    }
    if (e->type == '1') {                                       /* hard link: this system has none, so copy the earlier file */
        const char *target = safe_name(e->link);
        if (!target || kb_copy_file(target, path, 1) < 0) warn("%s: cannot link to %s: %s", path, e->link, strerror(errno));
        else set_times(path, e->mtime, 0);
        return skip_data(io, e->size);
    }
    if (e->type == '0' || e->type == '7' || (e->type != '3' && e->type != '4' && e->type != '6')) {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) { warn("%s: %s", path, strerror(errno)); return skip_data(io, e->size); }
        int rc = extract_data(io, e, fd);
        close(fd);
        chmod(path, final_mode(e->mode));
        if (g_root) chown(path, e->uid, e->gid);
        set_times(path, e->mtime, 0);
        return rc;
    }
    warn("%s: special files (devices, pipes) are not extracted", path);
    return skip_data(io, e->size);
}

static void finish_dirs(void) {
    for (int i = g_ndirs - 1; i >= 0; i--) {
        chmod(g_dirs[i].path, final_mode(g_dirs[i].mode));
        set_times(g_dirs[i].path, g_dirs[i].mtime, 0);
    }
    free(g_dirs);
    g_dirs = NULL;
    g_ndirs = 0;
}

/* With file operands, only the members they name (or that lie below a named directory) are listed or extracted. */
static int wanted(const char *name) {
    if (!g_nselect) return 1;
    const char *n = name;
    while (*n == '/') n++;
    for (int k = 0; k < g_nselect; k++) {
        const char *s = g_select[k];
        while (*s == '/') s++;
        size_t sl = strlen(s);
        while (sl > 1 && s[sl - 1] == '/') sl--;
        if (!strncmp(n, s, sl) && (n[sl] == 0 || n[sl] == '/' || (n[sl] == '/' && !n[sl + 1]))) { g_selected_seen[k < 64 ? k : 63] = 1; return 1; }
    }
    return 0;
}

static int do_read(TarIO *io) {
    Entry e;
    int rc;
    while ((rc = next_entry(io, &e)) > 0) {
        if (!wanted(e.name)) { if (skip_data(io, e.size) < 0) break; continue; }
        if (o_list) {
            if (!excluded(e.name)) list_entry(&e);
            if (skip_data(io, e.size) < 0) break;
        } else if (extract_entry(io, &e) < 0) break;
    }
    if (rc < 0) g_errors = 1;
    if (o_extract) finish_dirs();
    return g_errors;
}

/* ---------------------------------------------------------------- creation */

static void write_header(TarIO *io, const char *name, const char *link, unsigned mode, uint64_t size, int64_t mtime,
                         uint32_t uid, uint32_t gid, char type, int *fail) {
    uint8_t h[BLOCK];
    memset(h, 0, sizeof h);
    size_t nl = strlen(name), ll = strlen(link);
    const char *store_name = name;
    char prefix[156] = "", short_name[101];
    if (nl > 100) {
        int split = 0;                                          /* ustar prefix: split at a '/' so that both parts fit */
        for (size_t i = nl > 156 ? nl - 100 : 0; i < nl; i++) {
            if (name[i] == '/' && i <= 155 && nl - i - 1 <= 100 && nl - i - 1 > 0) { split = (int)i; break; }
        }
        if (split) {
            memcpy(prefix, name, (size_t)split);
            prefix[split] = 0;
            strlcpy(short_name, name + split + 1, sizeof short_name);
            store_name = short_name;
        } else {                                                /* GNU long name entry */
            uint8_t lh[BLOCK];
            memset(lh, 0, sizeof lh);
            memcpy(lh, "././@LongLink", 13);
            put_num(lh + 100, 8, 0644);
            put_num(lh + 124, 12, nl + 1);
            lh[156] = 'L';
            memcpy(lh + 257, "ustar  ", 8);
            memset(lh + 148, ' ', 8);
            snprintf((char *)lh + 148, 8, "%06o", header_sum(lh));
            lh[155] = ' ';
            if (io_write(io, lh, BLOCK) < 0 || io_write(io, name, nl + 1) < 0) *fail = 1;
            size_t pad = (BLOCK - (nl + 1) % BLOCK) % BLOCK;
            uint8_t zero[BLOCK] = { 0 };
            if (pad && io_write(io, zero, pad) < 0) *fail = 1;
            strlcpy(short_name, name, sizeof short_name);
            store_name = short_name;
        }
    }
    if (ll > 100) {
        uint8_t lh[BLOCK];
        memset(lh, 0, sizeof lh);
        memcpy(lh, "././@LongLink", 13);
        put_num(lh + 100, 8, 0644);
        put_num(lh + 124, 12, ll + 1);
        lh[156] = 'K';
        memcpy(lh + 257, "ustar  ", 8);
        memset(lh + 148, ' ', 8);
        snprintf((char *)lh + 148, 8, "%06o", header_sum(lh));
        lh[155] = ' ';
        if (io_write(io, lh, BLOCK) < 0 || io_write(io, link, ll + 1) < 0) *fail = 1;
        size_t pad = (BLOCK - (ll + 1) % BLOCK) % BLOCK;
        uint8_t zero[BLOCK] = { 0 };
        if (pad && io_write(io, zero, pad) < 0) *fail = 1;
    }
    strncpy((char *)h, store_name, 100);
    put_num(h + 100, 8, mode & 07777);
    put_num(h + 108, 8, uid);
    put_num(h + 116, 8, gid);
    put_num(h + 124, 12, size);
    put_num(h + 136, 12, mtime < 0 ? 0 : (uint64_t)mtime);
    h[156] = (uint8_t)type;
    strncpy((char *)h + 157, link, 100);
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    strncpy((char *)h + 265, kb_user_name(uid), 31);
    strncpy((char *)h + 297, kb_group_name(gid), 31);
    strncpy((char *)h + 345, prefix, 155);
    memset(h + 148, ' ', 8);
    snprintf((char *)h + 148, 8, "%06o", header_sum(h));
    h[155] = ' ';
    if (io_write(io, h, BLOCK) < 0) *fail = 1;
}

static void add_path(TarIO *io, const char *path, int top);

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void add_path(TarIO *io, const char *path, int top) {
    struct stat st;
    if ((o_deref ? stat(path, &st) : lstat(path, &st)) < 0) { warn("%s: %s", path, strerror(errno)); return; }
    const char *name = path;
    while (*name == '/') name++;
    if (name != path && top) fprintf(stderr, "%s: removing leading '/' from member names\n", kb_name);
    if (!*name) name = ".";
    if (excluded(name)) return;
    char nm[4096];
    strlcpy(nm, name, sizeof nm);
    size_t nl = strlen(nm);
    int fail = 0;
    if (o_verbose) fprintf(o_stdout ? stdout : stderr, "%s%s\n", nm, S_ISDIR(st.st_mode) && nm[nl - 1] != '/' ? "/" : "");
    if (S_ISDIR(st.st_mode)) {
        while (nl > 1 && nm[nl - 1] == '/') nm[--nl] = 0;
        if (nl + 1 < sizeof nm) { nm[nl++] = '/'; nm[nl] = 0; }
        write_header(io, nm, "", st.st_mode, 0, st.st_mtime, st.st_uid, st.st_gid, '5', &fail);
        if (fail) { warn("write error"); g_errors = 1; return; }
        DIR *d = opendir(path);
        if (!d) { warn("%s: %s", path, strerror(errno)); return; }
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
        for (int i = 0; i < n; i++) {
            char *child = kb_join(path, names[i]);
            add_path(io, child, 0);
            free(child);
            free(names[i]);
        }
        free(names);
    } else if (S_ISLNK(st.st_mode)) {
        char target[4096];
        ssize_t l = readlink(path, target, sizeof target - 1);
        if (l < 0) { warn("%s: %s", path, strerror(errno)); return; }
        target[l] = 0;
        write_header(io, nm, target, st.st_mode, 0, st.st_mtime, st.st_uid, st.st_gid, '2', &fail);
    } else if (S_ISREG(st.st_mode)) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) { warn("%s: %s", path, strerror(errno)); return; }
        uint64_t size = (uint64_t)st.st_size;
        write_header(io, nm, "", st.st_mode, size, st.st_mtime, st.st_uid, st.st_gid, '0', &fail);
        uint8_t *buf = malloc(IOBUF);
        uint64_t left = size;
        while (left && buf && !fail) {
            ssize_t r = read(fd, buf, left < IOBUF ? (size_t)left : IOBUF);
            if (r <= 0) { warn("%s: file changed size or cannot be read", path); break; }
            if (io_write(io, buf, (size_t)r) < 0) fail = 1;
            left -= (uint64_t)r;
        }
        free(buf);
        close(fd);
        if (left) {                                             /* keep the archive well-formed: pad with zeros */
            uint8_t zero[BLOCK] = { 0 };
            while (left) { size_t n = left < BLOCK ? (size_t)left : BLOCK; if (io_write(io, zero, n) < 0) { fail = 1; break; } left -= n; }
        }
        size_t pad = (size_t)((BLOCK - size % BLOCK) % BLOCK);
        uint8_t zero[BLOCK] = { 0 };
        if (pad && io_write(io, zero, pad) < 0) fail = 1;
    } else {
        warn("%s: special file ignored", path);
    }
    if (fail) { warn("write error: %s", strerror(errno)); g_errors = 1; }
}

/* ---------------------------------------------------------------- main */

static void usage(void) {
    fprintf(stderr, "usage: tar -c|-x|-t [-zvpkOhm] [-f ARCHIVE] [-C DIR] [--strip-components=N] [--exclude=PATTERN] [FILE...]\n");
}

APPLET(tar) {
    o_create = o_extract = o_list = o_gz = o_verbose = o_keep_old = o_stdout = o_perm = o_deref = o_touch = 0;
    o_strip = 0;
    o_level = 6;
    g_errors = 0;
    n_exclude = 0;
    g_nselect = 0;
    memset(g_selected_seen, 0, sizeof g_selected_seen);
    o_exclude = calloc((size_t)argc + 1, sizeof *o_exclude);
    char *archive = NULL;
    char **items = calloc((size_t)argc + 1, sizeof *items);      /* files; an item "\1DIR" means: change to DIR first */
    int nitems = 0;
    int i = 1;
    int old_style = argc > 1 && argv[1][0] != '-' && strlen(argv[1]) > 0;
    int old_next = 2;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (old_style && i == 1) {
            for (const char *c = a; *c; c++) {
                switch (*c) {
                case 'c': o_create = 1; break;
                case 'x': o_extract = 1; break;
                case 't': o_list = 1; break;
                case 'z': o_gz = 1; break;
                case 'v': o_verbose = 1; break;
                case 'p': o_perm = 1; break;
                case 'k': o_keep_old = 1; break;
                case 'O': o_stdout = 1; break;
                case 'h': o_deref = 1; break;
                case 'm': o_touch = 1; break;
                case 'f': if (old_next < argc) archive = argv[old_next++]; break;
                case 'C': if (old_next < argc) { char *d = argv[old_next++]; char *it = malloc(strlen(d) + 2); it[0] = 1; strcpy(it + 1, d); items[nitems++] = it; } break;
                default: usage(); return 2;
                }
            }
            i = old_next - 1;
            continue;
        }
        if (a[0] == '-' && a[1] == '-') {
            if (!strcmp(a, "--create")) o_create = 1;
            else if (!strcmp(a, "--extract") || !strcmp(a, "--get")) o_extract = 1;
            else if (!strcmp(a, "--list")) o_list = 1;
            else if (!strcmp(a, "--gzip") || !strcmp(a, "--gunzip")) o_gz = 1;
            else if (!strcmp(a, "--verbose")) o_verbose = 1;
            else if (!strcmp(a, "--keep-old-files")) o_keep_old = 1;
            else if (!strcmp(a, "--to-stdout")) o_stdout = 1;
            else if (!strcmp(a, "--preserve-permissions") || !strcmp(a, "--same-permissions")) o_perm = 1;
            else if (!strcmp(a, "--dereference")) o_deref = 1;
            else if (!strcmp(a, "--touch")) o_touch = 1;
            else if (!strncmp(a, "--file=", 7)) archive = (char *)a + 7;
            else if (!strncmp(a, "--directory=", 12)) { char *it = malloc(strlen(a)); it[0] = 1; strcpy(it + 1, a + 12); items[nitems++] = it; }
            else if (!strncmp(a, "--strip-components=", 19)) o_strip = atoi(a + 19);
            else if (!strncmp(a, "--exclude=", 10)) o_exclude[n_exclude++] = (char *)a + 10;
            else if (!strcmp(a, "--")) { for (i++; i < argc; i++) items[nitems++] = argv[i]; break; }
            else { kb_error("unknown option '%s'", a); usage(); return 2; }
            continue;
        }
        if (a[0] == '-' && a[1]) {
            for (const char *c = a + 1; *c; c++) {
                switch (*c) {
                case 'c': o_create = 1; break;
                case 'x': o_extract = 1; break;
                case 't': o_list = 1; break;
                case 'z': o_gz = 1; break;
                case 'v': o_verbose = 1; break;
                case 'p': o_perm = 1; break;
                case 'k': o_keep_old = 1; break;
                case 'O': o_stdout = 1; break;
                case 'h': o_deref = 1; break;
                case 'm': o_touch = 1; break;
                case 'f':
                case 'C': {
                    char *val = c[1] ? (char *)c + 1 : (i + 1 < argc ? argv[++i] : NULL);
                    if (!val) { kb_error("option -%c needs an argument", *c); return 2; }
                    if (*c == 'f') archive = val;
                    else { char *it = malloc(strlen(val) + 2); it[0] = 1; strcpy(it + 1, val); items[nitems++] = it; }
                    c = a + strlen(a) - 1;
                    break;
                }
                default: kb_error("unknown option -%c", *c); usage(); return 2;
                }
            }
            continue;
        }
        items[nitems++] = argv[i];
    }
    if (o_create + o_extract + o_list != 1) { kb_error("you must specify exactly one of -c, -x, -t"); usage(); return 2; }
    g_root = geteuid() == 0;
    g_umask = umask(0);
    umask(g_umask);

    int fd, rc = 0;
    TarIO io;
    int use_stdio = !archive || !strcmp(archive, "-");
    if (o_create) {
        fd = use_stdio ? 1 : open(archive, O_WRONLY | O_CREAT | O_TRUNC, 0666 & ~g_umask);
        if (fd < 0) { kb_error("%s: %s", archive, strerror(errno)); return 2; }
        if (use_stdio && isatty(1)) { kb_error("refusing to write archive contents to a terminal (use -f FILE)"); return 2; }
        if (io_open_write(&io, fd, o_gz, o_level) < 0) { kb_error("cannot start the compressor"); return 2; }
        int added = 0;
        for (int k = 0; k < nitems; k++) {
            if (items[k][0] == 1) {
                if (chdir(items[k] + 1) < 0) { warn("%s: %s", items[k] + 1, strerror(errno)); rc = 2; goto finish_create; }
                continue;
            }
            add_path(&io, items[k], 1);
            added++;
        }
        if (!added) { kb_error("cowardly refusing to create an empty archive"); rc = 2; goto finish_create; }
        {
            uint8_t zero[BLOCK * 2] = { 0 };
            if (io_write(&io, zero, sizeof zero) < 0) { warn("write error"); }
        }
    finish_create:
        if (io_close(&io) < 0) g_errors = 1;
        if (!use_stdio) close(fd);
        if (rc) { if (!use_stdio) unlink(archive); return rc; }
        return g_errors ? 2 : 0;
    }
    fd = use_stdio ? 0 : open(archive, O_RDONLY);
    if (fd < 0) { kb_error("%s: %s", archive, strerror(errno)); return 2; }
    if (io_open_read(&io, fd, o_gz) < 0) { kb_error("cannot read the archive: %s", strerror(errno)); return 2; }
    g_select = calloc((size_t)nitems + 1, sizeof *g_select);
    for (int k = 0; k < nitems; k++) {
        if (items[k][0] == 1) {                                 /* -C DIR: extraction happens there */
            if (chdir(items[k] + 1) < 0) { kb_error("%s: %s", items[k] + 1, strerror(errno)); return 2; }
        } else g_select[g_nselect++] = items[k];
    }
    rc = do_read(&io);
    io_close(&io);
    if (!use_stdio) close(fd);
    for (int k = 0; k < g_nselect; k++)
        if (!g_selected_seen[k < 64 ? k : 63]) { kb_error("%s: not found in archive", g_select[k]); rc = 1; }
    return rc ? 2 : 0;
}
