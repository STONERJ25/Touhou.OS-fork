/* diff: compare files or directories line by line.
 * The edit script comes from Myers' O(ND) algorithm in the form GNU diff uses it: the common head and tail are stripped,
 * lines that occur only in one file are dropped from the search, the rest is split by the linear-space middle snake
 * (with a cost limit, so two unrelated big files stay cheap and still give a valid diff), and groups of changed lines are
 * slid so they line up with their neighbours. Output: normal, -u, -c, -q; -r -N -s -a -b -w -i -B -x --label. */
#include "kappabox.h"
#include <fnmatch.h>

#define SNAKE_LIMIT 20
#define MIN_COST_LIMIT 2048

enum { FMT_NORMAL, FMT_UNIFIED, FMT_CONTEXT };

static int o_fmt, o_ctx = 3;
static int o_brief, o_recursive, o_newfile, o_same, o_text, o_minimal;
static int o_icase, o_ws;                   /* o_ws: 0, 1 (-b, collapse runs of blanks) or 2 (-w, ignore all blanks) */
static int o_blank_lines, o_strip_cr;
static const char *o_label[2];
static int o_nlabel;
static char **o_excl;
static int o_nexcl;
static char g_switches[512];
static int g_plain = 1;                     /* no -i -b -w: lines are compared as raw bytes */

/* ------------------------------------------------------------------ buffered output (stdio is slow byte by byte) */

static char g_ob[65536];
static size_t g_on;

static void out_flush(void) {
    size_t off = 0;
    while (off < g_on) {
        ssize_t w = write(1, g_ob + off, g_on - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    g_on = 0;
}

static void out_bytes(const char *s, size_t n) {
    if (n >= sizeof g_ob) {
        size_t off = 0;
        out_flush();
        while (off < n) {
            ssize_t w = write(1, s + off, n - off);
            if (w <= 0) break;
            off += (size_t)w;
        }
        return;
    }
    if (g_on + n > sizeof g_ob) out_flush();
    memcpy(g_ob + g_on, s, n);
    g_on += n;
}

static void out_str(const char *s) { out_bytes(s, strlen(s)); }

static void out_num(long v) {
    char tmp[24];
    int n = 0;
    if (v < 0) { out_bytes("-", 1); v = -v; }
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) out_bytes(&tmp[--n], 1);
}

/* ------------------------------------------------------------------ files as arrays of lines */

typedef struct {
    char *buf;
    size_t len;
    unsigned *start;                        /* start[i]: offset of line i, start[n] == len */
    long n;
    int nonl;                               /* the last line has no newline */
    int binary;
    long mtime;
} Text;

static void text_free(Text *t) {
    free(t->buf);
    free(t->start);
    memset(t, 0, sizeof *t);
}

/* "-" is standard input; an absent file (-N) is empty. Returns -1 after printing a message. */
static int text_load(Text *t, const char *name, int absent) {
    int fd = -1;
    size_t hint = 0, cap, n = 0;
    char *buf;
    memset(t, 0, sizeof *t);
    if (!absent) {
        struct stat st;
        if (!strcmp(name, "-")) fd = 0;
        else if ((fd = open(name, O_RDONLY)) < 0) { out_flush(); kb_error("%s: %s", name, strerror(errno)); return -1; }
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) { hint = (size_t)st.st_size; t->mtime = st.st_mtime; }
        else t->mtime = time(NULL);
    }
    cap = hint + 2;
    buf = malloc(cap);
    if (!buf) kb_die("out of memory");
    while (fd >= 0) {
        ssize_t r;
        if (n + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); if (!buf) kb_die("out of memory"); }
        r = read(fd, buf + n, cap - n - 1);
        if (r < 0) { out_flush(); kb_error("%s: %s", name, strerror(errno)); free(buf); if (fd) close(fd); return -1; }
        if (r == 0) break;
        n += (size_t)r;
    }
    if (fd > 0) close(fd);
    if (o_strip_cr) {
        size_t w = 0;
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == '\r' && i + 1 < n && buf[i + 1] == '\n') continue;
            buf[w++] = buf[i];
        }
        n = w;
    }
    if (n >= 0x7fffffffu) { free(buf); kb_error("%s: file too large", name); return -1; }
    t->buf = buf;
    t->len = n;
    t->binary = !o_text && memchr(buf, 0, n) != NULL;
    if (t->binary) return 0;
    {
        long lines = 0;
        const char *p = buf, *e = buf + n;
        while (p < e) {
            const char *q = memchr(p, '\n', (size_t)(e - p));
            lines++;
            if (!q) { t->nonl = 1; break; }
            p = q + 1;
        }
        t->n = lines;
        t->start = malloc((size_t)(lines + 1) * sizeof *t->start);
        if (!t->start) kb_die("out of memory");
        p = buf;
        for (long i = 0; i < lines; i++) {
            const char *q = memchr(p, '\n', (size_t)(e - p));
            t->start[i] = (unsigned)(p - buf);
            p = q ? q + 1 : e;
        }
        t->start[lines] = (unsigned)n;
    }
    return 0;
}

/* ------------------------------------------------------------------ comparing lines: -i -b -w and the equivalence classes */

static char *g_sc[2];
static size_t g_sccap[2];

/* The comparison form of a line: for -i -b -w a lower-cased copy without the ignored blanks (in scratch slot SLOT). */
static const char *canon(const char *p, size_t len, size_t *outlen, int slot) {
    char *o;
    size_t n = 0, end = len;
    int pend = 0, nl = len && p[len - 1] == '\n';
    if (g_plain) { *outlen = len; return p; }
    if (g_sccap[slot] < len + 2) {
        free(g_sc[slot]);
        g_sccap[slot] = len + 258;
        g_sc[slot] = malloc(g_sccap[slot]);
        if (!g_sc[slot]) kb_die("out of memory");
    }
    o = g_sc[slot];
    if (nl) end--;
    for (size_t i = 0; i < end; i++) {
        unsigned char c = (unsigned char)p[i];
        if (o_ws && isspace(c)) {
            if (o_ws == 1) pend = 1;
            continue;
        }
        if (pend) { o[n++] = ' '; pend = 0; }
        o[n++] = (char)(o_icase ? tolower(c) : c);
    }
    if (nl) o[n++] = '\n';
    *outlen = n;
    return o;
}

static int lines_eq(const Text *a, long i, const Text *b, long j) {
    const char *pa = a->buf + a->start[i], *pb = b->buf + b->start[j];
    size_t la = a->start[i + 1] - a->start[i], lb = b->start[j + 1] - b->start[j], ca, cb;
    if (g_plain) return la == lb && !memcmp(pa, pb, la);
    pa = canon(pa, la, &ca, 0);
    pb = canon(pb, lb, &cb, 1);
    return ca == cb && !memcmp(pa, pb, ca);
}

typedef struct {
    const char **p;
    unsigned *len, *hash;
    int *cnt[2];
    int n;
    int *slots;
    unsigned mask;
} Classes;

static void classes_init(Classes *c, long total) {
    unsigned size = 16;
    if (total < 1) total = 1;
    while (size < (unsigned long)total * 2) size <<= 1;
    c->n = 0;
    c->mask = size - 1;
    c->p = malloc((size_t)total * sizeof *c->p);
    c->len = malloc((size_t)total * sizeof *c->len);
    c->hash = malloc((size_t)total * sizeof *c->hash);
    c->cnt[0] = malloc((size_t)total * sizeof(int));
    c->cnt[1] = malloc((size_t)total * sizeof(int));
    c->slots = calloc(size, sizeof(int));
    if (!c->p || !c->len || !c->hash || !c->cnt[0] || !c->cnt[1] || !c->slots) kb_die("out of memory");
}

static void classes_free(Classes *c) {
    if (!g_plain) for (int i = 0; i < c->n; i++) free((void *)c->p[i]);
    free(c->p); free(c->len); free(c->hash); free(c->cnt[0]); free(c->cnt[1]); free(c->slots);
}

/* Gives lines lo..hi-1 of T a class number each (equal lines, as far as the options care, share one); counts go to cnt[WHICH]. */
static void assign_ids(Classes *cl, const Text *t, int which, long lo, long hi, int *ids) {
    for (long i = lo; i < hi; i++) {
        size_t clen;
        const char *cp = canon(t->buf + t->start[i], t->start[i + 1] - t->start[i], &clen, 0);
        unsigned h = 2166136261u, s;
        int c;
        for (size_t k = 0; k < clen; k++) h = (h ^ (unsigned char)cp[k]) * 16777619u;
        s = h & cl->mask;
        for (;;) {
            int v = cl->slots[s];
            if (!v) {
                c = cl->n++;
                cl->slots[s] = c + 1;
                cl->hash[c] = h;
                cl->len[c] = (unsigned)clen;
                if (g_plain) cl->p[c] = cp;
                else {
                    char *cpy = malloc(clen ? clen : 1);
                    if (!cpy) kb_die("out of memory");
                    memcpy(cpy, cp, clen);
                    cl->p[c] = cpy;
                }
                cl->cnt[0][c] = cl->cnt[1][c] = 0;
                break;
            }
            c = v - 1;
            if (cl->hash[c] == h && cl->len[c] == clen && !memcmp(cl->p[c], cp, clen)) break;
            s = (s + 1) & cl->mask;
        }
        cl->cnt[which][c]++;
        ids[i - lo] = c;
    }
}

/* ------------------------------------------------------------------ Myers: shortest edit script by the middle snake */

static const int *g_xv, *g_yv;              /* the lines that take part in the search (as class numbers) */
static const int *g_rx, *g_ry;              /* their line numbers in the files */
static char *g_chx, *g_chy;                 /* result: changed[line] */
static long *g_fd, *g_bd;
static long g_too_expensive;
static int g_heuristic;

/* Finds the midpoint of a shortest edit script for xv[xoff..xlim) against yv[yoff..ylim) by searching from both ends at
 * once. The first and the last elements of the two ranges must differ. When the search gets too expensive it settles for
 * a good-enough midpoint (the diff is then valid but may not be the smallest). */
static void diag(long xoff, long xlim, long yoff, long ylim, int minimal, long *pxmid, long *pymid, int *plo, int *phi) {
    long *fd = g_fd, *bd = g_bd;
    const int *xv = g_xv, *yv = g_yv;
    long dmin = xoff - ylim, dmax = xlim - yoff;
    long fmid = xoff - yoff, bmid = xlim - ylim;
    long fmin = fmid, fmax = fmid, bmin = bmid, bmax = bmid;
    int odd = (int)((fmid - bmid) & 1);

    fd[fmid] = xoff;
    bd[bmid] = xlim;
    for (long c = 1;; ++c) {
        long d;
        int big_snake = 0;

        if (fmin > dmin) fd[--fmin - 1] = -1; else ++fmin;
        if (fmax < dmax) fd[++fmax + 1] = -1; else --fmax;
        for (d = fmax; d >= fmin; d -= 2) {
            long tlo = fd[d - 1], thi = fd[d + 1];
            long x0 = tlo < thi ? thi : tlo + 1;
            long x = x0, y = x0 - d;
            while (x < xlim && y < ylim && xv[x] == yv[y]) { x++; y++; }
            if (x - x0 > SNAKE_LIMIT) big_snake = 1;
            fd[d] = x;
            if (odd && bmin <= d && d <= bmax && bd[d] <= x) {
                *pxmid = x; *pymid = y; *plo = *phi = 1;
                return;
            }
        }

        if (bmin > dmin) bd[--bmin - 1] = LONG_MAX; else ++bmin;
        if (bmax < dmax) bd[++bmax + 1] = LONG_MAX; else --bmax;
        for (d = bmax; d >= bmin; d -= 2) {
            long tlo = bd[d - 1], thi = bd[d + 1];
            long x0 = tlo < thi ? tlo : thi - 1;
            long x = x0, y = x0 - d;
            while (xoff < x && yoff < y && xv[x - 1] == yv[y - 1]) { x--; y--; }
            if (x0 - x > SNAKE_LIMIT) big_snake = 1;
            bd[d] = x;
            if (!odd && fmin <= d && d <= fmax && x <= fd[d]) {
                *pxmid = x; *pymid = y; *plo = *phi = 1;
                return;
            }
        }

        if (minimal) continue;

        /* a diagonal that has made much more progress than its cost suggests: take it (only for very large inputs) */
        if (200 < c && big_snake && g_heuristic) {
            long best = 0;
            for (d = fmax; d >= fmin; d -= 2) {
                long dd = d - fmid, x = fd[d], y = x - d;
                long v = (x - xoff) * 2 - dd;
                if (v > 12 * (c + (dd < 0 ? -dd : dd)) && v > best &&
                    xoff + SNAKE_LIMIT <= x && x < xlim && yoff + SNAKE_LIMIT <= y && y < ylim) {
                    int k;
                    for (k = 1; xv[x - k] == yv[y - k]; k++)
                        if (k == SNAKE_LIMIT) { best = v; *pxmid = x; *pymid = y; break; }
                }
            }
            if (best > 0) { *plo = 1; *phi = 0; return; }
            best = 0;
            for (d = bmax; d >= bmin; d -= 2) {
                long dd = d - bmid, x = bd[d], y = x - d;
                long v = (xlim - x) * 2 + dd;
                if (v > 12 * (c + (dd < 0 ? -dd : dd)) && v > best &&
                    xoff < x && x <= xlim - SNAKE_LIMIT && yoff < y && y <= ylim - SNAKE_LIMIT) {
                    int k;
                    for (k = 0; xv[x + k] == yv[y + k]; k++)
                        if (k == SNAKE_LIMIT - 1) { best = v; *pxmid = x; *pymid = y; break; }
                }
            }
            if (best > 0) { *plo = 0; *phi = 1; return; }
        }

        /* gone well beyond the call of duty: settle for the point halfway along the best diagonals found so far */
        if (c >= g_too_expensive) {
            long fxybest = -1, fxbest = 0, bxybest = LONG_MAX, bxbest = 0;
            for (d = fmax; d >= fmin; d -= 2) {
                long x = fd[d] < xlim ? fd[d] : xlim, y = x - d;
                if (ylim < y) { x = ylim + d; y = ylim; }
                if (fxybest < x + y) { fxybest = x + y; fxbest = x; }
            }
            for (d = bmax; d >= bmin; d -= 2) {
                long x = xoff > bd[d] ? xoff : bd[d], y = x - d;
                if (y < yoff) { x = yoff + d; y = yoff; }
                if (x + y < bxybest) { bxybest = x + y; bxbest = x; }
            }
            if ((xlim + ylim) - bxybest < fxybest - (xoff + yoff)) {
                *pxmid = fxbest; *pymid = fxybest - fxbest; *plo = 1; *phi = 0;
            } else {
                *pxmid = bxbest; *pymid = bxybest - bxbest; *plo = 0; *phi = 1;
            }
            return;
        }
    }
}

/* Marks the lines of xv[xoff..xlim) / yv[yoff..ylim) that are not part of a common subsequence. */
static void compareseq(long xoff, long xlim, long yoff, long ylim, int minimal) {
    for (;;) {
        long xmid, ymid;
        int lo, hi;
        while (xoff < xlim && yoff < ylim && g_xv[xoff] == g_yv[yoff]) { xoff++; yoff++; }
        while (xoff < xlim && yoff < ylim && g_xv[xlim - 1] == g_yv[ylim - 1]) { xlim--; ylim--; }
        if (xoff == xlim) { while (yoff < ylim) g_chy[g_ry[yoff++]] = 1; return; }
        if (yoff == ylim) { while (xoff < xlim) g_chx[g_rx[xoff++]] = 1; return; }
        diag(xoff, xlim, yoff, ylim, minimal, &xmid, &ymid, &lo, &hi);
        if ((xmid == xoff && ymid == yoff) || (xmid == xlim && ymid == ylim) ||
            xmid < xoff || xmid > xlim || ymid < yoff || ymid > ylim) {         /* no progress: give up on this range */
            while (xoff < xlim) g_chx[g_rx[xoff++]] = 1;
            while (yoff < ylim) g_chy[g_ry[yoff++]] = 1;
            return;
        }
        if ((xmid - xoff) + (ymid - yoff) < (xlim - xmid) + (ylim - ymid)) {    /* recurse into the smaller half */
            compareseq(xoff, xmid, yoff, ymid, lo);
            xoff = xmid; yoff = ymid; minimal = hi;
        } else {
            compareseq(xmid, xlim, ymid, ylim, hi);
            xlim = xmid; ylim = ymid; minimal = lo;
        }
    }
}

/* Lines that match nothing in the other file are changed for sure and are left out of the search. A line that matches
 * very many lines (a blank line, a brace) is only left out inside a run of such lines: it would mislead the search. */
static void discard_scan(unsigned char *discards, const int *equivs, long end, const int *counts) {
    long many = 5, tem = end / 64;
    while ((tem = tem >> 2) > 0) many *= 2;
    for (long i = 0; i < end; i++) {
        long nmatch = counts[equivs[i]];
        if (nmatch == 0) discards[i] = 1;
        else if (nmatch > many) discards[i] = 2;
    }
}

static void discard_refine(unsigned char *discards, long end) {
    for (long i = 0; i < end; i++) {
        if (discards[i] == 2) discards[i] = 0;
        else if (discards[i] != 0) {
            long j, length, last, provisional = 0;
            for (j = i; j < end; j++) {
                if (discards[j] == 0) break;
                if (discards[j] == 2) ++provisional;
            }
            while (j > i && discards[j - 1] == 2) { discards[--j] = 0; --provisional; }
            length = j - i;
            last = i + length - 1;
            if (provisional * 4 > length) {
                while (j > i) if (discards[--j] == 2) discards[j] = 0;
            } else {
                long consec, minimum = 1, tem = length >> 2;
                while (0 < (tem >>= 2)) minimum <<= 1;
                minimum++;
                for (j = 0, consec = 0; j < length; j++) {
                    if (discards[i + j] != 2) consec = 0;
                    else if (minimum == ++consec) j -= consec;
                    else if (minimum < consec) discards[i + j] = 0;
                }
                for (j = 0, consec = 0; j < length; j++) {
                    if (j >= 8 && discards[i + j] == 1) break;
                    if (discards[i + j] == 2) { consec = 0; discards[i + j] = 0; }
                    else if (discards[i + j] == 0) consec = 0;
                    else consec++;
                    if (consec == 3) break;
                }
                i += length - 1;
                for (j = 0, consec = 0; j < length; j++) {
                    if (j >= 8 && discards[i - j] == 1) break;
                    if (discards[i - j] == 2) { consec = 0; discards[i - j] = 0; }
                    else if (discards[i - j] == 0) consec = 0;
                    else consec++;
                    if (consec == 3) break;
                }
            }
            i = last;
        }
    }
}

/* Slides each group of changed lines up or down as far as the neighbouring lines are equal, merging groups that touch and
 * lining groups up with changes in the other file. Arrays changed[] have a zero before the first and after the last. */
static void shift_boundaries(const int *eq[2], char *chg[2], long len[2]) {
    for (int f = 0; f < 2; f++) {
        char *changed = chg[f], *other = chg[1 - f];
        const int *equivs = eq[f];
        long i = 0, j = 0, i_end = len[f];
        for (;;) {
            long runlength, start, corresponding;
            while (i < i_end && !changed[i]) {
                while (other[j++]) continue;
                i++;
            }
            if (i == i_end) break;
            start = i;
            while (changed[++i]) continue;
            while (other[j]) j++;
            do {
                runlength = i - start;
                while (start && equivs[start - 1] == equivs[i - 1]) {
                    changed[--start] = 1;
                    changed[--i] = 0;
                    while (changed[start - 1]) start--;
                    while (other[--j]) continue;
                }
                corresponding = other[j - 1] ? i : i_end;
                while (i != i_end && equivs[start] == equivs[i]) {
                    changed[start++] = 0;
                    changed[i++] = 1;
                    while (changed[i]) i++;
                    while (other[++j]) corresponding = i;
                }
            } while (runlength != i - start);
            while (corresponding < i) {
                changed[--start] = 1;
                changed[--i] = 0;
                while (other[--j]) continue;
            }
        }
    }
}

/* ids0[0..n0) and ids1[0..n1) are the class numbers of the lines; chg0/chg1 (zero-filled, one spare element each side)
 * receive 1 for every line that is deleted / inserted. */
static void diff_core(const int *ids0, long n0, const int *ids1, long n1, const int *cnt0, const int *cnt1, char *chg0, char *chg1) {
    const int *ids[2] = { ids0, ids1 };
    long n[2] = { n0, n1 };
    char *chg[2] = { chg0, chg1 };
    unsigned char *disc[2];
    int *und[2], *real[2];
    long nu[2], diags;
    long *fb;

    for (int f = 0; f < 2; f++) {
        disc[f] = calloc((size_t)n[f] + 1, 1);
        und[f] = malloc(((size_t)n[f] + 1) * sizeof(int));
        real[f] = malloc(((size_t)n[f] + 1) * sizeof(int));
        if (!disc[f] || !und[f] || !real[f]) kb_die("out of memory");
    }
    if (!o_minimal) {
        discard_scan(disc[0], ids0, n0, cnt1);
        discard_scan(disc[1], ids1, n1, cnt0);
        discard_refine(disc[0], n0);
        discard_refine(disc[1], n1);
    }
    for (int f = 0; f < 2; f++) {
        long j = 0;
        for (long i = 0; i < n[f]; i++) {
            if (disc[f][i] == 0) { und[f][j] = ids[f][i]; real[f][j++] = (int)i; }
            else chg[f][i] = 1;
        }
        nu[f] = j;
    }
    diags = nu[0] + nu[1] + 3;
    fb = malloc((size_t)(2 * diags + 2) * sizeof(long));
    if (!fb) kb_die("out of memory");
    g_fd = fb + nu[1] + 1;
    g_bd = g_fd + diags;
    g_xv = und[0]; g_yv = und[1];
    g_rx = real[0]; g_ry = real[1];
    g_chx = chg0; g_chy = chg1;
    g_too_expensive = 1;
    for (long d = diags; d != 0; d >>= 2) g_too_expensive <<= 1;
    if (g_too_expensive < MIN_COST_LIMIT) g_too_expensive = MIN_COST_LIMIT;
    g_heuristic = nu[0] + nu[1] > 100000;
    compareseq(0, nu[0], 0, nu[1], o_minimal);
    free(fb);
    for (int f = 0; f < 2; f++) { free(disc[f]); free(und[f]); free(real[f]); }
    shift_boundaries(ids, chg, n);
}

/* ------------------------------------------------------------------ the edit script */

typedef struct { long l0, l1, del, ins; int ignore; } Change;

static int line_blank(const Text *t, long i) {
    size_t cl;
    const char *c = canon(t->buf + t->start[i], t->start[i + 1] - t->start[i], &cl, 0);
    return cl == 0 || (cl == 1 && c[0] == '\n');
}

/* Computes the changes between two texts; returns their number (0 = identical). *OUT is malloc'd. */
static long make_script(const Text *t0, const Text *t1, Change **out) {
    long p = 0, s = 0, m0, m1, nch = 0, cap = 0;
    Change *ch = NULL;
    *out = NULL;
    while (p < t0->n && p < t1->n && lines_eq(t0, p, t1, p)) p++;
    while (s < t0->n - p && s < t1->n - p && lines_eq(t0, t0->n - 1 - s, t1, t1->n - 1 - s)) s++;
    m0 = t0->n - p - s;
    m1 = t1->n - p - s;
    if (m0 == 0 && m1 == 0) return 0;
    {
        char *c0 = calloc((size_t)m0 + 2, 1), *c1 = calloc((size_t)m1 + 2, 1);
        long i0 = 0, i1 = 0;
        if (!c0 || !c1) kb_die("out of memory");
        if (m0 == 0 || m1 == 0) {
            for (long i = 0; i < m0; i++) c0[1 + i] = 1;
            for (long i = 0; i < m1; i++) c1[1 + i] = 1;
        } else {
            Classes cl;
            int *id0 = malloc((size_t)m0 * sizeof(int)), *id1 = malloc((size_t)m1 * sizeof(int));
            if (!id0 || !id1) kb_die("out of memory");
            classes_init(&cl, m0 + m1);
            assign_ids(&cl, t0, 0, p, p + m0, id0);
            assign_ids(&cl, t1, 1, p, p + m1, id1);
            diff_core(id0, m0, id1, m1, cl.cnt[0], cl.cnt[1], c0 + 1, c1 + 1);
            classes_free(&cl);
            free(id0);
            free(id1);
        }
        for (;;) {
            long b0, b1;
            while (i0 < m0 && i1 < m1 && !c0[1 + i0] && !c1[1 + i1]) { i0++; i1++; }
            if (i0 >= m0 && i1 >= m1) break;
            b0 = i0;
            b1 = i1;
            while (i0 < m0 && c0[1 + i0]) i0++;
            while (i1 < m1 && c1[1 + i1]) i1++;
            if (i0 == b0 && i1 == b1) break;                /* cannot happen; do not loop forever if it does */
            if (nch == cap) {
                cap = cap ? cap * 2 : 64;
                ch = realloc(ch, (size_t)cap * sizeof *ch);
                if (!ch) kb_die("out of memory");
            }
            ch[nch].l0 = p + b0;
            ch[nch].l1 = p + b1;
            ch[nch].del = i0 - b0;
            ch[nch].ins = i1 - b1;
            ch[nch].ignore = 0;
            nch++;
        }
        free(c0);
        free(c1);
    }
    if (o_blank_lines) {
        for (long k = 0; k < nch; k++) {
            int blank = 1;
            for (long i = ch[k].l0; blank && i < ch[k].l0 + ch[k].del; i++) blank = line_blank(t0, i);
            for (long j = ch[k].l1; blank && j < ch[k].l1 + ch[k].ins; j++) blank = line_blank(t1, j);
            ch[k].ignore = blank;
        }
    }
    *out = ch;
    return nch;
}

/* ------------------------------------------------------------------ output formats */

static const char *g_n0, *g_n1;             /* names shown in the headers */
static const Text *g_t0, *g_t1;
static const char *g_dirhdr;                /* "diff -r a/x b/x" line to print before the first output of this file pair, or NULL */
static int g_started;

static void begin_output(void) {
    if (g_dirhdr) { out_str(g_dirhdr); out_str("\n"); g_dirhdr = NULL; }
    g_started = 1;
}

static void out_time(long t) {
    struct tm tm;
    char b[64];
    time_t tt = t;
    long off;
    localtime_r(&tt, &tm);
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
    out_str(b);
    out_str(".000000000 ");
    off = tm.tm_gmtoff;
    out_str(off < 0 ? "-" : "+");
    if (off < 0) off = -off;
    out_num(off / 3600 / 10);
    out_num(off / 3600 % 10);
    out_num(off / 60 % 60 / 10);
    out_num(off / 60 % 10);
}

static void out_title(const char *mark, const char *name, const Text *t, const char *label) {
    out_str(mark);
    out_str(" ");
    out_str(label ? label : name);
    if (!label) { out_str("\t"); out_time(t->mtime); }
    out_str("\n");
}

static void out_line(const char *prefix, const Text *t, long i) {
    const char *p = t->buf + t->start[i];
    size_t len = t->start[i + 1] - t->start[i];
    out_str(prefix);
    out_bytes(p, len);
    if (len == 0 || p[len - 1] != '\n') out_str("\n\\ No newline at end of file\n");
}

static void out_range(long a, long b) {                 /* 1-based, inclusive; a > b is the empty range after line b */
    out_num(b > a ? a : b);
    if (b > a) { out_str(","); out_num(b); }
}

static void out_urange(long a, long b) {
    if (b <= a) { out_num(b); if (b != a) out_str(",0"); }
    else { out_num(a); out_str(","); out_num(b - a + 1); }
}

static void print_normal(const Change *c) {
    long f0 = c->l0, l0 = c->l0 + c->del - 1, f1 = c->l1, l1 = c->l1 + c->ins - 1;
    begin_output();
    out_range(f0 + 1, l0 + 1);
    out_str(c->del && c->ins ? "c" : c->ins ? "a" : "d");
    out_range(f1 + 1, l1 + 1);
    out_str("\n");
    for (long i = f0; i <= l0; i++) out_line("< ", g_t0, i);
    if (c->del && c->ins) out_str("---\n");
    for (long j = f1; j <= l1; j++) out_line("> ", g_t1, j);
}

/* The lines of file 0 and 1 a hunk covers, widened by the context. */
static void hunk_span(const Change *ch, long k0, long k1, long *f0, long *l0, long *f1, long *l1) {
    *f0 = ch[k0].l0;
    *f1 = ch[k0].l1;
    *l0 = ch[k1].l0 + ch[k1].del - 1;
    *l1 = ch[k1].l1 + ch[k1].ins - 1;
    *f0 = *f0 - o_ctx > 0 ? *f0 - o_ctx : 0;
    *f1 = *f1 - o_ctx > 0 ? *f1 - o_ctx : 0;
    *l0 = *l0 < g_t0->n - o_ctx ? *l0 + o_ctx : g_t0->n - 1;
    *l1 = *l1 < g_t1->n - o_ctx ? *l1 + o_ctx : g_t1->n - 1;
}

static void print_unified(const Change *ch, long k0, long k1) {
    long f0, l0, f1, l1, i, j, k = k0;
    hunk_span(ch, k0, k1, &f0, &l0, &f1, &l1);
    begin_output();
    out_str("@@ -");
    out_urange(f0 + 1, l0 + 1);
    out_str(" +");
    out_urange(f1 + 1, l1 + 1);
    out_str(" @@\n");
    i = f0;
    j = f1;
    while (i <= l0 || j <= l1) {
        if (k > k1 || i < ch[k].l0) {
            out_line(" ", g_t0, i++);
            j++;
        } else {
            for (long d = ch[k].del; d > 0; d--) out_line("-", g_t0, i++);
            for (long d = ch[k].ins; d > 0; d--) out_line("+", g_t1, j++);
            k++;
        }
    }
}

static void print_context(const Change *ch, long k0, long k1) {
    long f0, l0, f1, l1, k, dels = 0, inss = 0;
    hunk_span(ch, k0, k1, &f0, &l0, &f1, &l1);
    for (k = k0; k <= k1; k++) { dels += ch[k].del; inss += ch[k].ins; }
    begin_output();
    out_str("***************\n*** ");
    out_range(f0 + 1, l0 + 1);
    out_str(" ****\n");
    if (dels) {
        k = k0;
        for (long i = f0; i <= l0; i++) {
            while (k <= k1 && ch[k].l0 + ch[k].del <= i) k++;
            out_line(k <= k1 && ch[k].l0 <= i ? (ch[k].ins > 0 ? "! " : "- ") : "  ", g_t0, i);
        }
    }
    out_str("--- ");
    out_range(f1 + 1, l1 + 1);
    out_str(" ----\n");
    if (inss) {
        k = k0;
        for (long j = f1; j <= l1; j++) {
            while (k <= k1 && ch[k].l1 + ch[k].ins <= j) k++;
            out_line(k <= k1 && ch[k].l1 <= j ? (ch[k].del > 0 ? "! " : "+ ") : "  ", g_t1, j);
        }
    }
}

/* Last change of the hunk that starts at change K: changes closer together than twice the context belong together. */
static long find_hunk(const Change *ch, long nc, long k) {
    long thresh_far = 2 * (long)o_ctx + 1;
    for (;;) {
        long top0 = ch[k].l0 + ch[k].del, prev = k, thresh;
        k++;
        if (k >= nc) return prev;
        thresh = ch[k].ignore || ch[prev].ignore ? o_ctx : thresh_far;
        if (!(ch[k].l0 - top0 < thresh)) return prev;
    }
}

/* Prints the changes; returns 1 if there was anything to show (blank-only changes with -B are not). */
static int print_script(const Change *ch, long nc, const char *n0, const char *n1, const Text *t0, const Text *t1) {
    int shown = 0;
    g_t0 = t0;
    g_t1 = t1;
    g_n0 = n0;
    g_n1 = n1;
    g_started = 0;
    for (long k = 0; k < nc;) {
        long e = o_fmt == FMT_NORMAL ? k : find_hunk(ch, nc, k);
        int trivial = 1;
        for (long m = k; m <= e; m++) if (!ch[m].ignore) trivial = 0;
        if (!trivial) {
            if (!shown && o_fmt != FMT_NORMAL) {
                begin_output();
                if (o_fmt == FMT_UNIFIED) {
                    out_title("---", n0, t0, o_label[0]);
                    out_title("+++", n1, t1, o_label[1]);
                } else {
                    out_title("***", n0, t0, o_label[0]);
                    out_title("---", n1, t1, o_label[1]);
                }
            }
            shown = 1;
            if (o_fmt == FMT_NORMAL) print_normal(&ch[k]);
            else if (o_fmt == FMT_UNIFIED) print_unified(ch, k, e);
            else print_context(ch, k, e);
        }
        k = e + 1;
    }
    return shown;
}

/* ------------------------------------------------------------------ one pair of files */

/* Compares two named files (absent: treated as empty). Returns 0 (same), 1 (different) or 2 (trouble). */
static int compare_texts(const char *n0, int absent0, const char *n1, int absent1, const char *dirhdr) {
    Text t0, t1;
    Change *ch = NULL;
    long nch;
    int rc = 0;
    g_dirhdr = dirhdr;
    if (text_load(&t0, n0, absent0) < 0) return 2;
    if (text_load(&t1, n1, absent1) < 0) { text_free(&t0); return 2; }
    if (t0.binary || t1.binary) {
        int same = t0.len == t1.len && !memcmp(t0.buf, t1.buf, t0.len);
        if (!same) {
            out_str(o_brief ? "Files " : "Binary files ");
            out_str(n0); out_str(" and "); out_str(n1); out_str(" differ\n");
            rc = 1;
        } else if (o_same) {
            out_str("Files "); out_str(n0); out_str(" and "); out_str(n1); out_str(" are identical\n");
        }
        text_free(&t0);
        text_free(&t1);
        return rc;
    }
    g_plain = !(o_icase || o_ws);
    nch = (g_plain && t0.len == t1.len && !memcmp(t0.buf, t1.buf, t0.len)) ? 0 : make_script(&t0, &t1, &ch);
    if (o_blank_lines) {
        long keep = 0;
        for (long k = 0; k < nch; k++) if (!ch[k].ignore) keep++;
        if (!keep) nch = 0;
    }
    if (nch == 0) {
        if (o_same) { out_str("Files "); out_str(n0); out_str(" and "); out_str(n1); out_str(" are identical\n"); }
    } else if (o_brief) {
        out_str("Files "); out_str(n0); out_str(" and "); out_str(n1); out_str(" differ\n");
        rc = 1;
    } else {
        print_script(ch, nch, n0, n1, &t0, &t1);
        rc = 1;
    }
    g_dirhdr = NULL;
    free(ch);
    text_free(&t0);
    text_free(&t1);
    return rc;
}

/* ------------------------------------------------------------------ files and directories */

static int excluded(const char *name) {
    for (int i = 0; i < o_nexcl; i++) if (fnmatch(o_excl[i], name, 0) == 0) return 1;
    return 0;
}

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int list_dir(const char *path, char ***names, int *count) {
    DIR *d = opendir(path);
    struct dirent *e;
    char **v = NULL;
    int n = 0, cap = 0;
    *names = NULL;
    *count = 0;
    if (!d) { out_flush(); kb_error("%s: %s", path, strerror(errno)); return -1; }
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || excluded(e->d_name)) continue;
        if (n == cap) { cap = cap ? cap * 2 : 32; v = realloc(v, (size_t)cap * sizeof *v); if (!v) kb_die("out of memory"); }
        v[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (n > 1) qsort(v, (size_t)n, sizeof *v, cmp_names);
    *names = v;
    *count = n;
    return 0;
}

static const char *kind_name(mode_t m) {
    if (S_ISDIR(m)) return "directory";
    if (S_ISREG(m)) return "regular file";
    if (S_ISCHR(m)) return "character special file";
    if (S_ISBLK(m)) return "block special file";
    if (S_ISFIFO(m)) return "fifo";
    return "weird file";
}

static int compare_paths(const char *n0, const char *n1, int absent0, int absent1, int top);

/* A directory that does not exist (absent, only with -N -r) is listed as empty. */
static int compare_dirs(const char *d0, const char *d1, int absent0, int absent1) {
    char **v0 = NULL, **v1 = NULL;
    int c0 = 0, c1 = 0, i = 0, j = 0, rc = 0;
    if (!absent0 && list_dir(d0, &v0, &c0) < 0) return 2;
    if (!absent1 && list_dir(d1, &v1, &c1) < 0) return 2;
    while (i < c0 || j < c1) {
        int cmp = i >= c0 ? 1 : j >= c1 ? -1 : strcmp(v0[i], v1[j]);
        const char *name = cmp <= 0 ? v0[i] : v1[j];
        char *p0 = kb_join(d0, name), *p1 = kb_join(d1, name);
        int r = 0;
        if (cmp < 0 || cmp > 0) {
            int only0 = cmp < 0;
            if (!o_newfile) {
                out_str("Only in ");
                out_str(only0 ? d0 : d1);
                out_str(": ");
                out_str(name);
                out_str("\n");
                r = 1;
            } else {
                r = compare_paths(p0, p1, !only0, only0, 0);
            }
            if (only0) i++; else j++;
        } else {
            r = compare_paths(p0, p1, 0, 0, 0);
            i++;
            j++;
        }
        free(p0);
        free(p1);
        if (r > rc || (r == 1 && rc == 0)) rc = r;
    }
    for (int k = 0; k < c0; k++) free(v0[k]);
    for (int k = 0; k < c1; k++) free(v1[k]);
    free(v0);
    free(v1);
    return rc;
}

/* The "diff -r a/x b/x" line that precedes each file diff found inside a directory comparison. */
static char *dir_header(const char *n0, const char *n1) {
    size_t l = 4 + strlen(g_switches) + 1 + strlen(n0) + 1 + strlen(n1) + 1;
    char *h = malloc(l);
    if (!h) kb_die("out of memory");
    snprintf(h, l, "diff%s %s %s", g_switches, n0, n1);
    return h;
}

static int compare_paths(const char *n0, const char *n1, int absent0, int absent1, int top) {
    struct stat s0, s1;
    int d0 = 0, d1 = 0, rc;
    memset(&s0, 0, sizeof s0);
    memset(&s1, 0, sizeof s1);
    if (!absent0 && strcmp(n0, "-")) {
        if (stat(n0, &s0) < 0) {
            if (errno == ENOENT && o_newfile) absent0 = 1;
            else { out_flush(); kb_error("%s: %s", n0, strerror(errno)); return 2; }
        } else d0 = S_ISDIR(s0.st_mode);
    }
    if (!absent1 && strcmp(n1, "-")) {
        if (stat(n1, &s1) < 0) {
            if (errno == ENOENT && o_newfile) absent1 = 1;
            else { out_flush(); kb_error("%s: %s", n1, strerror(errno)); return 2; }
        } else d1 = S_ISDIR(s1.st_mode);
    }
    if (absent0 && absent1) return 0;
    if (!absent0 && !absent1 && !d0 && !d1 && S_ISREG(s0.st_mode) && S_ISREG(s1.st_mode) &&
        s0.st_dev == s1.st_dev && s0.st_ino == s1.st_ino && !o_blank_lines) {      /* the very same file */
        if (o_same) { out_str("Files "); out_str(n0); out_str(" and "); out_str(n1); out_str(" are identical\n"); }
        return 0;
    }
    if (top) {
        if (d0 && d1) return compare_dirs(n0, n1, 0, 0);
        if (d0 || d1) {                                 /* a file against a directory: the file of that name in the directory */
            const char *file = d0 ? n1 : n0, *dir = d0 ? n0 : n1, *base = strrchr(file, '/');
            char *both = kb_join(dir, base ? base + 1 : file);
            rc = d0 ? compare_paths(both, n1, 0, 0, 1) : compare_paths(n0, both, 0, 0, 1);
            free(both);
            return rc;
        }
    } else {
        if ((d0 || absent0) && (d1 || absent1) && (d0 || d1)) {
            if (o_recursive) return compare_dirs(n0, n1, absent0, absent1);
            out_str("Common subdirectories: "); out_str(n0); out_str(" and "); out_str(n1); out_str("\n");
            return 0;
        }
        if (d0 || d1) {
            out_str("File "); out_str(n0); out_str(" is a "); out_str(kind_name(s0.st_mode)); out_str(" while file ");
            out_str(n1); out_str(" is a "); out_str(kind_name(s1.st_mode)); out_str("\n");
            return 1;
        }
    }
    if (!top) {
        char *hdr = dir_header(n0, n1);
        rc = compare_texts(n0, absent0, n1, absent1, hdr);
        free(hdr);
        return rc;
    }
    return compare_texts(n0, absent0, n1, absent1, NULL);
}

APPLET(diff) {
    static const struct option longopts[] = {
        { "brief", no_argument, 0, 'q' }, { "recursive", no_argument, 0, 'r' }, { "new-file", no_argument, 0, 'N' },
        { "text", no_argument, 0, 'a' }, { "ignore-case", no_argument, 0, 'i' }, { "ignore-space-change", no_argument, 0, 'b' },
        { "ignore-all-space", no_argument, 0, 'w' }, { "ignore-blank-lines", no_argument, 0, 'B' },
        { "report-identical-files", no_argument, 0, 's' }, { "unified", optional_argument, 0, 'u' },
        { "context", optional_argument, 0, 'c' }, { "label", required_argument, 0, 'L' },
        { "strip-trailing-cr", no_argument, 0, 256 }, { "exclude", required_argument, 0, 'x' },
        { "minimal", no_argument, 0, 'd' }, { "normal", no_argument, 0, 257 }, { "speed-large-files", no_argument, 0, 'H' },
        { "help", no_argument, 0, 258 }, { 0, 0, 0, 0 }
    };
    int c, status = 0;
    optind = 0;
    while ((c = getopt_long(argc, argv, "abBc::C:dHiL:NqrsTtu::U:wx:", longopts, NULL)) != -1) {
        switch (c) {
        case 'a': o_text = 1; break;
        case 'b': if (o_ws < 1) o_ws = 1; break;
        case 'w': o_ws = 2; break;
        case 'B': o_blank_lines = 1; break;
        case 'i': o_icase = 1; break;
        case 'q': o_brief = 1; break;
        case 'r': o_recursive = 1; break;
        case 'N': o_newfile = 1; break;
        case 's': o_same = 1; break;
        case 'd': o_minimal = 1; break;
        case 'H': case 'T': case 't': break;
        case 'c': o_fmt = FMT_CONTEXT; if (optarg) o_ctx = atoi(optarg); break;
        case 'C': o_fmt = FMT_CONTEXT; o_ctx = atoi(optarg); break;
        case 'u': o_fmt = FMT_UNIFIED; if (optarg) o_ctx = atoi(optarg); break;
        case 'U': o_fmt = FMT_UNIFIED; o_ctx = atoi(optarg); break;
        case 257: o_fmt = FMT_NORMAL; break;
        case 256: o_strip_cr = 1; break;
        case 'L':
            if (o_nlabel < 2) o_label[o_nlabel++] = optarg;
            else { kb_error("too many file label options"); return 2; }
            break;
        case 'x':
            o_excl = realloc(o_excl, (size_t)(o_nexcl + 1) * sizeof *o_excl);
            if (!o_excl) kb_die("out of memory");
            o_excl[o_nexcl++] = optarg;
            break;
        case 258:
            printf("Usage: diff [OPTION]... FILES\nCompare FILES line by line. -u -c -q -r -N -s -a -b -w -i -B -x PAT --label L\n");
            return 0;
        default:
            fprintf(stderr, "diff: Try 'diff --help' for more information.\n");
            return 2;
        }
    }
    if (o_ctx < 0) o_ctx = 0;
    g_plain = !(o_icase || o_ws);
    for (int i = 1; i < optind; i++) {                  /* the options as typed, for the "diff -r a/x b/x" lines */
        const char *w = argv[i];
        int plain = *w != 0;
        char tmp[256];
        size_t n = 0;
        for (const char *s = w; *s; s++) if (!isalnum((unsigned char)*s) && !strchr("_./=:,+@%-", *s)) plain = 0;
        if (strlen(w) > 100) continue;
        if (!plain) {                                   /* quoted like a shell would, so the line can be pasted */
            tmp[n++] = '\'';
            for (const char *s = w; *s; s++) {
                if (*s == '\'') { memcpy(tmp + n, "'\\''", 4); n += 4; }
                else tmp[n++] = *s;
            }
            tmp[n++] = '\'';
            tmp[n] = 0;
            w = tmp;
        }
        if (strlen(g_switches) + strlen(w) + 2 < sizeof g_switches) {
            strcat(g_switches, " ");
            strcat(g_switches, w);
        }
    }
    if (argc - optind != 2) {
        if (argc - optind < 2) kb_error("missing operand after '%s'", argc > 1 ? argv[argc - 1] : "diff");
        else kb_error("extra operand '%s'", argv[optind + 2]);
        fprintf(stderr, "diff: Try 'diff --help' for more information.\n");
        return 2;
    }
    status = compare_paths(argv[optind], argv[optind + 1], 0, 0, 1);
    out_flush();
    return status;
}
