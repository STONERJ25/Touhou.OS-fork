/* Memory management: physical frames (buddy allocator), kernel page tables, kernel heap. */
#ifndef K_MM_H
#define K_MM_H

#include "types.h"
#include "bootinfo.h"

/* ---- physical memory manager ---- */
#define PMM_MAX_ORDER 12            /* largest block: 4096 pages = 16 MiB */

#define PG_FREE     0x01            /* head of a free buddy block */
#define PG_RESERVED 0x02
#define PG_SLAB     0x04
#define PG_LARGE    0x08            /* head of a multi-page kmalloc allocation */
#define PG_USED     0x10

typedef struct Page {
    u32 next, prev;                 /* buddy free list links (page frame numbers) */
    u8  order;
    u8  flags;
    u8  slab_class;
    u8  pad;
    u32 refcount;
} Page;

const BootMemRegion *boot_mmap(u32 *count);
void    boot_mmap_save(const BootInfo *bi);          /* copies the loader's region list into kernel memory */
void    boot_mmap_report(void);                      /* log lines: how much memory of which kind, where RAM ends */

void    pmm_init(void);
paddr_t pmm_alloc(unsigned order);                  /* 0 on failure */
paddr_t pmm_alloc_below(unsigned order, paddr_t limit);     /* the whole block lies below limit (32-bit DMA); 0 on failure */
void    pmm_free(paddr_t p, unsigned order);
paddr_t pmm_alloc_page(void);
paddr_t pmm_alloc_page_zeroed(void);
void    pmm_free_page(paddr_t p);                   /* drops one reference; frees when it was the last */
void    pmm_get_page(paddr_t p);                    /* takes an extra reference on an allocated page */
bool    pmm_page_managed(paddr_t p);                /* ordinary allocated RAM, not device memory */
void    pmm_add_region(paddr_t base, u64 length);   /* hand memory to the allocator (boot reclaim) */
u64     pmm_total_pages(void);
u64     pmm_free_pages_count(void);
Page   *pmm_page(paddr_t p);
paddr_t pmm_ram_top(void);
void    pmm_reclaim_boot_memory(void);              /* frees BOOT_RECLAIMABLE regions; call after the boot page tables are gone */

/* ---- virtual memory ---- */
#define PTE_P    BIT(0)
#define PTE_W    BIT(1)
#define PTE_U    BIT(2)
#define PTE_PWT  BIT(3)
#define PTE_PCD  BIT(4)
#define PTE_A    BIT(5)
#define PTE_D    BIT(6)
#define PTE_PS   BIT(7)
#define PTE_G    BIT(8)
#define PTE_COW  BIT(9)             /* software: copy-on-write */
#define PTE_NX   BIT(63)
#define PTE_ADDR 0x000FFFFFFFFFF000UL

#define KERNEL_MMIO_BASE   0xFFFFC00000000000UL
#define KERNEL_STACK_BASE  0xFFFFC80000000000UL

void    vmm_init(void);
paddr_t vmm_kernel_pml4(void);
paddr_t vmm_new_user_pml4(void);                    /* kernel half shared, user half empty */
void    vmm_free_user_pml4(paddr_t pml4);           /* frees user page tables and the user pages they map */
int     vmm_map(paddr_t pml4, u64 virt, paddr_t phys, u64 flags);       /* 4 KiB page; 0 on success */
paddr_t vmm_unmap(paddr_t pml4, u64 virt);          /* returns the physical page that was mapped, or 0 */
paddr_t vmm_translate(paddr_t pml4, u64 virt, u64 *flags);              /* 0 if unmapped */
void    vmm_switch(paddr_t pml4);
void   *ioremap(paddr_t phys, size_t size, u64 flags);                  /* flags: PTE_PCD for UC, PTE_PWT for WC (PAT) */
void   *kstack_alloc(size_t size);                  /* guard-paged kernel stack, returns the LOW address */
void    kstack_free(void *base, size_t size);
int     kstack_slots_used(void);                    /* stacks alive (statistics) */

/* ---- kernel heap ---- */
void  *kmalloc(size_t size);
void  *kzalloc(size_t size);
void  *krealloc(void *p, size_t size);
void   kfree(void *p);
size_t kmalloc_usable(void *p);
u64    heap_bytes_in_use(void);
u64    heap_slab_pages(void);                       /* pages the small-object slabs hold (they are never given back) */
void   heap_init_debug(void);                       /* "heapdebug" on the command line: poison freed memory; call before the first allocation */

#endif
