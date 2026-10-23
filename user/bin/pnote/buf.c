/* The text of the notebook: a gap array of lines, the two raw edit primitives, undo/redo, loading and saving. */
#include "pnote.h"

Ed E;

#define MAX_LINE_BYTES ((1u << 27) - 2)
#define UNDO_LIMIT ((size_t)24 << 20)

/* ------------------------------------------------------------------ gap array of lines */

static void gap_grow(long need) {
    if (E.gaplen >= need) return;
    long ncap = E.cap ? E.cap * 2 : 1024;
    while (ncap - E.n < need) ncap *= 2;
    Line *na = malloc((size_t)ncap * sizeof(Line));
    if (!na) pt_oom();
    long back = E.cap - (E.gap + E.gaplen);
    if (E.gap) memcpy(na, E.a, (size_t)E.gap * sizeof(Line));
    if (back > 0) memcpy(na + ncap - back, E.a + E.gap + E.gaplen, (size_t)back * sizeof(Line));
    free(E.a);
    E.a = na;
    E.gaplen = ncap - E.n;
    E.cap = ncap;
}

static void gap_move(long pos) {
    if (pos == E.gap) return;
    if (pos < E.gap) memmove(E.a + pos + E.gaplen, E.a + pos, (size_t)(E.gap - pos) * sizeof(Line));
    else memmove(E.a + E.gap, E.a + E.gap + E.gaplen, (size_t)(pos - E.gap) * sizeof(Line));
    E.gap = pos;
}

/* Inserts an empty line at index pos and returns it. */
static Line *slot_insert(long pos) {
    gap_grow(1);
    gap_move(pos);
    Line *L = &E.a[E.gap];
    memset(L, 0, sizeof *L);
    E.gap++;
    E.gaplen--;
    E.n++;
    return L;
}

static void slot_remove(long pos, long count) {
    gap_move(pos);
    for (long i = 0; i < count; i++) {
        Line *L = &E.a[E.gap + E.gaplen + i];
        if (L->cap) free(L->s);
    }
    E.gaplen += count;
    E.n -= count;
}

/* Replaces `del` bytes at `at` with ins[0..ilen). Borrowed lines become owned here. */
static void line_splice(Line *L, size_t at, size_t del, const char *ins, size_t ilen) {
    size_t newlen = L->len - del + ilen;
    if (newlen > MAX_LINE_BYTES) pt_oom();
    size_t tail = L->len - at - del;
    if (L->cap && newlen <= L->cap) {
        if (tail && ilen != del) memmove(L->s + at + ilen, L->s + at + del, tail);
        if (ilen) memcpy(L->s + at, ins, ilen);
    } else {
        size_t ncap = newlen + newlen / 2 + 16;
        if (ncap > MAX_LINE_BYTES) ncap = MAX_LINE_BYTES;
        char *ns = malloc(ncap);
        if (!ns) pt_oom();
        if (at) memcpy(ns, L->s, at);
        if (ilen) memcpy(ns + at, ins, ilen);
        if (tail) memcpy(ns + at + ilen, L->s + at + del, tail);
        if (L->cap) free(L->s);
        L->s = ns;
        L->cap = (uint32_t)ncap;
    }
    L->len = (uint32_t)newlen;
}

static void line_set(Line *L, const char *s, size_t n, int cr) {
    L->len = 0;
    L->cap = 0;
    L->s = NULL;
    line_splice(L, 0, 0, s, n);
    L->cr = (unsigned)cr;
}

void buf_reset_empty(void) {
    for (long i = 0; i < E.n; i++) { Line *L = LN(i); if (L->cap) free(L->s); }
    E.gap = 0;
    E.gaplen = E.cap;
    E.n = 0;
    Line *L = slot_insert(0);
    L->s = (char *)"";
    L->len = 0;
    L->cap = 0;
    E.final_newline = 0;
    hl_invalidate(0);
}

/* ------------------------------------------------------------------ the two raw edit primitives */

/* Finds the end of the line piece starting at p: returns the length without the terminator and sets *cr. */
static const char *find_nl(const char *p, const char *end) {
    while (p < end && *p != '\n') p++;
    return p < end ? p : NULL;
}

/* Inserts text (n bytes, "\r\n" = a CR LF terminator) at line ln, byte col. Returns the end position. */
size_t buf_insert_raw(long ln, size_t col, const char *text, size_t n, long *eln, size_t *ecol) {
    hl_invalidate(ln);
    const char *p = text, *end = text + n;
    const char *nl = find_nl(p, end);
    if (!nl) {
        line_splice(LN(ln), col, 0, text, n);
        *eln = ln;
        *ecol = col + n;
        return n;
    }
    Line *L = LN(ln);
    size_t taillen = L->len - col;
    char *tail = malloc(taillen + 1);
    if (!tail) pt_oom();
    memcpy(tail, L->s + col, taillen);
    unsigned tail_cr = L->cr;
    size_t plen = (size_t)(nl - p);
    int cr = plen > 0 && p[plen - 1] == '\r';
    if (cr) plen--;
    line_splice(L, col, taillen, p, plen);
    L->cr = (unsigned)cr;
    long cur = ln;
    p = nl + 1;
    while ((nl = find_nl(p, end)) != NULL) {
        plen = (size_t)(nl - p);
        cr = plen > 0 && p[plen - 1] == '\r';
        if (cr) plen--;
        Line *M = slot_insert(cur + 1);
        line_set(M, p, plen, cr);
        cur++;
        p = nl + 1;
    }
    Line *Z = slot_insert(cur + 1);
    size_t lastlen = (size_t)(end - p);
    line_set(Z, p, lastlen, 0);
    if (taillen) line_splice(Z, lastlen, 0, tail, taillen);
    Z->cr = tail_cr;
    free(tail);
    *eln = cur + 1;
    *ecol = lastlen;
    return n;
}

/* Removes the text between (l1,c1) and (l2,c2) and returns it (malloc'd, "\r\n" for CR LF terminators). */
char *buf_delete_raw(long l1, size_t c1, long l2, size_t c2, size_t *outlen) {
    hl_invalidate(l1);
    PtBuf out = { 0 };
    if (l1 == l2) {
        Line *L = LN(l1);
        pb_add(&out, L->s + c1, c2 - c1);
        line_splice(L, c1, c2 - c1, "", 0);
    } else {
        Line *A = LN(l1);
        pb_add(&out, A->s + c1, A->len - c1);
        if (A->cr) pb_addc(&out, '\r');
        pb_addc(&out, '\n');
        for (long i = l1 + 1; i < l2; i++) {
            Line *M = LN(i);
            pb_add(&out, M->s, M->len);
            if (M->cr) pb_addc(&out, '\r');
            pb_addc(&out, '\n');
        }
        Line *B = LN(l2);
        pb_add(&out, B->s, c2);
        line_splice(A, c1, A->len - c1, B->s + c2, B->len - c2);
        A->cr = B->cr;
        slot_remove(l1 + 1, l2 - l1);
    }
    if (!out.p) pb_adds(&out, "");
    *outlen = out.len;
    return out.p;
}

void text_end_pos(long ln, size_t col, const char *text, size_t n, long *eln, size_t *ecol) {
    long k = 0;
    size_t last = (size_t)-1;
    for (size_t i = 0; i < n; i++) if (text[i] == '\n') { k++; last = i; }
    if (!k) { *eln = ln; *ecol = col + n; }
    else { *eln = ln + k; *ecol = n - (last + 1); }
}

/* ------------------------------------------------------------------ undo */

static void op_free(UndoOp *o) {
    free(o->text);
    E.undo_bytes -= o->len;
    o->text = NULL;
}

static void ops_drop_from(long idx) {
    for (long i = idx; i < E.nops; i++) op_free(&E.ops[i]);
    E.nops = idx;
}

/* Forget the oldest groups once the history gets too big. */
static void undo_trim(void) {
    if (E.undo_bytes <= UNDO_LIMIT) return;
    long cut = E.nops / 4;
    if (cut < 1) cut = 1;
    while (cut < E.undo_pos && E.ops[cut].group == E.ops[cut - 1].group) cut++;
    if (cut >= E.undo_pos) return;
    for (long i = 0; i < cut; i++) op_free(&E.ops[i]);
    memmove(E.ops, E.ops + cut, (size_t)(E.nops - cut) * sizeof(UndoOp));
    E.nops -= cut;
    E.undo_pos -= cut;
    if (E.save_pos >= 0) E.save_pos = E.save_pos >= cut ? E.save_pos - cut : -1;
}

static UndoOp *op_push(int type, long ln, size_t col, char *text, size_t len) {
    if (E.undo_pos < E.nops) {
        if (E.save_pos > E.undo_pos) E.save_pos = -1;      /* the saved state can no longer be reached */
        ops_drop_from(E.undo_pos);
    }
    if (E.nops == E.opcap) {
        E.opcap = E.opcap ? E.opcap * 2 : 256;
        E.ops = realloc(E.ops, (size_t)E.opcap * sizeof(UndoOp));
        if (!E.ops) pt_oom();
    }
    UndoOp *o = &E.ops[E.nops++];
    o->type = type;
    o->ln = ln;
    o->col = col;
    o->text = text;
    o->len = len;
    o->group = E.group;
    E.undo_bytes += len;
    E.undo_pos = E.nops;
    undo_trim();
    return &E.ops[E.nops - 1];
}

void undo_boundary(void) { E.group++; }

static int has_nl(const char *s, size_t n) { return memchr(s, '\n', n) != NULL; }

/* Recorded insert. `merge` lets a run of typing share one undo step. */
void edit_insert(long ln, size_t col, const char *text, size_t n, long *eln, size_t *ecol, int merge) {
    if (E.nops && E.undo_pos == E.nops && merge && !has_nl(text, n)) {
        UndoOp *p = &E.ops[E.nops - 1];
        if (p->group == E.group && p->type == OP_INS && p->ln == ln && p->col + p->len == col && !has_nl(p->text, p->len)) {
            char *nt = realloc(p->text, p->len + n + 1);
            if (!nt) pt_oom();
            memcpy(nt + p->len, text, n);
            p->text = nt;
            p->len += n;
            E.undo_bytes += n;
            buf_insert_raw(ln, col, text, n, eln, ecol);
            return;
        }
    }
    char *copy = malloc(n + 1);
    if (!copy) pt_oom();
    memcpy(copy, text, n);
    copy[n] = 0;
    buf_insert_raw(ln, col, text, n, eln, ecol);
    op_push(OP_INS, ln, col, copy, n);
}

/* Recorded delete; returns the removed text (caller frees). */
char *edit_delete(long l1, size_t c1, long l2, size_t c2, size_t *outlen, int merge) {
    size_t n;
    char *text = buf_delete_raw(l1, c1, l2, c2, &n);
    if (outlen) *outlen = n;
    if (merge && l1 == l2 && E.nops && E.undo_pos == E.nops && !has_nl(text, n)) {
        UndoOp *p = &E.ops[E.nops - 1];
        if (p->group == E.group && p->type == OP_DEL && p->ln == l1 && !has_nl(p->text, p->len)) {
            if (c1 + n == p->col) {                         /* backspacing: the new text goes in front */
                char *nt = malloc(p->len + n + 1);
                if (!nt) pt_oom();
                memcpy(nt, text, n);
                memcpy(nt + n, p->text, p->len);
                free(p->text);
                p->text = nt;
                p->col = c1;
                p->len += n;
                E.undo_bytes += n;
                return text;
            }
            if (c1 == p->col) {                             /* forward delete: the new text goes behind */
                char *nt = realloc(p->text, p->len + n + 1);
                if (!nt) pt_oom();
                memcpy(nt + p->len, text, n);
                p->text = nt;
                p->len += n;
                E.undo_bytes += n;
                return text;
            }
        }
    }
    char *copy = malloc(n + 1);
    if (!copy) pt_oom();
    memcpy(copy, text, n);
    copy[n] = 0;
    op_push(OP_DEL, l1, c1, copy, n);
    return text;
}

static void apply_undo(const UndoOp *o) {
    if (o->type == OP_INS) {
        long eln;
        size_t ecol, n;
        text_end_pos(o->ln, o->col, o->text, o->len, &eln, &ecol);
        free(buf_delete_raw(o->ln, o->col, eln, ecol, &n));
        E.cy = o->ln;
        E.cx = o->col;
    } else {
        long eln;
        size_t ecol;
        buf_insert_raw(o->ln, o->col, o->text, o->len, &eln, &ecol);
        E.cy = eln;
        E.cx = ecol;
    }
}

static void apply_redo(const UndoOp *o) {
    if (o->type == OP_INS) {
        long eln;
        size_t ecol;
        buf_insert_raw(o->ln, o->col, o->text, o->len, &eln, &ecol);
        E.cy = eln;
        E.cx = ecol;
    } else {
        long eln;
        size_t ecol, n;
        text_end_pos(o->ln, o->col, o->text, o->len, &eln, &ecol);
        free(buf_delete_raw(o->ln, o->col, eln, ecol, &n));
        E.cy = o->ln;
        E.cx = o->col;
    }
}

int do_undo(void) {
    if (E.undo_pos == 0) return 0;
    long g = E.ops[E.undo_pos - 1].group;
    while (E.undo_pos > 0 && E.ops[E.undo_pos - 1].group == g) apply_undo(&E.ops[--E.undo_pos]);
    return 1;
}

int do_redo(void) {
    if (E.undo_pos >= E.nops) return 0;
    long g = E.ops[E.undo_pos].group;
    while (E.undo_pos < E.nops && E.ops[E.undo_pos].group == g) apply_redo(&E.ops[E.undo_pos++]);
    return 1;
}

int buf_modified(void) { return E.force_modified || E.undo_pos != E.save_pos; }
void buf_mark_saved(void) { E.save_pos = E.undo_pos; E.force_modified = 0; }

/* ------------------------------------------------------------------ loading */

int buf_load_fd(int fd, const char *what) {
    (void)what;
    struct stat st;
    size_t cap = 65536;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) cap = (size_t)st.st_size + 3;   /* room to see EOF */
    char *blob = malloc(cap);
    if (!blob) { errno = ENOMEM; return -1; }
    size_t len = 0;
    for (;;) {
        if (len + 2 >= cap) {
            cap *= 2;
            char *nb = realloc(blob, cap);
            if (!nb) { free(blob); errno = ENOMEM; return -1; }
            blob = nb;
        }
        ssize_t r = read(fd, blob + len, cap - len - 1);
        if (r < 0) { if (errno == EINTR) continue; int e = errno; free(blob); errno = e; return -1; }
        if (r == 0) break;
        len += (size_t)r;
    }
    blob[len] = 0;
    if (len == 0) {
        free(blob);
        buf_reset_empty();
        E.was_empty = 1;
        return 0;
    }
    long count = 0;
    for (size_t i = 0; i < len; i++) if (blob[i] == '\n') count++;
    int terminated = blob[len - 1] == '\n';
    long nlines = count + (terminated ? 0 : 1);
    for (long i = 0; i < E.n; i++) { Line *L = LN(i); if (L->cap) free(L->s); }
    free(E.a);
    E.cap = nlines + nlines / 8 + 1024;
    E.a = malloc((size_t)E.cap * sizeof(Line));
    if (!E.a) { E.cap = E.n = 0; free(blob); errno = ENOMEM; return -1; }
    E.gap = nlines;
    E.gaplen = E.cap - nlines;
    E.n = nlines;
    size_t start = 0;
    long li = 0;
    for (size_t i = 0; i <= len && li < nlines; i++) {
        if (i == len || blob[i] == '\n') {
            if (i == len && start == len) break;                    /* nothing after the final newline */
            Line *L = &E.a[li++];
            size_t end = i;
            L->cr = 0;
            L->hl = 0;
            if (i < len && end > start && blob[end - 1] == '\r') { end--; L->cr = 1; }
            L->s = blob + start;
            L->len = (uint32_t)(end - start);
            L->cap = 0;
            start = i + 1;
        }
    }
    E.blob = blob;
    E.final_newline = terminated;
    hl_invalidate(0);
    return 0;
}

/* ------------------------------------------------------------------ saving */

size_t buf_total_bytes(void) {
    size_t total = 0;
    for (long i = 0; i < E.n; i++) {
        Line *L = LN(i);
        total += L->len;
        if (i < E.n - 1 || E.final_newline) total += L->cr ? 2 : 1;
    }
    return total;
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

/* Writes the whole buffer to fd. Returns 0 or -1 with errno set. */
int buf_write_fd(int fd) {
    if (E.was_empty && !(E.n == 1 && LN(0)->len == 0)) E.final_newline = 1;
    if (E.n == 1 && LN(0)->len == 0 && !E.final_newline) return 0;
    char *buf = malloc(65536 + 8);
    if (!buf) { errno = ENOMEM; return -1; }
    size_t fill = 0;
    for (long i = 0; i < E.n; i++) {
        Line *L = LN(i);
        const char *s = L->s;
        size_t n = L->len;
        while (n) {
            size_t room = 65536 - fill;
            size_t take = n < room ? n : room;
            memcpy(buf + fill, s, take);
            fill += take;
            s += take;
            n -= take;
            if (fill == 65536) {
                if (write_all(fd, buf, fill) < 0) { int e = errno; free(buf); errno = e; return -1; }
                fill = 0;
            }
        }
        if (i < E.n - 1 || E.final_newline) {
            if (L->cr) buf[fill++] = '\r';
            buf[fill++] = '\n';
            if (fill >= 65536) {
                if (write_all(fd, buf, fill) < 0) { int e = errno; free(buf); errno = e; return -1; }
                fill = 0;
            }
        }
    }
    int rc = fill ? write_all(fd, buf, fill) : 0;
    int e = errno;
    free(buf);
    errno = e;
    return rc;
}
