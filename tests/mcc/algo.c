/* classic algorithms: sorting, searching, number theory, matrices, backtracking */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 24

static void bubble(int *a, int n) {
    for (int i = 0; i < n - 1; i++)
        for (int j = 0; j < n - 1 - i; j++)
            if (a[j] > a[j + 1]) {
                int t = a[j];
                a[j] = a[j + 1];
                a[j + 1] = t;
            }
}

static void insertion(int *a, int n) {
    for (int i = 1; i < n; i++) {
        int v = a[i], j = i - 1;
        while (j >= 0 && a[j] > v) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = v;
    }
}

static void quick(int *a, int lo, int hi) {
    if (lo >= hi) return;
    int p = a[(lo + hi) / 2], i = lo, j = hi;
    while (i <= j) {
        while (a[i] < p) i++;
        while (a[j] > p) j--;
        if (i <= j) {
            int t = a[i];
            a[i++] = a[j];
            a[j--] = t;
        }
    }
    quick(a, lo, j);
    quick(a, i, hi);
}

static void merge_sort(int *a, int *tmp, int n) {
    if (n < 2) return;
    int mid = n / 2;
    merge_sort(a, tmp, mid);
    merge_sort(a + mid, tmp, n - mid);
    int i = 0, j = mid, k = 0;
    while (i < mid && j < n) tmp[k++] = a[i] <= a[j] ? a[i++] : a[j++];
    while (i < mid) tmp[k++] = a[i++];
    while (j < n) tmp[k++] = a[j++];
    memcpy(a, tmp, (size_t)n * sizeof(int));
}

static void sift(int *a, int start, int end) {
    int root = start;
    while (root * 2 + 1 <= end) {
        int child = root * 2 + 1;
        if (child + 1 <= end && a[child] < a[child + 1]) child++;
        if (a[root] >= a[child]) return;
        int t = a[root];
        a[root] = a[child];
        a[child] = t;
        root = child;
    }
}

static void heap_sort(int *a, int n) {
    for (int s = (n - 2) / 2; s >= 0; s--) sift(a, s, n - 1);
    for (int e = n - 1; e > 0; e--) {
        int t = a[0];
        a[0] = a[e];
        a[e] = t;
        sift(a, 0, e - 1);
    }
}

static int bsearch_idx(const int *a, int n, int key) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (a[mid] == key) return mid;
        if (a[mid] < key) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static unsigned rng_state = 12345;
static unsigned rng(void) {
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) & 0x7fff;
}

static void fill(int *a, int n) {
    for (int i = 0; i < n; i++) a[i] = (int)(rng() % 100);
}

static void show(const char *name, const int *a, int n) {
    printf("%-10s", name);
    for (int i = 0; i < n; i++) printf(" %d", a[i]);
    printf("\n");
}

static int queens_count;
static int cols[8], d1[16], d2[16];
static void queens(int row) {
    if (row == 8) {
        queens_count++;
        return;
    }
    for (int c = 0; c < 8; c++) {
        if (cols[c] || d1[row + c] || d2[row - c + 7]) continue;
        cols[c] = d1[row + c] = d2[row - c + 7] = 1;
        queens(row + 1);
        cols[c] = d1[row + c] = d2[row - c + 7] = 0;
    }
}

static unsigned long long modpow(unsigned long long b, unsigned long long e, unsigned long long m) {
    unsigned long long r = 1;
    b %= m;
    while (e) {
        if (e & 1) r = r * b % m;
        b = b * b % m;
        e >>= 1;
    }
    return r;
}

static long lcm(long a, long b) {
    long x = a, y = b;
    while (y) {
        long t = x % y;
        x = y;
        y = t;
    }
    return a / x * b;
}

static void to_base(unsigned long n, int base, char *out) {
    char tmp[70];
    int len = 0;
    if (n == 0) tmp[len++] = '0';
    while (n) {
        tmp[len++] = "0123456789abcdefghijklmnopqrstuvwxyz"[n % (unsigned)base];
        n /= (unsigned)base;
    }
    for (int i = 0; i < len; i++) out[i] = tmp[len - 1 - i];
    out[len] = 0;
}

static void matmul(double a[3][3], double b[3][3], double c[3][3]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double s = 0;
            for (int k = 0; k < 3; k++) s += a[i][k] * b[k][j];
            c[i][j] = s;
        }
}

int main(void) {
    int base[N], a[N], tmp[N];
    fill(base, N);
    memcpy(a, base, sizeof base);
    show("input", a, N);
    memcpy(a, base, sizeof base);
    bubble(a, N);
    show("bubble", a, N);
    memcpy(a, base, sizeof base);
    insertion(a, N);
    show("insertion", a, N);
    memcpy(a, base, sizeof base);
    quick(a, 0, N - 1);
    show("quick", a, N);
    memcpy(a, base, sizeof base);
    merge_sort(a, tmp, N);
    show("merge", a, N);
    memcpy(a, base, sizeof base);
    heap_sort(a, N);
    show("heap", a, N);
    printf("bsearch: %d %d %d\n", bsearch_idx(a, N, a[7]) >= 0, bsearch_idx(a, N, 1000), bsearch_idx(a, N, a[0]) >= 0);

    queens(0);
    printf("8 queens: %d solutions\n", queens_count);

    char sieve[200];
    memset(sieve, 1, sizeof sieve);
    sieve[0] = sieve[1] = 0;
    for (int i = 2; i * i < 200; i++)
        if (sieve[i])
            for (int j = i * i; j < 200; j += i) sieve[j] = 0;
    int primes = 0, last = 0;
    for (int i = 0; i < 200; i++)
        if (sieve[i]) {
            primes++;
            last = i;
        }
    printf("primes below 200: %d, last %d\n", primes, last);

    printf("modpow: %llu %llu\n", modpow(2, 100, 1000000007ULL), modpow(3, 1000000, 998244353ULL));
    printf("lcm(1..20)=%ld\n", ({ long l = 1; for (int i = 2; i <= 20; i++) l = lcm(l, i); l; }));
    char buf[72];
    to_base(255, 2, buf);
    printf("255 base2=%s ", buf);
    to_base(255, 16, buf);
    printf("base16=%s ", buf);
    to_base(123456789, 36, buf);
    printf("base36=%s\n", buf);

    double m1[3][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}}, m2[3][3] = {{9, 8, 7}, {6, 5, 4}, {3, 2, 1}}, m3[3][3];
    matmul(m1, m2, m3);
    for (int i = 0; i < 3; i++) printf("%6.1f %6.1f %6.1f\n", m3[i][0], m3[i][1], m3[i][2]);

    int tri[10][10];
    for (int i = 0; i < 10; i++) {
        tri[i][0] = tri[i][i] = 1;
        for (int j = 1; j < i; j++) tri[i][j] = tri[i - 1][j - 1] + tri[i - 1][j];
    }
    printf("pascal row 9:");
    for (int j = 0; j < 10; j++) printf(" %d", tri[9][j]);
    printf("\n");

    static unsigned long long memo[91];
    memo[1] = 1;
    for (int i = 2; i <= 90; i++) memo[i] = memo[i - 1] + memo[i - 2];
    printf("fib(90)=%llu\n", memo[90]);

    const char *pal[] = {"racecar", "level", "marisa", "a", ""};
    for (int i = 0; i < 5; i++) {
        int len = (int)strlen(pal[i]), ok = 1;
        for (int j = 0; j < len / 2; j++)
            if (pal[i][j] != pal[i][len - 1 - j]) ok = 0;
        printf("%s:%d ", pal[i], ok);
    }
    printf("\n");
    return 0;
}
