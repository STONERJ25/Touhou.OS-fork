/*
 * POSIX threads through a statically linked C library (clone with CLONE_THREAD, futex, thread-local storage, tgkill): mutexes under contention, condition
 * variables with and without a timeout, semaphores, join and the value a thread returns, pthread_exit, thread ids, detached threads, many threads at once,
 * and a signal aimed at one thread. A real Linux prints the same lines (tools/linux-oracle.py); tests/case_linux.py compares.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static long counter;
static int ready;
static __thread int tl = 7;
static sem_t sem;
static volatile int detached_done, signalled, in_loop;

static void nap(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

static void *worker(void *arg) {
    long id = (long)arg;
    for (int i = 0; i < 20000; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    return (void *)(id * 10);
}

static void *tls_thread(void *arg) { (void)arg; tl += 100; return (void *)(long)tl; }

static void *waiter(void *arg) {
    (void)arg;
    pthread_mutex_lock(&lock);
    while (!ready) pthread_cond_wait(&cond, &lock);
    pthread_mutex_unlock(&lock);
    return (void *)1;
}

static void *exiter(void *arg) { (void)arg; pthread_exit((void *)42); return NULL; }

static void *tid_thread(void *arg) {
    long *out = arg;
    out[0] = syscall(SYS_gettid);
    out[1] = getpid();
    return NULL;
}

static void *poster(void *arg) { (void)arg; nap(200); sem_post(&sem); return NULL; }

static void *index_thread(void *arg) { nap(10); return arg; }

static void *detached(void *arg) { (void)arg; nap(50); detached_done = 1; return NULL; }

static void on_usr1(int sig) { (void)sig; signalled = 1; }

static void *signal_target(void *arg) {
    (void)arg;
    in_loop = 1;
    while (!signalled) nap(10);
    return (void *)7;
}

int main(void) {
    pthread_t t[16];
    void *ret;
    long sum = 0;
    for (long i = 0; i < 4; i++) pthread_create(&t[i], NULL, worker, (void *)i);
    for (int i = 0; i < 4; i++) { pthread_join(t[i], &ret); sum += (long)ret; }
    printf("4 threads, 20000 locked increments each: counter %ld, return values sum %ld\n", counter, sum);

    pthread_create(&t[0], NULL, tls_thread, NULL);
    pthread_join(t[0], &ret);
    printf("thread-local storage: the thread saw %ld, main still has %d\n", (long)ret, tl);

    pthread_create(&t[0], NULL, waiter, NULL);
    nap(100);
    pthread_mutex_lock(&lock);
    ready = 1;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&lock);
    pthread_join(t[0], &ret);
    printf("condition variable: the waiter returned %ld\n", (long)ret);

    pthread_create(&t[0], NULL, exiter, NULL);
    pthread_join(t[0], &ret);
    printf("pthread_exit value: %ld\n", (long)ret);

    long ids[2] = { 0, 0 };
    pthread_create(&t[0], NULL, tid_thread, ids);
    pthread_join(t[0], NULL);
    printf("the thread's tid differs from the process id: %d, its getpid equals ours: %d\n", ids[0] != getpid(), ids[1] == getpid());

    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 100000000;
    if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&lock);
    ready = 0;
    int r = pthread_cond_timedwait(&cond, &lock, &until);
    pthread_mutex_unlock(&lock);
    printf("pthread_cond_timedwait with nobody signalling: %d (ETIMEDOUT %d)\n", r, r == ETIMEDOUT);

    sem_init(&sem, 0, 0);
    pthread_create(&t[0], NULL, poster, NULL);
    sem_wait(&sem);
    pthread_join(t[0], NULL);
    printf("semaphore posted by a thread: waited\n");

    for (long i = 0; i < 16; i++) pthread_create(&t[i], NULL, index_thread, (void *)i);
    sum = 0;
    for (int i = 0; i < 16; i++) { pthread_join(t[i], &ret); sum += (long)ret; }
    printf("16 threads at once: sum of their indexes %ld\n", sum);

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&t[0], &at, detached, NULL);
    for (int i = 0; i < 100 && !detached_done; i++) nap(10);
    printf("detached thread finished: %d\n", detached_done);

    signal(SIGUSR1, on_usr1);
    pthread_create(&t[0], NULL, signal_target, NULL);
    while (!in_loop) nap(5);
    nap(50);
    pthread_kill(t[0], SIGUSR1);
    pthread_join(t[0], &ret);
    printf("a signal aimed at one thread: handled %d, the thread returned %ld\n", signalled, (long)ret);
    return 0;
}
