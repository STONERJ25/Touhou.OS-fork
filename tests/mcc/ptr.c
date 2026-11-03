/* pointers, arrays, strings, function pointers */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef int (*binop)(int, int);

static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
static int mul(int a, int b) { return a * b; }

static int apply(binop f, int a, int b) { return f(a, b); }

static int sum_array(int *p, int n) {
    int s = 0;
    for (int i = 0; i < n; i++) s += p[i];
    return s;
}

static void swap(int *a, int *b) {
    int t = *a;
    *a = *b;
    *b = t;
}

static char *my_strcpy(char *dst, const char *src) {
    char *r = dst;
    while ((*dst++ = *src++))
        ;
    return r;
}

static int my_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (int)(p - s);
}

int global_arr[4] = {10, 20, 30, 40};
int *global_ptr = &global_arr[2];
const char *names[] = {"zero", "one", "two", "three"};

int main(void) {
    int a[6] = {1, 2, 3, 4, 5, 6};
    int *p = a;
    printf("*p=%d p[2]=%d *(p+3)=%d\n", *p, p[2], *(p + 3));
    p += 2;
    printf("*p=%d p-a=%ld\n", *p, (long)(p - a));
    p++;
    printf("*p=%d\n", *p);
    --p;
    printf("*p=%d\n", *p);
    printf("sum=%d\n", sum_array(a, 6));
    printf("&a[5]-&a[1]=%ld\n", (long)(&a[5] - &a[1]));
    printf("p<&a[4]: %d, p>a: %d\n", p < &a[4], p > a);

    int x = 1, y = 2;
    swap(&x, &y);
    printf("x=%d y=%d\n", x, y);

    int m[3][4];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) m[i][j] = i * 10 + j;
    printf("m[2][3]=%d m[1][0]=%d sizeof(m)=%d sizeof(m[0])=%d\n", m[2][3], m[1][0], (int)sizeof(m), (int)sizeof(m[0]));
    int(*row)[4] = m;
    printf("row[1][2]=%d (*(row+2))[1]=%d\n", row[1][2], (*(row + 2))[1]);
    int *flat = &m[0][0];
    printf("flat[5]=%d flat[11]=%d\n", flat[5], flat[11]);

    char buf[32];
    my_strcpy(buf, "Marisa");
    printf("buf=%s len=%d strlen=%d\n", buf, my_strlen(buf), (int)strlen(buf));
    char *q = buf + 3;
    printf("q=%s *q=%c q[-1]=%c\n", q, *q, q[-1]);
    buf[0] = 'm';
    printf("buf=%s\n", buf);

    binop ops[3] = {add, sub, mul};
    for (int i = 0; i < 3; i++) printf("ops[%d](7,3)=%d\n", i, ops[i](7, 3));
    printf("apply=%d %d\n", apply(add, 20, 22), apply(mul, 6, 7));
    binop f = &sub;
    printf("f(10,4)=%d (*f)(10,4)=%d\n", f(10, 4), (*f)(10, 4));

    printf("global_arr[1]=%d *global_ptr=%d\n", global_arr[1], *global_ptr);
    for (int i = 0; i < 4; i++) printf("%s ", names[i]);
    printf("\n");
    printf("names[2][1]=%c\n", names[2][1]);

    int **pp = &p;
    **pp = 99;
    printf("a[2]=%d\n", a[2]);

    char *heap = malloc(16);
    strcpy(heap, "heap data");
    heap = realloc(heap, 64);
    strcat(heap, " grown");
    printf("%s %d\n", heap, (int)strlen(heap));
    free(heap);

    int *z = NULL;
    printf("null: %d %d\n", z == NULL, !z);
    void *vp = a;
    printf("void*: %d\n", ((int *)vp)[1]);
    const char *s = "abc";
    printf("%d%d%d\n", s[0], s[1], s[2]);
    printf("%c%c\n", "xyz"[1], *("hello" + 4));
    long diff = (char *)&a[3] - (char *)&a[0];
    printf("bytes=%ld\n", diff);
    unsigned long addr = (unsigned long)a;
    printf("aligned=%d\n", (int)(addr % 4));
    return 0;
}
