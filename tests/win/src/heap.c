/* The process heap and VirtualAlloc: a few thousand blocks with patterns written and checked, grown and shrunk with HeapReAlloc, measured with HeapSize;
 * a 1 MiB VirtualAlloc'd region written and read; the clock functions move forward. Prints "heap ok" and exits 0, or names the first failure and exits 1. */
#include "win.h"

#define N 512
static unsigned char *blk[N];
static SIZE_T sz[N];
static unsigned rng = 12345;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static void fail(const char *what, unsigned long long n) {
    out("heap FAILED: "); out(what); out(" "); outnum(n); out("\n");
    ExitProcess(1);
}
static void fill(int i) { for (SIZE_T k = 0; k < sz[i]; k++) blk[i][k] = (unsigned char)(i * 7 + k); }
static void check(int i, SIZE_T n) { for (SIZE_T k = 0; k < n; k++) if (blk[i][k] != (unsigned char)(i * 7 + k)) fail("pattern", i); }

void start(void) {
    HANDLE h = GetProcessHeap();
    if (!h) fail("GetProcessHeap", 0);
    unsigned ops = 0;
    for (int round = 0; round < 8; round++) {
        for (int i = 0; i < N; i++) {
            if (blk[i]) {
                check(i, sz[i]);
                if (HeapSize(h, 0, blk[i]) != sz[i]) fail("HeapSize", i);
                if (rnd() & 1) {
                    SIZE_T old = sz[i], n = 1 + rnd() % 3000;
                    unsigned char *p = HeapReAlloc(h, 0, blk[i], n);
                    if (!p) fail("HeapReAlloc", n);
                    blk[i] = p;
                    sz[i] = n;
                    check(i, old < n ? old : n);
                    fill(i);
                } else {
                    if (!HeapFree(h, 0, blk[i])) fail("HeapFree", i);
                    blk[i] = 0;
                }
            } else {
                sz[i] = 1 + rnd() % 2000;
                blk[i] = HeapAlloc(h, (i & 1) ? 8 : 0, sz[i]);         /* odd ones with HEAP_ZERO_MEMORY */
                if (!blk[i]) fail("HeapAlloc", sz[i]);
                if ((unsigned long long)blk[i] & 15) fail("alignment", (unsigned long long)blk[i]);
                if (i & 1) for (SIZE_T k = 0; k < sz[i]; k++) if (blk[i][k]) fail("zeroed", i);
                fill(i);
            }
            ops++;
        }
    }
    for (int i = 0; i < N; i++) if (blk[i]) { check(i, sz[i]); HeapFree(h, 0, blk[i]); }
    unsigned char *v = VirtualAlloc(0, 1 << 20, 0x3000, 0x04);           /* MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE */
    if (!v) fail("VirtualAlloc", GetLastError());
    for (unsigned k = 0; k < (1u << 20); k += 4096) if (v[k]) fail("VirtualAlloc not zero", k);
    for (unsigned k = 0; k < (1u << 20); k += 512) v[k] = (unsigned char)(k >> 9);
    for (unsigned k = 0; k < (1u << 20); k += 512) if (v[k] != (unsigned char)(k >> 9)) fail("VirtualAlloc pattern", k);
    if (!VirtualFree(v, 0, 0x8000)) fail("VirtualFree", GetLastError());
    if (VirtualFree(v, 0, 0x8000)) fail("VirtualFree twice", 0);
    long long f = 0, c1 = 0, c2 = 0;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c1);
    unsigned long long t = GetTickCount64();
    QueryPerformanceCounter(&c2);
    if (f <= 0 || c2 < c1 || !t) fail("clock", (unsigned long long)f);
    if (!GetCurrentProcessId()) fail("pid", 0);
    out("heap ok: "); outnum(ops); out(" operations\n");
    ExitProcess(0);
}
