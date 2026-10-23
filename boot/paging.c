/*
 * Boot page tables and the final jump into the kernel.
 *
 * The loader builds a throwaway address space with three pieces:
 *   - HHDM_BASE + phys: all RAM (2 MiB pages, RW + NX) so the kernel can reach any physical page
 *   - KERNEL_VIRT_BASE...: the kernel image, 4 KiB pages with per-segment permissions
 *   - a small identity window around the handoff code so the instruction after "mov cr3" still exists
 * The kernel builds its own tables as soon as its allocator is up and then frees these.
 */
#include "loader.h"
#include "../shared/elf.h"

#define PTE_P    (1ULL << 0)
#define PTE_RW   (1ULL << 1)
#define PTE_PS   (1ULL << 7)
#define PTE_NX   (1ULL << 63)
#define PTE_ADDR 0x000FFFFFFFFFF000ULL
#define MIB2     (2ULL << 20)

static UINT64 g_arena_base, g_arena_size, g_arena_used;

void arena_init(UINT64 base, UINT64 size) {
    g_arena_base = base;
    g_arena_size = size;
    g_arena_used = 0;
    memset((void *)base, 0, size);
}

void *arena_alloc(UINT64 size, UINT64 align) {
    UINT64 off = (g_arena_used + align - 1) & ~(align - 1);
    if (off + size > g_arena_size) {
        log_str("loader: boot arena exhausted\n");
        loader_halt();
    }
    g_arena_used = off + size;
    return (void *)(g_arena_base + off);    /* arena_init zeroed everything */
}

static UINT64 *next_table(UINT64 *table, UINT64 idx) {
    if (!(table[idx] & PTE_P)) {
        UINT64 *fresh = arena_alloc(4096, 4096);
        table[idx] = (UINT64)fresh | PTE_P | PTE_RW;    /* permissive here; leaf entries decide */
    }
    return (UINT64 *)(table[idx] & PTE_ADDR);
}

static void map_4k(UINT64 *pml4, UINT64 virt, UINT64 phys, UINT64 flags) {
    UINT64 *pdpt = next_table(pml4, (virt >> 39) & 0x1FF);
    UINT64 *pd   = next_table(pdpt, (virt >> 30) & 0x1FF);
    UINT64 *pt   = next_table(pd,   (virt >> 21) & 0x1FF);
    pt[(virt >> 12) & 0x1FF] = phys | flags | PTE_P;
}

static void map_2m(UINT64 *pml4, UINT64 virt, UINT64 phys, UINT64 flags) {
    UINT64 *pdpt = next_table(pml4, (virt >> 39) & 0x1FF);
    UINT64 *pd   = next_table(pdpt, (virt >> 30) & 0x1FF);
    pd[(virt >> 21) & 0x1FF] = phys | flags | PTE_P | PTE_PS;
}

UINT64 build_page_tables(const KernelImage *k, UINT64 max_ram_end, UINT64 handoff_code_addr) {
    UINT64 *pml4 = arena_alloc(4096, 4096);

    /* Direct map: at least the first 4 GiB (framebuffer and legacy MMIO live there), capped at 128 GiB. */
    UINT64 top = max_ram_end;
    if (top < (4ULL << 30)) top = 4ULL << 30;
    if (top > (128ULL << 30)) top = 128ULL << 30;
    top = (top + MIB2 - 1) & ~(MIB2 - 1);
    for (UINT64 p = 0; p < top; p += MIB2)
        map_2m(pml4, HHDM_BASE + p, p, PTE_RW | PTE_NX);

    /* Kernel image, page by page with the permissions its ELF segments ask for. */
    for (UINT32 i = 0; i < k->nseg; i++) {
        const KernelSegment *s = &k->seg[i];
        UINT64 start = s->vaddr & ~0xFFFULL;
        UINT64 end   = (s->vaddr + s->memsz + 0xFFF) & ~0xFFFULL;
        UINT64 flags = 0;
        if (s->flags & PF_W) flags |= PTE_RW;
        if (!(s->flags & PF_X)) flags |= PTE_NX;
        for (UINT64 v = start; v < end; v += 4096)
            map_4k(pml4, v, k->phys_base + (v - k->virt_base), flags);
    }

    /* Identity window (executable) covering the handoff function, even if it straddles a 2 MiB boundary. */
    UINT64 w = handoff_code_addr & ~(MIB2 - 1);
    map_2m(pml4, w, w, PTE_RW);
    map_2m(pml4, w + MIB2, w + MIB2, PTE_RW);

    return (UINT64)pml4;
}

/*
 * Switch to the new address space and enter the kernel (SysV ABI: BootInfo* in rdi).
 * Everything lives in one asm block: after the cr3 write the next instruction is fetched
 * through the identity window, and the old stack is never touched again.
 */
__attribute__((noreturn, noinline))
void handoff(UINT64 pml4_phys, UINT64 entry, UINT64 stack_top, UINT64 bootinfo_virt) {
    __asm__ volatile (
        "cli\n\t"
        "mov %%rax, %%cr3\n\t"
        "mov %%rbx, %%rsp\n\t"
        "xor %%ebp, %%ebp\n\t"
        "jmp *%%rcx\n\t"
        :
        : "a"(pml4_phys), "b"(stack_top), "c"(entry), "D"(bootinfo_virt)
        : "memory");
    __builtin_unreachable();
}
