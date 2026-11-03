/*
 * bignum: arbitrary-precision decimal numbers shared by the bc and dc applets (this file is not an applet).
 * A Num is a sign, a scale (digits after the decimal point) and a magnitude in base 10^9 limbs, so the value is
 * (+/-) mantissa / 10^scale.  Every function returns a fresh Num (free it with num_free); none modifies its arguments.
 * The scale rules follow GNU bc: add/sub keep the larger scale, mul keeps min(sa+sb, max(scale, sa, sb)), div and the
 * power with a negative exponent use the scale argument.
 */
#ifndef BIGNUM_H
#define BIGNUM_H

#include <stdint.h>
#include <stddef.h>

typedef struct Num {
    int neg;            /* 1 when negative (never set for zero) */
    int scale;          /* decimal digits after the point */
    int n;              /* limbs in use; 0 for zero */
    int cap;
    uint32_t *d;        /* little endian base 10^9; d[n-1] != 0 */
} Num;

Num  *num_new(void);                                    /* zero, scale 0 */
Num  *num_from_long(long v);
Num  *num_copy(const Num *a);
void  num_free(Num *a);
int   num_is_zero(const Num *a);
int   num_cmp(const Num *a, const Num *b);              /* -1, 0, 1 */
Num  *num_neg(const Num *a);
Num  *num_add(const Num *a, const Num *b);
Num  *num_sub(const Num *a, const Num *b);
Num  *num_mul(const Num *a, const Num *b, int scale);
Num  *num_div(const Num *a, const Num *b, int scale);   /* NULL when b is zero */
Num  *num_mod(const Num *a, const Num *b, int scale);   /* bc: a - (a/b)*b with the quotient taken at SCALE; NULL on zero */
int   num_divmod(const Num *a, const Num *b, Num **quot, Num **rem);   /* dc: integer quotient and remainder; -1 on zero */
Num  *num_pow(const Num *a, const Num *b, int scale);   /* integer exponent (fraction ignored); NULL if it is too large */
Num  *num_sqrt(const Num *a, int scale);                /* NULL when a < 0 */
Num  *num_setscale(const Num *a, int scale);            /* truncate or pad with zeros to exactly SCALE digits */
Num  *num_intpart(const Num *a);                        /* scale 0 */
int   num_to_long(const Num *a, long *out);             /* integer part; 0 when it does not fit */
int   num_length(const Num *a);                         /* digits, as bc's length() and dc's Z */
Num  *num_modexp(const Num *base, const Num *e, const Num *m);   /* integers only; NULL on error */

/* Parses digits [0-9A-F] with an optional point in BASE (2..16 or more); stops at the first character that is not part of the
 * number.  Digits >= BASE count as BASE-1 unless the number is a single digit (GNU rule).  Returns the Num; *end is set. */
Num  *num_parse(const char *s, int base, const char **end);
/* Text of the number in OBASE (leading zero of a fraction omitted, "0" for zero), malloc'd.  Bases above 16 give each digit as
 * a space and a zero padded decimal number. */
char *num_to_string(const Num *a, int obase);

#endif
