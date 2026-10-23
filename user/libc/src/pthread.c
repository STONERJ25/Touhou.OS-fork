/* POSIX threads: creation and exit, mutexes, condition variables, read-write locks, barriers, spin locks, once,
 * thread-specific data and semaphores, all on top of SYS_thread_create and the futex. See docs/THREADS.md. */
#include "pthread_impl.h"
#include "syscall_priv.h"
#include <semaphore.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/mman.h>

#define DEFAULT_STACK (256 * 1024)
#define PAGE 4096UL

/* ---------------------------------------------------------------- the main thread, futex glue */

static struct pthread g_main_thread;

/* First thing __libc_start does: the main thread gets a control block, so errno works from here on. */
void __pthread_init_main(void) {
    g_main_thread.self = &g_main_thread;
    g_main_thread.tid = (int)__sc0(SYS_gettid);
    __sc1(SYS_set_tls, (long)&g_main_thread);
}

int __futex(volatile int *addr, int op, int val, const struct timespec *rel, volatile int *addr2, int val3) {
    struct gtimespec g;
    long t = 0;
    if (rel) { g.tv_sec = rel->tv_sec; g.tv_nsec = rel->tv_nsec; t = (long)&g; }
    return (int)__sc6(SYS_futex, (long)addr, op, val, t, (long)addr2, val3);       /* raw: 0 / count or -errno */
}

static inline int cas(volatile int *p, int expect, int desired) {
    return __atomic_compare_exchange_n(p, &expect, desired, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* Time left until an absolute CLOCK_REALTIME deadline; false if it has passed. */
static int rel_from_abs(const struct timespec *abs, struct timespec *rel) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    long long ns = (long long)(abs->tv_sec - now.tv_sec) * 1000000000LL + (abs->tv_nsec - now.tv_nsec);
    if (ns <= 0) return 0;
    rel->tv_sec = (time_t)(ns / 1000000000LL);
    rel->tv_nsec = (long)(ns % 1000000000LL);
    return 1;
}

/* ---------------------------------------------------------------- internal locks */

void __lock(volatile int *l) {
    int c = 0;
    if (__atomic_compare_exchange_n(l, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    if (c != 2) c = __atomic_exchange_n(l, 2, __ATOMIC_ACQUIRE);
    while (c) {
        __futex(l, FUTEX_WAIT, 2, NULL, NULL, 0);
        c = __atomic_exchange_n(l, 2, __ATOMIC_ACQUIRE);
    }
}

void __unlock(volatile int *l) {
    if (__atomic_exchange_n(l, 0, __ATOMIC_RELEASE) == 2) __futex(l, FUTEX_WAKE, 1, NULL, NULL, 0);
}

void __rlock(RLock *l) {
    int me = __pthread_self()->tid;
    if (l->owner == me && l->count) { l->count++; return; }
    __lock(&l->lock);
    l->owner = me;
    l->count = 1;
}

void __runlock(RLock *l) {
    if (--l->count) return;
    l->owner = 0;
    __unlock(&l->lock);
}

void __rlock_reset_after_fork(RLock *l) {      /* the child's only thread is the one that held it across fork() */
    l->owner = __pthread_self()->tid;
}

/* ---------------------------------------------------------------- atfork */

static void (*g_atfork_prepare[8])(void), (*g_atfork_parent[8])(void), (*g_atfork_child[8])(void);
static int g_natfork;

int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void)) {
    if (g_natfork >= 8) return ENOMEM;
    g_atfork_prepare[g_natfork] = prepare;
    g_atfork_parent[g_natfork] = parent;
    g_atfork_child[g_natfork] = child;
    g_natfork++;
    return 0;
}

void __fork_run_prepare(void) { for (int i = g_natfork; i-- > 0;) if (g_atfork_prepare[i]) g_atfork_prepare[i](); }
void __fork_run_parent(void) { for (int i = 0; i < g_natfork; i++) if (g_atfork_parent[i]) g_atfork_parent[i](); }
void __fork_run_child(void) {
    __pthread_self()->tid = (int)__sc0(SYS_gettid);            /* the copy of the control block still carries the parent's tid */
    for (int i = 0; i < g_natfork; i++) if (g_atfork_child[i]) g_atfork_child[i]();
}

/* ---------------------------------------------------------------- thread-specific data */

static struct { void (*destructor)(void *); unsigned short seq; char used; } g_keys[PTHREAD_KEYS_MAX];
static volatile int g_key_lock;

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    __lock(&g_key_lock);
    for (unsigned i = 0; i < PTHREAD_KEYS_MAX; i++) {
        if (g_keys[i].used) continue;
        g_keys[i].used = 1;
        g_keys[i].seq++;
        g_keys[i].destructor = destructor;
        __unlock(&g_key_lock);
        *key = i;
        return 0;
    }
    __unlock(&g_key_lock);
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key) {
    if (key >= PTHREAD_KEYS_MAX || !g_keys[key].used) return EINVAL;
    __lock(&g_key_lock);
    g_keys[key].used = 0;
    g_keys[key].seq++;                          /* values threads still hold for this key become invisible */
    __unlock(&g_key_lock);
    return 0;
}

void *pthread_getspecific(pthread_key_t key) {
    struct pthread *self = __pthread_self();
    if (key >= PTHREAD_KEYS_MAX || !g_keys[key].used || self->key_seq[key] != g_keys[key].seq) return NULL;
    return self->specific[key];
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    struct pthread *self = __pthread_self();
    if (key >= PTHREAD_KEYS_MAX || !g_keys[key].used) return EINVAL;
    self->specific[key] = (void *)value;
    self->key_seq[key] = g_keys[key].seq;
    return 0;
}

static void run_destructors(struct pthread *self) {
    for (int round = 0; round < PTHREAD_DESTRUCTOR_ITERATIONS; round++) {
        int any = 0;
        for (unsigned i = 0; i < PTHREAD_KEYS_MAX; i++) {
            if (!g_keys[i].used || self->key_seq[i] != g_keys[i].seq) continue;
            void *v = self->specific[i];
            void (*d)(void *) = g_keys[i].destructor;
            if (!v || !d) continue;
            self->specific[i] = NULL;
            d(v);
            any = 1;
        }
        if (!any) break;
    }
}

/* ---------------------------------------------------------------- threads */

static struct pthread *volatile g_zombies;      /* exited detached threads whose block can be unmapped once the kernel says they are gone */

static void zombie_push(struct pthread *t) {
    struct pthread *head;
    do {
        head = g_zombies;
        t->zombie_next = head;
    } while (!__atomic_compare_exchange_n(&g_zombies, &head, t, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

static void reap_zombies(void) {
    if (!g_zombies) return;
    struct pthread *list = __atomic_exchange_n(&g_zombies, NULL, __ATOMIC_ACQ_REL);
    while (list) {
        struct pthread *next = list->zombie_next;
        if (list->tid == 0) munmap(list->map, list->maplen);
        else zombie_push(list);                 /* still on its way out of the kernel */
        list = next;
    }
}

static void thread_entry(void *arg) {
    struct pthread *self = arg;
    __sc3(SYS_sigprocmask, SIG_UNBLOCK, 1L << SIGCANCEL, 0);        /* a new thread starts with cancellation enabled, whatever its creator chose */
    pthread_exit(self->fn(self->arg));
}

/* ---------------------------------------------------------------- cancellation
 * pthread_cancel sets a flag in the target's control block and sends it SIGCANCEL (tgkill). The kernel keeps that signal
 * pending without running a handler; a thread that blocks in the kernel while it is pending (and unblocked) gets -ECANCELED,
 * which every cancellation point turns into pthread_exit(PTHREAD_CANCELED). Blocking SIGCANCEL is how a thread disables
 * cancellation. Only deferred cancellation exists: PTHREAD_CANCEL_ASYNCHRONOUS is accepted and behaves as deferred. */

extern RLock __stdio_lock;

void __cancel_exit(void) {
    struct pthread *self = __pthread_self();
    if (__stdio_lock.count && __stdio_lock.owner == self->tid) { __stdio_lock.count = 1; __runlock(&__stdio_lock); }   /* cancelled inside stdio */
    pthread_exit(PTHREAD_CANCELED);
}

int pthread_cancel(pthread_t t) {
    int tid = t->tid;
    if (!tid) return ESRCH;
    __atomic_store_n(&t->cancel_flag, 1, __ATOMIC_SEQ_CST);
    long r = __sc3(SYS_tgkill, getpid(), tid, SIGCANCEL);
    return r < 0 ? (int)-r : 0;
}

int pthread_setcancelstate(int state, int *old) {
    struct pthread *self = __pthread_self();
    if (state != PTHREAD_CANCEL_ENABLE && state != PTHREAD_CANCEL_DISABLE) return EINVAL;
    if (old) *old = self->cancel_disabled ? PTHREAD_CANCEL_DISABLE : PTHREAD_CANCEL_ENABLE;
    self->cancel_disabled = state == PTHREAD_CANCEL_DISABLE;
    __sc3(SYS_sigprocmask, state == PTHREAD_CANCEL_DISABLE ? SIG_BLOCK : SIG_UNBLOCK, 1L << SIGCANCEL, 0);
    return 0;
}

int pthread_setcanceltype(int type, int *old) {
    if (type != PTHREAD_CANCEL_DEFERRED && type != PTHREAD_CANCEL_ASYNCHRONOUS) return EINVAL;
    if (old) *old = PTHREAD_CANCEL_DEFERRED;
    return 0;
}

void pthread_testcancel(void) {
    struct pthread *self = __pthread_self();
    if (!self->cancel_disabled && __atomic_load_n(&self->cancel_flag, __ATOMIC_ACQUIRE)) __cancel_exit();
}

int pthread_create(pthread_t *out, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    reap_zombies();
    size_t stack = attr && attr->__stack ? attr->__stack : DEFAULT_STACK;
    size_t guard = attr ? attr->__guard : PAGE;
    stack = (stack + PAGE - 1) & ~(PAGE - 1);
    if (stack < PTHREAD_STACK_MIN) stack = PTHREAD_STACK_MIN;
    guard = (guard + PAGE - 1) & ~(PAGE - 1);
    size_t tcb = (sizeof(struct pthread) + 63) & ~63UL;
    size_t total = (guard + stack + tcb + PAGE - 1) & ~(PAGE - 1);

    char *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) return EAGAIN;
    if (guard && mprotect(map, guard, PROT_NONE) < 0) { munmap(map, total); return EAGAIN; }

    struct pthread *t = (struct pthread *)(map + total - tcb);      /* the block ends with the control block; the stack grows down below it */
    t->self = t;
    t->fn = fn;
    t->arg = arg;
    t->map = map;
    t->maplen = total;
    if (attr && attr->__detach) t->flags = TF_DETACHED;
    char *top = (char *)t - 8;                                       /* entry sees rsp = 8 mod 16, like after a call */
    *(void **)top = NULL;
    long r = __sc6(SYS_thread_create, 0, (long)top, (long)t, (long)&t->tid, (long)thread_entry, (long)t);
    if (r < 0) { munmap(map, total); return (int)-r; }
    *out = t;
    return 0;
}

void pthread_exit(void *ret) {
    struct pthread *self = __pthread_self();
    while (self->cleanup) {
        struct __ptcleanup *c = self->cleanup;
        self->cleanup = c->next;
        c->fn(c->arg);
    }
    self->retval = ret;
    run_destructors(self);
    int old = __atomic_fetch_or(&self->flags, TF_EXITING, __ATOMIC_ACQ_REL);
    if ((old & TF_DETACHED) && self->map) zombie_push(self);        /* nobody will join: its block is unmapped by a later create */
    __sc1(SYS_thread_exit, 0);
    for (;;) __asm__ volatile ("ud2");
}

int pthread_join(pthread_t t, void **ret) {
    if (t == __pthread_self()) return EDEADLK;
    if (t->flags & TF_DETACHED) return EINVAL;
    int tid;
    while ((tid = __atomic_load_n(&t->tid, __ATOMIC_ACQUIRE)) != 0)         /* a cancellation point */
        if (__futex(&t->tid, FUTEX_WAIT | FUTEX_CANCELLABLE, tid, NULL, NULL, 0) == -ECANCELED) __cancel_exit();
    if (ret) *ret = t->retval;
    if (t->map) munmap(t->map, t->maplen);
    return 0;
}

int pthread_detach(pthread_t t) {
    int old = __atomic_fetch_or(&t->flags, TF_DETACHED, __ATOMIC_ACQ_REL);
    if (old & TF_DETACHED) return EINVAL;
    if ((old & TF_EXITING) && t->map) zombie_push(t);               /* it already left: it did not queue itself */
    return 0;
}

pthread_t pthread_self(void) { return __pthread_self(); }
int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

int pthread_kill(pthread_t t, int sig) {
    int tid = t->tid;
    if (!tid) return ESRCH;
    long r = __sc3(SYS_tgkill, getpid(), tid, sig);
    return r < 0 ? (int)-r : 0;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *old) {
    return sigprocmask(how, set, old) < 0 ? errno : 0;
}

int sched_yield(void) { return (int)__sc0(SYS_yield); }
pid_t gettid(void) { return (pid_t)__sc0(SYS_gettid); }

void __pthread_cleanup_push(struct __ptcleanup *c, void (*fn)(void *), void *arg) {
    struct pthread *self = __pthread_self();
    c->fn = fn;
    c->arg = arg;
    c->next = self->cleanup;
    self->cleanup = c;
}

void __pthread_cleanup_pop(struct __ptcleanup *c, int execute) {
    struct pthread *self = __pthread_self();
    self->cleanup = c->next;
    if (execute) c->fn(c->arg);
}

/* ---------------------------------------------------------------- attributes */

int pthread_attr_init(pthread_attr_t *a) { a->__stack = DEFAULT_STACK; a->__guard = PAGE; a->__detach = PTHREAD_CREATE_JOINABLE; return 0; }
int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size) { if (size < PTHREAD_STACK_MIN) return EINVAL; a->__stack = size; return 0; }
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *size) { *size = a->__stack; return 0; }
int pthread_attr_setdetachstate(pthread_attr_t *a, int s) {
    if (s != PTHREAD_CREATE_JOINABLE && s != PTHREAD_CREATE_DETACHED) return EINVAL;
    a->__detach = s;
    return 0;
}
int pthread_attr_getdetachstate(const pthread_attr_t *a, int *s) { *s = a->__detach; return 0; }
int pthread_attr_setguardsize(pthread_attr_t *a, size_t size) { a->__guard = size; return 0; }
int pthread_attr_getguardsize(const pthread_attr_t *a, size_t *size) { *size = a->__guard; return 0; }

/* ---------------------------------------------------------------- mutexes */

int pthread_mutexattr_init(pthread_mutexattr_t *a) { a->__kind = PTHREAD_MUTEX_DEFAULT; return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t *a) { (void)a; return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type) {
    if (type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_ERRORCHECK) return EINVAL;
    a->__kind = type;
    return 0;
}
int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type) { *type = a->__kind; return 0; }

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    m->__lock = 0;
    m->__kind = a ? a->__kind : PTHREAD_MUTEX_DEFAULT;
    m->__owner = 0;
    m->__count = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m) { return m->__lock ? EBUSY : 0; }

int pthread_mutex_lock(pthread_mutex_t *m) {
    int me = __pthread_self()->tid;
    if (m->__kind != PTHREAD_MUTEX_NORMAL && m->__owner == me) {
        if (m->__kind == PTHREAD_MUTEX_ERRORCHECK) return EDEADLK;
        m->__count++;
        return 0;
    }
    __lock((volatile int *)&m->__lock);
    m->__owner = me;
    m->__count = 1;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
    int me = __pthread_self()->tid;
    if (m->__kind == PTHREAD_MUTEX_RECURSIVE && m->__owner == me) { m->__count++; return 0; }
    if (!cas((volatile int *)&m->__lock, 0, 1)) return EBUSY;
    m->__owner = me;
    m->__count = 1;
    return 0;
}

int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abs) {
    int me = __pthread_self()->tid;
    if (m->__kind != PTHREAD_MUTEX_NORMAL && m->__owner == me) {
        if (m->__kind == PTHREAD_MUTEX_ERRORCHECK) return EDEADLK;
        m->__count++;
        return 0;
    }
    for (;;) {
        int c = 0;
        if (__atomic_compare_exchange_n(&m->__lock, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        if (c != 2) c = __atomic_exchange_n(&m->__lock, 2, __ATOMIC_ACQUIRE);
        if (!c) break;
        struct timespec rel;
        if (!rel_from_abs(abs, &rel)) return ETIMEDOUT;
        __futex(&m->__lock, FUTEX_WAIT, 2, &rel, NULL, 0);
    }
    m->__owner = me;
    m->__count = 1;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (m->__kind != PTHREAD_MUTEX_NORMAL) {
        if (m->__owner != __pthread_self()->tid) return EPERM;
        if (m->__kind == PTHREAD_MUTEX_RECURSIVE && --m->__count > 0) return 0;
    }
    m->__owner = 0;
    __unlock((volatile int *)&m->__lock);
    return 0;
}

/* ---------------------------------------------------------------- condition variables */

int pthread_condattr_init(pthread_condattr_t *a) { a->__pad = 0; return 0; }
int pthread_condattr_destroy(pthread_condattr_t *a) { (void)a; return 0; }
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) { (void)a; c->__seq = 0; return 0; }
int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }

/* A waiter reads the sequence number before it lets go of the mutex; a signal bumps it, so a wake-up between the unlock and
 * the sleep makes the futex wait return at once instead of being lost. Spurious wake-ups are allowed by POSIX. */
static int cond_wait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abs) {
    int seq = __atomic_load_n(&c->__seq, __ATOMIC_ACQUIRE);
    int r = 0;
    struct timespec rel;
    int have_time = abs ? rel_from_abs(abs, &rel) : 1;
    int cancelled = 0;
    pthread_mutex_unlock(m);
    if (!have_time) r = ETIMEDOUT;
    else {
        long fr = __futex(&c->__seq, FUTEX_WAIT | FUTEX_CANCELLABLE, seq, abs ? &rel : NULL, NULL, 0);       /* a cancellation point */
        if (fr == -ETIMEDOUT) r = ETIMEDOUT;
        else if (fr == -ECANCELED) cancelled = 1;
    }
    pthread_mutex_lock(m);                      /* the mutex is held again before cleanup handlers run, as POSIX requires */
    if (cancelled) __cancel_exit();
    return r;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { return cond_wait(c, m, NULL); }
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abs) { return cond_wait(c, m, abs); }

int pthread_cond_signal(pthread_cond_t *c) {
    __atomic_fetch_add(&c->__seq, 1, __ATOMIC_ACQ_REL);
    __futex(&c->__seq, FUTEX_WAKE, 1, NULL, NULL, 0);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) {
    __atomic_fetch_add(&c->__seq, 1, __ATOMIC_ACQ_REL);
    __futex(&c->__seq, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
    return 0;
}

/* ---------------------------------------------------------------- read-write locks (writers wait ahead of new readers) */

int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a) {
    (void)a;
    pthread_mutex_init(&l->__m, NULL);
    pthread_cond_init(&l->__c, NULL);
    l->__readers = l->__writer = l->__wwait = 0;
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *l) { return (l->__readers || l->__writer) ? EBUSY : 0; }

int pthread_rwlock_rdlock(pthread_rwlock_t *l) {
    pthread_mutex_lock(&l->__m);
    while (l->__writer || l->__wwait) pthread_cond_wait(&l->__c, &l->__m);
    l->__readers++;
    pthread_mutex_unlock(&l->__m);
    return 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *l) {
    int r = EBUSY;
    pthread_mutex_lock(&l->__m);
    if (!l->__writer && !l->__wwait) { l->__readers++; r = 0; }
    pthread_mutex_unlock(&l->__m);
    return r;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *l) {
    pthread_mutex_lock(&l->__m);
    l->__wwait++;
    while (l->__writer || l->__readers) pthread_cond_wait(&l->__c, &l->__m);
    l->__wwait--;
    l->__writer = 1;
    pthread_mutex_unlock(&l->__m);
    return 0;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *l) {
    int r = EBUSY;
    pthread_mutex_lock(&l->__m);
    if (!l->__writer && !l->__readers) { l->__writer = 1; r = 0; }
    pthread_mutex_unlock(&l->__m);
    return r;
}

int pthread_rwlock_unlock(pthread_rwlock_t *l) {
    pthread_mutex_lock(&l->__m);
    if (l->__writer) l->__writer = 0;
    else if (l->__readers) l->__readers--;
    if (!l->__writer && !l->__readers) pthread_cond_broadcast(&l->__c);
    pthread_mutex_unlock(&l->__m);
    return 0;
}

/* ---------------------------------------------------------------- barriers, spin locks, once */

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a, unsigned count) {
    (void)a;
    if (!count) return EINVAL;
    pthread_mutex_init(&b->__m, NULL);
    pthread_cond_init(&b->__c, NULL);
    b->__count = count;
    b->__waiting = 0;
    b->__phase = 0;
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *b) { return b->__waiting ? EBUSY : 0; }

int pthread_barrier_wait(pthread_barrier_t *b) {
    pthread_mutex_lock(&b->__m);
    unsigned phase = b->__phase;
    if (++b->__waiting == b->__count) {
        b->__waiting = 0;
        b->__phase++;
        pthread_cond_broadcast(&b->__c);
        pthread_mutex_unlock(&b->__m);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    while (phase == b->__phase) pthread_cond_wait(&b->__c, &b->__m);
    pthread_mutex_unlock(&b->__m);
    return 0;
}

int pthread_spin_init(pthread_spinlock_t *l, int pshared) { (void)pshared; *l = 0; return 0; }
int pthread_spin_destroy(pthread_spinlock_t *l) { (void)l; return 0; }
int pthread_spin_trylock(pthread_spinlock_t *l) { return cas(l, 0, 1) ? 0 : EBUSY; }
int pthread_spin_lock(pthread_spinlock_t *l) {
    while (!cas(l, 0, 1)) sched_yield();        /* one CPU: the holder only makes progress if we get out of its way */
    return 0;
}
int pthread_spin_unlock(pthread_spinlock_t *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); return 0; }

int pthread_once(pthread_once_t *once, void (*fn)(void)) {
    if (__atomic_load_n(once, __ATOMIC_ACQUIRE) == 2) return 0;
    if (cas(once, 0, 1)) {
        fn();
        __atomic_store_n(once, 2, __ATOMIC_RELEASE);
        __futex(once, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
        return 0;
    }
    while (__atomic_load_n(once, __ATOMIC_ACQUIRE) != 2) __futex(once, FUTEX_WAIT, 1, NULL, NULL, 0);
    return 0;
}

/* ---------------------------------------------------------------- semaphores */

int sem_init(sem_t *s, int pshared, unsigned value) {
    (void)pshared;
    if (value > (unsigned)SEM_VALUE_MAX) { errno = EINVAL; return -1; }
    s->__value = (int)value;
    s->__waiters = 0;
    return 0;
}

int sem_destroy(sem_t *s) { (void)s; return 0; }

int sem_trywait(sem_t *s) {
    int v = __atomic_load_n(&s->__value, __ATOMIC_ACQUIRE);
    while (v > 0) if (__atomic_compare_exchange_n(&s->__value, &v, v - 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
    errno = EAGAIN;
    return -1;
}

static int sem_wait_until(sem_t *s, const struct timespec *abs) {
    for (;;) {
        int v = __atomic_load_n(&s->__value, __ATOMIC_ACQUIRE);
        if (v > 0) {
            if (__atomic_compare_exchange_n(&s->__value, &v, v - 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
            continue;
        }
        struct timespec rel;
        if (abs && !rel_from_abs(abs, &rel)) { errno = ETIMEDOUT; return -1; }
        __atomic_fetch_add(&s->__waiters, 1, __ATOMIC_ACQ_REL);
        long r = __futex(&s->__value, FUTEX_WAIT | FUTEX_CANCELLABLE, 0, abs ? &rel : NULL, NULL, 0);        /* a cancellation point */
        __atomic_fetch_sub(&s->__waiters, 1, __ATOMIC_ACQ_REL);
        if (r == -ECANCELED) __cancel_exit();
        if (r == -ETIMEDOUT) { errno = ETIMEDOUT; return -1; }
    }
}

int sem_wait(sem_t *s) { return sem_wait_until(s, NULL); }
int sem_timedwait(sem_t *s, const struct timespec *abs) { return sem_wait_until(s, abs); }

int sem_post(sem_t *s) {
    int v = __atomic_fetch_add(&s->__value, 1, __ATOMIC_ACQ_REL);
    if (v == SEM_VALUE_MAX) { __atomic_fetch_sub(&s->__value, 1, __ATOMIC_ACQ_REL); errno = EOVERFLOW; return -1; }
    if (__atomic_load_n(&s->__waiters, __ATOMIC_ACQUIRE)) __futex(&s->__value, FUTEX_WAKE, 1, NULL, NULL, 0);
    return 0;
}

int sem_getvalue(sem_t *s, int *value) { *value = __atomic_load_n(&s->__value, __ATOMIC_ACQUIRE); return 0; }
