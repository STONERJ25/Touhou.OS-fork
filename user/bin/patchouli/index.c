/* Finding pages: scans the page directories for <section>/<name>.pw and reads each page's header. */
#include "patchouli.h"

char *read_file(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    if (!buf) { close(fd); return NULL; }
    for (;;) {
        if (n + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); close(fd); return NULL; }
            buf = nb;
        }
        ssize_t r = read(fd, buf + n, cap - n - 1);
        if (r < 0) { if (errno == EINTR) continue; free(buf); close(fd); return NULL; }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}

static void add_entry(Index *ix, const char *name, const char *sec, const char *desc, const char *path, const char *target, int is_alias) {
    if (ix->n == ix->cap) {
        ix->cap = ix->cap ? ix->cap * 2 : 64;
        ix->e = realloc(ix->e, (size_t)ix->cap * sizeof *ix->e);
        if (!ix->e) pt_oom();
    }
    Entry *e = &ix->e[ix->n++];
    e->name = strdup(name);
    e->sec = strdup(sec);
    e->desc = strdup(desc);
    e->path = strdup(path);
    e->target = target ? strdup(target) : NULL;
    e->is_alias = is_alias;
}

static int sec_rank(const char *sec) {
    static const char order[] = "1857";
    const char *p = strchr(order, sec[0]);
    return p ? (int)(p - order) : 10 + sec[0];
}

int index_cmp_name(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    int r = strcmp(x->name, y->name);
    if (r) return r;
    return sec_rank(x->sec) - sec_rank(y->sec);
}

static void scan_section(Index *ix, const char *root, const char *sec) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/%s", root, sec);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l < 4 || strcmp(de->d_name + l - 3, ".pw")) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        size_t len;
        char *src = read_file(path, &len);
        if (!src) continue;
        Page pg;
        memset(&pg, 0, sizeof pg);
        /* only the header matters here: parse the first part of the file */
        size_t head = len;
        const char *h1 = strstr(src, "\n# ");
        if (h1) head = (size_t)(h1 - src) + 1;
        pw_parse(&pg, src, head);
        char base[256];
        snprintf(base, sizeof base, "%.*s", (int)(l - 3), de->d_name);
        const char *name = pg.name ? pg.name : base;
        const char *psec = pg.sec ? pg.sec : sec;
        add_entry(ix, name, psec, pg.desc ? pg.desc : "(no description)", path, NULL, 0);
        for (int i = 0; i < pg.naliases; i++) {
            char desc[300];
            snprintf(desc, sizeof desc, "alias for %s(%s)", name, psec);
            add_entry(ix, pg.aliases[i], psec, desc, path, name, 1);
        }
        pw_free(&pg);
        free(src);
    }
    closedir(d);
}

void index_load(Index *ix) {
    memset(ix, 0, sizeof *ix);
    const char *env = getenv("PATCHOULI_PATH");
    char *roots = strdup(env && *env ? env : DEFAULT_ROOT);
    char *save, *tok;
    for (tok = strtok_r(roots, ":", &save); tok; tok = strtok_r(NULL, ":", &save)) {
        ix->roots = realloc(ix->roots, (size_t)(ix->nroots + 1) * sizeof(char *));
        ix->roots[ix->nroots++] = strdup(tok);
        DIR *d = opendir(tok);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
            scan_section(ix, tok, de->d_name);
        }
        closedir(d);
    }
    free(roots);
    if (ix->n) qsort(ix->e, (size_t)ix->n, sizeof *ix->e, index_cmp_name);
}

/* Looks a page up by name (and section, if given). Without a section the order 1, 8, 5, 7 decides. */
Entry *index_find(Index *ix, const char *section, const char *name) {
    for (int pass = 0; pass < 2; pass++) {
        Entry *best = NULL;
        for (int i = 0; i < ix->n; i++) {
            Entry *e = &ix->e[i];
            if (pass == 0 ? strcmp(e->name, name) : strcasecmp(e->name, name)) continue;
            if (section && strcmp(e->sec, section)) continue;
            if (!best || sec_rank(e->sec) < sec_rank(best->sec) || (sec_rank(e->sec) == sec_rank(best->sec) && best->is_alias && !e->is_alias)) best = e;
        }
        if (best) return best;
    }
    return NULL;
}
