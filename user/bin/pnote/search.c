/* Search (^W), search again (^N / ^P) and search-and-replace (^R).
 * Matching is per line. Plain-text patterns are smartcase: case is ignored unless the pattern has an upper case letter.
 * With regular expressions switched on (Alt-R at the prompt) the pattern is an extended regular expression. */
#include "pnote.h"

typedef struct {
    regex_t re;
    int use_re, icase;
    const char *pat;
    size_t plen;
    regmatch_t g[10];               /* groups of the last regex match, offsets into the line */
    int ngroups;
} Matcher;

static PtBuf g_scratch;

static int has_upper(const char *s) {
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') return 1;
    return 0;
}

static int matcher_init(Matcher *m, const char *pat) {
    memset(m, 0, sizeof *m);
    m->pat = pat;
    m->plen = strlen(pat);
    m->icase = E.s_case == 2 || (E.s_case == 0 && !has_upper(pat));
    if (E.s_regex) {
        if (regcomp(&m->re, pat, REG_EXTENDED | (m->icase ? REG_ICASE : 0)) != 0) {
            set_msg("Invalid regular expression: %s", pat);
            return -1;
        }
        m->use_re = 1;
    }
    return 0;
}

static void matcher_free(Matcher *m) { if (m->use_re) regfree(&m->re); }

static const char *find_sub(const char *h, size_t hn, const char *nd, size_t nn, int icase) {
    if (nn > hn) return NULL;
    unsigned char first = (unsigned char)nd[0];
    for (size_t i = 0; i + nn <= hn; i++) {
        unsigned char c = (unsigned char)h[i];
        if (icase ? tolower(c) != tolower(first) : c != first) continue;
        size_t k = 1;
        if (icase) { while (k < nn && tolower((unsigned char)h[i + k]) == tolower((unsigned char)nd[k])) k++; }
        else { while (k < nn && h[i + k] == nd[k]) k++; }
        if (k == nn) return h + i;
    }
    return NULL;
}

/* First non-empty match in s[0..n) starting at or after byte `from`. */
static int matcher_find(Matcher *m, const char *s, size_t n, size_t from, size_t *ms, size_t *me) {
    if (from > n) return 0;
    if (!m->plen) return 0;
    if (!m->use_re) {
        const char *hit = find_sub(s + from, n - from, m->pat, m->plen, m->icase);
        if (!hit) return 0;
        *ms = (size_t)(hit - s);
        *me = *ms + m->plen;
        return 1;
    }
    pb_clear(&g_scratch);
    pb_add(&g_scratch, s, n);
    const char *text = g_scratch.p;
    size_t pos = from;
    while (pos <= n) {
        regmatch_t mm[10];
        if (regexec(&m->re, text + pos, 10, mm, pos ? REG_NOTBOL : 0) != 0) return 0;
        size_t a = pos + (size_t)mm[0].rm_so, b = pos + (size_t)mm[0].rm_eo;
        if (b > a) {
            *ms = a;
            *me = b;
            for (int i = 0; i < 10; i++) {
                if (mm[i].rm_so < 0) { m->g[i].rm_so = m->g[i].rm_eo = -1; }
                else { m->g[i].rm_so = (regoff_t)(pos + (size_t)mm[i].rm_so); m->g[i].rm_eo = (regoff_t)(pos + (size_t)mm[i].rm_eo); }
            }
            m->ngroups = (int)m->re.re_nsub;
            return 1;
        }
        pos = a + 1;                                    /* skip an empty match */
    }
    return 0;
}

/* The last match in the line that starts before `limit`. */
static int matcher_find_last(Matcher *m, const char *s, size_t n, size_t limit, size_t *ms, size_t *me) {
    size_t from = 0, a, b;
    int found = 0;
    while (matcher_find(m, s, n, from, &a, &b) && a < limit) {
        *ms = a;
        *me = b;
        found = 1;
        from = a + 1;
    }
    return found;
}

static void show_match(long ln, size_t a, size_t b) {
    E.cy = ln;
    E.cx = a;
    E.mh_on = 1;
    E.mh_ln = ln;
    E.mh_c0 = a;
    E.mh_c1 = b;
    remember_col();
}

/* Looks for the next match forward from (ln, col), wrapping once around the end of the buffer. */
static int find_forward(Matcher *m, long ln, size_t col, long *rl, size_t *ra, size_t *rb, int *wrapped) {
    *wrapped = 0;
    for (long i = 0; i <= E.n; i++) {
        long li = ln + i;
        if (li >= E.n) { li -= E.n; *wrapped = 1; }
        if (li >= E.n) break;
        Line *L = LN(li);
        size_t from = i == 0 ? col : 0;
        if (matcher_find(m, L->s, L->len, from, ra, rb)) { *rl = li; return 1; }
    }
    return 0;
}

static int find_backward(Matcher *m, long ln, size_t col, long *rl, size_t *ra, size_t *rb, int *wrapped) {
    *wrapped = 0;
    for (long i = 0; i <= E.n; i++) {
        long li = ln - i;
        if (li < 0) { li += E.n; *wrapped = 1; }
        if (li < 0) break;
        Line *L = LN(li);
        size_t limit = i == 0 ? col : L->len + 1;
        if (matcher_find_last(m, L->s, L->len, limit, ra, rb)) { *rl = li; return 1; }
    }
    return 0;
}

static void run_search(Matcher *m, int dir) {
    long ln;
    size_t a, b;
    int wrapped;
    int ok;
    if (dir > 0) {
        Line *L = LN(E.cy);
        size_t from = E.cx < L->len ? E.cx + (size_t)pt_u8_next(L->s, L->len, E.cx) : L->len;
        ok = find_forward(m, E.cy, from, &ln, &a, &b, &wrapped);
    } else {
        ok = find_backward(m, E.cy, E.cx, &ln, &a, &b, &wrapped);
    }
    if (!ok) { set_msg("\"%s\" not found", m->pat); return; }
    show_match(ln, a, b);
    if (wrapped) set_msg("Search wrapped around the %s of the notebook", dir > 0 ? "end" : "start");
}

/* ------------------------------------------------------------------ prompts */

static void search_label(char *label, size_t cap, const char *what) {
    const char *flags = E.s_regex ? (E.s_case == 1 ? " (regex, case)" : E.s_case == 2 ? " (regex, nocase)" : " (regex)")
                                  : (E.s_case == 1 ? " (case)" : E.s_case == 2 ? " (nocase)" : "");
    if (E.sbuf[0]) snprintf(label, cap, "%s%s [%.60s]: ", what, flags, E.sbuf);
    else snprintf(label, cap, "%s%s: ", what, flags);
}

static const char *g_what = "Search";

static int search_onkey(int k, char *label, size_t cap) {
    if (k == ('c' | PT_M_ALT)) { E.s_case = (E.s_case + 1) % 3; search_label(label, cap, g_what); return 1; }
    if (k == ('r' | PT_M_ALT)) { E.s_regex = !E.s_regex; search_label(label, cap, g_what); return 1; }
    return 0;
}

/* Asks for a pattern; returns 1 and stores it in E.sbuf, or 0 if cancelled. */
static int ask_pattern(const char *what) {
    char label[160], text[256] = "";
    g_what = what;
    search_label(label, sizeof label, what);
    if (!prompt_line(label, sizeof label, text, sizeof text, search_onkey)) return 0;
    if (text[0]) snprintf(E.sbuf, sizeof E.sbuf, "%s", text);
    return E.sbuf[0] != 0;
}

void cmd_search(void) {
    if (!ask_pattern("Search")) return;
    Matcher m;
    if (matcher_init(&m, E.sbuf) < 0) return;
    run_search(&m, 1);
    matcher_free(&m);
}

void cmd_search_again(int dir) {
    if (!E.sbuf[0]) { set_msg("No search has been made yet (^W)"); return; }
    Matcher m;
    if (matcher_init(&m, E.sbuf) < 0) return;
    run_search(&m, dir);
    matcher_free(&m);
}

/* ------------------------------------------------------------------ replace */

/* Builds the replacement for a match; \n \t \\ always work, & and \1..\9 only in regular expression mode. */
static void build_replacement(PtBuf *out, Matcher *m, const char *line, const char *rep, int cr) {
    pb_clear(out);
    for (const char *p = rep; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') { if (cr) pb_addc(out, '\r'); pb_addc(out, '\n'); }
            else if (*p == 't') pb_addc(out, '\t');
            else if (*p == '\\') pb_addc(out, '\\');
            else if (*p == '&') pb_addc(out, '&');
            else if (m->use_re && *p >= '0' && *p <= '9') {
                int g = *p - '0';
                if (g <= m->ngroups && m->g[g].rm_so >= 0) pb_add(out, line + m->g[g].rm_so, (size_t)(m->g[g].rm_eo - m->g[g].rm_so));
            } else { pb_addc(out, '\\'); pb_addc(out, *p); }
        } else if (*p == '&' && m->use_re) {
            pb_add(out, line + m->g[0].rm_so, (size_t)(m->g[0].rm_eo - m->g[0].rm_so));
        } else pb_addc(out, *p);
    }
}

void cmd_replace(void) {
    if (!edit_allowed()) return;
    if (!ask_pattern("Search for")) return;
    char label[160] = "Replace with: ", rtext[256] = "";
    snprintf(rtext, sizeof rtext, "%s", E.rbuf);
    if (!prompt_line(label, sizeof label, rtext, sizeof rtext, NULL)) return;
    snprintf(E.rbuf, sizeof E.rbuf, "%s", rtext);
    Matcher m;
    if (matcher_init(&m, E.sbuf) < 0) return;

    long stop_ln = E.cy;
    size_t stop_col = E.cx;
    int phase = 1, all = 0, done = 0;
    long count = 0, ln = E.cy;
    size_t col = E.cx;
    PtBuf rep = { 0 };
    undo_boundary();
    while (!done) {
        size_t a = 0, b = 0;
        int found = 0;
        while (!found) {
            if (ln >= E.n) {
                if (phase == 1 && (stop_ln > 0 || stop_col > 0)) { phase = 2; ln = 0; col = 0; if (ln >= E.n) break; }
                else break;
            }
            if (phase == 2 && ln > stop_ln) break;
            Line *L = LN(ln);
            if (matcher_find(&m, L->s, L->len, col, &a, &b)) {
                if (phase == 2 && ln == stop_ln && a >= stop_col) break;
                found = 1;
            } else { ln++; col = 0; }
        }
        if (!found) break;
        Line *L = LN(ln);
        if (!all) {
            show_match(ln, a, b);
            int k = ask_key("Replace this instance?  Y yes  N no  A all  Esc stop", "yna");
            if (k == 0) break;
            if (k == 'n') { col = b > a ? b : a + 1; continue; }
            if (k == 'a') all = 1;
            undo_boundary();
        }
        E.mh_on = 0;
        build_replacement(&rep, &m, L->s, E.rbuf, L->cr);
        long eln;
        size_t ecol;
        free(edit_delete(ln, a, ln, b, NULL, 0));
        edit_insert(ln, a, rep.p, rep.len, &eln, &ecol, 0);
        count++;
        int rep_nl = 0;
        for (size_t i = 0; i < rep.len; i++) if (rep.p[i] == '\n') rep_nl++;
        if (phase == 2) {                               /* keep the stopping point in step with the changes */
            if (ln == stop_ln) {
                if (rep_nl) done = 1;
                else {
                    long sc = (long)stop_col + (long)rep.len - (long)(b - a);
                    stop_col = sc < 0 ? 0 : (size_t)sc;
                }
            } else if (rep_nl) stop_ln += rep_nl;
        }
        ln = eln;
        col = ecol;
        E.cy = ln;
        E.cx = col;
    }
    pb_free(&rep);
    matcher_free(&m);
    E.mh_on = 0;
    clamp_cursor();
    remember_col();
    if (count) set_msg("Replaced %ld occurrence%s", count, count == 1 ? "" : "s");
    else set_msg("\"%s\" not found", E.sbuf);
}
