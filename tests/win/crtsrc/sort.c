/* qsort and the heap: 20000 pseudo-random numbers in a calloc'd array, grown with realloc, sorted, checked, with a checksum; then the arguments sorted. */
#include "crt.h"

static int by_value(const void *a, const void *b) {
    unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
    return x < y ? -1 : x > y;
}
static int by_string(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(int argc, char **argv) {
    size_t n = 10000;
    unsigned *v = calloc(n, sizeof *v);
    unsigned seed = 1;
    for (size_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; v[i] = seed >> 8; }
    v = realloc(v, 2 * n * sizeof *v);
    for (size_t i = n; i < 2 * n; i++) { seed = seed * 1103515245u + 12345u; v[i] = seed >> 8; }
    n *= 2;
    qsort(v, n, sizeof *v, by_value);
    unsigned long long sum = 0;
    for (size_t i = 0; i < n; i++) {
        if (i && v[i - 1] > v[i]) { printf("not sorted at %zu\n", i); return 1; }
        sum = sum * 31 + v[i];
    }
    printf("sorted %zu numbers: min %u max %u checksum %llx\n", n, v[0], v[n - 1], sum);
    free(v);
    qsort(argv + 1, (size_t)(argc - 1), sizeof *argv, by_string);
    for (int i = 1; i < argc; i++) printf("%s%s", argv[i], i + 1 < argc ? " " : "\n");
    return 0;
}
