/* normal.c: normal, visual and insert mode, the operators, put, join, shifting, reflow. */
#include "vi.h"

int cmd_is_change, cur_count, cur_reg;
int pending_force_op;
char last_insert[8192];
size_t last_insert_len;
int undo_nest;
static int last_vis_mode;
static Pos last_vis_start, last_vis_end;
static int last_vis_curswant;
static int scroll_amount;                                /* Ctrl-D / Ctrl-U distance, 0 = half a window */
static long ai_line = -1;                                /* a line holding only an autoindent that was not typed over (-1: none) */
int dot_set(int on);
static int insert_key_wrapper(int k);
int a_start_col(Pos start, long first);

/* ------------------------------------------------------------------ cursor */

void fix_cursor(void) {
    if (nl <= 0) return;
    if (cur.l >= nl) cur.l = nl - 1;
    if (cur.l < 0) cur.l = 0;
    cur.c = clamp_col(cur.l, cur.c, mode == M_INSERT || mode == M_REPLACE);
}

void set_cursor(Pos p) {
    cur = p;
    fix_cursor();
}

static void update_curswant(void) { curswant = vcol_end(cur.l, cur.c); }

static int norm_key(int k) {
    switch (k) {
    case PT_K_UP: return 'k';
    case PT_K_DOWN: return 'j';
    case PT_K_LEFT: return 'h';
    case PT_K_RIGHT: return 'l';
    case PT_K_HOME: return '0';
    case PT_K_END: return '$';
    case PT_K_PGUP: return 2;
    case PT_K_PGDN: return 6;
    case PT_K_DEL: return 'x';
    case PT_K_INS: return 'i';
    case K_SLEFT: return 'b';
    case K_SRIGHT: return 'w';
    case K_CLEFT: return 'B';
    case K_CRIGHT: return 'W';
    default: return k;
    }
}

int indent_of(long l) {
    const char *s = ls(l);
    int v = 0;
    for (unsigned i = 0; i < LN(l).len; i++) {
        if (s[i] == ' ') v++;
        else if (s[i] == '\t') v += o.ts - v % o.ts;
        else break;
    }
    return v;
}

void set_indent(long l, int width) {
    const char *s = ls(l);
    int n = 0;
    while (n < (int)LN(l).len && (s[n] == ' ' || s[n] == '\t')) n++;
    PtBuf b = { 0 };
    int v = 0;
    if (!o.et) while (v + o.ts <= width) { pb_addc(&b, '\t'); v += o.ts; }
    while (v < width) { pb_addc(&b, ' '); v++; }
    PtBuf nw = { 0 };
    pb_add(&nw, b.p ? b.p : "", b.len);
    pb_add(&nw, s + n, LN(l).len - (size_t)n);
    ln_set(l, nw.p ? nw.p : "", nw.len);
    pb_free(&b);
    pb_free(&nw);
}

/* ------------------------------------------------------------------ operators on ranges */

/* J, gJ and :join: joins `count` lines (at least two) starting at `first`, like Vim's do_join(). With `spaces` the leading blanks
 * of each line go and one space is put between the pieces, except after a blank or tab, before a ")" and before an empty
 * piece; 'joinspaces' adds a second space after ".", "?" or "!". The cursor lands on the last join. */
void join_lines(long first, long count, int spaces) {
    if (count < 2) count = 2;
    if (first + 1 >= nl) return;
    long last = imin(nl - 1, first + count - 1);
    PtBuf b = { 0 };
    pb_add(&b, ls(first), LN(first).len);
    int endc1 = 0, endc2 = 0, curc = 0;
    if (spaces && LN(first).len) {
        endc1 = (unsigned char)b.p[b.len - 1];
        endc2 = b.len > 1 ? (unsigned char)b.p[b.len - 2] : 0;
    }
    for (long i = first + 1; i <= last; i++) {
        const char *s = ls(i);
        int n = 0, len = (int)LN(i).len, nsp = 0;
        if (spaces) {
            while (n < len && (s[n] == ' ' || s[n] == '\t')) n++;
            if (n < len && s[n] != ')' && b.len != 0 && endc1 != '\t') {
                if (endc1 == ' ') endc1 = endc2;
                else nsp++;
                if (o.js && (endc1 == '.' || endc1 == '?' || endc1 == '!')) nsp++;
            }
        }
        curc = (int)b.len;
        for (int k = 0; k < nsp; k++) pb_addc(&b, ' ');
        pb_add(&b, s + n, (size_t)(len - n));
        endc1 = endc2 = 0;
        if (spaces && len > n) {
            endc1 = (unsigned char)s[len - 1];
            endc2 = len - n > 1 ? (unsigned char)s[len - 2] : 0;
        }
    }
    ln_set(first, b.p ? b.p : "", b.len);
    ln_delete(first + 1, last - first);
    pb_free(&b);
    cur.l = first;
    cur.c = curc;
    mark_lb = (Pos){ first, 0 };
    mark_rb = (Pos){ first, 0 };
    fix_cursor();
}

void shift_lines(long first, long last, int amount) {
    for (long i = first; i <= last && i < nl; i++) {
        if (LN(i).len == 0) continue;
        int w = indent_of(i) + amount * o.sw;
        if (w < 0) w = 0;
        set_indent(i, w);
    }
}

void reflow_lines(long first, long last) {
    int tw = o.tw > 0 ? o.tw : 79;
    PtBuf out = { 0 };
    long l = first;
    long outlines = 0;
    while (l <= last) {
        if (blank_line_pub(l)) { pb_add(&out, "", 0); pb_addc(&out, '\n'); outlines++; l++; continue; }
        int ind = indent_of(l);
        PtBuf para = { 0 };
        while (l <= last && !blank_line_pub(l)) {
            const char *s = ls(l);
            int n = 0, len = (int)LN(l).len;
            while (n < len && (s[n] == ' ' || s[n] == '\t')) n++;
            if (para.len) pb_addc(&para, ' ');
            pb_add(&para, s + n, (size_t)(len - n));
            l++;
        }
        /* break para into words */
        PtBuf line = { 0 };
        for (int i = 0; i < ind; i++) pb_addc(&line, ' ');
        int linecols = ind, first_word = 1;
        size_t i = 0;
        while (i < para.len) {
            while (i < para.len && para.p[i] == ' ') i++;
            size_t j = i;
            while (j < para.len && para.p[j] != ' ') j++;
            if (j == i) break;
            int wcols = (int)pt_u8_count(para.p + i, j - i);
            if (!first_word && linecols + 1 + wcols > tw) {
                pb_add(&out, line.p, line.len); pb_addc(&out, '\n'); outlines++;
                pb_clear(&line);
                for (int k = 0; k < ind; k++) pb_addc(&line, ' ');
                linecols = ind; first_word = 1;
            }
            if (!first_word) { pb_addc(&line, ' '); linecols++; }
            pb_add(&line, para.p + i, j - i);
            linecols += wcols;
            first_word = 0;
            i = j;
        }
        pb_add(&out, line.p ? line.p : "", line.len); pb_addc(&out, '\n'); outlines++;
        pb_free(&line);
        pb_free(&para);
    }
    /* replace lines first..last with the new text */
    ln_delete(first, last - first + 1);
    if (nl == 1 && LN(0).len == 0 && first == 0) {        /* the buffer became one empty line: reuse it */
        text_insert((Pos){ 0, 0 }, out.p ? out.p : "", out.len ? out.len - 1 : 0);
    } else {
        long at = first;
        size_t p = 0;
        while (p < out.len) {
            char *e = memchr(out.p + p, '\n', out.len - p);
            size_t n = e ? (size_t)(e - (out.p + p)) : out.len - p;
            ln_insert(at++, out.p + p, n);
            p += n + 1;
        }
    }
    pb_free(&out);
    cur.l = imin(nl - 1, first + outlines - 1);
    cur.c = first_nonblank(cur.l);
}

int blank_line_pub(long l) {
    const char *s = ls(l);
    for (unsigned i = 0; i < LN(l).len; i++) if (s[i] != ' ' && s[i] != '\t') return 0;
    return 1;
}

/* Simple case mapping for the scripts a terminal user is likely to meet: ASCII, Latin-1, Latin Extended-A and Additional, Greek,
 * Cyrillic, Armenian and the full-width letters. Characters outside these map to themselves. */
uint32_t cp_upper(uint32_t c) {
    if (c < 0x80) return c >= 'a' && c <= 'z' ? c - 32 : c;
    if (c == 0xB5) return 0x39C;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x131) return 'I';
        if (c == 0x17F) return 'S';
        if (c == 0x130 || c == 0x138 || c == 0x149 || c == 0x178) return c;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
        return (c & 1) ? c - 1 : c;
    }
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c == 0x3C2) return 0x3A3;
    if (c >= 0x3AC && c <= 0x3AF) return c == 0x3AC ? 0x386 : c - 37;
    if (c >= 0x3CC && c <= 0x3CE) return c == 0x3CC ? 0x38C : c - 63;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F)) return (c & 1) ? c - 1 : c;
    if (c >= 0x561 && c <= 0x586) return c - 48;
    if (c >= 0x1E00 && c <= 0x1EFF && !(c >= 0x1E96 && c <= 0x1E9F)) return (c & 1) ? c - 1 : c;
    if (c >= 0xFF41 && c <= 0xFF5A) return c - 32;
    return c;
}

uint32_t cp_lower(uint32_t c) {
    if (c < 0x80) return c >= 'A' && c <= 'Z' ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x130) return 'i';
        if (c == 0x178) return 0xFF;
        if (c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F) return c;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c + 1 : c;
        return (c & 1) ? c : c + 1;
    }
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c == 0x386) return 0x3AC;
    if (c >= 0x388 && c <= 0x38A) return c + 37;
    if (c == 0x38C) return 0x3CC;
    if (c == 0x38E || c == 0x38F) return c + 63;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F)) return (c & 1) ? c : c + 1;
    if (c >= 0x531 && c <= 0x556) return c + 48;
    if (c >= 0x1E00 && c <= 0x1EFF && !(c >= 0x1E96 && c <= 0x1E9F)) return (c & 1) ? c : c + 1;
    if (c >= 0xFF21 && c <= 0xFF3A) return c + 32;
    return c;
}

static void case_range(const Range *r, int how) {
    for (long i = r->a.l; i <= r->b.l && i < nl; i++) {
        int s = 0, e = (int)LN(i).len;
        if (r->type == 'c') { if (i == r->a.l) s = r->a.c; if (i == r->b.l) e = imin(r->b.c, e); }
        else if (r->type == 'b') {
            if (line_vwidth(i) <= r->vc1) continue;
            s = byte_at_vcol(i, r->vc1);
            e = r->toeol || line_vwidth(i) <= r->vc2 + 1 ? (int)LN(i).len : byte_at_vcol(i, r->vc2 + 1);
        }
        if (e <= s) continue;
        const char *src = ls(i);
        int len = (int)LN(i).len, changed = 0;
        PtBuf nb = { 0 };
        pb_add(&nb, src, (size_t)s);
        for (int k = s; k < e;) {
            uint32_t cp;
            int n = pt_u8_decode((const unsigned char *)src + k, (size_t)(len - k), &cp);
            uint32_t m = cp;
            if (how == 'u') m = cp_lower(cp);
            else if (how == 'U') m = cp_upper(cp);
            else if (how == '~') m = cp_upper(cp) != cp ? cp_upper(cp) : cp_lower(cp);
            else if (how == '?' && cp < 0x80 && isalpha((int)cp)) { int base = isupper((int)cp) ? 'A' : 'a'; m = (uint32_t)(base + ((int)cp - base + 13) % 26); }
            if (m != cp && !pt_is_rawbyte(cp)) {
                char u[4];
                pb_add(&nb, u, (size_t)pt_u8_encode(m, u));
                changed = 1;
            } else pb_add(&nb, src + k, (size_t)n);
            k += n;
        }
        pb_add(&nb, src + e, (size_t)(len - e));
        if (changed) ln_set(i, nb.p, nb.len);
        pb_free(&nb);
    }
}

static void set_change_marks(const Range *r) {
    mark_lb = r->a;
    mark_rb = r->b;
    if (r->type == 'l') { mark_lb.c = 0; mark_rb.c = imax(0, (int)LN(imin(r->b.l, nl - 1)).len - 1); mark_rb.l = imin(r->b.l, nl - 1); }
    else if (mark_rb.c > 0) mark_rb.c--;
}

static void block_range_from_visual(Range *r) {
    Pos a = vis_start, b = cur;
    r->type = 'b';
    r->a.l = imin(a.l, b.l); r->b.l = imax(a.l, b.l);
    int v1 = vcol_of(a.l, a.c), v2 = vcol_of(b.l, b.c);
    int e1 = vcol_end(a.l, a.c), e2 = vcol_end(b.l, b.c);
    r->vc1 = (int)imin(v1, v2);
    r->vc2 = (int)imax(e1, e2);
    r->toeol = curswant < 0;
    r->a.c = r->vc1; r->b.c = r->vc2;
}

static void yank_range(const Range *r, int reg) {
    size_t n;
    char *t = range_text(r, &n);
    reg_set(reg, t, n, r->type, 0);
    free(t);
    long lines = r->b.l - r->a.l + 1;
    if (lines > 2) set_msg("%ld lines yanked", lines);
}

/* Performs operator `op` on range r. */
void do_op_range(int op, Range *r, int reg, int cnt) {
    long nlines = r->b.l - r->a.l + 1;
    switch (op) {
    case 'y':
        yank_range(r, reg);
        set_change_marks(r);
        cur = r->a;                                      /* the cursor goes to the start of the yanked text */
        if (r->type == 'b') cur.c = byte_at_vcol(cur.l, r->vc1);
        fix_cursor();
        return;
    case 'd': case 'c': {
        size_t n;
        char *t = range_text(r, &n);
        reg_set(reg, t, n, r->type, r->reg1 ? 2 : 1);
        free(t);
        if (op == 'c' && r->type == 'l') {
            int ind = o.ai ? indent_of(r->a.l) : 0;
            if (nlines > 1) ln_delete(r->a.l + 1, nlines - 1);
            ln_set(r->a.l, "", 0);
            if (ind) { set_indent(r->a.l, ind); ai_line = r->a.l; }
            cur.l = r->a.l;
            cur.c = (int)LN(cur.l).len;
            mode = M_INSERT;
            return;
        }
        range_delete(r);
        set_change_marks(r);
        if (r->type == 'l') {
            cur.l = imin(r->a.l, nl - 1);
            cur.c = first_nonblank(cur.l);
            if (nlines > 2) set_msg("%ld fewer lines", nlines);
        } else if (r->type == 'b') {
            cur.l = r->a.l;
            cur.c = byte_at_vcol(cur.l, r->vc1);
        } else {
            cur = r->a;
            if (r->b.l > r->a.l + 1) set_msg("%ld fewer lines", r->b.l - r->a.l);
        }
        if (op == 'c') mode = M_INSERT;
        if (cur.l >= nl) cur.l = nl - 1;
        if (op == 'c') { if (cur.c > (int)LN(cur.l).len) cur.c = (int)LN(cur.l).len; }
        else fix_cursor();
        return;
    }
    case '>': case '<': {
        int times = cnt > 1 ? cnt : 1;                   /* a count in Visual mode shifts that many times */
        shift_lines(r->a.l, r->b.l, (op == '>' ? 1 : -1) * times);
        cur.l = r->a.l;
        cur.c = first_nonblank(cur.l);
        if (nlines > 2) set_msg("%ld lines %ced %d time%s", nlines, op, times, times == 1 ? "" : "s");
        return;
    }
    case 'u': case 'U': case '~': case '?': {
        case_range(r, op);
        cur = r->a;
        if (r->type == 'b') cur.c = byte_at_vcol(cur.l, r->vc1);
        fix_cursor();
        return;
    }
    case 'q': case 'w': {
        Pos save = cur;
        reflow_lines(r->a.l, r->b.l);
        if (op == 'w') { cur = save; fix_cursor(); }
        return;
    }
    case '=': {
        for (long i = r->a.l + 1; i <= r->b.l && i < nl; i++) {          /* a modest "=": copy the indent of the line above */
            long p = i - 1;
            while (p > r->a.l - 1 && p > 0 && blank_line_pub(p)) p--;
            if (!blank_line_pub(i)) set_indent(i, indent_of(p));
        }
        cur.l = r->a.l;
        cur.c = first_nonblank(cur.l);
        return;
    }
    case '!': {
        char range[64];
        if (r->a.l == r->b.l) snprintf(range, sizeof range, ".!");
        else snprintf(range, sizeof range, ".,.+%ld!", r->b.l - r->a.l);
        cur.l = r->a.l;
        cur.c = first_nonblank(cur.l);
        ex_line_prompt(range);
        return;
    }
    default: break;
    }
}

int cur_start_line(void) { return (int)cur.l; }

/* ------------------------------------------------------------------ put */

int put_reg(int regname, int after, int cnt, int gmode) {
    Reg *rg = reg_get(regname);
    if (!rg || !rg->s) { set_err("E353: Nothing in register %c", regname ? regname : '"'); return 0; }
    return put_text(rg->s, rg->len, rg->type, after, cnt, gmode);
}

/* Puts text of the given type ('c', 'l', 'b') after or before the cursor, cnt times. */
int put_text(const char *src, size_t len, int type, int after, int cnt, int gmode) {
    if (cnt < 1) cnt = 1;
    char *text = malloc(len + 1);
    if (!text) pt_oom();
    memcpy(text, src, len);
    if (type == 'l') {
        long at = after ? cur.l + 1 : cur.l;
        long first = at;
        long total = 0;
        for (int i = 0; i < cnt; i++) {
            size_t p = 0;
            while (p < len) {
                char *e = memchr(text + p, '\n', len - p);
                size_t n = e ? (size_t)(e - (text + p)) : len - p;
                ln_insert(at++, text + p, n);
                total++;
                p += n + 1;
            }
        }
        mark_lb.l = first; mark_lb.c = 0; mark_rb.l = at - 1; mark_rb.c = 0;
        if (gmode) { cur.l = imin(nl - 1, at); cur.c = 0; }
        else { cur.l = first; cur.c = first_nonblank(first); }
        if (total > 2) set_msg("%ld more lines", total);
    } else if (type == 'b') {
        int width = 0;
        long nlin = 0;
        for (size_t p = 0; p <= len;) {                  /* find the widest line of the block */
            char *e = memchr(text + p, '\n', len - p);
            size_t n = e ? (size_t)(e - (text + p)) : len - p;
            int wd = (int)pt_u8_count(text + p, n);
            if (wd > width) width = wd;
            nlin++;
            p += n + 1;
            if (!e) break;
        }
        int vc = after && LN(cur.l).len ? vcol_end(cur.l, cur.c) + 1 : vcol_of(cur.l, cur.c);
        size_t p = 0;
        for (long i = 0; i < nlin; i++) {
            long ln = cur.l + i;
            if (ln >= nl) ln_insert(nl, "", 0);
            char *e = memchr(text + p, '\n', len - p);
            size_t n = e ? (size_t)(e - (text + p)) : len - p;
            int lw = line_vwidth(ln);
            PtBuf b = { 0 };
            if (lw < vc) { pb_add(&b, ls(ln), LN(ln).len); for (int k = lw; k < vc; k++) pb_addc(&b, ' '); }
            else pb_add(&b, ls(ln), (size_t)byte_at_vcol(ln, vc));
            int tail = lw > vc ? byte_at_vcol(ln, vc) : (int)LN(ln).len;
            for (int c = 0; c < cnt; c++) {
                pb_add(&b, text + p, n);
                if (lw > vc || c < cnt - 1) for (int k = (int)pt_u8_count(text + p, n); k < width; k++) pb_addc(&b, ' ');
            }
            pb_add(&b, ls(ln) + tail, LN(ln).len - (size_t)tail);
            ln_set(ln, b.p ? b.p : "", b.len);
            pb_free(&b);
            p += n + 1;
        }
        cur.c = byte_at_vcol(cur.l, vc);
    } else {
        Pos at = cur;
        if (after && LN(at.l).len) at.c = next_c(at.l, at.c);
        PtBuf all = { 0 };
        for (int i = 0; i < cnt; i++) pb_add(&all, text, len);
        text_insert(at, all.p ? all.p : "", all.len);
        mark_lb = at;
        int nlns = 0;
        for (size_t i = 0; i < all.len; i++) if (all.p[i] == '\n') nlns++;
        if (nlns == 0) {
            cur.l = at.l;
            cur.c = at.c + (int)all.len;
            mark_rb = cur;
            if (!gmode) cur.c = prev_c(cur.l, cur.c);
        } else {
            mark_rb.l = at.l + nlns; { size_t tl = 0; for (size_t q = all.len; q > 0 && all.p[q - 1] != 0x0a; q--) tl++; mark_rb.c = tl ? (int)tl - 1 : 0; }
            if (gmode) { cur.l = mark_rb.l; cur.c = mark_rb.c; }
            else cur = at;
            if (nlns > 1) set_msg("%d more lines", nlns);
        }
        pb_free(&all);
    }
    free(text);
    fix_cursor();
    return 1;
}

/* ------------------------------------------------------------------ insert mode */

static int *ins_keys;
static int ins_n, ins_cap;
static Pos ins_start;
static PtBuf ins_text;
static char *repl_saved;                                 /* bytes replaced by typed characters in replace mode */
static int repl_n, repl_cap;
static int ins_replay;

/* Leaving a line whose only content is the autoindent takes the indent away again. */
static void abbr_check(void);
static void ai_clean(void) {
    if (ai_line >= 0 && ai_line < nl && LN(ai_line).len > 0 && blank_line_pub(ai_line)) {
        ln_set(ai_line, "", 0);
        if (cur.l == ai_line) cur.c = 0;
    }
    ai_line = -1;
}

static void ins_record(int k) {
    if (ins_replay) return;
    if (ins_n == ins_cap) {
        ins_cap = ins_cap ? ins_cap * 2 : 64;
        ins_keys = realloc(ins_keys, (size_t)ins_cap * sizeof *ins_keys);
        if (!ins_keys) pt_oom();
    }
    ins_keys[ins_n++] = k;
}

static void ins_bytes(const char *s, size_t n) {
    if (n) ai_line = -1;
    text_insert(cur, s, n);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') { cur.l++; cur.c = 0; }
        else cur.c++;
    }
}

static void ins_note_text(const char *s, size_t n) {
    if (ins_replay) return;
    pb_add(&ins_text, s, n);
}

static void insert_cp(uint32_t cp) {
    char u[4];
    int n = pt_u8_encode(cp, u);
    if (mode == M_REPLACE) {
        int len = (int)LN(cur.l).len;
        if (cur.c < len) {
            int e = next_c(cur.l, cur.c);
            if (repl_n + 8 >= repl_cap) { repl_cap = repl_cap ? repl_cap * 2 : 64; repl_saved = realloc(repl_saved, (size_t)repl_cap); if (!repl_saved) pt_oom(); }
            repl_saved[repl_n++] = (char)(e - cur.c);
            memcpy(repl_saved + repl_n, ls(cur.l) + cur.c, (size_t)(e - cur.c));
            repl_n += e - cur.c;
            text_delete(cur, (Pos){ cur.l, e });
        } else {
            if (repl_n + 2 >= repl_cap) { repl_cap = repl_cap ? repl_cap * 2 : 64; repl_saved = realloc(repl_saved, (size_t)repl_cap); if (!repl_saved) pt_oom(); }
            repl_saved[repl_n++] = 0;
        }
    }
    ins_bytes(u, (size_t)n);
    ins_note_text(u, (size_t)n);
    if (o.tw > 0 && cp != ' ' && cp != '\t' && vcol_of(cur.l, cur.c) > o.tw) {   /* autowrap */
        /* break at the last blank that leaves the text before it within 'textwidth'; if the first word is longer than that, at
         * the first blank after it */
        const char *s = ls(cur.l);
        int ind0 = first_nonblank_raw(cur.l), brk = -1;
        for (int i = cur.c - 1; i > ind0; i--) {
            if ((s[i] == ' ' || s[i] == '\t') && vcol_of(cur.l, i) <= o.tw) { brk = i; break; }
        }
        if (brk < 0) {
            for (int i = ind0; i < cur.c; i++) if (s[i] == ' ' || s[i] == '\t') { brk = i; break; }
        }
        if (brk > ind0) {
            int ws = brk;
            while (ws > 0 && (s[ws - 1] == ' ' || s[ws - 1] == '\t')) ws--;
            int we = brk;
            while (we < (int)LN(cur.l).len && (s[we] == ' ' || s[we] == '\t')) we++;
            if (ws > 0 && we <= cur.c) {
                int ind = o.ai ? indent_of(cur.l) : 0;
                int moved = cur.c - we;
                Pos p = { cur.l, ws };
                text_delete(p, (Pos){ cur.l, we });
                text_insert(p, "\n", 1);
                cur.l = p.l + 1;
                if (ind) set_indent(cur.l, ind);
                cur.c = first_nonblank_raw(cur.l) + moved;
            }
        }
    }
}

/* The Tab key: a tab, or with 'expandtab' / 'softtabstop' spaces up to the next stop (blanks that reach a tab stop become a tab). */
static void ins_tab(void) {
    if (!o.et && o.sts == 0) { insert_cp('\t'); return; }
    int w = o.sts > 0 ? o.sts : o.ts;
    int n = w - vcol_of(cur.l, cur.c) % w;
    ai_line = -1;
    PtBuf sp = { 0 };
    for (int i = 0; i < n; i++) pb_addc(&sp, ' ');
    text_insert(cur, sp.p, sp.len);
    cur.c += n;
    pb_free(&sp);
    if (!o.et) {
        const char *s = ls(cur.l);
        int e = cur.c, b = e;
        while (b > 0 && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
        int v = vcol_of(cur.l, b), v1 = vcol_of(cur.l, e);
        PtBuf nb = { 0 };
        for (;;) { int next = (v / o.ts + 1) * o.ts; if (next > v1) break; pb_addc(&nb, '\t'); v = next; }
        while (v < v1) { pb_addc(&nb, ' '); v++; }
        if (nb.len != (size_t)(e - b) || memcmp(nb.p ? nb.p : "", s + b, nb.len)) {
            text_delete((Pos){ cur.l, b }, cur);
            text_insert((Pos){ cur.l, b }, nb.p ? nb.p : "", nb.len);
            cur.c = b + (int)nb.len;
        }
        pb_free(&nb);
    }
    ins_note_text("\t", 1);
}

int first_nonblank_raw(long l) {
    const char *s = ls(l);
    int i = 0;
    while (i < (int)LN(l).len && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

static void insert_newline(void) {
    int ind = 0;
    if (o.ai) ind = indent_of(cur.l);
    /* with autoindent, a line that has only the indent loses it when you press Enter */
    int only_ws = o.ai && ind > 0 && blank_line_pub(cur.l) && cur.c >= (int)LN(cur.l).len;
    ins_bytes("\n", 1);
    ins_note_text("\n", 1);
    if (only_ws) ln_set(cur.l - 1, "", 0);
    if (ind) {
        set_indent(cur.l, ind);
        cur.c = first_nonblank_raw(cur.l);
        ai_line = cur.l;
    }
}

static void backspace_char(void) {
    if (mode == M_REPLACE) {
        if (repl_n == 0) { if (cur.c > 0) cur.c = prev_c(cur.l, cur.c); return; }
        /* find the last saved entry */
        int idx = repl_n - 1;
        int saved_len = -1, start = -1;
        for (int i = repl_n - 1; i >= 0; i--) {
            int n = (unsigned char)repl_saved[i];
            if (i + 1 + n == repl_n && (n == 0 || n <= 4)) { saved_len = n; start = i; break; }
        }
        (void)idx;
        if (start < 0) return;
        int p = cur.c > 0 ? prev_c(cur.l, cur.c) : 0;
        text_delete((Pos){ cur.l, p }, cur);
        cur.c = p;
        if (saved_len > 0) text_insert(cur, repl_saved + start + 1, (size_t)saved_len);
        repl_n = start;
        return;
    }
    if (cur.c > 0 && o.sts > 0 && (ls(cur.l)[cur.c - 1] == ' ' || ls(cur.l)[cur.c - 1] == '\t')) {
        /* 'softtabstop': Backspace over blanks goes back to the previous multiple of it */
        int v = vcol_of(cur.l, cur.c), want = ((v - 1) / o.sts) * o.sts;
        while (cur.c > 0 && v > want && (ls(cur.l)[cur.c - 1] == ' ' || ls(cur.l)[cur.c - 1] == '\t')) {
            int p = cur.c - 1;
            text_delete((Pos){ cur.l, p }, cur);
            cur.c = p;
            v = vcol_of(cur.l, cur.c);
            if (ins_text.len) ins_text.len--;
        }
        if (v < want) {
            PtBuf sp = { 0 };
            for (; v < want; v++) pb_addc(&sp, ' ');
            text_insert(cur, sp.p, sp.len);
            cur.c += (int)sp.len;
            pb_free(&sp);
        }
    } else if (cur.c > 0) {
        int p = prev_c(cur.l, cur.c);
        text_delete((Pos){ cur.l, p }, cur);
        cur.c = p;
        if (ins_text.len) ins_text.len = pt_u8_prev_len(ins_text.p, ins_text.len);
    } else if (cur.l > 0) {
        int len = (int)LN(cur.l - 1).len;
        text_delete((Pos){ cur.l - 1, len }, (Pos){ cur.l, 0 });
        cur.l--;
        cur.c = len;
        if (ins_text.len) ins_text.len--;
    }
}

size_t pt_u8_prev_len(const char *s, size_t n) { return n ? pt_u8_prev(s, n) : 0; }

static void delete_word_back(void) {
    if (cur.c == 0) { backspace_char(); return; }
    int p = cur.c;
    const char *s = ls(cur.l);
    while (p > 0 && (s[p - 1] == ' ' || s[p - 1] == '\t')) p--;
    if (p > 0) {
        int cl = cls(cp_at(cur.l, prev_c(cur.l, p)), 0);
        while (p > 0 && cls(cp_at(cur.l, prev_c(cur.l, p)), 0) == cl) p = prev_c(cur.l, p);
    }
    text_delete((Pos){ cur.l, p }, cur);
    cur.c = p;
}

static void insert_register(int name) {
    Reg *r = reg_get(name);
    if (!r) return;
    size_t len = r->len;
    ins_bytes(r->s, len);
    ins_note_text(r->s, len);
    if (r->type == 'l') { /* linewise text already ends with a newline */ }
}

static char *complete_word(int dir, int *plen);

/* Handles one key in insert mode. Returns 1 when insert mode should end. */
static int insert_key(int k) {
    switch (k) {
    case K_ESC: case 3:
        return 1;
    case K_ENTER:
        abbr_check();
        insert_newline();
        break;
    case K_BS:
        backspace_char();
        break;
    case 23:
        delete_word_back();
        break;
    case 21: {                                           /* Ctrl-U: delete what was typed on this line */
        int to = (cur.l == ins_start.l && ins_start.c < cur.c) ? ins_start.c : 0;
        if (to == 0 && cur.c > 0) { int fnb = first_nonblank_raw(cur.l); if (fnb < cur.c) to = fnb; }
        if (cur.c > 0) { text_delete((Pos){ cur.l, to }, cur); cur.c = to; }
        else backspace_char();
        break;
    }
    case 20: {                                           /* Ctrl-T */
        int w = indent_of(cur.l);
        int old = (int)LN(cur.l).len;
        set_indent(cur.l, (w / o.sw + 1) * o.sw);
        cur.c += (int)LN(cur.l).len - old;
        break;
    }
    case 4: {                                            /* Ctrl-D */
        int w = indent_of(cur.l);
        int old = (int)LN(cur.l).len;
        set_indent(cur.l, w > 0 ? ((w - 1) / o.sw) * o.sw : 0);
        cur.c += (int)LN(cur.l).len - old;
        if (cur.c < 0) cur.c = 0;
        break;
    }
    case 18: {                                           /* Ctrl-R {reg} */
        int r = getkey();
        if (r == 18 || r == 15 || r == 16) r = getkey();
        insert_register(r);
        break;
    }
    case 1: ins_bytes(last_insert, last_insert_len); ins_note_text(last_insert, last_insert_len); break;
    case 22: case 17: {                                  /* Ctrl-V: a literal character or a decimal code */
        int c = getkey();
        if (c >= '0' && c <= '9') {
            int v = c - '0', n = 1;
            while (n < 3) { int d = getkey(); if (d >= '0' && d <= '9') { v = v * 10 + d - '0'; n++; } else { int tmp = d; ta_push(&tmp, 1); break; } }
            insert_cp((uint32_t)(v & 255));
        } else if (c == 'u' || c == 'x') {
            int v = 0, n = 0, max = c == 'u' ? 4 : 2;
            while (n < max) { int d = getkey(); if (isxdigit(d)) { v = v * 16 + (isdigit(d) ? d - '0' : (tolower(d) - 'a' + 10)); n++; } else { int tmp = d; ta_push(&tmp, 1); break; } }
            insert_cp((uint32_t)v);
        } else if (c < PT_K_BASE) insert_cp((uint32_t)c);
        break;
    }
    case 15: {                                           /* Ctrl-O: one normal-mode command */
        int saved = mode;
        int vm = vis_mode;
        mode = M_NORMAL;
        vis_mode = 0;
        if (cur.c >= (int)LN(cur.l).len && cur.c > 0 && 0) cur.c = last_c(cur.l);
        int at_end = cur.c >= (int)LN(cur.l).len;
        Pos before = cur;
        fix_cursor();
        (void)before;
        int dr = dot_set(0);
        normal_command();
        dot_set(dr);
        if (mode == M_NORMAL) mode = saved;
        vis_mode = vm;
        if (at_end && cur.l == before.l && cur.c == last_c(cur.l) && cur.c == before.c - 1) cur.c = (int)LN(cur.l).len;
        break;
    }
    case 14: case 16: {                                  /* Ctrl-N / Ctrl-P completion */
        int plen;
        char *w = complete_word(k == 14 ? 1 : -1, &plen);
        if (w) {
            text_delete((Pos){ cur.l, cur.c - plen }, cur);
            cur.c -= plen;
            ins_bytes(w, strlen(w));
            free(w);
        }
        break;
    }
    case 5: case 25: {                                   /* Ctrl-E / Ctrl-Y: the character below / above */
        long l = k == 5 ? cur.l + 1 : cur.l - 1;
        if (l >= 0 && l < nl) {
            int vc = vcol_of(cur.l, cur.c);
            int b = byte_at_vcol(l, vc);
            if (b < (int)LN(l).len) { int n = next_c(l, b) - b; ins_bytes(ls(l) + b, (size_t)n); ins_note_text(ls(l) + b, (size_t)n); }
        }
        break;
    }
    case 9:
        abbr_check();
        ins_tab();
        break;
    /* a cursor key leaves the autoindent behind and starts a new undo step */
    case PT_K_LEFT: ai_clean(); if (cur.c > 0) cur.c = prev_c(cur.l, cur.c); undo_begin(); ins_n = 0; break;
    case PT_K_RIGHT: ai_clean(); if (cur.c < (int)LN(cur.l).len) cur.c = next_c(cur.l, cur.c); undo_begin(); ins_n = 0; break;
    case PT_K_UP:
        ai_clean();
        if (cur.l > 0) { int v = vcol_of(cur.l, cur.c); cur.l--; cur.c = byte_at_vcol(cur.l, v); }
        undo_begin(); ins_n = 0; break;
    case PT_K_DOWN:
        ai_clean();
        if (cur.l < nl - 1) { int v = vcol_of(cur.l, cur.c); cur.l++; cur.c = byte_at_vcol(cur.l, v); }
        undo_begin(); ins_n = 0; break;
    case K_SLEFT: case K_SRIGHT: case K_CLEFT: case K_CRIGHT: {      /* a word / a WORD to the left or right */
        ai_clean();
        Motion m;
        memset(&m, 0, sizeof m);
        if (motion(norm_key(k), 1, 0, 0, &m) && !m.fail) cur = m.to;
        if (cur.c > (int)LN(cur.l).len) cur.c = (int)LN(cur.l).len;
        undo_begin(); ins_n = 0;
        break;
    }
    case PT_K_HOME: ai_clean(); cur.c = 0; undo_begin(); ins_n = 0; break;
    case PT_K_END: ai_clean(); cur.c = (int)LN(cur.l).len; undo_begin(); ins_n = 0; break;
    case PT_K_DEL:
        if (cur.c < (int)LN(cur.l).len) text_delete(cur, (Pos){ cur.l, next_c(cur.l, cur.c) });
        else if (cur.l < nl - 1) text_delete(cur, (Pos){ cur.l + 1, 0 });
        break;
    case PT_K_INS: mode = mode == M_INSERT ? M_REPLACE : M_INSERT; break;
    case PT_K_PGUP: case PT_K_PGDN: break;
    default:
        if (k >= 32 && k < PT_K_BASE) {
            if (!pt_is_word_cp((uint32_t)k)) abbr_check();       /* a non-keyword character ends the word: expand an abbreviation */
            insert_cp((uint32_t)k);
        }
        break;
    }
    return 0;
}

/* :abbreviate: the keyword just before the cursor is replaced by its expansion when a non-keyword character, Enter, Tab or Esc
 * is typed. */
static void abbr_check(void) {
    int e = cur.c, st = e;
    if (e == 0) return;
    while (st > 0 && pt_is_word_cp(cp_at(cur.l, prev_c(cur.l, st)))) st = prev_c(cur.l, st);
    if (st == e) return;
    const char *rhs = abbr_find(ls(cur.l) + st, (size_t)(e - st));
    if (!rhs) return;
    char *word = strndup(ls(cur.l) + st, (size_t)(e - st));
    text_delete((Pos){ cur.l, st }, cur);
    cur.c = st;
    ins_bytes(rhs, strlen(rhs));
    if (!ins_replay) {
        size_t wl = (size_t)(e - st);
        if (ins_text.len >= wl && !memcmp(ins_text.p + ins_text.len - wl, word, wl)) ins_text.len -= wl;
        pb_add(&ins_text, rhs, strlen(rhs));
    }
    free(word);
}

static char *complete_word(int dir, int *plen) {
    static char prefix[128];
    static char *cands[256];
    static int ncand, idx = -1, last_len;
    static long seq = -1;
    const char *s = ls(cur.l);
    int e = cur.c, st = e;
    if (seq == undo_seq + 1000000 && 0) { }
    while (st > 0 && pt_is_word_cp(cp_at(cur.l, prev_c(cur.l, st)))) st = prev_c(cur.l, st);
    int plen_now = e - st;
    if (idx >= 0 && last_key_was_complete && plen_now == last_len) {
        /* cycling: the "prefix" is the previous completion */
    } else {
        for (int i = 0; i < ncand; i++) free(cands[i]);
        ncand = 0; idx = -1;
        if (plen_now <= 0 || plen_now >= 127) return NULL;
        memcpy(prefix, s + st, (size_t)plen_now);
        prefix[plen_now] = 0;
        for (long off = 0; off < nl && ncand < 256; off++) {
            long l = (cur.l + (dir > 0 ? off : -off) + nl) % nl;
            const char *t = ls(l);
            int len = (int)LN(l).len;
            for (int i = 0; i < len && ncand < 256;) {
                if (pt_is_word_cp(cp_at(l, i))) {
                    int j = i;
                    while (j < len && pt_is_word_cp(cp_at(l, j))) j = next_c(l, j);
                    if (j - i > plen_now && !strncmp(t + i, prefix, (size_t)plen_now) && !(l == cur.l && i == st)) {
                        int dup = 0;
                        for (int c = 0; c < ncand; c++) if ((int)strlen(cands[c]) == j - i && !strncmp(cands[c], t + i, (size_t)(j - i))) dup = 1;
                        if (!dup) { cands[ncand] = strndup(t + i, (size_t)(j - i)); ncand++; }
                    }
                    i = j;
                } else i = next_c(l, i);
            }
        }
        if (!ncand) return NULL;
    }
    (void)seq;
    if (!ncand) return NULL;
    idx = (idx + 1) % (ncand + 1);
    char *res;
    if (idx == ncand) res = strdup(prefix);
    else res = strdup(cands[idx]);
    *plen = plen_now;
    last_len = (int)strlen(res);
    last_key_was_complete = 1;
    return res;
}

int last_key_was_complete;

/* Runs insert mode until Esc. `kind` is 'i', 'o', 'O' or 'R' (for repeating the typed text count-1 times). */
static void insert_session(int cnt, int kind) {
    if (mode != M_REPLACE) mode = kind == 'R' ? M_REPLACE : M_INSERT;
    ins_n = 0;
    ins_start = cur;
    pb_clear(&ins_text);
    repl_n = 0;
    int prev_k = 0;
    for (;;) {
        if (!normal_depth && !in_global) scroll_to_cursor();
        int k = getkey();
        last_key_was_complete = (k == 14 || k == 16) && (prev_k == 14 || prev_k == 16);
        prev_k = k;
        if (k == K_ESC) abbr_check();
        if (insert_key_wrapper(k)) break;
        if (k != K_ESC) ins_record(k);
        msg[0] = 0;
    }
    if (cnt > 1) {
        int saved = ins_replay;
        ins_replay = 1;
        int dr = dot_set(0);
        int *keys = malloc((size_t)(ins_n + 1) * sizeof *keys);
        if (!keys) pt_oom();
        int nk = ins_n;
        memcpy(keys, ins_keys, (size_t)nk * sizeof *keys);
        for (int rep = 1; rep < cnt; rep++) {
            if (kind == 'o') { long at = cur.l + 1; ln_insert(at, "", 0); cur.l = at; cur.c = 0; if (o.ai) { set_indent(cur.l, indent_of(cur.l - 1)); cur.c = (int)LN(cur.l).len; } }
            else if (kind == 'O') { }
            int *all = malloc((size_t)(nk + 1) * sizeof *all);
            memcpy(all, keys, (size_t)nk * sizeof *all);
            all[nk] = K_ESC;
            for (int i = 0; i <= nk; i++) all[i] |= K_NOMAP;
            ta_push(all, nk + 1);
            free(all);
            for (;;) { int k = getkey(); if (k == K_ESC) abbr_check(); if (insert_key_wrapper(k)) break; }
        }
        free(keys);
        dot_set(dr);
        ins_replay = saved;
    }
    /* leave insert mode */
    ai_clean();
    last_insert_len = ins_text.len < sizeof last_insert ? ins_text.len : sizeof last_insert - 1;
    if (ins_text.len) memcpy(last_insert, ins_text.p, last_insert_len);
    mode = M_NORMAL;
    mark_rb = cur;
    mark_lb = ins_start;
    mark_ins = cur;                                      /* '^: where insert mode stopped (gi starts here) */
    if (cur.c > 0) cur.c = prev_c(cur.l, cur.c);
    mark_dot = cur;
    fix_cursor();
    update_curswant();
}

static int insert_key_wrapper(int k) {
    if (k == K_ESC || k == 3) return 1;
    return insert_key(k);
}

/* Starts an insert command: cmdchar is one of i a I A o O (also 'g' = gI, 'G' = gi) or 'c' (after a change operator). */
void enter_insert(int cmdchar, int cnt) {
    cmd_is_change = 1;
    switch (cmdchar) {
    case 'i': break;
    case 'a': if (LN(cur.l).len) cur.c = next_c(cur.l, cur.c); break;
    case 'I': cur.c = first_nonblank_raw(cur.l); break;
    case 'A': cur.c = (int)LN(cur.l).len; break;
    case 'g': cur.c = 0; break;                          /* gI */
    case 'G': if (mark_ins.l >= 0) { cur = mark_ins; if (cur.l >= nl) cur.l = nl - 1; cur.c = clamp_col(cur.l, cur.c, 1); } break;
    case 'o': {
        int ind = o.ai ? indent_of(cur.l) : 0;
        ln_insert(cur.l + 1, "", 0);
        cur.l++;
        cur.c = 0;
        if (ind) { set_indent(cur.l, ind); cur.c = (int)LN(cur.l).len; ai_line = cur.l; }
        break;
    }
    case 'O': {
        int ind = o.ai ? indent_of(cur.l) : 0;
        ln_insert(cur.l, "", 0);
        cur.c = 0;
        if (ind) { set_indent(cur.l, ind); cur.c = (int)LN(cur.l).len; ai_line = cur.l; }
        break;
    }
    case 'R': mode = M_REPLACE; break;
    default: break;
    }
    mode = cmdchar == 'R' ? M_REPLACE : M_INSERT;
    insert_session(cnt, cmdchar == 'o' || cmdchar == 'O' ? 'o' : (cmdchar == 'R' ? 'R' : 'i'));
}

/* ------------------------------------------------------------------ visual mode helpers */

static void vis_save(void) {
    last_vis_mode = vis_mode;
    last_vis_start = vis_start;
    last_vis_end = cur;
    last_vis_curswant = curswant;
    Pos a = vis_start, b = cur;
    if (a.l > b.l || (a.l == b.l && a.c > b.c)) { Pos t = a; a = b; b = t; }
    mark_lt = a;
    mark_gt = b;
    if (vis_mode == M_VLINE) { mark_lt.c = 0; mark_gt.c = imax(0, (int)LN(b.l).len - 1); }
}

void visual_end(void) {
    if (!vis_mode) return;
    vis_save();
    vis_mode = 0;
    fix_cursor();
}

static void visual_range(Range *r) {
    Pos a = vis_start, b = cur;
    if (a.l > b.l || (a.l == b.l && a.c > b.c)) { Pos t = a; a = b; b = t; }
    memset(r, 0, sizeof *r);
    if (vis_mode == M_VLINE) { r->type = 'l'; r->a = (Pos){ a.l, 0 }; r->b = (Pos){ b.l, 0 }; return; }
    if (vis_mode == M_VBLOCK) { block_range_from_visual(r); return; }
    r->type = 'c';
    r->a = a;
    r->b = b;
    if (b.c < (int)LN(b.l).len) r->b.c = next_c(b.l, b.c);
    if (b.c >= (int)LN(b.l).len || curswant == -1) {     /* the end is on an empty line, or $ was used: the line break is selected too */
        if (b.l + 1 < nl) { r->b.l = b.l + 1; r->b.c = 0; }
        else r->b.c = (int)LN(b.l).len;
    }
}

/* ------------------------------------------------------------------ normal commands */

static int get_count_digit(int k, int count) {
    if (k >= '1' && k <= '9') return count * 10 + (k - '0');
    if (k == '0' && count > 0) return count * 10;
    return -1;
}

static void do_replace_chars(int cnt) {
    int k = getkey();
    if (k == K_ESC || k >= PT_K_BASE) return;
    if (k == 22) k = getkey();
    long l = cur.l;
    int pos = cur.c, end = cur.c;
    for (int i = 0; i < cnt; i++) {
        if (end >= (int)LN(l).len) { set_err("E?: not enough characters"); return; }
        end = next_c(l, end);
    }
    cmd_is_change = 1;
    char u[4];
    if (k == K_ENTER) {
        text_delete((Pos){ l, pos }, (Pos){ l, end });
        text_insert((Pos){ l, pos }, "\n", 1);
        cur.l = l + 1;
        cur.c = 0;
        return;
    }
    int n = pt_u8_encode((uint32_t)k, u);
    PtBuf b = { 0 };
    for (int i = 0; i < cnt; i++) pb_add(&b, u, (size_t)n);
    text_delete((Pos){ l, pos }, (Pos){ l, end });
    text_insert((Pos){ l, pos }, b.p, b.len);
    cur.c = pos + (int)b.len - n;
    pb_free(&b);
}

static void do_increment(int delta) {
    long l = cur.l;
    const char *s = ls(l);
    int len = (int)LN(l).len, i = cur.c;
    /* find a number at or after the cursor; step back over digits we are inside */
    while (i > 0 && isdigit((unsigned char)s[i]) && isdigit((unsigned char)s[i - 1])) i--;
    int hex = 0;
    if (i >= 2 && s[i] != 'x' && (s[i - 1] == 'x' || s[i - 1] == 'X') && s[i - 2] == '0' && isxdigit((unsigned char)s[i])) { i -= 2; }
    while (i < len && !isdigit((unsigned char)s[i])) i++;
    if (i >= len) { beep(); return; }
    int st = i;
    if (s[st] == '0' && st + 2 < len && (s[st + 1] == 'x' || s[st + 1] == 'X') && isxdigit((unsigned char)s[st + 2])) hex = 1;
    else if (st > 0 && (s[st - 1] == 'x' || s[st - 1] == 'X') && st > 1 && s[st - 2] == '0') { st -= 2; hex = 1; }
    int e = st;
    char out[40];
    int numstart = st;
    if (hex) {
        e = st + 2;
        while (e < len && isxdigit((unsigned char)s[e])) e++;
        unsigned long v = strtoul(s + st + 2, NULL, 16);
        v += (unsigned long)(long)delta;
        int upper = 0;
        for (int k = st + 2; k < e; k++) if (isupper((unsigned char)s[k])) upper = 1;
        snprintf(out, sizeof out, upper ? "0x%0*lX" : "0x%0*lx", e - st - 2, v);
    } else {
        e = st;
        int neg = st > 0 && s[st - 1] == '-';
        while (e < len && isdigit((unsigned char)s[e])) e++;
        long long v = strtoll(s + st, NULL, 10);
        if (neg) { v = -v; numstart = st - 1; }
        v += delta;
        int digits = e - st;
        int leading_zero = s[st] == '0' && digits > 1;
        if (leading_zero) snprintf(out, sizeof out, v < 0 ? "-%0*lld" : "%0*lld", v < 0 ? digits : digits, v < 0 ? -v : v);
        else snprintf(out, sizeof out, "%lld", v);
    }
    text_delete((Pos){ l, numstart }, (Pos){ l, e });
    text_insert((Pos){ l, numstart }, out, strlen(out));
    cur.c = numstart + (int)strlen(out) - 1;
    cmd_is_change = 1;
}

/* Ctrl-F / Ctrl-B: a page, with two lines of the old window kept. The cursor goes to the top (forward) or bottom (back) line. */
static void scroll_page(int dir, int cnt) {
    int th = text_rows();
    if (th <= 0) return;
    for (int i = 0; i < cnt; i++) {
        if (dir > 0) {
            if (topline >= nl - 1 && win_botline() >= nl) { beep(); break; }
            if (win_botline() >= nl) topline = nl - 1;                   /* the end is in view: only the last line stays */
            else {
                long nt = win_botline() - 2;
                if (nt <= topline) nt = topline + 1;
                topline = nt;
            }
            topoff = 0;
            cur.l = topline;
        } else {
            if (topline == 0 && topoff == 0) { beep(); break; }
            long nb = topline + 1;                                       /* the new last line: the second line of the old window */
            if (nb > nl - 1) nb = nl - 1;
            long t = nb, used = rows_of(nb);
            while (t > 0 && used + rows_of(t - 1) <= th) { t--; used += rows_of(t); }
            topline = t;
            topoff = 0;
            cur.l = nb;
        }
    }
    if (cur.l >= nl) cur.l = nl - 1;
    cur.c = first_nonblank(cur.l);
}

/* Ctrl-D / Ctrl-U: half a window (or the count given before), cursor and window move together. */
static void scroll_half(int dir, int cnt, int hascnt) {
    int th = text_rows();
    if (th <= 0) return;
    if (hascnt) scroll_amount = cnt;
    int n = scroll_amount > 0 ? scroll_amount : (th / 2 > 0 ? th / 2 : 1);
    topoff = 0;
    if (dir > 0) {
        if (cur.l >= nl - 1) { beep(); return; }
        for (int i = 0; i < n && win_botline() < nl; i++) topline++;
        cur.l = imin(nl - 1, cur.l + n);
    } else {
        if (cur.l == 0) { beep(); return; }
        for (int i = 0; i < n && topline > 0; i++) topline--;
        cur.l = imax(0, cur.l - n);
    }
    cur.c = first_nonblank(cur.l);
}

/* z commands: zt z<CR> (cursor line at the top), zz z. (middle), zb z- (bottom); a count first goes to that line. */
static void do_z(int cnt, int hascnt) {
    int k = getkey();
    switch (k) {
    case K_ENTER: case 't': case '+': case '.': case 'z': case '-': case 'b': case '^':
        if (hascnt) { cur.l = imin(cnt, nl) - 1; cur.c = clamp_col(cur.l, cur.c, 0); }
        if (k == '+' && !hascnt) { cur.l = imin(nl - 1, win_botline()); cur.c = first_nonblank(cur.l); }
        if (k == '^' && !hascnt) { cur.l = imax(0, topline - 1); cur.c = first_nonblank(cur.l); }
        if (k == K_ENTER || k == 't' || k == '+') { topline = cur.l; topoff = 0; }
        else if (k == '.' || k == 'z') scroll_halfway();
        else scroll_cursor_bottom();
        if (k == K_ENTER || k == '.' || k == '-' || k == '+' || k == '^') cur.c = first_nonblank(cur.l);
        break;
    case 'h': case PT_K_LEFT: case 'l': case PT_K_RIGHT: case 'H': case 'L': case 's': case 'e': break;   /* sideways scrolling is automatic */
    default: beep(); break;
    }
}

static int exec_macro(int reg, int cnt) {
    static int last_macro;
    if (reg == '@') reg = last_macro;
    if (!reg) { set_err("E748: No previously used register"); return 0; }
    if (reg == ':') {
        last_macro = ':';                                /* @@ repeats it */
        for (int i = 0; i < cnt; i++) ex_execute(last_ex);
        return 1;
    }
    Reg *r = reg_get(reg);
    if (!r) { beep(); return 0; }
    last_macro = reg;
    for (int i = 0; i < cnt; i++) ta_push_str(r->s, r->len, 0);
    if (r->type == 'l' && r->len && r->s[r->len - 1] != '\n') { }
    return 1;
}

static void visual_block_insert(int append, int change) {
    Range r;
    block_range_from_visual(&r);
    long first = r.a.l, last = r.b.l;
    int vc = append ? (r.toeol ? -1 : r.vc2 + 1) : r.vc1;
    if (change) {
        size_t n;
        char *t = range_text(&r, &n);
        reg_set(0, t, n, 'b', 1);
        free(t);
        range_delete(&r);
    }
    vis_mode = 0;
    cur.l = first;
    if (vc < 0) cur.c = (int)LN(first).len;
    else {
        int lw = line_vwidth(first);
        if (lw < vc && append) { PtBuf pad = { 0 }; for (int k = lw; k < vc; k++) pb_addc(&pad, ' '); text_insert((Pos){ first, (int)LN(first).len }, pad.p, pad.len); pb_free(&pad); }
        cur.c = lw <= vc ? (int)LN(first).len : byte_at_vcol(first, vc);
    }
    Pos start = cur;
    mode = M_INSERT;
    int dr = dot_set(1);
    (void)dr;
    insert_session(1, 'i');
    /* copy what was typed on the first line to the other lines */
    if (cur.l == first || 1) {
        long tl = first;
        int a = start.c;
        int e = cur.c + 1 > (int)LN(tl).len ? (int)LN(tl).len : cur.c + 1;
        if (e > (int)LN(tl).len || (LN(tl).len && cur.c < a)) e = (int)LN(tl).len;
        size_t typed = last_insert_len;
        if (typed > 0 && !memchr(last_insert, '\n', typed)) {
            (void)e;
            for (long i = first + 1; i <= last && i < nl; i++) {
                int pos;
                int lw = line_vwidth(i);
                if (vc < 0) pos = (int)LN(i).len;
                else if (lw < vc) { if (!append) continue; PtBuf pad = { 0 }; for (int k = lw; k < vc; k++) pb_addc(&pad, ' '); text_insert((Pos){ i, (int)LN(i).len }, pad.p, pad.len); pb_free(&pad); pos = (int)LN(i).len; }
                else pos = byte_at_vcol(i, vc);
                text_insert((Pos){ i, pos }, last_insert, typed);
            }
        }
    }
    cur.l = first;
    cur.c = clamp_col(first, a_start_col(start, first), 0);
    fix_cursor();
}

int a_start_col(Pos start, long first) { (void)first; return start.c; }

/* ------------------------------------------------------------------ operators */

/* Which motions always put a delete in "1 (even inside one line). */
static int motion_uses_reg1(int k) { return k == '%' || k == '(' || k == ')' || k == '`' || k == '/' || k == '?' || k == 'n' || k == 'N' || k == '{' || k == '}'; }

/* An operator has been typed (op: d c y < > ! = or u U ~ ? q w for the g operators). Reads its count, the motion or text object
 * and applies it. */
static void op_pending(int op, int count, int reg, int is_g) {
    int keys0 = dot_len();
    int count2 = 0, kk, ndig = 0;
    for (;;) {
        kk = norm_key(getkey());
        int nc = get_count_digit(kk, count2);
        if (nc >= 0) { count2 = nc; ndig++; continue; }
        break;
    }
    if (ndig) { dot_set_len(keys0); dot_push(kk); }     /* the count is not part of the repeated keys: it is a count of "." */
    int total = (count ? count : 1) * (count2 ? count2 : 1);
    int hastotal = count || count2;
    cur_count = hastotal ? total : 0;
    int force = 0;
    while (kk == 'v' || kk == 'V' || kk == 22) { force = kk; kk = norm_key(getkey()); }
    if (kk == K_ESC || kk == 3) return;
    Range r;
    memset(&r, 0, sizeof r);
    int dbl = kk == op;
    if (is_g && kk == 'g') {                             /* gUgU, g~g~, gqgq: doubled; otherwise a motion such as gg or ge */
        int k3 = getkey();
        if (k3 == op) dbl = 1;
        else motion_pushback(k3);
    }
    if (dbl) {                                           /* dd, >>, gUU ...: that many lines */
        if (total > 1 && cur.l >= nl - 1) { beep(); return; }
        r.type = 'l';
        r.a = cur;
        r.b = (Pos){ imin(nl - 1, cur.l + total - 1), 0 };
    } else if (kk == 'a' || kk == 'i') {
        int oc = getkey();
        if (!textobj(oc, kk == 'i', total, &r)) { beep(); return; }
        if (force == 'V' && r.type == 'c') { r.type = 'l'; r.a.c = 0; }
    } else {
        Motion mm;
        if (!motion(kk, total, hastotal, op, &mm)) { beep(); return; }
        if (mm.fail || mm.vcol == -3) { if (!msg[0]) beep(); return; }
        if (force == 'v') { if (mm.linewise) { mm.linewise = 0; mm.incl = 0; } else mm.incl = !mm.incl; }
        else if (force == 'V') mm.linewise = 1;
        Pos a = cur, b = mm.to;
        if (mm.jump) push_jump();
        r.reg1 = motion_uses_reg1(kk);
        if (mm.linewise) {
            if (a.l > b.l || (a.l == b.l && a.c > b.c)) { Pos t = a; a = b; b = t; }
            r.type = 'l'; r.a = a; r.b = (Pos){ b.l, 0 };
        } else {
            if (a.l > b.l || (a.l == b.l && a.c > b.c)) { Pos t = a; a = b; b = t; }
            r.type = 'c';
            if (mm.incl) b.c = b.c < (int)LN(b.l).len ? next_c(b.l, b.c) : b.c;
            else if (b.c == 0 && b.l > a.l && !force) {   /* exclusive, ends in column 0: the end moves to the end of the line before it */
                b.l--;
                b.c = (int)LN(b.l).len;
                int before_nb = 1;                       /* ... and if the start is not after the first non-blank, it is linewise */
                const char *s = ls(a.l);
                for (int i = 0; i < a.c; i++) if (s[i] != ' ' && s[i] != '\t') before_nb = 0;
                if (before_nb) { r.type = 'l'; a.c = 0; }
            }
            r.a = a; r.b = b;
        }
        if (force == 22) r.type = 'b';
    }
    do_op_range(op, &r, reg, 1);
    if (op != 'y') cmd_is_change = 1;
    if (op == 'c') enter_insert_after_op();
    else if (mode == M_NORMAL && op != 'y' && op != '!') fix_cursor();
}

/* ------------------------------------------------------------------ Visual mode commands */

static void visual_redo_record(int vm) {
    VisRedo v;
    Pos a = vis_start, b = cur;
    if (a.l > b.l || (a.l == b.l && a.c > b.c)) { Pos t = a; a = b; b = t; }
    memset(&v, 0, sizeof v);
    v.mode = vm;
    v.lines = b.l - a.l + 1;
    v.toeol = curswant < 0;
    if (vm == M_VBLOCK) {
        int v1 = vcol_of(vis_start.l, vis_start.c), v2 = vcol_of(cur.l, cur.c);
        int e1 = vcol_end(vis_start.l, vis_start.c), e2 = vcol_end(cur.l, cur.c);
        v.vcols = (int)(imax(e1, e2) - imin(v1, v2) + 1);
    } else if (vm == M_VISUAL) {
        if (v.lines <= 1) v.vcols = vcol_end(b.l, b.c) - vcol_of(a.l, a.c) + 1;
        else v.vcols = vcol_end(b.l, b.c);
    }
    dot_set_visual(&v);
}

/* The "." of a Visual change: an area of the same size starting at the cursor. */
static void visual_redo_select(const VisRedo *v) {
    vis_mode = v->mode;
    vis_start = cur;
    Pos e = cur;
    e.l = imin(nl - 1, cur.l + v->lines - 1);
    if (v->mode == M_VLINE) { e.c = cur.c; curswant = vcol_end(cur.l, cur.c); }
    else if (v->toeol) { e.c = LN(e.l).len ? last_c(e.l) : 0; curswant = -1; }
    else if (v->mode == M_VBLOCK || v->lines <= 1) {
        int want = vcol_of(cur.l, cur.c) + v->vcols - 1;
        e.c = clamp_col(e.l, byte_at_vcol(e.l, want), 0);
        curswant = want;
    } else {
        e.c = clamp_col(e.l, byte_at_vcol(e.l, v->vcols), 0);
        curswant = v->vcols;
    }
    cur = e;
}

/* Replaces the selection with the contents of a register (v_p). */
static void visual_put(int op, int reg, int n1, Range *r, int vm) {
    Reg *rg = reg_get(reg);
    if (!rg) { set_err("E353: Nothing in register %c", reg ? reg : '"'); return; }
    char *saved = malloc(rg->len + 1);
    if (!saved) pt_oom();
    memcpy(saved, rg->s, rg->len);
    size_t slen = rg->len;
    int stype = rg->type;
    size_t n;
    char *t = range_text(r, &n);
    int vtype = r->type;
    long old_nl = nl;
    range_delete(r);
    if (op == 'p') reg_set(0, t, n, vtype, 1);
    free(t);
    cmd_is_change = 1;
    (void)vm;
    if (vtype == 'l') {
        int all_gone = r->a.l == 0 && r->b.l == old_nl - 1;
        if (stype == 'l') {
            if (all_gone) { cur.l = 0; cur.c = 0; put_text(saved, slen, stype, 0, n1, 0); ln_delete(nl - 1, 1); }
            else if (r->a.l >= nl) { cur.l = nl - 1; put_text(saved, slen, stype, 1, n1, 0); }
            else { cur.l = r->a.l; put_text(saved, slen, stype, 0, n1, 0); }
        } else {
            long at = all_gone ? 0 : (r->a.l >= nl ? nl : r->a.l);
            if (all_gone) { cur.l = 0; cur.c = 0; }
            else { ln_insert(at, "", 0); cur.l = at; cur.c = 0; }
            put_text(saved, slen, stype, 0, n1, 0);
        }
    } else {
        cur = r->a;
        if (stype == 'l') {                              /* linewise text into a charwise selection: it gets its own lines */
            text_insert(cur, "\n", 1);
            cur.c = 0;
            put_text(saved, slen, stype, 1, n1, 0);
        } else {
            int at_eol = r->a.c >= (int)LN(r->a.l).len && LN(r->a.l).len > 0;
            if (at_eol) { cur.c = last_c(cur.l); put_text(saved, slen, stype, 1, n1, 0); }
            else { cur.c = r->a.c; put_text(saved, slen, stype, 0, n1, 0); }
        }
    }
    free(saved);
}

static void visual_replace_chars(const Range *r, int ch) {
    for (long i = r->a.l; i <= r->b.l; i++) {
        int s = 0, e = (int)LN(i).len;
        if (r->type == 'c') { if (i == r->a.l) s = r->a.c; if (i == r->b.l) e = imin(r->b.c, e); }
        else if (r->type == 'b') {
            if (line_vwidth(i) <= r->vc1) continue;
            s = byte_at_vcol(i, r->vc1);
            e = r->toeol || line_vwidth(i) <= r->vc2 + 1 ? (int)LN(i).len : byte_at_vcol(i, r->vc2 + 1);
        }
        if (e <= s) continue;
        PtBuf b = { 0 };
        char u[4];
        int un = pt_u8_encode((uint32_t)ch, u);
        for (int p = s; p < e; p = next_c(i, p)) pb_add(&b, u, (size_t)un);
        text_delete((Pos){ i, s }, (Pos){ i, e });
        text_insert((Pos){ i, s }, b.p, b.len);
        pb_free(&b);
    }
}

#define GCMD 0x180000                                    /* added to the second key of a "g" command in a switch with the plain keys */

/* One key typed in Visual mode. */
static void visual_cmd(int k, int count, int reg) {
    int n1 = count > 0 ? count : 1, hascount = count > 0, vm = vis_mode;
    Motion m;
    memset(&m, 0, sizeof m);
    if (k != 'a' && k != 'i' && motion(k, count, hascount, 0, &m)) {
        if (m.vcol == -3) { dot_end(0); return; }
        if (m.fail) { if (!msg[0] || !msg_err) beep(); dot_end(0); return; }
        if (m.jump) push_jump();
        cur = m.to;
        fix_cursor();
        if (m.vcol == -2) curswant = -1;
        else if (!m.keepcol) update_curswant();
        dot_end(0);
        return;
    }
    int g = 0;
    if (k == 'g' && m.fail == 2) { g = m.vcol; m.fail = 0; }
    int op = 0, as_lines = 0, to_eol = 0;
    switch (g ? GCMD + g : k) {
    case 27: case 3: visual_end(); dot_end(0); return;
    case 'v': case 'V': case 22: {
        int nm = k == 'v' ? M_VISUAL : k == 'V' ? M_VLINE : M_VBLOCK;
        if (vis_mode == nm) visual_end(); else vis_mode = nm;
        dot_end(0);
        return;
    }
    case 'o': case 'O':
        if (k == 'O' && vis_mode == M_VBLOCK) {
            int v1 = vcol_of(vis_start.l, vis_start.c), v2 = vcol_of(cur.l, cur.c);
            vis_start.c = byte_at_vcol(vis_start.l, v2); cur.c = byte_at_vcol(cur.l, v1);
        } else { Pos t = vis_start; vis_start = cur; cur = t; }
        update_curswant();
        dot_end(0);
        return;
    case 'a': case 'i': {
        int c2 = getkey();
        Range r;
        if (!textobj(c2, k == 'i', count, &r)) { beep(); dot_end(0); return; }
        int fresh = vis_start.l == cur.l && vis_start.c == cur.c;
        if (r.type == 'l') {
            if (vis_mode == M_VISUAL) vis_mode = M_VLINE;
            if (fresh) vis_start = (Pos){ r.a.l, 0 };
            cur.l = r.b.l; cur.c = first_nonblank(cur.l);
        } else {
            Pos e = r.b;
            if (e.c > 0) e.c = prev_c(e.l, e.c); else if (e.l > r.a.l) { e.l--; e.c = last_c(e.l); }
            /* a word, sentence or paragraph is added to the selection; a block, tag or quote replaces it by a bigger one */
            int additive = c2 == 'w' || c2 == 'W' || c2 == 's' || c2 == 'p';
            if (fresh || (!additive && (r.a.l < vis_start.l || (r.a.l == vis_start.l && r.a.c < vis_start.c)))) vis_start = r.a;
            cur = e;
        }
        fix_cursor();
        update_curswant();
        dot_end(0);
        return;
    }
    case ':':
        visual_end();
        ex_line_prompt("'<,'>");
        dot_end(0);
        return;
    case GCMD + 'v':                                     /* gv: swap with the previous selection */
        if (last_vis_mode) { Pos s = last_vis_start, e = last_vis_end; int lm = last_vis_mode, lc = last_vis_curswant; vis_save(); vis_mode = lm; vis_start = s; cur = e; curswant = lc; fix_cursor(); }
        dot_end(0);
        return;
    case 'I': case 'A':
        if (!cmd_ok_modify()) { visual_end(); dot_end(0); return; }
        if (vm == M_VBLOCK) { cmd_is_change = 1; visual_block_insert(k == 'A', 0); dot_end(0); return; }
        visual_end();
        if (k == 'I') { cur.l = mark_lt.l; cur.c = 0; enter_insert('i', 1); }
        else { cur.l = mark_gt.l; enter_insert('A', 1); }
        dot_end(cmd_is_change);
        return;
    case 'd': case 'x': case PT_K_DEL: op = 'd'; break;
    case 'X': op = 'd'; as_lines = 1; break;
    case 'D': op = 'd'; if (vm == M_VBLOCK) to_eol = 1; else as_lines = 1; break;
    case 'y': op = 'y'; break;
    case 'Y': op = 'y'; as_lines = 1; break;
    case 'c': case 's': op = 'c'; break;
    case 'S': case 'R': op = 'c'; as_lines = 1; break;
    case 'C': op = 'c'; if (vm == M_VBLOCK) to_eol = 1; else as_lines = 1; break;
    case '<': case '>': case '!': case '=': op = k; break;
    case 'u': case 'U': case '~': op = k; break;
    case GCMD + 'u': case GCMD + 'U': case GCMD + '~': case GCMD + '?': case GCMD + 'q': case GCMD + 'w': op = g; break;
    case 'J': case GCMD + 'J': op = 'J'; break;
    case 'r': op = 'r'; break;
    case 'p': case 'P': op = k; break;
    case 'z': do_z(count, hascount); dot_end(0); return;
    default: beep(); dot_end(0); return;
    }
    if (op != 'y' && !cmd_ok_modify()) { visual_end(); dot_end(0); return; }

    Range r;
    visual_range(&r);
    if (as_lines && vm != M_VBLOCK) r.type = 'l';
    if (to_eol) r.toeol = 1;
    if (as_lines && vm == M_VBLOCK) { r.type = 'l'; }
    if (op != 'y') visual_redo_record(vm);
    if (op == 'c' && vm == M_VBLOCK) {
        vis_save();
        cmd_is_change = 1;
        visual_block_insert(0, 1);
        dot_end(0);
        return;
    }
    vis_save();
    vis_mode = 0;
    if (op == 'J') {
        join_lines(r.a.l, r.b.l - r.a.l + 1, g == 0);
        cmd_is_change = 1;
        dot_end(1);
        return;
    }
    if (op == 'r') {
        int ch = getkey();
        if (ch == K_ESC || ch >= PT_K_BASE) { dot_end(0); return; }
        if (ch == 22) ch = getkey();
        cmd_is_change = 1;
        visual_replace_chars(&r, ch);
        cur = r.a;
        if (r.type == 'l') cur.c = 0;
        fix_cursor();
        dot_end(1);
        return;
    }
    if (op == 'p' || op == 'P') {
        visual_put(op, reg, n1, &r, vm);
        dot_end(cmd_is_change);
        return;
    }
    do_op_range(op, &r, reg, count);
    if (op == 'c') { enter_insert_after_op(); dot_end(1); return; }
    if (op != 'y') cmd_is_change = 1;
    dot_end(cmd_is_change);
    if (mode == M_NORMAL && op != '!') fix_cursor();
    update_curswant();
}

/* ------------------------------------------------------------------ g commands */

/* A "g" command that is not a motion; g is the second key. Returns 1 if the command was one that goes into the "." record. */
static void g_command(int g, int count, int reg) {
    int n1 = count > 0 ? count : 1;
    switch (g) {
    case 'u': case 'U': case '~': case '?': case 'q': case 'w':
        if (!cmd_ok_modify()) { dot_end(0); return; }
        op_pending(g, count, reg, 1);
        dot_end(cmd_is_change);
        update_curswant();
        return;
    case 'J':
        if (!cmd_ok_modify()) break;
        if (cur.l >= nl - 1) { beep(); break; }
        join_lines(cur.l, count < 2 ? 2 : count, 0);
        cmd_is_change = 1;
        break;
    case 'v':
        if (last_vis_mode) {
            vis_mode = last_vis_mode; vis_start = last_vis_start; cur = last_vis_end; curswant = last_vis_curswant;
            if (cur.l >= nl) cur.l = nl - 1;
            if (vis_start.l >= nl) vis_start.l = nl - 1;
            fix_cursor();
        }
        dot_end(0);
        return;
    case 'i': if (!cmd_ok_modify()) break; enter_insert('G', n1); break;
    case 'I': if (!cmd_ok_modify()) break; enter_insert('g', n1); break;
    case 'p': case 'P': if (!cmd_ok_modify()) break; cmd_is_change = 1; if (!put_reg(reg, g == 'p', n1, 1)) cmd_is_change = 0; break;
    case 'a': { uint32_t cp = cp_at(cur.l, cur.c); set_msg("<%c>  %u, Hex %02x", cp >= 32 && cp < 127 ? (int)cp : '?', cp, cp); dot_end(0); return; }
    case '&': ex_execute("%s//~/&"); dot_end(0); return;
    default: beep(); dot_end(0); return;
    }
    dot_end(cmd_is_change);
    if (!vis_mode) fix_cursor();
    update_curswant();
}

/* ------------------------------------------------------------------ normal mode */

void normal_command(void) {
    int count = 0, pre = 0, reg = 0, k, first_key = 1;
    cmd_is_change = 0;
    for (;;) {
        k = norm_key(getkey());
        if (first_key) {                                 /* a typed command starts an undo step; keys replayed from a macro join it */
            first_key = 0;
            if (!undo_nest && !key_from_ta) undo_begin();
        }
        if (k == '"') { reg = getkey(); if (count > 0) { pre = count; count = 0; } continue; }
        int nc = get_count_digit(k, count);
        if (nc >= 0) { count = nc; continue; }
        break;
    }
    if (pre > 0) count = count > 0 ? pre * count : pre;
    int hascount = count > 0, n1 = count > 0 ? count : 1;
    cur_count = count;
    cur_reg = reg;
    dot_begin();
    dot_push(k);
    msg[0] = 0;

    if (vis_mode) { visual_cmd(k, count, reg); return; }

    /* ---- motions */
    Motion m;
    memset(&m, 0, sizeof m);
    if (motion(k, count, hascount, 0, &m)) {
        if (m.vcol == -3) { dot_end(0); return; }        /* the search prompt was cancelled */
        if (m.fail) { if (!msg[0] || !msg_err) beep(); dot_end(0); return; }
        if (m.jump) push_jump();
        cur = m.to;
        fix_cursor();
        if (m.vcol == -2) curswant = -1;
        else if (!m.keepcol) update_curswant();
        dot_end(0);
        return;
    }
    if (k == 'g' && m.fail == 2) { g_command(m.vcol, count, reg); return; }

    /* ---- commands that change the text are refused when it is read-only */
    switch (k) {
    case 'x': case 'X': case 'D': case 'C': case 's': case 'S': case 'r': case 'R': case '~': case 'J': case 'p': case 'P':
    case 'i': case 'a': case 'I': case 'A': case 'o': case 'O': case 'd': case 'c': case '<': case '>': case '=': case 24: case 1:
    case '!': case '&':
        if (!cmd_ok_modify()) { dot_end(0); return; }
        break;
    default: break;
    }

    switch (k) {
    case 27: case 3:
        if (k == 3 && !hascount && !reg && !normal_depth)
            set_msg(modified ? "Type  :qa!  and press <Enter> to abandon all changes and exit Vim"
                             : "Type  :qa  and press <Enter> to exit Vim");
        dot_end(0);
        return;
    case 'i': case 'a': case 'I': case 'A': case 'o': case 'O':
        enter_insert(k, n1);
        break;
    case 'R':
        enter_insert('R', n1);
        break;
    case 'v': case 'V': case 22:
        vis_mode = k == 'v' ? M_VISUAL : k == 'V' ? M_VLINE : M_VBLOCK;
        vis_start = cur;
        dot_end(0);
        return;
    case 'x': case PT_K_DEL: {
        int len = (int)LN(cur.l).len;
        if (len == 0) { beep(); break; }
        int e = cur.c;
        for (int i = 0; i < n1 && e < len; i++) e = next_c(cur.l, e);
        Range r = { { cur.l, cur.c }, { cur.l, e }, 'c', 0, 0, 0, 0 };
        do_op_range('d', &r, reg, 1);
        cmd_is_change = 1;
        break;
    }
    case 'X': {
        if (cur.c == 0) { beep(); break; }
        int s = cur.c;
        for (int i = 0; i < n1 && s > 0; i++) s = prev_c(cur.l, s);
        Range r = { { cur.l, s }, { cur.l, cur.c }, 'c', 0, 0, 0, 0 };
        do_op_range('d', &r, reg, 1);
        cmd_is_change = 1;
        break;
    }
    case 'D': case 'C': {
        long last = imin(nl - 1, cur.l + n1 - 1);
        Range r = { { cur.l, cur.c }, { last, (int)LN(last).len }, 'c', 0, 0, 0, 0 };
        do_op_range(k == 'D' ? 'd' : 'c', &r, reg, 1);
        cmd_is_change = 1;
        if (k == 'C') enter_insert_after_op();
        break;
    }
    case 's': {
        int e = cur.c, len = (int)LN(cur.l).len;
        for (int i = 0; i < n1 && e < len; i++) e = next_c(cur.l, e);
        Range r = { { cur.l, cur.c }, { cur.l, e }, 'c', 0, 0, 0, 0 };
        do_op_range('c', &r, reg, 1);
        cmd_is_change = 1;
        enter_insert_after_op();
        break;
    }
    case 'S': {
        long last = imin(nl - 1, cur.l + n1 - 1);
        Range r = { { cur.l, 0 }, { last, 0 }, 'l', 0, 0, 0, 0 };
        do_op_range('c', &r, reg, 1);
        cmd_is_change = 1;
        enter_insert_after_op();
        break;
    }
    case 'Y': {
        long last = imin(nl - 1, cur.l + n1 - 1);
        Range r = { cur, { last, 0 }, 'l', 0, 0, 0, 0 };
        Pos sv = cur;
        do_op_range('y', &r, reg, 1);
        cur = sv;
        break;
    }
    case 'r': do_replace_chars(n1); break;
    case '~': {
        int len = (int)LN(cur.l).len;
        if (!len) { beep(); break; }
        int e = cur.c;
        for (int i = 0; i < n1 && e < len; i++) e = next_c(cur.l, e);
        Range r = { { cur.l, cur.c }, { cur.l, e }, 'c', 0, 0, 0, 0 };
        case_range(&r, '~');
        cur.c = e < len ? e : last_c(cur.l);
        cmd_is_change = 1;
        break;
    }
    case 'J':
        if (cur.l >= nl - 1) { beep(); break; }
        join_lines(cur.l, count < 2 ? 2 : count, 1);
        cmd_is_change = 1;
        break;
    case 'p': case 'P':
        cmd_is_change = 1;
        if (!put_reg(reg, k == 'p', n1, 0)) cmd_is_change = 0;
        break;
    case 'u': {
        dot_end(0);
        int did = 0;
        undo_counts_reset();
        for (int i = 0; i < n1; i++) { if (!undo_do(0)) break; did++; }
        if (!did) set_msg("Already at oldest change");
        else undo_message(0);
        update_curswant();
        return;
    }
    case 18: {
        dot_end(0);
        int did = 0;
        undo_counts_reset();
        for (int i = 0; i < n1; i++) { if (!undo_do(1)) break; did++; }
        if (!did) set_msg("Already at newest change");
        else undo_message(1);
        update_curswant();
        return;
    }
    case 'U':
        if (!cmd_ok_modify()) { dot_end(0); return; }
        undo_line_restore();
        dot_end(0);
        update_curswant();
        return;
    case '.': {
        dot_end(0);
        if (!dot_available()) { beep(); return; }
        const VisRedo *dv = dot_visual();
        VisRedo vr;
        if (dv) vr = *dv;
        in_dot_replay++;
        undo_nest++;
        dot_replay(count);
        if (dv) visual_redo_select(&vr);
        normal_command();
        undo_nest--;
        in_dot_replay--;
        return;
    }
    case 'q':
        dot_end(0);
        if (recording) macro_stop();
        else {
            int r = getkey();
            if ((r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') || r == '"') macro_start(r);
        }
        return;
    case '@':
        dot_end(0);
        exec_macro(getkey(), n1);
        return;
    case 'm': {
        dot_end(0);
        int r = getkey();
        if (r >= 'a' && r <= 'z') marks[r - 'a'] = cur;
        else if (r == '\'' || r == '`') push_jump();
        else if (r == '<') mark_lt = cur;
        else if (r == '>') mark_gt = cur;
        else if (r == '[') mark_lb = cur;
        else if (r == ']') mark_rb = cur;
        return;
    }
    case 'Z': {
        int r = getkey();
        dot_end(0);
        if (r == 'Z') ex_execute("x");
        else if (r == 'Q') ex_execute("q!");
        return;
    }
    case 1: case 24: do_increment(k == 1 ? n1 : -n1); break;
    case 7: {
        long pct = (cur.l + 1) * 100 / nl;
        set_msg("\"%s\"%s %ld line%s --%ld%%--", fname[0] ? fname : "[No Name]", modified ? " [Modified]" : "", nl, nl == 1 ? "" : "s", pct);
        dot_end(0);
        return;
    }
    case 12: force_repaint(); dot_end(0); return;
    case 4: scroll_half(1, count, hascount); dot_end(0); return;
    case 21: scroll_half(-1, count, hascount); dot_end(0); return;
    case 6: scroll_page(1, n1); dot_end(0); return;
    case 2: scroll_page(-1, n1); dot_end(0); return;
    case 5: scroll_lines(n1); dot_end(0); return;
    case 25: scroll_lines(-n1); dot_end(0); return;
    case 'z': do_z(count, hascount); dot_end(0); return;
    case ':': {
        dot_end(0);
        char init[16] = "";
        if (hascount) snprintf(init, sizeof init, count == 1 ? "." : ".,.+%d", count - 1);
        ex_line_prompt(init);
        return;
    }
    case '&':
        ex_execute("s");
        cmd_is_change = 1;
        break;
    case 15: case 9: {                                   /* Ctrl-O / Ctrl-I (Tab): through the jump list */
        dot_end(0);
        Pos t;
        if (jump_move(k == 15 ? -n1 : n1, &t)) {
            cur = t;
            if (cur.l >= nl) cur.l = nl - 1;
            fix_cursor();
            update_curswant();
        } else beep();
        return;
    }
    case 'K': case 8: case '\\': dot_end(0); return;
    case 'd': case 'c': case 'y': case '<': case '>': case '!': case '=':
        op_pending(k, count, reg, 0);
        break;
    default:
        beep();
        dot_end(0);
        return;
    }
    if (mode == M_NORMAL && !vis_mode) fix_cursor();
    dot_end(cmd_is_change);
    if (cmd_is_change) mark_dot = cur;
    update_curswant();
}

void enter_insert_after_op(void) {
    /* after c/s/C the delete already happened; run the insert session at the cursor */
    mode = M_INSERT;
    insert_session(1, 'i');
}

/* 'readonly' (a read-only file, -R, view) only stops writing the file: the text can still be changed, as in Vim. */
int cmd_ok_modify(void) { return 1; }

void fix_cursor_end(void) {
    if (cur.l >= nl) cur.l = nl - 1;
    if (cur.c > (int)LN(cur.l).len) cur.c = (int)LN(cur.l).len;
}

void normal_run_keys(const char *keys, size_t n) {
    void *save;
    ta_save(&save);
    ta_push_str(keys, n, 1);
    normal_depth++;
    undo_nest++;
    int guard = 0;
    while (ta_len() > 0 && !quit_flag && guard++ < 100000) {
        int sm = mode;
        (void)sm;
        normal_command();
    }
    if (mode == M_INSERT || mode == M_REPLACE) mode = M_NORMAL;
    if (vis_mode) visual_end();
    undo_nest--;
    normal_depth--;
    ta_restore(save);
}

