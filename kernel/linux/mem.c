/*
 * The memory calls that a C library and a dynamic linker make beyond mmap, munmap and mprotect (which are the native ones, kernel/sys/syscalls.c): madvise and mremap
 * (docs/COMPAT.md, L2). Written from the manual pages (madvise(2), mremap(2)); the behaviour is pinned by the differential test map_tour (tests/linux/src/map_tour.c).
 */
#include "linux.h"
#include "kernel.h"
#include "cpu.h"
#include "calls.h"

#define LNX_MADV_NORMAL      0
#define LNX_MADV_RANDOM      1
#define LNX_MADV_SEQUENTIAL  2
#define LNX_MADV_WILLNEED    3
#define LNX_MADV_DONTNEED    4
#define LNX_MADV_FREE        8
#define LNX_MADV_REMOVE      9
#define LNX_MADV_DONTFORK    10
#define LNX_MADV_DOFORK      11
#define LNX_MADV_MERGEABLE   12
#define LNX_MADV_UNMERGEABLE 13
#define LNX_MADV_HUGEPAGE    14
#define LNX_MADV_NOHUGEPAGE  15
#define LNX_MADV_DONTDUMP    16
#define LNX_MADV_DODUMP      17
#define LNX_MADV_WIPEONFORK  18
#define LNX_MADV_KEEPONFORK  19
#define LNX_MADV_COLD        20
#define LNX_MADV_PAGEOUT     21

#define LNX_MREMAP_MAYMOVE   1
#define LNX_MREMAP_FIXED     2
#define LNX_MREMAP_DONTUNMAP 4

static inline Process *me(void) { return current->proc; }

/* Is every page of [addr, end) in some mapping? */
static bool fully_mapped(AddrSpace *as, u64 addr, u64 end) {
    for (u64 va = addr; va < end; ) {
        Vma *v = vma_find(as, va);
        if (!v) return false;
        va = v->end;
    }
    return true;
}

/* madvise(addr, length, advice). The advice that only tunes the page cache or the kernel's merging does nothing here. MADV_DONTNEED and MADV_FREE on private memory that was not
 * filled from a file give the pages back: the next access finds zeros, which is what a C library relies on when it recycles a heap. (A mapping filled from a file would have to
 * read the file again, so it is left as it is.) */
long lnx_madvise(TrapFrame *tf) {
    u64 addr = A0(tf), len = ALIGN_UP(A1(tf), PAGE_SIZE);
    int advice = (int)A2(tf);
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    if (A1(tf) > ~0UL - PAGE_SIZE) return -EINVAL;
    switch (advice) {
    case LNX_MADV_NORMAL: case LNX_MADV_RANDOM: case LNX_MADV_SEQUENTIAL: case LNX_MADV_WILLNEED: case LNX_MADV_DONTNEED: case LNX_MADV_FREE: case LNX_MADV_REMOVE:
    case LNX_MADV_DONTFORK: case LNX_MADV_DOFORK: case LNX_MADV_MERGEABLE: case LNX_MADV_UNMERGEABLE: case LNX_MADV_HUGEPAGE: case LNX_MADV_NOHUGEPAGE:
    case LNX_MADV_DONTDUMP: case LNX_MADV_DODUMP: case LNX_MADV_WIPEONFORK: case LNX_MADV_KEEPONFORK: case LNX_MADV_COLD: case LNX_MADV_PAGEOUT:
        break;
    default:
        return -EINVAL;
    }
    if (!len) return 0;
    if (addr >= USER_TOP || len > USER_TOP - addr) return -ENOMEM;
    AddrSpace *as = &me()->as;
    mutex_lock(&as->lock);
    long rc = 0;
    if (!fully_mapped(as, addr, addr + len)) rc = -ENOMEM;
    else if (advice == LNX_MADV_DONTNEED || advice == LNX_MADV_FREE) {
        for (u64 va = addr; va < addr + len; va += PAGE_SIZE) {
            Vma *v = vma_find(as, va);
            if (!v || (v->flags & (VMA_IO | VMA_FILE))) continue;
            paddr_t p = vmm_unmap(as->pml4, va);
            if (p) pmm_free_page(p);
        }
    }
    mutex_unlock(&as->lock);
    return rc;
}

/* mremap(old_address, old_size, new_size, flags): shrink in place, grow in place when the pages after are free, else (MREMAP_MAYMOVE) move the mapping, taking its pages along. */
long lnx_mremap(TrapFrame *tf) {
    u64 old = A0(tf), osz = ALIGN_UP(A1(tf), PAGE_SIZE), nsz = ALIGN_UP(A2(tf), PAGE_SIZE);
    int flags = (int)A3(tf);
    if ((flags & ~(LNX_MREMAP_MAYMOVE | LNX_MREMAP_FIXED | LNX_MREMAP_DONTUNMAP)) || (flags & (LNX_MREMAP_FIXED | LNX_MREMAP_DONTUNMAP))) return -EINVAL;
    if ((old & (PAGE_SIZE - 1)) || !nsz || !osz || A1(tf) > ~0UL - PAGE_SIZE || A2(tf) > ~0UL - PAGE_SIZE) return -EINVAL;
    if (old >= USER_TOP || osz > USER_TOP - old || nsz > USER_TOP) return -EFAULT;
    AddrSpace *as = &me()->as;
    mutex_lock(&as->lock);
    long rc;
    Vma *v = vma_find(as, old);
    if (!v || !fully_mapped(as, old, old + osz)) {
        rc = -EFAULT;                                       /* part of the range is not mapped */
    } else if (v->flags & VMA_IO) {
        rc = -EINVAL;                                       /* device and shared buffers are not moved */
    } else if (nsz == osz) {
        rc = (long)old;
    } else if (nsz < osz) {                                 /* shrinking: the tail goes, whatever mappings it spans */
        rc = uvm_unmap(as, old + nsz, osz - nsz) ? -ENOMEM : (long)old;
    } else if (old + osz > v->end) {
        rc = -EFAULT;                                       /* growing needs a range inside one mapping */
    } else {
        u32 prot = v->prot, vflags = v->flags & ~(u32)(VMA_STACK | VMA_HEAP);
        bool room = old + osz == v->end && old + nsz <= USER_STACK_TOP - USER_STACK_MAX;
        for (Vma *w = as->vmas; room && w; w = w->next)
            if (w != v && old + osz < w->end && old + nsz > w->start) room = false;
        if (room) {                                         /* the pages after the mapping are free: the mapping simply grows */
            v->end = old + nsz;
            rc = (long)old;
        } else if (!(flags & LNX_MREMAP_MAYMOVE)) {
            rc = -ENOMEM;
        } else {
            u64 na = uvm_find_free(as, nsz);
            rc = na ? uvm_map(as, na, nsz, prot, vflags) : -ENOMEM;
            if (!rc) {
                for (u64 o = 0; o < osz; o += PAGE_SIZE) {  /* the pages move with their contents: no copy */
                    paddr_t p = vmm_translate(as->pml4, old + o, NULL);
                    if (!p) continue;
                    vmm_unmap(as->pml4, old + o);
                    if (vmm_map(as->pml4, na + o, p & PTE_ADDR, pte_flags_for(prot, vflags))) { pmm_free_page(p & PTE_ADDR); }
                }
                uvm_unmap(as, old, osz);
                rc = (long)na;
            }
        }
    }
    mutex_unlock(&as->lock);
    return rc;
}
