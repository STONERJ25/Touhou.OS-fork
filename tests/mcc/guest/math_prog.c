/* compiled by mcc ON the guest: libm */
#include <math.h>
#include <stdio.h>

static double series_pi(int terms) {
    double s = 0;
    for (int k = 0; k < terms; k++) s += (k % 2 ? -1.0 : 1.0) / (2 * k + 1);
    return 4 * s;
}

int main(void) {
    printf("sqrt(2)=%.6f sqrt(144)=%.1f\n", sqrt(2.0), sqrt(144.0));
    printf("sin(pi/6)=%.6f cos(pi/3)=%.6f tan(pi/4)=%.6f\n", sin(M_PI / 6), cos(M_PI / 3), tan(M_PI / 4));
    printf("atan2(1,1)=%.6f atan(1)*4=%.6f\n", atan2(1.0, 1.0), atan(1.0) * 4);
    printf("exp(1)=%.6f log(e)=%.6f log10(1000)=%.6f log2(1024)=%.6f\n", exp(1.0), log(M_E), log10(1000.0), log2(1024.0));
    printf("pow(2,0.5)=%.6f pow(2,-3)=%.6f pow(10,5)=%.1f\n", pow(2.0, 0.5), pow(2.0, -3), pow(10.0, 5));
    printf("floor/ceil/round/trunc: %.1f %.1f %.1f %.1f\n", floor(-1.5), ceil(-1.5), round(2.5), trunc(-2.7));
    printf("fmod(10,3)=%.1f hypot(3,4)=%.1f fabs(-2.5)=%.1f\n", fmod(10.0, 3.0), hypot(3.0, 4.0), fabs(-2.5));
    printf("fmin/fmax: %.1f %.1f copysign: %.1f\n", fmin(1.0, 2.0), fmax(1.0, 2.0), copysign(3.0, -1.0));
    printf("sinh(1)=%.6f cosh(1)=%.6f tanh(1)=%.6f\n", sinh(1.0), cosh(1.0), tanh(1.0));
    printf("asin(0.5)=%.6f acos(0.5)=%.6f\n", asin(0.5), acos(0.5));
    int e;
    double m = frexp(48.0, &e);
    printf("frexp(48)=%.3f*2^%d ldexp(0.75,6)=%.1f\n", m, e, ldexp(0.75, 6));
    double ip;
    double fp = modf(5.25, &ip);
    printf("modf(5.25)=%.2f+%.2f\n", ip, fp);
    printf("isnan/isinf: %d %d %d\n", isnan(NAN), isinf(INFINITY), isnan(1.0));
    printf("pi by series (1000 terms)=%.4f\n", series_pi(1000));
    float f = sqrtf(2.0f) * powf(2.0f, 3.0f);
    printf("float: %.4f %.4f\n", f, fabsf(-1.25f));
    return 0;
}
