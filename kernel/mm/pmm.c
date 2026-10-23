/* Physical memory manager: buddy allocator over page frames, bootstrapped from the loader's memory map. */
#include "mm.h"
#include "kernel.h"
#include "cpu.h"

#define MAX_REGIONS 512             /* real firmware describes 50-300 regions; the loader merges neighbours of the same type */
#define NIL 0xFFFFFFFFu

static BootMemRegion g_regions[MAX_REGIONS];
static u32 g_nregions;

static Page *g_pages;
static u64   g_max_pfn;             /* one past the highest frame we track */
static u32   g_free_head[PMM_MAX_ORDER + 1];
static u64   g_free_pages;
static u64   g_total_pages;
static paddr_t g_ram_top;

const BootMemRegion *boot_mmap(u32 *count) {
    *count = g_nregions;
    return g_regions;
}

void boot_mmap_save(const BootInfo *bi) {
    const BootMemRegion *src = phys_to_virt(bi->mmap_phys);
    g_nregions = bi->mmap_count > MAX_REGIONS ? MAX_REGIONS : bi->mmap_count;
    memcpy(g_regions, src, g_nregions * sizeof(BootMemRegion));
    if (bi->mmap_count > MAX_REGIONS)           /* the list is sorted by address: what is lost is the highest memory */
        klog("Boundary", "the firmware's memory map has %u regions; only the first %u are used", bi->mmap_count, MAX_REGIONS);
}

static bool is_ram_type(u32 t);

/* A log line on the shape of memory: what a photograph of the screen needs to tell a machine with 16 GiB and a big hole
 * below 4 GiB from one the loader misread. */
void boot_mmap_report(void) {
    u64 usable = 0, loader = 0, acpi = 0, rsvd = 0, mmio = 0, high = 0, top = 0;
    for (u32 i = 0; i < g_nregions; i++) {
        const BootMemRegion *r = &g_regions[i];
        u64 end = r->base + r->length;
        switch (r->type) {
        case BOOTMEM_USABLE: usable += r->length; break;
        case BOOTMEM_BOOT_RECLAIMABLE: case BOOTMEM_KERNEL: case BOOTMEM_MODULE: loader += r->length; break;
        case BOOTMEM_ACPI_RECLAIMABLE: case BOOTMEM_ACPI_NVS: acpi += r->length; break;
        case BOOTMEM_MMIO: mmio += r->length; break;
        default: rsvd += r->length; break;
        }
        if (is_ram_type(r->type) || r->type == BOOTMEM_ACPI_NVS) {
            if (end > top) top = end;
            if (end > (4ULL << 30)) high += end - (r->base > (4ULL << 30) ? r->base : (4ULL << 30));
        }
    }
    klog("Boundary", "memory map: %u regions, %lu MiB usable + %lu MiB loader + %lu MiB ACPI, %lu MiB reserved, %lu MiB device; RAM reaches %#lx, "
         "%lu MiB of it above 4 GiB", g_nregions, (unsigned long)(usable >> 20), (unsigned long)(loader >> 20), (unsigned long)(acpi >> 20),
         (unsigned long)(rsvd >> 20), (unsigned long)(mmio >> 20), (unsigned long)top, (unsigned long)(high >> 20));
}

static bool is_ram_type(u32 t) {
    return t == BOOTMEM_USABLE || t == BOOTMEM_BOOT_RECLAIMABLE || t == BOOTMEM_ACPI_RECLAIMABLE ||
           t == BOOTMEM_KERNEL || t == BOOTMEM_MODULE;
}

static void list_remove(unsigned order, u32 pfn) {
    Page *p = &g_pages[pfn];
    if (p->prev != NIL) g_pages[p->prev].next = p->next;
    else g_free_head[order] = p->next;
    if (p->next != NIL) g_pages[p->next].prev = p->prev;
    p->flags &= (u8)~PG_FREE;
}

static void list_add(unsigned order, u32 pfn) {
    Page *p = &g_pages[pfn];
    p->order = (u8)order;
    p->flags |= PG_FREE;
    p->prev = NIL;
    p->next = g_free_head[order];
    if (p->next != NIL) g_pages[p->next].prev = pfn;
    g_free_head[order] = pfn;
}

/* Adds [pfn, pfn+count) to the free lists as maximal aligned blocks (no coalescing needed: greedy is canonical). */
static void free_range(u64 pfn, u64 count) {
    while (count) {
        unsigned order = PMM_MAX_ORDER;
        while (order && ((pfn & ((1UL << order) - 1)) || (1UL << order) > count)) order--;
        for (u64 i = 0; i < (1UL << order); i++) g_pages[pfn + i].flags &= (u8)~(PG_RESERVED | PG_USED);
        list_add(order, (u32)pfn);
        g_free_pages += 1UL << order;
        pfn += 1UL << order;
        count -= 1UL << order;
    }
}

void pmm_init(void) {
    u64 top = 0;
    for (u32 i = 0; i < g_nregions; i++)
        if (is_ram_type(g_regions[i].type) && g_regions[i].base + g_regions[i].length > top)
            top = g_regions[i].base + g_regions[i].length;
    g_ram_top = top;
    g_max_pfn = top >> PAGE_SHIFT;

    /* Page database: carve it out of the first usable region that is large enough. */
    u64 db_bytes = ALIGN_UP(g_max_pfn * sizeof(Page), PAGE_SIZE);
    paddr_t db_phys = 0;
    for (u32 i = 0; i < g_nregions; i++) {
        const BootMemRegion *r = &g_regions[i];
        u64 base = r->base < 0x100000 ? 0x100000 : r->base;
        if (r->type == BOOTMEM_USABLE && r->base + r->length > base + db_bytes * 2) { db_phys = base; break; }
    }
    if (!db_phys) panic("pmm: no region large enough for the page database (%lu bytes)", (unsigned long)db_bytes);
    g_pages = phys_to_virt(db_phys);
    memset(g_pages, 0, db_bytes);
    for (u64 i = 0; i < g_max_pfn; i++) {
        g_pages[i].flags = PG_RESERVED;
        g_pages[i].next = g_pages[i].prev = NIL;
    }
    for (unsigned o = 0; o <= PMM_MAX_ORDER; o++) g_free_head[o] = NIL;

    for (u32 i = 0; i < g_nregions; i++) {
        const BootMemRegion *r = &g_regions[i];
        if (r->type != BOOTMEM_USABLE) continue;
        u64 start = r->base, end = r->base + r->length;
        if (start < 0x100000) start = 0x100000;                 /* keep the first MiB out of the pool */
        if (start >= end) continue;
        /* subtract the page database */
        u64 db_end = db_phys + db_bytes;
        if (start < db_end && end > db_phys) {
            if (start < db_phys) free_range(start >> PAGE_SHIFT, (db_phys - start) >> PAGE_SHIFT);
            start = db_end > start ? db_end : start;
            if (start >= end) continue;
        }
        free_range(start >> PAGE_SHIFT, (end - start) >> PAGE_SHIFT);
    }
    g_total_pages = g_free_pages;
}

void pmm_add_region(paddr_t base, u64 length) {
    u64 start = ALIGN_UP(base, PAGE_SIZE), end = ALIGN_DOWN(base + length, PAGE_SIZE);
    if (start < 0x100000) start = 0x100000;
    if (start >= end || (end >> PAGE_SHIFT) > g_max_pfn) return;
    u64 f = irq_save();
    free_range(start >> PAGE_SHIFT, (end - start) >> PAGE_SHIFT);
    g_total_pages += (end - start) >> PAGE_SHIFT;
    irq_restore(f);
}

void pmm_reclaim_boot_memory(void) {
    u64 reclaimed = 0;
    for (u32 i = 0; i < g_nregions; i++) {
        if (g_regions[i].type != BOOTMEM_BOOT_RECLAIMABLE) continue;
        pmm_add_region(g_regions[i].base, g_regions[i].length);
        reclaimed += g_regions[i].length;
        g_regions[i].type = BOOTMEM_USABLE;
    }
    klog("Boundary", "reclaimed %lu KiB of loader memory", (unsigned long)(reclaimed >> 10));
}

paddr_t pmm_alloc(unsigned order) {
    if (order > PMM_MAX_ORDER) return 0;
    u64 f = irq_save();
    unsigned o = order;
    while (o <= PMM_MAX_ORDER && g_free_head[o] == NIL) o++;
    if (o > PMM_MAX_ORDER) { irq_restore(f); return 0; }
    u32 pfn = g_free_head[o];
    list_remove(o, pfn);
    while (o > order) {                 /* split, keeping the lower half */
        o--;
        list_add(o, pfn + (1U << o));
    }
    g_pages[pfn].order = (u8)order;
    g_pages[pfn].flags |= PG_USED;
    g_pages[pfn].refcount = 1;
    g_free_pages -= 1UL << order;
    irq_restore(f);
    return (paddr_t)pfn << PAGE_SHIFT;
}

/* Like pmm_alloc, but the block must end below `limit`: devices that can only address 32 bits of memory (and every device
 * is cheaper to reason about that way) get their DMA buffers from the first 4 GiB even on a machine with 16 GiB of RAM. */
paddr_t pmm_alloc_below(unsigned order, paddr_t limit) {
    if (order > PMM_MAX_ORDER) return 0;
    u64 f = irq_save();
    for (unsigned o = order; o <= PMM_MAX_ORDER; o++) {
        for (u32 pfn = g_free_head[o]; pfn != NIL; pfn = g_pages[pfn].next) {
            paddr_t base = (paddr_t)pfn << PAGE_SHIFT;
            if (base + ((paddr_t)PAGE_SIZE << order) > limit) continue;
            list_remove(o, pfn);
            for (unsigned cur = o; cur > order;) {         /* split, keeping the lower half */
                cur--;
                list_add(cur, pfn + (1U << cur));
            }
            g_pages[pfn].order = (u8)order;
            g_pages[pfn].flags |= PG_USED;
            g_pages[pfn].refcount = 1;
            g_free_pages -= 1UL << order;
            irq_restore(f);
            return base;
        }
    }
    irq_restore(f);
    return 0;
}

void pmm_free(paddr_t p, unsigned order) {
    u64 f = irq_save();
    u64 pfn = p >> PAGE_SHIFT;
    if (pfn >= g_max_pfn || (pfn & ((1UL << order) - 1))) panic("pmm_free: bad block %#lx order %u", (unsigned long)p, order);
    if (g_pages[pfn].flags & PG_FREE) panic("pmm_free: double free of %#lx", (unsigned long)p);
    g_pages[pfn].flags &= (u8)~(PG_USED | PG_SLAB | PG_LARGE);
    g_pages[pfn].refcount = 0;
    g_free_pages += 1UL << order;
    while (order < PMM_MAX_ORDER) {
        u64 buddy = pfn ^ (1UL << order);
        if (buddy >= g_max_pfn) break;
        Page *b = &g_pages[buddy];
        if (!(b->flags & PG_FREE) || b->order != order) break;
        list_remove(order, (u32)buddy);
        if (buddy < pfn) pfn = buddy;
        order++;
    }
    list_add(order, (u32)pfn);
    irq_restore(f);
}

paddr_t pmm_alloc_page(void) { return pmm_alloc(0); }

paddr_t pmm_alloc_page_zeroed(void) {
    paddr_t p = pmm_alloc(0);
    if (p) memset(phys_to_virt(p), 0, PAGE_SIZE);
    return p;
}

/* Single pages are reference counted so a page can be shared (tmpfs pages mapped into several processes): the page goes
 * back to the allocator when the last reference is dropped. Larger blocks are never shared and use pmm_free directly. */
void pmm_free_page(paddr_t p) {
    u64 f = irq_save();
    Page *pg = pmm_page(p);
    if (pg && pg->refcount > 1) {
        pg->refcount--;
        irq_restore(f);
        return;
    }
    irq_restore(f);
    pmm_free(p, 0);
}

void pmm_get_page(paddr_t p) {
    u64 f = irq_save();
    Page *pg = pmm_page(p);
    if (pg) pg->refcount++;
    irq_restore(f);
}

/* True for an allocated page of ordinary RAM (as opposed to device memory such as the framebuffer or a BAR). */
bool pmm_page_managed(paddr_t p) {
    Page *pg = pmm_page(p);
    return pg && (pg->flags & PG_USED) && !(pg->flags & PG_RESERVED);
}

u64 pmm_total_pages(void) { return g_total_pages; }
u64 pmm_free_pages_count(void) { return g_free_pages; }
paddr_t pmm_ram_top(void) { return g_ram_top; }

Page *pmm_page(paddr_t p) {
    u64 pfn = p >> PAGE_SHIFT;
    return pfn < g_max_pfn ? &g_pages[pfn] : NULL;
}
