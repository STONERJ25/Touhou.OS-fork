/* Binary, checksum and miscellaneous utilities: od hexdump xxd strings base64 md5sum sha256sum cksum crc32 expr mktemp cal
 * factor shuf split cmp-free helpers. */
#include "kappabox.h"
#include <crypto.h>
#include <regex.h>

/* ------------------------------------------------------------------ hex dumps */

static void canonical_dump(FILE *f, long long start, long long limit, int uppercase) {
    unsigned char buf[16];
    unsigned char prev[16];
    int have_prev = 0, starred = 0;
    long long off = start;
    for (;;) {
        size_t want = sizeof buf;
        if (limit >= 0 && (long long)want > limit - (off - start)) want = (size_t)(limit - (off - start));
        size_t n = want ? fread(buf, 1, want, f) : 0;
        if (n == 0) break;
        if (n == sizeof buf && have_prev && !memcmp(buf, prev, sizeof buf)) {
            if (!starred) { puts("*"); starred = 1; }
            off += (long long)n;
            continue;
        }
        starred = 0;
        printf(uppercase ? "%08llX  " : "%08llx  ", (unsigned long long)off);
        for (size_t i = 0; i < sizeof buf; i++) {
            if (i < n) printf(uppercase ? "%02X " : "%02x ", buf[i]); else printf("   ");
            if (i == 7) putchar(' ');
        }
        printf(" |");
        for (size_t i = 0; i < n; i++) putchar(buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.');
        puts("|");
        memcpy(prev, buf, n);
        have_prev = n == sizeof buf;
        off += (long long)n;
    }
    printf(uppercase ? "%08llX\n" : "%08llx\n", (unsigned long long)off);
}

APPLET(hexdump) {
    int canon = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-C")) canon = 1;
    (void)canon;                                    /* only the canonical layout is implemented; -C is the common use */
    FILE *f = stdin;
    for (int i = 1; i < argc; i++) if (argv[i][0] != '-') { f = fopen(argv[i], "r"); if (!f) { kb_error("%s: %s", argv[i], strerror(errno)); return 1; } }
    canonical_dump(f, 0, -1, 0);
    return 0;
}

APPLET(xxd) {
    int cols = 16, group = 2, plain = 0, reverse = 0, upper = 0, c;
    long long skip = 0, limit = -1;
    optind = 0;
    while ((c = getopt(argc, argv, "c:g:l:s:prui")) != -1) {
        switch (c) {
        case 'c': cols = atoi(optarg); break;
        case 'g': group = atoi(optarg); break;
        case 'l': limit = atoll(optarg); break;
        case 's': skip = atoll(optarg); break;
        case 'p': plain = 1; break;
        case 'r': reverse = 1; break;
        case 'u': upper = 1; break;
        case 'i': break;
        default: return 1;
        }
    }
    FILE *f = optind < argc ? fopen(argv[optind], "r") : stdin;
    if (!f) { kb_error("%s: %s", argv[optind], strerror(errno)); return 1; }
    if (reverse) {
        int hi = -1, ch;
        while ((ch = fgetc(f)) != EOF) {
            if (ch == ':' ) { while ((ch = fgetc(f)) != EOF && ch != ' ') { } }
            if (!isxdigit(ch)) { if (ch == '\n') hi = -1; continue; }
            int v = isdigit(ch) ? ch - '0' : (tolower(ch) - 'a' + 10);
            if (hi < 0) hi = v; else { putchar(hi << 4 | v); hi = -1; }
        }
        return 0;
    }
    if (skip) fseek(f, (long)skip, SEEK_SET);
    if (cols < 1) cols = 16;
    unsigned char *buf = malloc((size_t)cols);
    long long off = skip, done = 0;
    for (;;) {
        size_t want = (size_t)cols;
        if (limit >= 0 && (long long)want > limit - done) want = (size_t)(limit - done);
        size_t n = want ? fread(buf, 1, want, f) : 0;
        if (!n) break;
        if (plain) { for (size_t i = 0; i < n; i++) printf(upper ? "%02X" : "%02x", buf[i]); putchar('\n'); }
        else {
            printf("%08llx:", (unsigned long long)off);
            for (int i = 0; i < cols; i++) {
                if (group > 0 && i % group == 0) putchar(' ');
                if ((size_t)i < n) printf(upper ? "%02X" : "%02x", buf[i]); else printf("  ");
            }
            printf("  ");
            for (size_t i = 0; i < n; i++) putchar(buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.');
            putchar('\n');
        }
        off += (long long)n;
        done += (long long)n;
    }
    return 0;
}

typedef struct { char kind; int size; } OdFmt;

static void od_value(const OdFmt *f, const unsigned char *p) {
    unsigned long long u = 0;
    for (int i = f->size - 1; i >= 0; i--) u = u << 8 | p[i];
    long long sgn = (long long)u;
    if (f->size < 8) { unsigned long long sign = 1ull << (f->size * 8 - 1); if (u & sign) sgn = (long long)(u | ~((sign << 1) - 1)); }
    switch (f->kind) {
    case 'x': printf(" %0*llx", f->size * 2, u); break;
    case 'o': printf(" %0*llo", (f->size * 8 + 2) / 3, u); break;
    case 'u': printf(" %*llu", f->size == 1 ? 3 : f->size == 2 ? 5 : 10, u); break;
    case 'd': printf(" %*lld", f->size == 1 ? 4 : f->size == 2 ? 6 : 11, sgn); break;
    case 'c': {
        unsigned char ch = p[0];
        switch (ch) {
        case 0: printf("  \\0"); break; case '\a': printf("  \\a"); break; case '\b': printf("  \\b"); break; case '\f': printf("  \\f"); break;
        case '\n': printf("  \\n"); break; case '\r': printf("  \\r"); break; case '\t': printf("  \\t"); break; case '\v': printf("  \\v"); break;
        default: if (ch >= 32 && ch < 127) printf("   %c", ch); else printf(" %03o", ch);
        }
        break;
    }
    }
}

APPLET(od) {
    char radix = 'o';
    OdFmt fmts[8];
    int nf = 0, width = 16, c, verbose = 0;
    long long skip = 0, limit = -1;
    optind = 0;
    while ((c = getopt(argc, argv, "A:t:j:N:w:vcxobdsh")) != -1) {
        switch (c) {
        case 'A': radix = optarg[0]; break;
        case 'j': skip = atoll(optarg); break;
        case 'N': limit = atoll(optarg); break;
        case 'w': width = atoi(optarg); break;
        case 'v': verbose = 1; break;
        case 'c': fmts[nf++] = (OdFmt){ 'c', 1 }; break;
        case 'x': fmts[nf++] = (OdFmt){ 'x', 2 }; break;
        case 'o': fmts[nf++] = (OdFmt){ 'o', 2 }; break;
        case 'd': fmts[nf++] = (OdFmt){ 'u', 2 }; break;
        case 'b': fmts[nf++] = (OdFmt){ 'o', 1 }; break;
        case 's': fmts[nf++] = (OdFmt){ 'd', 2 }; break;
        case 't': {
            char k = optarg[0];
            int sz = optarg[1] ? atoi(optarg + 1) : (k == 'c' ? 1 : 4);
            if (k == 'a') { k = 'c'; sz = 1; }
            if (nf < 8) fmts[nf++] = (OdFmt){ k, sz > 8 ? 8 : sz < 1 ? 1 : sz };
            break;
        }
        default: return 1;
        }
    }
    if (!nf) fmts[nf++] = (OdFmt){ 'o', 2 };
    FILE *f = optind < argc ? fopen(argv[optind], "r") : stdin;
    if (!f) { kb_error("%s: %s", argv[optind], strerror(errno)); return 1; }
    if (skip) fseek(f, (long)skip, SEEK_SET);
    unsigned char *buf = calloc(1, (size_t)width + 8), *prev = calloc(1, (size_t)width + 8);
    long long off = skip, done = 0;
    int star = 0, have_prev = 0;
    for (;;) {
        size_t want = (size_t)width;
        if (limit >= 0 && (long long)want > limit - done) want = (size_t)(limit - done);
        memset(buf, 0, (size_t)width + 8);
        size_t n = want ? fread(buf, 1, want, f) : 0;
        if (!n) break;
        if (!verbose && have_prev && n == (size_t)width && !memcmp(buf, prev, (size_t)width)) {
            if (!star) { puts("*"); star = 1; }
            off += (long long)n; done += (long long)n;
            continue;
        }
        star = 0;
        for (int fi = 0; fi < nf; fi++) {
            if (fi == 0) { if (radix != 'n') printf(radix == 'x' ? "%07llx" : radix == 'd' ? "%07llu" : "%07llo", (unsigned long long)off); }
            else if (radix != 'n') printf("       ");
            for (size_t i = 0; i < n; i += (size_t)fmts[fi].size) od_value(&fmts[fi], buf + i);
            putchar('\n');
        }
        memcpy(prev, buf, (size_t)width);
        have_prev = n == (size_t)width;
        off += (long long)n; done += (long long)n;
    }
    if (radix != 'n') printf(radix == 'x' ? "%07llx\n" : radix == 'd' ? "%07llu\n" : "%07llo\n", (unsigned long long)off);
    return 0;
}

APPLET(strings) {
    int minlen = 4, c;
    optind = 0;
    while ((c = getopt(argc, argv, "n:a")) != -1) { if (c == 'n') minlen = atoi(optarg); else if (c != 'a') return 1; }
    FILE *f = optind < argc ? fopen(argv[optind], "r") : stdin;
    if (!f) { kb_error("%s: %s", argv[optind], strerror(errno)); return 1; }
    char run[4096];
    int n = 0, ch;
    while ((ch = fgetc(f)) != EOF) {
        if ((ch >= 32 && ch < 127) || ch == '\t') {
            if (n < (int)sizeof run - 1) run[n++] = (char)ch;
        } else {
            if (n >= minlen) { run[n] = 0; puts(run); }
            n = 0;
        }
    }
    if (n >= minlen) { run[n] = 0; puts(run); }
    return 0;
}

/* ------------------------------------------------------------------ base64 */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

APPLET(base64) {
    int decode = 0, wrap = 76, c;
    optind = 0;
    while ((c = getopt(argc, argv, "dw:i")) != -1) { if (c == 'd') decode = 1; else if (c == 'w') wrap = atoi(optarg); else if (c != 'i') return 1; }
    FILE *f = optind < argc ? fopen(argv[optind], "r") : stdin;
    if (!f) { kb_error("%s: %s", argv[optind], strerror(errno)); return 1; }
    if (decode) {
        int acc = 0, bits = 0, ch;
        while ((ch = fgetc(f)) != EOF) {
            const char *p = ch == '=' ? NULL : strchr(B64, ch);
            if (!p || !ch) continue;
            acc = acc << 6 | (int)(p - B64);
            bits += 6;
            if (bits >= 8) { bits -= 8; putchar((acc >> bits) & 0xFF); }
        }
        return 0;
    }
    unsigned char in[3];
    size_t n;
    int col = 0;
    while ((n = fread(in, 1, 3, f)) > 0) {
        char out[4];
        out[0] = B64[in[0] >> 2];
        out[1] = B64[((in[0] & 3) << 4) | (n > 1 ? in[1] >> 4 : 0)];
        out[2] = n > 1 ? B64[((in[1] & 15) << 2) | (n > 2 ? in[2] >> 6 : 0)] : '=';
        out[3] = n > 2 ? B64[in[2] & 63] : '=';
        for (int i = 0; i < 4; i++) {
            if (wrap && col == wrap) { putchar('\n'); col = 0; }
            putchar(out[i]);
            col++;
        }
    }
    if (col) putchar('\n');
    return 0;
}

/* ------------------------------------------------------------------ checksums */

typedef struct { int kind; int check; int rc; } Sum;       /* 0 md5, 1 sha256 */

static void hex_digest(int kind, FILE *f, char *out) {
    uint8_t d[32];
    char buf[4096];
    size_t n;
    int len;
    if (kind == 0) { md5_ctx c; md5_init(&c); while ((n = fread(buf, 1, sizeof buf, f)) > 0) md5_update(&c, buf, n); md5_final(&c, d); len = 16; }
    else { sha256_ctx c; sha256_init(&c); while ((n = fread(buf, 1, sizeof buf, f)) > 0) sha256_update(&c, buf, n); sha256_final(&c, d); len = 32; }
    for (int i = 0; i < len; i++) sprintf(out + i * 2, "%02x", d[i]);
}

static int sum_main(int argc, char **argv, int kind) {
    int check = 0, c;
    optind = 0;
    while ((c = getopt(argc, argv, "cb")) != -1) { if (c == 'c') check = 1; else if (c != 'b') return 1; }
    int rc = 0;
    if (check) {
        FILE *list = optind < argc ? fopen(argv[optind], "r") : stdin;
        if (!list) { kb_error("%s: %s", argv[optind], strerror(errno)); return 1; }
        char *line = NULL;
        size_t cap = 0;
        int bad = 0;
        while (getline(&line, &cap, list) >= 0) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            char *sp = strstr(line, "  ");
            if (!sp) sp = strstr(line, " *");
            if (!sp) continue;
            *sp = 0;
            const char *name = sp + 2;
            FILE *f = fopen(name, "r");
            if (!f) { printf("%s: FAILED open or read\n", name); bad++; continue; }
            char hex[65];
            hex_digest(kind, f, hex);
            fclose(f);
            int ok = !strcasecmp(hex, line);
            printf("%s: %s\n", name, ok ? "OK" : "FAILED");
            if (!ok) bad++;
        }
        free(line);
        if (bad) fprintf(stderr, "%s: WARNING: %d computed checksum%s did NOT match\n", kb_name, bad, bad == 1 ? "" : "s");
        return bad ? 1 : 0;
    }
    char hex[65];
    if (optind >= argc) { hex_digest(kind, stdin, hex); printf("%s  -\n", hex); return 0; }
    for (int i = optind; i < argc; i++) {
        FILE *f = !strcmp(argv[i], "-") ? stdin : fopen(argv[i], "r");
        if (!f) { kb_error("%s: %s", argv[i], strerror(errno)); rc = 1; continue; }
        hex_digest(kind, f, hex);
        printf("%s  %s\n", hex, argv[i]);
        if (f != stdin) fclose(f);
    }
    return rc;
}

APPLET(md5sum) { return sum_main(argc, argv, 0); }
APPLET(sha256sum) { return sum_main(argc, argv, 1); }

APPLET(crc32) {
    int rc = 0;
    for (int i = argc > 1 ? 1 : 0; i < (argc > 1 ? argc : 1); i++) {
        FILE *f = argc > 1 ? fopen(argv[i], "r") : stdin;
        if (!f) { kb_error("%s: %s", argv[i], strerror(errno)); rc = 1; continue; }
        uint32_t crc = 0;
        char buf[4096];
        size_t n, total = 0;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) { crc = crc32_update(crc, buf, n); total += n; }
        if (argc > 1) printf("%08x %zu %s\n", crc, total, argv[i]); else printf("%08x %zu\n", crc, total);
    }
    return rc;
}

/* ------------------------------------------------------------------ expr */

typedef struct { char **v; int i, n; } ExprArgs;

static const char *e_peek(ExprArgs *a) { return a->i < a->n ? a->v[a->i] : NULL; }
static int is_number(const char *s) { char *end; if (!*s) return 0; strtoll(s, &end, 10); return !*end; }

static char *e_dup(const char *s) { char *d = strdup(s); if (!d) kb_die("out of memory"); return d; }
static char *e_int(long long v) { char buf[32]; snprintf(buf, sizeof buf, "%lld", v); return e_dup(buf); }
static int e_true(const char *s) { return s && *s && !(is_number(s) && strtoll(s, NULL, 10) == 0); }

static char *e_or(ExprArgs *a);

static char *e_primary(ExprArgs *a) {
    const char *t = e_peek(a);
    if (!t) kb_die("syntax error: missing argument");
    a->i++;
    if (!strcmp(t, "(")) {
        char *v = e_or(a);
        if (!e_peek(a) || strcmp(e_peek(a), ")")) kb_die("syntax error: expecting ')'");
        a->i++;
        return v;
    }
    if (!strcmp(t, "length") && a->i < a->n) { char *s = e_dup(a->v[a->i++]); char *r = e_int((long long)strlen(s)); free(s); return r; }
    if (!strcmp(t, "substr") && a->i + 2 < a->n) {
        const char *s = a->v[a->i++];
        long pos = atol(a->v[a->i++]), len = atol(a->v[a->i++]);
        if (pos < 1 || len < 1 || (size_t)pos > strlen(s)) return e_dup("");
        return strndup(s + pos - 1, (size_t)len);
    }
    if (!strcmp(t, "index") && a->i + 1 < a->n) {
        const char *s = a->v[a->i++], *chars = a->v[a->i++];
        size_t k = strcspn(s, chars);
        return e_int(s[k] ? (long long)k + 1 : 0);
    }
    return e_dup(t);
}

static char *e_match(ExprArgs *a) {
    char *l = e_primary(a);
    while (e_peek(a) && !strcmp(e_peek(a), ":")) {
        a->i++;
        char *pat = e_primary(a);
        regex_t re;
        regmatch_t m[2];
        if (regcomp(&re, pat, 0)) kb_die("invalid regular expression");
        char *r;
        if (regexec(&re, l, 2, m, 0) == 0 && m[0].rm_so == 0) {
            if (re.re_nsub > 0 && m[1].rm_so >= 0) r = strndup(l + m[1].rm_so, (size_t)(m[1].rm_eo - m[1].rm_so));
            else r = e_int(m[0].rm_eo);
        } else r = re.re_nsub > 0 ? e_dup("") : e_int(0);
        regfree(&re);
        free(l); free(pat);
        l = r;
    }
    return l;
}

static char *e_mul(ExprArgs *a) {
    char *l = e_match(a);
    while (e_peek(a) && (!strcmp(e_peek(a), "*") || !strcmp(e_peek(a), "/") || !strcmp(e_peek(a), "%"))) {
        char op = e_peek(a)[0];
        a->i++;
        char *r = e_match(a);
        if (!is_number(l) || !is_number(r)) kb_die("non-integer argument");
        long long x = strtoll(l, NULL, 10), y = strtoll(r, NULL, 10);
        if (op != '*' && y == 0) kb_die("division by zero");
        free(l); free(r);
        l = e_int(op == '*' ? x * y : op == '/' ? x / y : x % y);
    }
    return l;
}

static char *e_add(ExprArgs *a) {
    char *l = e_mul(a);
    while (e_peek(a) && (!strcmp(e_peek(a), "+") || !strcmp(e_peek(a), "-"))) {
        char op = e_peek(a)[0];
        a->i++;
        char *r = e_mul(a);
        if (!is_number(l) || !is_number(r)) kb_die("non-integer argument");
        long long x = strtoll(l, NULL, 10), y = strtoll(r, NULL, 10);
        free(l); free(r);
        l = e_int(op == '+' ? x + y : x - y);
    }
    return l;
}

static char *e_cmp(ExprArgs *a) {
    char *l = e_add(a);
    const char *ops[] = { "=", "==", "!=", "<", "<=", ">", ">=" };
    for (;;) {
        const char *t = e_peek(a);
        int which = -1;
        for (int k = 0; t && k < 7; k++) if (!strcmp(t, ops[k])) which = k;
        if (which < 0) break;
        a->i++;
        char *r = e_add(a);
        int c = is_number(l) && is_number(r) ? (strtoll(l, NULL, 10) < strtoll(r, NULL, 10) ? -1 : strtoll(l, NULL, 10) > strtoll(r, NULL, 10)) : strcmp(l, r);
        int res = which <= 1 ? c == 0 : which == 2 ? c != 0 : which == 3 ? c < 0 : which == 4 ? c <= 0 : which == 5 ? c > 0 : c >= 0;
        free(l); free(r);
        l = e_int(res);
    }
    return l;
}

static char *e_and(ExprArgs *a) {
    char *l = e_cmp(a);
    while (e_peek(a) && !strcmp(e_peek(a), "&")) {
        a->i++;
        char *r = e_cmp(a);
        if (e_true(l) && e_true(r)) free(r); else { free(l); free(r); l = e_int(0); }
    }
    return l;
}

static char *e_or(ExprArgs *a) {
    char *l = e_and(a);
    while (e_peek(a) && !strcmp(e_peek(a), "|")) {
        a->i++;
        char *r = e_and(a);
        if (e_true(l)) free(r); else { free(l); l = r; }
    }
    return l;
}

APPLET(expr) {
    ExprArgs a = { argv + 1, 0, argc - 1 };
    if (a.n < 1) kb_die("missing operand");
    char *v = e_or(&a);
    if (a.i < a.n) kb_die("syntax error");
    puts(v);
    return e_true(v) ? 0 : 1;
}

/* ------------------------------------------------------------------ small things */

APPLET(mktemp) {
    int dir = 0, i = 1;
    const char *tmpl = "/tmp/tmp.XXXXXXXXXX";
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-d")) dir = 1;
        else if (argv[i][0] != '-') tmpl = argv[i];
    }
    char path[PATH_MAX];
    strlcpy(path, tmpl, sizeof path);
    size_t len = strlen(path), x = len;
    while (x > 0 && path[x - 1] == 'X') x--;
    static const char alpha[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (int attempt = 0; attempt < 100; attempt++) {
        unsigned char r[32];
        gap_getrandom(r, sizeof r);
        for (size_t k = x; k < len; k++) path[k] = alpha[r[k % 32] % 36];
        int rc = dir ? mkdir(path, 0700) : open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (rc >= 0) { if (!dir) close(rc); puts(path); return 0; }
        if (errno != EEXIST) break;
    }
    kb_error("failed to create %s via template '%s': %s", dir ? "directory" : "file", tmpl, strerror(errno));
    return 1;
}

APPLET(factor) {
    for (int i = 1; i < argc; i++) {
        unsigned long long n = strtoull(argv[i], NULL, 10), m = n;
        printf("%llu:", n);
        for (unsigned long long p = 2; p * p <= m; p++) while (m % p == 0) { printf(" %llu", p); m /= p; }
        if (m > 1 || n < 2) printf(" %llu", m);
        putchar('\n');
    }
    return 0;
}

APPLET(shuf) {
    char **lines;
    size_t n = kb_read_lines(argc > 1 && argv[1][0] != '-' ? fopen(argv[1], "r") : stdin, &lines);
    for (size_t i = n; i > 1; i--) {
        unsigned r;
        gap_getrandom(&r, sizeof r);
        size_t j = r % i;
        char *t = lines[i - 1]; lines[i - 1] = lines[j]; lines[j] = t;
    }
    for (size_t i = 0; i < n; i++) puts(lines[i]);
    return 0;
}

static int days_in_month(int y, int m) {
    static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return d[m - 1] + (m == 2 && leap);
}

static int weekday(int y, int m, int d) {             /* 0 = Sunday (Sakamoto) */
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static void cal_month(int y, int m, int today, int hilite) {
    static const char *const names[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
    char title[32];
    snprintf(title, sizeof title, "%s %d", names[m - 1], y);
    printf("%*s%s\n", (20 - (int)strlen(title)) / 2, "", title);
    puts("Su Mo Tu We Th Fr Sa");
    int wd = weekday(y, m, 1);
    for (int i = 0; i < wd; i++) printf("   ");
    for (int d = 1; d <= days_in_month(y, m); d++) {
        if (hilite && d == today) printf("\033[7m%2d\033[0m", d); else printf("%2d", d);
        if ((wd + d) % 7 == 0 || d == days_in_month(y, m)) putchar('\n'); else putchar(' ');
    }
}

APPLET(cal) {
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    int y = tm.tm_year + 1900, m = tm.tm_mon + 1;
    if (argc == 2) { y = atoi(argv[1]); if (y < 1) y = tm.tm_year + 1900; else { for (m = 1; m <= 12; m++) { cal_month(y, m, 0, 0); putchar('\n'); } return 0; } }
    if (argc >= 3) { m = atoi(argv[1]); y = atoi(argv[2]); }
    if (m < 1 || m > 12) { kb_error("illegal month value: use 1-12"); return 1; }
    cal_month(y, m, tm.tm_mday, kb_color && y == tm.tm_year + 1900 && m == tm.tm_mon + 1);
    return 0;
}
