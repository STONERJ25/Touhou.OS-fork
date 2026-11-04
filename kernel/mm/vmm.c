/* Virtual memory: kernel page tables (direct map, image, MMIO, stacks) and per-process user page tables. */
#include "mm.h"
#include "kernel.h"
#include "cpu.h"

extern char __text_start[], __text_end[], __rodata_start[], __rodata_end[];
extern char __data_start[], __bss_end[], __ksyms_start[], __ksyms_end[];

extern u64 g_kernel_phys, g_kernel_virt;         /* set by kmain from BootInfo */

#define SLOT_HHDM   256
#define SLOT_MMIO   384
#define SLOT_KSTACK 400
#define SLOT_KERNEL 511

static paddr_t g_kernel_pml4;
static u64 g_mmio_next   = KERNEL_MMIO_BASE;

static inline u64 *tbl(paddr_t p) { return phys_to_virt(p); }

static u64 *next_level(u64 *table, unsigned idx, bool create, u64 user) {
    u64 e = table[idx];
    if (!(e & PTE_P)) {
        if (!create) return NULL;
        paddr_t p = pmm_alloc_page_zeroed();
        if (!p) return NULL;
        table[idx] = p | PTE_P | PTE_W | user;
        return tbl(p);
    }
    if (e & PTE_PS) return NULL;
    if (user && !(e & PTE_U)) table[idx] = e | PTE_U;
    return tbl(e & PTE_ADDR);
}

/* A change below the kernel half is shared by every address space: whichever one is loaded, its TLB may hold the old entry. */
static inline void flush_one(paddr_t pml4, u64 virt) {
    if (virt >= 0xFFFF800000000000UL || read_cr3() == pml4) invlpg((void *)virt);
}

int vmm_map(paddr_t pml4, u64 virt, paddr_t phys, u64 flags) {
    u64 user = flags & PTE_U;
    u64 *l3 = next_level(tbl(pml4), (virt >> 39) & 511, true, user);
    if (!l3) return -1;
    u64 *l2 = next_level(l3, (virt >> 30) & 511, true, user);
    if (!l2) return -1;
    u64 *l1 = next_level(l2, (virt >> 21) & 511, true, user);
    if (!l1) return -1;
    l1[(virt >> 12) & 511] = (phys & PTE_ADDR) | flags | PTE_P;
    flush_one(pml4, virt);
    return 0;
}

static u64 *walk_leaf(paddr_t pml4, u64 virt) {
    u64 *l3 = next_level(tbl(pml4), (virt >> 39) & 511, false, 0);
    if (!l3) return NULL;
    u64 *l2 = next_level(l3, (virt >> 30) & 511, false, 0);
    if (!l2) return NULL;
    u64 *l1 = next_level(l2, (virt >> 21) & 511, false, 0);
    if (!l1) return NULL;
    return &l1[(virt >> 12) & 511];
}

paddr_t vmm_unmap(paddr_t pml4, u64 virt) {
    u64 *pte = walk_leaf(pml4, virt);
    if (!pte || !(*pte & PTE_P)) return 0;
    paddr_t p = *pte & PTE_ADDR;
    *pte = 0;
    flush_one(pml4, virt);
    return p;
}

paddr_t vmm_translate(paddr_t pml4, u64 virt, u64 *flags) {
    u64 *pte = walk_leaf(pml4, virt);
    if (!pte || !(*pte & PTE_P)) return 0;
    if (flags) *flags = *pte & ~PTE_ADDR;
    return (*pte & PTE_ADDR) | (virt & 0xFFF);
}

void vmm_switch(paddr_t pml4) {
    if (read_cr3() != pml4) write_cr3(pml4);
}

paddr_t vmm_kernel_pml4(void) { return g_kernel_pml4; }

paddr_t vmm_new_user_pml4(void) {
    paddr_t p = pmm_alloc_page_zeroed();
    if (!p) return 0;
    u64 *dst = tbl(p), *src = tbl(g_kernel_pml4);
    for (int i = 256; i < 512; i++) dst[i] = src[i];
    return p;
}

static void free_level(u64 *table, int level) {
    for (int i = 0; i < 512; i++) {
        u64 e = table[i];
        if (!(e & PTE_P)) continue;
        if (level == 1) {
            if (!(e & PTE_COW) || true) pmm_free_page(e & PTE_ADDR);       /* leaf user page */
        } else if (!(e & PTE_PS)) {
            free_level(tbl(e & PTE_ADDR), level - 1);
            pmm_free_page(e & PTE_ADDR);
        }
    }
}

void vmm_free_user_pml4(paddr_t pml4) {
    u64 *l4 = tbl(pml4);
    for (int i = 0; i < 256; i++) {
        if (!(l4[i] & PTE_P)) continue;
        free_level(tbl(l4[i] & PTE_ADDR), 3);
        pmm_free_page(l4[i] & PTE_ADDR);
    }
    pmm_free_page(pml4);
}

static void map_direct_range(paddr_t start, paddr_t end) {
    u64 *l4 = tbl(g_kernel_pml4);
    while (start < end) {
        u64 virt = g_hhdm + start;
        if (!(start & (2UL * 1024 * 1024 - 1)) && start + 2UL * 1024 * 1024 <= end) {
            u64 *l3 = next_level(l4, SLOT_HHDM, true, 0);
            u64 *l2 = next_level(l3, (virt >> 30) & 511, true, 0);
            l2[(virt >> 21) & 511] = start | PTE_P | PTE_W | PTE_PS | PTE_NX;
            start += 2UL * 1024 * 1024;
        } else {
            vmm_map(g_kernel_pml4, virt, start, PTE_W | PTE_NX);
            start += PAGE_SIZE;
        }
    }
}

static void map_image(char *from, char *to, u64 flags) {
    for (u64 v = (u64)from; v < (u64)to; v += PAGE_SIZE)
        vmm_map(g_kernel_pml4, v, g_kernel_phys + (v - g_kernel_virt), flags);
}

void vmm_init(void) {
    g_kernel_pml4 = pmm_alloc_page_zeroed();
    u64 *l4 = tbl(g_kernel_pml4);

    /* Top-level slots the kernel uses get their PDPT now, so every user PML4 that copies them shares the sub-trees. */
    static const int slots[] = { SLOT_HHDM, SLOT_MMIO, SLOT_KSTACK, SLOT_KERNEL };
    for (unsigned i = 0; i < ARRAY_SIZE(slots); i++)
        l4[slots[i]] = pmm_alloc_page_zeroed() | PTE_P | PTE_W;

    u32 n;
    const BootMemRegion *r = boot_mmap(&n);
    for (u32 i = 0; i < n; i++) {
        u32 t = r[i].type;
        if (t == BOOTMEM_USABLE || t == BOOTMEM_BOOT_RECLAIMABLE || t == BOOTMEM_ACPI_RECLAIMABLE ||
            t == BOOTMEM_ACPI_NVS || t == BOOTMEM_KERNEL || t == BOOTMEM_MODULE)
            map_direct_range(r[i].base, r[i].base + r[i].length);
    }

    map_image(__text_start, __text_end, 0);                         /* R X */
    map_image(__rodata_start, __rodata_end, PTE_NX);                /* R */
    map_image(__data_start, __bss_end, PTE_W | PTE_NX);             /* RW */
    map_image(__ksyms_start, (char *)ALIGN_UP((u64)__ksyms_end, PAGE_SIZE), PTE_NX);

    write_cr3(g_kernel_pml4);
}

void *ioremap(paddr_t phys, size_t size, u64 flags) {
    u64 off = phys & 0xFFF;
    u64 pages = (off + size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    u64 virt = g_mmio_next;
    g_mmio_next += pages << PAGE_SHIFT;
    for (u64 i = 0; i < pages; i++)
        vmm_map(g_kernel_pml4, virt + (i << PAGE_SHIFT), (phys & PTE_ADDR) + (i << PAGE_SHIFT), PTE_W | PTE_NX | flags);
    return (void *)(virt + off);
}

/* Kernel stacks live in slots of the stack region: [guard page | stack], every slot the same size, handed out lowest first and
 * given back when the task is reaped, so a machine that starts and ends processes for months neither runs out of address
 * space nor accumulates page-table pages for stacks that are long gone. */
#define KSTACK_SLOTS 8192
static u64 g_kstack_map[KSTACK_SLOTS / 64];                         /* 1 = in use */
static u64 g_kstack_slot_pages;                                     /* pages per slot (guard included), fixed by the first stack */
static int g_kstack_used;

int kstack_slots_used(void) { return g_kstack_used; }

void *kstack_alloc(size_t size) {
    u64 pages = ALIGN_UP(size, PAGE_SIZE) >> PAGE_SHIFT;
    u64 u = irq_save();
    if (!g_kstack_slot_pages) g_kstack_slot_pages = pages + 1;
    if (g_kstack_slot_pages != pages + 1) { irq_restore(u); return NULL; }         /* one stack size only */
    int slot = -1;
    for (int w = 0; w < KSTACK_SLOTS / 64 && slot < 0; w++)
        if (g_kstack_map[w] != ~0UL) slot = w * 64 + __builtin_ctzl(~g_kstack_map[w]);
    if (slot < 0) { irq_restore(u); return NULL; }
    g_kstack_map[slot / 64] |= 1UL << (slot % 64);
    g_kstack_used++;
    irq_restore(u);
    u64 base = KERNEL_STACK_BASE + (u64)slot * (g_kstack_slot_pages << PAGE_SHIFT) + PAGE_SIZE;     /* one unmapped guard page below */
    for (u64 i = 0; i < pages; i++) {
        paddr_t p = pmm_alloc_page_zeroed();
        if (!p || vmm_map(g_kernel_pml4, base + (i << PAGE_SHIFT), p, PTE_W | PTE_NX)) {
            for (u64 j = 0; j < i; j++) pmm_free_page(vmm_unmap(g_kernel_pml4, base + (j << PAGE_SHIFT)));
            if (p) pmm_free_page(p);
            u64 v = irq_save();
            g_kstack_map[slot / 64] &= ~(1UL << (slot % 64));
            g_kstack_used--;
            irq_restore(v);
            return NULL;
        }
    }
    return (void *)base;
}

void kstack_free(void *base, size_t size) {
    u64 pages = ALIGN_UP(size, PAGE_SIZE) >> PAGE_SHIFT;
    for (u64 i = 0; i < pages; i++) {
        paddr_t p = vmm_unmap(g_kernel_pml4, (u64)base + (i << PAGE_SHIFT));
        if (p) pmm_free_page(p);
    }
    u64 slot = ((u64)base - PAGE_SIZE - KERNEL_STACK_BASE) / (g_kstack_slot_pages << PAGE_SHIFT);
    u64 u = irq_save();
    if (slot < KSTACK_SLOTS && (g_kstack_map[slot / 64] & (1UL << (slot % 64)))) {
        g_kstack_map[slot / 64] &= ~(1UL << (slot % 64));
        g_kstack_used--;
    }
    irq_restore(u);
}
