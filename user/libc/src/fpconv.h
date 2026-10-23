/* Exact binary <-> decimal floating point conversion shared by strtod/strtof (fpconv.c) and the printf engine (format.c).
 * Private to libgap (not installed). */
#ifndef LIBGAP_FPCONV_H
#define LIBGAP_FPCONV_H

#include <stdint.h>

/* The exact decimal expansion of a finite double has at most 309 digits before the point and 1074 after it. One spare
 * chunk of 9 digits is generated past the last wanted digit, plus the slot in front that a rounding carry may use. */
#define FP_DIGITS_MAX 1424

typedef struct {
    unsigned char *d;       /* digit VALUES (0..9), d[0] is the most significant digit; d[-1] is writable (carry slot) */
    int nd;                 /* digits present */
    int dp;                 /* digits before the decimal point: value = 0.d[0]d[1]... * 10^dp (dp == 0 for 0.00123) */
    int more;               /* the exact expansion continues past d[nd-1] with at least one non-zero digit */
    unsigned char buf[FP_DIGITS_MAX + 2];
} FpDigits;

/* Generates the exact decimal digits of v (finite, v > 0), stopping once the digit after the wanted ones is known:
 *   sig  > 0: until sig + 1 significant digits (counted from the first non-zero digit) exist,
 *   sig == 0: until frac + 1 digits after the decimal point exist.
 * Whichever way it stops early, `more` says whether the exact expansion goes on. */
void __fp_digits(double v, int sig, int frac, FpDigits *out);

#endif
