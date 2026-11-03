#ifndef _PTHREAD_H
#define _PTHREAD_H

#include <stddef.h>
#include <time.h>
#include <signal.h>
#include <sched.h>

/* POSIX threads on top of SYS_thread_create and the futex; see docs/THREADS.md for what is and is not supported. */

struct pthread;
typedef struct pthread *pthread_t;

#define PTHREAD_KEYS_MAX            128
#define PTHREAD_DESTRUCTOR_ITERATIONS 4
#define PTHREAD_STACK_MIN           16384
#define PTHREAD_THREADS_MAX         256

typedef struct { size_t __stack; size_t __guard; int __detach; } pthread_attr_t;
#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

typedef struct { int __lock; int __kind; int __owner; int __count; } pthread_mutex_t;
typedef struct { int __kind; } pthread_mutexattr_t;
#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL
#define PTHREAD_MUTEX_INITIALIZER           { 0, PTHREAD_MUTEX_NORMAL, 0, 0 }
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER { 0, PTHREAD_MUTEX_RECURSIVE, 0, 0 }
#define PTHREAD_ERRORCHECK_MUTEX_INITIALIZER { 0, PTHREAD_MUTEX_ERRORCHECK, 0, 0 }

typedef struct { int __seq; int __pad; } pthread_cond_t;
typedef struct { int __pad; } pthread_condattr_t;
#define PTHREAD_COND_INITIALIZER { 0, 0 }

typedef struct { pthread_mutex_t __m; pthread_cond_t __c; int __readers; int __writer; int __wwait; } pthread_rwlock_t;
typedef struct { int __pad; } pthread_rwlockattr_t;
#define PTHREAD_RWLOCK_INITIALIZER { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0 }

typedef struct { pthread_mutex_t __m; pthread_cond_t __c; unsigned __count, __waiting, __phase; } pthread_barrier_t;
typedef struct { int __pad; } pthread_barrierattr_t;
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)

typedef volatile int pthread_spinlock_t;
typedef int pthread_once_t;
#define PTHREAD_ONCE_INIT 0
typedef unsigned pthread_key_t;

#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED  1
#define PTHREAD_CANCELED ((void *)-1)

/* threads */
int  pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int  pthread_join(pthread_t t, void **ret);
int  pthread_detach(pthread_t t);
void pthread_exit(void *ret) __attribute__((noreturn));
pthread_t pthread_self(void);
int  pthread_equal(pthread_t a, pthread_t b);
int  pthread_kill(pthread_t t, int sig);
int  pthread_sigmask(int how, const sigset_t *set, sigset_t *old);
int  pthread_once(pthread_once_t *once, void (*fn)(void));
int  pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void));

/* cancellation: deferred only. A cancellation point is any call that can block in the kernel (read, write, poll, nanosleep,
 * sleep, waitpid, pause, accept, ...) plus pthread_join, pthread_cond_wait/timedwait, sem_wait/timedwait and pthread_testcancel. */
#define PTHREAD_CANCEL_ENABLE       0
#define PTHREAD_CANCEL_DISABLE      1
#define PTHREAD_CANCEL_DEFERRED     0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1   /* accepted, but behaves as deferred */
int  pthread_cancel(pthread_t t);
int  pthread_setcancelstate(int state, int *old);
int  pthread_setcanceltype(int type, int *old);
void pthread_testcancel(void);

/* attributes */
int  pthread_attr_init(pthread_attr_t *a);
int  pthread_attr_destroy(pthread_attr_t *a);
int  pthread_attr_setstacksize(pthread_attr_t *a, size_t size);
int  pthread_attr_getstacksize(const pthread_attr_t *a, size_t *size);
int  pthread_attr_setdetachstate(pthread_attr_t *a, int state);
int  pthread_attr_getdetachstate(const pthread_attr_t *a, int *state);
int  pthread_attr_setguardsize(pthread_attr_t *a, size_t size);
int  pthread_attr_getguardsize(const pthread_attr_t *a, size_t *size);

/* mutexes */
int  pthread_mutexattr_init(pthread_mutexattr_t *a);
int  pthread_mutexattr_destroy(pthread_mutexattr_t *a);
int  pthread_mutexattr_settype(pthread_mutexattr_t *a, int type);
int  pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type);
int  pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int  pthread_mutex_destroy(pthread_mutex_t *m);
int  pthread_mutex_lock(pthread_mutex_t *m);
int  pthread_mutex_trylock(pthread_mutex_t *m);
int  pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abstime);
int  pthread_mutex_unlock(pthread_mutex_t *m);

/* condition variables (timeouts are absolute CLOCK_REALTIME times) */
int  pthread_condattr_init(pthread_condattr_t *a);
int  pthread_condattr_destroy(pthread_condattr_t *a);
int  pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int  pthread_cond_destroy(pthread_cond_t *c);
int  pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int  pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime);
int  pthread_cond_signal(pthread_cond_t *c);
int  pthread_cond_broadcast(pthread_cond_t *c);

/* read-write locks */
int  pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a);
int  pthread_rwlock_destroy(pthread_rwlock_t *l);
int  pthread_rwlock_rdlock(pthread_rwlock_t *l);
int  pthread_rwlock_tryrdlock(pthread_rwlock_t *l);
int  pthread_rwlock_wrlock(pthread_rwlock_t *l);
int  pthread_rwlock_trywrlock(pthread_rwlock_t *l);
int  pthread_rwlock_unlock(pthread_rwlock_t *l);

/* barriers and spin locks */
int  pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a, unsigned count);
int  pthread_barrier_destroy(pthread_barrier_t *b);
int  pthread_barrier_wait(pthread_barrier_t *b);
int  pthread_spin_init(pthread_spinlock_t *l, int pshared);
int  pthread_spin_destroy(pthread_spinlock_t *l);
int  pthread_spin_lock(pthread_spinlock_t *l);
int  pthread_spin_trylock(pthread_spinlock_t *l);
int  pthread_spin_unlock(pthread_spinlock_t *l);

/* thread-specific data */
int  pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int  pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int  pthread_setspecific(pthread_key_t key, const void *value);

/* pthread_cleanup_push/pop: handlers run (newest first) by pthread_exit, never by a plain return. */
struct __ptcleanup { struct __ptcleanup *next; void (*fn)(void *); void *arg; };
void __pthread_cleanup_push(struct __ptcleanup *c, void (*fn)(void *), void *arg);
void __pthread_cleanup_pop(struct __ptcleanup *c, int execute);
#define pthread_cleanup_push(fn, arg) { struct __ptcleanup __cl; __pthread_cleanup_push(&__cl, (fn), (arg));
#define pthread_cleanup_pop(execute)  __pthread_cleanup_pop(&__cl, (execute)); }

#endif
