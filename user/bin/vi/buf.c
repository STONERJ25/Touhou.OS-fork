/* buf.c: the line array (a gap buffer), undo, registers, and reading and writing files. */
#include "vi.h"

/* The lines are kept in a gap buffer: line i is L[i] for i < gap_at and L[i + gap_len] after it, and gap_len == lcap - nl.
 * Editing near the gap (a run of deletes, an undo of many lines) costs nothing, which keeps :g/x/d on a 100,000-line file quick. */
Line *L;
long nl, lcap, gap_at, gap_len;
static char *image;                                      /* bytes of the loaded file; lines borrow from it until edited */
int noeol, buf_empty, ff_dos;
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

Line *lineat(long i) { return &LN(i); }
const char *ls(long i) { return LN(i).s ? LN(i).s : empty_str; }
unsigned ll(long i) { return LN(i).len; }

static void gap_move(long at) {
    if (at == gap_at) return;
    if (at < gap_at) memmove(&L[at + gap_len], &L[at], (size_t)(gap_at - at) * sizeof(Line));
    else memmove(&L[gap_at], &L[gap_at + gap_len], (size_t)(at - gap_at) * sizeof(Line));
    gap_at = at;
}

static void lines_reserve(long extra) {
    if (gap_len >= extra) return;
    long nc = lcap ? lcap : 64;
    while (nc - nl < extra) nc *= 2;
    Line *np = realloc(L, (size_t)nc * sizeof *L);
    if (!np) pt_oom();
    long tail = nl - gap_at;                             /* the lines after the gap move to the end of the new storage */
    if (tail > 0) memmove(&np[nc - tail], &np[gap_at + gap_len], (size_t)tail * sizeof(Line));
    L = np;
    lcap = nc;
    gap_len = nc - nl;
}

/* Puts n line structures into the buffer at index at / takes n out (copying them to dst when it is not NULL). */
static void raw_insert(long at, const Line *src, long n) {
    lines_reserve(n);
    gap_move(at);
    if (n) memcpy(&L[at], src, (size_t)n * sizeof(Line));
    gap_at += n;
    gap_len -= n;
    nl += n;
}

static void raw_remove(long at, long n, Line *dst) {
    gap_move(at);
    if (dst && n) memcpy(dst, &L[gap_at + gap_len], (size_t)n * sizeof(Line));
    gap_len += n;
    nl -= n;
}

static void line_free(Line *l) {
    if (l->cap) free(l->s);
    l->s = NULL;
    l->len = l->cap = 0;
    l->gid = 0;
}

static Line line_dup(const Line *l) {
    Line d;
    d.len = l->len;
    d.cap = l->len + 16;
    d.gid = 0;
    d.s = malloc(d.cap);
    if (!d.s) pt_oom();
    if (l->len) memcpy(d.s, l->s, l->len);
    return d;
}

static Line line_new(const char *s, size_t n) {
    Line d;
    d.len = (unsigned)n;
    d.cap = (unsigned)n + 16;
    d.gid = 0;
    d.s = malloc(d.cap);
    if (!d.s) pt_oom();
    if (n) memcpy(d.s, s, n);
    return d;
}

/* ------------------------------------------------------------------ the line saved for "U" */

static long uline_no = -1;                               /* the line whose first change was saved, or -1 */
static char *uline_s;
static unsigned uline_len, uline_cap;
static int uline_col;

static void uline_store(const char *s, unsigned n) {
    if (n + 1 > uline_cap) {
        free(uline_s);
        uline_cap = n + 64;
        uline_s = malloc(uline_cap);
        if (!uline_s) pt_oom();
    }
    if (n) memcpy(uline_s, s, n);
    uline_len = n;
}

/* Called before line i is changed: the first change of a line saves its text. */
static void uline_note(long i) {
    if (i == uline_no) return;
    uline_store(ls(i), LN(i).len);
    uline_no = i;
    uline_col = cur.l == i ? cur.c : 0;
}

/* ------------------------------------------------------------------ marks follow inserted and deleted lines */

/* Same rule for the global (:g) hook below. */
void (*line_deleted_hook)(const Line *l);

Pos jumps[MAXJUMPS];                                     /* the jump list (oldest first); jumpidx == njumps right after a jump */
int njumps, jumpidx;

static void adjust_pos(Pos *m, long start, long nold, long nnew) {
    if (m->l < 0) return;
    if (m->l >= start + nold) m->l += nnew - nold;
    else if (m->l >= start + nnew) m->l = -1;            /* the line was deleted or replaced by fewer lines */
}

static void marks_adjust(long start, long nold, long nnew) {
    if (nold == nnew) return;
    Pos *all[] = { &mark_ctx, &mark_lt, &mark_gt, &mark_lb, &mark_rb, &mark_dot, &mark_ins };
    for (int i = 0; i < 26 + 7; i++) adjust_pos(i < 26 ? &marks[i] : all[i - 26], start, nold, nnew);
    for (int i = 0; i < njumps; i++) adjust_pos(&jumps[i], start, nold, nnew);
    long delta = nnew - nold;
    if (uline_no >= 0) {
        if (uline_no >= start + nold) uline_no += delta;
        else if (uline_no >= start + nnew) uline_no = -1;
    }
}

/* ------------------------------------------------------------------ undo */

typedef struct { long start, nold, nnew; Line *old; } URec;
typedef struct { URec *r; int n, cap; Pos before; long seq; time_t when; } UStep;

static UStep *usteps;
static int un, ucap, upos, usaved;                       /* steps, capacity, applied steps, applied steps at the last write */
static int uopen;                                        /* is the newest step still collecting changes? */
static Pos ubefore;
static long useq;                                        /* the number of the newest step: they are numbered as they are made */
static long um_old, um_new, um_seq;                      /* for the message after u / Ctrl-R: lines taken out, lines put back, step */
static time_t um_when;
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
    uline_no = -1;
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
        usteps[un].seq = ++useq;
        usteps[un].when = time(NULL);
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
            if (take) for (long i = 0; i < nold; i++) { if (line_deleted_hook) line_deleted_hook(&LN(start + i)); line_free(&LN(start + i)); }
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
    for (long i = 0; i < nold; i++) {
        if (take && line_deleted_hook) line_deleted_hook(&LN(start + i));
        r->old[i] = take ? LN(start + i) : line_dup(&LN(start + i));
        r->old[i].gid = 0;
    }
}

/* Swap the lines of a record with the buffer's (used for both undo and redo). */
static void rec_swap(URec *r) {
    Line *cut = r->nnew ? malloc((size_t)r->nnew * sizeof(Line)) : NULL;
    if (r->nnew && !cut) pt_oom();
    raw_remove(r->start, r->nnew, cut);
    raw_insert(r->start, r->old, r->nold);
    marks_adjust(r->start, r->nnew, r->nold);
    free(r->old);
    r->old = cut;
    long t = r->nold;
    r->nold = r->nnew;
    r->nnew = t;
}

/* Where the cursor goes after an undo or redo (Vim's rule): back to where it was before the change when that is at the
 * changed lines (or one line above them, for "o"), else to the first changed line. */
static void undo_place_cursor(const UStep *st, long top, long size) {
    Pos sv = st->before;
    Pos c;
    long sv1 = sv.l + 1;
    if (sv1 >= top && sv1 <= top + size + 1) c = sv;
    else { c.l = top; c.c = 0; }
    if (sv.l + 1 == c.l && c.l > 0) c.l--;
    if (c.l >= nl) c.l = nl - 1;
    if (c.l < 0) c.l = 0;
    c.c = sv.l == c.l ? sv.c : first_nonblank(c.l);
    cur = c;
}

/* Starts the line counts for the message of one u / Ctrl-R command (which may undo several steps). */
void undo_counts_reset(void) {
    um_old = buf_empty ? -1 : 0;
    um_new = 0;
}

/* "3 fewer lines; before #5  4 seconds ago": Vim's message after u and Ctrl-R; the counts come from the records just swapped. */
void undo_message(int redo) {
    long oc = um_old, nc = um_new;
    if (buf_empty) nc--;                                 /* the placeholder line of an empty buffer is not text */
    oc -= nc;
    const char *what;
    if (oc == -1) what = "more line";
    else if (oc < 0) what = "more lines";
    else if (oc == 1) what = "line less";
    else if (oc > 1) what = "fewer lines";
    else { oc = nc; what = nc == 1 ? "change" : "changes"; }
    char when[48];
    long ago = (long)(time(NULL) - um_when);
    if (ago < 0) ago = 0;
    if (ago >= 100) {
        struct tm tmv;
        time_t t = um_when;
        localtime_r(&t, &tmv);
        strftime(when, sizeof when, ago < 12 * 3600 ? "%H:%M:%S" : "%Y/%m/%d %H:%M:%S", &tmv);
    } else snprintf(when, sizeof when, "%ld second%s ago", ago, ago == 1 ? "" : "s");
    set_msg("%ld %s; %s #%ld  %s", oc < 0 ? -oc : oc, what, redo ? "after" : "before", um_seq, when);
}

int undo_do(int redo) {
    UStep *st;
    long top = LONG_MAX, size = 0;
    if (!redo) {
        if (upos == 0) return 0;
        st = &usteps[--upos];
        for (int i = st->n - 1; i >= 0; i--) {
            um_old += st->r[i].nnew;
            um_new += st->r[i].nold;
            rec_swap(&st->r[i]);
            if (st->r[i].start < top) { top = st->r[i].start; size = st->r[i].nnew; }
        }
    } else {
        if (upos >= un) return 0;
        st = &usteps[upos++];
        for (int i = 0; i < st->n; i++) {
            um_old += st->r[i].nnew;
            um_new += st->r[i].nold;
            rec_swap(&st->r[i]);
            if (st->r[i].start < top) { top = st->r[i].start; size = st->r[i].nnew; }
        }
    }
    um_seq = st->seq;
    um_when = st->when;
    uopen = 0;
    if (nl == 0) { Line e = { 0 }; raw_insert(0, &e, 1); }
    if (top == LONG_MAX) top = 0;
    undo_place_cursor(st, top, size);
    fix_cursor();
    sync_modified();
    undo_seq++;
    uline_no = -1;
    return 1;
}

void undo_mark_saved(void) { usaved = upos; sync_modified(); }

void undo_force_modified(void) { usaved = -1; sync_modified(); }       /* e.g. after :set ff=dos */

/* "U": swaps the saved line with the text now in the line (so U again goes back). */
void undo_line_restore(void) {
    if (uline_no < 0 || uline_no >= nl) { beep(); return; }
    long l = uline_no;
    char *now = malloc(LN(l).len + 1);
    if (!now) pt_oom();
    unsigned nowlen = LN(l).len;
    if (nowlen) memcpy(now, ls(l), nowlen);
    char *sv = uline_s;
    unsigned svlen = uline_len;
    uline_s = NULL;
    uline_cap = 0;
    int col = uline_col;
    uline_col = cur.l == l ? cur.c : 0;
    ln_set(l, sv ? sv : "", svlen);                      /* uline_no is l, so the saved text is not overwritten */
    free(sv);
    uline_store(now, nowlen);
    free(now);
    uline_no = l;
    cur.l = l;
    cur.c = col;
}

/* ------------------------------------------------------------------ line primitives (all undo-recorded) */

static void check_empty_flag(void) { buf_empty = 0; }

void ln_set(long i, const char *s, size_t n) {
    uline_note(i);
    undo_note(i, 1, 1, 0);
    Line *ln = &LN(i);
    if (ln->cap && ln->cap > n) {
        if (n) memmove(ln->s, s, n);
        ln->len = (unsigned)n;
    } else {
        Line nw = line_new(s, n);
        nw.gid = ln->gid;
        if (ln->cap) free(ln->s);
        *ln = nw;
    }
    if (n) check_empty_flag();
}

void ln_insert(long at, const char *s, size_t n) {
    undo_note(at, 0, 1, 0);
    Line nw = line_new(s, n);
    raw_insert(at, &nw, 1);
    marks_adjust(at, 0, 1);
    check_empty_flag();
}

void ln_delete(long at, long n) {
    if (n <= 0) return;
    undo_note(at, n, 0, 1);
    raw_remove(at, n, NULL);
    marks_adjust(at, n, 0);
    if (nl == 0) {
        undo_note(0, 0, 1, 0);
        Line e = { 0 };
        raw_insert(0, &e, 1);
        buf_empty = 1;
    }
}

static void ln_splice(long i, size_t at, size_t del, const char *ins, size_t n) {
    uline_note(i);
    undo_note(i, 1, 1, 0);
    Line *ln = &LN(i);
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
            size_t tail = LN(p.l).len - (size_t)p.c;
            char *copy = malloc(tail + 1);
            if (!copy) pt_oom();
            memcpy(copy, LN(p.l).s ? LN(p.l).s + p.c : "", tail);
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
    if (b.l >= nl) { b.l = nl - 1; b.c = (int)LN(b.l).len; }
    size_t tail = LN(b.l).len - (size_t)b.c;
    char *copy = malloc(tail + 1);
    if (!copy) pt_oom();
    memcpy(copy, LN(b.l).s ? LN(b.l).s + b.c : "", tail);
    ln_splice(a.l, (size_t)a.c, LN(a.l).len - (size_t)a.c, copy, tail);
    free(copy);
    ln_delete(a.l + 1, b.l - a.l);
}

/* ------------------------------------------------------------------ ranges */

static void block_cols(long l, const Range *r, int *s, int *e) {
    int len = (int)LN(l).len;
    if (line_vwidth(l) <= r->vc1) { *s = *e = len; return; }
    *s = byte_at_vcol(l, r->vc1);
    if (r->toeol || line_vwidth(l) <= r->vc2 + 1) *e = len;
    else *e = byte_at_vcol(l, r->vc2 + 1);
    if (*e < *s) *e = *s;
}

char *range_text(const Range *r, size_t *len) {
    PtBuf b = { 0 };
    if (r->type == 'l') {
        for (long i = r->a.l; i <= r->b.l && i < nl; i++) { pb_add(&b, ls(i), LN(i).len); pb_addc(&b, '\n'); }
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
            int e = i == r->b.l ? r->b.c : (int)LN(i).len;
            if (e > (int)LN(i).len) e = (int)LN(i).len;
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
        if (b.l >= nl) { b.l = nl - 1; b.c = (int)LN(b.l).len; }
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
    char *copy = malloc(n + 1);                          /* s may point into r->s (the unnamed register is stored from a named one) */
    if (!copy) pt_oom();
    memcpy(copy, s, n);
    copy[n] = 0;
    free(r->s);
    r->s = copy;
    r->len = n;
    r->type = type;
}

void reg_free_all(void) {
    for (int i = 0; i < 40; i++) { free(regs[i].s); regs[i].s = NULL; regs[i].len = 0; }
}

static void shift_numbered(const char *s, size_t n, int type) {
    free(regs[26 + 9].s);
    for (int i = 9; i > 1; i--) regs[26 + i] = regs[26 + i - 1];
    regs[26 + 1].s = NULL;
    reg_store(&regs[26 + 1], s, n, type);
}

/* Stores text in register `name`. is_delete: 0 for a yank, 1 for a delete, 2 for a delete with a motion that always uses "1
 * (% ( ) ` / ? n N { }). A delete of a line or of several lines is also put in "1 (the older ones move up), a smaller one in
 * "-; the unnamed register always holds the latest text. */
void reg_set(int name, const char *s, size_t n, int type, int is_delete) {
    if (name == '_') return;
    int idx = reg_index(name);
    if (idx < 0) idx = R_UNNAMED;
    int multiline = type == 'l' || memchr(s, '\n', n) != NULL;
    int shift = is_delete == 2 || (is_delete && multiline);
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
        if (shift) shift_numbered(s, n, type);           /* a delete into a named register also goes into "1 */
    } else if (name == 0 || name == '"') {
        if (is_delete) {
            if (shift) shift_numbered(s, n, type);
            if (!multiline) reg_store(&regs[R_MINUS], s, n, type);
        } else reg_store(&regs[26], s, n, type);
    } else {
        reg_store(&regs[idx], s, n, type);
        if (shift) shift_numbered(s, n, type);
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
    for (long i = 0; i < nl; i++) line_free(&LN(i));
    nl = 0;
    gap_at = 0;
    gap_len = lcap;
    free(image);
    image = NULL;
    noeol = 0;
    ff_dos = 0;
    buf_empty = 0;
    for (int i = 0; i < 26; i++) marks[i].l = -1;
    mark_ctx.l = mark_lt.l = mark_gt.l = mark_lb.l = mark_rb.l = mark_dot.l = mark_ins.l = -1;
    njumps = jumpidx = 0;
}

void buf_new_empty(void) {
    buf_clear();
    Line e = { 0 };
    raw_insert(0, &e, 1);
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
 * -1 on any other error (errno is set, the buffer is one empty line). A file whose lines all end in CR LF is edited without
 * the CRs and written back with them (ff_dos). */
static int load_fd(int fd, int *nlines, long *nbytes) {
    struct stat st;
    size_t hint = 0;
    if (fstat(fd, &st) == 0) {
        if (S_ISDIR(st.st_mode)) { errno = EISDIR; return -1; }
        if (S_ISREG(st.st_mode) && st.st_size > 0) hint = (size_t)st.st_size;
    }
    size_t len = 0;
    char *img = slurp(fd, hint, &len);
    if (!img) return -1;
    if (len == 0) { free(img); return 0; }
    long count = 0, crlf = 0;
    for (const char *p = img; (p = memchr(p, '\n', (size_t)(img + len - p))) != NULL; p++) {
        count++;
        if (p > img && p[-1] == '\r') crlf++;
    }
    int eol = img[len - 1] == '\n';
    int dos = count > 0 && crlf == count;
    long nlines_total = count + (eol ? 0 : 1);
    nl = 0;
    gap_at = 0;
    gap_len = lcap;
    lines_reserve(nlines_total);
    image = img;
    const char *p = img, *end = img + len;
    long k = 0;
    while (p < end) {
        const char *q = memchr(p, '\n', (size_t)(end - p));
        size_t seg = q ? (size_t)(q - p) : (size_t)(end - p);
        size_t shown = seg;
        if (dos && q) shown--;                           /* the CR before the LF */
        L[k].s = (char *)p;
        L[k].len = (unsigned)shown;
        L[k].cap = 0;
        L[k].gid = 0;
        k++;
        p += seg + (q ? 1 : 0);
    }
    nl = k;
    gap_at = nl;
    gap_len = lcap - nl;
    noeol = !eol;
    ff_dos = dos;
    buf_empty = 0;
    *nlines = (int)nl;
    *nbytes = (long)len;
    return 0;
}

int buf_load(const char *path, int *nlines, long *nbytes) {
    buf_new_empty();
    *nlines = 0;
    *nbytes = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    int rc = load_fd(fd, nlines, nbytes);
    int err = errno;
    close(fd);
    errno = err;
    return rc;
}

/* "vi -": the text comes from standard input (the keys then come from the terminal, see pt_term_open). */
int buf_load_stdin(int *nlines, long *nbytes) {
    buf_new_empty();
    *nlines = 0;
    *nbytes = 0;
    return load_fd(0, nlines, nbytes);
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

/* Writes lines from..to (0-based, inclusive) to path. Returns 0 or -1 (errno set). The last line gets its newline unless
 * 'nofixeol' is set and the file had none. */
int buf_write(const char *path, long from, long to, int append, long *nbytes) {
    PtBuf b = { 0 };
    int whole = from == 0 && to == nl - 1;
    if (whole && buf_empty && nl == 1 && LN(0).len == 0) {
        /* an empty file stays empty */
    } else {
        for (long i = from; i <= to; i++) {
            pb_add(&b, ls(i), LN(i).len);
            if (i < to || !(whole && noeol && !o.fixeol)) { if (ff_dos) pb_addc(&b, '\r'); pb_addc(&b, '\n'); }
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
