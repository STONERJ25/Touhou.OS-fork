/* compiled by mcc ON the guest: pthreads, mutexes, condition variables, atomics and _Thread_local */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define THREADS 4
#define ROUNDS 2000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long shared_counter;
static long atomic_counter;
static _Thread_local int tls_value = 100;
static _Thread_local long tls_sum;

static void *worker(void *arg) {
    long id = (long)arg;
    tls_value += (int)id;
    for (int i = 0; i < ROUNDS; i++) {
        pthread_mutex_lock(&lock);
        shared_counter++;
        pthread_mutex_unlock(&lock);
        __atomic_fetch_add(&atomic_counter, 2, __ATOMIC_SEQ_CST);
        tls_sum += i;
    }
    return (void *)(tls_sum + tls_value);
}

static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t cvlock = PTHREAD_MUTEX_INITIALIZER;
static int turn;

static void *ping_pong(void *arg) {
    int me = (int)(long)arg;
    for (int i = 0; i < 5; i++) {
        pthread_mutex_lock(&cvlock);
        while (turn % 2 != me) pthread_cond_wait(&cv, &cvlock);
        turn++;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&cvlock);
    }
    return NULL;
}

static void *sleeper(void *arg) {
    usleep(20000);
    return arg;
}

int main(void) {
    pthread_t th[THREADS];
    for (long i = 0; i < THREADS; i++) pthread_create(&th[i], NULL, worker, (void *)i);
    long total = 0;
    for (int i = 0; i < THREADS; i++) {
        void *ret;
        pthread_join(th[i], &ret);
        total += (long)ret;
    }
    printf("shared=%ld atomic=%ld total=%ld\n", shared_counter, atomic_counter, total);
    printf("main thread tls_value=%d tls_sum=%ld\n", tls_value, tls_sum);

    pthread_t a, b;
    pthread_create(&a, NULL, ping_pong, (void *)0);
    pthread_create(&b, NULL, ping_pong, (void *)1);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    printf("ping-pong turns=%d\n", turn);

    pthread_t s;
    pthread_create(&s, NULL, sleeper, (void *)42);
    void *ret;
    pthread_join(s, &ret);
    printf("sleeper returned %ld\n", (long)ret);

    int expected = 0;
    int desired = 7;
    int cell = 0;
    int ok = __atomic_compare_exchange_n(&cell, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    int again = __atomic_compare_exchange_n(&cell, &expected, 9, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    printf("cas: %d %d cell=%d expected=%d\n", ok, again, cell, expected);
    printf("xchg old=%d, fetch_or old=%d now=%d\n", __atomic_exchange_n(&cell, 12, __ATOMIC_SEQ_CST), __atomic_fetch_or(&cell, 3, __ATOMIC_SEQ_CST),
           __atomic_load_n(&cell, __ATOMIC_SEQ_CST));
    return 0;
}
