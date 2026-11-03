/* patch: apply a diff (unified, context or normal format) to files.
 * Hunks must match exactly (no fuzz): each is tried at the line the diff names, then at growing distance before and after
 * (never before the end of the previous hunk). Whole files are kept as arrays of line offsets, so big files are fine.
 * Options: -p N -d DIR -i FILE -o FILE -R -N -E -b -z SUF -s -f -t --dry-run -F N -r REJFILE. */
#include "kappabox.h"

typedef struct { char t; const char *p; size_t len; int nonl; } PLine;       /* t: ' ' context, '-' old only, '+' new only */

typedef struct {
    long old_start, old_count, new_start, new_count;
    PLine *l;
    long nl, cap;
} Hunk;

typedef struct {
    char *old_name, *new_name, *idx_name;
    int fmt;                                /* 0 normal, 1 unified, 2 context */
    long header_at;                         /* line of the patch where it starts (for messages) */
    Hunk *h;
    long nh, cap;
} FPatch;

typedef struct { const char *p; size_t len; } Ln;

static int o_strip, o_reverse, o_dry, o_forward, o_backup, o_quiet, o_force, o_batch, o_remove_empty;
static const char *o_input, *o_dir, *o_output, *o_suffix = ".orig", *o_rejfile;

static char *g_pbuf;
static Ln *g_lines;
static long g_nlines;

/* ------------------------------------------------------------------ reading the patch */

static char *slurp_fd(int fd, size_t *lenp) {
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap);
    if (!buf) kb_die("out of memory");
    for (;;) {
        ssize_t r;
        if (n + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); if (!buf) kb_die("out of memory"); }
        r = read(fd, buf + n, cap - n - 1);
        if (r < 0) kb_die("read error: %s", strerror(errno));
        if (r == 0) break;
        n += (size_t)r;
    }
    buf[n] = 0;
    *lenp = n;
    return buf;
}

static void split_patch(char *buf, size_t len) {
    long n = 0, cap = 1024, i = 0;
    const char *p = buf, *e = buf + len;
    g_lines = malloc((size_t)cap * sizeof *g_lines);
    if (!g_lines) kb_die("out of memory");
    while (p < e) {
        const char *q = memchr(p, '\n', (size_t)(e - p));
        size_t l = q ? (size_t)(q - p) : (size_t)(e - p);
        if (n == cap) { cap *= 2; g_lines = realloc(g_lines, (size_t)cap * sizeof *g_lines); if (!g_lines) kb_die("out of memory"); }
        g_lines[n].p = p;
        g_lines[n].len = l;
        n++;
        p = q ? q + 1 : e;
    }
    (void)i;
    g_nlines = n;
}

static int is(const Ln *l, const char *prefix) {
    size_t n = strlen(prefix);
    return l->len >= n && !memcmp(l->p, prefix, n);
}

/* The file name in a "--- name<TAB>date" line. */
static char *header_name(const Ln *l) {
    const char *p = l->p + 4, *e = l->p + l->len;
    const char *t = memchr(p, '\t', (size_t)(e - p));
    if (t) e = t;
    else {
        const char *s = memchr(p, ' ', (size_t)(e - p));
        if (s) e = s;
    }
    return strndup(p, (size_t)(e - p));
}

static Hunk *add_hunk(FPatch *fp) {
    Hunk *h;
    if (fp->nh == fp->cap) {
        fp->cap = fp->cap ? fp->cap * 2 : 8;
        fp->h = realloc(fp->h, (size_t)fp->cap * sizeof *fp->h);
        if (!fp->h) kb_die("out of memory");
    }
    h = &fp->h[fp->nh++];
    memset(h, 0, sizeof *h);
    return h;
}

static void add_line(Hunk *h, char t, const char *p, size_t len) {
    if (h->nl == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 16;
        h->l = realloc(h->l, (size_t)h->cap * sizeof *h->l);
        if (!h->l) kb_die("out of memory");
    }
    h->l[h->nl].t = t;
    h->l[h->nl].p = p;
    h->l[h->nl].len = len;
    h->l[h->nl].nonl = 0;
    h->nl++;
}

static void mark_nonl(Hunk *h) { if (h->nl) h->l[h->nl - 1].nonl = 1; }

/* "a[,b]" -> first and count (count 1 when there is no comma). Returns the pointer after it. */
static const char *parse_pair(const char *s, long *a, long *cnt, int range) {
    char *e;
    *a = strtol(s, &e, 10);
    *cnt = 1;
    if (*e == ',') {
        long b = strtol(e + 1, &e, 10);
        *cnt = range ? b - *a + 1 : b;
    }
    return e;
}

/* @@ -a,b +c,d @@ */
static long parse_unified_hunk(FPatch *fp, long i) {
    const Ln *hl = &g_lines[i];
    const char *p = hl->p + 3;
    Hunk *h;
    long oc = 0, nc = 0;
    if (*p != '-') return i + 1;
    h = add_hunk(fp);
    p = parse_pair(p + 1, &h->old_start, &h->old_count, 0);
    while (*p == ' ') p++;
    if (*p == '+') parse_pair(p + 1, &h->new_start, &h->new_count, 0);
    i++;
    while (i < g_nlines && (oc < h->old_count || nc < h->new_count)) {
        const Ln *l = &g_lines[i];
        char c = l->len ? l->p[0] : ' ';
        if (c == '\\') { mark_nonl(h); i++; continue; }
        if (c != ' ' && c != '-' && c != '+') break;
        add_line(h, c, l->len ? l->p + 1 : l->p, l->len ? l->len - 1 : 0);
        if (c != '+') oc++;
        if (c != '-') nc++;
        i++;
    }
    while (i < g_nlines && g_lines[i].len && g_lines[i].p[0] == '\\') { mark_nonl(h); i++; }
    return i;
}

/* *** a,b ****  old lines  --- c,d ----  new lines */
static long parse_context_hunk(FPatch *fp, long i) {
    Hunk *h = add_hunk(fp);
    long ofirst, ocnt, nfirst, ncnt, no = 0, nn = 0;
    PLine *O = NULL, *N = NULL;
    long oi = 0, ni = 0;
    const Ln *l;
    i++;                                                    /* the *************** line */
    if (i >= g_nlines || !is(&g_lines[i], "*** ")) return i;
    parse_pair(g_lines[i].p + 4, &ofirst, &ocnt, 1);
    if (ocnt < 0) ocnt = 0;
    i++;
    O = calloc((size_t)(g_nlines - i + 2), sizeof *O);
    N = calloc((size_t)(g_nlines - i + 2), sizeof *N);
    if (!O || !N) kb_die("out of memory");
    while (i < g_nlines && !(is(&g_lines[i], "--- ") && memchr(g_lines[i].p, ',', g_lines[i].len) + 0 >= 0 && g_lines[i].len > 4 &&
                              g_lines[i].p[g_lines[i].len - 1] == '-' && g_lines[i].p[g_lines[i].len - 2] == '-')) {
        l = &g_lines[i];
        if (l->len && l->p[0] == '\\') { if (no) O[no - 1].nonl = 1; i++; continue; }
        if (l->len < 2 && l->len) break;
        if (l->len >= 2 && l->p[1] != ' ') break;
        O[no].t = l->len ? (l->p[0] == ' ' ? ' ' : '-') : ' ';
        O[no].p = l->len ? l->p + 2 : l->p;
        O[no].len = l->len >= 2 ? l->len - 2 : 0;
        no++;
        i++;
    }
    if (i >= g_nlines) { free(O); free(N); return i; }
    parse_pair(g_lines[i].p + 4, &nfirst, &ncnt, 1);
    if (ncnt < 0) ncnt = 0;
    i++;
    while (i < g_nlines && nn < ncnt + 1000000) {
        l = &g_lines[i];
        if (l->len && l->p[0] == '\\') { if (nn) N[nn - 1].nonl = 1; i++; continue; }
        if (l->len < 2 && l->len) break;
        if (l->len >= 2 && l->p[1] != ' ') break;
        if (l->len >= 2 && l->p[0] != ' ' && l->p[0] != '+' && l->p[0] != '!') break;
        N[nn].t = l->len ? (l->p[0] == ' ' ? ' ' : '+') : ' ';
        N[nn].p = l->len ? l->p + 2 : l->p;
        N[nn].len = l->len >= 2 ? l->len - 2 : 0;
        nn++;
        i++;
        if (nn >= ncnt && (i >= g_nlines || g_lines[i].len == 0 || g_lines[i].p[0] != '\\')) break;
    }
    /* merge the two halves into one sequence: deletions, then additions, with the context in between */
    while (oi < no || ni < nn) {
        if (oi < no && O[oi].t == '-') {
            while (oi < no && O[oi].t == '-') add_line(h, '-', O[oi].p, O[oi].len), h->l[h->nl - 1].nonl = O[oi].nonl, oi++;
            while (ni < nn && N[ni].t == '+') add_line(h, '+', N[ni].p, N[ni].len), h->l[h->nl - 1].nonl = N[ni].nonl, ni++;
        } else if (ni < nn && N[ni].t == '+') {
            while (ni < nn && N[ni].t == '+') add_line(h, '+', N[ni].p, N[ni].len), h->l[h->nl - 1].nonl = N[ni].nonl, ni++;
        } else if (oi < no) {
            add_line(h, ' ', O[oi].p, O[oi].len);
            h->l[h->nl - 1].nonl = O[oi].nonl;
            oi++;
            if (ni < nn && N[ni].t == ' ') ni++;
        } else {
            add_line(h, ' ', N[ni].p, N[ni].len);
            h->l[h->nl - 1].nonl = N[ni].nonl;
            ni++;
        }
    }
    h->old_start = ofirst;
    h->old_count = 0;
    h->new_start = nfirst;
    h->new_count = 0;
    for (long k = 0; k < h->nl; k++) {
        if (h->l[k].t != '+') h->old_count++;
        if (h->l[k].t != '-') h->new_count++;
    }
    if (h->old_count == 0 && ocnt == 0) h->old_start = ofirst;          /* "*** 5 ****" with no lines: insert after line 5 */
    free(O);
    free(N);
    return i;
}

static int normal_header(const Ln *l, long *a, long *b, char *cmd, long *c, long *d) {
    const char *p = l->p, *e = l->p + l->len;
    char *q;
    if (!l->len || !isdigit((unsigned char)*p)) return 0;
    *a = strtol(p, &q, 10);
    *b = *a;
    if (*q == ',') *b = strtol(q + 1, &q, 10);
    if (*q != 'a' && *q != 'c' && *q != 'd') return 0;
    *cmd = *q++;
    if (q >= e || !isdigit((unsigned char)*q)) return 0;
    *c = strtol(q, &q, 10);
    *d = *c;
    if (*q == ',') *d = strtol(q + 1, &q, 10);
    return q == e;
}

static long parse_normal_hunk(FPatch *fp, long i) {
    long a, b, c, d;
    char cmd;
    Hunk *h;
    normal_header(&g_lines[i], &a, &b, &cmd, &c, &d);
    h = add_hunk(fp);
    h->old_start = a;
    h->old_count = cmd == 'a' ? 0 : b - a + 1;
    h->new_start = c;
    h->new_count = cmd == 'd' ? 0 : d - c + 1;
    i++;
    if (cmd != 'a') {
        for (long k = 0; k < h->old_count && i < g_nlines; k++, i++) {
            const Ln *l = &g_lines[i];
            if (l->len >= 2 && l->p[0] == '<') add_line(h, '-', l->p + 2, l->len - 2);
            else if (l->len && l->p[0] == '\\') { mark_nonl(h); k--; }
            else break;
        }
        while (i < g_nlines && g_lines[i].len && g_lines[i].p[0] == '\\') { mark_nonl(h); i++; }
    }
    if (cmd == 'c' && i < g_nlines && g_lines[i].len == 3 && !memcmp(g_lines[i].p, "---", 3)) i++;
    if (cmd != 'd') {
        for (long k = 0; k < h->new_count && i < g_nlines; k++, i++) {
            const Ln *l = &g_lines[i];
            if (l->len >= 2 && l->p[0] == '>') add_line(h, '+', l->p + 2, l->len - 2);
            else if (l->len && l->p[0] == '\\') { mark_nonl(h); k--; }
            else break;
        }
        while (i < g_nlines && g_lines[i].len && g_lines[i].p[0] == '\\') { mark_nonl(h); i++; }
    }
    return i;
}

/* Finds and reads the next file's diff, starting at line *PI. Returns 0 when there is no more. */
static int next_patch(long *pi, FPatch *fp) {
    char *idx = NULL;
    long i = *pi;
    long a, b, c, d;
    char cmd;
    memset(fp, 0, sizeof *fp);
    for (; i < g_nlines; i++) {
        const Ln *l = &g_lines[i];
        if (is(l, "Index: ")) {
            free(idx);
            idx = strndup(l->p + 7, l->len - 7);
            continue;
        }
        if (is(l, "--- ") && i + 1 < g_nlines && is(&g_lines[i + 1], "+++ ")) {
            fp->fmt = 1;
            fp->old_name = header_name(l);
            fp->new_name = header_name(&g_lines[i + 1]);
            fp->header_at = i + 1;
            i += 2;
            while (i < g_nlines && is(&g_lines[i], "@@ ")) i = parse_unified_hunk(fp, i);
            break;
        }
        if (is(l, "*** ") && i + 1 < g_nlines && is(&g_lines[i + 1], "--- ") && i + 2 < g_nlines && is(&g_lines[i + 2], "***************")) {
            fp->fmt = 2;
            fp->old_name = header_name(l);
            fp->new_name = header_name(&g_lines[i + 1]);
            fp->header_at = i + 1;
            i += 2;
            while (i < g_nlines && is(&g_lines[i], "***************")) i = parse_context_hunk(fp, i);
            break;
        }
        if (normal_header(l, &a, &b, &cmd, &c, &d)) {
            fp->fmt = 0;
            fp->header_at = i + 1;
            while (i < g_nlines && normal_header(&g_lines[i], &a, &b, &cmd, &c, &d)) i = parse_normal_hunk(fp, i);
            break;
        }
    }
    fp->idx_name = idx;
    *pi = i;
    return fp->fmt >= 0 && (fp->nh > 0 || (i < g_nlines));
}

/* ------------------------------------------------------------------ files */

typedef struct {
    char *buf;
    size_t len;
    unsigned *st;
    long n;
    int exists;
} File;

static int file_load(File *f, const char *name) {
    int fd;
    struct stat sb;
    size_t cap, n = 0;
    memset(f, 0, sizeof *f);
    fd = open(name, O_RDONLY);
    if (fd < 0) {
        f->buf = malloc(1);
        f->st = malloc(sizeof *f->st);
        f->st[0] = 0;
        return errno == ENOENT ? 0 : -1;
    }
    f->exists = 1;
    cap = fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode) ? (size_t)sb.st_size + 2 : 65536;
    f->buf = malloc(cap);
    if (!f->buf) kb_die("out of memory");
    for (;;) {
        ssize_t r;
        if (n + 1 >= cap) { cap *= 2; f->buf = realloc(f->buf, cap); if (!f->buf) kb_die("out of memory"); }
        r = read(fd, f->buf + n, cap - n - 1);
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    f->len = n;
    {
        long lines = 0;
        const char *p = f->buf, *e = f->buf + n;
        while (p < e) {
            const char *q = memchr(p, '\n', (size_t)(e - p));
            lines++;
            p = q ? q + 1 : e;
        }
        f->n = lines;
        f->st = malloc((size_t)(lines + 1) * sizeof *f->st);
        if (!f->st) kb_die("out of memory");
        p = f->buf;
        for (long i = 0; i < lines; i++) {
            const char *q = memchr(p, '\n', (size_t)(e - p));
            f->st[i] = (unsigned)(p - f->buf);
            p = q ? q + 1 : e;
        }
        f->st[lines] = (unsigned)n;
    }
    return 0;
}

static void file_free(File *f) { free(f->buf); free(f->st); memset(f, 0, sizeof *f); }

static int line_matches(const File *f, long i, const PLine *pl) {
    size_t total = f->st[i + 1] - f->st[i], len = total;
    const char *p = f->buf + f->st[i];
    int has_nl = total && p[total - 1] == '\n';
    if (has_nl) len--;
    return len == pl->len && (!has_nl) == (pl->nonl != 0) && !memcmp(p, pl->p, len);
}

/* ------------------------------------------------------------------ growable output */

typedef struct { char *b; size_t n, cap; } Buf;

static void buf_add(Buf *b, const char *p, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2 + 4096;
        b->b = realloc(b->b, b->cap);
        if (!b->b) kb_die("out of memory");
    }
    memcpy(b->b + b->n, p, n);
    b->n += n;
}

static void buf_str(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }

static void buf_num(Buf *b, long v) {
    char t[24];
    int n = snprintf(t, sizeof t, "%ld", v);
    buf_add(b, t, (size_t)n);
}

static int write_file(const char *name, const char *p, size_t n, mode_t mode) {
    int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, mode);
    size_t off = 0;
    if (fd < 0) return -1;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w <= 0) { close(fd); return -1; }
        off += (size_t)w;
    }
    close(fd);
    return 0;
}

static void make_parent_dirs(const char *path) {
    char *tmp = strdup(path);
    for (char *s = tmp + 1; *s; s++) {
        if (*s == '/') { *s = 0; mkdir(tmp, 0755); *s = '/'; }
    }
    free(tmp);
}

/* ------------------------------------------------------------------ choosing the file */

static const char *strip_name(const char *name, int n) {
    while (n-- > 0 && *name) {
        while (*name && *name != '/') name++;
        while (*name == '/') name++;
    }
    return name;
}

static int exists_file(const char *n) {
    struct stat st;
    return n && *n && stat(n, &st) == 0;
}

static void print_fail_line(const FPatch *fp, long first_line) {
    printf("can't find file to patch at input line %ld\n", first_line);
    printf("Perhaps you used the wrong -p or --strip option?\nThe text leading up to this was:\n--------------------------\n");
    for (long k = (fp->header_at >= 2 ? fp->header_at - 2 : 0); k < fp->header_at + (fp->fmt ? 0 : 0) && k < g_nlines; k++) {
        printf("|%.*s\n", (int)g_lines[k].len, g_lines[k].p);
    }
    printf("--------------------------\n");
}

static void put_hunk(Buf *b, const Hunk *h) {
    buf_str(b, "@@ -");
    buf_num(b, h->old_start);
    if (h->old_count != 1) { buf_str(b, ","); buf_num(b, h->old_count); }
    buf_str(b, " +");
    buf_num(b, h->new_start);
    if (h->new_count != 1) { buf_str(b, ","); buf_num(b, h->new_count); }
    buf_str(b, " @@\n");
    for (long k = 0; k < h->nl; k++) {
        buf_add(b, &h->l[k].t, 1);
        buf_add(b, h->l[k].p, h->l[k].len);
        buf_str(b, "\n");
        if (h->l[k].nonl) buf_str(b, "\\ No newline at end of file\n");
    }
}

/* Applies one file's diff. Returns 0 if all went well, 1 if some hunks failed, 2 for trouble. */
static int apply_one(FPatch *fp, const char *forced) {
    const char *names[3], *name = NULL;
    char *on = NULL, *nn = NULL;
    int creating = 0, deleting = 0, failed = 0, rc = 0;
    File f;
    Buf out = { 0 }, rej = { 0 };
    long cur = 0, delta = 0, last_off = 0, min_pos = 0;
    const char *old_n = fp->old_name ? fp->old_name : "", *new_n = fp->new_name ? fp->new_name : "";
    int rev = o_reverse;

    if (fp->fmt == 0 && !forced) {
        fprintf(stderr, "patch: **** no file name given for a normal diff\n");
        return 2;
    }
    if (rev) { const char *t = old_n; old_n = new_n; new_n = t; }
    creating = !strcmp(old_n, "/dev/null");
    deleting = !strcmp(new_n, "/dev/null");
    if (!creating && !deleting && fp->nh > 0) {                 /* the dates 1970-01-01 in the headers also mark creation: not used */
        const Hunk *h0 = &fp->h[0];
        long oc = rev ? h0->new_count : h0->old_count, os = rev ? h0->new_start : h0->old_start;
        if (oc == 0 && os == 0 && fp->nh == 1 && fp->fmt == 1 && !forced && !exists_file(strip_name(new_n, o_strip))) creating = 1;
    }
    if (forced) name = forced;
    else {
        on = strdup(strip_name(old_n, o_strip));
        nn = strdup(strip_name(new_n, o_strip));
        names[0] = deleting ? NULL : nn;
        names[1] = creating ? NULL : on;
        names[2] = fp->idx_name ? strip_name(fp->idx_name, o_strip) : NULL;
        if (creating || deleting) {
            name = creating ? nn : on;
        } else {
            for (int k = 0; k < 3 && !name; k++) if (names[k] && *names[k] && exists_file(names[k])) name = names[k];
        }
        if ((!name || !*name) && !creating) {
            print_fail_line(fp, fp->header_at);
            printf("No file to patch.  Skipping patch.\n%ld out of %ld hunk%s ignored\n", fp->nh, fp->nh, fp->nh == 1 ? "" : "s");
            free(on);
            free(nn);
            return 1;
        }
    }
    if (file_load(&f, name) < 0) {
        fprintf(stderr, "patch: %s: %s\n", name, strerror(errno));
        free(on);
        free(nn);
        return 2;
    }
    if (creating && f.exists && f.len > 0) {
        printf("The next patch would create the file %s, which already exists!  Skipping patch.\n", name);
        printf("%ld out of %ld hunk%s ignored\n", fp->nh, fp->nh, fp->nh == 1 ? "" : "s");
        file_free(&f);
        free(on);
        free(nn);
        return 1;
    }
    if (!f.exists && !creating) {
        fprintf(stderr, "patch: %s: No such file or directory\n", name);
        file_free(&f);
        free(on);
        free(nn);
        return 2;
    }
    if (!o_quiet) printf("%s file %s\n", o_dry ? "checking" : "patching", name);

    for (long hi = 0; hi < fp->nh; hi++) {
        Hunk *h = &fp->h[hi];
        long nold = 0, ostart = rev ? h->new_start : h->old_start, ocount = rev ? h->new_count : h->old_count;
        long nominal = ocount == 0 ? ostart : ostart - 1, guess, cand = -1;
        char old_t = rev ? '+' : '-', new_t = rev ? '-' : '+';
        PLine *old = malloc(((size_t)h->nl + 1) * sizeof *old);
        if (!old) kb_die("out of memory");
        for (long k = 0; k < h->nl; k++) if (h->l[k].t == ' ' || h->l[k].t == old_t) old[nold++] = h->l[k];
        if (nominal < 0) nominal = 0;
        guess = nominal + last_off;
        if (guess < min_pos) guess = min_pos;
        if (guess > f.n) guess = f.n;
        for (long dist = 0; dist <= f.n + 1; dist++) {
            for (int side = 0; side < 2; side++) {
                long pos = side == 0 ? guess + dist : guess - dist;
                int ok = 1;
                if (side == 1 && dist == 0) continue;
                if (pos < min_pos || pos + nold > f.n) continue;
                for (long k = 0; k < nold; k++) if (!line_matches(&f, pos + k, &old[k])) { ok = 0; break; }
                if (ok) { cand = pos; goto found; }
            }
            if (guess + dist > f.n && guess - dist < min_pos) break;
        }
    found:
        if (cand < 0) {
            long shown = (rev ? h->new_start : h->old_start) + delta;
            failed++;
            if (!o_quiet || 1) printf("Hunk #%ld FAILED at %ld.\n", hi + 1, shown);
            put_hunk(&rej, h);
        } else {
            long off = cand - nominal;
            for (; cur < cand; cur++) buf_add(&out, f.buf + f.st[cur], f.st[cur + 1] - f.st[cur]);
            {
                long ofs = cand;
                for (long k = 0; k < h->nl; k++) {
                    const PLine *pl = &h->l[k];
                    if (pl->t == ' ') {
                        buf_add(&out, f.buf + f.st[ofs], f.st[ofs + 1] - f.st[ofs]);
                        ofs++;
                    } else if (pl->t == old_t) {
                        ofs++;
                    } else if (pl->t == new_t) {
                        buf_add(&out, pl->p, pl->len);
                        if (!pl->nonl) buf_str(&out, "\n");
                    }
                }
                cur = ofs;
            }
            if (off != 0 && !o_quiet) {
                long a = off < 0 ? -off : off;
                printf("Hunk #%ld succeeded at %ld (offset %ld line%s).\n", hi + 1, cand + 1 + delta, off, a == 1 ? "" : "s");
            }
            last_off = off;
            min_pos = cur;
            {
                long newn = 0;
                for (long k = 0; k < h->nl; k++) if (h->l[k].t == ' ' || h->l[k].t == new_t) newn++;
                delta += newn - nold;
            }
        }
        free(old);
    }
    for (; cur < f.n; cur++) buf_add(&out, f.buf + f.st[cur], f.st[cur + 1] - f.st[cur]);

    if (failed) {
        char *rejname;
        const char *rn = o_rejfile ? o_rejfile : NULL;
        if (rn) rejname = strdup(rn);
        else { rejname = malloc(strlen(name) + 5); sprintf(rejname, "%s.rej", name); }
        if (!o_quiet) printf("%ld out of %ld hunk%s FAILED -- saving rejects to file %s\n", (long)failed, fp->nh, fp->nh == 1 ? "" : "s", rejname);
        if (!o_dry) {
            Buf all = { 0 };
            buf_str(&all, "--- ");
            buf_str(&all, fp->old_name ? fp->old_name : name);
            buf_str(&all, "\n+++ ");
            buf_str(&all, fp->new_name ? fp->new_name : name);
            buf_str(&all, "\n");
            buf_add(&all, rej.b ? rej.b : "", rej.n);
            if (write_file(rejname, all.b, all.n, 0644) < 0) fprintf(stderr, "patch: %s: %s\n", rejname, strerror(errno));
            free(all.b);
        }
        free(rejname);
        rc = 1;
    }
    if (!o_dry) {
        mode_t mode = 0644;
        struct stat sb;
        int remove_it = deleting && out.n == 0;
        if (f.exists && stat(name, &sb) == 0) mode = sb.st_mode & 07777;
        if (o_remove_empty && out.n == 0 && fp->nh > 0) remove_it = 1;
        if (o_output) {
            if (write_file(o_output, out.b ? out.b : "", out.n, mode) < 0) { fprintf(stderr, "patch: %s: %s\n", o_output, strerror(errno)); rc = 2; }
        } else if (remove_it && f.exists) {
            if (o_backup) {
                char *bk = malloc(strlen(name) + strlen(o_suffix) + 1);
                sprintf(bk, "%s%s", name, o_suffix);
                write_file(bk, f.buf, f.len, mode);
                free(bk);
            }
            if (unlink(name) < 0) { fprintf(stderr, "patch: %s: %s\n", name, strerror(errno)); rc = 2; }
        } else if (!remove_it) {
            if (o_backup && f.exists) {
                char *bk = malloc(strlen(name) + strlen(o_suffix) + 1);
                sprintf(bk, "%s%s", name, o_suffix);
                if (write_file(bk, f.buf, f.len, mode) < 0) fprintf(stderr, "patch: %s: %s\n", bk, strerror(errno));
                free(bk);
            }
            if (!f.exists) make_parent_dirs(name);
            if (write_file(name, out.b ? out.b : "", out.n, mode) < 0) { fprintf(stderr, "patch: %s: %s\n", name, strerror(errno)); rc = 2; }
        }
    }
    file_free(&f);
    free(out.b);
    free(rej.b);
    free(on);
    free(nn);
    return rc;
}

APPLET(patch) {
    static const struct option longopts[] = {
        { "strip", required_argument, 0, 'p' }, { "directory", required_argument, 0, 'd' }, { "input", required_argument, 0, 'i' },
        { "output", required_argument, 0, 'o' }, { "reverse", no_argument, 0, 'R' }, { "forward", no_argument, 0, 'N' },
        { "dry-run", no_argument, 0, 256 }, { "backup", no_argument, 0, 'b' }, { "suffix", required_argument, 0, 'z' },
        { "silent", no_argument, 0, 's' }, { "quiet", no_argument, 0, 's' }, { "force", no_argument, 0, 'f' },
        { "batch", no_argument, 0, 't' }, { "remove-empty-files", no_argument, 0, 'E' }, { "fuzz", required_argument, 0, 'F' },
        { "reject-file", required_argument, 0, 'r' }, { "no-backup-if-mismatch", no_argument, 0, 257 },
        { "unified", no_argument, 0, 'u' }, { "context", no_argument, 0, 'c' }, { "normal", no_argument, 0, 'n' },
        { "verbose", no_argument, 0, 258 }, { 0, 0, 0, 0 }
    };
    int c, status = 0, any = 0;
    size_t plen;
    long pos = 0;
    const char *operand = NULL;
    FPatch fp;
    optind = 0;
    while ((c = getopt_long(argc, argv, "p:d:i:o:RNbz:sftEF:r:ucn", longopts, NULL)) != -1) {
        switch (c) {
        case 'p': o_strip = atoi(optarg); break;
        case 'd': o_dir = optarg; break;
        case 'i': o_input = optarg; break;
        case 'o': o_output = optarg; break;
        case 'R': o_reverse = 1; break;
        case 'N': o_forward = 1; break;
        case 'b': o_backup = 1; break;
        case 'z': o_suffix = optarg; o_backup = 1; break;
        case 's': o_quiet = 1; break;
        case 'f': o_force = 1; break;
        case 't': o_batch = 1; break;
        case 'E': o_remove_empty = 1; break;
        case 'r': o_rejfile = optarg; break;
        case 256: o_dry = 1; break;
        case 'F': case 'u': case 'c': case 'n': case 257: case 258: break;
        default: return 2;
        }
    }
    if (optind < argc) operand = argv[optind++];
    if (optind < argc && !o_input) o_input = argv[optind++];
    if (optind < argc) { kb_error("extra operand '%s'", argv[optind]); return 2; }
    {
        int fd = 0;
        if (o_input && strcmp(o_input, "-")) {
            fd = open(o_input, O_RDONLY);
            if (fd < 0) { kb_error("%s: %s", o_input, strerror(errno)); return 2; }
        }
        g_pbuf = slurp_fd(fd, &plen);
        if (fd) close(fd);
    }
    if (o_dir && chdir(o_dir) < 0) { kb_error("%s: %s", o_dir, strerror(errno)); return 2; }
    split_patch(g_pbuf, plen);
    while (next_patch(&pos, &fp)) {
        int r;
        if (fp.nh == 0 && !fp.fmt) break;
        any = 1;
        r = apply_one(&fp, operand);
        if (r > status) status = r;
        if (pos >= g_nlines) break;
    }
    if (!any) {
        kb_error("**** Only garbage was found in the patch input.");
        return 2;
    }
    fflush(stdout);
    return status;
}
