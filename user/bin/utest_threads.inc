/* Thread tests for utest (included by utest.c). Everything waits with generous timeouts: the guest clock can run several
 * times slower than the host, so no test sleeps a fixed time and then expects an effect. */
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>

#define TH_SECTION(name) printf("utest: threads: %s\n", name)

static void *th_return_arg(void *arg) { return (void *)((long)arg * 2); }

static pthread_mutex_t g_th_mutex = PTHREAD_MUTEX_INITIALIZER;
static long g_th_counter;
static long g_th_atomic;

static void *th_count(void *arg) {
    (void)arg;
    for (int i = 0; i < 2000; i++) {
        pthread_mutex_lock(&g_th_mutex);
        g_th_counter++;
        pthread_mutex_unlock(&g_th_mutex);
        __atomic_fetch_add(&g_th_atomic, 1, __ATOMIC_SEQ_CST);
    }
    return NULL;
}

static void test_threads_basic(void) {
    TH_SECTION("create, join, return values");
    pthread_t t;
    void *ret = NULL;
    CHECK(pthread_create(&t, NULL, th_return_arg, (void *)21) == 0);
    CHECK(pthread_join(t, &ret) == 0 && (long)ret == 42);
    CHECK(pthread_equal(pthread_self(), pthread_self()));
    CHECK(gettid() == getpid());                                    /* the first thread's id is the pid */

    TH_SECTION("many threads, one mutex, one atomic");
    pthread_t ts[8];
    for (int i = 0; i < 8; i++) CHECK(pthread_create(&ts[i], NULL, th_count, NULL) == 0);
    for (int i = 0; i < 8; i++) CHECK(pthread_join(ts[i], NULL) == 0);
    CHECK(g_th_counter == 8 * 2000);
    CHECK(g_th_atomic == 8 * 2000);
}

/* producer / consumer over a bounded queue with two condition variables */
static pthread_mutex_t g_q_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_q_notempty = PTHREAD_COND_INITIALIZER, g_q_notfull = PTHREAD_COND_INITIALIZER;
static int g_q[4], g_q_head, g_q_tail, g_q_count;
static long g_q_sum;

static void *th_producer(void *arg) {
    (void)arg;
    for (int i = 1; i <= 200; i++) {
        pthread_mutex_lock(&g_q_mutex);
        while (g_q_count == 4) pthread_cond_wait(&g_q_notfull, &g_q_mutex);
        g_q[g_q_tail] = i;
        g_q_tail = (g_q_tail + 1) % 4;
        g_q_count++;
        pthread_cond_signal(&g_q_notempty);
        pthread_mutex_unlock(&g_q_mutex);
    }
    return NULL;
}

static void *th_consumer(void *arg) {
    (void)arg;
    for (int i = 0; i < 200; i++) {
        pthread_mutex_lock(&g_q_mutex);
        while (g_q_count == 0) pthread_cond_wait(&g_q_notempty, &g_q_mutex);
        g_q_sum += g_q[g_q_head];
        g_q_head = (g_q_head + 1) % 4;
        g_q_count--;
        pthread_cond_signal(&g_q_notfull);
        pthread_mutex_unlock(&g_q_mutex);
    }
    return NULL;
}

static pthread_rwlock_t g_rw = PTHREAD_RWLOCK_INITIALIZER;
static int g_rw_value, g_rw_bad;

static void *th_reader(void *arg) {
    (void)arg;
    for (int i = 0; i < 300; i++) {
        pthread_rwlock_rdlock(&g_rw);
        int a = g_rw_value;
        sched_yield();
        if (a != g_rw_value || (a & 1)) g_rw_bad++;             /* a writer never leaves an odd value behind a read lock */
        pthread_rwlock_unlock(&g_rw);
    }
    return NULL;
}

static void *th_writer(void *arg) {
    (void)arg;
    for (int i = 0; i < 100; i++) {
        pthread_rwlock_wrlock(&g_rw);
        g_rw_value++;
        sched_yield();
        g_rw_value++;
        pthread_rwlock_unlock(&g_rw);
    }
    return NULL;
}

static pthread_barrier_t g_barrier;
static int g_barrier_phase[4], g_barrier_serial;

static void *th_barrier(void *arg) {
    long id = (long)arg;
    for (int round = 0; round < 5; round++) {
        g_barrier_phase[id] = round;
        int r = pthread_barrier_wait(&g_barrier);
        if (r == PTHREAD_BARRIER_SERIAL_THREAD) __atomic_fetch_add(&g_barrier_serial, 1, __ATOMIC_SEQ_CST);
        for (int k = 0; k < 4; k++) if (g_barrier_phase[k] < round) g_th_counter = -1;      /* nobody may lag behind the barrier */
        pthread_barrier_wait(&g_barrier);
    }
    return NULL;
}

static void test_threads_sync(void) {
    TH_SECTION("producer/consumer with condition variables");
    pthread_t p, c;
    CHECK(pthread_create(&p, NULL, th_producer, NULL) == 0);
    CHECK(pthread_create(&c, NULL, th_consumer, NULL) == 0);
    pthread_join(p, NULL);
    pthread_join(c, NULL);
    CHECK(g_q_sum == 200 * 201 / 2);

    TH_SECTION("rwlock");
    pthread_t rw[6];
    for (int i = 0; i < 4; i++) pthread_create(&rw[i], NULL, th_reader, NULL);
    for (int i = 4; i < 6; i++) pthread_create(&rw[i], NULL, th_writer, NULL);
    for (int i = 0; i < 6; i++) pthread_join(rw[i], NULL);
    CHECK(g_rw_bad == 0 && g_rw_value == 2 * 2 * 100);

    TH_SECTION("barrier");
    g_th_counter = 0;
    CHECK(pthread_barrier_init(&g_barrier, NULL, 4) == 0);
    pthread_t bt[4];
    for (long i = 0; i < 4; i++) pthread_create(&bt[i], NULL, th_barrier, (void *)i);
    for (int i = 0; i < 4; i++) pthread_join(bt[i], NULL);
    CHECK(g_th_counter == 0);
    CHECK(g_barrier_serial == 5);                                   /* exactly one thread per round is told it was last */
}

/* thread-local state: errno, thread-specific data with destructors, once */
static pthread_key_t g_key;
static int g_destroyed;
static int g_errno_bad;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_once_runs;

static void th_destructor(void *p) { if (p == (void *)0x5eed) __atomic_fetch_add(&g_destroyed, 1, __ATOMIC_SEQ_CST); }
static void th_once_fn(void) { __atomic_fetch_add(&g_once_runs, 1, __ATOMIC_SEQ_CST); }

static void *th_errno(void *arg) {
    long id = (long)arg;
    pthread_once(&g_once, th_once_fn);
    pthread_setspecific(g_key, (void *)0x5eed);
    for (int i = 0; i < 200; i++) {
        errno = (int)(id * 1000 + i);
        sched_yield();
        if (errno != (int)(id * 1000 + i)) g_errno_bad++;           /* another thread's errno must never show through */
        if (pthread_getspecific(g_key) != (void *)0x5eed) g_errno_bad++;
    }
    return NULL;
}

static void test_threads_local(void) {
    TH_SECTION("errno, thread-specific data, once");
    CHECK(pthread_key_create(&g_key, th_destructor) == 0);
    errno = 4242;
    pthread_t ts[4];
    for (long i = 0; i < 4; i++) pthread_create(&ts[i], NULL, th_errno, (void *)(i + 1));
    for (int i = 0; i < 4; i++) pthread_join(ts[i], NULL);
    CHECK(g_errno_bad == 0);
    CHECK(errno == 4242);                                           /* our own errno survived the other threads */
    CHECK(g_destroyed == 4);
    CHECK(g_once_runs == 1);
    CHECK(pthread_getspecific(g_key) == NULL);
    pthread_key_delete(g_key);
}

static volatile int g_detached_done;

static void *th_detached(void *arg) { (void)arg; __atomic_fetch_add(&g_detached_done, 1, __ATOMIC_SEQ_CST); return NULL; }

static void *th_nothing(void *arg) { return arg; }

static void test_threads_many(void) {
    TH_SECTION("detached threads, 200 created and joined");
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    for (int i = 0; i < 20; i++) CHECK(pthread_create(&t, &attr, th_detached, NULL) == 0);
    for (int i = 0; i < 2000 && g_detached_done < 20; i++) usleep(5000);
    CHECK(g_detached_done == 20);
    int made = 0, joined = 0;
    for (long i = 0; i < 200; i++) {
        if (pthread_create(&t, NULL, th_nothing, (void *)i)) break;
        made++;
        void *ret = NULL;
        if (pthread_join(t, &ret) == 0 && (long)ret == i) joined++;
    }
    CHECK(made == 200 && joined == 200);

    pthread_t all[100];
    int ok = 0;
    for (long i = 0; i < 100; i++) if (pthread_create(&all[i], NULL, th_nothing, (void *)i) == 0) ok++;
    CHECK(ok == 100);
    int sum_ok = 0;
    for (long i = 0; i < ok; i++) { void *ret = NULL; if (pthread_join(all[i], &ret) == 0 && (long)ret == i) sum_ok++; }
    CHECK(sum_ok == 100);
}

static void test_threads(void) {
    test_threads_basic();
    test_threads_sync();
    test_threads_local();
    test_threads_many();
}
