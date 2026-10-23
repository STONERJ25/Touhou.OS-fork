/* input.c: reading keys from the terminal, the type-ahead queue (macros, "." repeat, :normal, mappings). */
#include "vi.h"

#define ESC_WAIT_MS 120

int normal_depth;
int in_dot_replay;
int recording;
int last_typed_key;

/* ------------------------------------------------------------------ type-ahead: a stack, the next key is on top */

static int *taq;
static int ta_n, ta_cap;

static void ta_reserve(int extra) {
    if (ta_n + extra <= ta_cap) return;
    int nc = ta_cap ? ta_cap : 256;
    while (nc < ta_n + extra) nc *= 2;
    int *np = realloc(taq, (size_t)nc * sizeof *taq);
    if (!np) pt_oom();
    taq = np;
    ta_cap = nc;
}

int ta_len(void) { return ta_n; }

/* An error cancels macros, mappings and :normal keys that are still waiting. */
void ta_flush(void) { ta_n = 0; }

void ta_push(const int *keys, int n) {
    ta_reserve(n);
    for (int i = n - 1; i >= 0; i--) taq[ta_n++] = keys[i];
}

typedef struct { int *q; int n, cap; } TaSave;

void ta_save(void **save) {
    TaSave *s = malloc(sizeof *s);
    if (!s) pt_oom();
    s->q = taq;
    s->n = ta_n;
    s->cap = ta_cap;
    taq = NULL;
    ta_n = ta_cap = 0;
    *save = s;
}

void ta_restore(void *save) {
    TaSave *s = save;
    free(taq);
    taq = s->q;
    ta_n = s->n;
    ta_cap = s->cap;
    free(s);
}

int key_to_cp(int k) {
    k &= ~K_NOMAP;
    if (k >= PT_K_BASE && k < PT_K_BASE + 0x100) return 0xF700 + (k - PT_K_BASE);
    return k;
}

int cp_to_key(int cp) {
    if (cp >= 0xF700 && cp < 0xF800) return PT_K_BASE + (cp - 0xF700);
    if (pt_is_rawbyte((uint32_t)cp)) return cp - PT_RAWBYTE_BASE;
    return cp;
}

void ta_push_str(const char *s, size_t n, int nomap) {
    int *tmp = malloc((n + 1) * sizeof *tmp);
    if (!tmp) pt_oom();
    int k = 0;
    size_t i = 0;
    while (i < n) {
        uint32_t cp;
        int l = pt_u8_decode((const unsigned char *)s + i, n - i, &cp);
        i += (size_t)l;
        int key = cp_to_key((int)cp);
        tmp[k++] = nomap ? (key | K_NOMAP) : key;
    }
    ta_push(tmp, k);
    free(tmp);
}

/* ------------------------------------------------------------------ the byte source */

/* Bytes come from the push-back stack first, then from the script given with -s, then from the terminal. A script is
 * used up like a closed terminal: vi quits (status 1), so a test that forgot ":wq" cannot hang. */
static unsigned char *scr_buf;
static size_t scr_len, scr_pos;
static int pb_stack[32], pb_n;
int script_mode;

int script_open(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t cap = 4096, n = 0;
    scr_buf = malloc(cap);
    if (!scr_buf) pt_oom();
    for (;;) {
        if (n == cap) { cap *= 2; scr_buf = realloc(scr_buf, cap); if (!scr_buf) pt_oom(); }
        ssize_t r = read(fd, scr_buf + n, cap - n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    scr_len = n;
    scr_pos = 0;
    script_mode = 1;
    return 0;
}

static int src_ready(void) { return pb_n > 0 || scr_pos < scr_len || pt_pending(&term); }

static void unget_byte(int b) { if (pb_n < 32) pb_stack[pb_n++] = b; }

/* One byte: >= 0, -1 on timeout or interrupt, -2 at end of input. */
static int rd_byte(int ms) {
    if (pb_n > 0) return pb_stack[--pb_n];
    if (scr_pos < scr_len) return scr_buf[scr_pos++];
    if (script_mode) return -2;
    if (term.unget >= 0) { int b = term.unget; term.unget = -1; return b; }
    int r = pt_fill(&term, ms);
    if (r < 0) return -2;
    if (r == 0) return -1;
    return term.ibuf[term.ipos++];
}

/* ------------------------------------------------------------------ key decoding */

#define SEQ_WAIT_MS 100                                  /* between the bytes of one escape sequence */

/* CSI finals that stand for a key when no parameter came before them */
static int csi_plain_final(int f) { return f == 'A' || f == 'B' || f == 'C' || f == 'D' || f == 'F' || f == 'H' || f == 'Z'; }

/* After ESC [. Returns the key, K_NONE for a sequence that is complete but unknown (swallowed), or K_ESC after pushing
 * back everything that turned out not to be a sequence. */
static int csi_key(void) {
    int seen[24], n = 0, params[4] = { 0, 0, 0, 0 }, np = 0, fin = -1, have_param = 0;
    for (;;) {
        int b = rd_byte(SEQ_WAIT_MS);
        if (b < 0) break;                                /* incomplete: not a sequence */
        seen[n++] = b;
        if (b >= '0' && b <= '9') { params[np] = params[np] * 10 + (b - '0'); have_param = 1; if (n >= 20) break; continue; }
        if (b == ';') { if (np < 3) np++; have_param = 1; if (n >= 20) break; continue; }
        if ((b >= 0x20 && b <= 0x2F) || b == '?' || b == '>' || b == '=' || b == '<') { have_param = 1; if (n >= 20) break; continue; }
        if (b >= 0x40 && b <= 0x7E) { fin = b; }
        break;
    }
    if (fin >= 0 && (have_param || csi_plain_final(fin))) {
        int k = pt_csi_key(fin, params[0], np >= 1 ? params[1] : 0);
        return k == PT_KEY_NONE ? K_NONE : (k & ~(PT_M_SHIFT | PT_M_ALT | PT_M_CTRL));
    }
    for (int i = n - 1; i >= 0; i--) unget_byte(seen[i]);   /* Esc followed by "[" and what came after it */
    unget_byte('[');
    return K_ESC;
}

/* After ESC O (the SS3 form of the cursor keys and F1-F4, also the keypad in application mode). */
static int ss3_key(void) {
    int b = rd_byte(SEQ_WAIT_MS);
    int k = 0;
    switch (b) {
    case 'A': k = PT_K_UP; break;
    case 'B': k = PT_K_DOWN; break;
    case 'C': k = PT_K_RIGHT; break;
    case 'D': k = PT_K_LEFT; break;
    case 'H': k = PT_K_HOME; break;
    case 'F': k = PT_K_END; break;
    case 'P': case 'Q': case 'R': case 'S': return K_NONE;       /* F1-F4: no meaning here, swallowed */
    case 'M': return K_ENTER;                                      /* keypad Enter */
    case 'j': return '*'; case 'k': return '+'; case 'l': return ','; case 'm': return '-'; case 'n': return '.'; case 'o': return '/';
    case 'p': case 'q': case 'r': case 's': case 't': case 'u': case 'v': case 'w': case 'x': case 'y': return '0' + (b - 'p');
    default: break;
    }
    if (k) return k;
    if (b >= 0) unget_byte(b);
    unget_byte('O');
    return K_ESC;
}

/* Reads one key from the byte source: a code point, 1..26 for Ctrl-A..Z, 27 for Esc, PT_K_* for the special keys.
 * K_NONE means a timeout or an unknown sequence that was swallowed. */
static int term_key(int wait_ms) {
    int c = rd_byte(wait_ms);
    if (c == -2) return K_EOF;
    if (c < 0) return K_NONE;
    if (c == 27) {
        int c1 = rd_byte(src_ready() ? 0 : ESC_WAIT_MS);
        if (c1 == '[') return csi_key();
        if (c1 == 'O') return ss3_key();
        if (c1 >= 0) unget_byte(c1);
        return K_ESC;
    }
    if (c == '\r' || c == '\n') return K_ENTER;
    if (c == 8 || c == 127) return K_BS;
    if (c < 0x80) return c;
    unsigned char seq[4];
    int need = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 ? 1 : 0, n = 1;
    seq[0] = (unsigned char)c;
    if (!need) return PT_RAWBYTE_BASE + c;
    while (n <= need) {
        int b = rd_byte(SEQ_WAIT_MS);
        if (b < 0) break;
        if ((b & 0xC0) != 0x80) { unget_byte(b); break; }
        seq[n++] = (unsigned char)b;
    }
    uint32_t cp;
    if (n != need + 1 || pt_u8_decode(seq, (size_t)n, &cp) != n) {
        for (int i = n - 1; i >= 1; i--) unget_byte(seq[i]);       /* not a valid sequence: the lead byte stays raw */
        return PT_RAWBYTE_BASE + c;
    }
    return (int)cp;
}

/* ------------------------------------------------------------------ macros and "." */

static int *macbuf;
static int macn, maccap;
static int *dotrec, *dotkeys;
static int dotn, dotcap, dotkn, dot_on;
static int dot_count, dot_reg;
extern int cur_count, cur_reg;

void macro_start(int reg) {
    recording = reg;
    macn = 0;
}

void macro_stop(void) {
    if (!recording) return;
    if (macn > 0) macn--;                                /* the q that ended it */
    PtBuf b = { 0 };
    for (int i = 0; i < macn; i++) {
        char u[4];
        int cp = key_to_cp(macbuf[i]);
        pb_add(&b, u, (size_t)pt_u8_encode((uint32_t)cp, u));
    }
    reg_set(recording, b.p ? b.p : "", b.len, 'c', 0);
    pb_free(&b);
    recording = 0;
}

int dot_set(int on) { int old = dot_on; dot_on = on; return old; }
int dot_available(void) { return dotkn > 0; }

void dot_push(int k) {
    if (!dot_on) return;
    if (dotn == dotcap) {
        dotcap = dotcap ? dotcap * 2 : 64;
        dotrec = realloc(dotrec, (size_t)dotcap * sizeof *dotrec);
        if (!dotrec) pt_oom();
    }
    dotrec[dotn++] = k;
}

void dot_begin(void) {
    if (in_dot_replay) { dot_on = 0; return; }
    dotn = 0;
    dot_on = 1;
}

static int dot_nokeep;                                   /* set for visual-mode commands: "." does not repeat them */
void dot_nokeep_set(void) { dot_nokeep = 1; }

void dot_end(int keep) {
    int nokeep = dot_nokeep;
    dot_nokeep = 0;
    if (!dot_on) return;
    dot_on = 0;
    if (!keep || nokeep) return;
    free(dotkeys);
    dotkeys = malloc((size_t)dotn * sizeof *dotkeys + 4);
    if (!dotkeys) pt_oom();
    memcpy(dotkeys, dotrec, (size_t)dotn * sizeof *dotkeys);
    dotkn = dotn;
    dot_count = cur_count;
    dot_reg = cur_reg;
}

void dot_replay(int count) {
    if (!dotkn) return;
    if (count > 0) dot_count = count;
    int pre[40], np = 0;
    if (dot_reg) { pre[np++] = '"'; pre[np++] = dot_reg >= '1' && dot_reg < '9' ? dot_reg + 1 : dot_reg; }
    if (dot_count > 0) {
        char num[16];
        snprintf(num, sizeof num, "%d", dot_count);
        for (char *p = num; *p; p++) pre[np++] = *p;
    }
    int *all = malloc((size_t)(np + dotkn) * sizeof *all);
    if (!all) pt_oom();
    memcpy(all, pre, (size_t)np * sizeof *all);
    memcpy(all + np, dotkeys, (size_t)dotkn * sizeof *all);
    for (int i = 0; i < np + dotkn; i++) all[i] |= K_NOMAP;
    ta_push(all, np + dotkn);
    free(all);
}

/* ------------------------------------------------------------------ mappings */

typedef struct { int *lhs, *rhs; int nl, nr; char *lhs_s, *rhs_s; } Map;
static Map nmaps[64], imaps[64];
static int nnmaps, nimaps;

static int *keys_of(const char *s, int *n) {
    size_t len = strlen(s);
    int *k = malloc((len + 1) * sizeof *k);
    if (!k) pt_oom();
    int c = 0;
    for (size_t i = 0; i < len;) {
        if (s[i] == '<') {                               /* <CR> <Esc> <Space> <Tab> <C-x> <Bar> <lt> <BS> <Leader> */
            const char *e = strchr(s + i, '>');
            if (e && e - (s + i) <= 8) {
                char name[10];
                size_t nlen = (size_t)(e - (s + i) - 1);
                memcpy(name, s + i + 1, nlen);
                name[nlen] = 0;
                int v = -1;
                if (!strcasecmp(name, "CR") || !strcasecmp(name, "Enter") || !strcasecmp(name, "Return")) v = K_ENTER;
                else if (!strcasecmp(name, "Esc")) v = K_ESC;
                else if (!strcasecmp(name, "Space")) v = ' ';
                else if (!strcasecmp(name, "Tab")) v = 9;
                else if (!strcasecmp(name, "BS")) v = K_BS;
                else if (!strcasecmp(name, "Bar")) v = '|';
                else if (!strcasecmp(name, "lt")) v = '<';
                else if (!strcasecmp(name, "Leader")) v = '\\';
                else if (!strcasecmp(name, "Up")) v = PT_K_UP;
                else if (!strcasecmp(name, "Down")) v = PT_K_DOWN;
                else if (!strcasecmp(name, "Left")) v = PT_K_LEFT;
                else if (!strcasecmp(name, "Right")) v = PT_K_RIGHT;
                else if (nlen == 3 && (name[0] == 'C' || name[0] == 'c') && name[1] == '-' && isalpha((unsigned char)name[2])) v = toupper((unsigned char)name[2]) & 0x1F;
                if (v >= 0) { k[c++] = v; i = (size_t)(e - s) + 1; continue; }
            }
        }
        uint32_t cp;
        int l = pt_u8_decode((const unsigned char *)s + i, len - i, &cp);
        k[c++] = (int)cp;
        i += (size_t)l;
    }
    *n = c;
    return k;
}

void map_add(int imode, const char *lhs, const char *rhs) {
    Map *tab = imode ? imaps : nmaps;
    int *cnt = imode ? &nimaps : &nnmaps;
    int nlk, nrk;
    int *lk = keys_of(lhs, &nlk), *rk = keys_of(rhs, &nrk);
    for (int i = 0; i < *cnt; i++) {
        if (tab[i].nl == nlk && !memcmp(tab[i].lhs, lk, (size_t)nlk * sizeof *lk)) {
            free(tab[i].lhs); free(tab[i].rhs); free(tab[i].lhs_s); free(tab[i].rhs_s);
            tab[i] = (Map){ lk, rk, nlk, nrk, strdup(lhs), strdup(rhs) };
            return;
        }
    }
    if (*cnt >= 64) { free(lk); free(rk); return; }
    tab[(*cnt)++] = (Map){ lk, rk, nlk, nrk, strdup(lhs), strdup(rhs) };
}

int map_del(int imode, const char *lhs) {
    Map *tab = imode ? imaps : nmaps;
    int *cnt = imode ? &nimaps : &nnmaps;
    for (int i = 0; i < *cnt; i++) {
        if (!strcmp(tab[i].lhs_s, lhs)) {
            free(tab[i].lhs); free(tab[i].rhs); free(tab[i].lhs_s); free(tab[i].rhs_s);
            memmove(&tab[i], &tab[i + 1], (size_t)(*cnt - i - 1) * sizeof(Map));
            (*cnt)--;
            return 1;
        }
    }
    return 0;
}

void map_list(int imode) {
    Map *tab = imode ? imaps : nmaps;
    int cnt = imode ? nimaps : nnmaps;
    PtBuf b = { 0 };
    for (int i = 0; i < cnt; i++) pb_addf(&b, "%s%s  %s", i ? " | " : "", tab[i].lhs_s, tab[i].rhs_s);
    if (!cnt) pb_adds(&b, "No mapping found");
    set_msg("%s", b.p);
    pb_free(&b);
}

static const char *abbr_l[16], *abbr_r[16];
static int nabbr;

void abbr_add(const char *lhs, const char *rhs) {
    for (int i = 0; i < nabbr; i++) if (!strcmp(abbr_l[i], lhs)) { free((char *)abbr_r[i]); abbr_r[i] = strdup(rhs); return; }
    if (nabbr < 16) { abbr_l[nabbr] = strdup(lhs); abbr_r[nabbr] = strdup(rhs); nabbr++; }
}

const char *abbr_find(const char *w, size_t n) {
    for (int i = 0; i < nabbr; i++) if (strlen(abbr_l[i]) == n && !memcmp(abbr_l[i], w, n)) return abbr_r[i];
    return NULL;
}

/* ------------------------------------------------------------------ getkey */

int key_from_ta;                                         /* the key returned last came from the type-ahead queue (macro, "." ...) */
static int macro_note(int k);

/* The next key. Typed keys (from the terminal or the script) are what a macro records; keys from the queue are not. */
static int raw_next(int *from_ta) {
    if (ta_n > 0) { *from_ta = 1; return taq[--ta_n]; }
    *from_ta = 0;
    if (normal_depth > 0) return K_ESC;                  /* :normal ran out of keys: finish like <Esc> */
    for (;;) {
        if (!src_ready()) redraw();
        int k = term_key(-1);
        if (k == K_EOF) {
            if (!quit_flag) { quit_flag = 1; exit_status = 1; }
            return K_ESC;
        }
        if (k == K_NONE) continue;
        macro_note(k);
        return k;
    }
}

static int mapped_key(int *from_ta) {
    int k = raw_next(from_ta);
    if (k & K_NOMAP) return k & ~K_NOMAP;
    int insert = mode == M_INSERT || mode == M_REPLACE;
    Map *tab = insert ? imaps : nmaps;
    int cnt = insert ? nimaps : nnmaps;
    if (!cnt || cmdline_active) return k;
    int buf[16], n = 0;
    buf[n++] = k;
    for (;;) {
        int exact = -1, prefix = 0;
        for (int i = 0; i < cnt; i++) {
            if (tab[i].nl < n || memcmp(tab[i].lhs, buf, (size_t)n * sizeof *buf)) continue;
            if (tab[i].nl == n) exact = i; else prefix = 1;
        }
        if (exact >= 0 && !prefix) {
            int nr = tab[exact].nr;
            int *r = malloc((size_t)(nr + 1) * sizeof *r);
            if (!r) pt_oom();
            for (int i = 0; i < nr; i++) r[i] = tab[exact].rhs[i] | K_NOMAP;
            ta_push(r, nr);
            free(r);
            return mapped_key(from_ta);
        }
        if (!prefix || n >= 15) break;
        int k2, f2;
        if (ta_n > 0) { k2 = taq[--ta_n]; f2 = 1; }
        else {
            if (!src_ready()) redraw();
            k2 = term_key(1000);
            f2 = 0;
            if (k2 == K_NONE || k2 == K_EOF) break;
            macro_note(k2);
        }
        (void)f2;
        if (k2 & K_NOMAP) { ta_push(&k2, 1); break; }
        buf[n++] = k2;
    }
    if (n > 1) {                                         /* no mapping matched: give the keys back unmapped */
        for (int i = n - 1; i >= 1; i--) { int kk = buf[i] | K_NOMAP; ta_push(&kk, 1); }
    }
    return buf[0];
}

static int macro_note(int k) {
    if (!recording) return k;
    if (macn == maccap) {
        maccap = maccap ? maccap * 2 : 64;
        macbuf = realloc(macbuf, (size_t)maccap * sizeof *macbuf);
        if (!macbuf) pt_oom();
    }
    macbuf[macn++] = k;
    return k;
}

int getkey(void) {
    int from_ta;
    int k = mapped_key(&from_ta);
    key_from_ta = from_ta;
    dot_push(k);
    last_typed_key = k;
    return k;
}
