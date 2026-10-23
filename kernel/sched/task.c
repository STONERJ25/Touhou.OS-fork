/* Round-robin preemptive scheduler for a single CPU. All tasks live on one circular list; blocked/sleeping tasks are
 * skipped by state. Critical sections are interrupt-disabled regions (the "spinlock" of a uniprocessor kernel). */
#include "task.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "irq.h"
#include "process.h"

extern char boot_stack_top[];

Task *g_current;
bool (*g_signal_pending_hook)(void);
static Task g_boot_task;
static Task *g_idle;
static Task *g_list;
static int g_next_tid = 1;
static volatile bool g_need_resched;
static Task *g_reap;
static u8 g_fpu_default[512] ALIGNED(16);

static inline void fpu_save(u8 *area) { __asm__ volatile ("fxsave (%0)" : : "r"(area) : "memory"); }
static inline void fpu_restore(const u8 *area) { __asm__ volatile ("fxrstor (%0)" : : "r"(area) : "memory"); }

static void list_add(Task *t) {
    if (!g_list) {
        t->next = t;
        g_list = t;
    } else {
        t->next = g_list->next;
        g_list->next = t;
    }
}

static void list_remove(Task *t) {
    Task *p = g_list;
    while (p->next != t) p = p->next;
    if (p == t) {
        g_list = NULL;
    } else {
        p->next = t->next;
        if (g_list == t) g_list = p;
    }
}

static Task *task_create_common(const char *name, void (*entry)(void *), void *arg, bool detached, Process *proc);

static void idle_loop(void *arg) {
    (void)arg;
    for (;;) hlt();
}

void sched_init(void) {
    /* The context kmain is running in becomes an ordinary task; a dedicated idle thread runs only when nothing else can. */
    memset(&g_boot_task, 0, sizeof g_boot_task);
    strlcpy(g_boot_task.name, "kmain", sizeof g_boot_task.name);
    g_boot_task.tid = g_next_tid++;
    g_boot_task.state = TASK_RUNNING;
    g_boot_task.kstack_top = (u64)boot_stack_top;
    g_boot_task.pml4 = vmm_kernel_pml4();
    g_boot_task.slice = SCHED_SLICE;
    g_current = &g_boot_task;
    list_add(&g_boot_task);
    fpu_save(g_fpu_default);
    tss_set_rsp0((u64)boot_stack_top);

    g_idle = task_create_common("idle", idle_loop, NULL, false, NULL);
    if (!g_idle) panic("cannot create the idle task");
}

void task_print_current(void) {
    if (g_current) printk("task: %s (tid %d)\n", g_current->name, g_current->tid);
}

/* Runs on a new task's own stack right before its entry function. */
void task_first_run(void) {
    Task *z = g_reap;
    g_reap = NULL;
    while (z) {
        Task *n = z->wq_next;
        list_remove(z);
        kstack_free(z->kstack, KSTACK_SIZE);
        kfree(z->fpu);
        kfree(z);
        z = n;
    }
}

static void reap(void) {
    if (g_reap) task_first_run();
}

static Task *pick_next(Task *prev) {
    Task *t = prev;
    do {
        t = t->next;
        if (t != g_idle && t->state == TASK_READY) return t;
    } while (t != prev);
    if (prev != g_idle && prev->state == TASK_RUNNING) return prev;
    return g_idle;
}

void schedule(void) {
    u64 f = irq_save();
    Task *prev = g_current;
    Task *next = pick_next(prev);
    if (next != prev) {
        if (prev->state == TASK_RUNNING) prev->state = TASK_READY;
        next->state = TASK_RUNNING;
        next->slice = SCHED_SLICE;
        g_current = next;
        tss_set_rsp0(next->kstack_top);
        if (next->pml4 != prev->pml4) vmm_switch(next->pml4);
        if (prev->fpu) fpu_save(prev->fpu);
        if (next->fpu) fpu_restore(next->fpu);
        context_switch(&prev->rsp, next->rsp);
        reap();
    } else {
        prev->slice = SCHED_SLICE;
    }
    irq_restore(f);
}

void task_on_trap_return(TrapFrame *tf) {
    if (g_need_resched) {
        g_need_resched = false;
        schedule();
    }
    if ((tf->cs & 3) == 3) signal_deliver(tf);
}

static void wq_unlink(Task *t) {
    WaitQueue *wq = t->wq;
    if (!wq) return;
    Task *prev = NULL;
    for (Task *p = wq->head; p; prev = p, p = p->wq_next) {
        if (p != t) continue;
        if (prev) prev->wq_next = p->wq_next;
        else wq->head = p->wq_next;
        if (wq->tail == p) wq->tail = prev;
        break;
    }
    t->wq = NULL;
    t->wq_next = NULL;
}

void task_interrupt(Task *t) {
    u64 f = irq_save();
    if (t->intr && (t->state == TASK_BLOCKED || t->state == TASK_SLEEPING)) {
        if (t->state == TASK_BLOCKED) wq_unlink(t);
        t->state = TASK_READY;
    }
    irq_restore(f);
}

void sched_tick(void) {
    u64 now = uptime_ms();
    Task *t = g_list;
    if (t) {
        do {
            if ((t->state == TASK_SLEEPING || (t->state == TASK_BLOCKED && t->timed)) && now >= t->wake_ms) {
                if (t->state == TASK_BLOCKED) {
                    wq_unlink(t);
                    t->timed_out = true;
                }
                t->state = TASK_READY;
            }
            t = t->next;
        } while (t != g_list);
    }
    g_current->cpu_ticks++;
    if (g_current == g_idle) g_need_resched = true;
    else if (--g_current->slice <= 0) g_need_resched = true;
}

static Task *task_create_common(const char *name, void (*entry)(void *), void *arg, bool detached, Process *proc) {
    Task *t = kzalloc(sizeof(Task));
    if (!t) return NULL;
    t->kstack = kstack_alloc(KSTACK_SIZE);
    if (!t->kstack) { kfree(t); return NULL; }
    t->kstack_top = (u64)t->kstack + KSTACK_SIZE;
    strlcpy(t->name, name, sizeof t->name);
    t->entry = entry;
    t->arg = arg;
    t->detached = detached;
    t->pml4 = vmm_kernel_pml4();
    if (proc) {
        t->proc = proc;
        t->fpu = kmalloc(512);
        if (!t->fpu) { kstack_free(t->kstack, KSTACK_SIZE); kfree(t); return NULL; }
        memcpy(t->fpu, g_fpu_default, 512);
    }

    u64 *frame = (u64 *)(t->kstack_top - 72);
    frame[0] = 0;                       /* r15 */
    frame[1] = 0;                       /* r14 */
    frame[2] = (u64)arg;                /* r13 */
    frame[3] = (u64)entry;              /* r12 */
    frame[4] = 0;                       /* rbx */
    frame[5] = 0;                       /* rbp */
    frame[6] = (u64)task_entry_trampoline;
    frame[7] = 0;
    t->rsp = (u64)frame;

    u64 f = irq_save();
    t->tid = g_next_tid++;
    t->state = TASK_READY;
    list_add(t);
    irq_restore(f);
    return t;
}

Task *task_create(const char *name, void (*entry)(void *), void *arg) {
    return task_create_common(name, entry, arg, false, NULL);
}

Task *task_create_detached(const char *name, void (*entry)(void *), void *arg) {
    return task_create_common(name, entry, arg, true, NULL);
}

/* The kernel half of a user process: starts in `entry` (which execs and drops to ring 3). */
Task *task_create_user(const char *name, void (*entry)(void *), void *arg, Process *proc) {
    return task_create_common(name, entry, arg, true, proc);
}

/* A forked child: resumes from the parent's syscall frame with rax = 0 (see fork_child_start in syscall.asm). */
Task *task_create_fork(const char *name, const TrapFrame *tf, Process *proc) {
    Task *t = kzalloc(sizeof(Task));
    if (!t) return NULL;
    t->kstack = kstack_alloc(KSTACK_SIZE);
    t->fpu = kmalloc(512);
    if (!t->kstack || !t->fpu) {
        if (t->kstack) kstack_free(t->kstack, KSTACK_SIZE);
        kfree(t->fpu);
        kfree(t);
        return NULL;
    }
    t->kstack_top = (u64)t->kstack + KSTACK_SIZE;
    strlcpy(t->name, name, sizeof t->name);
    t->detached = true;
    t->proc = proc;
    t->pml4 = proc->as.pml4;
    fpu_save(t->fpu);                           /* the child inherits the parent's live FPU/SSE state */

    TrapFrame *ctf = (TrapFrame *)(t->kstack_top - sizeof(TrapFrame));
    *ctf = *tf;
    ctf->rax = 0;
    u64 *frame = (u64 *)((u64)ctf - 72);
    memset(frame, 0, 72);
    frame[3] = (u64)ctf;                        /* r12 */
    frame[6] = (u64)fork_child_start;
    t->rsp = (u64)frame;

    u64 f = irq_save();
    t->tid = g_next_tid++;
    t->state = TASK_READY;
    list_add(t);
    irq_restore(f);
    return t;
}

NORETURN void task_exit(int code) {
    cli();
    Task *t = g_current;
    t->exit_code = code;
    t->state = TASK_ZOMBIE;
    if (t->detached) {
        t->wq_next = g_reap;
        g_reap = t;
    }
    schedule();
    panic("zombie task %s was scheduled again", t->name);
}

void task_exit_current(int code) { task_exit(code); }

void task_yield(void) {
    schedule();
}

void task_sleep_ms(u64 ms) {
    if (!ms) { task_yield(); return; }
    u64 f = irq_save();
    g_current->wake_ms = uptime_ms() + ms;
    g_current->state = TASK_SLEEPING;
    schedule();
    irq_restore(f);
}

void task_for_each(void (*fn)(Task *t, void *ctx), void *ctx) {
    u64 f = irq_save();
    Task *t = g_list;
    if (t) {
        do {
            fn(t, ctx);
            t = t->next;
        } while (t != g_list);
    }
    irq_restore(f);
}

static void count_cb(Task *t, void *ctx) { (void)t; (*(int *)ctx)++; }
int task_count(void) { int n = 0; task_for_each(count_cb, &n); return n; }

Task *task_by_tid(int tid) {
    Task *found = NULL;
    u64 f = irq_save();
    Task *t = g_list;
    if (t) do { if (t->tid == tid) found = t; t = t->next; } while (t != g_list);
    irq_restore(f);
    return found;
}

/* ---- wait queues ---- */

void wq_init(WaitQueue *wq) { wq->head = wq->tail = NULL; }

static void wq_enqueue(WaitQueue *wq, Task *t) {
    t->wq = wq;
    t->wq_next = NULL;
    if (wq->tail) wq->tail->wq_next = t;
    else wq->head = t;
    wq->tail = t;
}

void wq_block(WaitQueue *wq) {
    Task *t = g_current;
    t->timed = false;
    wq_enqueue(wq, t);
    t->state = TASK_BLOCKED;
    schedule();
}

bool wq_block_timeout(WaitQueue *wq, u64 ms) {
    Task *t = g_current;
    t->timed = true;
    t->timed_out = false;
    t->wake_ms = uptime_ms() + ms;
    wq_enqueue(wq, t);
    t->state = TASK_BLOCKED;
    schedule();
    t->timed = false;
    return !t->timed_out;
}

void wq_wake_one(WaitQueue *wq) {
    u64 f = irq_save();
    Task *t = wq->head;
    if (t) {
        wq->head = t->wq_next;
        if (!wq->head) wq->tail = NULL;
        t->wq = NULL;
        t->wq_next = NULL;
        if (t->state == TASK_BLOCKED) t->state = TASK_READY;
    }
    irq_restore(f);
}

void wq_wake_all(WaitQueue *wq) {
    u64 f = irq_save();
    while (wq->head) {
        Task *t = wq->head;
        wq->head = t->wq_next;
        t->wq = NULL;
        t->wq_next = NULL;
        if (t->state == TASK_BLOCKED) t->state = TASK_READY;
    }
    wq->tail = NULL;
    irq_restore(f);
}

/* ---- mutex / semaphore ---- */

void mutex_init(Mutex *m) { m->locked = 0; wq_init(&m->wq); }

void mutex_lock(Mutex *m) {
    u64 f = irq_save();
    while (m->locked) wq_block(&m->wq);
    m->locked = 1;
    irq_restore(f);
}

bool mutex_trylock(Mutex *m) {
    u64 f = irq_save();
    bool ok = !m->locked;
    if (ok) m->locked = 1;
    irq_restore(f);
    return ok;
}

void mutex_unlock(Mutex *m) {
    u64 f = irq_save();
    m->locked = 0;
    wq_wake_one(&m->wq);
    irq_restore(f);
}

void sem_init(Semaphore *s, int count) { s->count = count; wq_init(&s->wq); }

void sem_wait(Semaphore *s) {
    u64 f = irq_save();
    while (s->count <= 0) wq_block(&s->wq);
    s->count--;
    irq_restore(f);
}

bool sem_wait_timeout(Semaphore *s, u64 ms) {
    u64 f = irq_save();
    u64 deadline = uptime_ms() + ms;
    while (s->count <= 0) {
        u64 now = uptime_ms();
        if (now >= deadline || !wq_block_timeout(&s->wq, deadline - now)) {
            if (s->count > 0) break;
            irq_restore(f);
            return false;
        }
    }
    s->count--;
    irq_restore(f);
    return true;
}

void sem_post(Semaphore *s) {
    u64 f = irq_save();
    s->count++;
    wq_wake_one(&s->wq);
    irq_restore(f);
}
