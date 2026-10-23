#ifndef _MATH_H
#define _MATH_H
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.78539816339744830962
#define M_E 2.7182818284590452354
#define M_SQRT2 1.41421356237309504880
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402
#define HUGE_VAL (__builtin_huge_val())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
double fabs(double x);
double sqrt(double x);
double floor(double x);
double ceil(double x);
double round(double x);
double trunc(double x);
double fmod(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double hypot(double x, double y);
double fmin(double a, double b);
double fmax(double a, double b);
double copysign(double x, double y);
double ldexp(double x, int e);
double frexp(double x, int *e);
double modf(double x, double *ip);
float fabsf(float x);
float sqrtf(float x);
float floorf(float x);
float ceilf(float x);
float sinf(float x);
float cosf(float x);
float atan2f(float y, float x);
float powf(float x, float y);
float fmodf(float x, float y);
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#endif
