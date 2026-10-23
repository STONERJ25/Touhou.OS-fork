/* preprocessor behaviour visible at run time */
#include <stdio.h>
#include <string.h>

#define STR(x) #x
#define XSTR(x) STR(x)
#define CAT(a, b) a##b
#define XCAT(a, b) CAT(a, b)
#define VERSION 3
#define NAME "mcc"
#define SQUARE(x) ((x) * (x))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define LOG(fmt, ...) printf("[log] " fmt "\n", ##__VA_ARGS__)
#define COUNT(...) (sizeof((int[]){__VA_ARGS__}) / sizeof(int))
#define FIELD(n) int field_##n
#define DECLARE(type, name) type name##_var = 0

struct holder {
    FIELD(a);
    FIELD(b);
};

DECLARE(int, alpha);
DECLARE(long, beta);

#define EMPTY
#define PAREN ()
#define FUNC(x) (x + 1)
#define APPLY(f, x) f(x)

#ifdef VERSION
#if VERSION >= 3 && defined(NAME) && !defined(MISSING)
#define FEATURE 1
#else
#define FEATURE 2
#endif
#endif

#if 0
#error never compiled
#elif FEATURE == 1
#define PICKED "elif branch"
#else
#define PICKED "else branch"
#endif

#undef VERSION
#define VERSION 4

#define LINE_OF() __LINE__

int main(void) {
    printf("%s %s\n", STR(hello world), XSTR(VERSION));
    printf("%s\n", XSTR(CAT(foo, bar)));
    int CAT(my, var) = 5;
    printf("myvar=%d\n", myvar);
    printf("%d %d\n", SQUARE(3 + 1), MAX(SQUARE(2), 3));
    LOG("no args");
    LOG("one arg %d", 42);
    LOG("two args %d %s", 1, "two");
    printf("COUNT=%d %d\n", (int)COUNT(1, 2, 3), (int)COUNT(7));
    struct holder h = {1, 2};
    printf("holder %d %d\n", h.field_a, h.field_b);
    printf("%d %ld\n", alpha_var, beta_var);
    printf("%d\n", APPLY(FUNC, 4));
    printf("%s\n", PICKED);
    printf("%d\n", VERSION);
    printf("%s:%d\n", "pp.c", LINE_OF());
    printf("file ends with pp.c: %d\n", strstr(__FILE__, "pp.c") != NULL);
    printf("stdc=%d version=%ld\n", __STDC__, __STDC_VERSION__);
    printf("%s\n", "adjacent" " string" " literals");
    printf("%d\n", (int)sizeof("abc" "def"));
    printf("%s\n", XSTR(a   b    c));
    printf("%s\n", STR("quoted"));
    printf("%s\n", STR('c'));
    printf("%d %d\n", __COUNTER__, __COUNTER__);
#define LOCAL 10
    printf("%d\n", LOCAL);
#undef LOCAL
#ifdef LOCAL
    printf("LOCAL still defined\n");
#else
    printf("LOCAL undefined\n");
#endif
    return 0;
}
