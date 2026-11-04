/* mcc's <emmintrin.h>: the SSE2 integer intrinsics that libgap's string functions use (user/libc/src/string.c), written in
 * plain C on two 64-bit halves. mcc has no vector registers, so these give the results of the real instructions without
 * their speed. Only what libgap needs is here; anything else is a compile error, not a silent wrong answer. */
#ifndef _MCC_EMMINTRIN_H
#define _MCC_EMMINTRIN_H

typedef struct { unsigned long long __q[2]; } __m128i;

/* Unaligned 64-bit loads and stores are fine on x86-64; no memcpy here (string.c's memcpy is built on these). */
static inline __m128i _mm_loadu_si128(const __m128i *p) {
    const unsigned long long *q = (const unsigned long long *)p;
    __m128i r;
    r.__q[0] = q[0];
    r.__q[1] = q[1];
    return r;
}

static inline __m128i _mm_load_si128(const __m128i *p) { return _mm_loadu_si128(p); }

static inline void _mm_storeu_si128(__m128i *p, __m128i v) {
    unsigned long long *q = (unsigned long long *)p;
    q[0] = v.__q[0];
    q[1] = v.__q[1];
}

static inline __m128i _mm_setzero_si128(void) {
    __m128i r;
    r.__q[0] = 0;
    r.__q[1] = 0;
    return r;
}

static inline __m128i _mm_set1_epi8(char c) {
    __m128i r;
    r.__q[0] = r.__q[1] = 0x0101010101010101ULL * (unsigned char)c;
    return r;
}

/* Byte i of the result is 0xff where the bytes are equal, 0 elsewhere. */
static inline __m128i _mm_cmpeq_epi8(__m128i a, __m128i b) {
    __m128i r;
    for (int h = 0; h < 2; h++) {
        unsigned long long x = a.__q[h] ^ b.__q[h], m = 0;
        for (int i = 0; i < 8; i++)
            if (!((x >> (8 * i)) & 0xff)) m |= 0xffULL << (8 * i);
        r.__q[h] = m;
    }
    return r;
}

/* Bit i of the result is the top bit of byte i. */
static inline int _mm_movemask_epi8(__m128i a) {
    int m = 0;
    for (int h = 0; h < 2; h++)
        for (int i = 0; i < 8; i++)
            if ((a.__q[h] >> (8 * i + 7)) & 1) m |= 1 << (8 * h + i);
    return m;
}

#endif
