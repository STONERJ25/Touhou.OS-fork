/*
 * A program with more in it than hello (tests/case_linux_py.py, linux_py_gcc_*): structures, function pointers, recursion, floating point and the math library, string handling,
 * variadic functions, bit twiddling, a qsort comparison, static tables and switch statements, so that the compiler's optimizers and the assembler have something to do. It prints
 * only what the language promises. The same source is compiled by the guest and by the oracle, and the object files, the assembly text and the output of the linked program are
 * compared.
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Node {
    int key;
    struct Node *left, *right;
} Node;

static Node *insert(Node *n, int key) {
    if (!n) {
        n = calloc(1, sizeof *n);
        n->key = key;
        return n;
    }
    if (key < n->key) n->left = insert(n->left, key);
    else if (key > n->key) n->right = insert(n->right, key);
    return n;
}

static int height(const Node *n) { return n ? 1 + (height(n->left) > height(n->right) ? height(n->left) : height(n->right)) : 0; }

static void walk(const Node *n, void (*visit)(int, void *), void *ctx) {
    if (!n) return;
    walk(n->left, visit, ctx);
    visit(n->key, ctx);
    walk(n->right, visit, ctx);
}

static void collect(int key, void *ctx) { *(*(int **)ctx)++ = key; }

static void release(Node *n) {
    if (!n) return;
    release(n->left);
    release(n->right);
    free(n);
}

static int compare_desc(const void *a, const void *b) { return *(const int *)b - *(const int *)a; }

static uint32_t crc32_bits(const unsigned char *p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

static int format_into(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

static const char *classify(int c) {
    switch (c) {
    case 0: return "zero";
    case 1: case 2: case 3: return "small";
    case 4: case 5: case 6: case 7: return "medium";
    default: return c < 0 ? "negative" : "large";
    }
}

static double series(int terms) {
    double pi4 = 0;
    for (int k = 0; k < terms; k++) pi4 += (k % 2 ? -1.0 : 1.0) / (2 * k + 1);
    return 4 * pi4;
}

int main(void) {
    int keys[] = { 50, 30, 70, 20, 40, 60, 80, 35, 45, 65, 10, 5, 90, 85, 95 };
    Node *root = NULL;
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) root = insert(root, keys[i]);
    int sorted[16], *out = sorted;
    walk(root, collect, &out);
    printf("tree: %d nodes, height %d, in order:", (int)(out - sorted), height(root));
    for (int *p = sorted; p < out; p++) printf(" %d", *p);
    printf("\n");
    qsort(sorted, (size_t)(out - sorted), sizeof sorted[0], compare_desc);
    printf("descending: %d %d %d ... %d\n", sorted[0], sorted[1], sorted[2], sorted[14]);
    release(root);

    unsigned char data[256];
    for (int i = 0; i < 256; i++) data[i] = (unsigned char)(i * 7 + 3);
    printf("crc32 of the table: %08x\n", crc32_bits(data, sizeof data));
    printf("popcount/clz/ctz: %d %d %d\n", __builtin_popcount(0xF0F0u), __builtin_clz(1u << 20), __builtin_ctz(1u << 11));

    char line[64];
    int n = format_into(line, sizeof line, "%s-%03d-%.3f-%c-%x", "tag", 7, 2.5, 'z', 255);
    printf("format: [%s] %d\n", line, n);
    char upper[32];
    const char *src = "Compiled and Linked";
    for (size_t i = 0; i <= strlen(src); i++) upper[i] = (char)toupper((unsigned char)src[i]);
    printf("%s | %s\n", upper, classify(5));
    for (int c = -1; c < 10; c += 3) printf("classify(%d) = %s\n", c, classify(c));

    printf("series(1000) = %.6f, sqrt(2) = %.9f, cbrt(27) = %.1f, pow(2, 0.5) = %.9f\n", series(1000), sqrt(2.0), cbrt(27.0), pow(2.0, 0.5));
    printf("limits: %d %u %lld %zu\n", INT_MAX, UINT_MAX, (long long)LLONG_MIN, sizeof(long double));
    errno = 0;
    long v = strtol("99999999999999999999", NULL, 10);
    printf("strtol overflow: %d, errno is ERANGE: %d\n", v == LONG_MAX, errno == ERANGE);
    return 0;
}
