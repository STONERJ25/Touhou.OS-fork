/* Kernel heap: size-class slabs for small objects, whole buddy blocks for large ones.
 * Page metadata (Page.flags/slab_class/order) identifies the class on free, so objects carry no header. */
#include "mm.h"
#include "kernel.h"
#include "cpu.h"

#define NCLASS 14
static const u16 g_class_size[NCLASS] = { 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048 };

static void *g_freelist[NCLASS];
static u64   g_in_use;

static int class_for(size_t n) {
    for (int i = 0; i < NCLASS; i++)
        if (n <= g_class_size[i]) return i;
    return -1;
}

static bool refill(int c) {
    paddr_t p = pmm_alloc_page();
    if (!p) return false;
    Page *pg = pmm_page(p);
    pg->flags |= PG_SLAB;
    pg->slab_class = (u8)c;
    u8 *base = phys_to_virt(p);
    size_t sz = g_class_size[c], n = PAGE_SIZE / sz;
    for (size_t i = n; i-- > 0;) {
        void **obj = (void **)(base + i * sz);
        *obj = g_freelist[c];
        g_freelist[c] = obj;
    }
    return true;
}

void *kmalloc(size_t n) {
    if (n == 0) n = 1;
    if (n <= 2048) {
        int c = class_for(n);
        u64 f = irq_save();
        if (!g_freelist[c] && !refill(c)) { irq_restore(f); return NULL; }
        void **obj = g_freelist[c];
        g_freelist[c] = *obj;
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
    if (pg->flags & PG_SLAB) return g_class_size[pg->slab_class];
    return (size_t)PAGE_SIZE << pg->order;
}

void kfree(void *ptr) {
    if (!ptr) return;
    paddr_t p = virt_to_phys_hhdm(ptr);
    Page *pg = pmm_page(p);
    if (!pg) panic("kfree: %p is not heap memory", ptr);
    u64 f = irq_save();
    if (pg->flags & PG_SLAB) {
        int c = pg->slab_class;
        *(void **)ptr = g_freelist[c];
        g_freelist[c] = ptr;
        g_in_use -= g_class_size[c];
        irq_restore(f);
    } else if (pg->flags & PG_LARGE) {
        unsigned order = pg->order;
        g_in_use -= PAGE_SIZE << order;
        irq_restore(f);
        pmm_free(p, order);
    } else {
        irq_restore(f);
        panic("kfree: bad pointer %p (flags %#x)", ptr, pg->flags);
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
