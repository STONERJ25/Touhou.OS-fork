/* Thread control block and internal helpers shared by pthread.c, stdio.c and stdlib.c (not installed). */
#ifndef LIBGAP_PTHREAD_IMPL_H
#define LIBGAP_PTHREAD_IMPL_H

#include <pthread.h>

/* The FS base of every thread points at its struct pthread; offsets 0 and 8 are fixed (errno.h reads them). */
struct pthread {
    struct pthread *self;           /* 0: FS:0 holds the thread pointer itself */
    int errno_val;                  /* 8: this thread's errno */
    int pad;
    volatile int tid;               /* kernel thread id; the kernel zeroes it and wakes it when the thread exits (join waits here) */
    volatile int flags;             /* TF_* */
    void *(*fn)(void *);
    void *arg;
    void *retval;
    void *map;                      /* the mmap'd block holding guard page, stack and this struct (NULL for the main thread) */
    size_t maplen;
    struct pthread *zombie_next;    /* list of exited detached threads whose block can be unmapped */
    unsigned long long specific_set;
    void *specific[PTHREAD_KEYS_MAX];
    unsigned short key_seq[PTHREAD_KEYS_MAX];
    struct __ptcleanup *cleanup;
};

#define TF_EXITING  1
#define TF_DETACHED 2

static inline struct pthread *__pthread_self(void) {
    struct pthread *t;
    __asm__ ("movq %%fs:0, %0" : "=r"(t));
    return t;
}

void __pthread_init_main(void);
int  __futex(volatile int *addr, int op, int val, const struct timespec *rel, volatile int *addr2, int val3);

/* Small non-recursive lock for libc internals; works before any thread exists (no system call unless contended). */
void __lock(volatile int *l);
void __unlock(volatile int *l);

/* Recursive lock for stdio. */
typedef struct { volatile int lock; volatile int owner; int count; } RLock;
void __rlock(RLock *l);
void __runlock(RLock *l);
void __rlock_reset_after_fork(RLock *l);

void __fork_run_prepare(void);
void __fork_run_parent(void);
void __fork_run_child(void);

#endif
