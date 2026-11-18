/* Resource limits: getrlimit, setrlimit, prlimit (docs/HARDENING.md, "Resource limits").
 *
 * Written from POSIX.1-2017 (getrlimit, setrlimit) and the prlimit(2) manual page. Every process has a soft and a hard value for each of the sixteen resources of
 * abi.h (the numbers and the structure are Linux's, so the Linux personality shares all of this). The soft value is what the kernel enforces; any process may set it
 * anywhere up to the hard value; any process may lower the hard value (and then cannot raise it again); only root may raise the hard value. The limits belong to the
 * Process: fork copies them, execve keeps them, so a shell's `ulimit` reaches every program it starts.
 *
 * What is enforced, and where (the rest is stored and reported, so that programs that set them are not refused):
 *   RLIMIT_NOFILE  fd_install and dup2 (kernel/sys/process.c, syscalls.c): the table has MAX_FD slots, so the hard limit cannot go above that; EMFILE / EBADF
 *   RLIMIT_NPROC   fork and the thread creation calls: the processes of one real user, root exempt; EAGAIN
 *   RLIMIT_AS      uvm_map (every mmap, brk and the other ways to get address space): the sum of the process's mappings, the stack counted at its initial size; ENOMEM
 *   RLIMIT_DATA    uvm_map: writable private mappings (the heap, anonymous maps, the program's data and bss); ENOMEM
 *   RLIMIT_STACK   the page fault handler: a stack page deeper than the limit is not mapped, so the program gets SIGSEGV as on any system
 *   RLIMIT_CORE    accepted (there are no core dumps)
 * The defaults keep every program that ran before running: nothing but NOFILE (64), NPROC (128 of the 256 slots of the process table) and the stack (8 MiB) is limited.
 */
#include "process.h"
#include "kernel.h"
#include "initcall.h"

#define NPROC_DEFAULT  (MAX_PROCS / 2)          /* one user cannot fill the process table: root and the others keep the other half */

void rlimit_defaults(Rlimits *r) {
    for (int i = 0; i < RLIM_NLIMITS; i++) r->cur[i] = r->max[i] = RLIM_INFINITY;
    r->cur[RLIMIT_STACK] = USER_STACK_MAX;
    r->cur[RLIMIT_CORE] = 0;
    r->cur[RLIMIT_NPROC] = r->max[RLIMIT_NPROC] = NPROC_DEFAULT;
    r->cur[RLIMIT_NOFILE] = r->max[RLIMIT_NOFILE] = MAX_FD;
    r->cur[RLIMIT_MEMLOCK] = r->max[RLIMIT_MEMLOCK] = 64 * 1024;
    r->cur[RLIMIT_SIGPENDING] = r->max[RLIMIT_SIGPENDING] = SIGQ_PER_PROC;
    r->cur[RLIMIT_MSGQUEUE] = r->max[RLIMIT_MSGQUEUE] = 819200;
    r->cur[RLIMIT_NICE] = r->max[RLIMIT_NICE] = 0;
    r->cur[RLIMIT_RTPRIO] = r->max[RLIMIT_RTPRIO] = 0;
}

/* ---- what the rest of the kernel asks ---- */

int rlimit_nofile(const Process *p) {
    u64 c = p->rlim.cur[RLIMIT_NOFILE];
    return c > MAX_FD ? MAX_FD : (int)c;
}

typedef struct { u32 uid; u64 n; } UserCount;

static void count_cb(Process *q, void *ctx) {
    UserCount *c = ctx;
    if (q->ruid == c->uid) c->n++;                  /* zombies count until their parent has collected them, as on every Unix */
}

bool rlimit_nproc_exceeded(const Process *p) {
    if (p->cred.uid == 0 || p->ruid == 0) return false;         /* root (and a setuid-root program run by anybody) may always fork: the system must stay in the administrator's hands */
    u64 lim = p->rlim.cur[RLIMIT_NPROC];
    if (lim == RLIM_INFINITY) return false;
    UserCount c = { p->ruid, 0 };
    process_for_each(count_cb, &c);
    return c.n >= lim;
}

/* Is `as` the address space of the running process? Only then do its limits apply: exec builds the next space of the same process before it is switched in, and its
 * loader maps whatever the program's headers ask for (a program too big for the limit is the program's problem, found when it first maps something itself). */
static const Process *owner_if_current(const AddrSpace *as) {
    const Process *p = current ? current->proc : NULL;
    return p && as == &p->as ? p : NULL;
}

int rlimit_vm_check(const AddrSpace *as, u64 len, u32 prot, u32 vflags) {
    const Process *p = owner_if_current(as);
    if (!p) return 0;
    u64 lim_as = p->rlim.cur[RLIMIT_AS], lim_data = p->rlim.cur[RLIMIT_DATA];
    if (lim_as == RLIM_INFINITY && lim_data == RLIM_INFINITY) return 0;
    u64 total = 0, data = 0;
    for (const Vma *v = as->vmas; v; v = v->next) {
        u64 n = v->end - v->start;
        if (v->flags & VMA_STACK) n = MIN(n, (u64)USER_STACK_INIT);         /* the stack is reserved at its full size but only counts as it is used: from its initial size on, RLIMIT_STACK is what limits it */
        total += n;
        if ((v->prot & PROT_WRITE) && !(v->flags & (VMA_STACK | VMA_IO))) data += n;
    }
    if (total > ~0ULL - len || (lim_as != RLIM_INFINITY && total + len > lim_as)) return -ENOMEM;
    bool is_data = (prot & PROT_WRITE) && !(vflags & (VMA_STACK | VMA_IO));
    if (is_data && lim_data != RLIM_INFINITY && (data > ~0ULL - len || data + len > lim_data)) return -ENOMEM;
    return 0;
}

bool rlimit_stack_ok(const AddrSpace *as, u64 addr) {
    const Process *p = owner_if_current(as);
    if (!p) return true;
    u64 lim = p->rlim.cur[RLIMIT_STACK];
    return lim == RLIM_INFINITY || USER_STACK_TOP - (addr & ~(u64)(PAGE_SIZE - 1)) <= lim;
}

/* ---- the calls ---- */

/* Finds the process a call is about and checks that the caller may look at it (or change it): itself, or a process of the same user, or anybody for root. */
static long target_of(const Process *caller, int pid, Process **out) {
    Process *t = pid ? process_by_pid(pid) : (Process *)caller;
    if (!t || t->state == PROC_ZOMBIE) return -ESRCH;
    if (!signal_permitted(caller, t)) return -EPERM;
    *out = t;
    return 0;
}

long rlimit_get(const Process *caller, int pid, int res, struct grlimit *out) {
    if (res < 0 || res >= RLIM_NLIMITS) return -EINVAL;
    Process *t;
    long rc = target_of(caller, pid, &t);
    if (rc) return rc;
    out->cur = t->rlim.cur[res];
    out->max = t->rlim.max[res];
    return 0;
}

long rlimit_set(Process *caller, int pid, int res, const struct grlimit *nw) {
    if (res < 0 || res >= RLIM_NLIMITS) return -EINVAL;
    if (nw->cur > nw->max) return -EINVAL;                      /* the soft limit above the hard one */
    Process *t;
    long rc = target_of(caller, pid, &t);
    if (rc) return rc;
    bool root = caller->cred.uid == 0;
    if (nw->max > t->rlim.max[res] && !root) return -EPERM;     /* only root raises a hard limit */
    if (res == RLIMIT_NOFILE && nw->max > MAX_FD) return -EPERM;        /* the descriptor table has MAX_FD slots: not even root can have more */
    t->rlim.cur[res] = nw->cur;
    t->rlim.max[res] = nw->max;
    return 0;
}

static long sys_getrlimit(TrapFrame *tf) {
    struct grlimit r;
    long rc = rlimit_get(current->proc, 0, (int)A0(tf), &r);
    return rc ? rc : (copy_to_user((void *)A1(tf), &r, sizeof r) ? -EFAULT : 0);
}

static long sys_setrlimit(TrapFrame *tf) {
    struct grlimit r;
    if (copy_from_user(&r, (const void *)A1(tf), sizeof r)) return -EFAULT;
    return rlimit_set(current->proc, 0, (int)A0(tf), &r);
}

/* prlimit(pid, resource, new, old): the old value is stored first, so a refused change still shows what the limit is. */
static long sys_prlimit(TrapFrame *tf) {
    if (A0(tf) > 0x7fffffffUL) return -ESRCH;
    int pid = (int)A0(tf), res = (int)A1(tf);
    struct grlimit nw, old;
    if (A2(tf) && copy_from_user(&nw, (const void *)A2(tf), sizeof nw)) return -EFAULT;
    long rc = rlimit_get(current->proc, pid, res, &old);
    if (rc) return rc;
    if (A3(tf) && copy_to_user((void *)A3(tf), &old, sizeof old)) return -EFAULT;
    return A2(tf) ? rlimit_set(current->proc, pid, res, &nw) : 0;
}

static void rlimit_init(void) {
    syscall_register(SYS_getrlimit, sys_getrlimit);
    syscall_register(SYS_setrlimit, sys_setrlimit);
    syscall_register(SYS_prlimit, sys_prlimit);
}
INITCALL(rlimit_init);
