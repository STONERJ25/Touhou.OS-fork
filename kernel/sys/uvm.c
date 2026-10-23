/* User virtual memory: VMA list, demand paging, mmap/brk support, address-space clone and teardown. */
#include "process.h"
#include "kernel.h"
#include "cpu.h"
#include "cpuinfo.h"

u64 pte_flags_for(u32 prot, u32 vflags) {
    u64 f = PTE_U;
    if (prot & PROT_WRITE) f |= PTE_W;
    if (!(prot & PROT_EXEC)) f |= PTE_NX;
    if (vflags & VMA_WC) f |= pte_wc_flags();          /* write-combining (PAT entry 1), or uncached on a CPU without PAT */
    return f;
}

int uvm_new(AddrSpace *as) {
    memset(as, 0, sizeof *as);
    as->pml4 = vmm_new_user_pml4();
    if (!as->pml4) return -ENOMEM;
    as->mmap_next = USER_MMAP_BASE;
    mutex_init(&as->lock);
    return 0;
}

static Vma *vma_new(u64 s, u64 e, u32 prot, u32 flags) {
    Vma *v = kzalloc(sizeof *v);
    if (!v) return NULL;
    v->start = s;
    v->end = e;
    v->prot = prot;
    v->flags = flags;
    return v;
}

Vma *vma_find(AddrSpace *as, u64 addr) {
    for (Vma *v = as->vmas; v; v = v->next)
        if (addr >= v->start && addr < v->end) return v;
    return NULL;
}

int uvm_map(AddrSpace *as, u64 addr, u64 len, u32 prot, u32 flags) {
    if ((addr & (PAGE_SIZE - 1)) || (len & (PAGE_SIZE - 1)) || !len || addr >= USER_TOP || len > USER_TOP - addr)
        return -EINVAL;
    for (Vma *v = as->vmas; v; v = v->next)
        if (addr < v->end && addr + len > v->start) return -EEXIST;
    Vma *v = vma_new(addr, addr + len, prot, flags);
    if (!v) return -ENOMEM;
    v->next = as->vmas;
    as->vmas = v;
    return 0;
}

int uvm_populate(AddrSpace *as, u64 addr, u64 len) {
    for (u64 va = ALIGN_DOWN(addr, PAGE_SIZE); va < addr + len; va += PAGE_SIZE) {
        if (vmm_translate(as->pml4, va, NULL)) continue;
        Vma *v = vma_find(as, va);
        if (!v) return -EFAULT;
        paddr_t p = pmm_alloc_page_zeroed();
        if (!p) return -ENOMEM;
        if (vmm_map(as->pml4, va, p, pte_flags_for(v->prot, v->flags))) { pmm_free_page(p); return -ENOMEM; }
    }
    return 0;
}

int uvm_unmap(AddrSpace *as, u64 addr, u64 len) {
    u64 end = addr + len;
    Vma **pp = &as->vmas;
    while (*pp) {
        Vma *v = *pp;
        if (v->end <= addr || v->start >= end) { pp = &v->next; continue; }
        u64 lo = MAX(v->start, addr), hi = MIN(v->end, end);
        for (u64 va = lo; va < hi; va += PAGE_SIZE) {
            paddr_t p = vmm_unmap(as->pml4, va);
            if (!p) continue;
            if (!(v->flags & VMA_IO)) pmm_free_page(p);
            else if (pmm_page_managed(p)) pmm_free_page(p);             /* drop the reference taken when it was mapped */
        }
        if (lo <= v->start && hi >= v->end) { *pp = v->next; kfree(v); continue; }
        if (lo <= v->start) {
            v->start = hi;
        } else if (hi >= v->end) {
            v->end = lo;
        } else {
            Vma *tail = vma_new(hi, v->end, v->prot, v->flags);
            if (!tail) return -ENOMEM;
            tail->next = v->next;
            v->next = tail;
            v->end = lo;
            pp = &tail->next;
            continue;
        }
        pp = &v->next;
    }
    return 0;
}

static int split_at(AddrSpace *as, u64 addr) {
    Vma *v = vma_find(as, addr);
    if (!v || v->start == addr) return 0;
    Vma *tail = vma_new(addr, v->end, v->prot, v->flags);
    if (!tail) return -ENOMEM;
    tail->next = v->next;
    v->next = tail;
    v->end = addr;
    return 0;
}

int uvm_protect(AddrSpace *as, u64 addr, u64 len, u32 prot) {
    u64 end = addr + len;
    int rc = split_at(as, addr);
    if (!rc) rc = split_at(as, end);
    if (rc) return rc;
    for (Vma *v = as->vmas; v; v = v->next) {
        if (v->start < addr || v->end > end) continue;
        v->prot = prot;
        for (u64 va = v->start; va < v->end; va += PAGE_SIZE) {
            paddr_t p = vmm_translate(as->pml4, va, NULL);
            if (p) vmm_map(as->pml4, va, p & PTE_ADDR, pte_flags_for(prot, v->flags));
        }
    }
    return 0;
}

u64 uvm_find_free(AddrSpace *as, u64 len) {
    u64 addr = as->mmap_next;
    for (;;) {
        bool clash = false;
        for (Vma *v = as->vmas; v; v = v->next) {
            if (addr < v->end && addr + len > v->start) { addr = ALIGN_UP(v->end, PAGE_SIZE); clash = true; }
        }
        if (!clash) break;
    }
    if (addr + len > USER_STACK_TOP - USER_STACK_MAX) return 0;
    as->mmap_next = addr + len;
    return addr;
}

int uvm_clone(AddrSpace *dst, const AddrSpace *src) {
    dst->brk_start = src->brk_start;
    dst->brk = src->brk;
    dst->mmap_next = src->mmap_next;
    for (Vma *v = src->vmas; v; v = v->next) {
        Vma *nv = vma_new(v->start, v->end, v->prot, v->flags);
        if (!nv) return -ENOMEM;
        nv->next = dst->vmas;
        dst->vmas = nv;
        for (u64 va = v->start; va < v->end; va += PAGE_SIZE) {
            u64 fl = 0;
            paddr_t sp = vmm_translate(src->pml4, va, &fl);
            if (!sp) continue;
            sp &= PTE_ADDR;
            fl &= ~(PTE_A | PTE_D);
            if (v->flags & VMA_IO) {
                if (vmm_map(dst->pml4, va, sp, fl)) return -ENOMEM;     /* shared device/shared-buffer pages */
                if (pmm_page_managed(sp)) pmm_get_page(sp);
                continue;
            }
            paddr_t np = pmm_alloc_page();
            if (!np) return -ENOMEM;
            memcpy(phys_to_virt(np), phys_to_virt(sp), PAGE_SIZE);
            if (vmm_map(dst->pml4, va, np, fl)) { pmm_free_page(np); return -ENOMEM; }
        }
    }
    return 0;
}

void uvm_destroy(AddrSpace *as) {
    if (!as->pml4) return;
    for (Vma *v = as->vmas; v; ) {
        Vma *next = v->next;
        if (v->flags & VMA_IO)
            for (u64 va = v->start; va < v->end; va += PAGE_SIZE) {
                paddr_t p = vmm_unmap(as->pml4, va);                     /* detach; shared RAM pages just lose one reference */
                if (p && pmm_page_managed(p)) pmm_free_page(p);
            }
        kfree(v);
        v = next;
    }
    as->vmas = NULL;
    vmm_free_user_pml4(as->pml4);
    as->pml4 = 0;
}

static bool uvm_fault_locked(AddrSpace *as, TrapFrame *tf, u64 addr);

/* Demand paging. Another thread may be in the middle of mmap/munmap: wait for it (a fault taken with interrupts on may sleep). */
bool uvm_fault(AddrSpace *as, TrapFrame *tf, u64 addr) {
    if (!as->pml4 || addr >= USER_TOP) return false;
    if (!mutex_trylock(&as->lock)) {
        if (!(tf->rflags & RFLAGS_IF)) return false;        /* interrupted code that could not be preempted: cannot wait either */
        sti();
        mutex_lock(&as->lock);
        cli();
    }
    bool ok = uvm_fault_locked(as, tf, addr);
    mutex_unlock(&as->lock);
    return ok;
}

static bool uvm_fault_locked(AddrSpace *as, TrapFrame *tf, u64 addr) {
    if (!as->pml4) return false;                            /* the process was torn down while we waited */
    Vma *v = vma_find(as, addr);
    if (!v || (v->flags & VMA_IO)) return false;
    u64 err = tf->error;
    if (err & 1) return false;                              /* present page: a real protection violation */
    if (vmm_translate(as->pml4, addr, NULL)) return true;   /* another thread mapped it while we waited: just retry */
    if ((err & 2) && !(v->prot & PROT_WRITE)) return false;
    if ((err & 16) && !(v->prot & PROT_EXEC)) return false;
    if (!(v->prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) return false;
    paddr_t p = pmm_alloc_page_zeroed();
    if (!p) {
        klog("Yuyuko", "out of memory while mapping page %#lx: Yuyuko is hungry", (unsigned long)addr);
        return false;
    }
    if (vmm_map(as->pml4, addr & ~(PAGE_SIZE - 1), p, pte_flags_for(v->prot, v->flags))) {
        pmm_free_page(p);
        return false;
    }
    return true;
}

int uvm_write(AddrSpace *as, u64 uaddr, const void *src, size_t n) {
    const u8 *s = src;
    while (n) {
        paddr_t p = vmm_translate(as->pml4, uaddr, NULL);
        if (!p) return -EFAULT;
        size_t chunk = MIN(n, PAGE_SIZE - (uaddr & (PAGE_SIZE - 1)));
        memcpy(phys_to_virt(p), s, chunk);
        uaddr += chunk;
        s += chunk;
        n -= chunk;
    }
    return 0;
}
