/*
 * Ran's Calculator: the expression engine.
 *
 * Numbers are x87 extended precision (long double, 64 bit mantissa), so results are exact well past the 15 significant
 * digits that get displayed. The grammar, loosest binding first:
 *
 *     expr    = term { ("+" | "-") term }
 *     term    = unary { ("*" | "/" | "mod" | implicit multiplication) unary }
 *     unary   = ("-" | "+") unary | power
 *     power   = postfix [ "^" unary ]                  right associative, so 2^3^2 = 2^9 and -2^2 = -4
 *     postfix = primary { "!" | "%" | "²" | "³" }
 *     primary = number | constant | name "(" args ")" | name unary | "(" expr ")"
 *
 * "50%" is 0.5. After "+" or "-" a percentage means a share of the left operand, as on a pocket calculator:
 * 200+10% is 220 and 200-10% is 180, while 200*10% is 20.
 */
#ifndef CALC_ENGINE_H
#define CALC_ENGINE_H

#include <stdbool.h>
#include <stddef.h>

typedef long double cnum;

typedef struct CalcError {
    char msg[112];
    int pos;                    /* byte offset of the offending token in the expression, -1 when there is none */
    int len;                    /* its length in bytes */
} CalcError;

typedef struct CalcContext {
    bool degrees;               /* trigonometry works in degrees (otherwise radians) */
    cnum ans;                   /* the value of the name "ans" */
} CalcContext;

/* Evaluates expr. Returns false and fills *err when the expression is wrong (division by zero, syntax error, ...). */
bool calc_eval(const CalcContext *ctx, const char *expr, cnum *out, CalcError *err);

/* Writes v with at most 15 significant digits and no trailing zeros ("0.3", "1.5e-7", "123456789012345", "1e15"). */
int calc_format(cnum v, char *buf, size_t cap);

/* A pasted piece of text made safe for the expression line: unusual spaces removed, x * and unicode operators mapped. */
size_t calc_sanitize(const char *in, size_t len, char *out, size_t cap);

#endif
