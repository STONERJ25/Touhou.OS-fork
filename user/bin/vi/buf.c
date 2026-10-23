/* buf.c: the line array, undo, registers, and reading and writing files. */
#include "vi.h"

Line *L;
long nl, lcap;
static char *image;                                      /* bytes of the loaded file; lines borrow from it until edited */
int noeol, buf_empty;
int modified;

static const char empty_str[] = "";

/* ------------------------------------------------------------------ small helpers */

long imin(long a, long b) { return a < b ? a : b; }
long imax(long a, long b) { return a > b ? a : b; }

void fatal(const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    pt_term_restore(&term);
    ssize_t w = write(2, "vi: ", 4);
    w = write(2, tmp, strlen(tmp));
    w = write(2, "\n", 1);
    (void)w;
    _exit(1);
}

Line *lineat(long i) { return &L[i]; }
const char *ls(long i) { return L[i].s ? L[i].s : empty_str; }
unsigned ll(long i) { return L[i].len; }

static void lines_reserve(long extra) {
    if (nl + extra <= lcap) return;
    long nc = lcap ? lcap : 64;
    while (nc < nl + extra) nc *= 2;
    Line *np = realloc(L, (size_t)nc * sizeof *L);
    if (!np) pt_oom();
    L = np;
    lcap = nc;
}

static void line_free(Line *l) {
    if (l->cap) free(l->s);
    l->s = NULL;
    l->len = l->cap = 0;
}

static Line line_dup(const Line *l) {
    Line d;
    d.len = l->len;
    d.cap = l->len + 16;
    d.s = malloc(d.cap);
    if (!d.s) pt_oom();
    if (l->len) memcpy(d.s, l->s, l->len);
    return d;
}

static Line line_new(const char *s, size_t n) {
    Line d;
    d.len = (unsigned)n;
    d.cap = (unsigned)n + 16;
    d.s = malloc(d.cap);
    if (!d.s) pt_oom();
    if (n) memcpy(d.s, s, n);
    return d;
}

/* ------------------------------------------------------------------ marks follow inserted and deleted lines */

static void marks_adjust(long start, long nold, long nnew) {
    if (nold == nnew) return;
    Pos *all[] = { &mark_ctx, &mark_lt, &mark_gt, &mark_lb, &mark_rb, &mark_dot };
    long delta = nnew - nold;
    for (int i = 0; i < 26 + 6; i++) {
        Pos *m = i < 26 ? &marks[i] : all[i - 26];
        if (m->l < 0) continue;
        if (m->l >= start + nold) m->l += delta;
        else if (m->l >= start + nnew && m->l >= start) { if (nnew == 0 || m->l >= start + nnew) m->l = -1; }
    }
}

/* ------------------------------------------------------------------ undo */

typedef struct { long start, nold, nnew; Line *old; } URec;
typedef struct { URec *r; int n, cap; Pos before; Pos after; } UStep;

static UStep *usteps;
static int un, ucap, upos, usaved;                       /* steps, capacity, applied steps, applied steps at the last write */
static int uopen;                                        /* is the newest step still collecting changes? */
static Pos ubefore;
long undo_seq;

static void sync_modified(void) { modified = upos != usaved; }

static void step_free(UStep *s) {
    for (int i = 0; i < s->n; i++) {
        for (long j = 0; j < s->r[i].nold; j++) line_free(&s->r[i].old[j]);
        free(s->r[i].old);
    }
    free(s->r);
    s->r = NULL;
    s->n = s->cap = 0;
}

void undo_clear(void) {
    for (int i = 0; i < un; i++) step_free(&usteps[i]);
    un = upos = usaved = 0;
    uopen = 0;
    modified = 0;
}

void undo_begin(void) {
    uopen = 0;
    ubefore = cur;
}

void undo_cursor(Pos p) { ubefore = p; }

void mark_modified(void) { sync_modified(); }

static UStep *step_open(void) {
    if (!uopen) {
        while (un > upos) step_free(&usteps[--un]);       /* a new change cuts off the redo history */
        if (usaved > upos) usaved = -1;
        if (un == ucap) {
            ucap = ucap ? ucap * 2 : 64;
            usteps = realloc(usteps, (size_t)ucap * sizeof *usteps);
            if (!usteps) pt_oom();
        }
        memset(&usteps[un], 0, sizeof usteps[un]);
        usteps[un].before = ubefore;
        un++;
        upos = un;
        uopen = 1;
    }
    return &usteps[un - 1];
}

/* Called before lines [start, start+nold) are replaced by nnew lines. Saves the old lines unless an earlier record of this
 * step already covers the place. When `take` is set the old lines are moved into the record instead of copied. */
static void undo_note(long start, long nold, long nnew, int take) {
    UStep *st = step_open();
    sync_modified();
    if (st->n > 0) {
        URec *p = &st->r[st->n - 1];
        if (p->start <= start && start + nold <= p->start + p->nnew) {
            p->nnew += nnew - nold;
            if (take) for (long i = 0; i < nold; i++) line_free(&L[start + i]);
            return;
        }
    }
    if (st->n == st->cap) {
        st->cap = st->cap ? st->cap * 2 : 8;
        st->r = realloc(st->r, (size_t)st->cap * sizeof *st->r);
        if (!st->r) pt_oom();
    }
    URec *r = &st->r[st->n++];
    r->start = start;
    r->nold = nold;
    r->nnew = nnew;
    r->old = nold ? malloc((size_t)nold * sizeof(Line)) : NULL;
    if (nold && !r->old) pt_oom();
    for (long i = 0; i < nold; i++) r->old[i] = take ? L[start + i] : line_dup(&L[start + i]);
}

/* Swap the lines of a record with the buffer's (used for both undo and redo). */
static void rec_swap(URec *r) {
    Line *cut = r->nnew ? malloc((size_t)r->nnew * sizeof(Line)) : NULL;
    if (r->nnew && !cut) pt_oom();
    memcpy(cut, &L[r->start], (size_t)r->nnew * sizeof(Line));
    lines_reserve(r->nold - r->nnew);
    memmove(&L[r->start + r->nold], &L[r->start + r->nnew], (size_t)(nl - r->start - r->nnew) * sizeof(Line));
    if (r->nold) memcpy(&L[r->start], r->old, (size_t)r->nold * sizeof(Line));
    marks_adjust(r->start, r->nnew, r->nold);
    nl += r->nold - r->nnew;
    free(r->old);
    r->old = cut;
    long t = r->nold;
    r->nold = r->nnew;
    r->nnew = t;
}

int undo_do(int redo) {
    UStep *st;
    if (!redo) {
        if (upos == 0) return 0;
        st = &usteps[--upos];
        for (int i = st->n - 1; i >= 0; i--) rec_swap(&st->r[i]);
    } else {
        if (upos >= un) return 0;
        st = &usteps[upos++];
        for (int i = 0; i < st->n; i++) rec_swap(&st->r[i]);
    }
    uopen = 0;
    if (nl == 0) { lines_reserve(1); L[0].s = NULL; L[0].len = L[0].cap = 0; nl = 1; }
    long top = nl;
    for (int i = 0; i < st->n; i++) top = imin(top, st->r[i].start);
    if (top >= nl) top = nl - 1;
    if (top < 0) top = 0;
    Pos p;
    p.l = top;
    p.c = top == st->before.l ? st->before.c : first_nonblank(top);
    cur = p;
    fix_cursor();
    sync_modified();
    undo_seq++;
    return 1;
}

void undo_mark_saved(void) { usaved = upos; sync_modified(); }

/* ------------------------------------------------------------------ line primitives (all undo-recorded) */

static void check_empty_flag(void) { buf_empty = 0; }

void ln_set(long i, const char *s, size_t n) {
    undo_note(i, 1, 1, 0);
    Line *ln = &L[i];
    if (ln->cap && ln->cap > n) {
        if (n) memmove(ln->s, s, n);
        ln->len = (unsigned)n;
    } else {
        Line nw = line_new(s, n);
        if (ln->cap) free(ln->s);
        *ln = nw;
    }
    if (n) check_empty_flag();
}

void ln_insert(long at, const char *s, size_t n) {
    undo_note(at, 0, 1, 0);
    lines_reserve(1);
    memmove(&L[at + 1], &L[at], (size_t)(nl - at) * sizeof(Line));
    L[at] = line_new(s, n);
    nl++;
    marks_adjust(at, 0, 1);
    check_empty_flag();
}

void ln_delete(long at, long n) {
    if (n <= 0) return;
    undo_note(at, n, 0, 1);
    memmove(&L[at], &L[at + n], (size_t)(nl - at - n) * sizeof(Line));
    marks_adjust(at, n, 0);
    nl -= n;
    if (nl == 0) {
        undo_note(0, 0, 1, 0);
        lines_reserve(1);
        L[0].s = NULL;
        L[0].len = L[0].cap = 0;
        nl = 1;
        buf_empty = 1;
    }
}

static void ln_splice(long i, size_t at, size_t del, const char *ins, size_t n) {
    undo_note(i, 1, 1, 0);
    Line *ln = &L[i];
    size_t newlen = ln->len - del + n;
    if (!ln->cap || newlen + 1 > ln->cap) {
        size_t nc = newlen * 2 + 32;
        char *np = malloc(nc);
        if (!np) pt_oom();
        if (ln->len) memcpy(np, ln->s, ln->len);
        if (ln->cap) free(ln->s);
        ln->s = np;
        ln->cap = (unsigned)nc;
    }
    memmove(ln->s + at + n, ln->s + at + del, ln->len - at - del);
    if (n) memcpy(ln->s + at, ins, n);
    ln->len = (unsigned)newlen;
    if (n) check_empty_flag();
}

/* Inserts s (may contain newlines) at p. */
void text_insert(Pos p, const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        const char *nlp = memchr(s + i, '\n', n - i);
        size_t seg = nlp ? (size_t)(nlp - (s + i)) : n - i;
        if (seg) { ln_splice(p.l, (size_t)p.c, 0, s + i, seg); p.c += (int)seg; }
        i += seg;
        if (nlp) {                                       /* split the line */
            size_t tail = L[p.l].len - (size_t)p.c;
            char *copy = malloc(tail + 1);
            if (!copy) pt_oom();
            memcpy(copy, L[p.l].s ? L[p.l].s + p.c : "", tail);
            ln_splice(p.l, (size_t)p.c, tail, "", 0);
            ln_insert(p.l + 1, copy, tail);
            free(copy);
            p.l++;
            p.c = 0;
            i++;
        }
    }
}

/* Deletes [a, b). */
void text_delete(Pos a, Pos b) {
    if (b.l < a.l || (b.l == a.l && b.c <= a.c)) return;
    if (a.l == b.l) {
        ln_splice(a.l, (size_t)a.c, (size_t)(b.c - a.c), "", 0);
        return;
    }
    if (b.l >= nl) { b.l = nl - 1; b.c = (int)L[b.l].len; }
    size_t tail = L[b.l].len - (size_t)b.c;
    char *copy = malloc(tail + 1);
    if (!copy) pt_oom();
    memcpy(copy, L[b.l].s ? L[b.l].s + b.c : "", tail);
    ln_splice(a.l, (size_t)a.c, L[a.l].len - (size_t)a.c, copy, tail);
    free(copy);
    ln_delete(a.l + 1, b.l - a.l);
}

/* ------------------------------------------------------------------ ranges */

static void block_cols(long l, const Range *r, int *s, int *e) {
    if (vcol_end(l, 0) < 0) { *s = *e = 0; return; }
    int len = (int)L[l].len;
    if (line_vwidth(l) <= r->vc1) { *s = *e = len; return; }
    *s = byte_at_vcol(l, r->vc1);
    if (r->toeol || line_vwidth(l) <= r->vc2 + 1) *e = len;
    else *e = byte_at_vcol(l, r->vc2 + 1);
    if (*e < *s) *e = *s;
}

char *range_text(const Range *r, size_t *len) {
    PtBuf b = { 0 };
    if (r->type == 'l') {
        for (long i = r->a.l; i <= r->b.l && i < nl; i++) { pb_add(&b, ls(i), L[i].len); pb_addc(&b, '\n'); }
    } else if (r->type == 'b') {
        for (long i = r->a.l; i <= r->b.l && i < nl; i++) {
            int s, e;
            block_cols(i, r, &s, &e);
            pb_add(&b, ls(i) + s, (size_t)(e - s));
            if (i < r->b.l) pb_addc(&b, '\n');
        }
    } else {
        for (long i = r->a.l; i <= r->b.l && i < nl; i++) {
            int s = i == r->a.l ? r->a.c : 0;
            int e = i == r->b.l ? r->b.c : (int)L[i].len;
            if (e > (int)L[i].len) e = (int)L[i].len;
            if (e > s) pb_add(&b, ls(i) + s, (size_t)(e - s));
            if (i < r->b.l) pb_addc(&b, '\n');
        }
    }
    if (!b.p) { b.p = malloc(1); b.p[0] = 0; }
    *len = b.len;
    return b.p;
}

void range_delete(const Range *r) {
    if (r->type == 'l') {
        ln_delete(r->a.l, r->b.l - r->a.l + 1);
    } else if (r->type == 'b') {
        for (long i = r->a.l; i <= r->b.l && i < nl; i++) {
            int s, e;
            block_cols(i, r, &s, &e);
            if (e > s) ln_splice(i, (size_t)s, (size_t)(e - s), "", 0);
        }
    } else {
        Pos b = r->b;
        if (b.l >= nl) { b.l = nl - 1; b.c = (int)L[b.l].len; }
        text_delete(r->a, b);
    }
}

/* ------------------------------------------------------------------ registers */

static Reg regs[40];                                     /* a-z, 0-9, '-', '"' */
#define R_MINUS 36
#define R_UNNAMED 37
extern char last_insert[];
extern size_t last_insert_len;
static Reg scratch;

static int reg_index(int name) {
    if (name >= 'a' && name <= 'z') return name - 'a';
    if (name >= 'A' && name <= 'Z') return name - 'A';
    if (name >= '0' && name <= '9') return 26 + name - '0';
    if (name == '-') return R_MINUS;
    if (name == '"' || name == 0) return R_UNNAMED;
    return -1;
}

static void reg_store(Reg *r, const char *s, size_t n, int type) {
    free(r->s);
    r->s = malloc(n + 1);
    if (!r->s) pt_oom();
    memcpy(r->s, s, n);
    r->s[n] = 0;
    r->len = n;
    r->type = type;
}

void reg_free_all(void) {
    for (int i = 0; i < 40; i++) { free(regs[i].s); regs[i].s = NULL; regs[i].len = 0; }
}

void reg_set(int name, const char *s, size_t n, int type, int is_delete) {
    if (name == '_') return;
    int idx = reg_index(name);
    if (idx < 0) idx = R_UNNAMED;
    if (name >= 'A' && name <= 'Z') {                    /* append */
        Reg *r = &regs[idx];
        if (r->s) {
            PtBuf b = { 0 };
            pb_add(&b, r->s, r->len);
            if (r->type == 'l' && type != 'l') { pb_add(&b, s, n); pb_addc(&b, '\n'); type = 'l'; }
            else if (r->type != 'l' && type == 'l') { pb_addc(&b, '\n'); pb_add(&b, s, n); }
            else pb_add(&b, s, n);
            reg_store(r, b.p, b.len, type);
            pb_free(&b);
        } else reg_store(r, s, n, type);
        s = r->s; n = r->len; type = r->type;
    } else if (name == 0 || name == '"') {
        if (is_delete) {
            if (type == 'l' || memchr(s, '\n', n)) {
                free(regs[26 + 9].s);
                for (int i = 9; i > 1; i--) regs[26 + i] = regs[26 + i - 1];
                regs[26 + 1].s = NULL;
                reg_store(&regs[26 + 1], s, n, type);
            } else reg_store(&regs[R_MINUS], s, n, type);
        } else reg_store(&regs[26], s, n, type);
    } else {
        reg_store(&regs[idx], s, n, type);
    }
    reg_store(&regs[R_UNNAMED], s, n, type);             /* the unnamed register always holds the latest text */
}

Reg *reg_get(int name) {
    switch (name) {
    case '%': reg_store(&scratch, fname, strlen(fname), 'c'); return fname[0] ? &scratch : NULL;
    case '.': reg_store(&scratch, last_insert, last_insert_len, 'c'); return &scratch;
    case ':': reg_store(&scratch, last_ex, strlen(last_ex), 'c'); return last_ex[0] ? &scratch : NULL;
    case '/': reg_store(&scratch, last_search, strlen(last_search), 'c'); return last_search[0] ? &scratch : NULL;
    default: break;
    }
    int idx = reg_index(name);
    if (idx < 0 || !regs[idx].s) return NULL;
    return &regs[idx];
}

/* ------------------------------------------------------------------ files */

void buf_clear(void) {
    undo_clear();
    for (long i = 0; i < nl; i++) line_free(&L[i]);
    nl = 0;
    free(image);
    image = NULL;
    noeol = 0;
    buf_empty = 0;
    for (int i = 0; i < 26; i++) marks[i].l = -1;
    mark_ctx.l = mark_lt.l = mark_gt.l = mark_lb.l = mark_rb.l = mark_dot.l = -1;
}

void buf_new_empty(void) {
    buf_clear();
    lines_reserve(1);
    L[0].s = NULL;
    L[0].len = L[0].cap = 0;
    nl = 1;
    buf_empty = 1;
}

static char *slurp(int fd, size_t hint, size_t *len) {
    size_t cap = hint + 4096, n = 0;
    char *buf = malloc(cap);
    if (!buf) pt_oom();
    for (;;) {
        if (n + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) pt_oom();
            buf = nb;
        }
        ssize_t r = read(fd, buf + n, cap - n - 1);
        if (r < 0) { if (errno == EINTR) continue; free(buf); return NULL; }
        if (r == 0) break;
        n += (size_t)r;
    }
    buf[n] = 0;
    *len = n;
    return buf;
}

/* Loads path into a fresh buffer. Returns 0 on success, 1 if the file does not exist (the buffer is one empty line),
 * -1 on any other error (errno is set, the buffer is one empty line). */
int buf_load(const char *path, int *nlines, long *nbytes) {
    buf_new_empty();
    *nlines = 0;
    *nbytes = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { close(fd); errno = EISDIR; return -1; }
    size_t len = 0;
    char *img = slurp(fd, fstat(fd, &st) == 0 && st.st_size > 0 ? (size_t)st.st_size : 0, &len);
    int err = errno;
    close(fd);
    if (!img) { errno = err; return -1; }
    if (len == 0) { free(img); return 0; }
    long count = 0;
    for (const char *p = img; (p = memchr(p, '\n', (size_t)(img + len - p))) != NULL; p++) count++;
    int eol = img[len - 1] == '\n';
    long nlines_total = count + (eol ? 0 : 1);
    nl = 0;
    lines_reserve(nlines_total);
    image = img;
    const char *p = img, *end = img + len;
    while (p < end) {
        const char *q = memchr(p, '\n', (size_t)(end - p));
        size_t seg = q ? (size_t)(q - p) : (size_t)(end - p);
        L[nl].s = (char *)p;
        L[nl].len = (unsigned)seg;
        L[nl].cap = 0;
        nl++;
        p += seg + (q ? 1 : 0);
    }
    noeol = !eol;
    buf_empty = 0;
    *nlines = (int)nl;
    *nbytes = (long)len;
    return 0;
}

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Writes lines from..to (0-based, inclusive) to path. Returns 0 or -1 (errno set). */
int buf_write(const char *path, long from, long to, int append, long *nbytes) {
    PtBuf b = { 0 };
    int whole = from == 0 && to == nl - 1;
    if (whole && buf_empty && nl == 1 && L[0].len == 0) {
        /* an empty file stays empty */
    } else {
        for (long i = from; i <= to; i++) {
            pb_add(&b, ls(i), L[i].len);
            if (i < to || !(whole && noeol)) pb_addc(&b, '\n');
        }
    }
    *nbytes = (long)b.len;
    struct stat st;
    int exists = lstat(path, &st) == 0;
    int rc = -1;
    if (!append && exists && S_ISREG(st.st_mode)) {
        char tmp[MAXPATH + 40];
        snprintf(tmp, sizeof tmp, "%s.vi%d.tmp", path, (int)getpid());
        int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, (mode_t)(st.st_mode & 07777));
        if (fd >= 0) {
            int ok = write_all(fd, b.p ? b.p : "", b.len) == 0;
            if (close(fd) != 0) ok = 0;
            if (ok) { chmod(tmp, (mode_t)(st.st_mode & 07777)); if (rename(tmp, path) == 0) rc = 0; }
            if (rc != 0) unlink(tmp);
        }
    }
    if (rc != 0) {                                       /* a new file, a symlink, a device, or no room for a temp file */
        int fd = open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0666);
        if (fd < 0) { pb_free(&b); return -1; }
        int ok = write_all(fd, b.p ? b.p : "", b.len) == 0;
        int e = errno;
        if (close(fd) != 0) ok = 0;
        errno = e;
        rc = ok ? 0 : -1;
    }
    pb_free(&b);
    return rc;
}
