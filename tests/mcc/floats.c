/* float and double arithmetic and conversions */
#include <stdio.h>
#include <math.h>

static double dsum(double a, double b, double c, double d, double e, double f, double g, double h, double i, double j) {
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10;
}

static float fmix(int a, float b, long c, double d, char e, float f) {
    return a + b + (float)c + (float)d + e + f;
}

static double halve(double x) { return x / 2; }

int main(void) {
    double d = 3.5, e = 1.25;
    float f = 2.5f, g = 0.75f;
    printf("d+e=%.3f d-e=%.3f d*e=%.3f d/e=%.3f\n", d + e, d - e, d * e, d / e);
    printf("f+g=%.3f f-g=%.3f f*g=%.3f f/g=%.3f\n", f + g, f - g, f * g, f / g);
    printf("-d=%.2f -f=%.2f\n", -d, -f);
    printf("cmp: %d %d %d %d %d %d\n", d < e, d > e, d <= 3.5, d >= 3.6, d == 3.5, d != 3.5);
    printf("fcmp: %d %d %d %d\n", f < g, f > g, f == 2.5f, f != 2.5f);
    int i = (int)d;
    long l = (long)(d * 1000);
    unsigned u = (unsigned)3000000000.0;
    printf("conv: %d %ld %u\n", i, l, u);
    printf("neg conv: %d %d\n", (int)-2.9, (int)-0.5);
    double fromi = 7;
    double fromu = 4000000000u;
    double froml = 1234567890123LL;
    float ff = 16777216;
    printf("fromint: %.1f %.1f %.1f %.1f\n", fromi, fromu, froml, ff);
    unsigned long long big = 18000000000000000000ULL;
    double bd = (double)big;
    printf("ull->double: %.4e\n", bd);
    unsigned long long back = (unsigned long long)bd;
    printf("double->ull: %llu\n", back);
    double mix = 1 + 2.5f + 3L;
    printf("mixed arithmetic: %.2f\n", mix);
    float fsum = 0;
    for (int k = 1; k <= 10; k++) fsum += 1.0f / k;
    printf("harmonic float: %.5f\n", fsum);
    double dsum2 = 0;
    for (int k = 1; k <= 10; k++) dsum2 += 1.0 / k;
    printf("harmonic double: %.10f\n", dsum2);
    printf("dsum=%.1f\n", dsum(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
    printf("fmix=%.2f\n", fmix(1, 2.5f, 3, 4.25, 'A', 0.5f));
    printf("halve=%.3f\n", halve(7));
    double arr[4] = {1.5, 2.5, 3.5, 4.5};
    double total = 0;
    for (int k = 0; k < 4; k++) total += arr[k];
    printf("total=%.2f\n", total);
    printf("sqrt(2)=%.6f pow=%.1f\n", sqrt(2.0), pow(2.0, 10.0));
    printf("floor/ceil: %.1f %.1f %.1f %.1f\n", floor(2.7), ceil(2.2), floor(-2.7), ceil(-2.2));
    printf("fabs: %.2f %.2f\n", fabs(-3.25), fabs(3.25));
    double zero = 0.0;
    double nan_v = zero / zero;
    double inf_v = 1.0 / zero;
    printf("nan: %d %d inf: %d %d\n", nan_v != nan_v, nan_v == nan_v, inf_v > 1e308, -inf_v < -1e308);
    printf("nan compares: %d %d %d\n", nan_v < 1.0, nan_v > 1.0, nan_v <= nan_v);
    float tiny = 1e-30f;
    printf("tiny*tiny=%g\n", (double)(tiny * tiny));
    printf("consts: %.8f %.3e %g\n", 3.14159265358979, 6.02e23, 0.0001);
    double hexf = 0x1.8p1;
    printf("hexfloat=%.1f\n", hexf);
    float fl = 1.1f;
    double dl = fl;
    printf("float widening: %d\n", dl == 1.1);
    printf("float narrowing: %d\n", (float)1.1 == fl);
    long double ld = 2.5;
    printf("long double: %.2f\n", (double)(ld * 2));
    int cond = (d > e) ? 10 : 20;
    double dcond = (e > d) ? 1.5 : 2.5;
    printf("cond: %d %.1f\n", cond, dcond);
    double acc = 0;
    int n = 0;
    while (acc < 10.0) {
        acc += 0.75;
        n++;
    }
    printf("loop: %d %.2f\n", n, acc);
    d += 1;
    d *= 2;
    d -= 0.5;
    d /= 4;
    printf("compound: %.4f\n", d);
    f++;
    --f;
    f += 0.5f;
    printf("f=%.2f\n", f);
    return 0;
}
