/* Futexes: the one blocking primitive user space builds every lock on (mutexes, condition variables, join, semaphores).
 *
 * A waiter sleeps while a 32-bit user word still holds an expected value; a waker wakes the sleepers of a word. Words are
 * identified by their physical address, so a private word and a word in a shared mapping both work. The wait list is one
 * FIFO guarded by interrupt-disabling (the lock of a one-CPU kernel): checking the word and joining the list happen in the
 * same critical section, so a wake-up that follows a change of the word can never be missed. */
#include "process.h"
#include "kernel.h"
#include "cpu.h"
#include "irq.h"
#include "initcall.h"
#include "futex.h"

typedef struct FutexWaiter {
    struct FutexWaiter *prev, *next;
    u64  key;                           /* physical address of the word */
    Task *task;
    bool woken;
} FutexWaiter;

static FutexWaiter *g_head, *g_tail;    /* FIFO of everybody asleep on a futex */

static void fw_append(FutexWaiter *w) {
    w->next = NULL;
    w->prev = g_tail;
    if (g_tail) g_tail->next = w; else g_head = w;
    g_tail = w;
}

static void fw_unlink(FutexWaiter *w) {
    if (w->prev) w->prev->next = w->next; else g_head = w->next;
    if (w->next) w->next->prev = w->prev; else g_tail = w->prev;
    w->prev = w->next = NULL;
}

/* Physical address of the word at uaddr. With populate, an absent page is faulted in first (a waiter needs a real page; a
 * waker whose page is absent has nobody to wake). Returns 1 and sets *key, 0 for "not present", or -errno. */
static int futex_key(u64 uaddr, bool populate, u64 *key) {
    if (uaddr & 3) return -EINVAL;
    if (!user_range_ok((void *)uaddr, 4)) return -EFAULT;
    Process *p = current->proc;
    paddr_t pa = vmm_translate(p->as.pml4, uaddr, NULL);
    if (!pa) {
        if (!populate) return 0;
        u32 touch;
        if (copy_from_user(&touch, (void *)uaddr, sizeof touch)) return -EFAULT;
        pa = vmm_translate(p->as.pml4, uaddr, NULL);
        if (!pa) return -EFAULT;
    }
    *key = pa;
    return 1;
}

/* Wakes up to n waiters on key; with a destination key, moves up to `requeue` more of them there instead. irq off. */
static int wake_locked(u64 key, int n, u64 key2, int requeue) {
    int woken = 0;
    for (FutexWaiter *w = g_head, *next; w; w = next) {
        next = w->next;
        if (w->key != key) continue;
        if (woken < n) {
            fw_unlink(w);
            w->woken = true;
            if (w->task->state == TASK_SLEEPING) w->task->state = TASK_READY;
            woken++;
        } else if (requeue > 0) {
            w->key = key2;
            requeue--;
        } else {
            break;
        }
    }
    return woken;
}

/* The core: sleep on key while the word there still equals val. deadline is an uptime_ms() time, or ~0 for none.
 * 0 = woken, -EAGAIN = the word differed, -ETIMEDOUT, -EINTR (a signal, or the thread is wanted elsewhere). */
long futex_wait_key(u64 key, u32 val, u64 deadline) {
    FutexWaiter w = { .key = key, .task = current, .woken = false };
    long rc;
    u64 f = irq_save();
    if (*(volatile u32 *)phys_to_virt(key) != val) { irq_restore(f); return -EAGAIN; }
    fw_append(&w);
    rc = 0;
    for (;;) {
        if (w.woken) break;
        if (signal_pending_now(current->proc)) { rc = -EINTR; break; }
        if (deadline != ~0ULL && uptime_ms() >= deadline) { rc = -ETIMEDOUT; break; }
        current->intr = true;
        current->wake_ms = deadline;
        current->state = TASK_SLEEPING;
        schedule();
        current->intr = false;
    }
    if (!w.woken) fw_unlink(&w);
    irq_restore(f);
    return w.woken ? 0 : rc;
}

int futex_wake_key(u64 key, int n) {
    u64 f = irq_save();
    int woken = wake_locked(key, n < 0 ? 0x7FFFFFFF : n, 0, 0);
    irq_restore(f);
    return woken;
}

/* Wakes nwake waiters of key1 and moves up to nrequeue of the rest to key2. */
int futex_requeue_key(u64 key1, int nwake, u64 key2, int nrequeue) {
    u64 f = irq_save();
    int woken = wake_locked(key1, nwake < 0 ? 0x7FFFFFFF : nwake, key2, nrequeue);
    irq_restore(f);
    return woken;
}

int futex_waiters_on(u64 key) {
    int n = 0;
    u64 f = irq_save();
    for (FutexWaiter *w = g_head; w; w = w->next) if (w->key == key) n++;
    irq_restore(f);
    return n;
}

static long futex_wait(u64 uaddr, u32 val, u64 utimeout) {
    u64 key;
    int rc = futex_key(uaddr, true, &key);
    if (rc < 0) return rc;
    u64 deadline = ~0ULL;
    if (utimeout) {
        struct gtimespec ts;
        if (copy_from_user(&ts, (void *)utimeout, sizeof ts)) return -EFAULT;
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) return -EINVAL;
        u64 ms = (u64)ts.tv_sec * 1000 + (u64)ts.tv_nsec / 1000000 + ((ts.tv_nsec % 1000000) ? 1 : 0);
        deadline = uptime_ms() + ms;
    }
    return futex_wait_key(key, val, deadline);
}

static long futex_wake(u64 uaddr, int n) {
    u64 key;
    int rc = futex_key(uaddr, false, &key);
    if (rc <= 0) return rc;
    return futex_wake_key(key, n);
}

static long futex_cmp_requeue(u64 uaddr, int nwake, u64 nrequeue, u64 uaddr2, u32 expected) {
    u64 k1, k2;
    int rc = futex_key(uaddr, true, &k1);
    if (rc < 0) return rc;
    rc = futex_key(uaddr2, true, &k2);
    if (rc < 0) return rc;
    u64 f = irq_save();
    if (*(volatile u32 *)phys_to_virt(k1) != expected) { irq_restore(f); return -EAGAIN; }
    int woken = wake_locked(k1, nwake < 0 ? 0x7FFFFFFF : nwake, k2, (int)(nrequeue > 0x7FFFFFFF ? 0x7FFFFFFF : nrequeue));
    irq_restore(f);
    return woken;
}

/* Thread exit: every waiter on the (already zeroed) tid word wakes. */
void futex_wake_all_at(u64 uaddr) { futex_wake(uaddr, -1); }

static long sys_futex(TrapFrame *tf) {
    u64 uaddr = A0(tf);
    int op = (int)A1(tf);
    bool cancellable = (op & FUTEX_CANCELLABLE) != 0;
    op &= ~FUTEX_CANCELLABLE;
    switch (op) {
    case FUTEX_WAIT: {
        current->ignore_cancel = !cancellable;          /* a mutex wait must not spin because a cancel is pending */
        long r = futex_wait(uaddr, (u32)A2(tf), A3(tf));
        current->ignore_cancel = false;
        return r;
    }
    case FUTEX_WAKE:        return futex_wake(uaddr, (int)A2(tf));
    case FUTEX_CMP_REQUEUE: return futex_cmp_requeue(uaddr, (int)A2(tf), A3(tf), A4(tf), (u32)A5(tf));
    default:                return -ENOSYS;
    }
}

static void futex_init(void) { syscall_register(SYS_futex, sys_futex); }
INITCALL(futex_init);

/* ---- self test (called from selftest.c): the queue logic on words in kernel memory ---- */

static u32 *g_st_words;                     /* word 0 and word 1 are the two futexes the test uses */
static volatile int g_st_order[4], g_st_n;

static void st_waiter(void *arg) {
    int id = (int)(long)arg;
    if (futex_wait_key(virt_to_phys_hhdm(&g_st_words[0]), 0, ~0ULL) == 0) g_st_order[g_st_n++] = id;
}

static bool st_until(bool (*cond)(u64), u64 arg) {
    for (int i = 0; i < 3000; i++) {
        if (cond(arg)) return true;
        task_sleep_ms(1);
    }
    return false;
}

static bool st_waiting(u64 n) { return futex_waiters_on(virt_to_phys_hhdm(&g_st_words[0])) == (int)n; }
static bool st_woken(u64 n) { return g_st_n == (int)n; }

int futex_selftest(void) {
    int fails = 0;
#define FCHECK(cond) do { if (!(cond)) { fails++; printk("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
    klog("selftest", "futex: wake order, requeue, timeout, value mismatch");
    g_st_words = kzalloc(64);
    FCHECK(g_st_words);
    if (!g_st_words) return fails + 1;
    u64 k0 = virt_to_phys_hhdm(&g_st_words[0]), k1 = virt_to_phys_hhdm(&g_st_words[1]);

    FCHECK(futex_wake_key(k0, 1) == 0);                                 /* nobody is asleep there */
    FCHECK(futex_wait_key(k0, 1, ~0ULL) == -EAGAIN);                    /* the word holds 0, not 1: no sleep at all */
    u64 t0 = uptime_ms();
    FCHECK(futex_wait_key(k0, 0, t0 + 25) == -ETIMEDOUT);
    FCHECK(uptime_ms() - t0 >= 25);
    FCHECK(futex_waiters_on(k0) == 0);                                  /* a timed-out waiter left the queue */

    g_st_n = 0;
    for (long i = 0; i < 3; i++) {                                      /* three sleepers, queued in a known order */
        task_create_detached("futex-waiter", st_waiter, (void *)i);
        FCHECK(st_until(st_waiting, (u64)i + 1));
    }
    for (int i = 0; i < 3; i++) {                                       /* woken oldest first, one at a time */
        FCHECK(futex_wake_key(k0, 1) == 1);
        FCHECK(st_until(st_woken, (u64)i + 1));
        FCHECK(g_st_order[i] == i);
    }
    FCHECK(futex_waiters_on(k0) == 0);

    g_st_n = 0;
    for (long i = 0; i < 3; i++) {
        task_create_detached("futex-waiter", st_waiter, (void *)i);
        FCHECK(st_until(st_waiting, (u64)i + 1));
    }
    FCHECK(futex_requeue_key(k0, 1, k1, 2) == 1);                       /* wake one, move the other two to the second word */
    FCHECK(st_until(st_woken, 1) && g_st_order[0] == 0);
    FCHECK(futex_waiters_on(k0) == 0 && futex_waiters_on(k1) == 2);
    FCHECK(futex_wake_key(k0, 8) == 0);                                 /* the old word has nobody left */
    FCHECK(futex_wake_key(k1, 8) == 2);
    FCHECK(st_until(st_woken, 3));
    task_sleep_ms(30);                                                  /* let the detached waiters finish and be reaped */
    kfree(g_st_words);
    g_st_words = NULL;
#undef FCHECK
    return fails;
}
