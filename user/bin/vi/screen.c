/* screen.c: geometry of the text on screen, painting, messages and the command line. */
#include "vi.h"

PtScreen scr;
char msg[512];
int msg_err;
int cmdline_active;
char cmdline_prompt[8];
PtBuf cmdline_text;
int cmdline_pos;
static int leftcol;                                      /* first display column shown with nowrap */

typedef struct { uint32_t cp; unsigned char attr; } Cell;
enum { A_REV = 1, A_HL = 2, A_CTL = 4, A_CURLINE = 8, A_MATCH = 16 };

void beep(void) {}
void force_repaint(void) { pts_invalidate(&scr); }

void set_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    msg_err = 0;
}

void set_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    msg_err = 1;
}

static void on_winch(int sig) { (void)sig; }

void scr_init(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
    pt_term_size(&term);
    pts_resize(&scr, term.rows, term.cols);
}

/* ------------------------------------------------------------------ geometry */

static int glyph_w(uint32_t cp, int vcol) {
    if (cp == '\t') return o.list ? 2 : o.ts - vcol % o.ts;
    if (cp < 32 || cp == 127) return 2;
    return 1;
}

int text_rows(void) {
    if (term.rows >= 3) return term.rows - 2;
    if (term.rows == 2) return 1;
    return 0;
}

static int numw(void) {
    if (!o.number && !o.rnu) return 0;
    int d = 1;
    for (long n = nl; n >= 10; n /= 10) d++;
    if (d < 3) d = 3;
    return d + 1;
}

int text_cols(void) {
    int c = term.cols - numw();
    return c < 1 ? 1 : c;
}

int vcol_of(long l, int c) {
    const char *s = ls(l);
    int len = (int)L[l].len, v = 0, i = 0;
    while (i < c && i < len) {
        uint32_t cp;
        int n = pt_u8_decode((const unsigned char *)s + i, (size_t)(len - i), &cp);
        v += glyph_w(cp, v);
        i += n;
    }
    return v;
}

int vcol_end(long l, int c) {
    const char *s = ls(l);
    int len = (int)L[l].len;
    int v = vcol_of(l, c);
    if (c >= len) return v;
    uint32_t cp;
    pt_u8_decode((const unsigned char *)s + c, (size_t)(len - c), &cp);
    return v + glyph_w(cp, v) - 1;
}

int line_vwidth(long l) { return vcol_of(l, (int)L[l].len); }

int byte_at_vcol(long l, int vcol) {
    const char *s = ls(l);
    int len = (int)L[l].len, v = 0, i = 0;
    while (i < len) {
        uint32_t cp;
        int n = pt_u8_decode((const unsigned char *)s + i, (size_t)(len - i), &cp);
        int w = glyph_w(cp, v);
        if (v + w > vcol) return i;
        v += w;
        i += n;
    }
    return len;
}

static int row_count(long l) {
    if (!o.wrap) return 1;
    int w = line_vwidth(l) + (o.list ? 1 : 0);
    int a = text_cols();
    return w <= a ? 1 : (w + a - 1) / a;
}

/* Rows used by lines first..last-1. */
static long rows_between(long first, long last) {
    long r = 0;
    for (long i = first; i < last && i < nl; i++) r += row_count(i);
    return r;
}

void scroll_to_cursor(void) {
    int th = text_rows();
    if (th <= 0) return;
    if (topline >= nl) topline = nl - 1;
    if (topline < 0) topline = 0;
    if (cur.l < topline) {
        topline = topline - cur.l > th ? imax(0, cur.l - th / 2) : cur.l;
    } else if (cur.l - topline > th + 1) {
        topline = cur.l - th / 2;
        if (topline < 0) topline = 0;
        if (cur.l == nl - 1 || nl - topline < th) topline = imax(0, nl - th);
    }
    if (cur.l >= topline) {
        int need = row_count(cur.l);
        while (topline < cur.l && rows_between(topline, cur.l) + need > th) topline++;
    }
    if (!o.wrap) {
        int a = text_cols();
        int v = (mode == M_INSERT || mode == M_REPLACE) ? vcol_of(cur.l, cur.c) : vcol_end(cur.l, cur.c);
        if (v < leftcol || v >= leftcol + a) { leftcol = v - a / 2; if (leftcol < 0) leftcol = 0; }
    } else leftcol = 0;
}

void scroll_lines(long n) {
    topline += n;
    if (topline < 0) topline = 0;
    if (topline > nl - 1) topline = nl - 1;
    int th = text_rows();
    if (cur.l < topline) { cur.l = topline; }
    long last = topline;
    long rows = 0;
    while (last < nl && rows + row_count(last) <= th) rows += row_count(last++);
    if (last > topline && cur.l > last - 1) cur.l = last - 1;
    cur.c = byte_at_vcol(cur.l, curswant < 0 ? 100000000 : curswant);
    fix_cursor();
}

/* ------------------------------------------------------------------ painting */

static int in_visual(long l, int c, int vcol) {
    if (!vis_mode) return 0;
    Pos a = vis_start, b = cur;
    if (a.l > b.l || (a.l == b.l && a.c > b.c)) { Pos t = a; a = b; b = t; }
    if (l < a.l || l > b.l) return 0;
    if (vis_mode == M_VLINE) return 1;
    if (vis_mode == M_VBLOCK) {
        int v1 = vcol_of(vis_start.l, vis_start.c), v2 = vcol_of(cur.l, cur.c);
        int e1 = vcol_end(vis_start.l, vis_start.c), e2 = vcol_end(cur.l, cur.c);
        int lo = imin(v1, v2), hi = imax(e1, e2);
        return vcol >= lo && (curswant < 0 || vcol <= hi);
    }
    if (l == a.l && c < a.c) return 0;
    if (l == b.l && c > b.c) return 0;
    return 1;
}

static regex_t hl_re;
static int hl_ok;
static char hl_pat[256];
static int hl_ic;

static void hl_prepare(void) {
    if (hl_ok) { regfree(&hl_re); hl_ok = 0; }
    if (!o.hls || !last_search[0]) return;
    if (re_compile(&hl_re, last_search, -1) == 0) { hl_ok = 1; strncpy(hl_pat, last_search, sizeof hl_pat - 1); }
}

/* Marks the matches of the search pattern in line l: sets A_HL on the cells. */
static void hl_line(long l, Cell *cells, int ncells, const int *byteoff) {
    if (!hl_ok) return;
    const char *s = ls(l);
    size_t len = L[l].len, pos = 0;
    regmatch_t m[1];
    int guard = 0;
    while (pos <= len && guard++ < 500) {
        if (re_exec_at(&hl_re, s, len, pos, m, 1) != 0) break;
        size_t so = (size_t)m[0].rm_so, eo = (size_t)m[0].rm_eo;
        for (int i = 0; i < ncells; i++) if ((size_t)byteoff[i] >= so && (size_t)byteoff[i] < eo) cells[i].attr |= A_HL;
        pos = eo > so ? eo : so + 1;
    }
}

/* Builds the display cells of line l (at most cap columns). Returns the count. */
static int render_line(long l, Cell *cells, int cap, int *byteoff) {
    const char *s = ls(l);
    int len = (int)L[l].len, v = 0, i = 0, n = 0;
    int curline = o.cursorline && l == cur.l;
    while (i < len && n < cap) {
        uint32_t cp;
        int bl = pt_u8_decode((const unsigned char *)s + i, (size_t)(len - i), &cp);
        int sel = in_visual(l, i, v);
        unsigned char base = (unsigned char)((sel ? A_REV : 0) | (curline ? A_CURLINE : 0));
        if (cp == '\t' && !o.list) {
            int w = o.ts - v % o.ts;
            for (int k = 0; k < w && n < cap; k++) { byteoff[n] = i; cells[n].cp = ' '; cells[n++].attr = base; }
            v += w;
        } else if (cp < 32 || cp == 127) {
            byteoff[n] = i; cells[n].cp = '^'; cells[n++].attr = base | A_CTL;
            if (n < cap) { byteoff[n] = i; cells[n].cp = cp == 127 ? '?' : cp + 64; cells[n++].attr = base | A_CTL; }
            v += 2;
        } else {
            byteoff[n] = i;
            cells[n].cp = pt_is_rawbyte(cp) ? '?' : cp;
            cells[n++].attr = (unsigned char)(base | (pt_is_rawbyte(cp) ? A_CTL : 0));
            v++;
        }
        i += bl;
    }
    if (o.list && n < cap) { byteoff[n] = len; cells[n].cp = '$'; cells[n++].attr = A_CTL; }
    else if (n < cap && (vis_mode == M_VLINE || (vis_mode && in_visual(l, len, v) && len == 0))) {
        byteoff[n] = len; cells[n].cp = ' '; cells[n++].attr = A_REV;
    }
    hl_line(l, cells, n, byteoff);
    return n;
}

static void sgr_for(PtRow *r, int attr) {
    pb_adds(&r->b, "\x1b[0m");
    if (attr & A_REV) pb_adds(&r->b, "\x1b[7m");
    else if (attr & A_HL) pb_adds(&r->b, "\x1b[43;30m");
    else if (attr & A_CURLINE) pb_adds(&r->b, "\x1b[44m");
    if ((attr & A_CTL) && !(attr & (A_REV | A_HL))) pb_adds(&r->b, "\x1b[36m");
}

static void put_cell(PtRow *r, const Cell *c, int *cur_attr) {
    if (c->attr != *cur_attr) { sgr_for(r, c->attr); *cur_attr = c->attr; }
    char u[4];
    pb_add(&r->b, u, (size_t)pt_u8_encode(c->cp, u));
    r->w++;
}

static void end_attr(PtRow *r, int *cur_attr) {
    if (*cur_attr) { pb_adds(&r->b, "\x1b[0m"); *cur_attr = 0; }
}

static const char *mode_name(void) {
    switch (mode) {
    case M_INSERT: return "INSERT";
    case M_REPLACE: return "REPLACE";
    default: break;
    }
    if (vis_mode == M_VISUAL) return "VISUAL";
    if (vis_mode == M_VLINE) return "V-LINE";
    if (vis_mode == M_VBLOCK) return "V-BLOCK";
    return "NORMAL";
}

static void put_text(PtRow *r, const char *s, int maxw) {
    pr_str(r, s, maxw);
}

void redraw(void) {
    pt_term_size(&term);
    if (term.rows != scr.rows || term.cols != scr.cols) pts_resize(&scr, term.rows, term.cols);
    if (cur.l >= nl) cur.l = nl - 1;
    if (cur.l < 0) cur.l = 0;
    int rows = term.rows, cols = term.cols;
    int th = text_rows();
    scroll_to_cursor();
    hl_prepare();
    int nw = numw(), avail = text_cols();
    int crow = -1, ccol = 0;

    /* ---- text rows */
    long l = topline;
    int row = 0;
    int cap = (o.wrap ? avail * (th > 0 ? th : 1) : leftcol + avail) + 1;
    Cell *cells = malloc((size_t)cap * sizeof *cells);
    int *boff = malloc((size_t)cap * sizeof *boff);
    if (!cells || !boff) pt_oom();
    while (row < th) {
        if (l >= nl) {
            PtRow *r = pts_row(&scr, row);
            pr_str(r, "~", cols);
            row++;
            continue;
        }
        int nrows = row_count(l);
        int n = render_line(l, cells, cap, boff);
        if (o.wrap && nrows > th - row && row > 0) {            /* does not fit: show @ like vi */
            while (row < th) { PtRow *r = pts_row(&scr, row++); pr_str(r, "@", cols); }
            break;
        }
        for (int k = 0; k < nrows && row < th; k++, row++) {
            PtRow *r = pts_row(&scr, row);
            if (nw) {
                char nb[24];
                long shown = l + 1;
                if (o.rnu) { shown = l == cur.l ? (o.number ? l + 1 : 0) : labs(l - cur.l); }
                if (k == 0) snprintf(nb, sizeof nb, o.rnu && !o.number ? "%*ld " : (o.rnu && l == cur.l ? "%-*ld " : "%*ld "), nw - 1, shown);
                else snprintf(nb, sizeof nb, "%*s", nw, "");
                pr_str(r, nb, nw);
            }
            int from = o.wrap ? k * avail : leftcol, to = from + avail, at = 0;
            int ca = 0;
            for (int i = from; i < to && i < n; i++) { put_cell(r, &cells[i], &ca); at++; }
            end_attr(r, &ca);
            (void)at;
        }
        if (l == cur.l) {
            int v = (mode == M_INSERT || mode == M_REPLACE) ? vcol_of(l, cur.c) : vcol_end(l, cur.c);
            if (o.wrap) {
                int rr = row - nrows + v / avail;
                if (rr >= row) rr = row - 1;
                crow = rr;
                ccol = nw + v % avail;
            } else {
                crow = row - 1;
                ccol = nw + (v - leftcol);
            }
        }
        l++;
    }
    free(cells);
    free(boff);

    /* ---- status line */
    if (rows >= 3) {
        PtRow *r = pts_row(&scr, rows - 2);
        PtBuf left = { 0 }, right = { 0 };
        pb_addf(&left, " %s  %s%s%s", mode_name(), fname[0] ? fname : "[No Name]", modified ? " [Modified]" : "",
                (o.ro || view_mode) ? " [RO]" : "");
        if (recording) pb_addf(&left, "  recording @%c", recording);
        int vc = vcol_end(cur.l, cur.c);
        if (L[cur.l].len == 0) pb_addf(&right, "%ld,0-1  ", cur.l + 1);
        else if (vc == cur.c) pb_addf(&right, "%ld,%d  ", cur.l + 1, cur.c + 1);
        else pb_addf(&right, "%ld,%d-%d  ", cur.l + 1, cur.c + 1, vc + 1);
        pb_addf(&right, "%ld%% ", (cur.l + 1) * 100 / nl);
        pb_adds(&r->b, "\x1b[7m");
        int lw = (int)pt_u8_count(left.p, left.len);
        int rw = (int)right.len;
        if (lw + rw > cols) { lw = cols - rw > 0 ? cols - rw : 0; }
        put_text(r, left.p, lw);
        int pad = cols - r->w - rw;
        if (pad < 0) pad = 0;
        pr_spaces(r, pad);
        put_text(r, right.p, cols - r->w);
        pb_adds(&r->b, "\x1b[0m");
        pb_free(&left);
        pb_free(&right);
    }

    /* ---- message / command line row */
    {
        PtRow *r = pts_row(&scr, rows - 1);
        if (cmdline_active) {
            pb_adds(&r->b, cmdline_prompt);
            r->w += (int)strlen(cmdline_prompt);
            int maxw = cols - r->w - 1;
            size_t total = pt_u8_count(cmdline_text.p ? cmdline_text.p : "", cmdline_text.len);
            size_t skip = total > (size_t)maxw ? total - (size_t)maxw : 0;
            size_t before = pt_u8_count(cmdline_text.p ? cmdline_text.p : "", (size_t)cmdline_pos);
            size_t start = pt_u8_skip(cmdline_text.p ? cmdline_text.p : "", cmdline_text.len, skip);
            put_text(r, cmdline_text.p ? cmdline_text.p + start : "", maxw);
            crow = rows - 1;
            ccol = (int)strlen(cmdline_prompt) + (int)(before > skip ? before - skip : 0);
        } else if (msg[0]) {
            if (msg_err) pb_adds(&r->b, "\x1b[31;1m");
            put_text(r, msg, cols - 1);
            if (msg_err) pb_adds(&r->b, "\x1b[0m");
        } else if (o.showmode && (mode == M_INSERT || mode == M_REPLACE || vis_mode)) {
            char tmp[40];
            snprintf(tmp, sizeof tmp, "-- %s --", mode_name());
            put_text(r, tmp, cols - 1);
        }
    }
    if (rows < 3 && crow < 0) crow = rows - 1;
    if (crow >= rows) crow = rows - 1;
    if (ccol >= cols) ccol = cols - 1;
    pts_flush(&scr, &term, crow, ccol, 1);
}

/* ------------------------------------------------------------------ the command line */

#define HISTN 50
static char *hist[2][HISTN];
static int histn[2];

static void hist_add(int which, const char *s) {
    if (!*s) return;
    if (histn[which] && !strcmp(hist[which][histn[which] - 1], s)) return;
    if (histn[which] == HISTN) { free(hist[which][0]); memmove(&hist[which][0], &hist[which][1], (HISTN - 1) * sizeof(char *)); histn[which]--; }
    hist[which][histn[which]++] = strdup(s);
}

static void cl_insert(const char *s, size_t n) {
    pb_reserve(&cmdline_text, n);
    memmove(cmdline_text.p + cmdline_pos + n, cmdline_text.p + cmdline_pos, cmdline_text.len - (size_t)cmdline_pos);
    memcpy(cmdline_text.p + cmdline_pos, s, n);
    cmdline_text.len += n;
    cmdline_text.p[cmdline_text.len] = 0;
    cmdline_pos += (int)n;
}

static void cl_delete(int from, int to) {
    memmove(cmdline_text.p + from, cmdline_text.p + to, cmdline_text.len - (size_t)to);
    cmdline_text.len -= (size_t)(to - from);
    cmdline_text.p[cmdline_text.len] = 0;
    cmdline_pos = from;
}

int cmdline_read(const char *prompt, const char *init, PtBuf *out) {
    int which = prompt[0] == ':' ? 0 : 1;
    int hi = histn[which];
    cmdline_active = 1;
    snprintf(cmdline_prompt, sizeof cmdline_prompt, "%s", prompt);
    pb_clear(&cmdline_text);
    pb_reserve(&cmdline_text, 1);
    if (cmdline_text.p) cmdline_text.p[0] = 0;
    cmdline_pos = 0;
    if (init) cl_insert(init, strlen(init));
    int accepted = 0;
    msg[0] = 0;
    for (;;) {
        int k = getkey();
        if (k == K_ENTER) { accepted = 1; break; }
        if (k == K_ESC || k == 3) break;
        if (k == K_BS || k == PT_K_DEL) {
            if (k == PT_K_DEL) { if ((size_t)cmdline_pos < cmdline_text.len) cl_delete(cmdline_pos, cmdline_pos + pt_u8_next(cmdline_text.p, cmdline_text.len, (size_t)cmdline_pos)); continue; }
            if (cmdline_text.len == 0) break;
            if (cmdline_pos > 0) cl_delete((int)pt_u8_prev(cmdline_text.p, (size_t)cmdline_pos), cmdline_pos);
        } else if (k == 21) cl_delete(0, cmdline_pos);
        else if (k == 23) {
            int p = cmdline_pos;
            while (p > 0 && cmdline_text.p[p - 1] == ' ') p--;
            if (p > 0 && (isalnum((unsigned char)cmdline_text.p[p - 1]) || cmdline_text.p[p - 1] == '_')) { while (p > 0 && (isalnum((unsigned char)cmdline_text.p[p - 1]) || cmdline_text.p[p - 1] == '_')) p--; }
            else if (p > 0) p--;
            cl_delete(p, cmdline_pos);
        } else if (k == 22) {
            int c = getkey();
            char u[4];
            if (c < 32 || c == 127) { u[0] = (char)c; cl_insert(u, 1); }
            else cl_insert(u, (size_t)pt_u8_encode((uint32_t)c, u));
        } else if (k == 18) {
            int rg = getkey();
            if (rg == 23) {                              /* Ctrl-R Ctrl-W: the word under the cursor */
                const char *s = ls(cur.l);
                int c = cur.c, e = cur.c, len = (int)L[cur.l].len;
                while (c > 0 && (isalnum((unsigned char)s[c - 1]) || s[c - 1] == '_')) c--;
                while (e < len && (isalnum((unsigned char)s[e]) || s[e] == '_')) e++;
                cl_insert(s + c, (size_t)(e - c));
            } else {
                Reg *rr = reg_get(rg);
                if (rr) for (size_t i = 0; i < rr->len; i++) { char ch = rr->s[i] == '\n' ? '\r' : rr->s[i]; cl_insert(&ch, 1); }
            }
        } else if (k == PT_K_LEFT || k == 2) { if (cmdline_pos > 0) cmdline_pos = (int)pt_u8_prev(cmdline_text.p, (size_t)cmdline_pos); }
        else if (k == PT_K_RIGHT || k == 6) { if ((size_t)cmdline_pos < cmdline_text.len) cmdline_pos += pt_u8_next(cmdline_text.p, cmdline_text.len, (size_t)cmdline_pos); }
        else if (k == PT_K_HOME || k == 1) cmdline_pos = 0;
        else if (k == PT_K_END || k == 5) cmdline_pos = (int)cmdline_text.len;
        else if (k == PT_K_UP || k == 16 || k == PT_K_DOWN || k == 14) {
            int up = k == PT_K_UP || k == 16;
            if (up && hi > 0) hi--;
            else if (!up && hi < histn[which]) hi++;
            else continue;
            pb_clear(&cmdline_text);
            cmdline_pos = 0;
            if (hi < histn[which]) cl_insert(hist[which][hi], strlen(hist[which][hi]));
        } else if (k >= 32 && k < PT_K_BASE) {
            char u[4];
            cl_insert(u, (size_t)pt_u8_encode((uint32_t)k, u));
        } else if (k == 9) { char t = '\t'; cl_insert(&t, 1); }
    }
    cmdline_active = 0;
    if (accepted) {
        pb_clear(out);
        pb_add(out, cmdline_text.p ? cmdline_text.p : "", cmdline_text.len);
        hist_add(which, out->p);
    }
    return accepted;
}
