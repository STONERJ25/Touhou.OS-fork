/* declarators, typedefs, enums, qualifiers, tricky syntax */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

typedef unsigned char byte;
typedef int (*op_t)(int, int);
typedef int arr3_t[3];
typedef struct node node_t;
struct node {
    int value;
    node_t *next;
};
typedef struct {
    int w, h;
} size_t2;
typedef enum { RED, GREEN = 5, BLUE, LAST = BLUE + 10 } color_t;
enum shape { CIRCLE = 1, SQUARE = 2, TRIANGLE = 4 };

typedef int (*(*fnptr_ret)(int))(char);

static int add(int a, int b) { return a + b; }
static int (*select_op(int which))(int, int) { return which ? add : 0; }

static int apply_n(int (*f)(int), int n, int x) {
    while (n--) x = f(x);
    return x;
}
static int dbl(int x) { return x * 2; }

int (*func_table[3])(int) = {dbl, dbl, dbl};
int *(*ptr_func)(void);
char *(*arr_of_funcptr[2])(const char *);
int (*(*complex_decl)[3])(void);

static const int ci = 7;
static volatile int vi = 8;
static int *const cp = 0;
static const char *const ccs = "const";
static int const *p_to_const;
static char buf[2][3][4];

static byte b = 255;
static arr3_t a3 = {1, 2, 3};
static op_t op = add;

struct S {
    int a;
    union {
        int i;
        float f;
    } u;
    int (*cb)(int);
    int arr[2];
};

int main(void) {
    printf("byte=%d a3=%d%d%d op=%d\n", b, a3[0], a3[1], a3[2], op(3, 4));
    printf("sizes: byte=%d arr3_t=%d op_t=%d node_t=%d size_t2=%d color_t=%d\n", (int)sizeof(byte), (int)sizeof(arr3_t), (int)sizeof(op_t), (int)sizeof(node_t), (int)sizeof(size_t2), (int)sizeof(color_t));
    printf("enum: %d %d %d %d | %d %d %d\n", RED, GREEN, BLUE, LAST, CIRCLE, SQUARE, TRIANGLE);
    enum shape sh = SQUARE | TRIANGLE;
    printf("shape=%d\n", sh);
    color_t col = BLUE;
    col++;
    printf("col=%d\n", col);
    printf("select_op: %d %d\n", select_op(1)(5, 6), select_op(0) == 0);
    printf("apply_n=%d\n", apply_n(dbl, 5, 1));
    printf("func_table=%d\n", func_table[2](21));
    printf("sizes: %d %d %d %d\n", (int)sizeof(func_table), (int)sizeof(arr_of_funcptr), (int)sizeof(complex_decl), (int)sizeof(buf));
    printf("buf dims: %d %d %d\n", (int)sizeof(buf[0]), (int)sizeof(buf[0][0]), (int)(sizeof(buf) / sizeof(buf[0])));
    printf("const: %d %d %s %d\n", ci, vi, ccs, p_to_const == 0);
    struct S s = {1, {2}, dbl, {3, 4}};
    printf("S: %d %d %d %d %d size=%d\n", s.a, s.u.i, s.cb(s.arr[0]), s.arr[1], (int)offsetof(struct S, cb), (int)sizeof(struct S));
    node_t n2 = {2, 0}, n1 = {1, &n2};
    printf("list: %d %d\n", n1.value, n1.next->value);
    int x = 5;
    int *px = &x;
    int **ppx = &px;
    int ***pppx = &ppx;
    ***pppx = 6;
    printf("x=%d\n", x);
    int (*pa)[3] = &a3;
    printf("(*pa)[1]=%d\n", (*pa)[1]);
    char c = 'a', *pc = &c, **ppc = &pc;
    printf("%c\n", **ppc);
    uint8_t u8 = 200;
    int16_t i16 = -300;
    uint32_t u32 = 4000000000u;
    int64_t i64 = -5000000000LL;
    printf("%d %d %u %lld %d\n", u8, i16, u32, (long long)i64, (int)sizeof(intptr_t));
    typedef int local_int;
    local_int li = 9;
    printf("local typedef %d\n", li);
    struct { int a, b; } anon = {1, 2};
    printf("anon %d\n", anon.a + anon.b);
    long long ll = 1LL << 40;
    unsigned long ul = ~0UL;
    printf("%lld %lu\n", ll, ul);
    int multi_a = 1, multi_b[2] = {2, 3}, *multi_c = &multi_a;
    printf("multi %d %d %d\n", multi_a, multi_b[1], *multi_c);
    return 0;
}
