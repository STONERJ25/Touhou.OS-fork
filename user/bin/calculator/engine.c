/* Ran's Calculator: tokenizer, parser/evaluator, extended precision math and number formatting. */
#include "engine.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <setjmp.h>

#define C_PI      3.14159265358979323846264338327950288L
#define C_HALF_PI 1.57079632679489661923132169163975144L
#define C_TAU     6.28318530717958647692528676655900577L
#define C_E       2.71828182845904523536028747135266250L
#define C_PHI     1.61803398874989484820458683436563812L
#define C_LOG2E   1.44269504088896340735992468100189214L
#define C_LN2     0.693147180559945309417232121458176568L
#define C_MAX     __LDBL_MAX__

/* ---------------------------------------------------------------- x87 primitives (full 64 bit mantissa) */

/* Every instruction sequence loads its operands from memory and stores the result back, so the compiler's own x87 register
 * allocation (which keeps long doubles on the FP stack) never meets the stack effects of the inline assembly. */
static inline cnum x_fabs(cnum x) { return __builtin_fabsl(x); }
#define X87_UNARY(fname, seq) \
    static cnum fname(cnum x) { cnum r; __asm__("fldt %1\n\t" seq "\n\tfstpt %0" : "=m"(r) : "m"(x) : "cc"); return r; }
X87_UNARY(x_sqrt, "fsqrt")
X87_UNARY(x_rint, "frndint")
X87_UNARY(x_sin, "fsin")
X87_UNARY(x_cos, "fcos")
X87_UNARY(x_tan, "fptan\n\tfstp %%st(0)")
X87_UNARY(x_f2xm1, "f2xm1")
static cnum x_log2(cnum x) { cnum r; __asm__("fld1\n\tfldt %1\n\tfyl2x\n\tfstpt %0" : "=m"(r) : "m"(x) : "cc"); return r; }
static cnum x_ln(cnum x) { cnum r; __asm__("fldln2\n\tfldt %1\n\tfyl2x\n\tfstpt %0" : "=m"(r) : "m"(x) : "cc"); return r; }
static cnum x_log10(cnum x) { cnum r; __asm__("fldlg2\n\tfldt %1\n\tfyl2x\n\tfstpt %0" : "=m"(r) : "m"(x) : "cc"); return r; }
static cnum x_atan2(cnum y, cnum x) {
    cnum r;
    __asm__("fldt %1\n\tfldt %2\n\tfpatan\n\tfstpt %0" : "=m"(r) : "m"(y), "m"(x) : "cc");
    return r;
}
static cnum x_scale(cnum x, cnum n) {
    cnum r;
    __asm__("fldt %2\n\tfldt %1\n\tfscale\n\tfstp %%st(1)\n\tfstpt %0" : "=m"(r) : "m"(x), "m"(n) : "cc");
    return r;
}
static cnum x_fmod(cnum x, cnum y) {
    cnum r;
    __asm__("fldt %2\n\tfldt %1\n1:\tfprem\n\tfnstsw %%ax\n\tsahf\n\tjp 1b\n\tfstp %%st(1)\n\tfstpt %0"
            : "=m"(r) : "m"(x), "m"(y) : "ax", "cc");
    return r;
}

static bool finite_(cnum x) { return x == x && x_fabs(x) <= C_MAX; }
static cnum x_floor(cnum x) {
    if (!(x_fabs(x) < 9.2e18L)) return x;
    cnum t = (cnum)(long long)x;
    return t > x ? t - 1 : t;
}
static cnum x_exp2(cnum t) {
    if (t > 16383.0L) return __builtin_infl();
    if (t < -16445.0L) return 0;
    cnum n = x_rint(t);
    return x_scale(x_f2xm1(t - n) + 1, n);
}
static cnum x_exp(cnum x) { return x_exp2(x * C_LOG2E); }
/* e^x - 1 without losing the small digits */
static cnum x_expm1(cnum x) {
    cnum t = x * C_LOG2E;
    if (x_fabs(t) <= 1.0L) return x_f2xm1(t);
    return x_exp2(t) - 1;
}

static bool is_whole(cnum x) { return x_fabs(x - x_rint(x)) <= 1e-16L * (x_fabs(x) > 1 ? x_fabs(x) : 1); }

/* 10^n for n >= 0 by repeated squaring */
static cnum pow10_(int n) {
    cnum r = 1, b = 10;
    while (n > 0) {
        if (n & 1) r *= b;
        n >>= 1;
        if (n) b *= b;
    }
    return r;
}

/* ---------------------------------------------------------------- parser state */

typedef enum { T_END, T_NUM, T_NAME, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_CARET, T_LP, T_RP, T_COMMA, T_BANG, T_PCT, T_SQ2, T_SQ3 } TokKind;

typedef struct Tok {
    TokKind kind;
    int pos, len;
    cnum num;
    char name[24];
} Tok;

typedef struct Parser {
    const char *src;
    int p;
    Tok tok;
    const CalcContext *ctx;
    CalcError *err;
    jmp_buf jb;
    int depth;
} Parser;

typedef struct Val { cnum v; bool pct; } Val;

static void fail(Parser *ps, int pos, int len, const char *fmt, ...) __attribute__((noreturn, format(printf, 4, 5)));
static void fail(Parser *ps, int pos, int len, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ps->err->msg, sizeof ps->err->msg, fmt, ap);
    va_end(ap);
    ps->err->pos = pos;
    ps->err->len = len;
    longjmp(ps->jb, 1);
}

/* ---------------------------------------------------------------- lexer */

static const char *tok_text(Parser *ps, const Tok *t, char *buf, size_t cap) {
    size_t n = (size_t)t->len < cap - 1 ? (size_t)t->len : cap - 1;
    memcpy(buf, ps->src + t->pos, n);
    buf[n] = 0;
    return buf;
}

static void lex_number(Parser *ps, int i) {
    const unsigned char *s = (const unsigned char *)ps->src;
    Tok *t = &ps->tok;
    int start = i;
    cnum acc = 0;
    int sig = 0, dec_exp = 0;
    bool dot = false;
    for (;; i++) {
        if (s[i] == '.') {
            if (dot) fail(ps, start, i + 1 - start, "A number can only have one decimal point");
            dot = true;
        } else if (isdigit(s[i])) {
            int d = s[i] - '0';
            if (sig < 19) {
                acc = acc * 10 + d;
                if (acc > 0) sig++;
                if (dot) dec_exp--;
            } else if (!dot) dec_exp++;                      /* digits beyond the 19th only shift the magnitude */
        } else break;
    }
    if (s[i] == 'e' || s[i] == 'E') {
        int j = i + 1;
        int sign = 1;
        if (s[j] == '+' || s[j] == '-') { sign = s[j] == '-' ? -1 : 1; j++; }
        if (isdigit(s[j])) {
            long ev = 0;
            while (isdigit(s[j])) { if (ev < 100000) ev = ev * 10 + (s[j] - '0'); j++; }
            dec_exp += sign * (int)ev;
            i = j;
        }
    }
    if (s[i] == '.') fail(ps, start, i + 1 - start, "A number can only have one decimal point");
    cnum v = 0;
    if (acc != 0) {
        int mag = dec_exp + sig;                              /* decimal exponent of the leading digit + 1 */
        if (mag < -4950) v = 0;
        else if (dec_exp >= 0) v = acc * pow10_(dec_exp);
        else if (-dec_exp <= 4900) v = acc / pow10_(-dec_exp);
        else v = (acc / pow10_(4900)) / pow10_(-dec_exp - 4900);
        if (!finite_(v)) fail(ps, start, i - start, "That number is too large");
    }
    t->kind = T_NUM;
    t->pos = start;
    t->len = i - start;
    t->num = v;
    ps->p = i;
}

static void set_tok(Parser *ps, TokKind k, int pos, int len) {
    ps->tok.kind = k;
    ps->tok.pos = pos;
    ps->tok.len = len;
    ps->p = pos + len;
}

static void lex(Parser *ps) {
    const unsigned char *s = (const unsigned char *)ps->src;
    int i = ps->p;
    while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') i++;
    Tok *t = &ps->tok;
    t->name[0] = 0;
    t->num = 0;
    if (!s[i]) { set_tok(ps, T_END, i, 0); return; }
    unsigned c = s[i];
    if (isdigit(c) || (c == '.' && isdigit(s[i + 1]))) { lex_number(ps, i); return; }
    if (isalpha(c) || c == '_') {
        int j = i, n = 0;
        while (isalnum(s[j]) || s[j] == '_') { if (n < (int)sizeof t->name - 1) t->name[n++] = (char)tolower(s[j]); j++; }
        t->name[n] = 0;
        set_tok(ps, T_NAME, i, j - i);
        return;
    }
    if (c >= 0x80) {                                           /* the unicode symbols a pasted formula may carry */
        unsigned c1 = s[i + 1], c2 = c1 ? s[i + 2] : 0;
        if (c == 0xC3 && c1 == 0x97) { set_tok(ps, T_STAR, i, 2); return; }            /* x */
        if (c == 0xC3 && c1 == 0xB7) { set_tok(ps, T_SLASH, i, 2); return; }           /* division sign */
        if (c == 0xC2 && c1 == 0xB7) { set_tok(ps, T_STAR, i, 2); return; }            /* middle dot */
        if (c == 0xC2 && c1 == 0xB2) { set_tok(ps, T_SQ2, i, 2); return; }             /* squared */
        if (c == 0xC2 && c1 == 0xB3) { set_tok(ps, T_SQ3, i, 2); return; }             /* cubed */
        if (c == 0xC2 && c1 == 0xA0) { ps->p = i + 2; lex(ps); return; }               /* no-break space */
        if (c == 0xE2 && c1 == 0x88 && c2 == 0x92) { set_tok(ps, T_MINUS, i, 3); return; }
        if (c == 0xE2 && c1 == 0x80 && (c2 == 0x93 || c2 == 0x94)) { set_tok(ps, T_MINUS, i, 3); return; }
        if (c == 0xE2 && c1 == 0x88 && c2 == 0x9A) { strcpy(t->name, "sqrt"); set_tok(ps, T_NAME, i, 3); return; }
        if (c == 0xCF && c1 == 0x80) { strcpy(t->name, "pi"); set_tok(ps, T_NAME, i, 2); return; }
        int n = 1;
        if ((c & 0xE0) == 0xC0) n = 2; else if ((c & 0xF0) == 0xE0) n = 3; else if ((c & 0xF8) == 0xF0) n = 4;
        char shown[8];
        int k = 0;
        for (; k < n && s[i + k]; k++) shown[k] = (char)s[i + k];
        shown[k] = 0;
        fail(ps, i, k, "I do not know the character '%s'", shown);
    }
    switch (c) {
    case '+': set_tok(ps, T_PLUS, i, 1); return;
    case '-': set_tok(ps, T_MINUS, i, 1); return;
    case '*':
        if (s[i + 1] == '*') { set_tok(ps, T_CARET, i, 2); return; }
        set_tok(ps, T_STAR, i, 1);
        return;
    case '/': set_tok(ps, T_SLASH, i, 1); return;
    case '^': set_tok(ps, T_CARET, i, 1); return;
    case '(': case '[': set_tok(ps, T_LP, i, 1); return;
    case ')': case ']': set_tok(ps, T_RP, i, 1); return;
    case ',': set_tok(ps, T_COMMA, i, 1); return;
    case '!': set_tok(ps, T_BANG, i, 1); return;
    case '%': set_tok(ps, T_PCT, i, 1); return;
    case '=': {
        int j = i + 1;
        while (s[j] == ' ' || s[j] == '\t') j++;
        if (!s[j]) { set_tok(ps, T_END, i, 0); return; }               /* "2+3=" */
        fail(ps, i, 1, "Use '=' only at the very end");
    }
    default: break;
    }
    fail(ps, i, 1, "I do not know the character '%c'", (int)c);
}

static void advance(Parser *ps) { lex(ps); }

/* ---------------------------------------------------------------- functions */

enum {
    F_SIN, F_COS, F_TAN, F_ASIN, F_ACOS, F_ATAN, F_SINH, F_COSH, F_TANH, F_LN, F_LOG, F_LOG2, F_EXP, F_SQRT, F_CBRT, F_ABS,
    F_FLOOR, F_CEIL, F_ROUND, F_FACT, F_POW, F_ATAN2, F_MIN, F_MAX, F_MOD, F_ROOT, F_DEG, F_RAD, F_NFUNCS
};

static const struct FuncDef { const char *name; int id; int nargs; const char *usage; } g_funcs[] = {
    { "sin", F_SIN, 1, "sin(angle)" }, { "cos", F_COS, 1, "cos(angle)" }, { "tan", F_TAN, 1, "tan(angle)" },
    { "asin", F_ASIN, 1, "asin(x)" }, { "acos", F_ACOS, 1, "acos(x)" }, { "atan", F_ATAN, 1, "atan(x)" },
    { "sinh", F_SINH, 1, "sinh(x)" }, { "cosh", F_COSH, 1, "cosh(x)" }, { "tanh", F_TANH, 1, "tanh(x)" },
    { "ln", F_LN, 1, "ln(x)" }, { "log", F_LOG, 1, "log(x)" }, { "log10", F_LOG, 1, "log10(x)" }, { "log2", F_LOG2, 1, "log2(x)" },
    { "exp", F_EXP, 1, "exp(x)" }, { "sqrt", F_SQRT, 1, "sqrt(x)" }, { "cbrt", F_CBRT, 1, "cbrt(x)" },
    { "abs", F_ABS, 1, "abs(x)" }, { "floor", F_FLOOR, 1, "floor(x)" }, { "ceil", F_CEIL, 1, "ceil(x)" },
    { "round", F_ROUND, 1, "round(x)" }, { "fact", F_FACT, 1, "fact(n)" }, { "pow", F_POW, 2, "pow(x, y)" },
    { "atan2", F_ATAN2, 2, "atan2(y, x)" }, { "min", F_MIN, 2, "min(a, b)" }, { "max", F_MAX, 2, "max(a, b)" },
    { "mod", F_MOD, 2, "mod(a, b)" }, { "root", F_ROOT, 2, "root(x, n)" }, { "deg", F_DEG, 1, "deg(radians)" },
    { "rad", F_RAD, 1, "rad(degrees)" },
};
#define NFUNCS_DEF ((int)(sizeof g_funcs / sizeof g_funcs[0]))

static const struct FuncDef *find_func(const char *name) {
    for (int i = 0; i < NFUNCS_DEF; i++) if (!strcmp(g_funcs[i].name, name)) return &g_funcs[i];
    return NULL;
}

static cnum check_result(Parser *ps, cnum v, int pos, int len) {
    if (v != v) fail(ps, pos, len, "That is not a number (undefined result)");
    if (!finite_(v)) fail(ps, pos, len, "Overflow: the result is too large");
    return v;
}

static cnum factorial(Parser *ps, cnum x, int pos, int len) {
    if (!is_whole(x) || x < 0) fail(ps, pos, len, "A factorial needs a whole number, zero or more");
    long n = (long)x_rint(x);
    if (n > 1754) fail(ps, pos, len, "Overflow: that factorial is too large");
    cnum r = 1;
    for (long i = 2; i <= n; i++) r *= (cnum)i;
    return r;
}

static cnum power(Parser *ps, cnum x, cnum y, int pos, int len) {
    if (y == 0) return 1;
    if (x == 0) {
        if (y < 0) fail(ps, pos, len, "Division by zero (zero to a negative power)");
        return 0;
    }
    if (is_whole(y) && x_fabs(y) <= 2147483647.0L) {
        long n = (long)x_rint(x_fabs(y));
        cnum r = 1, b = x;
        while (n > 0) {
            if (n & 1) r *= b;
            n >>= 1;
            if (n) b *= b;
            if (!finite_(r) || !finite_(b)) break;
        }
        if (!finite_(r)) fail(ps, pos, len, "Overflow: the result is too large");
        if (y < 0) {
            if (r == 0) fail(ps, pos, len, "Division by zero");
            r = 1 / r;
        }
        return r;
    }
    if (x < 0) fail(ps, pos, len, "A negative number cannot be raised to a fractional power");
    return check_result(ps, x_exp2(y * x_log2(x)), pos, len);
}

static cnum cube_root(cnum x) {
    if (x == 0) return 0;
    bool neg = x < 0;
    cnum a = neg ? -x : x;
    cnum y = x_exp(x_ln(a) / 3);
    y = y - (y * y * y - a) / (3 * y * y);                     /* one Newton step settles the last digits */
    cnum r = x_rint(y);
    if (r * r * r == a) y = r;
    return neg ? -y : y;
}

/* Angles: degrees are reduced exactly, so sin(180) is 0 and cos(90) is 0, not 1e-17. Radians snap to exact multiples of pi/2. */
static cnum trig(Parser *ps, int which, cnum x, int pos, int len) {
    int quad = -1;
    cnum rad = 0;
    if (ps->ctx->degrees) {
        if (x_fabs(x) > 1e18L) fail(ps, pos, len, "That angle is too large");
        cnum r = x_fmod(x, 360);
        if (r < 0) r += 360;
        cnum q = r / 90;
        if (q == x_rint(q)) quad = (int)x_rint(q) % 4;
        else rad = r * (C_PI / 180);
    } else {
        if (x_fabs(x) > 1e10L) fail(ps, pos, len, "That angle is too large for the trig functions");
        cnum qi = x_rint(x / C_HALF_PI);
        if (x_fabs(x - qi * C_HALF_PI) <= 1e-17L * (x_fabs(x) > 1 ? x_fabs(x) : 1)) {
            long long k = (long long)qi % 4;
            quad = (int)(k < 0 ? k + 4 : k);
        } else rad = x;
    }
    if (quad >= 0) {
        static const int sn[4] = { 0, 1, 0, -1 }, cs[4] = { 1, 0, -1, 0 };
        if (which == 0) return sn[quad];
        if (which == 1) return cs[quad];
        if (quad & 1) fail(ps, pos, len, "tan is undefined at this angle");
        return 0;
    }
    return which == 0 ? x_sin(rad) : which == 1 ? x_cos(rad) : x_tan(rad);
}

static cnum inverse_angle(Parser *ps, cnum rad) { return ps->ctx->degrees ? rad * (180 / C_PI) : rad; }

static cnum call_function(Parser *ps, const struct FuncDef *f, const cnum *a, int pos, int len) {
    cnum x = a[0], y = a[1], r = 0;
    switch (f->id) {
    case F_SIN: return trig(ps, 0, x, pos, len);
    case F_COS: return trig(ps, 1, x, pos, len);
    case F_TAN: return trig(ps, 2, x, pos, len);
    case F_ASIN: case F_ACOS:
        if (x_fabs(x) > 1 + 1e-18L) fail(ps, pos, len, "%s needs a value between -1 and 1", f->name);
        if (x > 1) x = 1;
        if (x < -1) x = -1;
        r = x_sqrt((1 - x) * (1 + x));
        return inverse_angle(ps, f->id == F_ASIN ? x_atan2(x, r) : x_atan2(r, x));
    case F_ATAN: return inverse_angle(ps, x_atan2(x, 1));
    case F_ATAN2:
        if (x == 0 && y == 0) fail(ps, pos, len, "atan2(0, 0) is undefined");
        return inverse_angle(ps, x_atan2(x, y));
    case F_SINH: { cnum e = x_expm1(x); return check_result(ps, (e + e / (e + 1)) / 2, pos, len); }
    case F_COSH: return check_result(ps, (x_exp(x) + x_exp(-x)) / 2, pos, len);
    case F_TANH: {
        if (x > 40) return 1;                                      /* tanh(40) differs from 1 by 1e-35 */
        if (x < -40) return -1;
        cnum e = x_expm1(2 * x);
        return e / (e + 2);
    }
    case F_LN: case F_LOG: case F_LOG2:
        if (x <= 0) fail(ps, pos, len, "%s needs a number above zero", f->name);
        return f->id == F_LN ? x_ln(x) : f->id == F_LOG ? x_log10(x) : x_log2(x);
    case F_EXP: return check_result(ps, x_exp(x), pos, len);
    case F_SQRT:
        if (x < 0) fail(ps, pos, len, "The square root of a negative number is not real");
        return x_sqrt(x);
    case F_CBRT: return cube_root(x);
    case F_ABS: return x_fabs(x);
    case F_FLOOR: return x_floor(x);
    case F_CEIL: return -x_floor(-x);
    case F_ROUND: return x < 0 ? -x_floor(-x + 0.5L) : x_floor(x + 0.5L);
    case F_FACT: return factorial(ps, x, pos, len);
    case F_POW: return power(ps, x, y, pos, len);
    case F_MIN: return x < y ? x : y;
    case F_MAX: return x > y ? x : y;
    case F_MOD:
        if (y == 0) fail(ps, pos, len, "Division by zero (mod 0)");
        return x - y * x_floor(x / y);
    case F_ROOT:
        if (y == 0) fail(ps, pos, len, "The 0th root does not exist");
        if (x < 0 && is_whole(y) && ((long long)x_rint(y) & 1)) return -power(ps, -x, 1 / y, pos, len);
        if (x < 0) fail(ps, pos, len, "That root of a negative number is not real");
        return power(ps, x, 1 / y, pos, len);
    case F_DEG: return x * (180 / C_PI);
    case F_RAD: return x * (C_PI / 180);
    default: break;
    }
    return 0;
}

/* ---------------------------------------------------------------- recursive descent */

static Val parse_expr(Parser *ps);
static Val parse_unary(Parser *ps);

static bool starts_operand(const Parser *ps) {
    TokKind k = ps->tok.kind;
    return k == T_NUM || k == T_NAME || k == T_LP || k == T_MINUS || k == T_PLUS;
}

static void expect_rp(Parser *ps, int open_pos) {
    if (ps->tok.kind == T_RP) { advance(ps); return; }
    if (ps->tok.kind == T_END) fail(ps, open_pos, 1, "A closing parenthesis ')' is missing");
    char b[24];
    fail(ps, ps->tok.pos, ps->tok.len, "Expected ')' but found '%s'", tok_text(ps, &ps->tok, b, sizeof b));
}

static Val parse_name(Parser *ps) {
    Tok nt = ps->tok;
    const char *name = nt.name;
    Val r = { 0, false };
    if (!strcmp(name, "pi")) { advance(ps); r.v = C_PI; return r; }
    if (!strcmp(name, "e")) { advance(ps); r.v = C_E; return r; }
    if (!strcmp(name, "tau")) { advance(ps); r.v = C_TAU; return r; }
    if (!strcmp(name, "phi")) { advance(ps); r.v = C_PHI; return r; }
    if (!strcmp(name, "ans")) { advance(ps); r.v = ps->ctx->ans; return r; }
    const struct FuncDef *f = find_func(name);
    if (!f) {
        char b[32];
        fail(ps, nt.pos, nt.len, "I do not know the name '%s'", tok_text(ps, &nt, b, sizeof b));
    }
    advance(ps);
    cnum args[2] = { 0, 0 };
    int n = 0;
    if (ps->tok.kind == T_LP) {
        int open = ps->tok.pos;
        advance(ps);
        if (ps->tok.kind != T_RP) {
            for (;;) {
                Val a = parse_expr(ps);
                if (n < 2) args[n] = a.v;
                n++;
                if (ps->tok.kind == T_COMMA) { advance(ps); continue; }
                break;
            }
        }
        expect_rp(ps, open);
        if (n != f->nargs) {
            fail(ps, nt.pos, ps->p - nt.pos, f->nargs == 1 ? "%s takes one value: %s" : "%s takes two values: %s", f->name, f->usage);
        }
    } else if (f->nargs == 1 && starts_operand(ps)) {                      /* "sqrt 16" and "sin 30" */
        args[0] = parse_unary(ps).v;
    } else {
        fail(ps, nt.pos, nt.len, "%s needs a value in parentheses: %s", f->name, f->usage);
    }
    r.v = check_result(ps, call_function(ps, f, args, nt.pos, ps->p - nt.pos), nt.pos, nt.len);
    return r;
}

static Val parse_primary(Parser *ps) {
    Val r = { 0, false };
    if (++ps->depth > 120) fail(ps, ps->tok.pos, ps->tok.len, "Too many nested parentheses");
    switch (ps->tok.kind) {
    case T_NUM:
        r.v = ps->tok.num;
        advance(ps);
        break;
    case T_NAME:
        r = parse_name(ps);
        break;
    case T_LP: {
        int open = ps->tok.pos;
        advance(ps);
        if (ps->tok.kind == T_RP) fail(ps, open, ps->tok.pos + 1 - open, "There is nothing between the parentheses");
        r = parse_expr(ps);
        r.pct = false;
        expect_rp(ps, open);
        break;
    }
    case T_END:
        fail(ps, ps->tok.pos, 0, "The expression ends too early: a number is missing");
    case T_RP:
        fail(ps, ps->tok.pos, ps->tok.len, "This ')' has no matching '('");
    default: {
        char b[24];
        fail(ps, ps->tok.pos, ps->tok.len, "Expected a number or '(' but found '%s'", tok_text(ps, &ps->tok, b, sizeof b));
    }
    }
    ps->depth--;
    return r;
}

static Val parse_postfix(Parser *ps) {
    Val v = parse_primary(ps);
    for (;;) {
        int pos = ps->tok.pos, len = ps->tok.len;
        if (ps->tok.kind == T_BANG) { v.v = factorial(ps, v.v, pos, len); v.pct = false; advance(ps); }
        else if (ps->tok.kind == T_PCT) { v.v /= 100; v.pct = true; advance(ps); }
        else if (ps->tok.kind == T_SQ2) { v.v = check_result(ps, v.v * v.v, pos, len); v.pct = false; advance(ps); }
        else if (ps->tok.kind == T_SQ3) { v.v = check_result(ps, v.v * v.v * v.v, pos, len); v.pct = false; advance(ps); }
        else break;
    }
    return v;
}

static Val parse_power(Parser *ps) {
    Val b = parse_postfix(ps);
    if (ps->tok.kind == T_CARET) {
        int pos = ps->tok.pos, len = ps->tok.len;
        advance(ps);
        Val e = parse_unary(ps);                                   /* right associative, and "2^-3" works */
        b.v = power(ps, b.v, e.v, pos, len);
        b.pct = false;
    }
    return b;
}

static Val parse_unary(Parser *ps) {
    if (++ps->depth > 120) fail(ps, ps->tok.pos, ps->tok.len, "Too many nested operators");
    Val v;
    if (ps->tok.kind == T_PLUS) { advance(ps); v = parse_unary(ps); }
    else if (ps->tok.kind == T_MINUS) { advance(ps); v = parse_unary(ps); v.v = -v.v; }
    else v = parse_power(ps);
    ps->depth--;
    return v;
}

static Val parse_term(Parser *ps) {
    Val l = parse_unary(ps);
    for (;;) {
        TokKind k = ps->tok.kind;
        int pos = ps->tok.pos, len = ps->tok.len;
        bool is_mod = k == T_NAME && !strcmp(ps->tok.name, "mod");
        if (k == T_STAR || k == T_SLASH || is_mod) {
            advance(ps);
            Val r = parse_unary(ps);
            if (k == T_STAR) l.v = l.v * r.v;
            else if (k == T_SLASH) {
                if (r.v == 0) fail(ps, pos, len, "Division by zero");
                l.v = l.v / r.v;
            } else {
                if (r.v == 0) fail(ps, pos, len + 0, "Division by zero (mod 0)");
                l.v = l.v - r.v * x_floor(l.v / r.v);
            }
            l.pct = false;
            check_result(ps, l.v, pos, len);
        } else if (k == T_LP || k == T_NAME) {                       /* implicit multiplication: 2pi, 3(4+5), (1+2)(3+4) */
            Val r = parse_unary(ps);
            l.v = l.v * r.v;
            l.pct = false;
            check_result(ps, l.v, pos, len);
        } else break;
    }
    return l;
}

static Val parse_expr(Parser *ps) {
    Val l = parse_term(ps);
    while (ps->tok.kind == T_PLUS || ps->tok.kind == T_MINUS) {
        bool plus = ps->tok.kind == T_PLUS;
        int pos = ps->tok.pos, len = ps->tok.len;
        advance(ps);
        Val r = parse_term(ps);
        cnum rv = r.pct ? l.v * r.v : r.v;                          /* 200+10% is 220 */
        l.v = plus ? l.v + rv : l.v - rv;
        l.pct = false;
        check_result(ps, l.v, pos, len);
    }
    return l;
}

bool calc_eval(const CalcContext *ctx, const char *expr, cnum *out, CalcError *err) {
    Parser ps;
    CalcError dummy;
    memset(&ps, 0, sizeof ps);
    if (!err) err = &dummy;
    err->msg[0] = 0;
    err->pos = -1;
    err->len = 0;
    ps.src = expr ? expr : "";
    ps.ctx = ctx;
    ps.err = err;
    if (setjmp(ps.jb)) return false;
    advance(&ps);
    if (ps.tok.kind == T_END) fail(&ps, 0, 0, "There is nothing to calculate yet");
    Val v = parse_expr(&ps);
    if (ps.tok.kind != T_END) {
        char b[24];
        if (ps.tok.kind == T_RP) fail(&ps, ps.tok.pos, ps.tok.len, "This ')' has no matching '('");
        if (ps.tok.kind == T_NUM) fail(&ps, ps.tok.pos, ps.tok.len, "Two numbers in a row: an operator like + or * is missing");
        if (ps.tok.kind == T_COMMA) fail(&ps, ps.tok.pos, ps.tok.len, "A comma only belongs between function arguments");
        fail(&ps, ps.tok.pos, ps.tok.len, "I did not expect '%s' here", tok_text(&ps, &ps.tok, b, sizeof b));
    }
    check_result(&ps, v.v, 0, 0);
    if (out) *out = v.v;
    return true;
}

/* ---------------------------------------------------------------- formatting */

int calc_format(cnum v, char *buf, size_t cap) {
    char out[64];
    int n = 0;
    if (v != v) { n = snprintf(out, sizeof out, "undefined"); goto done; }
    if (!finite_(v)) { n = snprintf(out, sizeof out, v < 0 ? "-overflow" : "overflow"); goto done; }
    if (v == 0) { n = snprintf(out, sizeof out, "0"); goto done; }
    bool neg = v < 0;
    if (neg) v = -v;

    int e10 = (int)x_floor(x_log10(v));
    cnum m;
    if (e10 >= 0) m = e10 <= 4000 ? v / pow10_(e10) : (v / pow10_(4000)) / pow10_(e10 - 4000);
    else m = -e10 <= 4000 ? v * pow10_(-e10) : (v * pow10_(4000)) * pow10_(-e10 - 4000);
    while (m >= 10) { m /= 10; e10++; }
    while (m < 1) { m *= 10; e10--; }
    long long dig = (long long)(m * 100000000000000.0L + 0.5L);                /* 15 significant digits */
    if (dig >= 1000000000000000LL) { dig = 100000000000000LL; e10++; }
    char ds[16];
    for (int i = 14; i >= 0; i--) { ds[i] = (char)('0' + dig % 10); dig /= 10; }
    ds[15] = 0;
    int nd = 15;
    while (nd > 1 && ds[nd - 1] == '0') nd--;

    if (neg) out[n++] = '-';
    if (e10 >= 15 || e10 < -6) {                                              /* scientific: 1.5e-7 */
        out[n++] = ds[0];
        if (nd > 1) { out[n++] = '.'; for (int i = 1; i < nd; i++) out[n++] = ds[i]; }
        n += snprintf(out + n, sizeof out - (size_t)n, "e%d", e10);
    } else if (e10 >= 0) {
        for (int i = 0; i <= e10; i++) out[n++] = i < nd ? ds[i] : '0';
        if (nd > e10 + 1) { out[n++] = '.'; for (int i = e10 + 1; i < nd; i++) out[n++] = ds[i]; }
    } else {
        out[n++] = '0';
        out[n++] = '.';
        for (int i = 0; i < -e10 - 1; i++) out[n++] = '0';
        for (int i = 0; i < nd; i++) out[n++] = ds[i];
    }
    out[n] = 0;
done:
    if (cap) {
        size_t c = (size_t)n < cap - 1 ? (size_t)n : cap - 1;
        memcpy(buf, out, c);
        buf[c] = 0;
    }
    return n;
}

/* ---------------------------------------------------------------- pasting */

size_t calc_sanitize(const char *in, size_t len, char *out, size_t cap) {
    size_t n = 0;
    bool has_paren = false;
    for (size_t i = 0; i < len; i++) if (in[i] == '(' || in[i] == '[') has_paren = true;
    for (size_t i = 0; i < len && n + 4 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\n' || c == '\r' || c == '\t') { if (n && out[n - 1] != ' ') out[n++] = ' '; continue; }
        if (c == 0xC2 && i + 1 < len && (unsigned char)in[i + 1] == 0xA0) { i++; continue; }             /* no-break space */
        if (c == 0xE2 && i + 2 < len && (unsigned char)in[i + 1] == 0x80 &&
            ((unsigned char)in[i + 2] <= 0x8A || (unsigned char)in[i + 2] == 0xAF)) { i += 2; continue; }  /* thin and wide spaces */
        if (c == ',' && !has_paren && i > 0 && isdigit((unsigned char)in[i - 1]) && i + 3 < len + 0 &&
            isdigit((unsigned char)in[i + 1]) && isdigit((unsigned char)in[i + 2]) && isdigit((unsigned char)in[i + 3]) &&
            (i + 4 >= len || !isdigit((unsigned char)in[i + 4]))) continue;                              /* 1,234,567 */
        if (c < 0x20 || c == 0x7F) continue;
        out[n++] = (char)c;
    }
    while (n && out[n - 1] == ' ') n--;
    size_t start = 0;
    while (start < n && out[start] == ' ') start++;
    if (start) { memmove(out, out + start, n - start); n -= start; }
    out[n] = 0;
    return n;
}
