#include "kappabox.h"
#include <stdarg.h>

int kb_color;

void kb_error(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s: ", kb_name);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void kb_die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s: ", kb_name);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/* ---- /etc/passwd and /etc/group lookups with a tiny cache ---- */

typedef struct Ent { unsigned id; char name[32]; } Ent;
static Ent g_users[64], g_groups[64];
static int g_nusers = -1, g_ngroups = -1;

static int load_db(const char *path, Ent *tab) {
    FILE *f = fopen(path, "r");
    int n = 0;
    if (!f) return 0;
    char line[256];
    while (n < 64 && fgets(line, sizeof line, f)) {
        char *name = strtok(line, ":");
        char *pw = name ? strtok(NULL, ":") : NULL;
        char *id = pw ? strtok(NULL, ":") : NULL;
        if (!name || !id) continue;
        strlcpy(tab[n].name, name, sizeof tab[n].name);
        tab[n].id = (unsigned)atoi(id);
        n++;
    }
    fclose(f);
    return n;
}

static const char *lookup(Ent *tab, int n, unsigned id, char *fallback) {
    for (int i = 0; i < n; i++) if (tab[i].id == id) return tab[i].name;
    snprintf(fallback, 16, "%u", id);
    return fallback;
}

const char *kb_user_name(unsigned uid) {
    static char fb[16];
    if (g_nusers < 0) g_nusers = load_db("/etc/passwd", g_users);
    return lookup(g_users, g_nusers, uid, fb);
}

const char *kb_group_name(unsigned gid) {
    static char fb[16];
    if (g_ngroups < 0) g_ngroups = load_db("/etc/group", g_groups);
    return lookup(g_groups, g_ngroups, gid, fb);
}

int kb_user_id(const char *name) {
    if (g_nusers < 0) g_nusers = load_db("/etc/passwd", g_users);
    for (int i = 0; i < g_nusers; i++) if (!strcmp(g_users[i].name, name)) return (int)g_users[i].id;
    return isdigit((unsigned char)name[0]) ? atoi(name) : -1;
}

int kb_group_id(const char *name) {
    if (g_ngroups < 0) g_ngroups = load_db("/etc/group", g_groups);
    for (int i = 0; i < g_ngroups; i++) if (!strcmp(g_groups[i].name, name)) return (int)g_groups[i].id;
    return isdigit((unsigned char)name[0]) ? atoi(name) : -1;
}

char *kb_join(const char *dir, const char *name) {
    size_t dl = strlen(dir);
    char *s = malloc(dl + strlen(name) + 2);
    if (!s) kb_die("out of memory");
    if (dl && dir[dl - 1] == '/') sprintf(s, "%s%s", dir, name);
    else sprintf(s, "%s/%s", dir, name);
    return s;
}

int kb_term_cols(void) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col) return ws.ws_col;
    return 80;
}

int kb_copy_fd(int in, int out) {
    char buf[8192];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) return -1;
            off += w;
        }
    }
    return n < 0 ? -1 : 0;
}

int kb_copy_file(const char *src, const char *dst, int preserve_mode) {
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    struct stat st;
    fstat(in, &st);
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, preserve_mode ? (st.st_mode & 07777) : 0644);
    if (out < 0) { close(in); return -1; }
    int r = kb_copy_fd(in, out);
    close(in);
    close(out);
    return r;
}

int kb_remove_tree(const char *path, int force, int verbose) {
    struct stat st;
    if (lstat(path, &st) < 0) { if (force && errno == ENOENT) return 0; kb_error("cannot remove '%s': %s", path, strerror(errno)); return -1; }
    int rc = 0;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) { kb_error("cannot open '%s': %s", path, strerror(errno)); return -1; }
        struct dirent *e;
        char **names = NULL;
        int n = 0;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            names = realloc(names, (size_t)(n + 1) * sizeof(char *));
            names[n++] = strdup(e->d_name);
        }
        closedir(d);
        for (int i = 0; i < n; i++) {
            char *child = kb_join(path, names[i]);
            if (kb_remove_tree(child, force, verbose) < 0) rc = -1;
            free(child);
            free(names[i]);
        }
        free(names);
        if (rmdir(path) < 0) { kb_error("cannot remove '%s': %s", path, strerror(errno)); rc = -1; }
        else if (verbose) printf("removed directory '%s'\n", path);
    } else {
        if (unlink(path) < 0) { kb_error("cannot remove '%s': %s", path, strerror(errno)); rc = -1; }
        else if (verbose) printf("removed '%s'\n", path);
    }
    return rc;
}

int kb_each_input(int argc, char **argv, int first, int (*fn)(FILE *, const char *, void *), void *ctx) {
    int rc = 0;
    if (first >= argc) return fn(stdin, "-", ctx) ? 1 : 0;
    for (int i = first; i < argc; i++) {
        FILE *f = stdin;
        if (strcmp(argv[i], "-")) {
            f = fopen(argv[i], "r");
            if (!f) { kb_error("%s: %s", argv[i], strerror(errno)); rc = 1; continue; }
        }
        if (fn(f, argv[i], ctx)) rc = 1;
        if (f != stdin) fclose(f);
    }
    return rc;
}

size_t kb_read_lines(FILE *f, char ***out) {
    char **v = NULL;
    size_t n = 0, cap = 0, bufcap = 0;
    char *buf = NULL;
    ssize_t len;
    while ((len = getline(&buf, &bufcap, f)) >= 0) {
        if (len && buf[len - 1] == '\n') buf[--len] = 0;
        if (n == cap) { cap = cap ? cap * 2 : 64; v = realloc(v, cap * sizeof *v); if (!v) kb_die("out of memory"); }
        v[n++] = strdup(buf);
    }
    free(buf);
    *out = v;
    return n;
}

void kb_free_lines(char **lines, size_t n) {
    for (size_t i = 0; i < n; i++) free(lines[i]);
    free(lines);
}

int kb_parse_ranges(const char *spec, int *lo, int *hi, int max) {
    int n = 0;
    const char *p = spec;
    while (*p && n < max) {
        int a = 1, b = INT_MAX;
        if (isdigit((unsigned char)*p)) { a = (int)strtol(p, (char **)&p, 10); b = a; }
        if (*p == '-') {
            p++;
            if (isdigit((unsigned char)*p)) b = (int)strtol(p, (char **)&p, 10);
            else b = INT_MAX;
        }
        lo[n] = a;
        hi[n] = b;
        n++;
        if (*p == ',') p++;
        else if (*p) break;
    }
    return n;
}

int kb_in_ranges(int pos, const int *lo, const int *hi, int n) {
    for (int i = 0; i < n; i++) if (pos >= lo[i] && pos <= hi[i]) return 1;
    return 0;
}

char *kb_read_all(int fd, size_t *len) {
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    if (!buf) kb_die("out of memory");
    for (;;) {
        if (n + 1024 >= cap) { cap *= 2; buf = realloc(buf, cap); if (!buf) kb_die("out of memory"); }
        ssize_t r = read(fd, buf + n, cap - n - 1);
        if (r <= 0) break;
        n += (size_t)r;
    }
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}

char *kb_format_size(unsigned long long b, char *buf, size_t n) {
    static const char *const units = "BKMGT";
    double v = (double)b;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    if (u == 0) snprintf(buf, n, "%llu", b);
    else snprintf(buf, n, v < 10 ? "%.1f%c" : "%.0f%c", v, units[u]);
    return buf;
}

void kb_mode_string(unsigned mode, char *out) {
    char t = '?';
    switch (mode & S_IFMT) {
    case S_IFDIR: t = 'd'; break;
    case S_IFREG: t = '-'; break;
    case S_IFLNK: t = 'l'; break;
    case S_IFCHR: t = 'c'; break;
    case S_IFBLK: t = 'b'; break;
    case S_IFIFO: t = 'p'; break;
    }
    out[0] = t;
    for (int i = 0; i < 9; i++) out[1 + i] = (mode & (0400u >> i)) ? "rwx"[i % 3] : '-';
    if (mode & S_ISUID) out[3] = (mode & 0100) ? 's' : 'S';
    if (mode & S_ISGID) out[6] = (mode & 010) ? 's' : 'S';
    if (mode & S_ISVTX) out[9] = (mode & 01) ? 't' : 'T';
    out[10] = 0;
}
