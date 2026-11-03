/*
 * The loader (see loader.h). Threads: one per running download, each inside libhttp's blocking http_get (or a file read);
 * they share nothing with the main thread but the fetch records and the chunk queues, which one mutex guards. Specifications
 * followed for the pieces written here: RFC 2397 (data: URLs), RFC 4648 (base64), RFC 8089 (file: URLs through urls.c).
 */
#include "loader.h"
#include "urls.h"

#include <http.h>
#include <tls.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct BrChunk {
    struct BrChunk *next;
    size_t len, cap;
    uint8_t data[];
} BrChunk;

typedef struct BrFetch {
    struct BrFetch *next;
    BrLoader *l;
    uint32_t id;
    unsigned gen;
    BrFetchKind kind;
    char url[BR_MAX_URL];
    size_t max_bytes;
    int state;                       /* 0 queued, 1 running, 2 done */
    bool cancelled, released, done_reported, started;
    bool ev_headers, ev_data, headers_known;
    BrChunk *head, *tail;
    size_t queued;                   /* bytes in the chunk list */
    uint64_t total;                  /* bytes received in all */
    BrResult res;
    uint64_t t0;
    bool dns_held;
    int stop;                        /* why our callback stopped libhttp: 0, or a BR_E_* */
    HttpResult *hr;                  /* the worker's own result, read by its callbacks */
    pthread_t thread;
} BrFetch;

struct BrLoader {
    pthread_mutex_t lock;
    BrFetch *list;
    uint32_t next_id;
    int running;
    int pipefd[2];
    bool wake_pending;
};

/* what the workers share across loaders: the name resolver's cache (libc keeps it without a lock) and the root store */
static pthread_mutex_t g_dns_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_trust_lock = PTHREAD_MUTEX_INITIALIZER;
static TlsTrust *g_trust;
static bool g_trust_tried;
static char g_trust_error[300];

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ---------------------------------------------------------------- the loader's own bookkeeping (lock held) */

static void wake_locked(BrLoader *l) {
    if (l->wake_pending) return;
    l->wake_pending = true;
    char c = 'x';
    if (write(l->pipefd[1], &c, 1) < 0) { /* a full pipe already says "wake up" */ }
}

static void free_chunks(BrFetch *f) {
    for (BrChunk *c = f->head; c;) {
        BrChunk *n = c->next;
        free(c);
        c = n;
    }
    f->head = f->tail = NULL;
    f->queued = 0;
}

static void unlink_fetch(BrLoader *l, BrFetch *f) {
    for (BrFetch **pp = &l->list; *pp; pp = &(*pp)->next)
        if (*pp == f) { *pp = f->next; break; }
    free_chunks(f);
    free(f);
}

static BrFetch *find_locked(BrLoader *l, uint32_t id) {
    for (BrFetch *f = l->list; f; f = f->next) if (f->id == id) return f;
    return NULL;
}

static bool append_locked(BrFetch *f, const void *d, size_t n) {
    if (!n) return true;
    BrChunk *t = f->tail;
    if (!t || t->len + n > t->cap) {
        size_t cap = n > 32768 ? n : 32768;
        BrChunk *c = malloc(sizeof *c + cap);
        if (!c) return false;
        c->next = NULL;
        c->len = 0;
        c->cap = cap;
        if (t) t->next = c; else f->head = c;
        f->tail = c;
        t = c;
    }
    memcpy(t->data + t->len, d, n);
    t->len += n;
    f->queued += n;
    f->total += n;
    f->res.bytes = f->total;
    return true;
}

static void fail_locked(BrFetch *f, int err, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void fail_locked(BrFetch *f, int err, const char *fmt, ...) {
    f->res.error = err;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->res.error_text, sizeof f->res.error_text, fmt, ap);
    va_end(ap);
}

/* the headers of the answer are known: the first body bytes (or the end) are here */
static void headers_locked(BrFetch *f) {
    if (f->headers_known) return;
    f->headers_known = true;
    if (f->kind == BR_F_MAIN) f->ev_headers = true;
    wake_locked(f->l);
}

static void pump_locked(BrLoader *l);
static void *worker(void *arg);

static int priority(const BrFetch *f) { return f->kind == BR_F_MAIN ? 0 : (f->kind == BR_F_STYLE || f->kind == BR_F_SCRIPT) ? 1 : 2; }

/* starts waiting fetches while threads are free: the best class first, oldest first inside it */
static void pump_locked(BrLoader *l) {
    while (l->running < BR_FETCH_THREADS) {
        BrFetch *best = NULL;
        for (BrFetch *f = l->list; f; f = f->next) {
            if (f->state != 0 || f->cancelled) continue;
            if (!best || priority(f) < priority(best) || (priority(f) == priority(best) && f->id < best->id)) best = f;
        }
        if (!best) return;
        best->state = 1;
        best->t0 = now_ms();
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1u << 20);              /* TLS handshakes with big keys use a lot of stack */
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        l->running++;
        best->started = true;
        int rc = pthread_create(&best->thread, &attr, worker, best);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            l->running--;
            best->started = false;
            best->state = 2;
            fail_locked(best, BR_E_NOMEM, "the browser could not start a download thread (%s)", strerror(rc));
            headers_locked(best);
            wake_locked(l);
        }
    }
}

static void finish(BrFetch *f) {
    BrLoader *l = f->l;
    pthread_mutex_lock(&l->lock);
    if (f->cancelled && !f->res.error) fail_locked(f, BR_E_CANCELLED, "stopped");
    headers_locked(f);
    f->state = 2;
    l->running--;
    pump_locked(l);
    wake_locked(l);
    pthread_mutex_unlock(&l->lock);
}

/* ---------------------------------------------------------------- public API */

BrLoader *brl_new(void) {
    BrLoader *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    pthread_mutex_init(&l->lock, NULL);
    if (pipe(l->pipefd) != 0) { free(l); return NULL; }
    for (int i = 0; i < 2; i++) fcntl(l->pipefd[i], F_SETFL, fcntl(l->pipefd[i], F_GETFL, 0) | O_NONBLOCK);
    l->next_id = 1;
    return l;
}

void brl_free(BrLoader *l) {
    if (!l) return;
    pthread_mutex_lock(&l->lock);
    for (BrFetch *f = l->list; f; f = f->next) f->cancelled = true;
    pthread_mutex_unlock(&l->lock);
    /* running workers keep their BrFetch (they are detached and finish on their own); the loader itself is leaked on purpose
     * when some are still running, since they would touch it. A program frees the loader only when it is about to exit. */
    pthread_mutex_lock(&l->lock);
    bool busy = l->running > 0;
    pthread_mutex_unlock(&l->lock);
    if (busy) return;
    while (l->list) unlink_fetch(l, l->list);
    close(l->pipefd[0]);
    close(l->pipefd[1]);
    free(l);
}

int brl_wake_fd(const BrLoader *l) { return l->pipefd[0]; }

void brl_drain(BrLoader *l) {
    char buf[64];
    pthread_mutex_lock(&l->lock);
    while (read(l->pipefd[0], buf, sizeof buf) > 0) { }
    l->wake_pending = false;
    pthread_mutex_unlock(&l->lock);
}

uint32_t brl_start(BrLoader *l, const char *url, BrFetchKind kind, unsigned gen, size_t max_bytes) {
    if (!url || strlen(url) >= BR_MAX_URL) return 0;
    BrFetch *f = calloc(1, sizeof *f);
    if (!f) return 0;
    f->l = l;
    f->gen = gen;
    f->kind = kind;
    f->max_bytes = max_bytes ? max_bytes : BR_MAX_SUB_BYTES;
    snprintf(f->url, sizeof f->url, "%s", url);
    snprintf(f->res.url, sizeof f->res.url, "%s", url);
    pthread_mutex_lock(&l->lock);
    f->id = l->next_id++;
    f->next = l->list;
    l->list = f;
    uint32_t id = f->id;
    pump_locked(l);
    pthread_mutex_unlock(&l->lock);
    return id;
}

void brl_cancel_gen(BrLoader *l, unsigned gen) {
    pthread_mutex_lock(&l->lock);
    for (BrFetch *f = l->list; f; f = f->next) {
        if (f->gen != gen) continue;
        f->cancelled = true;
        if (f->state == 0) f->state = 2;                         /* never started: nothing will finish it */
    }
    pthread_mutex_unlock(&l->lock);
}

bool brl_next_event(BrLoader *l, BrEvent *ev) {
    bool found = false;
    pthread_mutex_lock(&l->lock);
    for (BrFetch *f = l->list, *next; f; f = next) {
        next = f->next;
        if (f->state == 2 && (f->cancelled || f->released)) { unlink_fetch(l, f); continue; }      /* nobody wants it any more */
        if (f->cancelled || found) continue;
        ev->id = f->id;
        ev->gen = f->gen;
        ev->kind = f->kind;
        if (f->ev_headers) { f->ev_headers = false; ev->type = BR_EV_HEADERS; found = true; }
        else if (f->ev_data && f->queued) { f->ev_data = false; ev->type = BR_EV_DATA; found = true; }
        else if (f->state == 2 && !f->done_reported) { f->done_reported = true; ev->type = BR_EV_DONE; found = true; }
        else f->ev_data = false;
    }
    pthread_mutex_unlock(&l->lock);
    return found;
}

bool brl_result(BrLoader *l, uint32_t id, BrResult *out) {
    pthread_mutex_lock(&l->lock);
    BrFetch *f = find_locked(l, id);
    if (f) *out = f->res;
    pthread_mutex_unlock(&l->lock);
    return f != NULL;
}

uint8_t *brl_take_data(BrLoader *l, uint32_t id, size_t *len) {
    uint8_t *buf = NULL;
    *len = 0;
    pthread_mutex_lock(&l->lock);
    BrFetch *f = find_locked(l, id);
    if (f && f->queued) {
        buf = malloc(f->queued);
        if (buf) {
            size_t o = 0;
            for (BrChunk *c = f->head; c; c = c->next) { memcpy(buf + o, c->data, c->len); o += c->len; }
            *len = o;
            free_chunks(f);
        }
    }
    pthread_mutex_unlock(&l->lock);
    return buf;
}

void brl_release(BrLoader *l, uint32_t id) {
    pthread_mutex_lock(&l->lock);
    BrFetch *f = find_locked(l, id);
    if (f) {
        f->released = true;
        if (f->state != 2) f->cancelled = true;
        if (f->state == 0) f->state = 2;
        if (f->state == 2) unlink_fetch(l, f);
    }
    pthread_mutex_unlock(&l->lock);
}

int brl_active(BrLoader *l) {
    int n = 0;
    pthread_mutex_lock(&l->lock);
    for (BrFetch *f = l->list; f; f = f->next) if (!f->cancelled && !(f->state == 2 && f->done_reported)) n++;
    pthread_mutex_unlock(&l->lock);
    return n;
}

/* ---------------------------------------------------------------- types */

static bool ext_is(const char *path, const char *ext) {
    size_t pl = strlen(path), el = strlen(ext);
    return pl > el && strcasecmp(path + pl - el, ext) == 0;
}

const char *brl_guess_type(const char *path, const uint8_t *head, size_t n) {
    static const char *const text_ext[] = { ".txt", ".log", ".md", ".c", ".h", ".cc", ".cpp", ".hpp", ".py", ".js", ".json", ".css", ".xml", ".csv", ".ini",
                                            ".conf", ".cfg", ".sh", ".rs", ".go", ".java", ".asm", ".s", ".mk", ".toml", ".yaml", ".yml", ".tex", ".patch", ".diff", NULL };
    if (ext_is(path, ".html") || ext_is(path, ".htm") || ext_is(path, ".xhtml")) return "text/html";
    if (ext_is(path, ".png")) return "image/png";
    if (ext_is(path, ".jpg") || ext_is(path, ".jpeg") || ext_is(path, ".jpe")) return "image/jpeg";
    if (ext_is(path, ".gif")) return "image/gif";
    if (ext_is(path, ".bmp")) return "image/bmp";
    if (ext_is(path, ".tga")) return "image/x-tga";
    if (ext_is(path, ".ppm") || ext_is(path, ".pgm") || ext_is(path, ".pbm") || ext_is(path, ".pnm")) return "image/x-portable-anymap";
    for (int i = 0; text_ext[i]; i++) if (ext_is(path, text_ext[i])) return "text/plain";
    if (n >= 8 && !memcmp(head, "\x89PNG\r\n\x1a\n", 8)) return "image/png";
    if (n >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF) return "image/jpeg";
    if (n >= 6 && (!memcmp(head, "GIF87a", 6) || !memcmp(head, "GIF89a", 6))) return "image/gif";
    if (n >= 2 && head[0] == 'B' && head[1] == 'M') return "image/bmp";
    size_t bad = 0;
    for (size_t i = 0; i < n && i < 512; i++) if (head[i] == 0 || (head[i] < 9) || (head[i] > 13 && head[i] < 32 && head[i] != 27)) bad++;
    if (n && bad == 0) {
        for (size_t i = 0; i + 8 < n && i < 256; i++)
            if (head[i] == '<' && (!strncasecmp((const char *)head + i, "<!doctype html", 14) || !strncasecmp((const char *)head + i, "<html", 5))) return "text/html";
        return "text/plain";
    }
    return n ? "application/octet-stream" : "text/plain";
}

/* ---------------------------------------------------------------- files, folders and data: URLs */

static void escape_html(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 7 < cap; s++) {
        switch (*s) {
        case '<': memcpy(out + o, "&lt;", 4); o += 4; break;
        case '>': memcpy(out + o, "&gt;", 4); o += 4; break;
        case '&': memcpy(out + o, "&amp;", 5); o += 5; break;
        case '"': memcpy(out + o, "&quot;", 6); o += 6; break;
        default: out[o++] = *s;
        }
    }
    out[o] = 0;
}

static void encode_name(const char *s, char *out, size_t cap) {         /* a file name as a relative URL path segment */
    static const char hex[] = "0123456789ABCDEF";
    size_t h = 0;
    for (; *s && h + 4 < cap; s++) {
        unsigned char ch = (unsigned char)*s;
        if (isalnum(ch) || strchr("-._~", ch)) out[h++] = (char)ch;
        else { out[h++] = '%'; out[h++] = hex[ch >> 4]; out[h++] = hex[ch & 15]; }
    }
    out[h] = 0;
}

typedef struct DirEnt { char name[256]; bool dir; long long size; } DirEnt;

static int cmp_dirent(const void *a, const void *b) {
    const DirEnt *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

/* an index of a folder as a page: name links, sizes */
static void fetch_folder(BrFetch *f, const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        pthread_mutex_lock(&f->l->lock);
        fail_locked(f, errno == EACCES ? BR_E_ACCESS : BR_E_NOT_FOUND, "%s: %s", path, strerror(errno));
        pthread_mutex_unlock(&f->l->lock);
        return;
    }
    size_t cap = 256, n = 0;
    DirEnt *list = malloc(cap * sizeof *list);
    struct dirent *e;
    while (list && (e = readdir(d)) != NULL && n < 20000) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) {
            DirEnt *nl = realloc(list, cap * 2 * sizeof *list);
            if (!nl) break;
            list = nl;
            cap *= 2;
        }
        DirEnt *de = &list[n++];
        snprintf(de->name, sizeof de->name, "%s", e->d_name);
        char full[BR_MAX_URL];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        struct stat st;
        de->dir = false;
        de->size = -1;
        if (stat(full, &st) == 0) { de->dir = S_ISDIR(st.st_mode); de->size = de->dir ? -1 : (long long)st.st_size; }
    }
    closedir(d);
    if (list) qsort(list, n, sizeof *list, cmp_dirent);
    char epath[BR_MAX_URL * 2];
    escape_html(path, epath, sizeof epath);
    size_t hcap = 2048 + n * 700;
    char *html = malloc(hcap);
    if (!html) { free(list); return; }
    size_t o = (size_t)snprintf(html, hcap,
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>Index of %s</title><style>"
        "body{font-family:sans-serif;margin:24px;color:#1a1a24;background:#f4efd8}h1{font-size:22px;color:#8e0b21;border-bottom:2px solid #c8102e;padding-bottom:6px}"
        "table{border-collapse:collapse}td{padding:2px 18px 2px 0}a{color:#1d2a5c}.size{color:#666;text-align:right}.note{color:#666;margin-top:18px;font-size:13px}"
        "</style></head><body><h1>Index of %s</h1><table>", epath, epath);
    if (strcmp(path, "/") != 0) o += (size_t)snprintf(html + o, hcap - o, "<tr><td><a href=\"../\">../</a></td><td class=\"size\">parent folder</td></tr>");
    for (size_t i = 0; i < n && o + 1000 < hcap; i++) {
        char href[1024], name[600];
        encode_name(list[i].name, href, sizeof href);
        escape_html(list[i].name, name, sizeof name);
        char size[40] = "";
        if (!list[i].dir && list[i].size >= 0) {
            if (list[i].size < 10240) snprintf(size, sizeof size, "%lld bytes", list[i].size);
            else snprintf(size, sizeof size, "%lld KiB", list[i].size / 1024);
        } else if (list[i].dir) snprintf(size, sizeof size, "folder");
        o += (size_t)snprintf(html + o, hcap - o, "<tr><td><a href=\"%s%s\">%s%s</a></td><td class=\"size\">%s</td></tr>", href, list[i].dir ? "/" : "", name,
                              list[i].dir ? "/" : "", size);
    }
    o += (size_t)snprintf(html + o, hcap - o, "</table><p class=\"note\">%zu entries%s. Gensokyo's files, as the Explorer sees them.</p></body></html>", n, n >= 20000 ? " (the list stops at 20000)" : "");
    free(list);
    pthread_mutex_lock(&f->l->lock);
    f->res.status = 200;
    snprintf(f->res.content_type, sizeof f->res.content_type, "text/html; charset=utf-8");
    append_locked(f, html, o);
    headers_locked(f);
    if (f->kind == BR_F_MAIN) f->ev_data = true;
    wake_locked(f->l);
    pthread_mutex_unlock(&f->l->lock);
    free(html);
}

static void fetch_file(BrFetch *f) {
    char path[BR_MAX_URL];
    if (!br_file_url_to_path(f->url, path, sizeof path)) {
        pthread_mutex_lock(&f->l->lock);
        fail_locked(f, BR_E_URL, "%s is not a file address this computer can use (only files on this computer, no host name)", f->url);
        pthread_mutex_unlock(&f->l->lock);
        return;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        pthread_mutex_lock(&f->l->lock);
        fail_locked(f, errno == EACCES ? BR_E_ACCESS : BR_E_NOT_FOUND, "%s: %s", path, strerror(errno));
        pthread_mutex_unlock(&f->l->lock);
        return;
    }
    if (S_ISDIR(st.st_mode)) { fetch_folder(f, path); return; }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        pthread_mutex_lock(&f->l->lock);
        fail_locked(f, errno == EACCES ? BR_E_ACCESS : BR_E_NOT_FOUND, "%s: %s", path, strerror(errno));
        pthread_mutex_unlock(&f->l->lock);
        return;
    }
    uint8_t buf[16384];
    bool first = true;
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        pthread_mutex_lock(&f->l->lock);
        if (r < 0) fail_locked(f, BR_E_IO, "%s: %s", path, strerror(errno));
        if (f->cancelled || r <= 0) {
            if (first) { f->res.status = 200; snprintf(f->res.content_type, sizeof f->res.content_type, "%s", brl_guess_type(path, buf, 0)); }
            pthread_mutex_unlock(&f->l->lock);
            break;
        }
        if (first) {
            first = false;
            f->res.status = 200;
            snprintf(f->res.content_type, sizeof f->res.content_type, "%s%s", brl_guess_type(path, buf, (size_t)r),
                     strncmp(brl_guess_type(path, buf, (size_t)r), "text/", 5) == 0 ? "; charset=utf-8" : "");
        }
        size_t take = (size_t)r;
        if (f->total + take > f->max_bytes) { take = f->max_bytes - (size_t)f->total; f->res.truncated = true; }
        append_locked(f, buf, take);
        headers_locked(f);
        if (f->kind == BR_F_MAIN) f->ev_data = true;
        wake_locked(f->l);
        bool stop = f->res.truncated;
        pthread_mutex_unlock(&f->l->lock);
        if (stop) break;
    }
    close(fd);
}

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* data:[<type>][;base64],<data> */
static void fetch_data(BrFetch *f) {
    const char *p = f->url + 5;
    const char *comma = strchr(p, ',');
    pthread_mutex_lock(&f->l->lock);
    if (!comma) { fail_locked(f, BR_E_URL, "a data: address needs a comma before its data"); pthread_mutex_unlock(&f->l->lock); return; }
    size_t tl = (size_t)(comma - p);
    bool b64 = false;
    char type[160] = "text/plain;charset=US-ASCII";
    if (tl > 0) {
        char meta[160];
        snprintf(meta, sizeof meta, "%.*s", (int)(tl < sizeof meta - 1 ? tl : sizeof meta - 1), p);
        size_t ml = strlen(meta);
        if (ml >= 7 && strcasecmp(meta + ml - 7, ";base64") == 0) { b64 = true; meta[ml - 7] = 0; }
        if (meta[0]) snprintf(type, sizeof type, "%s", meta);
        else if (b64) snprintf(type, sizeof type, "text/plain;charset=US-ASCII");
    }
    pthread_mutex_unlock(&f->l->lock);
    size_t dl = strlen(comma + 1);
    uint8_t *out = malloc(dl + 1);
    if (!out) { pthread_mutex_lock(&f->l->lock); fail_locked(f, BR_E_NOMEM, "out of memory"); pthread_mutex_unlock(&f->l->lock); return; }
    size_t o = 0;
    if (b64) {
        unsigned acc = 0;
        int bits = 0;
        for (const char *c = comma + 1; *c; c++) {
            if (*c == '%' && isxdigit((unsigned char)c[1]) && isxdigit((unsigned char)c[2])) {          /* percent-escapes may hide inside base64 text */
                char hx[3] = { c[1], c[2], 0 };
                int ch = (int)strtol(hx, NULL, 16);
                c += 2;
                int v = b64_val(ch);
                if (v < 0) continue;
                acc = acc << 6 | (unsigned)v;
                bits += 6;
            } else {
                int v = b64_val((unsigned char)*c);
                if (v < 0) continue;
                acc = acc << 6 | (unsigned)v;
                bits += 6;
            }
            if (bits >= 8) { bits -= 8; out[o++] = (uint8_t)(acc >> bits); acc &= (1u << bits) - 1; }
        }
    } else {
        memcpy(out, comma + 1, dl);
        out[dl] = 0;
        o = br_percent_decode((char *)out);
    }
    pthread_mutex_lock(&f->l->lock);
    f->res.status = 200;
    snprintf(f->res.content_type, sizeof f->res.content_type, "%s", type);
    size_t take = o;
    if (take > f->max_bytes) { take = f->max_bytes; f->res.truncated = true; }
    append_locked(f, out, take);
    headers_locked(f);
    if (f->kind == BR_F_MAIN) f->ev_data = true;
    wake_locked(f->l);
    pthread_mutex_unlock(&f->l->lock);
    free(out);
}

/* ---------------------------------------------------------------- http and https */

static const TlsTrust *shared_trust(char *err, size_t cap) {
    pthread_mutex_lock(&g_trust_lock);
    if (!g_trust_tried) {
        g_trust_tried = true;
        const char *path = getenv("SSL_CERT_FILE");
        if (!path || !*path) path = TLS_DEFAULT_CA_FILE;
        TlsTrust *t = tls_trust_new();
        int n = t ? tls_trust_load_file(t, path) : -1;
        if (n > 0) g_trust = t;
        else {
            if (t) tls_trust_free(t);
            snprintf(g_trust_error, sizeof g_trust_error, "there are no trusted root certificates (no usable bundle at %s): put one there, or point SSL_CERT_FILE at one, "
                     "and this page will be fetched with every certificate checked", path);
        }
    }
    const TlsTrust *t = g_trust;
    if (!t) snprintf(err, cap, "%s", g_trust_error);
    pthread_mutex_unlock(&g_trust_lock);
    return t;
}

static void dns_lock(BrFetch *f) { if (!f->dns_held) { pthread_mutex_lock(&g_dns_lock); f->dns_held = true; } }
static void dns_unlock(BrFetch *f) { if (f->dns_held) { f->dns_held = false; pthread_mutex_unlock(&g_dns_lock); } }

/* the resolver is done once libhttp says it is connecting; a redirect makes it resolve again */
static void on_event(void *ctx, const char *line) {
    BrFetch *f = ctx;
    if (strncmp(line, "connecting to ", 14) == 0) dns_unlock(f);
}

static void on_status(void *ctx, int status, const char *reason) {
    (void)reason;
    BrFetch *f = ctx;
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) dns_lock(f);
}

static int on_header(void *ctx, const char *name, const char *value) {
    BrFetch *f = ctx;
    if (f->kind != BR_F_MAIN && strcasecmp(name, "Content-Length") == 0) {
        long long n = atoll(value);
        if (n > 0 && (unsigned long long)n > f->max_bytes) { f->stop = BR_E_TOO_BIG; return 1; }
    }
    return 0;
}

static void on_tls(void *ctx, const TlsInfo *ti) {
    BrFetch *f = ctx;
    pthread_mutex_lock(&f->l->lock);
    const char *cn = ti->path_len > 0 ? ti->path[0].subject : "";
    const char *group = ti->group_name && *ti->group_name ? ti->group_name : "";              /* a handshake that failed early has no group yet */
    snprintf(f->res.tls, sizeof f->res.tls, "%s, %s%s%s; %s%s%s", ti->version_name ? ti->version_name : "TLS", ti->suite_name && *ti->suite_name ? ti->suite_name : "no suite agreed",
             *group ? ", " : "", group, ti->verified ? "certificate verified" : "certificate NOT verified", *cn ? ": " : "", cn);
    pthread_mutex_unlock(&f->l->lock);
}

static int on_body(void *ctx, const uint8_t *d, size_t n) {
    BrFetch *f = ctx;
    pthread_mutex_lock(&f->l->lock);
    int stop = 0;
    if (f->cancelled) stop = BR_E_CANCELLED;
    else if (now_ms() - f->t0 > BR_DEADLINE_MS) stop = BR_E_TIMEOUT;
    if (!stop) {
        if (!f->headers_known) {
            HttpResult *h = f->hr;
            f->res.status = h->status;
            snprintf(f->res.url, sizeof f->res.url, "%s", h->url);
            snprintf(f->res.content_type, sizeof f->res.content_type, "%s", h->content_type);
            f->res.redirects = h->redirects;
            f->res.downgraded = h->downgraded != 0;
            headers_locked(f);
        }
        size_t take = n;
        if (f->total + take > f->max_bytes) { take = f->max_bytes - (size_t)f->total; f->res.truncated = true; stop = BR_E_TOO_BIG; }
        if (!append_locked(f, d, take)) stop = BR_E_NOMEM;
        if (f->kind == BR_F_MAIN) f->ev_data = true;
        wake_locked(f->l);
    }
    if (stop) f->stop = stop;
    pthread_mutex_unlock(&f->l->lock);
    return stop ? 1 : 0;
}

static int map_http_error(const BrFetch *f, const HttpResult *h) {
    switch (h->error) {
    case HTTP_E_URL: return BR_E_URL;
    case HTTP_E_DNS: return BR_E_DNS;
    case HTTP_E_CONNECT: return BR_E_CONNECT;
    case HTTP_E_TLS: return BR_E_TLS;
    case HTTP_E_IO: return BR_E_IO;
    case HTTP_E_TIMEOUT: return BR_E_TIMEOUT;
    case HTTP_E_PARSE: case HTTP_E_TOO_BIG: case HTTP_E_DECODE: case HTTP_E_UNSUPPORTED: return BR_E_PARSE;
    case HTTP_E_TRUNCATED: return BR_E_TRUNCATED;
    case HTTP_E_REDIRECTS: return BR_E_REDIRECTS;
    case HTTP_E_NOMEM: return BR_E_NOMEM;
    case HTTP_E_CALLBACK: return f->stop ? f->stop : BR_E_CANCELLED;
    default: return BR_E_IO;
    }
}

/* BR_TIMEOUT_MS, or $BROWSER_TIMEOUT_MS when it is set: a knob for the tests, which cannot wait twenty seconds for a server that stays silent */
static int test_timeout_ms(void) {
    const char *e = getenv("BROWSER_TIMEOUT_MS");
    int v = e ? atoi(e) : 0;
    return v > 0 ? v : BR_TIMEOUT_MS;
}

static void fetch_http(BrFetch *f) {
    bool https = br_url_kind(f->url) == BR_URL_HTTPS;
    TlsConfig tc;
    memset(&tc, 0, sizeof tc);
    if (https) {
        char err[300];
        const TlsTrust *t = shared_trust(err, sizeof err);
        if (!t) {
            pthread_mutex_lock(&f->l->lock);
            fail_locked(f, BR_E_NOTRUST, "%s", err);
            pthread_mutex_unlock(&f->l->lock);
            return;
        }
        tc.trust = t;
    }
    HttpResult hr;
    memset(&hr, 0, sizeof hr);
    f->hr = &hr;
    HttpOptions o;
    memset(&o, 0, sizeof o);
    o.tls = &tc;
    o.max_redirects = BR_MAX_REDIRECTS;
    o.timeout_ms = test_timeout_ms();
    o.extra_headers = "Accept-Language: en\r\n";
    o.on_body = on_body;
    o.on_status = on_status;
    o.on_header = on_header;
    o.on_event = on_event;
    o.on_tls = on_tls;
    o.ctx = f;
    dns_lock(f);
    int rc = http_get(f->url, &o, &hr);
    dns_unlock(f);
    pthread_mutex_lock(&f->l->lock);
    f->res.status = hr.status;
    snprintf(f->res.url, sizeof f->res.url, "%s", hr.url[0] ? hr.url : f->url);
    snprintf(f->res.content_type, sizeof f->res.content_type, "%s", hr.content_type);
    f->res.redirects = hr.redirects;
    f->res.downgraded = hr.downgraded != 0;
    if (rc != 0) {
        int e = map_http_error(f, &hr);
        if (e == BR_E_TOO_BIG && f->res.truncated && f->kind == BR_F_MAIN) { /* the page is longer than we read: what we have is shown, with a note */ }
        else {
            f->res.tls_error = hr.tls_error;
            const char *text = hr.error_text;
            if (e == BR_E_TOO_BIG) text = "this is larger than the browser reads (see the limits in docs/BROWSER.md)";
            else if (e == BR_E_TIMEOUT && f->stop == BR_E_TIMEOUT) text = "the download took longer than the browser waits for one resource";
            fail_locked(f, e, "%s", text && *text ? text : http_strerror(hr.error));
        }
    }
    f->hr = NULL;
    pthread_mutex_unlock(&f->l->lock);
}

static void *worker(void *arg) {
    BrFetch *f = arg;
    switch (br_url_kind(f->url)) {
    case BR_URL_HTTP: case BR_URL_HTTPS: fetch_http(f); break;
    case BR_URL_FILE: fetch_file(f); break;
    case BR_URL_DATA: fetch_data(f); break;
    default:
        pthread_mutex_lock(&f->l->lock);
        fail_locked(f, BR_E_SCHEME, "the browser cannot fetch '%.200s' (it speaks http, https, file and data)", f->url);
        pthread_mutex_unlock(&f->l->lock);
        break;
    }
    finish(f);
    return NULL;
}
