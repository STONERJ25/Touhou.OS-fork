/* variadic functions: integers, doubles, structs, va_list passing, va_copy, many arguments */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int sum_ints(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int s = 0;
    for (int i = 0; i < n; i++) s += va_arg(ap, int);
    va_end(ap);
    return s;
}

static double sum_doubles(int n, ...) {
    va_list ap;
    va_start(ap, n);
    double s = 0;
    for (int i = 0; i < n; i++) s += va_arg(ap, double);
    va_end(ap);
    return s;
}

static double mixed(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    double total = 0;
    for (const char *p = fmt; *p; p++) {
        switch (*p) {
        case 'i': total += va_arg(ap, int); break;
        case 'l': total += (double)va_arg(ap, long); break;
        case 'd': total += va_arg(ap, double); break;
        case 'p': total += (double)(*va_arg(ap, int *)); break;
        case 'c': total += va_arg(ap, int); break;
        }
    }
    va_end(ap);
    return total;
}

struct S {
    long a, b, c;
};

struct P {
    int x, y;
};

static long sum_structs(int n, ...) {
    va_list ap;
    va_start(ap, n);
    long s = 0;
    for (int i = 0; i < n; i++) {
        struct S v = va_arg(ap, struct S);
        s += v.a + v.b * 10 + v.c * 100;
    }
    va_end(ap);
    return s;
}

static int sum_points(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int s = 0;
    for (int i = 0; i < n; i++) {
        struct P p = va_arg(ap, struct P);
        s += p.x * p.y;
    }
    va_end(ap);
    return s;
}

static int my_vformat(char *buf, size_t n, const char *fmt, va_list ap) { return vsnprintf(buf, n, fmt, ap); }

static int my_format(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = my_vformat(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

static int twice(int n, ...) {
    va_list ap, ap2;
    va_start(ap, n);
    va_copy(ap2, ap);
    int s = 0;
    for (int i = 0; i < n; i++) s += va_arg(ap, int);
    for (int i = 0; i < n; i++) s += va_arg(ap2, int);
    va_end(ap);
    va_end(ap2);
    return s;
}

static void logf_(const char *tag, const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("[%s] %s\n", tag, buf);
}

static long late(long a, long b, long c, long d, long e, long f, ...) {
    va_list ap;
    va_start(ap, f);
    long s = a + b + c + d + e + f;
    s += va_arg(ap, long);
    s += va_arg(ap, long);
    s += (long)va_arg(ap, double);
    va_end(ap);
    return s;
}

#define CALL_COUNT(...) count_args(__VA_ARGS__, 0)
static int count_args(int first, ...) {
    va_list ap;
    va_start(ap, first);
    int n = 0, v = first;
    while (v) {
        n++;
        v = va_arg(ap, int);
    }
    va_end(ap);
    return n;
}

int main(void) {
    printf("sum_ints: %d %d %d\n", sum_ints(0), sum_ints(3, 1, 2, 3), sum_ints(10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
    printf("sum_doubles: %.2f %.2f\n", sum_doubles(2, 1.5, 2.5), sum_doubles(10, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0));
    int five = 5;
    printf("mixed: %.2f\n", mixed("ildpc", 1, 2L, 3.5, &five, 'A'));
    printf("mixed many: %.1f\n", mixed("iiiiiiiiiidddddddddd", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0));
    struct S s1 = {1, 2, 3}, s2 = {4, 5, 6};
    printf("structs: %ld\n", sum_structs(2, s1, s2));
    struct P p1 = {2, 3}, p2 = {4, 5};
    printf("points: %d\n", sum_points(2, p1, p2));
    char buf[64];
    int r = my_format(buf, sizeof buf, "%d-%s-%c-%5.2f-%ld", 42, "str", 'x', 3.14159, 123456789012L);
    printf("my_format: %s (%d)\n", buf, r);
    printf("twice: %d\n", twice(4, 1, 2, 3, 4));
    logf_("tag", "%s has %d items costing %.2f", "cart", 3, 9.5);
    printf("late: %ld\n", late(1, 2, 3, 4, 5, 6, 7L, 8L, 9.9));
    printf("count: %d %d\n", CALL_COUNT(1), CALL_COUNT(1, 2, 3, 4));
    snprintf(buf, sizeof buf, "%s|%5d|%-5d|%05d|%x|%X|%o|%c|%%", "abc", 42, 42, 42, 255, 255, 8, 'q');
    printf("snprintf: %s\n", buf);
    return 0;
}
