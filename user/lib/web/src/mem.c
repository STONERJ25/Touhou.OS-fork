/*
 * Memory for the web engine: every byte a document uses comes from its budget (WebBudget), through arenas (WebArena).
 *
 * Why arenas: a page is thousands of small objects (nodes, attributes, rules, boxes) that all die together when the page
 * is closed; an arena hands them out by bumping a pointer in 64 KiB chunks and frees them all at once. Some objects die
 * earlier (a computed style replaced by a restyle, a text node's buffer that grew): those go to a free list of their size
 * class (16, 32, ... 4096 bytes) and the next allocation of that class takes them, so a page that keeps changing does not
 * grow without bound. Blocks above 4096 bytes are allocated one by one and can be freed individually.
 *
 * The budget counts what the arenas took from the C allocator (or the embedder's hooks); when the limit is reached every
 * further allocation fails and the budget stays "exceeded": the parser stops, layout gives up, and web_doc_status says so.
 */
#include "webint.h"

#include <stdlib.h>

typedef struct WaChunk { struct WaChunk *next; size_t size; } WaChunk;            /* 16 bytes: data stays 16-aligned */
typedef struct WaBig { struct WaBig *prev, *next; size_t size; size_t pad; } WaBig; /* 32 bytes */

#define WA_CHUNK_SIZE (64 * 1024)
#define WA_MAX_SMALL  4096

static void *budget_alloc(WebBudget *b, size_t n) {
    if (b->exceeded || n > b->limit || b->used > b->limit - n) {
        b->exceeded = true;
        return NULL;
    }
    void *p = b->al.alloc ? b->al.alloc(b->al.user, n) : malloc(n);
    if (!p) {
        b->exceeded = true;
        return NULL;
    }
    b->used += n;
    return p;
}

static void budget_free(WebBudget *b, void *p, size_t n) {
    if (!p) return;
    if (b->al.free) b->al.free(b->al.user, p);
    else free(p);
    b->used -= n;
}

void *web_raw_alloc(WebBudget *b, size_t n) {
    size_t *p = budget_alloc(b, n + 16);
    if (!p) return NULL;
    p[0] = n + 16;
    memset(p + 2, 0, n);
    return p + 2;
}

void web_raw_free(WebBudget *b, void *q) {
    if (!q) return;
    size_t *p = (size_t *)q - 2;
    budget_free(b, p, p[0]);
}

void wa_init(WebArena *a, WebBudget *b) {
    memset(a, 0, sizeof *a);
    a->budget = b;
}

static int size_class(size_t n) {
    if (n <= 16) return 0;
    int bits = 64 - __builtin_clzll((unsigned long long)(n - 1));   /* n - 1 < 2^bits */
    return bits - 4;
}

void *wa_alloc(WebArena *a, size_t n) {
    if (n == 0) n = 1;
    if (n > WA_MAX_SMALL) {
        if (n > (size_t)1 << 40) return NULL;
        WaBig *b = budget_alloc(a->budget, sizeof(WaBig) + n);
        if (!b) return NULL;
        memset(b, 0, sizeof(WaBig) + n);
        b->size = n;
        a->total += sizeof(WaBig) + n;
        b->next = a->big;
        if (a->big) a->big->prev = b;
        a->big = b;
        return b + 1;
    }
    int c = size_class(n);
    size_t sz = (size_t)16 << c;
    void *p = a->free_list[c];
    if (p) {
        a->free_list[c] = *(void **)p;
        memset(p, 0, sz);
        return p;
    }
    if ((size_t)(a->end - a->cur) < sz) {
        WaChunk *ch = budget_alloc(a->budget, sizeof(WaChunk) + WA_CHUNK_SIZE);
        if (!ch) return NULL;
        memset(ch, 0, sizeof(WaChunk) + WA_CHUNK_SIZE);
        ch->size = WA_CHUNK_SIZE;
        a->total += sizeof(WaChunk) + WA_CHUNK_SIZE;
        ch->next = a->chunks;
        a->chunks = ch;
        a->cur = (char *)(ch + 1);
        a->end = a->cur + WA_CHUNK_SIZE;
    }
    p = a->cur;
    a->cur += sz;
    return p;
}

void wa_free(WebArena *a, void *p, size_t n) {
    if (!p) return;
    if (n == 0) n = 1;
    if (n > WA_MAX_SMALL) {
        WaBig *b = (WaBig *)p - 1;
        if (b->prev) b->prev->next = b->next;
        else a->big = b->next;
        if (b->next) b->next->prev = b->prev;
        a->total -= sizeof(WaBig) + b->size;
        budget_free(a->budget, b, sizeof(WaBig) + b->size);
        return;
    }
    int c = size_class(n);
    *(void **)p = a->free_list[c];
    a->free_list[c] = p;
}

void *wa_realloc(WebArena *a, void *p, size_t old, size_t n) {
    if (!p) return wa_alloc(a, n);
    if (old <= WA_MAX_SMALL && n <= WA_MAX_SMALL && size_class(old ? old : 1) == size_class(n ? n : 1)) {
        if (n > old) memset((char *)p + old, 0, n - old);
        return p;
    }
    void *q = wa_alloc(a, n);
    if (!q) return NULL;
    memcpy(q, p, old < n ? old : n);
    wa_free(a, p, old);
    return q;
}

char *wa_strndup(WebArena *a, const char *s, size_t n) {
    char *p = wa_alloc(a, n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *wa_strdup(WebArena *a, const char *s) {
    return wa_strndup(a, s, strlen(s));
}

void wa_reset(WebArena *a) {
    WebBudget *b = a->budget;
    for (WaChunk *c = a->chunks, *nx; c; c = nx) {
        nx = c->next;
        budget_free(b, c, sizeof(WaChunk) + c->size);
    }
    for (WaBig *g = a->big, *nx; g; g = nx) {
        nx = g->next;
        budget_free(b, g, sizeof(WaBig) + g->size);
    }
    wa_init(a, b);
}

void wa_destroy(WebArena *a) {
    wa_reset(a);
}

/* ------------------------------------------------------------------------------------------------ buffers and vectors */

void buf_init(WebBuf *b, WebArena *a) {
    b->p = NULL;
    b->n = b->cap = 0;
    b->a = a;
}

bool buf_reserve(WebBuf *b, size_t extra) {
    size_t need = (size_t)b->n + extra + 1;                 /* room for a terminating NUL */
    if (need <= b->cap) return true;
    if (need > 0x7FFFFFFF) return false;
    size_t cap = b->cap ? b->cap : 32;
    while (cap < need) cap *= 2;
    char *p = wa_realloc(b->a, b->p, b->cap, cap);
    if (!p) return false;
    b->p = p;
    b->cap = (uint32_t)cap;
    return true;
}

bool buf_put(WebBuf *b, const void *s, size_t n) {
    if (!buf_reserve(b, n)) return false;
    if (n) memcpy(b->p + b->n, s, n);
    b->n += (uint32_t)n;
    b->p[b->n] = 0;
    return true;
}

bool buf_putc(WebBuf *b, char c) {
    if (b->n + 1 >= b->cap && !buf_reserve(b, 1)) return false;
    b->p[b->n++] = c;
    b->p[b->n] = 0;
    return true;
}

bool buf_puts(WebBuf *b, const char *s) {
    return buf_put(b, s, strlen(s));
}

bool buf_put_cp(WebBuf *b, uint32_t cp) {
    char tmp[4];
    int n = utf8_encode(cp, tmp);
    return buf_put(b, tmp, (size_t)n);
}

bool buf_put_int(WebBuf *b, long v) {
    char tmp[24];
    int i = 0;
    unsigned long u = v < 0 ? (unsigned long)(-(v + 1)) + 1 : (unsigned long)v;
    do { tmp[i++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) tmp[i++] = '-';
    char out[24];
    for (int k = 0; k < i; k++) out[k] = tmp[i - 1 - k];
    return buf_put(b, out, (size_t)i);
}

void buf_free(WebBuf *b) {
    if (b->p) wa_free(b->a, b->p, b->cap);
    b->p = NULL;
    b->n = b->cap = 0;
}

bool vec_grow_(WebArena *a, void **v, uint32_t *cap, uint32_t need, size_t esz) {
    if (need <= *cap) return true;
    size_t nc = *cap ? (size_t)*cap * 2 : 8;
    while (nc < need) nc *= 2;
    if (nc * esz > ((size_t)1 << 34)) return false;
    void *p = wa_realloc(a, *v, (size_t)*cap * esz, nc * esz);
    if (!p) return false;
    *v = p;
    *cap = (uint32_t)nc;
    return true;
}

/* ------------------------------------------------------------------------------------------------ UTF-8 */

int utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp > 0x10FFFF) cp = 0xFFFD;
    if (cp < 0x10000) return utf8_encode(cp, out);
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Internal strings are UTF-8 (WTF-8: a lone surrogate from document.write keeps its three-byte form), so this decoder is
 * lenient: it only has to undo utf8_encode; malformed bytes become U+FFFD one byte at a time. */
uint32_t utf8_decode(const char *s8, size_t n, size_t *len) {
    const unsigned char *s = (const unsigned char *)s8;
    if (n == 0) { *len = 0; return 0; }
    unsigned c = s[0];
    if (c < 0x80) { *len = 1; return c; }
    int need;
    uint32_t cp;
    if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; }
    else if (c >= 0xE0) { if (c > 0xEF) { *len = 1; return 0xFFFD; } need = 2; cp = c & 0x0F; }
    else if (c >= 0xC2) { need = 1; cp = c & 0x1F; }
    else { *len = 1; return 0xFFFD; }
    if ((size_t)need >= n) { *len = 1; return 0xFFFD; }
    for (int i = 1; i <= need; i++) {
        if ((s[i] & 0xC0) != 0x80) { *len = 1; return 0xFFFD; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *len = (size_t)need + 1;
    return cp;
}
