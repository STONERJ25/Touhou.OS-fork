/*
 * pthread_cancel through a statically linked C library: a thread cancelled while it waits in read, nanosleep, a condition variable, a semaphore or pthread_join,
 * cleanup handlers that run in the right order, cancellation disabled until the thread enables it, asynchronous cancellation of a thread that computes, several
 * threads cancelled at once, pthread_exit. The C library cancels with its own real-time signal (32, aimed at the thread with tgkill), which only works if the
 * kernel delivers it with the siginfo of tgkill and unwinds correctly through the signal frame. A real Linux prints the same lines (tools/linux-oracle.py);
 * tests/case_linux_signals.py compares. A watchdog ends the program with a message if a join never returns.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void nap(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}

static void watchdog(int sig) {
    (void)sig;
    static const char msg[] = "the watchdog ended the program: a cancelled thread never went away\n";
    write(1, msg, sizeof msg - 1);
    _exit(3);
}

static volatile int ready, cleaned, order_n;
static char order[16];

static void cleanup_flag(void *arg) { (void)arg; cleaned = 1; }
static void cleanup_a(void *arg) { (void)arg; order[order_n++] = 'a'; }
static void cleanup_b(void *arg) { (void)arg; order[order_n++] = 'b'; }
static void cleanup_c(void *arg) { (void)arg; order[order_n++] = 'c'; }

/* joins a thread, reports what it returned and how long that took */
static void reap(const char *what, pthread_t th) {
    void *ret = NULL;
    long t0 = now_ms();
    int r = pthread_join(th, &ret);
    long took = now_ms() - t0;
    printf("%s: join %d, result is PTHREAD_CANCELED: %d, quick: %d\n", what, r, ret == PTHREAD_CANCELED, took < 3000);
}

static int fds[2];

static void *in_read(void *arg) {
    (void)arg;
    char c;
    pthread_cleanup_push(cleanup_flag, NULL);
    ready = 1;
    read(fds[0], &c, 1);                                /* a cancellation point: never returns */
    cleaned = -1;
    pthread_cleanup_pop(0);
    return NULL;
}

static void *in_nanosleep(void *arg) {
    (void)arg;
    pthread_cleanup_push(cleanup_flag, NULL);
    ready = 1;
    struct timespec ts = { 30, 0 };
    nanosleep(&ts, NULL);
    cleaned = -1;
    pthread_cleanup_pop(0);
    return NULL;
}

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

static void unlock_mutex(void *arg) { pthread_mutex_unlock(arg); cleaned = 1; }

static void *in_cond_wait(void *arg) {
    (void)arg;
    pthread_mutex_lock(&mu);
    pthread_cleanup_push(unlock_mutex, &mu);
    ready = 1;
    for (;;) pthread_cond_wait(&cv, &mu);
    pthread_cleanup_pop(1);
    return NULL;
}

static sem_t sem;

static void *in_sem_wait(void *arg) {
    (void)arg;
    pthread_cleanup_push(cleanup_flag, NULL);
    ready = 1;
    sem_wait(&sem);
    cleaned = -1;
    pthread_cleanup_pop(0);
    return NULL;
}

static void *forever(void *arg) {
    (void)arg;
    ready = 1;
    for (;;) nap(1000);
    return NULL;
}

static void *in_join(void *arg) {
    pthread_t target = *(pthread_t *)arg;
    pthread_cleanup_push(cleanup_flag, NULL);
    ready = 1;
    pthread_join(target, NULL);
    cleaned = -1;
    pthread_cleanup_pop(0);
    return NULL;
}

static void *nested_cleanup(void *arg) {
    (void)arg;
    pthread_cleanup_push(cleanup_a, NULL);
    pthread_cleanup_push(cleanup_b, NULL);
    pthread_cleanup_push(cleanup_c, NULL);
    ready = 1;
    for (;;) nap(1000);
    pthread_cleanup_pop(0);
    pthread_cleanup_pop(0);
    pthread_cleanup_pop(0);
    return NULL;
}

static volatile int disabled_steps, enabled_now;

static void *disabled_first(void *arg) {
    (void)arg;
    int old;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old);
    ready = 1;
    while (!enabled_now) { nap(5); pthread_testcancel(); disabled_steps++; }     /* testcancel does nothing while cancellation is off */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &old);
    pthread_testcancel();                               /* the request that was waiting takes effect here */
    disabled_steps = -1000;
    return NULL;
}

static volatile unsigned long spin_count;

static void *async_spinner(void *arg) {
    (void)arg;
    int old;
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &old);
    ready = 1;
    for (;;) spin_count++;                              /* no cancellation point at all: only the signal can stop it */
    return NULL;
}

static void *exiter(void *arg) {
    (void)arg;
    pthread_cleanup_push(cleanup_flag, NULL);
    pthread_exit((void *)42);
    pthread_cleanup_pop(0);
    return NULL;
}

static void *plain_return(void *arg) { return arg; }

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGALRM, watchdog);
    alarm(60);
    pthread_t th;
    void *ret;

    ready = cleaned = 0;
    pipe(fds);
    pthread_create(&th, NULL, in_read, NULL);
    while (!ready) nap(2);
    nap(50);
    printf("pthread_cancel of a thread in read: %d\n", pthread_cancel(th));
    reap("read", th);
    printf("the cleanup handler ran: %d, read did not return: %d\n", cleaned == 1, cleaned != -1);

    ready = cleaned = 0;
    pthread_create(&th, NULL, in_nanosleep, NULL);
    while (!ready) nap(2);
    nap(50);
    pthread_cancel(th);
    reap("nanosleep", th);
    printf("the cleanup handler ran: %d\n", cleaned == 1);

    ready = cleaned = 0;
    pthread_create(&th, NULL, in_cond_wait, NULL);
    while (!ready) nap(2);
    nap(50);
    pthread_cancel(th);
    reap("pthread_cond_wait", th);
    printf("the mutex was released by the cleanup handler: %d, trylock works: %d\n", cleaned == 1, pthread_mutex_trylock(&mu) == 0);
    pthread_mutex_unlock(&mu);

    ready = cleaned = 0;
    sem_init(&sem, 0, 0);
    pthread_create(&th, NULL, in_sem_wait, NULL);
    while (!ready) nap(2);
    nap(50);
    pthread_cancel(th);
    reap("sem_wait", th);
    printf("the cleanup handler ran: %d\n", cleaned == 1);

    pthread_t victim;
    ready = cleaned = 0;
    pthread_create(&victim, NULL, forever, NULL);
    while (!ready) nap(2);
    ready = 0;
    pthread_create(&th, NULL, in_join, &victim);
    while (!ready) nap(2);
    nap(50);
    pthread_cancel(th);
    reap("pthread_join", th);
    printf("the cleanup handler ran: %d\n", cleaned == 1);
    pthread_cancel(victim);
    reap("the thread that was being joined", victim);

    ready = 0;
    order_n = 0;
    memset(order, 0, sizeof order);
    pthread_create(&th, NULL, nested_cleanup, NULL);
    while (!ready) nap(2);
    nap(20);
    pthread_cancel(th);
    reap("nested cleanup handlers", th);
    printf("the handlers ran last pushed first: %s\n", order);

    ready = 0;
    disabled_steps = 0;
    enabled_now = 0;
    pthread_create(&th, NULL, disabled_first, NULL);
    while (!ready) nap(2);
    pthread_cancel(th);
    nap(150);
    printf("cancellation disabled: the thread kept running: %d\n", disabled_steps > 5);
    enabled_now = 1;
    reap("cancellation enabled again", th);
    printf("it stopped at the first cancellation point: %d\n", disabled_steps > 0);

    ready = 0;
    spin_count = 0;
    pthread_create(&th, NULL, async_spinner, NULL);
    while (!ready) nap(2);
    nap(50);
    unsigned long before = spin_count;
    printf("the thread that spins is running: %d\n", before > 0);
    pthread_cancel(th);
    reap("asynchronous cancellation of a spinning thread", th);

    pthread_t many[4];
    for (int i = 0; i < 4; i++) pthread_create(&many[i], NULL, in_read, NULL);        /* all four wait in read on the same pipe */
    nap(100);
    for (int i = 0; i < 4; i++) pthread_cancel(many[i]);
    int canceled = 0;
    for (int i = 0; i < 4; i++) {
        pthread_join(many[i], &ret);
        canceled += ret == PTHREAD_CANCELED;
    }
    printf("four threads cancelled at once: %d of 4 came back as cancelled\n", canceled);

    cleaned = 0;
    pthread_create(&th, NULL, exiter, NULL);
    pthread_join(th, &ret);
    printf("pthread_exit: the value is 42: %d, the cleanup handler ran: %d\n", ret == (void *)42, cleaned == 1);

    pthread_create(&th, NULL, plain_return, (void *)7);
    nap(50);
    printf("pthread_cancel of a thread that has finished: %d\n", pthread_cancel(th));
    pthread_join(th, &ret);
    printf("it returned its own value, not PTHREAD_CANCELED: %d\n", ret == (void *)7);

    printf("done\n");
    return 0;
}
