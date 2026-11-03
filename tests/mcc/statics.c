/* static and global data: initializers with relocations, tentative definitions, static locals, string tables */
#include <stdio.h>
#include <string.h>

int tentative;
int tentative;
static int counter = 5;
const int table[] = {1, 1, 2, 3, 5, 8, 13, 21};
static const char *const words[] = {"alpha", "beta", "gamma", "delta"};
char message[] = "hello, \"world\"\n\ttab\\ \x41\102\0end";
char fixed[10] = "abc";
short shorts[] = {1, -2, 3};
double doubles[] = {1.5, -2.25, 1e10};
float floats[3] = {0.5f, 1.5f};
long long big_value = 0x7fffffffffffffffLL;
unsigned char bytes[] = {0xff, 0x80, 0x7f, 256 - 1};

struct entry {
    const char *name;
    int value;
    int (*fn)(int);
};

static int twice(int x) { return x * 2; }
static int square(int x) { return x * x; }

static struct entry entries[] = {
    {"twice", 1, twice},
    {"square", 2, square},
    {"null", 3, 0},
};

struct node {
    int v;
    struct node *next;
};
static struct node n3 = {3, 0};
static struct node n2 = {2, &n3};
static struct node n1 = {1, &n2};
struct node *head = &n1;

int *ptr_to_table_elem = (int *)&table[3];
const char *msg_ptr = message + 7;
int array_of_ptrs_target[2][3] = {{1, 2, 3}, {4, 5, 6}};
int (*row_ptr)[3] = array_of_ptrs_target;
void *vptr = &counter;
long addr_arith = (long)&table[2] - (long)&table[0];
char nested_str[2][8] = {"ab", "cdefg"};

union u {
    int i;
    char c[4];
} un = {0x41424344};

struct bits {
    unsigned a : 4, b : 4;
    int c;
} bs = {3, 12, 7};

struct with_arr {
    int n;
    char name[8];
    short dims[2][2];
} wa = {5, "name", {{1, 2}, {3, 4}}};

static int next_id(void) {
    static int id = 100;
    return id++;
}

static int *static_ptr(void) {
    static int storage[3] = {7, 8, 9};
    return storage;
}

int counter_user(void) { return ++counter; }

_Thread_local int tl_counter = 10;
_Thread_local char tl_buf[4];

int main(void) {
    printf("tentative=%d counter=%d\n", tentative, counter);
    int sum = 0;
    for (int i = 0; i < (int)(sizeof(table) / sizeof(table[0])); i++) sum += table[i];
    printf("table sum=%d count=%d\n", sum, (int)(sizeof(table) / sizeof(*table)));
    for (int i = 0; i < 4; i++) printf("%s%c", words[i], i == 3 ? '\n' : ',');
    printf("message[%d]: ", (int)sizeof(message));
    for (size_t i = 0; i < sizeof(message); i++) printf("%d.", message[i]);
    printf("\n");
    printf("fixed=%s last=%d\n", fixed, fixed[9]);
    printf("shorts=%d %d %d doubles=%.2f %.2f %.1e floats=%.1f %.1f %.1f\n", shorts[0], shorts[1], shorts[2], doubles[0], doubles[1], doubles[2], floats[0], floats[1], floats[2]);
    printf("big=%lld bytes=%d %d %d %d\n", big_value, bytes[0], bytes[1], bytes[2], bytes[3]);
    for (int i = 0; i < 3; i++) printf("%s(%d)=%d ", entries[i].name, entries[i].value, entries[i].fn ? entries[i].fn(7) : -1);
    printf("\n");
    int total = 0;
    for (struct node *n = head; n; n = n->next) total = total * 10 + n->v;
    printf("list=%d\n", total);
    printf("ptr_to_table_elem=%d msg_ptr=%.5s row_ptr[1][2]=%d vptr==&counter:%d addr_arith=%ld\n", *ptr_to_table_elem, msg_ptr, row_ptr[1][2], vptr == (void *)&counter, addr_arith);
    printf("nested_str: %s %s\n", nested_str[0], nested_str[1]);
    printf("union: %c%c%c%c\n", un.c[3], un.c[2], un.c[1], un.c[0]);
    printf("bits: %d %d %d\n", bs.a, bs.b, bs.c);
    printf("wa: %d %s %d %d %d %d\n", wa.n, wa.name, wa.dims[0][0], wa.dims[0][1], wa.dims[1][0], wa.dims[1][1]);
    printf("ids: %d %d %d\n", next_id(), next_id(), next_id());
    int *sp = static_ptr();
    sp[1] = 80;
    printf("static storage: %d %d %d\n", static_ptr()[0], static_ptr()[1], static_ptr()[2]);
    printf("counter_user: %d %d\n", counter_user(), counter_user());
    tl_counter += 5;
    tl_buf[2] = 'x';
    printf("tls: %d %d\n", tl_counter, tl_buf[2]);
    static const char hello[] = "static local";
    printf("%s %d\n", hello, (int)sizeof(hello));
    printf("__func__=%s\n", __func__);
    printf("__LINE__=%d\n", __LINE__);
    return 0;
}
