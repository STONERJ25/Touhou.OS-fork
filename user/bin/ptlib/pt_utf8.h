/*
 * ptlib: header-only helpers shared by pnote (editor), patchouli-pager and patchouli (manual viewer).
 * tools/build.py turns every directory under user/bin that holds .c files into a program; this one holds only headers,
 * so it is skipped and the three programs just #include "../ptlib/<header>".
 *
 * pt_utf8.h: UTF-8 decoding for terminal text. A "column" is one code point (no wide-character tables, the console
 * font is 8x16 cells). Bytes that are not valid UTF-8 decode as single "raw byte" code points 0xDC80..0xDCFF (the
 * surrogateescape convention), so callers can show them as '?' and still write the original bytes back unchanged.
 */
#ifndef PT_UTF8_H
#define PT_UTF8_H

#include <stddef.h>
#include <stdint.h>

#define PT_RAWBYTE_BASE 0xDC00u

static inline int pt_is_rawbyte(uint32_t cp) { return cp >= 0xDC80u && cp <= 0xDCFFu; }

/* Decodes the code point at s[0] (n >= 1 bytes available). Returns the number of bytes it occupies (1..4). */
static inline int pt_u8_decode(const unsigned char *s, size_t n, uint32_t *cp) {
    unsigned char c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    int need;
    uint32_t v, min;
    if (c >= 0xC2 && c <= 0xDF) { need = 1; v = c & 0x1Fu; min = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { need = 2; v = c & 0x0Fu; min = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { need = 3; v = c & 0x07u; min = 0x10000; }
    else { *cp = PT_RAWBYTE_BASE + c; return 1; }
    if ((size_t)need >= n) { *cp = PT_RAWBYTE_BASE + c; return 1; }
    for (int i = 1; i <= need; i++) {
        if ((s[i] & 0xC0) != 0x80) { *cp = PT_RAWBYTE_BASE + c; return 1; }
        v = (v << 6) | (s[i] & 0x3Fu);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) { *cp = PT_RAWBYTE_BASE + c; return 1; }
    *cp = v;
    return need + 1;
}

/* Writes the UTF-8 form of cp to out (room for 4 bytes). Raw-byte code points turn back into the byte they came from. */
static inline int pt_u8_encode(uint32_t cp, char *out) {
    if (pt_is_rawbyte(cp)) { out[0] = (char)(cp - PT_RAWBYTE_BASE); return 1; }
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Byte length of the code point starting at s[i] (i < n). */
static inline int pt_u8_next(const char *s, size_t n, size_t i) {
    uint32_t cp;
    return pt_u8_decode((const unsigned char *)s + i, n - i, &cp);
}

/* Start of the code point that ends just before byte i (i > 0). */
static inline size_t pt_u8_prev(const char *s, size_t i) {
    size_t j = i - 1;
    int back = 0;
    while (j > 0 && back < 3 && ((unsigned char)s[j] & 0xC0) == 0x80) { j--; back++; }
    uint32_t cp;
    if (j + (size_t)pt_u8_decode((const unsigned char *)s + j, i - j, &cp) == i) return j;
    return i - 1;
}

/* Number of code points (= terminal columns) in s[0..n). */
static inline size_t pt_u8_count(const char *s, size_t n) {
    size_t i = 0, cols = 0;
    while (i < n) { i += (size_t)pt_u8_next(s, n, i); cols++; }
    return cols;
}

/* Byte offset after skipping `cols` code points from the start of s (stops at n). */
static inline size_t pt_u8_skip(const char *s, size_t n, size_t cols) {
    size_t i = 0;
    while (i < n && cols--) i += (size_t)pt_u8_next(s, n, i);
    return i;
}

/* ASCII-and-Latin-1 aware "is this a word character" test used for word movement and double-click style searches. */
static inline int pt_is_word_cp(uint32_t cp) {
    if (cp < 0x80) return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '_';
    return cp >= 0xC0 && !pt_is_rawbyte(cp);
}

#endif
