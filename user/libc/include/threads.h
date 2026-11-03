#ifndef _THREADS_H
#define _THREADS_H

#include <pthread.h>
#include <time.h>

/* C11 threads as thin wrappers over the pthread library (user/libc/src/cthreads.c). */

typedef pthread_t thrd_t;
typedef int (*thrd_start_t)(void *);
typedef pthread_mutex_t mtx_t;
typedef pthread_cond_t cnd_t;
typedef pthread_key_t tss_t;
typedef void (*tss_dtor_t)(void *);
typedef pthread_once_t once_flag;

#define thread_local _Thread_local
#define ONCE_FLAG_INIT PTHREAD_ONCE_INIT
#define TSS_DTOR_ITERATIONS PTHREAD_DESTRUCTOR_ITERATIONS

enum { thrd_success = 0, thrd_busy = 1, thrd_error = 2, thrd_nomem = 3, thrd_timedout = 4 };
enum { mtx_plain = 0, mtx_recursive = 1, mtx_timed = 2 };

int    thrd_create(thrd_t *t, thrd_start_t fn, void *arg);
int    thrd_equal(thrd_t a, thrd_t b);
thrd_t thrd_current(void);
int    thrd_sleep(const struct timespec *duration, struct timespec *remaining);
void   thrd_yield(void);
void   thrd_exit(int res) __attribute__((noreturn));
int    thrd_detach(thrd_t t);
int    thrd_join(thrd_t t, int *res);

int    mtx_init(mtx_t *m, int type);
void   mtx_destroy(mtx_t *m);
int    mtx_lock(mtx_t *m);
int    mtx_timedlock(mtx_t *m, const struct timespec *abstime);
int    mtx_trylock(mtx_t *m);
int    mtx_unlock(mtx_t *m);

int    cnd_init(cnd_t *c);
void   cnd_destroy(cnd_t *c);
int    cnd_signal(cnd_t *c);
int    cnd_broadcast(cnd_t *c);
int    cnd_wait(cnd_t *c, mtx_t *m);
int    cnd_timedwait(cnd_t *c, mtx_t *m, const struct timespec *abstime);

int    tss_create(tss_t *key, tss_dtor_t dtor);
void   tss_delete(tss_t key);
void  *tss_get(tss_t key);
int    tss_set(tss_t key, void *val);

void   call_once(once_flag *flag, void (*fn)(void));

#endif
