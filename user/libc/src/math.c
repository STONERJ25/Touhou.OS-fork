/* libm: x87 for transcendentals, SSE (compiler builtins) for sqrt/abs, plain C for the rest. */
#include <math.h>
#include <stdint.h>

int signbit_d(double x) {
    union { double d; uint64_t u; } v = { x };
    return (int)(v.u >> 63);
}

static double x87_f2xm1(double x) { double r; __asm__("f2xm1" : "=t"(r) : "0"(x)); return r; }
static double x87_fscale(double x, double n) { double r; __asm__("fscale" : "=t"(r) : "0"(x), "u"(n) : "st(1)"); return r; }
static double x87_rint(double x) { double r; __asm__("frndint" : "=t"(r) : "0"(x)); return r; }

double fabs(double x) { return __builtin_fabs(x); }
double sqrt(double x) { return __builtin_sqrt(x); }
float fabsf(float x) { return __builtin_fabsf(x); }
float sqrtf(float x) { return __builtin_sqrtf(x); }

double floor(double x) {
    if (!(x < 4503599627370496.0 && x > -4503599627370496.0)) return x;
    double t = (double)(long long)x;
    return t > x ? t - 1.0 : t;
}

double ceil(double x) {
    if (!(x < 4503599627370496.0 && x > -4503599627370496.0)) return x;
    double t = (double)(long long)x;
    return t < x ? t + 1.0 : t;
}

double trunc(double x) {
    if (!(x < 4503599627370496.0 && x > -4503599627370496.0)) return x;
    return (double)(long long)x;
}

double round(double x) { return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5); }

double fmod(double x, double y) {
    if (y == 0 || x != x || y != y) return NAN;
    double r;
    __asm__("1: fprem; fnstsw %%ax; sahf; jp 1b" : "=t"(r) : "0"(x), "u"(y) : "ax", "cc", "st(1)");
    return r;
}

double sin(double x) { double r; __asm__("fsin" : "=t"(r) : "0"(x)); return r; }
double cos(double x) { double r; __asm__("fcos" : "=t"(r) : "0"(x)); return r; }
double tan(double x) { double r; __asm__("fptan; fstp %%st(0)" : "=t"(r) : "0"(x)); return r; }
double atan2(double y, double x) { double r; __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r; }
double atan(double x) { return atan2(x, 1.0); }
double asin(double x) { return atan2(x, sqrt((1.0 - x) * (1.0 + x))); }
double acos(double x) { return atan2(sqrt((1.0 - x) * (1.0 + x)), x); }

double log2(double x) { double r; __asm__("fld1; fxch; fyl2x" : "=t"(r) : "0"(x) : "st(1)"); return r; }
double log(double x) { double r; __asm__("fldln2; fxch; fyl2x" : "=t"(r) : "0"(x) : "st(1)"); return r; }
double log10(double x) { double r; __asm__("fldlg2; fxch; fyl2x" : "=t"(r) : "0"(x) : "st(1)"); return r; }

static double exp2d(double x) {
    if (x > 1023.0) return HUGE_VAL;
    if (x < -1074.0) return 0.0;
    double n = x87_rint(x);
    double f = x - n;
    return x87_fscale(x87_f2xm1(f) + 1.0, n);
}

double exp(double x) { return exp2d(x * 1.4426950408889634074); }

double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x == 0.0) return y > 0 ? 0.0 : HUGE_VAL;
    if (x < 0) {
        if (y != floor(y)) return NAN;
        double r = exp2d(y * log2(-x));
        return ((long long)y & 1) ? -r : r;
    }
    return exp2d(y * log2(x));
}

double sinh(double x) { return (exp(x) - exp(-x)) / 2.0; }
double cosh(double x) { return (exp(x) + exp(-x)) / 2.0; }
double tanh(double x) { double a = exp(x), b = exp(-x); return (a - b) / (a + b); }
double hypot(double x, double y) { return sqrt(x * x + y * y); }
double fmin(double a, double b) { return a < b ? a : b; }
double fmax(double a, double b) { return a > b ? a : b; }
double copysign(double x, double y) { return signbit_d(y) == signbit_d(x) ? x : -x; }

double frexp(double x, int *e) {
    union { double d; uint64_t u; } v = { x };
    int ex = (int)((v.u >> 52) & 0x7FF);
    if (ex == 0 || ex == 0x7FF) { *e = 0; return x; }
    *e = ex - 1022;
    v.u = (v.u & ~(0x7FFULL << 52)) | (1022ULL << 52);
    return v.d;
}

double ldexp(double x, int e) { return x87_fscale(x, (double)e); }

double modf(double x, double *ip) {
    double t = trunc(x);
    *ip = t;
    return x - t;
}

float floorf(float x) { return (float)floor(x); }
float ceilf(float x) { return (float)ceil(x); }
float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
float atan2f(float y, float x) { return (float)atan2(y, x); }
float powf(float x, float y) { return (float)pow(x, y); }
float fmodf(float x, float y) { return (float)fmod(x, y); }
