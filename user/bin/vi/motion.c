/* motion.c: cursor motions, text objects and searching. */
#include "vi.h"

char last_find_char[8];
int last_find_cmd, last_find_len;
char last_search[256];
int last_search_dir = 1;
static char last_search_off[32];

/* ------------------------------------------------------------------ characters and columns */

uint32_t cp_at(long l, int c) {
    if (c >= (int)L[l].len) return 0;
    uint32_t cp;
    pt_u8_decode((const unsigned char *)ls(l) + c, L[l].len - (size_t)c, &cp);
    return cp;
}

int next_c(long l, int c) {
    int len = (int)L[l].len;
    if (c >= len) return len;
    return c + pt_u8_next(ls(l), (size_t)len, (size_t)c);
}

int prev_c(long l, int c) {
    if (c <= 0) return 0;
    if (c > (int)L[l].len) c = (int)L[l].len;
    return (int)pt_u8_prev(ls(l), (size_t)c);
}

int last_c(long l) { return L[l].len ? prev_c(l, (int)L[l].len) : 0; }

int first_nonblank(long l) {
    const char *s = ls(l);
    int len = (int)L[l].len, i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= len) return len ? last_c(l) : 0;
    return i;
}

int clamp_col(long l, int c, int allow_end) {
    int len = (int)L[l].len;
    if (c < 0) c = 0;
    if (c >= len) return allow_end ? len : (len ? last_c(l) : 0);
    const char *s = ls(l);
    while (c > 0 && ((unsigned char)s[c] & 0xC0) == 0x80 && cp_at(l, c) >= PT_RAWBYTE_BASE + 0x80 - 0x80 && 0) c--;
    /* step back to the start of the code point (raw bytes are single-byte code points) */
    int j = c;
    while (j > 0 && c - j < 3 && ((unsigned char)s[j] & 0xC0) == 0x80) j--;
    if (j != c) {
        uint32_t cp;
        int n = pt_u8_decode((const unsigned char *)s + j, (size_t)(len - j), &cp);
        if (j + n > c) c = j;
    }
    return c;
}

int cls(uint32_t cp, int big) {
    if (cp == 0 || cp == ' ' || cp == '\t') return 0;
    if (big) return 1;
    return pt_is_word_cp(cp) ? 2 : 1;
}

void push_jump(void) { mark_ctx = cur; }

/* ------------------------------------------------------------------ word motions (after Vim's search.c) */

static Pos w;                                            /* the position being moved */
static int wbig;

static int wcls(void) { return cls(cp_at(w.l, w.c), wbig); }

/* Moves one character forward. 0: moved, 2: moved onto the end of the line, 1: moved to the next line, -1: at the end. */
static int winc(void) {
    int len = (int)L[w.l].len;
    if (w.c < len) {
        w.c = next_c(w.l, w.c);
        return w.c < len ? 0 : 2;
    }
    if (w.l < nl - 1) { w.l++; w.c = 0; return 1; }
    return -1;
}

static int wdec(void) {
    if (w.c > 0) { w.c = prev_c(w.l, w.c); return 0; }
    if (w.l > 0) { w.l--; w.c = (int)L[w.l].len; return 1; }
    return -1;
}

static int skip_chars(int cclass, int fwd) {
    while (wcls() == cclass) if ((fwd ? winc() : wdec()) == -1) return 1;
    return 0;
}

static int fwd_word(int count, int eol) {
    while (--count >= 0) {
        int sclass = wcls();
        int last_line = w.l == nl - 1;
        int i = winc();
        if (i == -1 || (i >= 1 && last_line)) return 0;
        if (i >= 1 && eol && count == 0) return 1;
        if (sclass != 0) {
            while (wcls() == sclass) {
                i = winc();
                if (i == -1 || (i >= 1 && eol && count == 0)) return 1;
            }
        }
        while (wcls() == 0) {
            if (w.c == 0 && L[w.l].len == 0) break;
            i = winc();
            if (i == -1 || (i >= 1 && eol && count == 0)) return 1;
        }
    }
    return 1;
}

static int bck_word(int count, int stop) {
    while (--count >= 0) {
        int sclass = wcls();
        if (wdec() == -1) return 0;
        int skip = 1;
        if (!stop || sclass == wcls() || sclass == 0) {
            int finished = 0;
            while (wcls() == 0) {
                if (w.c == 0 && L[w.l].len == 0) { finished = 1; break; }
                if (wdec() == -1) return 1;
            }
            if (!finished) {
                if (skip_chars(wcls(), 0)) return 1;
            } else skip = 0;
        }
        if (skip) winc();
        stop = 0;
    }
    return 1;
}

static int end_word(int count, int stop, int empty) {
    while (--count >= 0) {
        int sclass = wcls();
        if (winc() == -1) return 0;
        int finished = 0;
        if (wcls() == sclass && sclass != 0) {
            if (skip_chars(sclass, 1)) return 0;
        } else if (!stop || sclass == 0) {
            while (wcls() == 0) {
                if (empty && w.c == 0 && L[w.l].len == 0) { finished = 1; break; }
                if (winc() == -1) return 0;
            }
            if (!finished && skip_chars(wcls(), 1)) return 0;
        }
        if (!finished) wdec();
        stop = 0;
    }
    return 1;
}

static int bckend_word(int count, int eol) {
    while (--count >= 0) {
        int sclass = wcls();
        int i = wdec();
        if (i == -1) return 0;
        if (eol && i == 1) return 1;
        if (sclass != 0) {
            while (wcls() == sclass) if ((i = wdec()) == -1 || (eol && i == 1)) return 1;
        }
        while (wcls() == 0) {
            if (w.c == 0 && L[w.l].len == 0) break;
            if ((i = wdec()) == -1 || (eol && i == 1)) return 1;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ patterns */

static int has_upper(const char *p) {
    for (; *p; p++) {
        if (*p == '\\' && p[1]) { p++; continue; }
        if (isupper((unsigned char)*p)) return 1;
    }
    return 0;
}

int re_ic(const char *pat) {
    if (!o.ic) return 0;
    if (o.scs && has_upper(pat)) return 0;
    return 1;
}

/* Vim's magic patterns are close to POSIX BREs (the regex engine has \( \) \| \+ \? \< \> \w \s \d); what differs is
 * \{n,m} without the backslash on the closing brace, \= and \c. */
int re_compile(regex_t *re, const char *pat, int ic_override) {
    PtBuf b = { 0 };
    int ic = ic_override >= 0 ? ic_override : re_ic(pat);
    for (const char *p = pat; *p; p++) {
        if (*p == '\\' && p[1]) {
            char e = p[1];
            if (e == '=') { pb_adds(&b, "\\?"); p++; }
            else if (e == 'c') { ic = 1; p++; }
            else if (e == 'C') { ic = 0; p++; }
            else if (e == '/') { pb_addc(&b, '/'); p++; }
            else if (e == '{') {
                pb_adds(&b, "\\{");
                p += 2;
                while (*p && *p != '}' && !(*p == '\\' && p[1] == '}')) pb_addc(&b, *p++);
                pb_adds(&b, "\\}");
                if (*p == '\\') p++;
                if (!*p) break;
            } else { pb_addc(&b, '\\'); pb_addc(&b, e); p++; }
        } else if (*p == '[') {                          /* copy a bracket expression as it is */
            const char *q = p + 1;
            if (*q == '^') q++;
            if (*q == ']') q++;
            while (*q && *q != ']') {
                if (*q == '[' && (q[1] == ':' || q[1] == '.' || q[1] == '=')) {
                    char t = q[1];
                    const char *e = strstr(q + 2, (char[]){ t, ']', 0 });
                    if (e) { q = e + 2; continue; }
                }
                q++;
            }
            if (*q == ']') { pb_add(&b, p, (size_t)(q - p + 1)); p = q; }
            else pb_adds(&b, "\\[");
        } else pb_addc(&b, *p);
    }
    int rc = regcomp(re, b.p ? b.p : "", ic ? REG_ICASE : 0);
    pb_free(&b);
    return rc;
}

int re_exec_at(regex_t *re, const char *s, size_t len, size_t start, regmatch_t *m, size_t nm) {
    regmatch_t local[10];
    regmatch_t *mm = m ? m : local;
    size_t n = m ? nm : 10;
    mm[0].rm_so = (regoff_t)start;
    mm[0].rm_eo = (regoff_t)len;
    return regexec(re, s, n, mm, REG_STARTEND);
}

/* The first match in line l that starts at or after byte `from`; fills so/eo. */
static int line_match_from(regex_t *re, long l, int from, int *so, int *eo) {
    regmatch_t m[1];
    if (re_exec_at(re, ls(l), L[l].len, (size_t)from, m, 1) != 0) return 0;
    *so = (int)m[0].rm_so;
    *eo = (int)m[0].rm_eo;
    return 1;
}

/* The last match in line l that starts before byte `before`. */
static int line_match_last(regex_t *re, long l, int before, int *so, int *eo) {
    int pos = 0, s, e, found = 0;
    while (pos <= (int)L[l].len && line_match_from(re, l, pos, &s, &e)) {
        if (s >= before) break;
        *so = s;
        *eo = e;
        found = 1;
        pos = s + 1 > e ? next_c(l, s) + (s >= (int)L[l].len ? 1 : 0) : (e > s ? next_c(l, s) : e);
        if (s >= (int)L[l].len) break;
    }
    return found;
}

/* Searches for pat from `from` (not matching at `from` itself unless accept_at). Returns 1 and the match start in *out. */
static int last_match_end_c;
int search_find(const char *pat, int dir, Pos from, Pos *out, int wrap, int accept_at) {
    regex_t re;
    if (re_compile(&re, pat, -1) != 0) { set_err("E486: Invalid pattern: %s", pat); return -1; }
    int found = 0, so, eo;
    long l = from.l;
    for (long step = 0; step <= nl; step++) {
        if (dir > 0) {
            int start = 0;
            if (step == 0) start = accept_at ? from.c : (from.c >= (int)L[l].len ? (int)L[l].len + 1 : next_c(l, from.c));
            else if (step == nl) { /* back at the first line: only matches before the start */ }
            if (start <= (int)L[l].len && line_match_from(&re, l, start, &so, &eo)) {
                if (step < nl || so <= from.c) { found = 1; break; }
            }
        } else {
            int before = (int)L[l].len + 1;
            if (step == 0) before = from.c;
            else if (step == nl) before = (int)L[l].len + 1;
            if ((step > 0 || before > 0) && line_match_last(&re, l, before, &so, &eo)) {
                if (step < nl || so >= from.c) { found = 1; break; }
            }
        }
        if (step == nl) break;
        long nxt = l + dir;
        if (nxt >= nl || nxt < 0) {
            if (!wrap) break;
            nxt = dir > 0 ? 0 : nl - 1;
            set_msg("search hit %s, continuing at %s", dir > 0 ? "BOTTOM" : "TOP", dir > 0 ? "TOP" : "BOTTOM");
        }
        l = nxt;
    }
    regfree(&re);
    if (!found) return 0;
    out->l = l;
    out->c = so;
    last_match_end_c = eo;
    return 1;
}

void re_clear(void) {}

/* Parses "pattern[/offset]" for a search in direction dir (delimiter ch), runs it cnt times from `from`. */
static int do_search(const char *text, int dir, int cnt, Pos from, Motion *m, int *op_incl) {
    char delim = dir > 0 ? '/' : '?';
    char pat[256];
    size_t n = 0;
    const char *p = text;
    while (*p && *p != delim) {
        if (*p == '\\' && p[1]) { if (p[1] != delim || delim == '?') { if (n < sizeof pat - 2) pat[n++] = *p; } p++; }
        if (n < sizeof pat - 1) pat[n++] = *p;
        p++;
    }
    pat[n] = 0;
    const char *off = "";
    if (*p == delim) off = p + 1;
    if (!pat[0]) {
        if (!last_search[0]) { set_err("E35: No previous regular expression"); m->fail = 1; return 0; }
        snprintf(pat, sizeof pat, "%s", last_search);
    } else {
        snprintf(last_search, sizeof last_search, "%s", pat);
    }
    snprintf(last_search_off, sizeof last_search_off, "%s", off);
    last_search_dir = dir;
    Pos pos = from;
    for (int i = 0; i < cnt; i++) {
        Pos nx;
        int r = search_find(pat, dir, pos, &nx, o.ws, 0);
        if (r <= 0) {
            if (r == 0) set_err("E486: Pattern not found: %s", pat);
            m->fail = 1;
            return 0;
        }
        pos = nx;
    }
    m->to = pos;
    m->jump = 1;
    /* offsets: e[+-N], s/b[+-N], [+-]N lines */
    const char *q = off;
    if (*q == 'e' || *q == 's' || *q == 'b') {
        char k = *q++;
        long amt = 0;
        int sign = 1;
        if (*q == '+' || *q == '-') { sign = *q == '-' ? -1 : 1; q++; amt = isdigit((unsigned char)*q) ? strtol(q, NULL, 10) : 1; }
        regex_t re;
        if (k == 'e' && re_compile(&re, pat, -1) == 0) {
            int s2, e2;
            if (line_match_from(&re, pos.l, pos.c, &s2, &e2)) m->to.c = e2 > s2 ? prev_c(pos.l, e2) : s2;
            regfree(&re);
            *op_incl = 1;
        }
        for (long i = 0; i < amt; i++) m->to.c = sign > 0 ? next_c(m->to.l, m->to.c) : prev_c(m->to.l, m->to.c);
    } else if (*q == '+' || *q == '-' || isdigit((unsigned char)*q)) {
        long amt = *q == '+' || *q == '-' ? (q[1] ? strtol(q, NULL, 10) : (*q == '-' ? -1 : 1)) : strtol(q, NULL, 10);
        m->to.l = imin(nl - 1, imax(0, m->to.l + amt));
        m->to.c = first_nonblank(m->to.l);
        m->linewise = 1;
    }
    return 1;
}

/* ------------------------------------------------------------------ brackets, sentences, paragraphs */

static int is_open(int ch) { return ch == '(' || ch == '[' || ch == '{'; }
static int is_close(int ch) { return ch == ')' || ch == ']' || ch == '}'; }
static int partner(int ch) {
    switch (ch) { case '(': return ')'; case ')': return '('; case '[': return ']'; case ']': return '['; case '{': return '}'; case '}': return '{'; }
    return 0;
}

/* Finds the bracket matching the one at p. Returns 1 and sets *out. */
static int match_pair(Pos p, Pos *out) {
    int ch = (unsigned char)ls(p.l)[p.c];
    int want = partner(ch), fwd = is_open(ch), depth = 0;
    Pos q = p;
    for (;;) {
        if (fwd) {
            q.c++;
            while (q.c >= (int)L[q.l].len) { if (q.l + 1 >= nl) return 0; q.l++; q.c = 0; if (L[q.l].len) break; }
        } else {
            q.c--;
            while (q.c < 0) { if (q.l == 0) return 0; q.l--; q.c = (int)L[q.l].len - 1; }
        }
        if (q.c < 0 || q.c >= (int)L[q.l].len) continue;
        int x = (unsigned char)ls(q.l)[q.c];
        if (x == ch) depth++;
        else if (x == want) { if (depth == 0) { *out = q; return 1; } depth--; }
    }
}

/* Finds the cnt-th unmatched `open` before p (or at p). */
static int find_enclosing(Pos p, int open, int close, int cnt, Pos *out, Pos *outc) {
    Pos q = p;
    int depth = 0;
    int ch = q.c < (int)L[q.l].len ? (unsigned char)ls(q.l)[q.c] : 0;
    if (ch == open) { depth = 0; if (--cnt == 0) { *out = q; goto found; } }
    else if (ch == close) { depth = 1; }
    for (;;) {
        q.c--;
        while (q.c < 0) { if (q.l == 0) return 0; q.l--; q.c = (int)L[q.l].len - 1; }
        int x = (unsigned char)ls(q.l)[q.c];
        if (x == close) depth++;
        else if (x == open) { if (depth == 0) { if (--cnt == 0) { *out = q; break; } } else depth--; }
    }
found:;
    Pos e;
    Pos start = *out;
    int d = 0;
    e = start;
    for (;;) {
        e.c++;
        while (e.c >= (int)L[e.l].len) { if (e.l + 1 >= nl) return 0; e.l++; e.c = 0; if (L[e.l].len) break; }
        int x = (unsigned char)ls(e.l)[e.c];
        if (x == open) d++;
        else if (x == close) { if (d == 0) { *outc = e; return 1; } d--; }
    }
}

static int blank_line(long l) {
    const char *s = ls(l);
    for (unsigned i = 0; i < L[l].len; i++) if (s[i] != ' ' && s[i] != '\t') return 0;
    return 1;
}

/* Sentence boundaries: the start of a sentence is the first non-blank after ". " "! " "? " (closing quotes and brackets may
 * follow the dot) or after an empty line; an empty line is a sentence of its own. */
static int sentence_start_at(long l, int c) {
    const char *s = ls(l);
    int len = (int)L[l].len;
    if (len == 0) return 1;
    if (c >= len || s[c] == ' ' || s[c] == '\t') return 0;
    int i = c;
    while (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t')) i--;
    if (i == 0) {
        if (l == 0) return 1;
        if (blank_line(l - 1)) return 1;
        const char *p = ls(l - 1);
        int pl = (int)L[l - 1].len;
        int j = pl;
        while (j > 0 && (p[j - 1] == ' ' || p[j - 1] == '\t')) j--;
        while (j > 0 && strchr(")]\"'", p[j - 1])) j--;
        return j > 0 && strchr(".!?", p[j - 1]) != NULL;
    }
    if (i == c) return 0;                                /* no white space before it */
    int j = i;
    while (j > 0 && strchr(")]\"'", s[j - 1])) j--;
    return j > 0 && strchr(".!?", s[j - 1]) != NULL;
}

static int sentence_fwd(Pos *p, int cnt) {
    Pos q = *p;
    while (cnt-- > 0) {
        for (;;) {
            q.c = next_c(q.l, q.c);
            if (q.c >= (int)L[q.l].len) {
                if (q.l + 1 >= nl) { q.c = last_c(q.l); *p = q; return 1; }
                q.l++; q.c = 0;
            }
            if (sentence_start_at(q.l, q.c)) break;
            if (L[q.l].len == 0 && q.l > 0 && !blank_line(q.l - 1)) break;
        }
    }
    *p = q;
    return 1;
}

static int sentence_bck(Pos *p, int cnt) {
    Pos q = *p;
    while (cnt-- > 0) {
        for (;;) {
            if (q.c > 0) q.c = prev_c(q.l, q.c);
            else if (q.l > 0) { q.l--; q.c = last_c(q.l); }
            else { *p = q; return 1; }
            if (sentence_start_at(q.l, q.c)) break;
        }
    }
    *p = q;
    return 1;
}

/* ------------------------------------------------------------------ the motion table */

static int visible_last(void) {
    int th = text_rows(), rows = 0;
    long l = topline;
    while (l < nl) {
        int rc = 1;
        if (o.wrap) { int wd = line_vwidth(l) + (o.list ? 1 : 0), a = text_cols(); rc = wd <= a ? 1 : (wd + a - 1) / a; }
        if (rows + rc > th && l > topline) break;
        rows += rc;
        l++;
    }
    return (int)(l - 1);
}

static int find_char(Pos *p, int cmd, const char *ch, int chlen, int cnt, int repeat) {
    int fwd = cmd == 'f' || cmd == 't';
    int till = cmd == 't' || cmd == 'T';
    int c = p->c, len = (int)L[p->l].len;
    const char *s = ls(p->l);
    for (int i = 0; i < cnt; i++) {
        int pos = c;
        if (till && repeat && i == 0) {                  /* ;/, after t/T must not stay stuck before the character */
            int adj = fwd ? next_c(p->l, c) : prev_c(p->l, c);
            if (adj != c && adj + chlen <= len && !memcmp(s + adj, ch, (size_t)chlen) && fwd) pos = adj;
            else if (adj != c && !fwd && adj >= 0 && adj + chlen <= len && !memcmp(s + adj, ch, (size_t)chlen)) pos = adj;
        }
        int found = -1;
        if (fwd) {
            for (int k = next_c(p->l, pos); k < len; k = next_c(p->l, k)) if (k + chlen <= len && !memcmp(s + k, ch, (size_t)chlen)) { found = k; break; }
        } else {
            for (int k = pos; k > 0;) { k = prev_c(p->l, k); if (k + chlen <= len && !memcmp(s + k, ch, (size_t)chlen)) { found = k; break; } }
        }
        if (found < 0) return 0;
        c = found;
    }
    if (till) c = fwd ? prev_c(p->l, c) : next_c(p->l, c);
    p->c = c;
    return 1;
}

/* The keyword under or after the cursor (else the non-blank string); *start is its first byte. */
static void word_under_cursor(char *buf, size_t cap, int *is_word, int *start) {
    const char *s = ls(cur.l);
    int len = (int)L[cur.l].len, c = cur.c;
    *is_word = 1;
    *start = c;
    int i = c;
    while (i < len && !pt_is_word_cp(cp_at(cur.l, i))) i = next_c(cur.l, i);
    if (i >= len) {                                      /* no word characters ahead: use the non-blank run */
        *is_word = 0;
        i = c;
        while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i >= len) { buf[0] = 0; return; }
        int e = i;
        while (e < len && s[e] != ' ' && s[e] != '\t') e++;
        size_t n = (size_t)(e - i) < cap - 1 ? (size_t)(e - i) : cap - 1;
        memcpy(buf, s + i, n);
        buf[n] = 0;
        *start = i;
        return;
    }
    int st = i;
    while (st > 0 && pt_is_word_cp(cp_at(cur.l, prev_c(cur.l, st)))) st = prev_c(cur.l, st);
    int e = i;
    while (e < len && pt_is_word_cp(cp_at(cur.l, e))) e = next_c(cur.l, e);
    size_t n = (size_t)(e - st) < cap - 1 ? (size_t)(e - st) : cap - 1;
    memcpy(buf, s + st, n);
    buf[n] = 0;
    *start = st;
}

int motion(int c, int cnt, int hascnt, int op, Motion *m) {
    memset(m, 0, sizeof *m);
    m->to = cur;
    m->vcol = -1;
    int n1 = cnt > 0 ? cnt : 1;
    Pos p = cur;
    switch (c) {
    case PT_K_LEFT: case 'h':
        if (p.c <= 0) { m->fail = 1; return 1; }
        for (int i = 0; i < n1 && p.c > 0; i++) p.c = prev_c(p.l, p.c);
        m->to = p;
        return 1;
    case K_BS: case 8:                                   /* <BS> also wraps to the end of the previous line ('whichwrap' has b) */
        for (int i = 0; i < n1; i++) {
            if (p.c > 0) p.c = prev_c(p.l, p.c);
            else if (p.l > 0 && !op) { p.l--; p.c = last_c(p.l); }
            else { if (i == 0) m->fail = 1; break; }
        }
        m->to = p;
        return 1;
    case PT_K_RIGHT: case 'l': {
        int len = (int)L[p.l].len;
        int limit = op ? len : (len ? last_c(p.l) : 0);
        if (p.c >= limit) { m->fail = 1; return 1; }
        for (int i = 0; i < n1 && p.c < limit; i++) p.c = next_c(p.l, p.c);
        m->to = p;
        return 1;
    }
    case ' ': {
        for (int i = 0; i < n1; i++) {
            if (p.c < (int)L[p.l].len - 1 + (op ? 1 : 0) && next_c(p.l, p.c) < (int)L[p.l].len + (op ? 1 : 0)) { if (next_c(p.l, p.c) >= (int)L[p.l].len && !op) { if (p.l + 1 < nl) { p.l++; p.c = 0; } else { if (i == 0) m->fail = 1; break; } } else p.c = next_c(p.l, p.c); }
            else if (p.l + 1 < nl) { p.l++; p.c = 0; }
            else { if (i == 0 && !op) m->fail = 1; if (op) p.c = (int)L[p.l].len; break; }
        }
        m->to = p;
        return 1;
    }
    case PT_K_UP: case 'k': case 16:
        if (p.l == 0) { m->fail = 1; return 1; }
        p.l = imax(0, p.l - n1);
        m->linewise = 1;
        m->keepcol = 1;
        p.c = byte_at_vcol(p.l, curswant < 0 ? 100000000 : curswant);
        m->to = p;
        return 1;
    case PT_K_DOWN: case 'j': case 14: case 10:
        if (p.l >= nl - 1) { m->fail = 1; return 1; }
        p.l = imin(nl - 1, p.l + n1);
        m->linewise = 1;
        m->keepcol = 1;
        p.c = byte_at_vcol(p.l, curswant < 0 ? 100000000 : curswant);
        m->to = p;
        return 1;
    case '0': case PT_K_HOME:
        p.c = 0; m->to = p; return 1;
    case '^':
        p.c = first_nonblank(p.l); m->to = p; return 1;
    case '$': case PT_K_END:
        if (p.l + n1 - 1 >= nl) { m->fail = 1; return 1; }
        p.l += n1 - 1;
        p.c = L[p.l].len ? (op ? last_c(p.l) : last_c(p.l)) : 0;
        m->incl = 1;
        m->vcol = -2;
        m->to = p;
        return 1;
    case '|':
        p.c = byte_at_vcol(p.l, n1 - 1);
        p.c = clamp_col(p.l, p.c, 0);
        m->to = p;
        return 1;
    case '_':
        if (p.l + n1 - 1 >= nl) { m->fail = 1; return 1; }
        p.l += n1 - 1; p.c = first_nonblank(p.l); m->linewise = 1; m->to = p; return 1;
    case '+': case K_ENTER:
        if (p.l + n1 >= nl) { m->fail = 1; return 1; }
        p.l += n1; p.c = first_nonblank(p.l); m->linewise = 1; m->to = p; return 1;
    case '-':
        if (p.l - n1 < 0) { m->fail = 1; return 1; }
        p.l -= n1; p.c = first_nonblank(p.l); m->linewise = 1; m->to = p; return 1;
    case 'G':
        p.l = hascnt ? imin(cnt, nl) - 1 : nl - 1;
        p.c = first_nonblank(p.l);
        m->linewise = m->jump = 1;
        m->to = p;
        return 1;
    case 'H': case 'M': case 'L': {
        int last = visible_last();
        long t;
        if (c == 'H') t = imin(topline + n1 - 1, last);
        else if (c == 'L') t = imax(topline, last - (n1 - 1));
        else t = topline + (last - topline) / 2;
        p.l = t; p.c = first_nonblank(t);
        m->linewise = m->jump = 1;
        m->to = p;
        return 1;
    }
    case 'w': case 'W': {
        w = p; wbig = c == 'W';
        if (op == 'c' && cls(cp_at(w.l, w.c), wbig) != 0) {
            if (!end_word(n1, 1, 0)) { m->fail = !(w.l != p.l || w.c != p.c); }
            m->incl = 1;
            m->to = w;
            return 1;
        }
        int ok = fwd_word(n1, op != 0);
        if (!ok && w.l == p.l && w.c == p.c) { m->fail = 1; }
        if (!ok && op) { w.c = (int)L[w.l].len; }
        else if (!ok) { w.c = last_c(w.l); }
        m->to = w;
        return 1;
    }
    case 'b': case 'B': {
        w = p; wbig = c == 'B';
        if (!bck_word(n1, 0)) m->fail = 1;
        m->to = w;
        if (w.c > (int)L[w.l].len) m->to.c = (int)L[w.l].len;
        return 1;
    }
    case 'e': case 'E': {
        w = p; wbig = c == 'E';
        if (!end_word(n1, 0, 0)) { if (w.l == p.l && w.c == p.c) m->fail = 1; else w.c = last_c(w.l); }
        m->incl = 1;
        m->to = w;
        if (m->to.c >= (int)L[m->to.l].len) m->to.c = last_c(m->to.l);
        return 1;
    }
    case 'f': case 'F': case 't': case 'T': {
        int k = getkey();
        if (k == K_ESC || k >= PT_K_BASE) { m->fail = 1; return 1; }
        if (k == 22) k = getkey();
        last_find_len = pt_u8_encode((uint32_t)k, last_find_char);
        last_find_cmd = c;
        if (!find_char(&p, c, last_find_char, last_find_len, n1, 0)) { m->fail = 1; return 1; }
        m->incl = c == 'f' || c == 't';
        m->to = p;
        return 1;
    }
    case ';': case ',': {
        if (!last_find_cmd) { m->fail = 1; return 1; }
        int cmd = last_find_cmd;
        if (c == ',') cmd = cmd == 'f' ? 'F' : cmd == 'F' ? 'f' : cmd == 't' ? 'T' : 't';
        if (!find_char(&p, cmd, last_find_char, last_find_len, n1, 1)) { m->fail = 1; return 1; }
        m->incl = cmd == 'f' || cmd == 't';
        m->to = p;
        return 1;
    }
    case '}': case '{': {
        int dir = c == '}' ? 1 : -1;
        long l = p.l;
        for (int i = 0; i < n1; i++) {
            while (l >= 0 && l < nl && L[l].len == 0) l += dir;                 /* skip blank lines first */
            while (l >= 0 && l < nl && L[l].len != 0) l += dir;
            if (l < 0 || l >= nl) break;
        }
        if (l < 0) { p.l = 0; p.c = 0; }
        else if (l >= nl) { p.l = nl - 1; p.c = (int)L[p.l].len; if (!op) p.c = last_c(p.l); else if (L[p.l].len) m->incl = 0; }
        else { p.l = l; p.c = 0; }
        if (p.l == cur.l && p.c == cur.c) { m->fail = 1; return 1; }
        m->jump = 1;
        m->to = p;
        return 1;
    }
    case '(': case ')': {
        if (c == ')') sentence_fwd(&p, n1); else sentence_bck(&p, n1);
        m->jump = 1;
        m->to = p;
        return 1;
    }
    case '[': case ']': {
        int k = getkey();
        if (k == c) {                                    /* [[ and ]]: lines that start with { */
            long l = p.l;
            int dir = c == ']' ? 1 : -1;
            for (int i = 0; i < n1; i++) {
                l += dir;
                while (l >= 0 && l < nl && !(L[l].len && ls(l)[0] == '{')) l += dir;
                if (l < 0 || l >= nl) break;
            }
            if (l < 0) { p.l = 0; p.c = 0; } else if (l >= nl) { p.l = nl - 1; p.c = last_c(p.l); } else { p.l = l; p.c = 0; }
            m->jump = 1;
            m->to = p;
            return 1;
        }
        if ((c == '[' && k == ']') || (c == ']' && k == '[')) {   /* [] and ][: lines that start with } */
            long l = p.l;
            int dir = c == ']' ? 1 : -1;
            for (int i = 0; i < n1; i++) {
                l += dir;
                while (l >= 0 && l < nl && !(L[l].len && ls(l)[0] == '}')) l += dir;
                if (l < 0 || l >= nl) break;
            }
            if (l < 0) { p.l = 0; p.c = 0; } else if (l >= nl) { p.l = nl - 1; p.c = last_c(p.l); } else { p.l = l; p.c = 0; }
            m->jump = 1;
            m->to = p;
            return 1;
        }
        if (k == '(' || k == '{' || k == ')' || k == '}') {            /* [( [{ ]) ]}: the unmatched bracket */
            int open = k == '(' || k == ')' ? '(' : '{', close = partner(open);
            Pos o1, cl;
            if (!find_enclosing(p, open, close, n1, &o1, &cl)) { m->fail = 1; return 1; }
            m->to = (c == '[') ? o1 : cl;
            m->jump = 1;
            return 1;
        }
        m->fail = 1;
        return 1;
    }
    case '%': {
        if (hascnt) {                                    /* N%: go to a line N percent of the way through the file */
            p.l = imin(nl - 1, (cnt * nl + 99) / 100 - 1);
            if (p.l < 0) p.l = 0;
            p.c = first_nonblank(p.l);
            m->linewise = m->jump = 1;
            m->to = p;
            return 1;
        }
        const char *s = ls(p.l);
        int len = (int)L[p.l].len, i = p.c;
        while (i < len && !is_open(s[i]) && !is_close(s[i])) i++;
        if (i >= len) { m->fail = 1; return 1; }
        p.c = i;
        Pos q;
        if (!match_pair(p, &q)) { m->fail = 1; return 1; }
        m->incl = 1;
        m->jump = 1;
        m->to = q;
        return 1;
    }
    case '\'': case '`': {
        int k = getkey();
        Pos mk;
        mk.l = -1;
        if (k >= 'a' && k <= 'z') mk = marks[k - 'a'];
        else if (k == '\'' || k == '`') mk = mark_ctx;
        else if (k == '[') mk = mark_lb;
        else if (k == ']') mk = mark_rb;
        else if (k == '<') mk = mark_lt;
        else if (k == '>') mk = mark_gt;
        else if (k == '.') mk = mark_dot;
        else if (k == '^') mk = mark_dot;
        if (mk.l < 0) { set_err("E20: Mark not set"); m->fail = 1; return 1; }
        if (mk.l >= nl) mk.l = nl - 1;
        if (c == '\'') { mk.c = first_nonblank(mk.l); m->linewise = 1; }
        else mk.c = clamp_col(mk.l, mk.c, 0);
        m->jump = 1;
        m->to = mk;
        return 1;
    }
    case '/': case '?': {
        PtBuf b = { 0 };
        char pr[2] = { (char)c, 0 };
        if (!cmdline_read(pr, NULL, &b)) { m->fail = 1; pb_free(&b); m->vcol = -3; return 1; }
        int incl = 0;
        do_search(b.p ? b.p : "", c == '/' ? 1 : -1, n1, cur, m, &incl);
        m->incl = incl;
        pb_free(&b);
        return 1;
    }
    case 'n': case 'N': {
        if (!last_search[0]) { set_err("E35: No previous regular expression"); m->fail = 1; return 1; }
        int dir = last_search_dir * (c == 'N' ? -1 : 1);
        char tmp[300];
        snprintf(tmp, sizeof tmp, "%s%c%s", last_search, dir > 0 ? '/' : '?', last_search_off);
        int incl = 0;
        char saved[256];
        snprintf(saved, sizeof saved, "%s", last_search);
        int sd = last_search_dir;
        do_search(tmp, dir, n1, cur, m, &incl);
        last_search_dir = sd;
        snprintf(last_search, sizeof last_search, "%s", saved);
        m->incl = incl;
        return 1;
    }
    case '*': case '#': {
        char word[200];
        int isw;
        int wstart;
        word_under_cursor(word, sizeof word, &isw, &wstart);
        if (!word[0]) { set_err("E348: No string under cursor"); m->fail = 1; return 1; }
        char pat[260];
        PtBuf esc = { 0 };
        for (char *q = word; *q; q++) { if (strchr("\\/.*$^~[]", *q)) pb_addc(&esc, '\\'); pb_addc(&esc, *q); }
        if (isw) snprintf(pat, sizeof pat, "\\<%s\\>", esc.p); else snprintf(pat, sizeof pat, "%s", esc.p);
        pb_free(&esc);
        snprintf(last_search, sizeof last_search, "%s", pat);
        last_search_off[0] = 0;
        last_search_dir = c == '*' ? 1 : -1;
        Pos from = cur;
        from.c = wstart;                                 /* search from the start of the word, so # does not find itself */
        Pos res = from;
        for (int i = 0; i < n1; i++) {
            Pos nx;
            int r = search_find(pat, last_search_dir, res, &nx, o.ws, 0);
            if (r <= 0) { if (r == 0) set_err("E486: Pattern not found: %s", pat); m->fail = 1; return 1; }
            res = nx;
        }
        m->jump = 1;
        m->to = res;
        return 1;
    }
    case 'g': {
        int k = getkey();
        switch (k) {
        case 'g':
            p.l = hascnt ? imin(cnt, nl) - 1 : 0;
            p.c = first_nonblank(p.l);
            m->linewise = m->jump = 1;
            m->to = p;
            return 1;
        case '_':
            if (p.l + n1 - 1 >= nl) { m->fail = 1; return 1; }
            p.l += n1 - 1;
            {
                const char *s = ls(p.l);
                int e = (int)L[p.l].len;
                while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
                p.c = e > 0 ? prev_c(p.l, e) : 0;
            }
            m->incl = 1;
            m->to = p;
            return 1;
        case 'e': case 'E':
            w = p; wbig = k == 'E';
            if (!bckend_word(n1, 0)) m->fail = 1;
            m->incl = 1;
            m->to = w;
            if (m->to.c >= (int)L[m->to.l].len) m->to.c = last_c(m->to.l);
            return 1;
        case 'j': case PT_K_DOWN: return motion('j', cnt, hascnt, op, m);
        case 'k': case PT_K_UP: return motion('k', cnt, hascnt, op, m);
        case '0': p.c = 0; m->to = p; return 1;
        case '$': return motion('$', cnt, hascnt, op, m);
        case 'm': p.c = byte_at_vcol(p.l, text_cols() / 2); p.c = clamp_col(p.l, p.c, 0); m->to = p; return 1;
        case 'o': {
            long off = n1 - 1, l = 0;
            while (l < nl - 1 && off > (long)L[l].len) { off -= (long)L[l].len + 1; l++; }
            p.l = l; p.c = clamp_col(l, (int)off, 0); m->jump = 1; m->to = p; return 1;
        }
        default:
            m->fail = 2;                                 /* not a motion: caller handles the g command */
            m->vcol = k;
            return 0;
        }
    }
    default: break;
    }
    return 0;
}

/* ------------------------------------------------------------------ text objects */

static void set_range_chars(Range *r, long l1, int c1, long l2, int c2_excl) {
    r->type = 'c';
    r->a.l = l1; r->a.c = c1;
    r->b.l = l2; r->b.c = c2_excl;
}

static int obj_word(int inner, int cnt, int big, Range *r) {
    long l = cur.l;
    int len = (int)L[l].len;
    if (len == 0) { set_range_chars(r, l, 0, l, 0); return 1; }
    const char *s = ls(l);
    int c = cur.c;
    if (vis_mode && (vis_start.l != cur.l || vis_start.c != cur.c)) c = next_c(l, cur.c) < len ? next_c(l, cur.c) : cur.c;
    int cl = cls(cp_at(l, c), big);
    int st = c;
    while (st > 0 && cls(cp_at(l, prev_c(l, st)), big) == cl) st = prev_c(l, st);
    int e = c;
    while (e < len && cls(cp_at(l, e), big) == cl) e = next_c(l, e);
    int first_is_blank = cl == 0;
    for (int i = 1; i < cnt; i++) {
        if (e >= len) break;
        int cl2 = cls(cp_at(l, e), big);
        while (e < len && cls(cp_at(l, e), big) == cl2) e = next_c(l, e);
    }
    if (!inner) {
        if (first_is_blank) {                            /* aw on white space: the space and the word after it */
            if (e < len) { int cl2 = cls(cp_at(l, e), big); while (e < len && cls(cp_at(l, e), big) == cl2) e = next_c(l, e); }
        } else if (e < len && cls(cp_at(l, e), big) == 0) {
            while (e < len && cls(cp_at(l, e), big) == 0) e = next_c(l, e);
        } else {
            while (st > 0 && cls(cp_at(l, prev_c(l, st)), big) == 0) st = prev_c(l, st);
        }
    }
    (void)s;
    set_range_chars(r, l, st, l, e);
    return 1;
}

static int obj_para(int inner, int cnt, Range *r) {
    long l = cur.l;
    if (vis_mode && vis_start.l != cur.l) l = imin(nl - 1, cur.l + 1);
    int blank = blank_line(l);
    long st = l;
    while (st > 0 && blank_line(st - 1) == blank) st--;
    long e = l;
    int cur_blank = blank;
    for (int i = 0; i < cnt; i++) {
        if (i > 0) { if (e + 1 >= nl) return 0; e++; cur_blank = blank_line(e); }
        while (e + 1 < nl && blank_line(e + 1) == cur_blank) e++;
        if (!inner) {                                    /* ap: the paragraph and the blank lines after it */
            if (e + 1 < nl) { e++; int b2 = blank_line(e); while (e + 1 < nl && blank_line(e + 1) == b2) e++; }
            else if (!cur_blank) { while (st > 0 && blank_line(st - 1)) st--; }
        }
    }
    r->type = 'l';
    r->a.l = st; r->a.c = 0;
    r->b.l = e; r->b.c = 0;
    return 1;
}

static int obj_block(int open, int close, int inner, int cnt, Range *r) {
    Pos o1, c1;
    Pos from = cur;
    if (vis_mode && (vis_start.l != cur.l || vis_start.c != cur.c)) {
        Pos a = vis_start, b = cur;
        if (a.l > b.l || (a.l == b.l && a.c > b.c)) { Pos t = a; a = b; b = t; }
        from = a;
        if (from.c > 0) from.c = prev_c(from.l, from.c); else if (from.l > 0) { from.l--; from.c = last_c(from.l); }
        cnt++;
    }
    if (!find_enclosing(from, open, close, cnt, &o1, &c1)) return 0;
    if (!inner) {
        set_range_chars(r, o1.l, o1.c, c1.l, c1.c + 1);
        return 1;
    }
    Pos a = o1, b = c1;
    a.c++;
    int linewise = 0;
    if (a.c >= (int)L[a.l].len && a.l < b.l) { a.l++; a.c = 0; linewise = 1; }
    if (linewise) {
        const char *s = ls(b.l);
        int k = 0;
        while (k < b.c && (s[k] == ' ' || s[k] == '\t')) k++;
        if (k == b.c) {
            if (b.l - 1 < a.l) { set_range_chars(r, a.l, 0, a.l, 0); return 1; }
            r->type = 'l'; r->a.l = a.l; r->a.c = 0; r->b.l = b.l - 1; r->b.c = 0;
            return 1;
        }
    }
    set_range_chars(r, a.l, a.c, b.l, b.c);
    return 1;
}

static int obj_quote(int q, int inner, Range *r) {
    long l = cur.l;
    const char *s = ls(l);
    int len = (int)L[l].len;
    int pos[256], n = 0;
    for (int i = 0; i < len && n < 256; i++) {
        if (s[i] == '\\' && i + 1 < len) { i++; continue; }
        if (s[i] == q) pos[n++] = i;
    }
    int a = -1, b = -1;
    int c = cur.c;
    for (int i = 0; i + 1 < n; i += 2) {
        if (c >= pos[i] && c <= pos[i + 1]) { a = pos[i]; b = pos[i + 1]; break; }
        if (c < pos[i]) { a = pos[i]; b = pos[i + 1]; break; }
    }
    if (a < 0) {                                         /* odd cursor-on-closing-quote case: pair (1,2),(3,4)... */
        for (int i = 1; i + 1 < n; i += 2) if (c >= pos[i] && c <= pos[i + 1]) { a = pos[i]; b = pos[i + 1]; break; }
    }
    if (a < 0) return 0;
    if (inner) { set_range_chars(r, l, a + 1, l, b); return 1; }
    int st = a, e = b + 1;
    if (e < len && (s[e] == ' ' || s[e] == '\t')) { while (e < len && (s[e] == ' ' || s[e] == '\t')) e++; }
    else { while (st > 0 && (s[st - 1] == ' ' || s[st - 1] == '\t')) st--; }
    set_range_chars(r, l, st, l, e);
    return 1;
}

static int obj_sentence(int inner, int cnt, Range *r) {
    Pos st = cur;
    /* walk back to the start of this sentence */
    while (!sentence_start_at(st.l, st.c)) {
        if (st.c > 0) st.c = prev_c(st.l, st.c);
        else if (st.l > 0) { st.l--; st.c = last_c(st.l); }
        else break;
    }
    Pos e = st;
    for (int i = 0; i < cnt; i++) {
        Pos nx = e;
        sentence_fwd(&nx, 1);
        if (nx.l == e.l && nx.c == e.c) { nx.c = (int)L[nx.l].len; }
        e = nx;
    }
    /* e is the start of the next sentence: back up over the white space between */
    Pos b = e;
    if (!(e.l == nl - 1 && e.c >= last_c(e.l) && !sentence_start_at(e.l, e.c))) {
        if (b.c == 0 && b.l > st.l) { b.l--; b.c = (int)L[b.l].len; }
    } else b.c = (int)L[b.l].len;
    if (inner) {
        const char *s = ls(b.l);
        while (b.c > 0 && (s[b.c - 1] == ' ' || s[b.c - 1] == '\t') && (b.l > st.l || b.c > st.c)) b.c--;
    }
    set_range_chars(r, st.l, st.c, b.l, b.c);
    return 1;
}

static int obj_tag(int inner, int cnt, Range *r) {
    /* find the innermost <tag ...> ... </tag> around the cursor on a flat text scan of nearby lines */
    long l0 = imax(0, cur.l - 200), l1 = imin(nl - 1, cur.l + 200);
    PtBuf t = { 0 };
    long *lstart = malloc((size_t)(l1 - l0 + 2) * sizeof *lstart);
    if (!lstart) pt_oom();
    for (long i = l0; i <= l1; i++) { lstart[i - l0] = (long)t.len; pb_add(&t, ls(i), L[i].len); pb_addc(&t, '\n'); }
    long curoff = lstart[cur.l - l0] + cur.c;
    int ok = 0;
    long ostart = -1, oend = -1, cstart = -1, cend = -1;
    int found = 0;
    for (long i = curoff; i >= 0; i--) {                 /* an opening tag before (or at) the cursor */
        if (t.p[i] != '<' || t.p[i + 1] == '/' || t.p[i + 1] == '!') continue;
        long j = i + 1;
        while (isalnum((unsigned char)t.p[j]) || t.p[j] == '-' || t.p[j] == '_' || t.p[j] == ':') j++;
        if (j == i + 1) continue;
        long nlen = j - i - 1;
        long k = j;
        while (k < (long)t.len && t.p[k] != '>') k++;
        if (k >= (long)t.len || t.p[k - 1] == '/') continue;
        /* find its closing tag, counting nesting */
        long q = k + 1;
        int depth = 1;
        long cs = -1;
        while (q < (long)t.len) {
            if (t.p[q] == '<') {
                if (t.p[q + 1] == '/' && !strncmp(t.p + q + 2, t.p + i + 1, (size_t)nlen) && t.p[q + 2 + nlen] == '>') { if (--depth == 0) { cs = q; break; } }
                else if (t.p[q + 1] != '/' && !strncmp(t.p + q + 1, t.p + i + 1, (size_t)nlen) && !isalnum((unsigned char)t.p[q + 1 + nlen])) depth++;
            }
            q++;
        }
        if (cs < 0) continue;
        long ce = cs + nlen + 3;
        if (curoff >= i && curoff < ce) { if (++found >= cnt) { ostart = i; oend = k + 1; cstart = cs; cend = ce; ok = 1; break; } }
    }
    if (ok) {
        long sa = inner ? oend : ostart, sb = inner ? cstart : cend;
        long li = 0;
        while (li + 1 <= l1 - l0 && lstart[li + 1] <= sa) li++;
        r->a.l = l0 + li; r->a.c = (int)(sa - lstart[li]);
        li = 0;
        while (li + 1 <= l1 - l0 && lstart[li + 1] <= sb) li++;
        r->b.l = l0 + li; r->b.c = (int)(sb - lstart[li]);
        r->type = 'c';
    }
    pb_free(&t);
    free(lstart);
    return ok;
}

int textobj(int c, int inner, int cnt, Range *r) {
    memset(r, 0, sizeof *r);
    if (cnt < 1) cnt = 1;
    switch (c) {
    case 'w': return obj_word(inner, cnt, 0, r);
    case 'W': return obj_word(inner, cnt, 1, r);
    case 's': return obj_sentence(inner, cnt, r);
    case 'p': return obj_para(inner, cnt, r);
    case '(': case ')': case 'b': return obj_block('(', ')', inner, cnt, r);
    case '{': case '}': case 'B': return obj_block('{', '}', inner, cnt, r);
    case '[': case ']': return obj_block('[', ']', inner, cnt, r);
    case '<': case '>': return obj_block('<', '>', inner, cnt, r);
    case '"': case '\'': case '`': return obj_quote(c, inner, r);
    case 't': return obj_tag(inner, cnt, r);
    default: return 0;
    }
}
