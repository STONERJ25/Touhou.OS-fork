/* A handful of small kernels for comparing the speed of mcc's code with gcc's (tools/mcc_bench.sh).  Each prints its name, its time in
 * milliseconds and a checksum; the checksums must be the same for every compiler. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ---- integer sort: quicksort of 1.5 million pseudo-random ints */
static void qs(int *a, int lo, int hi) {
    while (lo < hi) {
        int p = a[(lo + hi) / 2], i = lo, j = hi;
        while (i <= j) {
            while (a[i] < p) i++;
            while (a[j] > p) j--;
            if (i <= j) {
                int t = a[i];
                a[i] = a[j];
                a[j] = t;
                i++;
                j--;
            }
        }
        if (j - lo < hi - i) {
            qs(a, lo, j);
            lo = i;
        } else {
            qs(a, i, hi);
            hi = j;
        }
    }
}

static unsigned long long k_sort(void) {
    int n = 1500000;
    int *a = malloc((size_t)n * sizeof *a);
    unsigned s = 12345;
    for (int i = 0; i < n; i++) {
        s = s * 1103515245u + 12345u;
        a[i] = (int)(s >> 8);
    }
    qs(a, 0, n - 1);
    unsigned long long h = 0;
    for (int i = 0; i < n; i += 997) h = h * 31 + (unsigned)a[i];
    free(a);
    return h;
}

/* ---- sieve of Eratosthenes to 20 million */
static unsigned long long k_sieve(void) {
    int n = 20000000;
    unsigned char *c = calloc((size_t)n + 1, 1);
    unsigned long long count = 0;
    for (int i = 2; i <= n; i++) {
        if (c[i]) continue;
        count++;
        for (long long j = (long long)i * i; j <= n; j += i) c[j] = 1;
    }
    free(c);
    return count;
}

/* ---- SHA-256 of 24 MiB */
static unsigned rotr(unsigned x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha_block(unsigned *h, const unsigned char *p) {
    static const unsigned K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74,
        0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d,
        0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e,
        0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
        0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    unsigned w[64];
    for (int i = 0; i < 16; i++) w[i] = (unsigned)p[4 * i] << 24 | (unsigned)p[4 * i + 1] << 16 | (unsigned)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        unsigned s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    unsigned a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        unsigned S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25), ch = (e & f) ^ (~e & g), t1 = hh + S1 + ch + K[i] + w[i];
        unsigned S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static unsigned long long k_sha(void) {
    unsigned h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char block[64];
    for (int i = 0; i < 64; i++) block[i] = (unsigned char)i;
    for (int n = 0; n < 24 * 1024 * 1024 / 64; n++) {
        block[0] = (unsigned char)n;
        sha_block(h, block);
    }
    return ((unsigned long long)h[0] << 32) | h[1];
}

/* ---- floating point: a Mandelbrot grid */
static unsigned long long k_mandel(void) {
    unsigned long long inside = 0;
    for (int py = 0; py < 300; py++)
        for (int px = 0; px < 400; px++) {
            double x0 = px / 400.0 * 3.5 - 2.5, y0 = py / 300.0 * 2.0 - 1.0, x = 0, y = 0;
            int it = 0;
            while (x * x + y * y <= 4.0 && it < 200) {
                double t = x * x - y * y + x0;
                y = 2 * x * y + y0;
                x = t;
                it++;
            }
            inside += (unsigned long long)it;
        }
    return inside;
}

/* ---- strings and structures: a hash table of 400,000 formatted keys */
typedef struct Node {
    char key[24];
    int val;
    struct Node *next;
} Node;

static unsigned long long k_hash(void) {
    enum { B = 1 << 16 };
    Node **tab = calloc(B, sizeof *tab);
    unsigned long long sum = 0;
    for (int i = 0; i < 400000; i++) {
        char key[24];
        snprintf(key, sizeof key, "key-%d-%d", i % 100003, i % 7);
        unsigned h = 5381;
        for (const char *p = key; *p; p++) h = h * 33 + (unsigned char)*p;
        Node **pp = &tab[h % B];
        while (*pp && strcmp((*pp)->key, key)) pp = &(*pp)->next;
        if (!*pp) {
            *pp = calloc(1, sizeof(Node));
            strcpy((*pp)->key, key);
        }
        (*pp)->val += i;
        sum += (unsigned)(*pp)->val;
    }
    return sum;
}

int main(int argc, char **argv) {
    static const struct {
        const char *name;
        unsigned long long (*fn)(void);
    } kernels[] = {{"sort", k_sort}, {"sieve", k_sieve}, {"sha256", k_sha}, {"mandel", k_mandel}, {"hash", k_hash}};
    for (size_t i = 0; i < sizeof kernels / sizeof kernels[0]; i++) {
        if (argc > 1 && strcmp(argv[1], kernels[i].name)) continue;
        double t0 = now_ms();
        unsigned long long sum = kernels[i].fn();
        printf("%-7s %8.0f ms  %016llx\n", kernels[i].name, now_ms() - t0, sum);
    }
    return 0;
}
