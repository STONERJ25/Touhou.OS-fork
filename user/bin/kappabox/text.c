/* Text filters: head tail wc sort uniq tr cut tee rev tac nl fold paste comm cmp expand unexpand echo */
#include "kappabox.h"

/* ------------------------------------------------------------------ echo */

APPLET(echo) {
    int nl = 1, esc = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o = argv[i] + 1;
        while (*o == 'n' || *o == 'e' || *o == 'E') o++;
        if (*o) break;
        for (o = argv[i] + 1; *o; o++) { if (*o == 'n') nl = 0; else if (*o == 'e') esc = 1; else esc = 0; }
    }
    for (int first = 1; i < argc; i++, first = 0) {
        if (!first) putchar(' ');
        for (const char *p = argv[i]; *p; p++) {
            if (esc && *p == '\\' && p[1]) {
                p++;
                switch (*p) {
                case 'n': putchar('\n'); break;
                case 't': putchar('\t'); break;
                case 'r': putchar('\r'); break;
                case 'a': putchar('\a'); break;
                case 'b': putchar('\b'); break;
                case 'f': putchar('\f'); break;
                case 'v': putchar('\v'); break;
                case 'e': putchar(27); break;
                case 'c': return 0;
                case '\\': putchar('\\'); break;
                case '0': { int v = 0, k = 0; while (k < 3 && p[1] >= '0' && p[1] <= '7') { v = v * 8 + (*++p - '0'); k++; } putchar(v); break; }
                default: putchar('\\'); putchar(*p);
                }
            } else putchar(*p);
        }
    }
    if (nl) putchar('\n');
    return 0;
}

/* ------------------------------------------------------------------ head / tail */

typedef struct { long n; int bytes; int from_start; int quiet, verbose, nfiles, shown; } HeadTail;

static void header(HeadTail *o, const char *name) {
    if (o->verbose || (o->nfiles > 1 && !o->quiet)) printf("%s==> %s <==\n", o->shown ? "\n" : "", !strcmp(name, "-") ? "standard input" : name);
    o->shown++;
}

static int head_one(FILE *f, const char *name, void *ctx) {
    HeadTail *o = ctx;
    header(o, name);
    long left = o->n;
    int c;
    if (o->bytes) { while (left-- > 0 && (c = fgetc(f)) != EOF) putchar(c); return 0; }
    if (o->n < 0) {                                 /* head -n -N: everything except the last N lines */
        char **lines;
        size_t n = kb_read_lines(f, &lines);
        for (size_t i = 0; i + (size_t)(-o->n) < n; i++) puts(lines[i]);
        kb_free_lines(lines, n);
        return 0;
    }
    while (left > 0 && (c = fgetc(f)) != EOF) {
        putchar(c);
        if (c == '\n') left--;
    }
    return 0;
}

static int parse_count(const char *s, long *out, int *plus) {
    char *end;
    if (*s == '+') { *plus = 1; s++; }
    long v = strtol(s, &end, 10);
    if (end == s) return -1;
    if (*end == 'k' || *end == 'K') v *= 1024;
    else if (*end == 'm' || *end == 'M') v *= 1024 * 1024;
    else if (*end == 'b') v *= 512;
    *out = v;
    return 0;
}

APPLET(head) {
    HeadTail o = { 10, 0, 1, 0, 0, 0, 0 };
    int plus = 0, c;
    optind = 0;
    for (int i = 1; i < argc; i++)                  /* head -5 */
        if (argv[i][0] == '-' && isdigit((unsigned char)argv[i][1])) { o.n = atol(argv[i] + 1); memmove(&argv[i], &argv[i + 1], (size_t)(argc - i) * sizeof *argv); argc--; i--; }
    while ((c = getopt(argc, argv, "n:c:qv")) != -1) {
        switch (c) {
        case 'n': if (parse_count(optarg, &o.n, &plus)) { kb_error("invalid number of lines: '%s'", optarg); return 1; } break;
        case 'c': if (parse_count(optarg, &o.n, &plus)) { kb_error("invalid number of bytes: '%s'", optarg); return 1; } o.bytes = 1; break;
        case 'q': o.quiet = 1; break;
        case 'v': o.verbose = 1; break;
        default: return 1;
        }
    }
    o.nfiles = argc - optind;
    return kb_each_input(argc, argv, optind, head_one, &o);
}

static int tail_one(FILE *f, const char *name, void *ctx) {
    HeadTail *o = ctx;
    header(o, name);
    if (o->bytes) {
        long cap = o->n > 0 ? o->n : 1, len = 0, pos = 0;
        char *ring = malloc((size_t)cap);
        int c;
        long skip = o->from_start ? o->n - 1 : 0;
        while ((c = fgetc(f)) != EOF) {
            if (o->from_start) { if (skip > 0) { skip--; continue; } putchar(c); continue; }
            ring[pos] = (char)c;
            pos = (pos + 1) % cap;
            if (len < cap) len++;
        }
        if (!o->from_start && o->n > 0) {
            long start = (pos - len + cap) % cap;
            for (long i = 0; i < len; i++) putchar(ring[(start + i) % cap]);
        }
        free(ring);
        return 0;
    }
    if (o->from_start) {                            /* tail -n +N */
        long line = 1;
        int c;
        while ((c = fgetc(f)) != EOF) {
            if (line >= o->n) putchar(c);
            if (c == '\n') line++;
        }
        return 0;
    }
    if (o->n == 0) return 0;
    char **ring = calloc((size_t)o->n, sizeof *ring);
    size_t pos = 0, count = 0;
    char *buf = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&buf, &cap, f)) >= 0) {
        free(ring[pos]);
        ring[pos] = strdup(buf);
        pos = (pos + 1) % (size_t)o->n;
        count++;
    }
    free(buf);
    size_t total = count < (size_t)o->n ? count : (size_t)o->n;
    size_t start = (pos + (size_t)o->n - total) % (size_t)o->n;
    for (size_t i = 0; i < total; i++) fputs(ring[(start + i) % (size_t)o->n], stdout);
    for (long i = 0; i < o->n; i++) free(ring[i]);
    free(ring);
    return 0;
}

APPLET(tail) {
    HeadTail o = { 10, 0, 0, 0, 0, 0, 0 };
    int plus = 0, c, follow = 0;
    optind = 0;
    for (int i = 1; i < argc; i++)
        if (argv[i][0] == '-' && isdigit((unsigned char)argv[i][1])) { o.n = atol(argv[i] + 1); memmove(&argv[i], &argv[i + 1], (size_t)(argc - i) * sizeof *argv); argc--; i--; }
    while ((c = getopt(argc, argv, "n:c:qvfF")) != -1) {
        switch (c) {
        case 'n': plus = 0; if (parse_count(optarg, &o.n, &plus)) { kb_error("invalid number of lines: '%s'", optarg); return 1; } o.from_start = plus; break;
        case 'c': plus = 0; if (parse_count(optarg, &o.n, &plus)) { kb_error("invalid number of bytes: '%s'", optarg); return 1; } o.bytes = 1; o.from_start = plus; break;
        case 'q': o.quiet = 1; break;
        case 'v': o.verbose = 1; break;
        case 'f': case 'F': follow = 1; break;
        default: return 1;
        }
    }
    o.nfiles = argc - optind;
    int rc = kb_each_input(argc, argv, optind, tail_one, &o);
    fflush(stdout);
    if (follow && optind < argc) {
        FILE *f = fopen(argv[argc - 1], "r");
        if (!f) return 1;
        fseek(f, 0, SEEK_END);
        for (;;) {
            char buf[1024];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) { fwrite(buf, 1, n, stdout); fflush(stdout); }
            clearerr(f);
            usleep(200000);
        }
    }
    return rc;
}

/* ------------------------------------------------------------------ wc */

typedef struct { int l, w, c, m, L; unsigned long long tl, tw, tc, tm, tL; int nfiles; int width; } Wc;

static void wc_print(Wc *o, unsigned long long l, unsigned long long w, unsigned long long c, unsigned long long m,
                     unsigned long long L, const char *name) {
    int cols = (o->l != 0) + (o->w != 0) + (o->c != 0) + (o->m != 0) + (o->L != 0);
    int wd = (cols == 1 && o->nfiles <= 1) ? 0 : o->width, first = 1;
#define COL(flag, v) if (flag) { printf("%s%*llu", first ? "" : " ", wd, v); first = 0; }
    COL(o->l, l) COL(o->w, w) COL(o->m, m) COL(o->c, c) COL(o->L, L)
#undef COL
    if (name) printf(" %s", name);
    putchar('\n');
}

static int wc_one(FILE *f, const char *name, void *ctx) {
    Wc *o = ctx;
    unsigned long long l = 0, w = 0, c = 0, m = 0, maxl = 0, cur = 0;
    int ch, in_word = 0;
    while ((ch = fgetc(f)) != EOF) {
        c++;
        if ((ch & 0xC0) != 0x80) m++;
        if (ch == '\n') { l++; if (cur > maxl) maxl = cur; cur = 0; }
        else if (ch == '\t') cur = (cur + 8) & ~7ull;
        else cur++;
        if (isspace(ch)) in_word = 0;
        else if (!in_word) { in_word = 1; w++; }
    }
    if (cur > maxl) maxl = cur;
    o->tl += l; o->tw += w; o->tc += c; o->tm += m; if (maxl > o->tL) o->tL = maxl;
    wc_print(o, l, w, c, m, maxl, !strcmp(name, "-") && o->nfiles == 0 ? NULL : name);
    return 0;
}

APPLET(wc) {
    Wc o = {0};
    int c;
    optind = 0;
    while ((c = getopt(argc, argv, "lwcmL")) != -1) {
        switch (c) {
        case 'l': o.l = 1; break;
        case 'w': o.w = 1; break;
        case 'c': o.c = 1; break;
        case 'm': o.m = 1; break;
        case 'L': o.L = 1; break;
        default: return 1;
        }
    }
    if (!(o.l | o.w | o.c | o.m | o.L)) o.l = o.w = o.c = 1;
    o.nfiles = argc - optind;
    o.width = 7;
    int rc = kb_each_input(argc, argv, optind, wc_one, &o);
    if (o.nfiles > 1) wc_print(&o, o.tl, o.tw, o.tc, o.tm, o.tL, "total");
    return rc;
}

/* ------------------------------------------------------------------ sort */

typedef struct {
    int reverse, numeric, fold, unique, blanks, stable, check;
    int key_start, key_end, key_start_char;                 /* -k FIELD (1-based, 0 = whole line) */
    int sep;                                                /* -t */
    int key_numeric, key_fold, key_rev;
} SortOpts;

static SortOpts g_so;

static const char *sort_key(const char *line, int *len) {
    const char *p = line;
    if (g_so.key_start) {
        for (int f = 1; f < g_so.key_start; f++) {
            if (g_so.sep) { const char *q = strchr(p, g_so.sep); if (!q) { p += strlen(p); break; } p = q + 1; }
            else { while (*p == ' ' || *p == '\t') p++; while (*p && *p != ' ' && *p != '\t') p++; }
        }
        if (!g_so.sep) while (*p == ' ' || *p == '\t') p++;
    }
    const char *e = p + strlen(p);
    if (g_so.key_end) {
        const char *q = line;
        for (int f = 1; f <= g_so.key_end; f++) {
            if (g_so.sep) { const char *r = strchr(q, g_so.sep); if (!r) { q += strlen(q); break; } if (f == g_so.key_end) { q = r; break; } q = r + 1; }
            else { while (*q == ' ' || *q == '\t') q++; while (*q && *q != ' ' && *q != '\t') q++; if (f == g_so.key_end) break; }
        }
        if (q >= p) e = q;
    }
    if (g_so.blanks) while (p < e && (*p == ' ' || *p == '\t')) p++;
    *len = (int)(e - p);
    return p;
}

static int sort_cmp_keys(const char *a, const char *b) {
    int la, lb;
    const char *ka = sort_key(a, &la), *kb = sort_key(b, &lb);
    int numeric = g_so.numeric || g_so.key_numeric, fold = g_so.fold || g_so.key_fold;
    int r;
    if (numeric) {
        char ta[64], tb[64];
        snprintf(ta, sizeof ta, "%.*s", la < 63 ? la : 63, ka);
        snprintf(tb, sizeof tb, "%.*s", lb < 63 ? lb : 63, kb);
        double x = strtod(ta, NULL), y = strtod(tb, NULL);
        r = x < y ? -1 : x > y ? 1 : 0;
    } else {
        int n = la < lb ? la : lb;
        r = 0;
        for (int i = 0; i < n && !r; i++) {
            int x = (unsigned char)ka[i], y = (unsigned char)kb[i];
            if (fold) { x = tolower(x); y = tolower(y); }
            r = x - y;
        }
        if (!r) r = la - lb;
    }
    return r;
}

static int sort_cmp(const char *a, const char *b) {
    int r = sort_cmp_keys(a, b);
    if (r && g_so.key_rev) r = -r;
    if (!r && !g_so.stable && !g_so.unique) r = strcmp(a, b);
    return g_so.reverse ? -r : r;
}

static void msort(char **v, char **tmp, size_t n) {
    if (n < 2) return;
    size_t mid = n / 2;
    msort(v, tmp, mid);
    msort(v + mid, tmp, n - mid);
    size_t i = 0, j = mid, k = 0;
    while (i < mid && j < n) tmp[k++] = sort_cmp(v[i], v[j]) <= 0 ? v[i++] : v[j++];
    while (i < mid) tmp[k++] = v[i++];
    while (j < n) tmp[k++] = v[j++];
    memcpy(v, tmp, n * sizeof *v);
}

typedef struct { char **lines; size_t n, cap; } LineList;

static int sort_collect(FILE *f, const char *name, void *ctx) {
    (void)name;
    LineList *l = ctx;
    char **v;
    size_t n = kb_read_lines(f, &v);
    l->lines = realloc(l->lines, (l->n + n + 1) * sizeof *l->lines);
    memcpy(l->lines + l->n, v, n * sizeof *v);
    l->n += n;
    free(v);
    return 0;
}

APPLET(sort) {
    memset(&g_so, 0, sizeof g_so);
    const char *outfile = NULL;
    int c;
    optind = 0;
    while ((c = getopt(argc, argv, "rnfubsck:t:o:gMhdiz")) != -1) {
        switch (c) {
        case 'r': g_so.reverse = 1; break;
        case 'n': case 'g': case 'h': g_so.numeric = 1; break;
        case 'f': g_so.fold = 1; break;
        case 'u': g_so.unique = 1; break;
        case 'b': g_so.blanks = 1; break;
        case 's': g_so.stable = 1; break;
        case 'c': g_so.check = 1; break;
        case 't': g_so.sep = (unsigned char)optarg[0]; break;
        case 'o': outfile = optarg; break;
        case 'k': {
            char *p = optarg;
            g_so.key_start = (int)strtol(p, &p, 10);
            if (*p == '.') strtol(p + 1, &p, 10);
            while (*p && *p != ',') { if (*p == 'n') g_so.key_numeric = 1; else if (*p == 'f') g_so.key_fold = 1; else if (*p == 'r') g_so.key_rev = 1; else if (*p == 'b') g_so.blanks = 1; p++; }
            if (*p == ',') {
                p++;
                g_so.key_end = (int)strtol(p, &p, 10);
                if (*p == '.') strtol(p + 1, &p, 10);
                while (*p) { if (*p == 'n') g_so.key_numeric = 1; else if (*p == 'f') g_so.key_fold = 1; else if (*p == 'r') g_so.key_rev = 1; p++; }
            }
            break;
        }
        case 'M': case 'd': case 'i': case 'z': break;
        default: return 2;
        }
    }
    LineList l = {0};
    int rc = kb_each_input(argc, argv, optind, sort_collect, &l);
    char **tmp = malloc((l.n + 1) * sizeof *tmp);
    if (g_so.check) {
        for (size_t i = 1; i < l.n; i++) if (sort_cmp(l.lines[i - 1], l.lines[i]) > 0) { kb_error("disorder at line %zu", i + 1); return 1; }
        return 0;
    }
    msort(l.lines, tmp, l.n);
    FILE *out = outfile ? fopen(outfile, "w") : stdout;
    if (!out) { kb_error("%s: %s", outfile, strerror(errno)); return 2; }
    for (size_t i = 0; i < l.n; i++) {
        if (g_so.unique && i && sort_cmp_keys(l.lines[i - 1], l.lines[i]) == 0) continue;
        fputs(l.lines[i], out);
        fputc('\n', out);
    }
    if (outfile) fclose(out);
    free(tmp);
    kb_free_lines(l.lines, l.n);
    return rc;
}

/* ------------------------------------------------------------------ uniq */

typedef struct { int count, dup_only, uniq_only, icase; long skip_fields; } Uniq;

static int uniq_eq(const Uniq *o, const char *a, const char *b) {
    for (long f = 0; f < o->skip_fields; f++) {
        while (*a == ' ' || *a == '\t') a++;
        while (*a && *a != ' ' && *a != '\t') a++;
        while (*b == ' ' || *b == '\t') b++;
        while (*b && *b != ' ' && *b != '\t') b++;
    }
    return o->icase ? !strcasecmp(a, b) : !strcmp(a, b);
}

static void uniq_flush(const Uniq *o, const char *line, long n) {
    if (!line) return;
    if (o->dup_only && n < 2) return;
    if (o->uniq_only && n > 1) return;
    if (o->count) printf("%7ld %s\n", n, line);
    else puts(line);
}

static int uniq_one(FILE *f, const char *name, void *ctx) {
    (void)name;
    Uniq *o = ctx;
    char **lines;
    size_t n = kb_read_lines(f, &lines);
    const char *cur = NULL;
    long count = 0;
    for (size_t i = 0; i < n; i++) {
        if (cur && uniq_eq(o, cur, lines[i])) { count++; continue; }
        uniq_flush(o, cur, count);
        cur = lines[i];
        count = 1;
    }
    uniq_flush(o, cur, count);
    kb_free_lines(lines, n);
    return 0;
}

APPLET(uniq) {
    Uniq o = {0};
    int c;
    optind = 0;
    while ((c = getopt(argc, argv, "cduif:")) != -1) {
        switch (c) {
        case 'c': o.count = 1; break;
        case 'd': o.dup_only = 1; break;
        case 'u': o.uniq_only = 1; break;
        case 'i': o.icase = 1; break;
        case 'f': o.skip_fields = atol(optarg); break;
        default: return 1;
        }
    }
    return kb_each_input(optind < argc ? optind + 1 : argc, argv, optind, uniq_one, &o);
}

/* ------------------------------------------------------------------ tr */

static int tr_class(const char *name, size_t n, unsigned char *out) {
    static const struct { const char *name; int (*fn)(int); } tab[] = {
        { "alpha", isalpha }, { "digit", isdigit }, { "alnum", isalnum }, { "upper", isupper }, { "lower", islower },
        { "space", isspace }, { "blank", isblank }, { "punct", ispunct }, { "print", isprint }, { "graph", isgraph },
        { "cntrl", iscntrl }, { "xdigit", isxdigit },
    };
    int k = 0;
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strlen(tab[i].name) == n && !strncmp(tab[i].name, name, n)) {
            for (int c = 0; c < 256; c++) if (tab[i].fn(c)) out[k++] = (unsigned char)c;
            return k;
        }
    return -1;
}

static int tr_escape(const char **pp) {
    const char *p = *pp;
    int c = (unsigned char)*p++;
    if (c == '\\' && *p) {
        c = (unsigned char)*p++;
        switch (c) {
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        case 'a': c = '\a'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'v': c = '\v'; break;
        case '\\': c = '\\'; break;
        default:
            if (c >= '0' && c <= '7') { int v = c - '0', k = 1; while (k < 3 && *p >= '0' && *p <= '7') { v = v * 8 + (*p++ - '0'); k++; } c = v; }
        }
    }
    *pp = p;
    return c;
}

static int tr_expand(const char *s, unsigned char *out) {
    int n = 0;
    while (*s && n < 1024) {
        if (s[0] == '[' && s[1] == ':') {
            const char *e = strstr(s + 2, ":]");
            if (e) { int k = tr_class(s + 2, (size_t)(e - s - 2), out + n); if (k >= 0) { n += k; s = e + 2; continue; } }
        }
        int a = tr_escape(&s);
        if (*s == '-' && s[1]) {
            s++;
            int b = tr_escape(&s);
            for (int c = a; c <= b && n < 1024; c++) out[n++] = (unsigned char)c;
        } else out[n++] = (unsigned char)a;
    }
    return n;
}

APPLET(tr) {
    int del = 0, squeeze = 0, comp = 0, trunc = 0, c;
    optind = 0;
    while ((c = getopt(argc, argv, "dscCt")) != -1) {
        switch (c) {
        case 'd': del = 1; break;
        case 's': squeeze = 1; break;
        case 'c': case 'C': comp = 1; break;
        case 't': trunc = 1; break;
        default: return 1;
        }
    }
    if (optind >= argc || (!del && !squeeze && optind + 1 >= argc)) { kb_error("missing operand"); return 1; }
    unsigned char s1[1100], s2[1100];
    int n1 = tr_expand(argv[optind], s1), n2 = optind + 1 < argc ? tr_expand(argv[optind + 1], s2) : 0;
    if (comp) {
        unsigned char in[256] = {0}, all[256];
        int k = 0;
        for (int i = 0; i < n1; i++) in[s1[i]] = 1;
        for (int ch = 0; ch < 256; ch++) if (!in[ch]) all[k++] = (unsigned char)ch;
        memcpy(s1, all, (size_t)k);
        n1 = k;
    }
    unsigned char map[256], in1[256] = {0}, in2[256] = {0};
    for (int i = 0; i < 256; i++) map[i] = (unsigned char)i;
    for (int i = 0; i < n1; i++) in1[s1[i]] = 1;
    if (!del && n2) {
        if (trunc && n2 < n1) n1 = n2;
        for (int i = 0; i < n1; i++) map[s1[i]] = s2[i < n2 ? i : n2 - 1];
        for (int i = 0; i < n2; i++) in2[s2[i]] = 1;
    } else if (squeeze && optind + 1 < argc) for (int i = 0; i < n2; i++) in2[s2[i]] = 1;
    int prev = -1, ch;
    const unsigned char *sq = (del && optind + 1 < argc) || (!del && n2) ? in2 : in1;
    while ((ch = getchar()) != EOF) {
        if (del && in1[ch]) continue;
        int o = del ? ch : (in1[ch] ? map[ch] : ch);
        if (squeeze && o == prev && sq[o]) continue;
        putchar(o);
        prev = o;
    }
    return 0;
}

/* ------------------------------------------------------------------ cut */

typedef struct { int lo[32], hi[32], n; int delim, only_delim, mode_c; const char *outdelim; } Cut;

static int cut_one(FILE *f, const char *name, void *ctx) {
    (void)name;
    Cut *o = ctx;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) >= 0) {
        if (len && line[len - 1] == '\n') line[--len] = 0;
        if (o->mode_c) {
            int first = 1;
            for (int i = 0; i < len; i++) if (kb_in_ranges(i + 1, o->lo, o->hi, o->n)) { putchar(line[i]); first = 0; }
            (void)first;
            putchar('\n');
            continue;
        }
        if (!strchr(line, o->delim)) { if (!o->only_delim) puts(line); continue; }
        int field = 1, out_any = 0;
        char *p = line;
        for (;;) {
            char *q = strchr(p, o->delim);
            if (kb_in_ranges(field, o->lo, o->hi, o->n)) {
                if (out_any) fputs(o->outdelim, stdout);
                if (q) fwrite(p, 1, (size_t)(q - p), stdout); else fputs(p, stdout);
                out_any = 1;
            }
            if (!q) break;
            p = q + 1;
            field++;
        }
        putchar('\n');
    }
    free(line);
    return 0;
}

APPLET(cut) {
    Cut o = { .delim = '\t', .outdelim = NULL };
    const char *spec = NULL;
    int c;
    optind = 0;
    static const struct option lo[] = { { "output-delimiter", 1, NULL, 'O' }, { "delimiter", 1, NULL, 'd' }, { "fields", 1, NULL, 'f' }, { 0, 0, 0, 0 } };
    while ((c = getopt_long(argc, argv, "f:c:b:d:s", lo, NULL)) != -1) {
        switch (c) {
        case 'f': spec = optarg; break;
        case 'c': case 'b': spec = optarg; o.mode_c = 1; break;
        case 'd': o.delim = (unsigned char)optarg[0]; break;
        case 's': o.only_delim = 1; break;
        case 'O': o.outdelim = optarg; break;
        default: return 1;
        }
    }
    if (!spec) { kb_error("you must specify a list of bytes, characters, or fields"); return 1; }
    static char dstr[2];
    dstr[0] = (char)o.delim;
    if (!o.outdelim) o.outdelim = dstr;
    o.n = kb_parse_ranges(spec, o.lo, o.hi, 32);
    return kb_each_input(argc, argv, optind, cut_one, &o);
}

/* ------------------------------------------------------------------ tee / rev / tac / nl / fold / paste / comm / cmp / expand */

APPLET(tee) {
    int append = 0, c;
    optind = 0;
    while ((c = getopt(argc, argv, "ai")) != -1) { if (c == 'a') append = 1; else if (c != 'i') return 1; }
    int nf = argc - optind, rc = 0;
    FILE **files = calloc((size_t)nf + 1, sizeof *files);
    for (int i = 0; i < nf; i++) {
        files[i] = fopen(argv[optind + i], append ? "a" : "w");
        if (!files[i]) { kb_error("%s: %s", argv[optind + i], strerror(errno)); rc = 1; }
    }
    char buf[4096];
    ssize_t n;
    while ((n = read(0, buf, sizeof buf)) > 0) {
        write(1, buf, (size_t)n);
        for (int i = 0; i < nf; i++) if (files[i]) fwrite(buf, 1, (size_t)n, files[i]);
    }
    for (int i = 0; i < nf; i++) if (files[i]) fclose(files[i]);
    free(files);
    return rc;
}

static int rev_one(FILE *f, const char *name, void *ctx) {
    (void)name; (void)ctx;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) >= 0) {
        if (len && line[len - 1] == '\n') line[--len] = 0;
        ssize_t end = len;
        while (end > 0) {                           /* walk backwards one UTF-8 character at a time */
            ssize_t start = end - 1;
            while (start > 0 && ((unsigned char)line[start] & 0xC0) == 0x80) start--;
            fwrite(line + start, 1, (size_t)(end - start), stdout);
            end = start;
        }
        putchar('\n');
    }
    free(line);
    return 0;
}

APPLET(rev) { return kb_each_input(argc, argv, 1, rev_one, NULL); }

static int tac_one(FILE *f, const char *name, void *ctx) {
    (void)name; (void)ctx;
    char **lines;
    size_t n = kb_read_lines(f, &lines);
    for (size_t i = n; i-- > 0;) puts(lines[i]);
    kb_free_lines(lines, n);
    return 0;
}

APPLET(tac) { return kb_each_input(argc, argv, 1, tac_one, NULL); }

typedef struct { int all; long n, width; const char *sep; } Nl;

static int nl_one(FILE *f, const char *name, void *ctx) {
    (void)name;
    Nl *o = ctx;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) >= 0) {
        if (len && line[len - 1] == '\n') line[--len] = 0;
        if (o->all || len) printf("%*ld%s%s\n", (int)o->width, o->n++, o->sep, line);
        else printf("%*s%s\n", (int)o->width + (int)strlen(o->sep), "", line);
    }
    free(line);
    return 0;
}

APPLET(nl) {
    Nl o = { 0, 1, 6, "\t" };
    int c;
    optind = 0;
    while ((c = getopt(argc, argv, "b:w:s:v:n:")) != -1) {
        switch (c) {
        case 'b': o.all = optarg[0] == 'a'; break;
        case 'w': o.width = atol(optarg); break;
        case 's': o.sep = optarg; break;
        case 'v': o.n = atol(optarg); break;
        case 'n': break;
        default: return 1;
        }
    }
    return kb_each_input(argc, argv, optind, nl_one, &o);
}

typedef struct { int width, spaces; } Fold;

static int fold_one(FILE *f, const char *name, void *ctx) {
    (void)name;
    Fold *o = ctx;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) >= 0) {
        if (len && line[len - 1] == '\n') line[--len] = 0;
        ssize_t pos = 0;
        while (len - pos > o->width) {
            ssize_t cut = pos + o->width;
            if (o->spaces) {
                ssize_t k = cut;
                while (k > pos && line[k - 1] != ' ') k--;
                if (k > pos) cut = k;
            }
            fwrite(line + pos, 1, (size_t)(cut - pos), stdout);
            putchar('\n');
            pos = cut;
        }
        fputs(line + pos, stdout);
        putchar('\n');
    }
    free(line);
    return 0;
}

APPLET(fold) {
    Fold o = { 80, 0 };
    int c;
    optind = 0;
    while ((c = getopt(argc, argv, "w:s")) != -1) { if (c == 'w') o.width = atoi(optarg); else if (c == 's') o.spaces = 1; else return 1; }
    if (o.width < 1) o.width = 1;
    return kb_each_input(argc, argv, optind, fold_one, &o);
}

APPLET(paste) {
    const char *delims = "\t";
    int serial = 0, c;
    optind = 0;
    while ((c = getopt(argc, argv, "d:s")) != -1) { if (c == 'd') delims = optarg; else if (c == 's') serial = 1; else return 1; }
    int nf = argc - optind;
    static char *dash[] = { "-" };
    char **names = nf < 1 ? dash : &argv[optind];
    if (nf < 1) nf = 1;
    size_t nd = strlen(delims);
    FILE **fs = calloc((size_t)nf, sizeof *fs);
    for (int i = 0; i < nf; i++) {
        fs[i] = !strcmp(names[i], "-") ? stdin : fopen(names[i], "r");
        if (!fs[i]) { kb_error("%s: %s", names[i], strerror(errno)); return 1; }
    }
    char *line = NULL;
    size_t cap = 0;
    if (serial) {
        for (int i = 0; i < nf; i++) {
            int first = 1;
            size_t k = 0;
            ssize_t len;
            while ((len = getline(&line, &cap, fs[i])) >= 0) {
                if (len && line[len - 1] == '\n') line[--len] = 0;
                if (!first && nd) putchar(delims[k++ % nd]);
                fputs(line, stdout);
                first = 0;
            }
            putchar('\n');
        }
        return 0;
    }
    for (;;) {
        int any = 0;
        for (int i = 0; i < nf; i++) {
            ssize_t len = fs[i] ? getline(&line, &cap, fs[i]) : -1;
            if (len < 0) { if (fs[i]) { if (fs[i] != stdin) fclose(fs[i]); fs[i] = NULL; } line[0] = 0; len = 0; }
            else { any = 1; if (len && line[len - 1] == '\n') line[len - 1] = 0; }
            if (i && nd) putchar(delims[(size_t)(i - 1) % nd]);
            fputs(line ? line : "", stdout);
        }
        if (!any) break;
        putchar('\n');
    }
    return 0;
}

APPLET(comm) {
    int s1 = 0, s2 = 0, s3 = 0, c;
    optind = 0;
    while ((c = getopt(argc, argv, "123")) != -1) { if (c == '1') s1 = 1; else if (c == '2') s2 = 1; else if (c == '3') s3 = 1; else return 1; }
    if (argc - optind != 2) { kb_error("usage: comm [-123] file1 file2"); return 1; }
    FILE *fa = !strcmp(argv[optind], "-") ? stdin : fopen(argv[optind], "r");
    FILE *fb = !strcmp(argv[optind + 1], "-") ? stdin : fopen(argv[optind + 1], "r");
    if (!fa || !fb) { kb_error("cannot open input"); return 1; }
    char **a, **b;
    size_t na = kb_read_lines(fa, &a), nb = kb_read_lines(fb, &b), i = 0, j = 0;
    while (i < na || j < nb) {
        int cmp = i >= na ? 1 : j >= nb ? -1 : strcmp(a[i], b[j]);
        if (cmp < 0) { if (!s1) puts(a[i]); i++; }
        else if (cmp > 0) { if (!s2) printf("%s%s\n", s1 ? "" : "\t", b[j]); j++; }
        else { if (!s3) printf("%s%s%s\n", s1 ? "" : "\t", s2 ? "" : "\t", a[i]); i++; j++; }
    }
    return 0;
}

APPLET(cmp) {
    int silent = 0, list = 0, c;
    optind = 0;
    while ((c = getopt(argc, argv, "sl")) != -1) { if (c == 's') silent = 1; else if (c == 'l') list = 1; else return 2; }
    if (argc - optind < 2) { kb_error("usage: cmp [-sl] file1 file2"); return 2; }
    FILE *fa = fopen(argv[optind], "r"), *fb = fopen(argv[optind + 1], "r");
    if (!fa) { kb_error("%s: %s", argv[optind], strerror(errno)); return 2; }
    if (!fb) { kb_error("%s: %s", argv[optind + 1], strerror(errno)); return 2; }
    long byte = 1, line = 1;
    int diff = 0;
    for (;;) {
        int x = fgetc(fa), y = fgetc(fb);
        if (x == EOF && y == EOF) break;
        if (x == EOF || y == EOF) {
            if (!silent) fprintf(stderr, "cmp: EOF on %s after byte %ld\n", x == EOF ? argv[optind] : argv[optind + 1], byte - 1);
            return 1;
        }
        if (x != y) {
            diff = 1;
            if (silent) return 1;
            if (list) printf("%ld %o %o\n", byte, x, y);
            else { printf("%s %s differ: byte %ld, line %ld\n", argv[optind], argv[optind + 1], byte, line); return 1; }
        }
        if (x == '\n') line++;
        byte++;
    }
    return diff;
}

APPLET(expand) {
    int tab = 8, c;
    optind = 0;
    while ((c = getopt(argc, argv, "t:")) != -1) { if (c == 't') tab = atoi(optarg); else return 1; }
    if (tab < 1) tab = 8;
    int ch, col = 0;
    FILE *f = optind < argc ? fopen(argv[optind], "r") : stdin;
    if (!f) { kb_error("%s: %s", argv[optind], strerror(errno)); return 1; }
    while ((ch = fgetc(f)) != EOF) {
        if (ch == '\t') { do { putchar(' '); col++; } while (col % tab); }
        else { putchar(ch); col = ch == '\n' ? 0 : col + 1; }
    }
    return 0;
}
