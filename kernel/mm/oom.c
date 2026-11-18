/*
 * Out-of-memory handling (docs/HARDENING.md, "The OOM killer").
 *
 * The page allocator keeps a reserve (pmm_reserve_pages: 1/32 of the RAM, 256 KiB .. 8 MiB) that only the kernel's own allocations may use. A page for a user process
 * (a page fault, mmap, fork's copy of the address space, a tmpfs write) comes from pmm_alloc_user and fails while the free pages would drop under the reserve. That is the
 * moment this file is for: instead of letting every process that wants a page run into a wall one after another (the machine would stay up, with nothing in it able to
 * start), it chooses one victim, ends it the way `kill -9` does and lets the allocation try once more when the victim's memory is back.
 *
 *   * Where the victim is chosen: in the caller's own context, in oom_make_room, which is called by the allocation that failed (oom_alloc_user_page), or by a tmpfs write
 *     before it takes the file system lock (oom_wait_for_pages). Nothing here runs from an interrupt or from the allocator itself, so the allocator's irq-off sections
 *     never wait for anything: pmm_alloc_user only says "no".
 *   * Whom: every user process except init, except one that is dying already (exiting, or SIGKILL waiting for it), by badness: the pages it has mapped (RAM only: a
 *     framebuffer mapping is not memory) plus the pages of unlinked tmpfs files it holds open (memory that dies with its last descriptor), times 5/4 for a process that
 *     runs as an ordinary user and 3/4 for one that runs as root, times 5/4 again when it started less than half a minute ago (the least work is lost). The biggest wins.
 *     Kernel threads have no process and are never candidates. Nothing is killed that has no pages at all.
 *   * How: signal_send(victim, SIGKILL), the same path as kill(2). The victim's threads die on their way back to user mode (a sleeping one is woken), the last one out
 *     frees the address space, the descriptors and everything else (process_finalize). Nothing here frees memory itself.
 *   * At which point it takes effect, and the caller who waits: a caller that can sleep (interrupts were on where it was interrupted) yields until the free pages are above the
 *     reserve again or the victim has become a zombie, at most OOM_PATIENCE_MS, and then tries once more. It never waits while holding a lock the victim could need that it
 *     could avoid: page faults hold only their own address space's lock (the victim is another process, or the caller itself), tmpfs writes wait before taking the
 *     file system lock, and the wait is bounded in any case. A caller that cannot sleep only starts the kill and fails its allocation.
 *   * When the victim is the caller (or a thread of the caller's process): the kill is sent and the caller does not wait for itself: its allocation fails, the fault
 *     handler lets a user-mode fault go (the SIGKILL ends the thread before it can run another instruction), and a copy to user space gives EFAULT on the way out.
 *   * While a victim is still alive within OOM_PATIENCE_MS of the kill, other callers wait for it instead of choosing another one: a burst of failing allocations is one
 *     event, not one kill each. After that time a victim that has not died (it is stuck in something uninterruptible) is skipped as "dying already" and the next choice is made.
 *   * A kernel allocation that fails (pmm_alloc, kmalloc: page tables, stacks, the heap) never comes here: it gives ENOMEM to its caller and nobody is killed. The reserve
 *     is what lets those succeed long after the user processes hit the wall.
 * "oom=off" on the kernel command line keeps the reserve and the failing allocations but kills nobody (a fault then ends in SIGSEGV, as before this existed).
 * /proc/oom says what the killer has done. The pages are counted in /proc/kstats (oom_kills). Written from the design above and the SDM's page table formats; no code
 * from other systems.
 */
#include "kernel.h"
#include "process.h"
#include "mm.h"
#include "cmdline.h"
#include "cpu.h"
#include "irq.h"
#include "ktime.h"
#include "initcall.h"

#define OOM_PATIENCE_MS 2000                /* how long a caller waits for a victim's memory, and how long a victim is given before another is chosen */
#define OOM_YOUNG_MS    30000

static u64 g_oom_kills;
static u64 g_attempts;                      /* calls of oom_make_room: an allocation failed at the reserve */
static int g_victim_pid;                    /* the process the latest kill was aimed at (0: none yet) */
static u64 g_victim_ms;                     /* when */
static u64 g_victim_pages;
static char g_victim_name[32];
static bool g_disabled;

u64 oom_kill_count(void) { return g_oom_kills; }

/* ---- how much memory a process is holding ---- */

/* The pages the page tables of one address space map: ordinary RAM only (a device mapping such as the framebuffer is not memory). The walk reads the user half of the
 * tables through the direct map; a process is only walked with interrupts off, so nothing changes under it on this one CPU. */
static u64 user_pages(paddr_t pml4) {
    u64 n = 0;
    const u64 *l4 = phys_to_virt(pml4);
    for (int a = 0; a < 256; a++) {
        if (!(l4[a] & PTE_P)) continue;
        const u64 *l3 = phys_to_virt(l4[a] & PTE_ADDR);
        for (int b = 0; b < 512; b++) {
            if (!(l3[b] & PTE_P) || (l3[b] & PTE_PS)) continue;
            const u64 *l2 = phys_to_virt(l3[b] & PTE_ADDR);
            for (int c = 0; c < 512; c++) {
                if (!(l2[c] & PTE_P) || (l2[c] & PTE_PS)) continue;
                const u64 *l1 = phys_to_virt(l2[c] & PTE_ADDR);
                for (int d = 0; d < 512; d++)
                    if ((l1[d] & PTE_P) && pmm_page_managed(l1[d] & PTE_ADDR)) n++;
            }
        }
    }
    return n;
}

/* Pages of tmpfs files that nobody can open again (no name) but this process still holds: they go back to the allocator when it dies. */
static u64 orphan_tmpfs_pages(const Process *p) {
    u64 n = 0;
    for (int i = 0; i < MAX_FD; i++) {
        const File *f = p->fds[i];
        if (!f || !f->vn || f->vn->nlink != 0 || !S_ISREG(f->vn->mode)) continue;
        bool seen = false;
        for (int j = 0; j < i && !seen; j++) seen = p->fds[j] && p->fds[j]->vn == f->vn;      /* a dup of a descriptor counts once */
        if (!seen) n += tmpfs_file_pages(f->vn);
    }
    return n;
}

static u64 badness(const Process *p, u64 pages, u64 now) {
    u64 s = pages * 16;
    s = s * (p->cred.uid == 0 ? 12 : 20) / 16;                  /* root's processes weigh 3/4, everybody else's 5/4 */
    if (now - p->start_ms < OOM_YOUNG_MS) s += s / 4;           /* the youngest lose the least work */
    return s;
}

typedef struct { int pid; } Cand;
static Cand g_cands[MAX_PROCS];
static int g_ncands;

static void collect_cb(Process *p, void *ctx) {
    (void)ctx;
    if (p == g_init_proc || p->state == PROC_ZOMBIE || p->exiting || (p->sig_pending & sigbit(SIGKILL))) return;   /* init never; the dying already */
    if (g_ncands < (int)ARRAY_SIZE(g_cands)) g_cands[g_ncands++].pid = p->pid;
}

/* The process to end, or NULL when there is nothing that may be ended. *pages gets what it holds. The caller has interrupts off: the whole choice is one step on this CPU
 * (nobody else can choose, kill or exit in the middle of it), which costs the timer a few milliseconds at the worst moment, on a machine with no memory left. */
static Process *pick_victim(u64 *pages) {
    g_ncands = 0;
    process_for_each(collect_cb, NULL);
    u64 now = uptime_ms(), best_score = 0;
    int best_pid = 0;
    for (int i = 0; i < g_ncands; i++) {
        Process *p = process_by_pid(g_cands[i].pid);
        if (!p || p->state == PROC_ZOMBIE || p->exiting || !p->as.pml4) continue;
        u64 n = user_pages(p->as.pml4) + orphan_tmpfs_pages(p);
        u64 s = n ? badness(p, n, now) : 0;
        if (s > best_score || (s == best_score && s && p->pid > best_pid)) { best_score = s; best_pid = p->pid; *pages = n; }
    }
    return best_pid ? process_by_pid(best_pid) : NULL;
}

/* ---- the killer ---- */

static bool victim_alive(void) {
    if (!g_victim_pid) return false;
    Process *v = process_by_pid(g_victim_pid);
    return v && v->state != PROC_ZOMBIE;
}

static bool dying(const Process *p) { return p && (p->exiting || (p->sig_pending & sigbit(SIGKILL))); }

/* Called when an allocation for a user process failed at the reserve. Makes sure a victim is on its way, and when the caller can sleep, waits for its memory.
 * True: the memory may be there now, try again once. False: nothing to wait for (no victim, the killer is off, the caller is the victim, or it cannot sleep). */
bool oom_make_room(bool can_sleep) {
    Process *me = current ? current->proc : NULL;
    g_attempts++;
    if (g_disabled) return false;
    u64 f = irq_save();                                         /* look, choose, record and signal in one step: two failing allocations never end two processes for one shortage */
    u64 now = uptime_ms();
    bool fresh = victim_alive() && now - g_victim_ms < OOM_PATIENCE_MS;       /* a victim is on its way: wait for it, kill nobody else */
    Process *v = NULL;
    u64 pages = 0;
    char vname[32] = "";
    int vpid = 0;
    if (!fresh) {
        v = pick_victim(&pages);
        if (v) {
            strlcpy(vname, v->name, sizeof vname);
            vpid = v->pid;
            strlcpy(g_victim_name, vname, sizeof g_victim_name);
            g_victim_pid = vpid;
            g_victim_ms = now;
            g_victim_pages = pages;
            g_oom_kills++;
            signal_send(v, SIGKILL);
        }
    }
    irq_restore(f);
    if (v) klog("Eiki", "out of memory: killed '%s' (pid %d), %lu pages", vname, vpid, (unsigned long)pages);
    else if (!fresh) return false;                              /* nothing that may be killed */
    Process *victim = g_victim_pid ? process_by_pid(g_victim_pid) : NULL;
    if (!victim || victim == me || dying(me)) return false;     /* the caller is on its way out: its allocation just fails */
    if (!can_sleep) return false;
    for (u64 waited = 0; waited < OOM_PATIENCE_MS; waited++) {
        if (pmm_free_pages_count() > pmm_reserve_pages()) return true;
        if (!victim_alive()) break;
        task_sleep_ms(1);
    }
    return pmm_free_pages_count() > pmm_reserve_pages();
}

/* A page for a user process (zeroed or not): the allocator's answer, and when it says no, the killer's, and one more try. 0 only when there is really none. */
paddr_t oom_alloc_user_page(bool zero, bool can_sleep) {
    paddr_t p = zero ? pmm_alloc_page_user_zeroed() : pmm_alloc_user(0);
    if (p) return p;
    if (!oom_make_room(can_sleep)) return 0;
    p = zero ? pmm_alloc_page_user_zeroed() : pmm_alloc_user(0);
    return p;
}

/* For writers that want several pages while they hold a lock the victim might need (tmpfs): called first, with nothing held. True when the room is there. */
bool oom_wait_for_pages(u64 npages) {
    if (pmm_free_pages_count() >= pmm_reserve_pages() + npages) return true;
    bool can_sleep = (read_rflags() & RFLAGS_IF) != 0;
    if (oom_make_room(can_sleep)) return pmm_free_pages_count() >= pmm_reserve_pages() + npages;
    return false;
}

bool oom_current_is_dying(void) { return current && dying(current->proc); }

/* ---- /proc/oom ---- */

static size_t gen_oom(char *b, size_t cap) {
    size_t n = (size_t)snprintf(b, cap, "killer: %s\nreserve_pages: %lu\nfree_pages: %lu\nkills: %lu\nattempts: %lu\npatience_ms: %d\n",
                                g_disabled ? "off (oom=off)" : "on", (unsigned long)pmm_reserve_pages(), (unsigned long)pmm_free_pages_count(),
                                (unsigned long)g_oom_kills, (unsigned long)g_attempts, OOM_PATIENCE_MS);
    if (g_victim_pid)
        n += (size_t)snprintf(b + n, cap - n, "last: '%s' (pid %d), %lu pages, %lu ms ago\n", g_victim_name, g_victim_pid, (unsigned long)g_victim_pages,
                              (unsigned long)(uptime_ms() - g_victim_ms));
    else
        n += (size_t)snprintf(b + n, cap - n, "last: none\n");
    return n;
}

static void oom_init(void) {
    char v[8];
    g_disabled = cmdline_value("oom", v, sizeof v) && !strcmp(v, "off");
    procfs_register("oom", gen_oom);
}
INITCALL(oom_init);
