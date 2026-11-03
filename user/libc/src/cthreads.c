/* C11 <threads.h> on top of the pthread library. */
#include <threads.h>
#include <stdlib.h>
#include <errno.h>

static int result(int err) {
    switch (err) {
    case 0:         return thrd_success;
    case EBUSY:     return thrd_busy;
    case ETIMEDOUT: return thrd_timedout;
    case ENOMEM:
    case EAGAIN:    return thrd_nomem;
    default:        return thrd_error;
    }
}

typedef struct { thrd_start_t fn; void *arg; } Start;

/* pthreads return void*, C11 threads return int: the result travels in the pointer. */
static void *start(void *p) {
    Start s = *(Start *)p;
    free(p);
    return (void *)(long)s.fn(s.arg);
}

int thrd_create(thrd_t *t, thrd_start_t fn, void *arg) {
    Start *s = malloc(sizeof *s);
    if (!s) return thrd_nomem;
    s->fn = fn;
    s->arg = arg;
    int err = pthread_create(t, NULL, start, s);
    if (err) free(s);
    return result(err);
}

int thrd_equal(thrd_t a, thrd_t b) { return pthread_equal(a, b); }
thrd_t thrd_current(void) { return pthread_self(); }
void thrd_yield(void) { sched_yield(); }
void thrd_exit(int res) { pthread_exit((void *)(long)res); }
int thrd_detach(thrd_t t) { return result(pthread_detach(t)); }

int thrd_join(thrd_t t, int *res) {
    void *ret = NULL;
    int err = pthread_join(t, &ret);
    if (!err && res) *res = (int)(long)ret;
    return result(err);
}

int thrd_sleep(const struct timespec *duration, struct timespec *remaining) {
    (void)remaining;
    if (nanosleep(duration, NULL) == 0) return 0;
    return errno == EINTR ? -1 : -2;
}

int mtx_init(mtx_t *m, int type) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    if (type & mtx_recursive) pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    return result(pthread_mutex_init(m, &a));
}
void mtx_destroy(mtx_t *m) { pthread_mutex_destroy(m); }
int mtx_lock(mtx_t *m) { return result(pthread_mutex_lock(m)); }
int mtx_timedlock(mtx_t *m, const struct timespec *abs) { return result(pthread_mutex_timedlock(m, abs)); }
int mtx_trylock(mtx_t *m) { return result(pthread_mutex_trylock(m)); }
int mtx_unlock(mtx_t *m) { return result(pthread_mutex_unlock(m)); }

int cnd_init(cnd_t *c) { return result(pthread_cond_init(c, NULL)); }
void cnd_destroy(cnd_t *c) { pthread_cond_destroy(c); }
int cnd_signal(cnd_t *c) { return result(pthread_cond_signal(c)); }
int cnd_broadcast(cnd_t *c) { return result(pthread_cond_broadcast(c)); }
int cnd_wait(cnd_t *c, mtx_t *m) { return result(pthread_cond_wait(c, m)); }
int cnd_timedwait(cnd_t *c, mtx_t *m, const struct timespec *abs) { return result(pthread_cond_timedwait(c, m, abs)); }

int tss_create(tss_t *key, tss_dtor_t dtor) { return result(pthread_key_create(key, dtor)); }
void tss_delete(tss_t key) { pthread_key_delete(key); }
void *tss_get(tss_t key) { return pthread_getspecific(key); }
int tss_set(tss_t key, void *val) { return result(pthread_setspecific(key, val)); }

void call_once(once_flag *flag, void (*fn)(void)) { pthread_once(flag, fn); }
