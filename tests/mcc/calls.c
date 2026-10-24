/* calling convention torture: many arguments, mixed classes, callbacks, recursion, struct passing */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long ten(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j) {
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10;
}

static double twelve_d(double a, double b, double c, double d, double e, double f, double g, double h, double i, double j, double k, double l) {
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10 + k * 11 + l * 12;
}

static double interleaved(int a, double b, int c, double d, int e, double f, int g, double h, int i, double j, int k, double l, int m, double n, int o, double p, int q, double r) {
    return a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p + q + r;
}

static char byte_args(char a, unsigned char b, short c, unsigned short d, signed char e, _Bool f) {
    return (char)(a + b + c + d + e + f);
}

static unsigned char ret_uchar(int v) { return (unsigned char)v; }
static signed char ret_schar(int v) { return (signed char)v; }
static short ret_short(int v) { return (short)v; }
static _Bool ret_bool(int v) { return v; }
static float ret_float(float a, float b) { return a * b; }
static void *ret_ptr(void *p) { return p; }

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

typedef int (*callback)(int, void *);

static int each(int *arr, int n, callback cb, void *ctx) {
    int total = 0;
    for (int i = 0; i < n; i++) total += cb(arr[i], ctx);
    return total;
}

static int add_ctx(int v, void *ctx) { return v + *(int *)ctx; }

static long long fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }

static int depth_sum(int n) { return n == 0 ? 0 : n + depth_sum(n - 1); }

struct s16 {
    long a, b;
};
struct s24 {
    long a, b, c;
};
struct sf {
    double x;
    long y;
};
struct sg {
    float a, b;
    long c;
};

static struct s16 id16(struct s16 s) { return s; }
static struct s24 id24(struct s24 s) {
    s.a++;
    return s;
}
static struct sf idf(struct sf s) {
    s.x *= 2;
    return s;
}
static struct sg idg(struct sg s) {
    s.a += 1;
    s.b += 2;
    s.c += 3;
    return s;
}
static long args_mix(struct s16 a, int x, struct s24 b, double y, struct sf c, struct sg d, long z) {
    return a.a + a.b + x + b.a + b.b + b.c + (long)y + (long)c.x + c.y + (long)d.a + (long)d.b + d.c + z;
}

static int through_ptr(int (*f)(int), int v) { return f(v) + 1; }
static int inc(int x) { return x + 1; }
static int (*get_fn(int which))(int) { return which ? inc : 0; }

int main(void) {
    printf("ten=%ld\n", ten(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
    printf("twelve_d=%.1f\n", twelve_d(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12));
    printf("interleaved=%.1f\n", interleaved(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18));
    printf("byte_args=%d\n", byte_args(100, 200, -300, 60000, -5, 1));
    printf("rets: %d %d %d %d %d %.2f\n", ret_uchar(300), ret_schar(200), ret_short(70000), ret_bool(5), ret_bool(0), ret_float(1.5f, 4.0f));
    int target = 5;
    printf("ret_ptr: %d\n", ret_ptr(&target) == &target);

    int arr[] = {5, 3, 9, 1, 7, 2, 8};
    qsort(arr, 7, sizeof(int), cmp_int);
    for (int i = 0; i < 7; i++) printf("%d ", arr[i]);
    printf("\n");
    const char *strs[] = {"pear", "apple", "fig", "banana"};
    qsort(strs, 4, sizeof(char *), cmp_str);
    for (int i = 0; i < 4; i++) printf("%s ", strs[i]);
    printf("\n");
    int ctx = 100;
    printf("each=%d\n", each(arr, 7, add_ctx, &ctx));
    printf("fact(20)=%lld depth_sum(1000)=%d\n", fact(20), depth_sum(1000));

    struct s16 a16 = {1, 2};
    struct s24 a24 = {10, 20, 30};
    struct sf af = {1.5, 7};
    struct sg ag = {0.5f, 1.5f, 9};
    a16 = id16(a16);
    a24 = id24(a24);
    af = idf(af);
    ag = idg(ag);
    printf("id: %ld %ld | %ld %ld %ld | %.1f %ld | %.1f %.1f %ld\n", a16.a, a16.b, a24.a, a24.b, a24.c, af.x, af.y, ag.a, ag.b, ag.c);
    printf("args_mix=%ld\n", args_mix(a16, 3, a24, 4.5, af, ag, 1000));
    printf("fn ptrs: %d %d %d\n", through_ptr(inc, 4), get_fn(1)(10), get_fn(0) == 0);
    printf("nested calls: %ld\n", ten(fact(3), fact(4), 1, 2, 3, 4, 5, 6, depth_sum(3), ret_short(2)));
    printf("abs/labs: %d %ld\n", abs(-5), labs(-6000000000L));
    return 0;
}
