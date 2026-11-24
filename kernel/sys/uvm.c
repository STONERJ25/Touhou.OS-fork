/* User virtual memory: VMA list, demand paging, mmap/brk support, address-space clone and teardown. */
#include "process.h"
#include "kernel.h"
#include "cpu.h"
#include "cpuinfo.h"
#include "fb.h"
#include "cmdline.h"

u64 pte_flags_for(u32 prot, u32 vflags) {
    u64 f = (prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) ? PTE_U : 0;       /* PROT_NONE: a page that is there but that user code may not touch (a guard page that was already in use) */
    if (prot & PROT_WRITE) f |= PTE_W;
    if (!(prot & PROT_EXEC)) f |= PTE_NX;
    if (vflags & VMA_WC) f |= g_fb_force_uc ? (PTE_PCD | PTE_PWT) : pte_wc_flags();     /* write-combining (PAT entry 1), uncached without PAT or with fb.cache=uc */
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

/* ---- the file a mapping was made from (VmaFile, process.h): only /proc/<pid>/maps reads it ---- */

static void vmafile_put(VmaFile *f) {
    if (f && __atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) == 0) kfree(f);
}

static void vma_free(Vma *v) {
    vmafile_put(v->file);
    kfree(v);
}

void uvm_copy_file(Vma *to, const Vma *from) {
    to->file = from->file;
    if (!to->file) return;
    __atomic_add_fetch(&to->file->refs, 1, __ATOMIC_RELAXED);
    to->file_off = from->file_off + (to->start - from->start);
}

void uvm_set_file(AddrSpace *as, u64 addr, u64 len, const char *path, bool canonical, u64 dev, u64 ino, u64 off) {
    Vma *v = vma_find(as, addr);
    if (!v || v->start != addr || v->end != addr + len || !path || !path[0]) return;
    char canon[256];
    if (!canonical) {
        path_canonical(NULL, path, canon, sizeof canon);
        path = canon;
    }
    VmaFile *f = kmalloc(sizeof *f + strlen(path) + 1);
    if (!f) return;                                         /* the mapping stays anonymous: a name is only for /proc */
    f->refs = 1;
    f->dev = dev;
    f->ino = ino;
    strcpy(f->path, path);
    vmafile_put(v->file);
    v->file = f;
    v->file_off = off;
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
    int lim = rlimit_vm_check(as, len, prot, flags);                    /* RLIMIT_AS and RLIMIT_DATA of the running process (kernel/sys/rlimit.c) */
    if (lim) return lim;
    Vma *v = vma_new(addr, addr + len, prot, flags);
    if (!v) return -ENOMEM;
    v->next = as->vmas;
    as->vmas = v;
    return 0;
}

int uvm_populate(AddrSpace *as, u64 addr, u64 len) {
    bool can_sleep = (read_rflags() & RFLAGS_IF) != 0;
    for (u64 va = ALIGN_DOWN(addr, PAGE_SIZE); va < addr + len; va += PAGE_SIZE) {
        if (vmm_translate(as->pml4, va, NULL)) continue;
        Vma *v = vma_find(as, va);
        if (!v) return -EFAULT;
        paddr_t p = oom_alloc_user_page(true, can_sleep);          /* memory for a user process: the reserve and the OOM killer (kernel/mm/oom.c) */
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
        if (lo <= v->start && hi >= v->end) { *pp = v->next; vma_free(v); continue; }
        if (lo <= v->start) {
            v->file_off += hi - v->start;
            v->start = hi;
        } else if (hi >= v->end) {
            v->end = lo;
        } else {
            Vma *tail = vma_new(hi, v->end, v->prot, v->flags);
            if (!tail) return -ENOMEM;
            uvm_copy_file(tail, v);
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
    uvm_copy_file(tail, v);
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
            u64 ofl = 0;
            paddr_t p = vmm_translate(as->pml4, va, &ofl);
            if (p) vmm_map(as->pml4, va, p & PTE_ADDR, uvm_keep_cow(pte_flags_for(prot, v->flags), ofl));    /* a page still shared after a fork stays read-only whatever the new rights say (docs/COW.md) */
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

/* ---- copy-on-write (docs/COW.md) ----
 *
 * A fork shares every private page instead of copying it: both entries lose PTE_W and gain PTE_COW (a software bit of the entry, Intel SDM vol. 3A, 4.5: bits 9..11 are
 * ignored by the CPU), and the page's reference count says how many address spaces map it. The first write from either side faults (CR0.WP is set, so a write by the kernel
 * through the uaccess window faults exactly like a write by the program) and uvm_cow_break gives the writer a page of its own. "cow=off" on the kernel command line keeps the
 * eager copy. */
static int g_cow = -1;

bool uvm_cow_enabled(void) {
    if (g_cow < 0) {
        char v[8];
        g_cow = UVM_COW_DEFAULT;
        if (cmdline_value("cow", v, sizeof v)) g_cow = strcmp(v, "off") == 0 ? 0 : strcmp(v, "on") == 0 ? 1 : UVM_COW_DEFAULT;
    }
    return g_cow != 0;
}

/* The flags an entry gets when its rights change or it moves: a page that is still marked copy-on-write stays marked and read-only. */
u64 uvm_keep_cow(u64 newfl, u64 oldfl) {
    return (oldfl & PTE_COW) ? ((newfl & ~PTE_W) | PTE_COW) : newfl;
}

/* Makes the page at va this address space's own if it is marked copy-on-write (nothing to do otherwise). The caller holds as->lock, or nobody else can use the address space
 * (a child that has not run yet). The only reference: the entry becomes an ordinary one in place. Shared: a new page gets the contents, the old one loses a reference. The
 * allocation may wait for the OOM killer (can_sleep), so the entry is read again after it. 0, or -ENOMEM (the entry is unchanged: still shared, still read-only). */
int uvm_cow_break(AddrSpace *as, u64 va, bool can_sleep) {
    va &= ~(u64)(PAGE_SIZE - 1);
    for (;;) {
        u64 fl = 0;
        paddr_t old = vmm_translate(as->pml4, va, &fl);
        if (!old || !(fl & PTE_COW)) return 0;
        Vma *v = vma_find(as, va);
        u64 nfl = v ? pte_flags_for(v->prot, v->flags) : (fl & ~(PTE_COW | PTE_A | PTE_D | PTE_P));
        Page *pg = pmm_page(old);
        if (pg && pg->refcount == 1) {                      /* everybody else has let go (exit, exec, munmap, or their own copy) */
            vmm_map(as->pml4, va, old, nfl);
            return 0;
        }
        paddr_t np = oom_alloc_user_page(false, can_sleep);
        if (!np) return -ENOMEM;
        u64 fl2 = 0;
        paddr_t again = vmm_translate(as->pml4, va, &fl2);
        if (again != old || !(fl2 & PTE_COW)) {             /* changed while the allocation waited: look again */
            pmm_free_page(np);
            continue;
        }
        memcpy(phys_to_virt(np), phys_to_virt(old), PAGE_SIZE);
        if (vmm_map(as->pml4, va, np, nfl)) { pmm_free_page(np); return -ENOMEM; }      /* (the tables are there: cannot fail) */
        pmm_free_page(old);                                 /* one reference less; the page itself lives on in the other address spaces */
        return 0;
    }
}

bool futex_page_has_waiters(paddr_t page);                  /* kernel/sys/futex.c */

int uvm_clone(AddrSpace *dst, const AddrSpace *src) {
    dst->brk_start = src->brk_start;
    dst->brk = src->brk;
    dst->mmap_next = src->mmap_next;
    bool can_sleep = (read_rflags() & RFLAGS_IF) != 0;
    bool cow = uvm_cow_enabled();
    for (Vma *v = src->vmas; v; v = v->next) {
        Vma *nv = vma_new(v->start, v->end, v->prot, v->flags);
        if (!nv) return -ENOMEM;
        uvm_copy_file(nv, v);
        nv->next = dst->vmas;
        dst->vmas = nv;
        for (u64 va = v->start; va < v->end; va += PAGE_SIZE) {
            u64 fl = 0;
            paddr_t sp = vmm_translate(src->pml4, va, &fl);
            if (!sp) continue;
            sp &= PTE_ADDR;
            if (v->flags & VMA_IO) {
                if (vmm_map(dst->pml4, va, sp, fl & ~(PTE_A | PTE_D))) return -ENOMEM;     /* shared device/shared-buffer pages: shared for real, never copy-on-write */
                if (pmm_page_managed(sp)) pmm_get_page(sp);
                continue;
            }
            /* Shared, read-only and marked in both address spaces. Also a page of a read-only mapping (the program text): nothing is copied for it either, and a later
             * mprotect(PROT_WRITE) finds the mark. The exception is a page some thread sleeps on in a futex: the wait list knows the word by its physical address, so
             * that page must stay the parent's own (kernel/sys/futex.c); the child gets a copy at once, as every page did before. */
            if (cow && pmm_page_managed(sp) && !futex_page_has_waiters(sp)) {
                u64 nfl = (fl & ~(PTE_W | PTE_A | PTE_D)) | PTE_COW;
                if (vmm_map(dst->pml4, va, sp, nfl)) return -ENOMEM;
                pmm_get_page(sp);
                if ((fl & PTE_W) || !(fl & PTE_COW)) vmm_map(src->pml4, va, sp, (fl & ~PTE_W) | PTE_COW);     /* the parent's entry: the TLB entry goes with it (vmm_map) */
                continue;
            }
            paddr_t np = oom_alloc_user_page(false, can_sleep);     /* the eager copy: the reserve and the OOM killer decide when that is too much */
            if (!np) return -ENOMEM;
            memcpy(phys_to_virt(np), phys_to_virt(sp), PAGE_SIZE);
            u64 cfl = (fl & PTE_COW) ? pte_flags_for(v->prot, v->flags) : (fl & ~(PTE_A | PTE_D));
            if (vmm_map(dst->pml4, va, np, cfl)) { pmm_free_page(np); return -ENOMEM; }
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
        vma_free(v);
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
    if (err & 1) {                                          /* present page: a write to a copy-on-write page, or a real protection violation */
        u64 fl = 0;
        if (!(err & 2) || (err & (8 | 16))) return false;
        if (!vmm_translate(as->pml4, addr, &fl)) return true;           /* unmapped while we waited: the retry is a not-present fault */
        if ((fl & PTE_W) && (fl & PTE_U)) return true;                  /* another thread broke the sharing while we waited for the lock */
        if (!(fl & PTE_COW) || !(fl & PTE_U) || !(v->prot & PROT_WRITE)) return false;
        if (uvm_cow_break(as, addr, (tf->rflags & RFLAGS_IF) != 0)) {   /* no page for the copy: as for a demand-zero page below */
            if ((tf->cs & 3) == 3 && oom_current_is_dying()) return true;
            klog("Yuyuko", "out of memory while copying the shared page %#lx: Yuyuko is hungry", (unsigned long)addr);
            return false;
        }
        return true;
    }
    if (vmm_translate(as->pml4, addr, NULL)) return true;   /* another thread mapped it while we waited: just retry */
    if ((err & 2) && !(v->prot & PROT_WRITE)) return false;
    if ((err & 16) && !(v->prot & PROT_EXEC)) return false;
    if (!(v->prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) return false;
    if ((v->flags & VMA_STACK) && !rlimit_stack_ok(as, addr)) return false;     /* deeper than RLIMIT_STACK: the program gets SIGSEGV */
    paddr_t p = oom_alloc_user_page(true, (tf->rflags & RFLAGS_IF) != 0);       /* the reserve and the OOM killer (kernel/mm/oom.c); a handler that interrupted code with interrupts off cannot wait */
    if (!p) {
        if ((tf->cs & 3) == 3 && oom_current_is_dying()) return true;           /* this process was the victim: the SIGKILL ends the thread on the way out of the fault, before the instruction runs again */
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
        if (uvm_cow_break(as, uaddr, (read_rflags() & RFLAGS_IF) != 0)) return -ENOMEM;     /* the direct map knows no rights: a page shared after a fork gets its copy first */
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
