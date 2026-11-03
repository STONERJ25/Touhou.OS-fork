/*
 * sftp-server: the SFTP protocol (version 3, the one OpenSSH speaks) on standard input and output. sshd(8) starts it as the logged-in
 * resident when a client asks for the "sftp" subsystem, so `sftp`, `scp` (OpenSSH 9 and later use SFTP) and the graphical clients work, and the
 * kernel's file permissions apply to everything they do. Paths are the real paths of the system; relative ones start in the home directory.
 *
 * Supported: open, close, read, write, stat, lstat, fstat, setstat, fsetstat, opendir, readdir, remove, mkdir, rmdir, realpath, rename, readlink,
 * symlink. No extensions are announced (a client then uses plain rename and so on). The program ends when its input does.
 */
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include <sys/stat.h>
#include <sys/time.h>

enum {
    FXP_INIT = 1, FXP_VERSION = 2, FXP_OPEN = 3, FXP_CLOSE = 4, FXP_READ = 5, FXP_WRITE = 6, FXP_LSTAT = 7, FXP_FSTAT = 8, FXP_SETSTAT = 9,
    FXP_FSETSTAT = 10, FXP_OPENDIR = 11, FXP_READDIR = 12, FXP_REMOVE = 13, FXP_MKDIR = 14, FXP_RMDIR = 15, FXP_REALPATH = 16, FXP_STAT = 17,
    FXP_RENAME = 18, FXP_READLINK = 19, FXP_SYMLINK = 20,
    FXP_STATUS = 101, FXP_HANDLE = 102, FXP_DATA = 103, FXP_NAME = 104, FXP_ATTRS = 105,
};
enum { FX_OK = 0, FX_EOF = 1, FX_NO_SUCH_FILE = 2, FX_PERMISSION_DENIED = 3, FX_FAILURE = 4, FX_BAD_MESSAGE = 5, FX_OP_UNSUPPORTED = 8 };
enum { ATTR_SIZE = 0x1, ATTR_UIDGID = 0x2, ATTR_PERMISSIONS = 0x4, ATTR_ACMODTIME = 0x8, ATTR_EXTENDED = 0x80000000u };
enum { PF_READ = 0x1, PF_WRITE = 0x2, PF_APPEND = 0x4, PF_CREAT = 0x8, PF_TRUNC = 0x10, PF_EXCL = 0x20 };

#define MAX_PACKET (256 * 1024)
#define MAX_HANDLES 64

static uint8_t g_in[MAX_PACKET + 16];
static uint8_t g_out[MAX_PACKET + 128];

/* ---------------------------------------------------------------------------------------------------- reading and writing the wire */

typedef struct { const uint8_t *p; size_t n; int bad; } Rd;

static uint32_t rd_u32(Rd *r) {
    if (r->n < 4) { r->bad = 1; r->n = 0; return 0; }
    uint32_t v = (uint32_t)r->p[0] << 24 | (uint32_t)r->p[1] << 16 | (uint32_t)r->p[2] << 8 | r->p[3];
    r->p += 4;
    r->n -= 4;
    return v;
}

static uint64_t rd_u64(Rd *r) {
    uint64_t hi = rd_u32(r);
    return hi << 32 | rd_u32(r);
}

static const uint8_t *rd_str(Rd *r, size_t *len) {
    uint32_t n = rd_u32(r);
    if (r->bad || n > r->n) { r->bad = 1; *len = 0; return r->p; }
    const uint8_t *s = r->p;
    r->p += n;
    r->n -= n;
    *len = n;
    return s;
}

/* a path or name from the client as a C string; fails on a NUL inside or when it does not fit */
static int rd_path(Rd *r, char *out, size_t cap) {
    size_t n;
    const uint8_t *s = rd_str(r, &n);
    if (r->bad || n >= cap || memchr(s, 0, n)) return -1;
    memcpy(out, s, n);
    out[n] = 0;
    return 0;
}

typedef struct { uint8_t *p; size_t n, cap; } Wr;

static void wr_raw(Wr *w, const void *p, size_t n) {
    if (n > w->cap - w->n) return;                           /* callers keep within the buffer; a miscount truncates rather than overflows */
    memcpy(w->p + w->n, p, n);
    w->n += n;
}
static void wr_u8(Wr *w, uint8_t v) { wr_raw(w, &v, 1); }
static void wr_u32(Wr *w, uint32_t v) { uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; wr_raw(w, b, 4); }
static void wr_u64(Wr *w, uint64_t v) { wr_u32(w, (uint32_t)(v >> 32)); wr_u32(w, (uint32_t)v); }
static void wr_str(Wr *w, const void *p, size_t n) { wr_u32(w, (uint32_t)n); wr_raw(w, p, n); }
static void wr_cstr(Wr *w, const char *s) { wr_str(w, s, strlen(s)); }

static int read_full(void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(0, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* Sends g_out as one packet: the 4-byte length is filled in here. */
static void send_packet(Wr *w) {
    uint32_t len = (uint32_t)(w->n - 4);
    w->p[0] = (uint8_t)(len >> 24); w->p[1] = (uint8_t)(len >> 16); w->p[2] = (uint8_t)(len >> 8); w->p[3] = (uint8_t)len;
    size_t done = 0;
    while (done < w->n) {
        ssize_t r = write(1, w->p + done, w->n - done);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) _exit(1);                                  /* the client is gone */
        done += (size_t)r;
    }
}

static void begin(Wr *w, uint8_t type, uint32_t id) {
    w->p = g_out; w->n = 0; w->cap = sizeof g_out;
    wr_u32(w, 0);                                              /* the length, later */
    wr_u8(w, type);
    wr_u32(w, id);
}

static void status(uint32_t id, uint32_t code, const char *msg) {
    Wr w;
    begin(&w, FXP_STATUS, id);
    wr_u32(&w, code);
    wr_cstr(&w, msg);
    wr_cstr(&w, "en");
    send_packet(&w);
}

static void fail_errno(uint32_t id) {
    int e = errno;
    uint32_t code = e == ENOENT || e == ENOTDIR ? FX_NO_SUCH_FILE : e == EACCES || e == EPERM || e == EROFS ? FX_PERMISSION_DENIED : FX_FAILURE;
    status(id, code, strerror(e));
}

/* ---------------------------------------------------------------------------------------------------- attributes */

typedef struct {
    uint32_t flags;
    uint64_t size;
    uint32_t uid, gid, perm, atime, mtime;
} Attrs;

static int rd_attrs(Rd *r, Attrs *a) {
    memset(a, 0, sizeof *a);
    a->flags = rd_u32(r);
    if (a->flags & ATTR_SIZE) a->size = rd_u64(r);
    if (a->flags & ATTR_UIDGID) { a->uid = rd_u32(r); a->gid = rd_u32(r); }
    if (a->flags & ATTR_PERMISSIONS) a->perm = rd_u32(r);
    if (a->flags & ATTR_ACMODTIME) { a->atime = rd_u32(r); a->mtime = rd_u32(r); }
    if (a->flags & ATTR_EXTENDED) {
        uint32_t count = rd_u32(r);
        for (uint32_t i = 0; i < count && !r->bad; i++) { size_t n; rd_str(r, &n); rd_str(r, &n); }
    }
    return r->bad ? -1 : 0;
}

static void wr_attrs(Wr *w, const struct stat *st) {
    wr_u32(w, ATTR_SIZE | ATTR_UIDGID | ATTR_PERMISSIONS | ATTR_ACMODTIME);
    wr_u64(w, (uint64_t)st->st_size);
    wr_u32(w, st->st_uid);
    wr_u32(w, st->st_gid);
    wr_u32(w, st->st_mode);
    wr_u32(w, (uint32_t)st->st_atime);
    wr_u32(w, (uint32_t)st->st_mtime);
}

static void mode_string(uint32_t m, char *s) {
    s[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : S_ISCHR(m) ? 'c' : S_ISBLK(m) ? 'b' : '-';
    for (int i = 0; i < 9; i++) s[1 + i] = m & (0400u >> i) ? "rwxrwxrwx"[i] : '-';
    s[10] = 0;
}

/* What `ls -l` would print: the sftp client shows this for its own `ls -l`. */
static void long_name(const char *name, const struct stat *st, char *out, size_t cap) {
    char mode[12], when[24];
    mode_string(st->st_mode, mode);
    time_t t = (time_t)st->st_mtime;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(when, sizeof when, "%b %e %H:%M", &tmv);
    snprintf(out, cap, "%s %3u %-8u %-8u %8lld %s %s", mode, (unsigned)st->st_nlink, (unsigned)st->st_uid, (unsigned)st->st_gid, (long long)st->st_size, when, name);
}

/* Applies what a client asked for with setstat: size, permissions, times, owner. 0 or -1 with errno. */
static int apply_attrs(const char *path, const Attrs *a) {
    if ((a->flags & ATTR_SIZE) && truncate(path, (off_t)a->size) != 0) return -1;
    if ((a->flags & ATTR_PERMISSIONS) && chmod(path, (mode_t)(a->perm & 07777)) != 0) return -1;
    if (a->flags & ATTR_ACMODTIME) {
        struct timeval tv[2] = { { (time_t)a->atime, 0 }, { (time_t)a->mtime, 0 } };
        if (utimes(path, tv) != 0) return -1;
    }
    if ((a->flags & ATTR_UIDGID) && chown(path, (uid_t)a->uid, (gid_t)a->gid) != 0) return -1;
    return 0;
}

/* ---------------------------------------------------------------------------------------------------- handles */

typedef struct {
    int kind;                                                  /* 0 free, 1 file, 2 directory */
    int fd;
    DIR *dir;
    char path[1024];
} Handle;

static Handle g_h[MAX_HANDLES];

static int handle_new(int kind) {
    for (int i = 0; i < MAX_HANDLES; i++)
        if (!g_h[i].kind) { memset(&g_h[i], 0, sizeof g_h[i]); g_h[i].kind = kind; g_h[i].fd = -1; return i; }
    return -1;
}

static void send_handle(uint32_t id, int idx) {
    Wr w;
    begin(&w, FXP_HANDLE, id);
    uint8_t h[4] = { 0, 0, 0, (uint8_t)(idx + 1) };
    wr_str(&w, h, 4);
    send_packet(&w);
}

/* The handle named in the request: of the given kind (1 file, 2 directory), or of any kind (0). */
static Handle *handle_get(Rd *r, int kind) {
    size_t n;
    const uint8_t *s = rd_str(r, &n);
    if (r->bad || n != 4 || s[0] || s[1] || s[2] || s[3] < 1 || s[3] > MAX_HANDLES) return NULL;
    Handle *h = &g_h[s[3] - 1];
    return h->kind && (!kind || h->kind == kind) ? h : NULL;
}

static void handle_close(Handle *h) {
    if (h->kind == 1 && h->fd >= 0) close(h->fd);
    if (h->kind == 2 && h->dir) closedir(h->dir);
    memset(h, 0, sizeof *h);
}

/* ---------------------------------------------------------------------------------------------------- the requests */

static void join_path(const char *dir, const char *name, char *out, size_t cap) {
    size_t l = strlen(dir);
    snprintf(out, cap, "%s%s%s", dir, l && dir[l - 1] == '/' ? "" : "/", name);
}

/* A path made absolute and tidy without needing it to exist (realpath() when it does). */
static void canonical(const char *path, char *out, size_t cap) {
    char tmp[2048], real[1024];
    if (!*path) path = ".";
    if (path[0] == '/') snprintf(tmp, sizeof tmp, "%s", path);
    else {
        char cwd[1024];
        if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "/");
        join_path(cwd, path, tmp, sizeof tmp);
    }
    if (realpath(tmp, real)) { snprintf(out, cap, "%s", real); return; }
    char *parts[128];
    int n = 0;
    for (char *tok = strtok(tmp, "/"); tok; tok = strtok(NULL, "/")) {
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) { if (n) n--; continue; }
        if (n < 128) parts[n++] = tok;
    }
    size_t o = 0;
    if (cap) out[0] = 0;
    if (n == 0 && cap > 1) { out[0] = '/'; out[1] = 0; return; }
    for (int i = 0; i < n; i++) o += (size_t)snprintf(out + o, o < cap ? cap - o : 0, "/%s", parts[i]);
}

static void do_open(uint32_t id, Rd *r) {
    char path[1024];
    Attrs a;
    if (rd_path(r, path, sizeof path)) { status(id, FX_BAD_MESSAGE, "bad open request"); return; }
    uint32_t pflags = rd_u32(r);
    if (rd_attrs(r, &a) || r->bad) { status(id, FX_BAD_MESSAGE, "bad open request"); return; }
    int flags = (pflags & PF_READ) && (pflags & PF_WRITE) ? O_RDWR : (pflags & PF_WRITE) ? O_WRONLY : O_RDONLY;
    if (pflags & PF_APPEND) flags |= O_APPEND;
    if (pflags & PF_CREAT) flags |= O_CREAT;
    if (pflags & PF_TRUNC) flags |= O_TRUNC;
    if (pflags & PF_EXCL) flags |= O_EXCL;
    int idx = handle_new(1);
    if (idx < 0) { status(id, FX_FAILURE, "too many open files"); return; }
    int fd = open(path, flags, (a.flags & ATTR_PERMISSIONS) ? (mode_t)(a.perm & 0777) : 0666);
    if (fd < 0) { g_h[idx].kind = 0; fail_errno(id); return; }
    g_h[idx].fd = fd;
    snprintf(g_h[idx].path, sizeof g_h[idx].path, "%s", path);
    send_handle(id, idx);
}

static void do_opendir(uint32_t id, Rd *r) {
    char path[1024];
    if (rd_path(r, path, sizeof path) || r->bad) { status(id, FX_BAD_MESSAGE, "bad request"); return; }
    int idx = handle_new(2);
    if (idx < 0) { status(id, FX_FAILURE, "too many open directories"); return; }
    DIR *d = opendir(path);
    if (!d) { g_h[idx].kind = 0; fail_errno(id); return; }
    g_h[idx].dir = d;
    snprintf(g_h[idx].path, sizeof g_h[idx].path, "%s", path);
    send_handle(id, idx);
}

static void do_readdir(uint32_t id, Rd *r) {
    Handle *h = handle_get(r, 2);
    if (!h) { status(id, FX_FAILURE, "bad handle"); return; }
    Wr w;
    begin(&w, FXP_NAME, id);
    size_t count_at = w.n;
    wr_u32(&w, 0);
    uint32_t count = 0;
    struct dirent *e;
    while (w.n < 32 * 1024 && count < 100 && (e = readdir(h->dir))) {
        char full[2048], lname[1400];
        struct stat st;
        join_path(h->path, e->d_name, full, sizeof full);
        if (lstat(full, &st) != 0) memset(&st, 0, sizeof st);
        long_name(e->d_name, &st, lname, sizeof lname);
        wr_cstr(&w, e->d_name);
        wr_cstr(&w, lname);
        wr_attrs(&w, &st);
        count++;
    }
    if (!count) { status(id, FX_EOF, "end of directory"); return; }
    w.p[count_at] = (uint8_t)(count >> 24); w.p[count_at + 1] = (uint8_t)(count >> 16); w.p[count_at + 2] = (uint8_t)(count >> 8); w.p[count_at + 3] = (uint8_t)count;
    send_packet(&w);
}

static void do_read(uint32_t id, Rd *r) {
    Handle *h = handle_get(r, 1);
    uint64_t off = rd_u64(r);
    uint32_t len = rd_u32(r);
    if (!h || r->bad) { status(id, FX_FAILURE, "bad handle"); return; }
    if (len > MAX_PACKET - 64) len = MAX_PACKET - 64;
    Wr w;
    begin(&w, FXP_DATA, id);
    wr_u32(&w, 0);
    if (lseek(h->fd, (off_t)off, SEEK_SET) < 0) { fail_errno(id); return; }
    ssize_t got = read(h->fd, w.p + w.n, len);
    if (got < 0) { fail_errno(id); return; }
    if (got == 0) { status(id, FX_EOF, "end of file"); return; }
    w.p[w.n - 4] = (uint8_t)((uint32_t)got >> 24); w.p[w.n - 3] = (uint8_t)((uint32_t)got >> 16); w.p[w.n - 2] = (uint8_t)((uint32_t)got >> 8); w.p[w.n - 1] = (uint8_t)got;
    w.n += (size_t)got;
    send_packet(&w);
}

static void do_write(uint32_t id, Rd *r) {
    Handle *h = handle_get(r, 1);
    uint64_t off = rd_u64(r);
    size_t n;
    const uint8_t *data = rd_str(r, &n);
    if (!h || r->bad) { status(id, FX_FAILURE, "bad handle"); return; }
    if (lseek(h->fd, (off_t)off, SEEK_SET) < 0) { fail_errno(id); return; }
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(h->fd, data + done, n - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { fail_errno(id); return; }
        done += (size_t)w;
    }
    status(id, FX_OK, "ok");
}

static void send_stat(uint32_t id, const struct stat *st) {
    Wr w;
    begin(&w, FXP_ATTRS, id);
    wr_attrs(&w, st);
    send_packet(&w);
}

static void send_one_name(uint32_t id, const char *name, const struct stat *st) {
    Wr w;
    begin(&w, FXP_NAME, id);
    wr_u32(&w, 1);
    wr_cstr(&w, name);
    char lname[1400] = "";
    if (st) long_name(name, st, lname, sizeof lname);
    wr_cstr(&w, st ? lname : name);
    if (st) wr_attrs(&w, st); else wr_u32(&w, 0);
    send_packet(&w);
}

static void handle_request(uint8_t type, uint32_t id, Rd *r) {
    char path[1024], path2[1024];
    struct stat st;
    switch (type) {
    case FXP_OPEN:
        do_open(id, r);
        break;
    case FXP_CLOSE: {
        Handle *h = handle_get(r, 0);
        if (!h) { status(id, FX_FAILURE, "bad handle"); break; }
        handle_close(h);
        status(id, FX_OK, "ok");
        break;
    }
    case FXP_READ:
        do_read(id, r);
        break;
    case FXP_WRITE:
        do_write(id, r);
        break;
    case FXP_LSTAT: case FXP_STAT:
        if (rd_path(r, path, sizeof path) || r->bad) { status(id, FX_BAD_MESSAGE, "bad request"); break; }
        if ((type == FXP_STAT ? stat(path, &st) : lstat(path, &st)) != 0) fail_errno(id); else send_stat(id, &st);
        break;
    case FXP_FSTAT: {
        Handle *h = handle_get(r, 1);
        if (!h) { status(id, FX_FAILURE, "bad handle"); break; }
        if (fstat(h->fd, &st) != 0) fail_errno(id); else send_stat(id, &st);
        break;
    }
    case FXP_SETSTAT: {
        Attrs a;
        if (rd_path(r, path, sizeof path) || rd_attrs(r, &a)) { status(id, FX_BAD_MESSAGE, "bad request"); break; }
        if (apply_attrs(path, &a) != 0) fail_errno(id); else status(id, FX_OK, "ok");
        break;
    }
    case FXP_FSETSTAT: {
        Handle *h = handle_get(r, 1);
        Attrs a;
        if (!h || rd_attrs(r, &a)) { status(id, FX_FAILURE, "bad handle"); break; }
        if (apply_attrs(h->path, &a) != 0) fail_errno(id); else status(id, FX_OK, "ok");
        break;
    }
    case FXP_OPENDIR:
        do_opendir(id, r);
        break;
    case FXP_READDIR:
        do_readdir(id, r);
        break;
    case FXP_REMOVE:
        if (rd_path(r, path, sizeof path) || r->bad) { status(id, FX_BAD_MESSAGE, "bad request"); break; }
        if (unlink(path) != 0) fail_errno(id); else status(id, FX_OK, "ok");
        break;
    case FXP_MKDIR: {
        Attrs a;
        if (rd_path(r, path, sizeof path) || rd_attrs(r, &a)) { status(id, FX_BAD_MESSAGE, "bad request"); break; }
        if (mkdir(path, (a.flags & ATTR_PERMISSIONS) ? (mode_t)(a.perm & 0777) : 0777) != 0) fail_errno(id); else status(id, FX_OK, "ok");
        break;
    }
    case FXP_RMDIR:
        if (rd_path(r, path, sizeof path) || r->bad) { status(id, FX_BAD_MESSAGE, "bad request"); break; }
        if (rmdir(path) != 0) fail_errno(id); else status(id, FX_OK, "ok");
        break;
    case FXP_REALPATH: {
        char real[1024];
        if (rd_path(r, path, sizeof path) || r->bad) { status(id, FX_BAD_MESSAGE, "bad request"); break; }
        canonical(path, real, sizeof real);
        send_one_name(id, real, NULL);
        break;
    }
    case FXP_RENAME:
        if (rd_path(r, path, sizeof path) || rd_path(r, path2, sizeof path2) || r->bad) { status(id, FX_BAD_MESSAGE, "bad request"); break; }
        if (rename(path, path2) != 0) fail_errno(id); else status(id, FX_OK, "ok");
        break;
    case FXP_READLINK: {
        char target[1024];
        if (rd_path(r, path, sizeof path) || r->bad) { status(id, FX_BAD_MESSAGE, "bad request"); break; }
        ssize_t n = readlink(path, target, sizeof target - 1);
        if (n < 0) { fail_errno(id); break; }
        target[n] = 0;
        send_one_name(id, target, NULL);
        break;
    }
    case FXP_SYMLINK:                                           /* OpenSSH's order: what the link points to first, then the link's own name */
        if (rd_path(r, path, sizeof path) || rd_path(r, path2, sizeof path2) || r->bad) { status(id, FX_BAD_MESSAGE, "bad request"); break; }
        if (symlink(path, path2) != 0) fail_errno(id); else status(id, FX_OK, "ok");
        break;
    default:
        status(id, FX_OP_UNSUPPORTED, "not supported");
    }
}

int main(void) {
    for (;;) {
        uint8_t head[4];
        if (read_full(head, 4) != 0) return 0;                  /* the client closed the channel */
        uint32_t len = (uint32_t)head[0] << 24 | (uint32_t)head[1] << 16 | (uint32_t)head[2] << 8 | head[3];
        if (len < 1 || len > MAX_PACKET) return 1;
        if (read_full(g_in, len) != 0) return 1;
        Rd r = { g_in, len, 0 };
        uint8_t type = *r.p++;
        r.n--;
        if (type == FXP_INIT) {
            Wr w;
            w.p = g_out; w.n = 0; w.cap = sizeof g_out;
            wr_u32(&w, 0);
            wr_u8(&w, FXP_VERSION);
            wr_u32(&w, 3);
            send_packet(&w);
            continue;
        }
        uint32_t id = rd_u32(&r);
        if (r.bad) return 1;
        handle_request(type, id, &r);
    }
}
