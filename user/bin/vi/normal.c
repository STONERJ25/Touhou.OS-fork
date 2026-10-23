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
int dot_set(int on);
static void abbr_expand_before(int k);
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
    default: return k;
    }
}

int indent_of(long l) {
    const char *s = ls(l);
    int v = 0;
    for (unsigned i = 0; i < L[l].len; i++) {
        if (s[i] == ' ') v++;
        else if (s[i] == '\t') v += o.ts - v % o.ts;
        else break;
    }
    return v;
}

void set_indent(long l, int width) {
    const char *s = ls(l);
    int n = 0;
    while (n < (int)L[l].len && (s[n] == ' ' || s[n] == '\t')) n++;
    PtBuf b = { 0 };
    int v = 0;
    if (!o.et) while (v + o.ts <= width) { pb_addc(&b, '\t'); v += o.ts; }
    while (v < width) { pb_addc(&b, ' '); v++; }
    PtBuf nw = { 0 };
    pb_add(&nw, b.p ? b.p : "", b.len);
    pb_add(&nw, s + n, L[l].len - (size_t)n);
    ln_set(l, nw.p ? nw.p : "", nw.len);
    pb_free(&b);
    pb_free(&nw);
}

/* ------------------------------------------------------------------ operators on ranges */

void join_lines(long first, long count, int spaces) {
    if (first + 1 >= nl) return;
    long last = imin(nl - 1, first + (count < 2 ? 1 : count - 1));
    PtBuf b = { 0 };
    pb_add(&b, ls(first), L[first].len);
    int curc = 0;
    for (long i = first + 1; i <= last; i++) {
        const char *s = ls(i);
        int n = 0, len = (int)L[i].len;
        if (spaces) while (n < len && (s[n] == ' ' || s[n] == '\t')) n++;
        if (spaces) {
            while (b.len && (b.p[b.len - 1] == ' ' || b.p[b.len - 1] == '\t') && 0) b.len--;
            int needspace = b.len > 0 && b.p[b.len - 1] != ' ' && b.p[b.len - 1] != '\t' && n < len && s[n] != ')';
            if (b.len > 0 && (b.p[b.len - 1] == ' ' || b.p[b.len - 1] == '\t')) needspace = 0;
            curc = (int)b.len;
            if (needspace) pb_addc(&b, ' ');
            else if (curc > 0 && (n >= len)) curc--;
        } else curc = (int)b.len;
        pb_add(&b, s + n, (size_t)(len - n));
    }
    ln_set(first, b.p ? b.p : "", b.len);
    ln_delete(first + 1, last - first);
    pb_free(&b);
    cur.l = first;
    cur.c = curc;
    fix_cursor();
}

void shift_lines(long first, long last, int amount) {
    for (long i = first; i <= last && i < nl; i++) {
        if (L[i].len == 0) continue;
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
            int n = 0, len = (int)L[l].len;
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
    if (nl == 1 && L[0].len == 0 && first == 0) {        /* the buffer became one empty line: reuse it */
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
    for (unsigned i = 0; i < L[l].len; i++) if (s[i] != ' ' && s[i] != '\t') return 0;
    return 1;
}

static void case_range(const Range *r, int how) {
    for (long i = r->a.l; i <= r->b.l && i < nl; i++) {
        int s = 0, e = (int)L[i].len;
        if (r->type == 'c') { if (i == r->a.l) s = r->a.c; if (i == r->b.l) e = imin(r->b.c, e); }
        else if (r->type == 'b') {
            if (line_vwidth(i) <= r->vc1) continue;
            s = byte_at_vcol(i, r->vc1);
            e = r->toeol || line_vwidth(i) <= r->vc2 + 1 ? (int)L[i].len : byte_at_vcol(i, r->vc2 + 1);
        }
        if (e <= s) continue;
        char *copy = malloc((size_t)L[i].len + 1);
        if (!copy) pt_oom();
        memcpy(copy, ls(i), L[i].len);
        int changed = 0;
        for (int k = s; k < e; k++) {
            unsigned char ch = (unsigned char)copy[k];
            unsigned char n = ch;
            if (how == 'u') n = (unsigned char)tolower(ch);
            else if (how == 'U') n = (unsigned char)toupper(ch);
            else if (how == '~') n = (unsigned char)(isupper(ch) ? tolower(ch) : toupper(ch));
            else if (how == '?') { if (isalpha(ch)) { int base = isupper(ch) ? 'A' : 'a'; n = (unsigned char)(base + (ch - base + 13) % 26); } }
            if (n != ch) { copy[k] = (char)n; changed = 1; }
        }
        if (changed) ln_set(i, copy, L[i].len);
        free(copy);
    }
}

static void set_change_marks(const Range *r) {
    mark_lb = r->a;
    mark_rb = r->b;
    if (r->type == 'l') { mark_lb.c = 0; mark_rb.c = imax(0, (int)L[imin(r->b.l, nl - 1)].len - 1); mark_rb.l = imin(r->b.l, nl - 1); }
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
        cur.l = r->a.l;
        if (r->type == 'c') cur.c = r->a.c;
        else if (r->type == 'b') cur.c = byte_at_vcol(cur.l, r->vc1);
        fix_cursor();
        return;
    case 'd': case 'c': {
        size_t n;
        char *t = range_text(r, &n);
        reg_set(reg, t, n, r->type, 1);
        free(t);
        if (op == 'c' && r->type == 'l') {
            int ind = o.ai ? indent_of(r->a.l) : 0;
            if (nlines > 1) ln_delete(r->a.l + 1, nlines - 1);
            ln_set(r->a.l, "", 0);
            if (ind) set_indent(r->a.l, ind);
            cur.l = r->a.l;
            cur.c = (int)L[cur.l].len;
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
        if (op == 'c') { if (cur.c > (int)L[cur.l].len) cur.c = (int)L[cur.l].len; }
        else fix_cursor();
        return;
    }
    case '>': case '<': {
        int times = cnt > 1 && vis_mode ? cnt : 1;
        shift_lines(r->a.l, r->b.l, (op == '>' ? 1 : -1) * times);
        cur.l = r->a.l;
        cur.c = first_nonblank(cur.l);
        if (nlines > 2) set_msg("%ld lines %ced %d time%s", nlines, op, times, times == 1 ? "" : "s");
        return;
    }
    case 'u': case 'U': case '~': case '?': {
        case_range(r, op);
        cur = r->a;
        if (r->type == 'l') cur.c = cur.l == cur_start_line() ? cur.c : 0;
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
        int vc = after && L[cur.l].len ? vcol_end(cur.l, cur.c) + 1 : vcol_of(cur.l, cur.c);
        size_t p = 0;
        for (long i = 0; i < nlin; i++) {
            long ln = cur.l + i;
            if (ln >= nl) ln_insert(nl, "", 0);
            char *e = memchr(text + p, '\n', len - p);
            size_t n = e ? (size_t)(e - (text + p)) : len - p;
            int lw = line_vwidth(ln);
            PtBuf b = { 0 };
            if (lw < vc) { pb_add(&b, ls(ln), L[ln].len); for (int k = lw; k < vc; k++) pb_addc(&b, ' '); }
            else pb_add(&b, ls(ln), (size_t)byte_at_vcol(ln, vc));
            int tail = lw > vc ? byte_at_vcol(ln, vc) : (int)L[ln].len;
            for (int c = 0; c < cnt; c++) {
                pb_add(&b, text + p, n);
                if (lw > vc || c < cnt - 1) for (int k = (int)pt_u8_count(text + p, n); k < width; k++) pb_addc(&b, ' ');
            }
            pb_add(&b, ls(ln) + tail, L[ln].len - (size_t)tail);
            ln_set(ln, b.p ? b.p : "", b.len);
            pb_free(&b);
            p += n + 1;
        }
        cur.c = byte_at_vcol(cur.l, vc);
    } else {
        Pos at = cur;
        if (after && L[at.l].len) at.c = next_c(at.l, at.c);
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
        int len = (int)L[cur.l].len;
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
        const char *s = ls(cur.l);
        int brk = -1;
        for (int i = cur.c - 1; i > 0; i--) {
            if ((s[i] == ' ' || s[i] == '\t') && vcol_of(cur.l, i) <= o.tw) { brk = i; break; }
        }
        if (brk > 0) {
            int ws = brk;
            while (ws > 0 && (s[ws - 1] == ' ' || s[ws - 1] == '\t')) ws--;
            int we = brk;
            while (we < (int)L[cur.l].len && (s[we] == ' ' || s[we] == '\t')) we++;
            if (ws > 0 && we <= cur.c) {
                int ind = o.ai ? indent_of(cur.l) : 0;
                int moved = cur.c - we;
                Pos p = { cur.l, ws };
                text_delete(p, (Pos){ cur.l, we });
                text_insert(p, "\n", 1);
                cur.l = p.l + 1;
                if (ind) { set_indent(cur.l, ind); cur.c = (int)L[cur.l].len; int rest = moved; cur.c -= 0; (void)rest; }
                else cur.c = moved;
                if (ind) cur.c = first_nonblank_raw(cur.l) + moved;
            }
        }
    }
}

int first_nonblank_raw(long l) {
    const char *s = ls(l);
    int i = 0;
    while (i < (int)L[l].len && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

static void insert_newline(void) {
    int ind = 0;
    if (o.ai) ind = indent_of(cur.l);
    /* with autoindent, a line that has only the indent loses it when you press Enter */
    int only_ws = o.ai && ind > 0 && blank_line_pub(cur.l) && cur.c >= (int)L[cur.l].len;
    if (mode == M_REPLACE) { }
    ins_bytes("\n", 1);
    ins_note_text("\n", 1);
    if (only_ws) ln_set(cur.l - 1, "", 0);
    if (ind) {
        set_indent(cur.l, ind);
        cur.c = (int)L[cur.l].len - (int)(L[cur.l].len - (size_t)first_nonblank_raw(cur.l));
        cur.c = first_nonblank_raw(cur.l);
    }
    /* trailing white space before the break stays (as in vi) */
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
    if (cur.c > 0) {
        int p = prev_c(cur.l, cur.c);
        if (o.sts > 0 || 0) { }
        text_delete((Pos){ cur.l, p }, cur);
        cur.c = p;
        if (ins_text.len) ins_text.len = pt_u8_prev_len(ins_text.p, ins_text.len);
    } else if (cur.l > 0) {
        int len = (int)L[cur.l - 1].len;
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
        int old = (int)L[cur.l].len;
        set_indent(cur.l, (w / o.sw + 1) * o.sw);
        cur.c += (int)L[cur.l].len - old;
        break;
    }
    case 4: {                                            /* Ctrl-D */
        int w = indent_of(cur.l);
        int old = (int)L[cur.l].len;
        set_indent(cur.l, w > 0 ? ((w - 1) / o.sw) * o.sw : 0);
        cur.c += (int)L[cur.l].len - old;
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
        if (cur.c >= (int)L[cur.l].len && cur.c > 0 && 0) cur.c = last_c(cur.l);
        int at_end = cur.c >= (int)L[cur.l].len;
        Pos before = cur;
        fix_cursor();
        (void)before;
        int dr = dot_set(0);
        normal_command();
        dot_set(dr);
        if (mode == M_NORMAL) mode = saved;
        vis_mode = vm;
        if (at_end && cur.l == before.l && cur.c == last_c(cur.l) && cur.c == before.c - 1) cur.c = (int)L[cur.l].len;
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
            if (b < (int)L[l].len) { int n = next_c(l, b) - b; ins_bytes(ls(l) + b, (size_t)n); ins_note_text(ls(l) + b, (size_t)n); }
        }
        break;
    }
    case 9:
        if (o.et) {
            int v = vcol_of(cur.l, cur.c);
            int w = o.sts > 0 ? o.sts : o.ts;
            int n = w - v % w;
            for (int i = 0; i < n; i++) insert_cp(' ');
        } else insert_cp('\t');
        break;
    case PT_K_LEFT: if (cur.c > 0) cur.c = prev_c(cur.l, cur.c); ins_n = 0; break;
    case PT_K_RIGHT: if (cur.c < (int)L[cur.l].len) cur.c = next_c(cur.l, cur.c); ins_n = 0; break;
    case PT_K_UP:
        if (cur.l > 0) { int v = vcol_of(cur.l, cur.c); cur.l--; cur.c = byte_at_vcol(cur.l, v); }
        ins_n = 0; break;
    case PT_K_DOWN:
        if (cur.l < nl - 1) { int v = vcol_of(cur.l, cur.c); cur.l++; cur.c = byte_at_vcol(cur.l, v); }
        ins_n = 0; break;
    case PT_K_HOME: cur.c = 0; ins_n = 0; break;
    case PT_K_END: cur.c = (int)L[cur.l].len; ins_n = 0; break;
    case PT_K_DEL:
        if (cur.c < (int)L[cur.l].len) text_delete(cur, (Pos){ cur.l, next_c(cur.l, cur.c) });
        else if (cur.l < nl - 1) text_delete(cur, (Pos){ cur.l + 1, 0 });
        break;
    case PT_K_INS: mode = mode == M_INSERT ? M_REPLACE : M_INSERT; break;
    case PT_K_PGUP: case PT_K_PGDN: break;
    default:
        if (k >= 32 && k < PT_K_BASE) {
            if (k == ' ' || k == '.' || k == ',' || k == ';' || k == ')' || k == '!' || k == '?' || k == ':') abbr_expand_before(k);
            insert_cp((uint32_t)k);
        }
        break;
    }
    return 0;
}

static void abbr_expand_before(int k) {
    (void)k;
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
            int len = (int)L[l].len;
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
        int k = getkey();
        last_key_was_complete = (k == 14 || k == 16) && (prev_k == 14 || prev_k == 16);
        prev_k = k;
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
            if (kind == 'o') { long at = cur.l + 1; ln_insert(at, "", 0); cur.l = at; cur.c = 0; if (o.ai) { set_indent(cur.l, indent_of(cur.l - 1)); cur.c = (int)L[cur.l].len; } }
            else if (kind == 'O') { }
            int *all = malloc((size_t)(nk + 1) * sizeof *all);
            memcpy(all, keys, (size_t)nk * sizeof *all);
            all[nk] = K_ESC;
            for (int i = 0; i <= nk; i++) all[i] |= K_NOMAP;
            ta_push(all, nk + 1);
            free(all);
            for (;;) { int k = getkey(); if (insert_key_wrapper(k)) break; }
        }
        free(keys);
        dot_set(dr);
        ins_replay = saved;
    }
    /* leave insert mode */
    last_insert_len = ins_text.len < sizeof last_insert ? ins_text.len : sizeof last_insert - 1;
    if (ins_text.len) memcpy(last_insert, ins_text.p, last_insert_len);
    mode = M_NORMAL;
    mark_rb = cur;
    mark_lb = ins_start;
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
    case 'a': if (L[cur.l].len) cur.c = next_c(cur.l, cur.c); break;
    case 'I': cur.c = first_nonblank_raw(cur.l); break;
    case 'A': cur.c = (int)L[cur.l].len; break;
    case 'g': cur.c = 0; break;                          /* gI */
    case 'G': if (mark_dot.l >= 0 && mark_ins.l >= 0) { cur = mark_ins; if (cur.l >= nl) cur.l = nl - 1; cur.c = clamp_col(cur.l, cur.c, 1); } break;
    case 'o': {
        int ind = o.ai ? indent_of(cur.l) : 0;
        ln_insert(cur.l + 1, "", 0);
        cur.l++;
        cur.c = 0;
        if (ind) { set_indent(cur.l, ind); cur.c = (int)L[cur.l].len; }
        break;
    }
    case 'O': {
        int ind = o.ai ? indent_of(cur.l) : 0;
        ln_insert(cur.l, "", 0);
        cur.c = 0;
        if (ind) { set_indent(cur.l, ind); cur.c = (int)L[cur.l].len; }
        break;
    }
    case 'R': mode = M_REPLACE; break;
    default: break;
    }
    if (cmdchar == 'c') { }
    mode = cmdchar == 'R' ? M_REPLACE : M_INSERT;
    Pos start = cur;
    (void)start;
    insert_session(cnt, cmdchar == 'o' || cmdchar == 'O' ? (cmdchar == 'O' ? 'o' : 'o') : (cmdchar == 'R' ? 'R' : 'i'));
    mark_ins = cur;
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
    if (vis_mode == M_VLINE) { mark_lt.c = 0; mark_gt.c = imax(0, (int)L[b.l].len - 1); }
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
    int incl_end = curswant == -1 && cur.l >= vis_start.l ? 1 : 0;
    r->b = b;
    if (b.c < (int)L[b.l].len) r->b.c = next_c(b.l, b.c);
    else if (L[b.l].len == 0 && b.l + 1 < nl) { r->b.l = b.l + 1; r->b.c = 0; }       /* an empty last line takes its newline */
    (void)incl_end;
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
        if (end >= (int)L[l].len) { set_err("E?: not enough characters"); return; }
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
    int len = (int)L[l].len, i = cur.c;
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

static void scroll_page(int dir, int cnt) {
    int th = text_rows();
    if (th <= 0) return;
    for (int i = 0; i < cnt; i++) {
        if (dir > 0) {
            int last = 0;
            long l = topline, rows = 0;
            while (l < nl && rows < th) { rows += 1; last = (int)l; l++; }
            if (topline >= nl - 1) { beep(); break; }
            long nt = (long)last - 1;
            if (nt <= topline) nt = topline + 1;
            if (nt > nl - 1) nt = nl - 1;
            topline = nt;
            if (cur.l < topline) { cur.l = topline; }
        } else {
            if (topline == 0) { beep(); break; }
            long nt = topline + 1 - (th - 1);
            if (nt < 0) nt = 0;
            topline = nt;
            long bottom = topline + th - 1;
            if (cur.l > bottom) cur.l = bottom;
        }
    }
    if (cur.l >= nl) cur.l = nl - 1;
    cur.c = first_nonblank(cur.l);
}

static void scroll_half(int dir, int cnt, int hascnt) {
    int th = text_rows();
    if (th <= 0) return;
    if (hascnt) scroll_amount = cnt;
    int n = scroll_amount > 0 ? scroll_amount : (th + 1) / 2;
    if (dir > 0) {
        if (cur.l >= nl - 1) { beep(); return; }
        topline = imin(topline + n, imax(0, nl - 1));
        cur.l = imin(nl - 1, cur.l + n);
    } else {
        if (cur.l == 0) { beep(); return; }
        topline = imax(0, topline - n);
        cur.l = imax(0, cur.l - n);
    }
    cur.c = first_nonblank(cur.l);
}

static void do_z(int cnt, int hascnt) {
    int k = getkey();
    int th = text_rows();
    long l = hascnt && k != 'z' ? imin(cnt, nl) - 1 : cur.l;
    if (hascnt && (k == K_ENTER || k == 't' || k == '.' || k == 'z' || k == '-' || k == 'b')) { cur.l = imin(cnt, nl) - 1; l = cur.l; }
    switch (k) {
    case K_ENTER: case 't': case '+':
        topline = l; if (k == K_ENTER) cur.c = first_nonblank(cur.l); break;
    case '.': case 'z': {
        topline = imax(0, l - (th - 1) / 2);
        if (k == '.') cur.c = first_nonblank(cur.l);
        break;
    }
    case '-': case 'b': {
        topline = imax(0, l - th + 1);
        if (k == '-') cur.c = first_nonblank(cur.l);
        break;
    }
    case 'h': case PT_K_LEFT: break;
    case 'l': case PT_K_RIGHT: break;
    default: beep(); break;
    }
}

static int exec_macro(int reg, int cnt) {
    static int last_macro;
    if (reg == '@') reg = last_macro;
    if (!reg) { set_err("E748: No previously used register"); return 0; }
    if (reg == ':') {
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
    if (vc < 0) cur.c = (int)L[first].len;
    else {
        int lw = line_vwidth(first);
        if (lw < vc && append) { PtBuf pad = { 0 }; for (int k = lw; k < vc; k++) pb_addc(&pad, ' '); text_insert((Pos){ first, (int)L[first].len }, pad.p, pad.len); pb_free(&pad); }
        cur.c = lw <= vc ? (int)L[first].len : byte_at_vcol(first, vc);
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
        int e = cur.c + 1 > (int)L[tl].len ? (int)L[tl].len : cur.c + 1;
        if (e > (int)L[tl].len || (L[tl].len && cur.c < a)) e = (int)L[tl].len;
        size_t typed = last_insert_len;
        if (typed > 0 && !memchr(last_insert, '\n', typed)) {
            (void)e;
            for (long i = first + 1; i <= last && i < nl; i++) {
                int pos;
                int lw = line_vwidth(i);
                if (vc < 0) pos = (int)L[i].len;
                else if (lw < vc) { if (!append) continue; PtBuf pad = { 0 }; for (int k = lw; k < vc; k++) pb_addc(&pad, ' '); text_insert((Pos){ i, (int)L[i].len }, pad.p, pad.len); pb_free(&pad); pos = (int)L[i].len; }
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

void normal_command(void) {
    int count = 0, reg = 0, k;
    cmd_is_change = 0;
    int hascount, first_key = 1;
    for (;;) {
        k = norm_key(getkey());
        if (first_key) {                                 /* a typed command starts an undo step; keys replayed from a macro join it */
            first_key = 0;
            if (!undo_nest && !key_from_ta) undo_begin();
        }
        if (k == '"') { reg = getkey(); continue; }
        int nc = get_count_digit(k, count);
        if (nc >= 0) { count = nc; continue; }
        break;
    }
    hascount = count > 0;
    int n1 = count > 0 ? count : 1;
    cur_count = count;
    cur_reg = reg;
    dot_begin();
    dot_push(k);
    msg[0] = 0;
    Motion m;
    int in_vis = vis_mode;
    if (in_vis) dot_nokeep_set();

    /* ---- motions */
    if (k != 'g' && k != 'z' && k != 'Z' && !(in_vis && (k == 'a' || k == 'i')) ? motion(k, count, hascount, 0, &m) : (k == 'g' ? motion(k, count, hascount, 0, &m) : 0)) {
        if (m.vcol == -3) { dot_end(0); return; }        /* cancelled search prompt */
        if (m.fail) { if (!msg[0] || !msg_err) beep(); dot_end(0); return; }
        if (m.jump) push_jump();
        cur = m.to;
        if (in_vis && vis_mode != M_VBLOCK) fix_cursor();
        else fix_cursor();
        if (m.vcol == -2) curswant = -1;
        else if (!m.keepcol) update_curswant();
        dot_end(0);
        return;
    }
    if (k == 'g' && m.fail == 2) {
        int g = m.vcol;
        m.fail = 0;
        dot_push(g);
        switch (g) {
        case 'u': case 'U': case '~': case '?': case 'q': case 'w': k = 0x10000 + g; goto operator_cmd;
        case 'J': if (cmd_ok_modify()) { cmd_is_change = 1; join_lines(cur.l, count < 2 ? 2 : count, 0); } break;
        case 'v':
            if (last_vis_mode) {
                vis_mode = last_vis_mode; vis_start = last_vis_start; cur = last_vis_end; curswant = last_vis_curswant; fix_cursor();
            }
            break;
        case 'i': if (!cmd_ok_modify()) break; enter_insert('G', n1); break;
        case 'I': if (!cmd_ok_modify()) break; enter_insert('g', n1); break;
        case 'p': case 'P': if (!cmd_ok_modify()) break; cmd_is_change = 1; put_reg(reg, g == 'p', n1, 1); break;
        case 'a': { uint32_t cp = cp_at(cur.l, cur.c); set_msg("<%c>  %u, Hex %02x", cp >= 32 && cp < 127 ? (int)cp : '?', cp, cp); break; }
        case '&': ex_execute("%s//~/&"); break;
        default: beep(); break;
        }
        dot_end(cmd_is_change);
        if (!vis_mode) fix_cursor();
        return;
    }

    /* ---- commands that change the text */
    int readonly_cmd = 0;
    switch (k) {
    case 'x': case 'X': case 'D': case 'C': case 's': case 'S': case 'r': case 'R': case '~': case 'J': case 'p': case 'P':
    case 'i': case 'a': case 'I': case 'A': case 'o': case 'O': case 'd': case 'c': case '<': case '>': case '=': case 24: case 1:
        readonly_cmd = 1; break;
    case '!': readonly_cmd = 1; break;
    default: break;
    }
    if (readonly_cmd && !cmd_ok_modify()) { dot_end(0); vis_mode = 0; return; }

    if (in_vis) {
        switch (k) {
        case 27: case 3: visual_end(); dot_end(0); return;
        case 'v': case 'V': case 22: {
            int nm = k == 'v' ? M_VISUAL : k == 'V' ? M_VLINE : M_VBLOCK;
            if (vis_mode == nm) visual_end(); else vis_mode = nm;
            dot_end(0);
            return;
        }
        case 'o': case 'O': {
            if (k == 'O' && vis_mode == M_VBLOCK) {
                int v1 = vcol_of(vis_start.l, vis_start.c), v2 = vcol_of(cur.l, cur.c);
                vis_start.c = byte_at_vcol(vis_start.l, v2); cur.c = byte_at_vcol(cur.l, v1);
            } else { Pos t = vis_start; vis_start = cur; cur = t; }
            update_curswant();
            dot_end(0);
            return;
        }
        case 'a': case 'i': {
            int c2 = getkey();
            Range r;
            if (!textobj(c2, k == 'i', count, &r)) { beep(); dot_end(0); return; }
            Pos a = vis_start, b = cur;
            int fresh = a.l == b.l && a.c == b.c;
            if (r.type == 'l') {
                if (vis_mode == M_VISUAL) vis_mode = M_VLINE;
                if (fresh) vis_start = (Pos){ r.a.l, 0 };
                cur.l = r.b.l; cur.c = first_nonblank(cur.l);
            } else {
                Pos e = r.b;
                if (e.c > 0) e.c = prev_c(e.l, e.c); else if (e.l > r.a.l) { e.l--; e.c = last_c(e.l); }
                if (fresh) vis_start = r.a;
                cur = e;
            }
            fix_cursor();
            update_curswant();
            dot_end(0);
            return;
        }
        case ':': {
            visual_end();
            ex_line_prompt("'<,'>");
            dot_end(0);
            return;
        }
        case 'I': case 'A': case 'c':
            if (vis_mode == M_VBLOCK && (k != 'c')) { cmd_is_change = 1; visual_block_insert(k == 'A', 0); dot_end(0); return; }
            if (vis_mode == M_VBLOCK && k == 'c') { cmd_is_change = 1; visual_block_insert(0, 1); dot_end(0); return; }
            if (k == 'c') goto visual_op;
            {
                int vm = vis_mode;
                visual_end();
                if (vm == M_VLINE) { cur.l = mark_lt.l; }
                if (k == 'I') { cur.l = mark_lt.l; cur.c = vm == M_VLINE ? first_nonblank_raw(cur.l) : mark_lt.c; enter_insert('i', 1); }
                else { cur.l = mark_gt.l; enter_insert('A', 1); }
                dot_end(cmd_is_change);
            }
            return;
        case 'd': case 'x': case PT_K_DEL: case 'y': case '<': case '>': case 'u': case 'U': case '~': case 'J': case 'r': case 'p': case 'P':
        case 'S': case 'R': case 'C': case 's': case 'D': case 'X': case 'Y': case '!': case '=': case 'g': case 'z':
        visual_op: {
            Range r;
            int op = k;
            if (k == 'g') { int g = getkey(); dot_push(g); if (g == 'J') { visual_range(&r); vis_save(); vis_mode = 0; cmd_is_change = 1; join_lines(r.a.l, r.b.l - r.a.l + 1 < 2 ? 2 : r.b.l - r.a.l + 1, 0); dot_end(1); return; } if (g == 'u' || g == 'U' || g == '~' || g == '?' || g == 'q' || g == 'w') op = g; else { beep(); visual_end(); dot_end(0); return; } }
            if (k == 'z') { do_z(count, hascount); dot_end(0); return; }
            int vm = vis_mode;
            visual_range(&r);
            if (op == 'x' || op == PT_K_DEL) op = 'd';
            if (op == 'X' || op == 'D') { op = 'd'; if (vm != M_VBLOCK) r.type = 'l'; else r.toeol = 1; }
            if (op == 'Y') { op = 'y'; if (vm != M_VBLOCK) r.type = 'l'; }
            if (op == 'S' || op == 'R') { op = 'c'; r.type = 'l'; }
            if (op == 'C') { op = 'c'; if (vm != M_VBLOCK) r.type = 'l'; else r.toeol = 1; }
            if (op == 's') op = 'c';
            vis_save();
            vis_mode = 0;
            if (op == 'J') { join_lines(r.a.l, r.b.l - r.a.l + 1 < 2 ? 2 : r.b.l - r.a.l + 1, 1); cmd_is_change = 1; dot_end(1); return; }
            if (op == 'r') {
                int ch = getkey();
                if (ch == K_ESC) { dot_end(0); return; }
                cmd_is_change = 1;
                for (long i = r.a.l; i <= r.b.l; i++) {
                    int s = 0, e = (int)L[i].len;
                    if (r.type == 'c') { if (i == r.a.l) s = r.a.c; if (i == r.b.l) e = imin(r.b.c, e); }
                    else if (r.type == 'b') { if (line_vwidth(i) <= r.vc1) continue; s = byte_at_vcol(i, r.vc1); e = r.toeol || line_vwidth(i) <= r.vc2 + 1 ? (int)L[i].len : byte_at_vcol(i, r.vc2 + 1); }
                    if (e <= s) continue;
                    PtBuf b = { 0 };
                    char u[4];
                    int un = pt_u8_encode((uint32_t)ch, u);
                    for (int p = s; p < e; p = next_c(i, p)) pb_add(&b, u, (size_t)un);
                    text_delete((Pos){ i, s }, (Pos){ i, e });
                    text_insert((Pos){ i, s }, b.p, b.len);
                    pb_free(&b);
                }
                cur = r.a; if (r.type == 'l') cur.c = 0;
                fix_cursor();
                dot_end(1);
                return;
            }
            if (op == 'p' || op == 'P') {
                Reg *rg = reg_get(reg);
                if (!rg) { set_err("E353: Nothing in register %c", reg ? reg : '"'); dot_end(0); return; }
                char *saved = malloc(rg->len + 1);
                if (!saved) pt_oom();
                memcpy(saved, rg->s, rg->len);
                size_t slen = rg->len;
                int stype = rg->type;
                size_t n;
                char *t = range_text(&r, &n);
                int vtype = r.type;
                long old_nl = nl;
                range_delete(&r);
                if (op == 'p') reg_set(0, t, n, vtype, 1);
                free(t);
                cmd_is_change = 1;
                if (vtype == 'l') {
                    int all_gone = r.a.l == 0 && r.b.l == old_nl - 1;
                    if (stype == 'l') {
                        if (all_gone) { cur.l = 0; cur.c = 0; put_text(saved, slen, stype, 0, n1, 0); ln_delete(nl - 1, 1); }
                        else if (r.a.l >= nl) { cur.l = nl - 1; put_text(saved, slen, stype, 1, n1, 0); }
                        else { cur.l = r.a.l; put_text(saved, slen, stype, 0, n1, 0); }
                    } else {
                        long at = all_gone ? 0 : (r.a.l >= nl ? nl : r.a.l);
                        if (all_gone) { cur.l = 0; cur.c = 0; }
                        else { ln_insert(at, "", 0); cur.l = at; cur.c = 0; }
                        put_text(saved, slen, stype, 0, n1, 0);
                    }
                } else {
                    cur = r.a;
                    if (stype == 'l') {                  /* linewise text into a charwise selection: it gets its own lines */
                        text_insert(cur, "\n", 1);
                        cur.c = 0;
                        put_text(saved, slen, stype, 1, n1, 0);
                    } else {
                        int at_eol = r.a.c >= (int)L[r.a.l].len && L[r.a.l].len > 0;
                        if (at_eol) { cur.c = last_c(cur.l); put_text(saved, slen, stype, 1, n1, 0); }
                        else { cur.c = r.a.c; put_text(saved, slen, stype, 0, n1, 0); }
                    }
                }
                free(saved);
                dot_end(1);
                return;
            }
            if (op == 'c' && vm == M_VBLOCK) { }
            do_op_range(op, &r, op == 'y' ? reg : reg, count);
            if (op == 'c') { enter_insert_after_op(); dot_end(1); return; }
            if (op != 'y') cmd_is_change = 1;
            dot_end(cmd_is_change);
            fix_cursor();
            update_curswant();
            return;
        }
        default: break;
        }
    }

    switch (k) {
    case 27: case 3:
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
        if (hascount) { }
        dot_end(0);
        return;
    case 'x': case PT_K_DEL: {
        int len = (int)L[cur.l].len;
        if (len == 0) { beep(); break; }
        int e = cur.c;
        for (int i = 0; i < n1 && e < len; i++) e = next_c(cur.l, e);
        Range r = { { cur.l, cur.c }, { cur.l, e }, 'c', 0, 0, 0 };
        do_op_range('d', &r, reg, 1);
        cmd_is_change = 1;
        break;
    }
    case 'X': {
        if (cur.c == 0) { beep(); break; }
        int s = cur.c;
        for (int i = 0; i < n1 && s > 0; i++) s = prev_c(cur.l, s);
        Range r = { { cur.l, s }, { cur.l, cur.c }, 'c', 0, 0, 0 };
        do_op_range('d', &r, reg, 1);
        cmd_is_change = 1;
        break;
    }
    case 'D': case 'C': {
        long last = imin(nl - 1, cur.l + n1 - 1);
        Range r = { { cur.l, cur.c }, { last, (int)L[last].len }, 'c', 0, 0, 0 };
        do_op_range(k == 'D' ? 'd' : 'c', &r, reg, 1);
        cmd_is_change = 1;
        if (k == 'C') { enter_insert_after_op(); }
        break;
    }
    case 's': {
        int e = cur.c, len = (int)L[cur.l].len;
        for (int i = 0; i < n1 && e < len; i++) e = next_c(cur.l, e);
        Range r = { { cur.l, cur.c }, { cur.l, e }, 'c', 0, 0, 0 };
        do_op_range('c', &r, reg, 1);
        cmd_is_change = 1;
        enter_insert_after_op();
        break;
    }
    case 'S': {
        long last = imin(nl - 1, cur.l + n1 - 1);
        Range r = { { cur.l, 0 }, { last, 0 }, 'l', 0, 0, 0 };
        do_op_range('c', &r, reg, 1);
        cmd_is_change = 1;
        enter_insert_after_op();
        break;
    }
    case 'Y': {
        long last = imin(nl - 1, cur.l + n1 - 1);
        Range r = { { cur.l, 0 }, { last, 0 }, 'l', 0, 0, 0 };
        Pos sv = cur;
        do_op_range('y', &r, reg, 1);
        cur = sv;
        break;
    }
    case 'r': do_replace_chars(n1); break;
    case '~': {
        int len = (int)L[cur.l].len;
        if (!len) { beep(); break; }
        int e = cur.c;
        for (int i = 0; i < n1 && e < len; i++) e = next_c(cur.l, e);
        Range r = { { cur.l, cur.c }, { cur.l, e }, 'c', 0, 0, 0 };
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
        for (int i = 0; i < n1; i++) { if (!undo_do(0)) break; did++; }
        if (!did) set_msg("Already at oldest change");
        else set_msg("%d change%s undone", did, did == 1 ? "" : "s");
        update_curswant();
        return;
    }
    case 18: {
        dot_end(0);
        int did = 0;
        for (int i = 0; i < n1; i++) { if (!undo_do(1)) break; did++; }
        if (!did) set_msg("Already at newest change");
        update_curswant();
        return;
    }
    case 'U': undo_line_restore(); break;
    case '.':
        dot_end(0);
        if (!dot_available()) { beep(); return; }
        in_dot_replay++;
        undo_nest++;
        dot_replay(count);
        normal_command();
        undo_nest--;
        in_dot_replay--;
        return;
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
        else if (r == '\'' || r == '`') mark_ctx = cur;
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
    case 15: case 9: {                                   /* Ctrl-O / Tab: jump list (just the previous position) */
        dot_end(0);
        if (mark_ctx.l >= 0) { Pos t = cur; cur = mark_ctx; if (cur.l >= nl) cur.l = nl - 1; mark_ctx = t; fix_cursor(); }
        return;
    }
    case 'K': case 8: dot_end(0); return;
    case '\\': dot_end(0); return;
    case 'd': case 'c': case 'y': case '<': case '>': case '!': case '=':
    operator_cmd: {
        int op = k >= 0x10000 ? k - 0x10000 : k;
        int count2 = 0, kk;
        for (;;) {
            kk = norm_key(getkey());
            int nc = get_count_digit(kk, count2);
            if (nc >= 0) { count2 = nc; continue; }
            break;
        }
        int total = (count ? count : 1) * (count2 ? count2 : 1);
        int hastotal = count || count2;
        int force = 0;
        while (kk == 'v' || kk == 'V' || kk == 22) { force = kk; kk = norm_key(getkey()); }
        Range r;
        memset(&r, 0, sizeof r);
        int dbl = kk == k || (k >= 0x10000 && (kk == op || (kk == 'g' && 0)));
        if (k >= 0x10000 && kk == 'g') {                 /* gUgU, gugu, g~g~, gqgq */
            int k3 = getkey();
            if (k3 == op) dbl = 1;
            else { ta_push(&k3, 1); kk = 'g'; }
        }
        if (op == 'c' || op == 'd' || op == 'y' || op == '<' || op == '>' || op == '!' || op == '=' || k >= 0x10000) {
            if (dbl) {
                if (total > 1 && cur.l >= nl - 1) { beep(); break; }
                long last = imin(nl - 1, cur.l + total - 1);
                r.type = 'l'; r.a = (Pos){ cur.l, 0 }; r.b = (Pos){ last, 0 };
                if (op == '>' || op == '<') { }
                goto have_range;
            }
        }
        if (kk == K_ESC || kk == 3) break;
        if (kk == 'a' || kk == 'i') {
            int oc = getkey();
            if (!textobj(oc, kk == 'i', total, &r)) { beep(); break; }
            if (r.type == 'c' && force == 'V') { r.type = 'l'; }
            goto have_range;
        }
        {
            Motion mm;
            int mk = kk;
            int opx = op;
            if (!motion(mk, total, hastotal, opx, &mm)) {
                if (mm.fail == 2 && kk == 'g') { beep(); break; }
                beep();
                break;
            }
            if (mm.fail || mm.vcol == -3) { if (!msg[0]) beep(); break; }
            if (force == 'v') { if (mm.linewise) { mm.linewise = 0; mm.incl = 0; } else mm.incl = !mm.incl; }
            else if (force == 'V') mm.linewise = 1;
            Pos a = cur, b = mm.to;
            if (mm.jump) push_jump();
            if (mm.linewise) {
                if (a.l > b.l) { Pos t = a; a = b; b = t; }
                r.type = 'l'; r.a = (Pos){ a.l, 0 }; r.b = (Pos){ b.l, 0 };
            } else {
                if (a.l > b.l || (a.l == b.l && a.c > b.c)) { Pos t = a; a = b; b = t; }
                r.type = 'c';
                if (mm.incl) b.c = b.c < (int)L[b.l].len ? next_c(b.l, b.c) : b.c;
                else if (b.c == 0 && b.l > a.l && op != 'c' + 1000) {
                    b.l--; b.c = (int)L[b.l].len;
                    int before_nb = 1;
                    const char *s = ls(a.l);
                    for (int i = 0; i < a.c; i++) if (s[i] != ' ' && s[i] != '\t') before_nb = 0;
                    if (before_nb && !force) { r.type = 'l'; a.c = 0; }
                }
                r.a = a; r.b = b;
            }
            if (force == 22) { r.type = 'b'; }
        }
    have_range:
        if (op == 'y' && 0) { }
        {
            Pos sv = cur;
            do_op_range(op, &r, reg, 1);
            if (op == 'y') {
                if (r.type == 'l') { /* the cursor stays on its line for yj, moves up for yk */ if (sv.l < r.a.l || sv.l > r.b.l) { } else if (sv.l == r.a.l) { cur = sv; } else { cur.l = r.a.l; cur.c = byte_at_vcol(cur.l, curswant < 0 ? 100000000 : curswant); } }
            } else cmd_is_change = 1;
            if (op == 'c') { enter_insert_after_op(); }
        }
        if (op != 'c' && op != 'y') { fix_cursor(); }
        break;
    }
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

int cmd_ok_modify(void) {
    if (o.ro || view_mode) { set_err("E21: Cannot make changes, 'modifiable' is off"); return 0; }
    return 1;
}

void fix_cursor_end(void) {
    if (cur.l >= nl) cur.l = nl - 1;
    if (cur.c > (int)L[cur.l].len) cur.c = (int)L[cur.l].len;
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

void undo_line_restore(void) {
    beep();
}

int dot_set_unused;
