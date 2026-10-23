/* malloc: boundary-tag allocator over brk with segregated free lists, mmap for big blocks.
 *
 * Layout. A block starts with an 8-byte header: its size (a multiple of 16) with two flag bits, INUSE for the block itself and
 * PINUSE for the block before it. The payload follows the header and is 16-byte aligned. A free block also keeps its size in
 * its last 8 bytes (the footer) so the block after it can find it when it is freed, and the free-list links in its first 16
 * payload bytes; an allocated block has no footer, so the overhead of an allocation is 8 bytes. The smallest block is 32 bytes.
 * The heap ends with a zero-size "epilogue" header that is always INUSE.
 *
 * Free lists. Free blocks live in doubly linked lists by size: one list for every size up to 1008 bytes (exact fit, O(1)),
 * and four lists per power of two above that. A bitmap says which lists are not empty, so malloc finds the smallest block
 * that fits without walking anything: malloc and free are O(1) apart from the limited scan of one large list. Neighbouring
 * free blocks are merged when a block is freed.
 *
 * realloc grows in place when the next block is free (or when the block is the last one: the heap is extended), and shrinks in
 * place by splitting. Blocks of 256 KiB or more get their own mapping; realloc grows those geometrically so that appending in a
 * loop does not copy the whole block every time.
 *
 * Threads share the heap under one lock (__malloc_lock); a program that never created a thread skips it. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include "pthread_impl.h"

#define HDR 8
#define MIN_BLOCK 32
#define CHUNK (256 * 1024)                      /* the heap grows by at least this much */
#define MMAP_THRESHOLD (256 * 1024)
#define MMAP_MAGIC 0x4D4D4150UL                 /* "MMAP": the word before the payload of a mapped block */
#define MMAP_HDR 16                             /* [length of the mapping][MMAP_MAGIC] */

#define INUSE  1UL
#define PINUSE 2UL
#define SIZE_MASK (~(size_t)15)

#define GET(b)       (*(size_t *)(b))
#define BSIZE(b)     (GET(b) & SIZE_MASK)
#define NEXT_BLK(b)  ((char *)(b) + BSIZE(b))
#define FOOT(b, sz)  (*(size_t *)((char *)(b) + (sz) - HDR))
#define PAYLOAD(b)   ((char *)(b) + HDR)
#define BLOCK(p)     ((char *)(p) - HDR)

typedef struct Node { struct Node *next, *prev; } Node;
#define NODE(b) ((Node *)PAYLOAD(b))

#define SMALL_MAX 1008                          /* sizes up to this have a list of their own */
#define NSMALL 62                               /* 32, 48, ..., 1008 */
#define NBINS 152

static Node g_bins[NBINS];
static uint64_t g_bitmap[3];
static char *g_epilogue;                        /* header of the zero-size block that ends the heap; NULL until the heap exists */

volatile int __malloc_lock;

static int bin_index(size_t size) {
    if (size <= SMALL_MAX) return (int)(size >> 4) - 2;
    int k = 63 - __builtin_clzl(size);          /* >= 10 */
    int idx = NSMALL + (k - 10) * 4 + (int)((size >> (k - 2)) & 3);
    return idx < NBINS ? idx : NBINS - 1;
}

static void bin_insert(char *b) {
    int i = bin_index(BSIZE(b));
    Node *s = &g_bins[i], *n = NODE(b);
    n->next = s->next;
    n->prev = s;
    s->next->prev = n;
    s->next = n;
    g_bitmap[i >> 6] |= 1UL << (i & 63);
}

static void bin_remove(char *b) {
    int i = bin_index(BSIZE(b));
    Node *n = NODE(b);
    n->prev->next = n->next;
    n->next->prev = n->prev;
    if (g_bins[i].next == &g_bins[i]) g_bitmap[i >> 6] &= ~(1UL << (i & 63));
}

static int next_bin(int i) {                    /* the first non-empty list at index i or above, or -1 */
    int w = i >> 6;
    uint64_t m = g_bitmap[w] & (~0UL << (i & 63));
    while (!m) {
        if (++w >= 3) return -1;
        m = g_bitmap[w];
    }
    return w * 64 + __builtin_ctzl(m);
}

/* Makes [b, b+size) a free block whose predecessor's state is pin (0 or PINUSE), merges it with a free successor and
 * files it. The caller has already merged backwards when needed. */
static char *make_free(char *b, size_t size, size_t pin) {
    char *nxt = b + size;
    if (!(GET(nxt) & INUSE)) {
        size_t ns = BSIZE(nxt);
        bin_remove(nxt);
        size += ns;
        nxt = b + size;
    }
    GET(b) = size | pin;
    FOOT(b, size) = size;
    GET(nxt) &= ~PINUSE;
    bin_insert(b);
    return b;
}

static int heap_init(void) {
    for (int i = 0; i < NBINS; i++) g_bins[i].next = g_bins[i].prev = &g_bins[i];
    char *p = sbrk(0);
    if (p == (char *)-1) return -1;
    size_t pad = (8 - ((size_t)p & 15)) & 15;   /* headers sit 8 bytes below a 16-byte boundary, so payloads are aligned */
    size_t size = CHUNK;                        /* first block; the epilogue header follows it */
    if (sbrk((long)(pad + size + HDR)) == (void *)-1) return -1;
    char *b = p + pad;
    GET(b) = size | PINUSE;                     /* nothing before it: pretend that is in use */
    FOOT(b, size) = size;
    g_epilogue = b + size;
    GET(g_epilogue) = INUSE;
    bin_insert(b);
    return 0;
}

/* Grows the heap by at least `bytes`; returns the new free block (merged with a free last block), already filed. */
static char *extend_heap(size_t bytes) {
    bytes = (bytes + 4095) & ~(size_t)4095;
    char *old = sbrk((long)bytes);
    if (old == (char *)-1 || old != g_epilogue + HDR) return NULL;
    char *b = g_epilogue;                       /* the old epilogue's header becomes the new block's header */
    size_t pin = GET(b) & PINUSE;
    g_epilogue = b + bytes;
    GET(g_epilogue) = INUSE;
    if (!pin) {                                 /* the last block was free: merge with it */
        size_t ps = *(size_t *)(b - HDR);
        char *pb = b - ps;
        bin_remove(pb);
        return make_free(pb, ps + bytes, GET(pb) & PINUSE);
    }
    return make_free(b, bytes, PINUSE);
}

static char *find_fit(size_t nb) {
    int idx = bin_index(nb);
    if (idx >= NSMALL) {                        /* one list holds a range of sizes: look for a block that is big enough */
        Node *s = &g_bins[idx];
        int tries = 0;
        for (Node *n = s->next; n != s && tries < 32; n = n->next, tries++)
            if (BSIZE(BLOCK(n)) >= nb) return BLOCK(n);
        idx++;
    }
    int j = next_bin(idx);
    return j < 0 ? NULL : BLOCK(g_bins[j].next);
}

/* b is free and filed; takes nb bytes from its start and returns the payload. */
static void *place(char *b, size_t nb) {
    size_t size = BSIZE(b), pin = GET(b) & PINUSE;
    bin_remove(b);
    if (size - nb >= MIN_BLOCK) {
        GET(b) = nb | INUSE | pin;
        char *r = b + nb;
        GET(r) = (size - nb) | PINUSE;          /* the next block's PINUSE is already clear */
        FOOT(r, size - nb) = size - nb;
        bin_insert(r);
    } else {
        GET(b) = size | INUSE | pin;
        GET(b + size) |= PINUSE;
    }
    return PAYLOAD(b);
}

static void *mmap_alloc(size_t n) {
    size_t total = (n + MMAP_HDR + 4095) & ~(size_t)4095;
    if (total < n) { errno = ENOMEM; return NULL; }
    char *m = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return NULL;
    *(size_t *)m = total;
    *(size_t *)(m + 8) = MMAP_MAGIC;
    return m + MMAP_HDR;
}

static inline int is_mapped(void *p) { return *(size_t *)((char *)p - HDR) == MMAP_MAGIC; }
static inline size_t mapped_total(void *p) { return *(size_t *)((char *)p - MMAP_HDR); }

static size_t block_for(size_t n) {
    size_t nb = (n + HDR + 15) & ~(size_t)15;
    return nb < MIN_BLOCK ? MIN_BLOCK : nb;
}

/* Allocates from the heap, whatever the size. */
static void *heap_alloc(size_t n) {
    if (n > SIZE_MAX - 4096) { errno = ENOMEM; return NULL; }
    if (!g_epilogue && heap_init()) { errno = ENOMEM; return NULL; }
    size_t nb = block_for(n);
    char *b = find_fit(nb);
    if (!b) {
        b = extend_heap(nb > CHUNK ? nb : CHUNK);
        if (!b) { errno = ENOMEM; return NULL; }
    }
    return place(b, nb);
}

static void *malloc_nl(size_t n) {
    if (n >= MMAP_THRESHOLD) {
        void *p = mmap_alloc(n);
        if (p || errno != ENOMEM) return p;
    }
    return heap_alloc(n ? n : 1);
}

static void free_nl(void *p) {
    if (is_mapped(p)) { munmap((char *)p - MMAP_HDR, mapped_total(p)); return; }
    char *b = BLOCK(p);
    size_t h = GET(b), size = h & SIZE_MASK, pin = h & PINUSE;
    if (!pin) {                                 /* merge with the free block before it */
        size_t ps = *(size_t *)(b - HDR);
        char *pb = b - ps;
        bin_remove(pb);
        b = pb;
        size += ps;
        pin = GET(pb) & PINUSE;
    }
    make_free(b, size, pin);
}

static size_t usable(void *p) {
    if (is_mapped(p)) return mapped_total(p) - MMAP_HDR;
    return BSIZE(BLOCK(p)) - HDR;
}

/* Shrinks the allocated block b to nb bytes, freeing the rest when it is big enough to be a block. */
static void shrink_block(char *b, size_t nb) {
    size_t size = BSIZE(b);
    if (size - nb < MIN_BLOCK) return;
    GET(b) = nb | (GET(b) & (PINUSE | INUSE));
    char *r = b + nb;
    GET(r) = (size - nb) | INUSE | PINUSE;      /* looks allocated for a moment so that free_nl can merge it forwards */
    free_nl(PAYLOAD(r));
}

/* Tries to make the allocated block b at least nb bytes without moving it. */
static int grow_in_place(char *b, size_t nb) {
    size_t size = BSIZE(b);
    char *nxt = b + size;
    if ((GET(nxt) & INUSE) && nxt == g_epilogue) {      /* the last block: ask the kernel for more heap */
        size_t need = nb - size;
        if (!extend_heap(need > CHUNK ? need : CHUNK)) return 0;
    }
    size_t nh = GET(nxt);
    if (nh & INUSE) return 0;
    size_t ns = nh & SIZE_MASK;
    if (size + ns < nb) return 0;
    bin_remove(nxt);
    size_t tot = size + ns;
    GET(b) = tot | INUSE | (GET(b) & PINUSE);
    GET(b + tot) |= PINUSE;
    shrink_block(b, nb);
    return 1;
}

static void *realloc_nl(void *p, size_t n) {
    if (is_mapped(p)) {
        size_t have = mapped_total(p) - MMAP_HDR;
        if (n <= have && n >= have / 2) return p;
        if (n > have) {                                  /* grow by half again, so appending in a loop stays cheap */
            size_t want = n + n / 2 > n ? n + n / 2 : n;
            void *np = mmap_alloc(want);
            if (!np) np = mmap_alloc(n);
            if (!np) return NULL;
            memcpy(np, p, have);
            munmap((char *)p - MMAP_HDR, mapped_total(p));
            return np;
        }
        void *np = malloc_nl(n);                         /* much smaller: move it to the heap (or a smaller mapping) */
        if (!np) return p;
        memcpy(np, p, n);
        munmap((char *)p - MMAP_HDR, mapped_total(p));
        return np;
    }
    char *b = BLOCK(p);
    if (n > SIZE_MAX - 4096) { errno = ENOMEM; return NULL; }
    size_t nb = block_for(n), size = BSIZE(b);
    if (nb <= size) { shrink_block(b, nb); return p; }
    if (n < MMAP_THRESHOLD && grow_in_place(b, nb)) return p;
    void *np = malloc_nl(n);
    if (!np) return NULL;
    memcpy(np, p, size - HDR);
    free_nl(p);
    return np;
}

static void *aligned_nl(size_t align, size_t n) {
    if (align <= 16) return malloc_nl(n);
    if (n > SIZE_MAX - align - 4096) { errno = ENOMEM; return NULL; }
    void *raw = heap_alloc(n + align + MIN_BLOCK);
    if (!raw) return NULL;
    char *b = BLOCK(raw);
    size_t total = BSIZE(b);
    size_t a = ((size_t)raw + align - 1) & ~(align - 1);
    size_t gap = a - (size_t)raw;
    if (gap && gap < MIN_BLOCK) gap += align;             /* the part in front must be a block of its own */
    if (gap) {
        char *nb2 = b + gap;
        size_t pin = GET(b) & PINUSE;
        GET(nb2) = (total - gap) | INUSE;                 /* its predecessor (the front part) is about to be freed */
        GET(b) = gap | INUSE | pin;
        free_nl(PAYLOAD(b));
        b = nb2;
    }
    shrink_block(b, block_for(n));
    return PAYLOAD(b);
}

/* ---------------------------------------------------------------- the public functions */

#define LOCK()   int lk_ = __libc_threaded; if (lk_) __lock(&__malloc_lock)
#define UNLOCK() do { if (lk_) __unlock(&__malloc_lock); } while (0)

void *malloc(size_t n) {
    LOCK();
    void *p = malloc_nl(n);
    UNLOCK();
    return p;
}

void free(void *p) {
    if (!p) return;
    LOCK();
    free_nl(p);
    UNLOCK();
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (n == 0) { free(p); return NULL; }
    LOCK();
    void *np = realloc_nl(p, n);
    UNLOCK();
    return np;
}

void *calloc(size_t n, size_t sz) {
    if (sz && n > (size_t)-1 / sz) { errno = ENOMEM; return NULL; }
    size_t total = n * sz;
    LOCK();
    void *p = malloc_nl(total);
    UNLOCK();
    if (p && !is_mapped(p)) memset(p, 0, total);          /* a fresh mapping is already zero */
    return p;
}

void *aligned_alloc(size_t align, size_t n) {
    if (align & (align - 1)) { errno = EINVAL; return NULL; }
    LOCK();
    void *p = aligned_nl(align, n);
    UNLOCK();
    return p;
}

void *memalign(size_t align, size_t n) { return aligned_alloc(align, n); }

int posix_memalign(void **out, size_t align, size_t n) {
    if (align < sizeof(void *) || (align & (align - 1))) return EINVAL;
    void *p = aligned_alloc(align, n);
    if (!p) return ENOMEM;
    *out = p;
    return 0;
}

size_t malloc_usable_size(void *p) {
    if (!p) return 0;
    LOCK();
    size_t n = usable(p);
    UNLOCK();
    return n;
}
