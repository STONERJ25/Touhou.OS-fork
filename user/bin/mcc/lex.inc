/* lex.inc -- diagnostics and the tokenizer. */

static File *current_file;
static File **input_files;
static int n_input_files;
static bool at_bol;
static bool has_space;
static int error_count;

File **get_input_files(void) {
    return input_files;
}

File *new_file(char *name, int file_no, char *contents) {
    File *file = xcalloc(sizeof(File));
    file->name = name;
    file->display_name = name;
    file->file_no = file_no;
    file->contents = contents;
    file->inc_idx = -1;
    return file;
}

/* ---- diagnostics.  The plain "file:line: error: text" part stays conventional; Marisa only adds a remark. */

static const char *marisa_remark(const char *msg, bool is_warning) {
    if (is_warning) return "Marisa: it will run, ze, but I wouldn't trust it.";
    if (strstr(msg, "undeclared") || strstr(msg, "not declared")) return "Marisa: I never borrowed that one, ze. Did you declare it?";
    if (strstr(msg, "expected")) return "Marisa: something is missing around here, ze.";
    if (strstr(msg, "incompatible") || strstr(msg, "invalid operands")) return "Marisa: those two don't mix, ze (not even with mushrooms).";
    if (strstr(msg, "redefinition") || strstr(msg, "redeclaration")) return "Marisa: two books with the same title, ze.";
    if (strstr(msg, "unclosed")) return "Marisa: you left something open, ze.";
    if (strstr(msg, "not supported")) return "Marisa: that spell is not in my book yet, ze.";
    return "Marisa: the Master Spark went wide, ze.";
}

static void print_diag(const char *kind, char *filename, char *input, int line_no, char *loc, const char *msg, bool is_warning) {
    /* find the line that contains loc */
    char *line = loc;
    while (input < line && line[-1] != '\n') line--;
    char *end = loc;
    while (*end && *end != '\n') end++;
    fprintf(stderr, "%s:%d: %s: %s\n", filename, line_no, kind, msg);
    int indent = (int)(loc - line);
    fprintf(stderr, "    %.*s\n", (int)(end - line), line);
    fprintf(stderr, "    ");
    for (int i = 0; i < indent; i++) fputc(line[i] == '\t' ? '\t' : ' ', stderr);
    fprintf(stderr, "^ %s\n", marisa_remark(msg, is_warning));
}

static int line_of(File *file, char *loc) {
    int n = 1;
    for (char *p = file->contents; p < loc && *p; p++)
        if (*p == '\n') n++;
    return n;
}

static void verror_at(char *filename, char *input, int line_no, char *loc, const char *fmt, va_list ap, bool is_warning) {
    char *msg = xvformat(fmt, ap);
    print_diag(is_warning ? "warning" : "error", filename, input, line_no, loc, msg, is_warning);
}

void error_at(char *loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    verror_at(current_file->display_name, current_file->contents, line_of(current_file, loc) + current_file->line_delta, loc, fmt, ap, false);
    va_end(ap);
    error_count++;
    fprintf(stderr, "mcc: 1 error -- nothing was built, ze.\n");
    exit(1);
}

void error_tok(Token *tok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    verror_at(tok->filename, tok->file->contents, tok->line_no + tok->line_delta, tok->loc, fmt, ap, false);
    va_end(ap);
    error_count++;
    fprintf(stderr, "mcc: 1 error -- nothing was built, ze.\n");
    exit(1);
}

void warn_tok(Token *tok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    verror_at(tok->filename, tok->file->contents, tok->line_no + tok->line_delta, tok->loc, fmt, ap, true);
    va_end(ap);
}

/* ---- token helpers */

bool equal(Token *tok, const char *op) {
    int n = tok->len;
    for (int i = 0; i < n; i++)
        if (op[i] != tok->loc[i]) return false; /* a shorter op ends with NUL, which never matches */
    return op[n] == '\0';
}

Token *skip(Token *tok, const char *op) {
    if (!equal(tok, op)) error_tok(tok, "expected '%s'", op);
    return tok->next;
}

bool consume(Token **rest, Token *tok, const char *str) {
    if (equal(tok, str)) {
        *rest = tok->next;
        return true;
    }
    *rest = tok;
    return false;
}

static Token *new_token(TokenKind kind, char *start, char *end) {
    Token *tok = xcalloc(sizeof(Token));
    tok->kind = kind;
    tok->loc = start;
    tok->len = (int)(end - start);
    tok->file = current_file;
    tok->filename = current_file->display_name;
    tok->at_bol = at_bol;
    tok->has_space = has_space;
    at_bol = has_space = false;
    return tok;
}

static bool startswith(const char *p, const char *q) {
    return strncmp(p, q, strlen(q)) == 0;
}

static bool is_ident1(char c) {
    return isalpha((u8)c) || c == '_' || c == '$' || (u8)c >= 0x80;
}

static bool is_ident2(char c) {
    return is_ident1(c) || isdigit((u8)c);
}

static int from_hex(char c) {
    if ('0' <= c && c <= '9') return c - '0';
    if ('a' <= c && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

static int read_punct(char *p) {
    static const char *kw[] = {
        "<<=", ">>=", "...", "==", "!=", "<=", ">=", "->", "+=", "-=", "*=", "/=", "++", "--", "%=", "&=", "|=", "^=", "&&", "||", "<<", ">>", "##",
    };
    for (size_t i = 0; i < sizeof(kw) / sizeof(*kw); i++)
        if (startswith(p, kw[i])) return (int)strlen(kw[i]);
    return ispunct((u8)*p) ? 1 : 0;
}

/* ---- UTF-8 */

static int encode_utf8(char *buf, u32 c) {
    if (c <= 0x7F) {
        buf[0] = (char)c;
        return 1;
    }
    if (c <= 0x7FF) {
        buf[0] = (char)(0xC0 | (c >> 6));
        buf[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c <= 0xFFFF) {
        buf[0] = (char)(0xE0 | (c >> 12));
        buf[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    buf[0] = (char)(0xF0 | (c >> 18));
    buf[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

static u32 decode_utf8(char **new_pos, char *p) {
    u8 c = (u8)*p;
    if (c < 128) {
        *new_pos = p + 1;
        return c;
    }
    int len;
    u32 cp;
    if (c >= 0xF0) {
        len = 4;
        cp = c & 7;
    } else if (c >= 0xE0) {
        len = 3;
        cp = c & 15;
    } else if (c >= 0xC0) {
        len = 2;
        cp = c & 31;
    } else {
        *new_pos = p + 1;
        return c;
    }
    for (int i = 1; i < len; i++) {
        if (((u8)p[i] & 0xC0) != 0x80) {
            *new_pos = p + 1;
            return c;
        }
        cp = (cp << 6) | ((u8)p[i] & 0x3F);
    }
    *new_pos = p + len;
    return cp;
}

/* ---- literals */

static int read_escaped_char(char **new_pos, char *p) {
    if ('0' <= *p && *p <= '7') {
        int c = *p++ - '0';
        if ('0' <= *p && *p <= '7') {
            c = (c << 3) + (*p++ - '0');
            if ('0' <= *p && *p <= '7') c = (c << 3) + (*p++ - '0');
        }
        *new_pos = p;
        return c;
    }
    if (*p == 'x') {
        p++;
        if (!isxdigit((u8)*p)) error_at(p, "invalid hex escape sequence");
        int c = 0;
        for (; isxdigit((u8)*p); p++) c = (c << 4) + from_hex(*p);
        *new_pos = p;
        return c;
    }
    *new_pos = p + 1;
    switch (*p) {
    case 'a': return 7;
    case 'b': return 8;
    case 't': return 9;
    case 'n': return 10;
    case 'v': return 11;
    case 'f': return 12;
    case 'r': return 13;
    case 'e': return 27;
    default: return (u8)*p;
    }
}

static u32 read_ucn(char **new_pos, char *p) {
    int n = (*p == 'u') ? 4 : 8;
    p++;
    u32 c = 0;
    for (int i = 0; i < n; i++) {
        if (!isxdigit((u8)*p)) error_at(p, "invalid universal character name");
        c = (c << 4) + (u32)from_hex(*p++);
    }
    *new_pos = p;
    return c;
}

static char *string_literal_end(char *p) {
    char *start = p;
    for (; *p != '"'; p++) {
        if (*p == '\n' || *p == '\0') error_at(start, "unclosed string literal");
        if (*p == '\\') p++;
    }
    return p;
}

static Token *read_string_literal(char *start, char *quote) {
    char *end = string_literal_end(quote + 1);
    char *buf = xcalloc((size_t)(end - quote) + 1);
    int len = 0;
    for (char *p = quote + 1; p < end;) {
        if (*p == '\\') {
            if (p[1] == 'u' || p[1] == 'U') {
                u32 c = read_ucn(&p, p + 1);
                len += encode_utf8(buf + len, c);
            } else {
                buf[len++] = (char)read_escaped_char(&p, p + 1);
            }
        } else {
            buf[len++] = *p++;
        }
    }
    Token *tok = new_token(TK_STR, start, end + 1);
    tok->ty = array_of(ty_char, len + 1);
    tok->str = buf;
    return tok;
}

/* L"..." (wchar_t = int), u"..." (UTF-16) and U"..." (UTF-32): the string is stored as an array of the element type */
static Token *read_wide_string_literal(char *start, char *quote, Type *elemty) {
    char *end = string_literal_end(quote + 1);
    u32 *cps = xcalloc(((size_t)(end - quote) + 1) * sizeof(u32));
    int n = 0;
    for (char *p = quote + 1; p < end;) {
        if (*p == '\\') {
            if (p[1] == 'u' || p[1] == 'U') cps[n++] = read_ucn(&p, p + 1);
            else cps[n++] = (u32)read_escaped_char(&p, p + 1);
        } else {
            cps[n++] = decode_utf8(&p, p);
        }
    }
    Token *tok = new_token(TK_STR, start, end + 1);
    int esz = elemty->size;
    char *buf = xcalloc((size_t)(2 * n + 1) * (size_t)esz);
    int len = 0;
    for (int i = 0; i < n; i++) {
        u32 c = cps[i];
        if (esz == 2 && c >= 0x10000) {
            c -= 0x10000;
            u32 hi = 0xD800 + (c >> 10), lo = 0xDC00 + (c & 0x3FF);
            buf[len * 2] = (char)hi;
            buf[len * 2 + 1] = (char)(hi >> 8);
            len++;
            buf[len * 2] = (char)lo;
            buf[len * 2 + 1] = (char)(lo >> 8);
            len++;
        } else {
            for (int b = 0; b < esz; b++) buf[len * esz + b] = (char)(c >> (8 * b));
            len++;
        }
    }
    tok->ty = array_of(elemty, len + 1);
    tok->str = buf;
    return tok;
}

static Token *read_char_literal(char *start, char *quote, Type *ty) {
    char *p = quote + 1;
    if (*p == '\0') error_at(start, "unclosed char literal");
    i64 val = 0;
    int nchars = 0;
    bool wide = ty != ty_int || start != quote;
    while (*p != '\'') {
        if (*p == '\n' || *p == '\0') error_at(start, "unclosed char literal");
        i64 c;
        if (*p == '\\') {
            if (p[1] == 'u' || p[1] == 'U') c = read_ucn(&p, p + 1);
            else c = read_escaped_char(&p, p + 1);
        } else if (wide) {
            c = decode_utf8(&p, p);
        } else {
            c = (u8)*p++;
        }
        val = nchars == 0 ? c : ((val << 8) | (c & 0xff));
        nchars++;
    }
    if (nchars == 0) error_at(start, "empty character constant");
    char *end = p + 1;
    Token *tok = new_token(TK_NUM, start, end);
    if (start == quote && nchars == 1) {
        tok->val = (char)val; /* plain char literals are signed */
    } else if (start == quote) {
        tok->val = (int)val;
    } else {
        tok->val = val;
    }
    tok->ty = ty;
    return tok;
}

static bool convert_pp_int(Token *tok) {
    char *p = tok->loc;
    int base = 10;
    if (!strncasecmp(p, "0x", 2) && isxdigit((u8)p[2])) {
        p += 2;
        base = 16;
    } else if (!strncasecmp(p, "0b", 2) && (p[2] == '0' || p[2] == '1')) {
        p += 2;
        base = 2;
    } else if (*p == '0') {
        base = 8;
    }
    u64 val = 0;
    char *q = p;
    for (;; q++) {
        int d;
        if (*q >= '0' && *q <= '9') d = *q - '0';
        else if (base == 16 && isxdigit((u8)*q)) d = from_hex(*q);
        else break;
        if (d >= base) return false;
        val = val * (u64)base + (u64)d;
    }
    if (q == p && base != 8) return false;
    bool l = false, u = false;
    if (startswith(q, "LLU") || startswith(q, "LLu") || startswith(q, "llU") || startswith(q, "llu")) {
        q += 3;
        l = u = true;
    } else if (startswith(q, "ULL") || startswith(q, "Ull") || startswith(q, "uLL") || startswith(q, "ull")) {
        q += 3;
        l = u = true;
    } else if (!strncasecmp(q, "lu", 2) || !strncasecmp(q, "ul", 2)) {
        q += 2;
        l = u = true;
    } else if (startswith(q, "LL") || startswith(q, "ll")) {
        q += 2;
        l = true;
    } else if (*q == 'L' || *q == 'l') {
        q++;
        l = true;
    } else if (*q == 'U' || *q == 'u') {
        q++;
        u = true;
    }
    if (q != tok->loc + tok->len) return false;
    Type *ty;
    if (base == 10) {
        if (l && u) ty = ty_ulong;
        else if (l) ty = (val >> 63) ? ty_ulong : ty_long;
        else if (u) ty = (val >> 32) ? ty_ulong : ty_uint;
        else ty = (val >> 31) ? ((val >> 63) ? ty_ulong : ty_long) : ty_int;
    } else {
        if (l && u) ty = ty_ulong;
        else if (l) ty = (val >> 63) ? ty_ulong : ty_long;
        else if (u) ty = (val >> 32) ? ty_ulong : ty_uint;
        else if (val >> 63) ty = ty_ulong;
        else if (val >> 32) ty = ty_long;
        else if (val >> 31) ty = ty_uint;
        else ty = ty_int;
    }
    tok->kind = TK_NUM;
    tok->val = (i64)val;
    tok->ty = ty;
    return true;
}

/* A small strtod that rounds correctly in the common cases and behaves the same in every build of mcc. */
static double mcc_pow10(int n) {
    static const double tab[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                                 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    if (n <= 22) return tab[n];
    double r = 1e22;
    n -= 22;
    while (n > 0) {
        r *= 10.0;
        n--;
    }
    return r;
}

static double mcc_strtod(const char *s, char **end) {
    const char *p = s;
    double result = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        double m = 0;
        int exp2 = 0;
        bool any = false;
        for (; isxdigit((u8)*p); p++) {
            m = m * 16 + from_hex(*p);
            any = true;
        }
        if (*p == '.') {
            p++;
            for (; isxdigit((u8)*p); p++) {
                m = m * 16 + from_hex(*p);
                exp2 -= 4;
                any = true;
            }
        }
        if (!any) {
            *end = (char *)s;
            return 0;
        }
        if (*p == 'p' || *p == 'P') {
            const char *q = p + 1;
            int sign = 1, e = 0;
            if (*q == '+' || *q == '-') sign = (*q++ == '-') ? -1 : 1;
            if (isdigit((u8)*q)) {
                for (; isdigit((u8)*q); q++) e = e * 10 + (*q - '0');
                exp2 += sign * e;
                p = q;
            }
        }
        while (exp2 > 0) {
            m *= 2.0;
            exp2--;
        }
        while (exp2 < 0) {
            m /= 2.0;
            exp2++;
        }
        *end = (char *)p;
        return m;
    }
    u64 mant = 0;
    int ndig = 0, dropped = 0, exp10 = 0;
    bool any = false;
    for (; isdigit((u8)*p); p++) {
        any = true;
        if (ndig < 19) {
            if (mant || *p != '0') ndig++;
            mant = mant * 10 + (u64)(*p - '0');
        } else {
            dropped++;
        }
    }
    if (*p == '.') {
        p++;
        for (; isdigit((u8)*p); p++) {
            any = true;
            if (ndig < 19) {
                if (mant || *p != '0') ndig++;
                mant = mant * 10 + (u64)(*p - '0');
                exp10--;
            }
        }
    }
    if (!any) {
        *end = (char *)s;
        return 0;
    }
    exp10 += dropped;
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int sign = 1, e = 0;
        if (*q == '+' || *q == '-') sign = (*q++ == '-') ? -1 : 1;
        if (isdigit((u8)*q)) {
            for (; isdigit((u8)*q); q++)
                if (e < 100000) e = e * 10 + (*q - '0');
            exp10 += sign * e;
            p = q;
        }
    }
    *end = (char *)p;
    if (mant == 0) return 0;
    result = (double)mant;
    if (exp10 > 0) {
        if (mant <= (1ULL << 53) && exp10 <= 22) result *= mcc_pow10(exp10);
        else {
            while (exp10 > 22) {
                result *= 1e22;
                exp10 -= 22;
                if (result > 1.7e308) return result * 2.0;
            }
            result *= mcc_pow10(exp10);
        }
    } else if (exp10 < 0) {
        int e = -exp10;
        if (mant <= (1ULL << 53) && e <= 22) result /= mcc_pow10(e);
        else {
            while (e > 22) {
                result /= 1e22;
                e -= 22;
                if (result == 0) break;
            }
            result /= mcc_pow10(e);
        }
    }
    return result;
}

static void convert_pp_number(Token *tok) {
    if (convert_pp_int(tok)) return;
    char *end;
    double val = mcc_strtod(tok->loc, &end);
    Type *ty;
    if (*end == 'f' || *end == 'F') {
        ty = ty_float;
        end++;
    } else if (*end == 'l' || *end == 'L') {
        ty = ty_double; /* long double is double */
        end++;
    } else {
        ty = ty_double;
    }
    if (end != tok->loc + tok->len) error_tok(tok, "invalid numeric constant");
    tok->kind = TK_NUM;
    tok->fval = ty == ty_float ? (double)(float)val : val;
    tok->ty = ty;
}

static bool is_keyword(Token *tok) {
    static Map map;
    if (map.capacity == 0) {
        static const char *kw[] = {
            "return", "if", "else", "for", "while", "int", "sizeof", "char", "struct", "union", "short", "long", "void",
            "typedef", "_Bool", "enum", "static", "goto", "break", "continue", "switch", "case", "default", "extern",
            "_Alignof", "_Alignas", "do", "signed", "unsigned", "const", "volatile", "auto", "register", "restrict",
            "__restrict", "__restrict__", "_Noreturn", "float", "double", "typeof", "asm", "_Thread_local", "__thread",
            "_Atomic", "__attribute__", "_Generic", "_Static_assert", "inline", "__inline", "__inline__", "__const",
            "__const__", "__volatile", "__volatile__", "__signed", "__signed__", "__asm", "__asm__", "__typeof",
            "__typeof__", "__extension__", "__attribute", "__alignof", "__alignof__", "_Complex", "__label__",
        };
        for (size_t i = 0; i < sizeof(kw) / sizeof(*kw); i++) map_put(&map, kw[i], (void *)kw[i]);
    }
    return map_get2(&map, tok->loc, tok->len) != NULL;
}

/* maps the alternative spellings (__inline__, __const, ...) to one canonical keyword */
static void canonicalize_keyword(Token *t) {
    struct {
        const char *from, *to;
    } alt[] = {
        {"__restrict", "restrict"}, {"__restrict__", "restrict"}, {"__inline", "inline"}, {"__inline__", "inline"},
        {"__const", "const"}, {"__const__", "const"}, {"__volatile", "volatile"}, {"__volatile__", "volatile"},
        {"__signed", "signed"}, {"__signed__", "signed"}, {"__asm", "asm"}, {"__asm__", "asm"},
        {"__typeof", "typeof"}, {"__typeof__", "typeof"}, {"__attribute", "__attribute__"},
        {"__alignof", "_Alignof"}, {"__alignof__", "_Alignof"}, {"__thread", "_Thread_local"},
    };
    for (size_t i = 0; i < sizeof(alt) / sizeof(*alt); i++) {
        if (equal(t, alt[i].from)) {
            t->loc = (char *)alt[i].to;
            t->len = (int)strlen(alt[i].to);
            return;
        }
    }
}

void convert_pp_tokens(Token *tok) {
    for (Token *t = tok; t->kind != TK_EOF; t = t->next) {
        if (is_keyword(t) && t->kind == TK_IDENT) {
            t->kind = TK_KEYWORD;
            canonicalize_keyword(t);
        } else if (t->kind == TK_PP_NUM) {
            convert_pp_number(t);
        }
    }
}

static void add_line_numbers(Token *tok) {
    char *p = current_file->contents;
    int n = 1;
    do {
        if (p == tok->loc) {
            tok->line_no = n + current_file->line_delta;
            tok = tok->next;
        }
        if (*p == '\n') n++;
    } while (*p++);
}

/* Re-reads a narrow string literal token as a wide one (used when "a" L"b" are concatenated). */
Token *tokenize_string_literal(Token *tok, Type *basety) {
    char *q = tok->loc;
    while (*q != '"') q++;
    File *saved = current_file;
    current_file = tok->file;
    Token *t = read_wide_string_literal(tok->loc, q, basety);
    current_file = saved;
    t->filename = tok->filename;
    t->line_no = tok->line_no;
    t->line_delta = tok->line_delta;
    t->next = tok->next;
    return t;
}

static int read_ident(char *start) {
    char *p = start;
    if (!is_ident1(*p)) return 0;
    p++;
    while (is_ident2(*p)) p++;
    return (int)(p - start);
}

Token *tokenize(File *file) {
    current_file = file;
    char *p = file->contents;
    Token head;
    memset(&head, 0, sizeof head);
    Token *cur = &head;
    at_bol = true;
    has_space = false;

    while (*p) {
        if (startswith(p, "//")) {
            p += 2;
            while (*p && *p != '\n') p++;
            has_space = true;
            continue;
        }
        if (startswith(p, "/*")) {
            char *q = strstr(p + 2, "*/");
            if (!q) error_at(p, "unclosed block comment");
            p = q + 2;
            has_space = true;
            continue;
        }
        if (*p == '\n') {
            p++;
            at_bol = true;
            has_space = false;
            continue;
        }
        if (isspace((u8)*p)) {
            p++;
            has_space = true;
            continue;
        }
        if (isdigit((u8)*p) || (*p == '.' && isdigit((u8)p[1]))) {
            char *q = p++;
            for (;;) {
                if (p[0] && p[1] && strchr("eEpP", p[0]) && strchr("+-", p[1])) p += 2;
                else if (isalnum((u8)*p) || *p == '.' || *p == '_') p++;
                else break;
            }
            cur = cur->next = new_token(TK_PP_NUM, q, p);
            continue;
        }
        if (*p == '"') {
            cur = cur->next = read_string_literal(p, p);
            p += cur->len;
            continue;
        }
        if (startswith(p, "u8\"")) {
            cur = cur->next = read_string_literal(p, p + 2);
            p += cur->len;
            continue;
        }
        if (startswith(p, "u\"")) {
            cur = cur->next = read_wide_string_literal(p, p + 1, ty_ushort);
            p += cur->len;
            continue;
        }
        if (startswith(p, "L\"")) {
            cur = cur->next = read_wide_string_literal(p, p + 1, ty_int);
            p += cur->len;
            continue;
        }
        if (startswith(p, "U\"")) {
            cur = cur->next = read_wide_string_literal(p, p + 1, ty_uint);
            p += cur->len;
            continue;
        }
        if (*p == '\'') {
            cur = cur->next = read_char_literal(p, p, ty_int);
            p += cur->len;
            continue;
        }
        if (startswith(p, "u'")) {
            cur = cur->next = read_char_literal(p, p + 1, ty_ushort);
            cur->val &= 0xffff;
            p += cur->len;
            continue;
        }
        if (startswith(p, "L'")) {
            cur = cur->next = read_char_literal(p, p + 1, ty_int);
            p += cur->len;
            continue;
        }
        if (startswith(p, "U'")) {
            cur = cur->next = read_char_literal(p, p + 1, ty_uint);
            p += cur->len;
            continue;
        }
        int ident_len = read_ident(p);
        if (ident_len) {
            cur = cur->next = new_token(TK_IDENT, p, p + ident_len);
            p += cur->len;
            continue;
        }
        int punct_len = read_punct(p);
        if (punct_len) {
            cur = cur->next = new_token(TK_PUNCT, p, p + punct_len);
            p += cur->len;
            continue;
        }
        error_at(p, "invalid token");
    }
    cur = cur->next = new_token(TK_EOF, p, p);
    add_line_numbers(head.next);
    return head.next;
}

/* ---- reading files */

static void remove_backslash_newline(char *p) {
    int i = 0, j = 0;
    int n = 0; /* the number of newlines we have removed: they are put back after the next real newline so line numbers stay right */
    while (p[i]) {
        if (p[i] == '\\' && p[i + 1] == '\n') {
            i += 2;
            n++;
        } else if (p[i] == '\n') {
            p[j++] = p[i++];
            for (; n > 0; n--) p[j++] = '\n';
        } else {
            p[j++] = p[i++];
        }
    }
    for (; n > 0; n--) p[j++] = '\n';
    p[j] = 0;
}

static char *canonicalize_text(u8 *buf, size_t len) {
    /* drop \r, make sure the text ends with a newline, skip a UTF-8 BOM */
    char *out = xmalloc(len + 2);
    size_t j = 0, i = 0;
    if (len >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) i = 3;
    for (; i < len; i++)
        if (buf[i] != '\r') out[j++] = (char)buf[i];
    if (j == 0 || out[j - 1] != '\n') out[j++] = '\n';
    out[j] = 0;
    remove_backslash_newline(out);
    return out;
}

static u8 *read_stdin_all(size_t *len) {
    size_t cap = 65536, n = 0;
    u8 *buf = xmalloc(cap + 1);
    for (;;) {
        size_t r = fread(buf + n, 1, cap - n, stdin);
        n += r;
        if (n < cap) break;
        cap *= 2;
        buf = xrealloc(buf, cap + 1);
    }
    buf[n] = 0;
    *len = n;
    return buf;
}

static File *load_file(char *path) {
    size_t len;
    u8 *raw = str_eq(path, "-") ? read_stdin_all(&len) : read_whole_file(path, &len);
    if (!raw) return NULL;
    char *text = canonicalize_text(raw, len);
    free(raw);
    File *file = new_file(path, n_input_files + 1, text);
    input_files = xrealloc(input_files, (size_t)(n_input_files + 2) * sizeof(File *));
    input_files[n_input_files++] = file;
    input_files[n_input_files] = NULL;
    return file;
}

Token *tokenize_file(char *path) {
    File *file = load_file(path);
    if (!file) return NULL;
    return tokenize(file);
}
