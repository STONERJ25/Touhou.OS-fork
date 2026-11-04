/* Kernel heap: size-class slabs for small objects, whole buddy blocks for large ones.
 * Page metadata (Page.flags/slab_class/order) identifies the class on free, so objects carry no header.
 *
 * Hardening (docs/HARDENING.md): requests above 16 MiB fail instead of wrapping around in the page arithmetic; a free object
 * carries a marker in its second word, so freeing it twice is noticed (warning, no harm done) instead of corrupting the free
 * list; a free list that has been scribbled on is abandoned instead of followed; kfree of something that is not a heap object
 * warns and returns. With "heapdebug" on the kernel command line, freed memory is filled with 0x6B and the fill is checked when
 * the object is handed out again, which finds writes through stale pointers. */
#include "mm.h"
#include "kernel.h"
#include "cpu.h"
#include "cmdline.h"

#define NCLASS 14
static const u16 g_class_size[NCLASS] = { 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048 };

#define KMALLOC_MAX ((size_t)PAGE_SIZE << PMM_MAX_ORDER)
#define POISON_FREE 0x6B

static void *g_freelist[NCLASS];
static u64   g_in_use;
static u64   g_slab_pages;
static bool  g_debug;

static int class_for(size_t n) {
    for (int i = 0; i < NCLASS; i++)
        if (n <= g_class_size[i]) return i;
    return -1;
}

/* The second word of a free object. Depends on the address, so a copy of one freed object inside another does not look free. */
static inline u64 free_mark(const void *obj) { return (u64)obj ^ 0xF4EE0B1EC7F4EE00UL; }

static inline bool freelist_ptr_ok(void *p, int c) {            /* NULL, or an object of class c inside the direct map */
    if (!p) return true;
    u64 a = (u64)p;
    if (a < g_hhdm || a >= g_hhdm + pmm_ram_top()) return false;
    return ((a & (PAGE_SIZE - 1)) % g_class_size[c]) == 0;
}

void heap_init_debug(void) { g_debug = cmdline_flag("heapdebug"); }

static bool refill(int c) {
    paddr_t p = pmm_alloc_page();
    if (!p) return false;
    Page *pg = pmm_page(p);
    pg->flags |= PG_SLAB;
    pg->slab_class = (u8)c;
    g_slab_pages++;
    u8 *base = phys_to_virt(p);
    size_t sz = g_class_size[c], n = PAGE_SIZE / sz;
    for (size_t i = n; i-- > 0;) {
        void **obj = (void **)(base + i * sz);
        obj[0] = g_freelist[c];
        obj[1] = (void *)free_mark(obj);
        if (g_debug) memset((u8 *)obj + 16, POISON_FREE, sz - 16);
        g_freelist[c] = obj;
    }
    return true;
}

void *kmalloc(size_t n) {
    if (n == 0) n = 1;
    if (n > KMALLOC_MAX) return NULL;
    if (n <= 2048) {
        int c = class_for(n);
        u64 f = irq_save();
        if (g_freelist[c] && !freelist_ptr_ok(g_freelist[c], c)) {          /* somebody wrote through a stale pointer: give up the list */
            g_freelist[c] = NULL;
            irq_restore(f);
            KWARN("heap class %d: the free list is corrupt; its objects are abandoned", c);
            f = irq_save();
        }
        if (!g_freelist[c] && !refill(c)) { irq_restore(f); return NULL; }
        void **obj = g_freelist[c];
        void *next = obj[0];
        if (obj[1] != (void *)free_mark(obj) || !freelist_ptr_ok(next, c)) {   /* the object was written to while it was free */
            g_freelist[c] = NULL;
            irq_restore(f);
            KWARN("heap class %d: object %p was modified after it was freed; the free list is abandoned", c, (void *)obj);
            return kmalloc(n);
        }
        if (g_debug) {
            const u8 *b = (const u8 *)obj;
            for (size_t i = 16; i < g_class_size[c]; i++)
                if (b[i] != POISON_FREE) {
                    g_freelist[c] = next;
                    irq_restore(f);
                    KWARN("heap class %d: object %p was written to at +%zu after it was freed", c, (void *)obj, i);
                    return kmalloc(n);                                   /* this object is lost, the others are fine */
                }
        }
        g_freelist[c] = next;
        obj[1] = NULL;
        g_in_use += g_class_size[c];
        irq_restore(f);
        return obj;
    }
    unsigned order = 0;
    size_t pages = (n + PAGE_SIZE - 1) >> PAGE_SHIFT;
    while ((1UL << order) < pages) order++;
    paddr_t p = pmm_alloc(order);
    if (!p) return NULL;
    Page *pg = pmm_page(p);
    pg->flags |= PG_LARGE;
    pg->order = (u8)order;
    u64 f = irq_save();
    g_in_use += PAGE_SIZE << order;
    irq_restore(f);
    return phys_to_virt(p);
}

void *kzalloc(size_t n) {
    void *p = kmalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

size_t kmalloc_usable(void *ptr) {
    if (!ptr) return 0;
    Page *pg = pmm_page(virt_to_phys_hhdm(ptr));
    if (!pg) return 0;
    if (pg->flags & PG_SLAB) return g_class_size[pg->slab_class];
    return (size_t)PAGE_SIZE << pg->order;
}

void kfree(void *ptr) {
    if (!ptr) return;
    paddr_t p = virt_to_phys_hhdm(ptr);
    Page *pg = (u64)ptr >= g_hhdm ? pmm_page(p) : NULL;
    if (!pg) { KWARN("kfree(%p): not heap memory", ptr); return; }
    u64 f = irq_save();
    if (pg->flags & PG_SLAB) {
        int c = pg->slab_class;
        void **obj = ptr;
        if (((u64)ptr & (PAGE_SIZE - 1)) % g_class_size[c]) {
            irq_restore(f);
            KWARN("kfree(%p): not the start of a class-%d object", ptr, c);
            return;
        }
        if (obj[1] == (void *)free_mark(obj)) {                          /* freed already */
            irq_restore(f);
            KWARN("kfree(%p): double free (class %d)", ptr, c);
            return;
        }
        obj[0] = g_freelist[c];
        obj[1] = (void *)free_mark(obj);
        if (g_debug) memset((u8 *)obj + 16, POISON_FREE, g_class_size[c] - 16);
        g_freelist[c] = obj;
        g_in_use -= g_class_size[c];
        irq_restore(f);
    } else if (pg->flags & PG_LARGE) {
        unsigned order = pg->order;
        if (virt_to_phys_hhdm(ptr) & ((PAGE_SIZE << order) - 1)) {
            irq_restore(f);
            KWARN("kfree(%p): inside a large block, not its start", ptr);
            return;
        }
        pg->flags &= (u8)~PG_LARGE;                                       /* a second kfree finds a block that is no longer large */
        g_in_use -= PAGE_SIZE << order;
        irq_restore(f);
        pmm_free(p, order);
    } else {
        irq_restore(f);
        KWARN("kfree(%p): bad pointer (page flags %#x): freed already, or never from the heap", ptr, pg->flags);
    }
}

void *krealloc(void *ptr, size_t n) {
    if (!ptr) return kmalloc(n);
    size_t old = kmalloc_usable(ptr);
    if (n <= old) return ptr;
    void *np = kmalloc(n);
    if (!np) return NULL;
    memcpy(np, ptr, old);
    kfree(ptr);
    return np;
}

u64 heap_bytes_in_use(void) { return g_in_use; }
u64 heap_slab_pages(void) { return g_slab_pages; }
