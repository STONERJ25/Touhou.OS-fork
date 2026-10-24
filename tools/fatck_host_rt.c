/*
 * The little C runtime that shared/fatck.c needs when tools/fatck_host.py builds it into a freestanding Windows DLL or Linux shared
 * object (there is no C library on the build machine's clang): mem* and strlen, plus a size report so the ctypes mirrors can be checked.
 * Compiled with -fno-builtin so these loops are not turned back into calls to themselves.
 */
#include <stddef.h>
#include <stdint.h>
#include "fatck.h"

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dd = d;
    const unsigned char *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dd = d;
    const unsigned char *ss = s;
    if (dd < ss) { while (n--) *dd++ = *ss++; }
    else { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d;
}

void *memset(void *d, int c, size_t n) {
    unsigned char *dd = d;
    while (n--) *dd++ = (unsigned char)c;
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* sizes of the structures the Python side mirrors, in the order: Finding, Result, Geom, Io */
void fatck_host_sizes(uint32_t *out) {
    out[0] = (uint32_t)sizeof(FatCkFinding);
    out[1] = (uint32_t)sizeof(FatCkResult);
    out[2] = (uint32_t)sizeof(FatCkGeom);
    out[3] = (uint32_t)sizeof(FatCkIo);
}
