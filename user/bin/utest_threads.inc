/* Thread tests for utest (included by utest.c). Everything waits with generous timeouts: the guest clock can run several
 * times slower than the host, so no test sleeps a fixed time and then expects an effect. */
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>

#define TH_SECTION(name) printf("utest: threads: %s\n", name)

/* The raw futex system call, so the tests can check its own contract (-ETIMEDOUT, -EAGAIN, wake counts). */
static long th_futex(volatile int *addr, int op, int val, const struct timespec *rel) {
    struct gtimespec g;
    long t = 0, r;
    if (rel) { g.tv_sec = rel->tv_sec; g.tv_nsec = rel->tv_nsec; t = (long)&g; }
    register long r10 __asm__("r10") = t;
    __asm__ volatile ("syscall" : "=a"(r) : "a"((long)SYS_futex), "D"((long)addr), "S"((long)op), "d"((long)val), "r"(r10)
                      : "rcx", "r11", "memory");
    return r;
}

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

/* _Thread_local: every thread has its own copy, initialised from the .tdata template and zero for .tbss */
static _Thread_local int tl_int = 7;
static _Thread_local char tl_zero[100];
static _Thread_local struct { int a; long b; } tl_struct = { 3, 4 };
static _Alignas(64) _Thread_local char tl_aligned[8];

static void *th_tls_check(void *arg) {
    long id = (long)arg;
    int ok = tl_int == 7 && tl_struct.a == 3 && tl_struct.b == 4 && !tl_zero[0] && !tl_zero[99] && ((unsigned long)tl_aligned % 64) == 0;
    tl_int = (int)id;
    tl_zero[50] = (char)id;
    tl_struct.b = id;
    for (int i = 0; i < 100; i++) {
        sched_yield();
        if (tl_int != (int)id || tl_zero[50] != (char)id || tl_struct.b != id) ok = 0;
    }
    return ok ? (void *)&tl_int : NULL;
}

static void test_threads_tls(void) {
    TH_SECTION("_Thread_local: initialised and zeroed copies per thread");
    CHECK(tl_int == 7 && tl_struct.a == 3 && tl_struct.b == 4 && !tl_zero[3] && ((unsigned long)tl_aligned % 64) == 0);
    pthread_t t[4];
    void *addr[4];
    for (long i = 0; i < 4; i++) pthread_create(&t[i], NULL, th_tls_check, (void *)(i + 10));
    for (int i = 0; i < 4; i++) { addr[i] = NULL; pthread_join(t[i], &addr[i]); }
    for (int i = 0; i < 4; i++) {
        CHECK(addr[i] != NULL && addr[i] != (void *)&tl_int);
        for (int j = i + 1; j < 4; j++) CHECK(addr[i] != addr[j]);
    }
    CHECK(tl_int == 7 && tl_struct.b == 4 && !tl_zero[50]);         /* the main thread's copy never changed */
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

/* ---- signals and threads ---- */

static volatile int g_th_sig_tid, g_th_sig_hits;
static sem_t g_th_ready;

static void th_on_sig(int sig) {
    (void)sig;
    g_th_sig_tid = gettid();
    __atomic_fetch_add(&g_th_sig_hits, 1, __ATOMIC_SEQ_CST);
}

static void *th_wait_for_signal(void *arg) {
    int unblock = (int)(long)arg;
    if (unblock) {
        sigset_t s;
        sigemptyset(&s);
        sigaddset(&s, SIGUSR2);
        pthread_sigmask(SIG_UNBLOCK, &s, NULL);
    }
    sem_post(&g_th_ready);
    for (int i = 0; i < 4000 && !g_th_sig_hits; i++) usleep(2000);     /* the signal interrupts the sleep */
    return (void *)(long)gettid();
}

static void test_threads_signals(void) {
    TH_SECTION("signals: pthread_kill, per-thread masks, process-directed delivery");
    struct sigaction sa = { th_on_sig, 0, 0 };
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    CHECK(sigaction(SIGUSR2, &sa, NULL) == 0);
    sem_init(&g_th_ready, 0, 0);

    /* thread-directed: the handler must run in the thread that was aimed at, not in main */
    g_th_sig_hits = 0;
    g_th_sig_tid = 0;
    pthread_t t;
    void *ret = NULL;
    CHECK(pthread_create(&t, NULL, th_wait_for_signal, (void *)0) == 0);
    sem_wait(&g_th_ready);
    CHECK(pthread_kill(t, SIGUSR1) == 0);
    CHECK(pthread_join(t, &ret) == 0);
    CHECK(g_th_sig_hits == 1 && g_th_sig_tid != 0 && g_th_sig_tid == (int)(long)ret && g_th_sig_tid != gettid());

    /* process-directed: main blocks SIGUSR2, so the thread that does not block it must take it */
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR2);
    CHECK(pthread_sigmask(SIG_BLOCK, &s, NULL) == 0);
    g_th_sig_hits = 0;
    g_th_sig_tid = 0;
    CHECK(pthread_create(&t, NULL, th_wait_for_signal, (void *)1) == 0);
    sem_wait(&g_th_ready);
    CHECK(kill(getpid(), SIGUSR2) == 0);
    CHECK(pthread_join(t, &ret) == 0);
    CHECK(g_th_sig_hits == 1 && g_th_sig_tid == (int)(long)ret && g_th_sig_tid != gettid());
    pthread_sigmask(SIG_UNBLOCK, &s, NULL);

    /* raise() is aimed at the calling thread */
    g_th_sig_hits = 0;
    raise(SIGUSR1);
    CHECK(g_th_sig_hits == 1 && g_th_sig_tid == gettid());
    signal(SIGUSR1, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);
}

/* ---- processes and threads ---- */

static void *th_segv(void *arg) { (void)arg; *(volatile int *)0 = 1; return NULL; }

static void *th_forker(void *arg) {
    (void)arg;
    pid_t pid = fork();
    if (pid == 0) {                                                 /* only the calling thread exists here */
        char *p = malloc(100);                                      /* the allocator lock must not be left held by another thread */
        char buf[32];
        snprintf(buf, sizeof buf, "%d", (int)getpid());
        _exit(p && gettid() == getpid() && getppid() > 0 && buf[0] ? 7 : 8);
    }
    int status = 0;
    pid_t w = waitpid(pid, &status, 0);
    return (void *)(long)(w == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

static void *th_execer(void *arg) {
    (void)arg;
    execl("/bin/hello", "hello", "from", "thread", (char *)NULL);
    _exit(111);
    return NULL;
}

static int g_th_pipe[2];
static pthread_mutex_t g_th_block_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_th_futex_word;

static void *th_block_read(void *arg) { char c; (void)arg; read(g_th_pipe[0], &c, 1); return NULL; }
static void *th_block_poll(void *arg) { struct pollfd p = { g_th_pipe[0], POLLIN, 0 }; (void)arg; poll(&p, 1, -1); return NULL; }
static void *th_block_futex(void *arg) { (void)arg; th_futex(&g_th_futex_word, FUTEX_WAIT, 0, NULL); return NULL; }
static void *th_block_sleep(void *arg) { (void)arg; sleep(600); return NULL; }
static void *th_block_cond(void *arg) {
    static pthread_cond_t c = PTHREAD_COND_INITIALIZER;
    (void)arg;
    pthread_mutex_lock(&g_th_block_mutex);
    pthread_cond_wait(&c, &g_th_block_mutex);
    return NULL;
}
static void *th_exit_other(void *arg) { (void)arg; usleep(20000); exit(6); return NULL; }

/* Waits for a child with a generous bound; kills it and reports a failure instead of hanging the suite. */
static int th_wait_child(pid_t pid, int *status) {
    for (int i = 0; i < 3000; i++) {
        pid_t w = waitpid(pid, status, WNOHANG);
        if (w == pid) return 1;
        if (w < 0) return 0;
        usleep(10000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, status, 0);
    return 0;
}

static void test_threads_processes(void) {
    TH_SECTION("SIGSEGV in a thread ends the whole process");
    int status = 0;
    pid_t pid = fork();
    if (pid == 0) {
        pthread_t t;
        pthread_create(&t, NULL, th_segv, NULL);
        pthread_join(t, NULL);                                      /* never returns: the process is torn down around us */
        _exit(0);
    }
    CHECK(th_wait_child(pid, &status) && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);

    TH_SECTION("fork from a thread");
    pthread_t t;
    void *ret = NULL;
    CHECK(pthread_create(&t, NULL, th_forker, NULL) == 0 && pthread_join(t, &ret) == 0);
    CHECK((long)ret == 7);

    TH_SECTION("exec from a thread (the other threads are killed)");
    int fds[2];
    CHECK(pipe(fds) == 0);
    pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        pthread_t sleeper, e;
        pthread_create(&sleeper, NULL, th_block_sleep, NULL);       /* asleep in the kernel while execve runs */
        pthread_create(&e, NULL, th_execer, NULL);
        pthread_join(e, NULL);
        _exit(112);
    }
    close(fds[1]);
    char out[256];
    ssize_t n = 0, total = 0;
    while ((n = read(fds[0], out + total, sizeof out - 1 - (size_t)total)) > 0) total += n;
    out[total] = 0;
    close(fds[0]);
    CHECK(th_wait_child(pid, &status) && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(strstr(out, "Hello from Gensokyo") && strstr(out, "argv[2] = thread"));

    TH_SECTION("exit while other threads block in read, poll, futex, sleep and a condition variable");
    pid = fork();
    if (pid == 0) {
        pipe(g_th_pipe);
        pthread_t b[5];
        pthread_create(&b[0], NULL, th_block_read, NULL);
        pthread_create(&b[1], NULL, th_block_poll, NULL);
        pthread_create(&b[2], NULL, th_block_futex, NULL);
        pthread_create(&b[3], NULL, th_block_sleep, NULL);
        pthread_create(&b[4], NULL, th_block_cond, NULL);
        for (int i = 0; i < 50; i++) sched_yield();
        usleep(30000);
        exit(5);
    }
    CHECK(th_wait_child(pid, &status) && WIFEXITED(status) && WEXITSTATUS(status) == 5);

    TH_SECTION("exit_group from a thread other than the main one");
    pid = fork();
    if (pid == 0) {
        pthread_t b;
        pthread_create(&b, NULL, th_exit_other, NULL);
        pthread_join(b, NULL);                                      /* main is blocked in the join when the other thread exits the process */
        _exit(9);
    }
    CHECK(th_wait_child(pid, &status) && WIFEXITED(status) && WEXITSTATUS(status) == 6);

    TH_SECTION("the last thread to leave ends the process with status 0");
    pid = fork();
    if (pid == 0) {
        pthread_t b;
        pthread_create(&b, NULL, th_nothing, NULL);
        pthread_join(b, NULL);
        pthread_exit(NULL);                                         /* main thread leaves; nobody else is left */
    }
    CHECK(th_wait_child(pid, &status) && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* ---- timeouts and limits ---- */

static sem_t g_th_hold;

static void *th_hold(void *arg) { (void)arg; sem_wait(&g_th_hold); return NULL; }

static long th_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void th_abs_after(struct timespec *ts, long ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_nsec += (ms % 1000) * 1000000L;
    ts->tv_sec += ms / 1000 + ts->tv_nsec / 1000000000L;
    ts->tv_nsec %= 1000000000L;
}

static void test_threads_timeouts(void) {
    TH_SECTION("futex, condition variable, semaphore and mutex timeouts");
    volatile int word = 5;
    struct timespec rel = { 0, 30 * 1000000L };
    long t0 = th_now_ms();
    CHECK(th_futex(&word, FUTEX_WAIT, 5, &rel) == -ETIMEDOUT);
    CHECK(th_now_ms() - t0 >= 25);                                  /* guest time: it did wait */
    CHECK(th_futex(&word, FUTEX_WAIT, 6, &rel) == -EAGAIN); /* the word no longer holds the expected value */
    CHECK(th_futex(&word, FUTEX_WAKE, 1, NULL) == 0);       /* nobody to wake */

    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t c = PTHREAD_COND_INITIALIZER;
    struct timespec abs;
    pthread_mutex_lock(&m);
    th_abs_after(&abs, 30);
    CHECK(pthread_cond_timedwait(&c, &m, &abs) == ETIMEDOUT);
    pthread_mutex_unlock(&m);

    sem_t s;
    sem_init(&s, 0, 0);
    th_abs_after(&abs, 30);
    CHECK(sem_timedwait(&s, &abs) < 0 && errno == ETIMEDOUT);
    CHECK(sem_trywait(&s) < 0 && errno == EAGAIN);
    sem_post(&s);
    CHECK(sem_trywait(&s) == 0);

    pthread_mutex_t held = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&held);
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_t rec;
    pthread_mutex_init(&rec, &ma);
    CHECK(pthread_mutex_lock(&rec) == 0 && pthread_mutex_lock(&rec) == 0 && pthread_mutex_unlock(&rec) == 0 && pthread_mutex_unlock(&rec) == 0);
    CHECK(pthread_mutex_unlock(&rec) == EPERM);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&rec, &ma);
    CHECK(pthread_mutex_lock(&rec) == 0 && pthread_mutex_lock(&rec) == EDEADLK && pthread_mutex_trylock(&rec) == EBUSY);
    pthread_mutex_unlock(&rec);
    pthread_mutex_unlock(&held);

    TH_SECTION("thread limit: creation fails cleanly with EAGAIN");
    sem_init(&g_th_hold, 0, 0);
    pthread_attr_t small;
    pthread_attr_init(&small);
    pthread_attr_setstacksize(&small, PTHREAD_STACK_MIN);
    static pthread_t held_threads[PTHREAD_THREADS_MAX + 8];
    int made = 0, err = 0;
    while (made < PTHREAD_THREADS_MAX + 8) {
        err = pthread_create(&held_threads[made], &small, th_hold, NULL);
        if (err) break;
        made++;
    }
    CHECK(err == EAGAIN && made >= 200 && made < PTHREAD_THREADS_MAX + 8);
    for (int i = 0; i < made; i++) sem_post(&g_th_hold);
    int joined = 0;
    for (int i = 0; i < made; i++) if (pthread_join(held_threads[i], NULL) == 0) joined++;
    CHECK(joined == made);
    pthread_t again;
    CHECK(pthread_create(&again, NULL, th_nothing, NULL) == 0 && pthread_join(again, NULL) == 0);     /* the slots came back */
}

/* ---- stress: threads, pipes, mmap/munmap and fork together, then check for leaked pages ---- */

static int g_th_stress_pipe[2];

static void *th_stress(void *arg) {
    long id = (long)arg;
    char c = (char)('a' + id);
    char *m = mmap(NULL, 64 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m != MAP_FAILED) { m[0] = c; m[64 * 1024 - 1] = c; munmap(m, 64 * 1024); }
    void *p = malloc(1000 + (size_t)id * 100);
    free(p);
    write(g_th_stress_pipe[1], &c, 1);
    char got;
    read(g_th_stress_pipe[0], &got, 1);
    return NULL;
}

static unsigned long th_free_kib(void) {
    struct gsysinfo si;
    gap_sysinfo(&si);
    return (unsigned long)(si.free_ram >> 10);
}

static void test_threads_stress(void) {
    TH_SECTION("stress: threads + pipes + mmap/munmap + fork, no pages leaked");
    pipe(g_th_stress_pipe);
    unsigned long before = 0;
    for (int iter = 0; iter < 25; iter++) {
        if (iter == 2) before = th_free_kib();                      /* warm up first: page tables and slabs settle */
        pthread_t ts[6];
        for (long i = 0; i < 6; i++) pthread_create(&ts[i], NULL, th_stress, (void *)i);
        pid_t pid = fork();
        if (pid == 0) _exit(0);
        int status = 0;
        CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
        for (int i = 0; i < 6; i++) pthread_join(ts[i], NULL);
    }
    unsigned long after = th_free_kib();
    printf("  stress: free memory %lu KiB before, %lu KiB after\n", before, after);
    CHECK(after + 1024 >= before);                                  /* within 1 MiB: page-table pages for fresh address ranges stay until exit */
    close(g_th_stress_pipe[0]);
    close(g_th_stress_pipe[1]);
}

/* ---- sigsuspend, sigwait, sigtimedwait, sigpending ---- */

static void *th_kill_main(void *arg) {
    usleep(5000);
    pthread_kill((pthread_t)arg, SIGUSR1);
    return NULL;
}

static void *th_sigwaiter(void *arg) {
    sigset_t s;
    int sig = 0;
    (void)arg;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR2);
    sem_post(&g_th_ready);
    int r = sigwait(&s, &sig);
    return (void *)(long)(r == 0 ? sig : -1);
}

static void test_threads_sigwait(void) {
    TH_SECTION("sigsuspend, sigwait, sigtimedwait and sigpending");
    struct sigaction sa = { th_on_sig, 0, 0 };
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0 && sigaction(SIGUSR2, &sa, NULL) == 0);
    sem_init(&g_th_ready, 0, 0);

    sigset_t blk, empty, cur;
    sigemptyset(&blk);
    sigaddset(&blk, SIGUSR1);
    sigemptyset(&empty);
    pthread_sigmask(SIG_BLOCK, &blk, NULL);
    g_th_sig_hits = 0;
    g_th_sig_tid = 0;
    pthread_t t;
    CHECK(pthread_create(&t, NULL, th_kill_main, (void *)pthread_self()) == 0);
    CHECK(sigsuspend(&empty) == -1 && errno == EINTR);              /* the handler ran inside sigsuspend, under the empty mask */
    CHECK(g_th_sig_hits == 1 && g_th_sig_tid == gettid());
    pthread_sigmask(SIG_BLOCK, NULL, &cur);
    CHECK(cur & (1U << SIGUSR1));                                   /* ... and the old mask came back afterwards */
    pthread_join(t, NULL);
    pthread_sigmask(SIG_UNBLOCK, &blk, NULL);

    /* a dedicated thread accepts SIGUSR2 with sigwait while every thread blocks it: no handler runs */
    sigset_t b2;
    sigemptyset(&b2);
    sigaddset(&b2, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &b2, NULL);
    g_th_sig_hits = 0;
    void *ret = NULL;
    CHECK(pthread_create(&t, NULL, th_sigwaiter, NULL) == 0);
    sem_wait(&g_th_ready);
    CHECK(kill(getpid(), SIGUSR2) == 0);
    CHECK(pthread_join(t, &ret) == 0 && (long)ret == SIGUSR2 && g_th_sig_hits == 0);

    /* sigpending sees a blocked signal, sigtimedwait takes it, and times out when there is none */
    raise(SIGUSR2);
    sigset_t pend;
    sigemptyset(&pend);
    CHECK(sigpending(&pend) == 0 && (pend & (1U << SIGUSR2)));
    struct timespec ts = { 0, 20 * 1000000L };
    CHECK(sigtimedwait(&b2, NULL, &ts) == SIGUSR2);
    CHECK(sigtimedwait(&b2, NULL, &ts) == -1 && errno == EAGAIN);
    sigemptyset(&pend);
    CHECK(sigpending(&pend) == 0 && !(pend & (1U << SIGUSR2)));
    pthread_sigmask(SIG_UNBLOCK, &b2, NULL);
    signal(SIGUSR1, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);
}

/* ---- cancellation ---- */

static int g_cancel_cleanup, g_cancel_progress, g_cancel_go;
static pthread_cond_t g_cancel_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_cancel_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_cancel_pipe[2];

static void th_cleanup_count(void *arg) { (void)arg; __atomic_fetch_add(&g_cancel_cleanup, 1, __ATOMIC_SEQ_CST); }
static void th_cleanup_unlock(void *m) { pthread_mutex_unlock(m); __atomic_fetch_add(&g_cancel_cleanup, 1, __ATOMIC_SEQ_CST); }

static void *th_cancel_read(void *arg) {
    char c;
    (void)arg;
    pthread_cleanup_push(th_cleanup_count, NULL)
    sem_post(&g_th_ready);
    read(g_cancel_pipe[0], &c, 1);                                  /* blocks: a cancellation point */
    g_cancel_progress = -1;                                         /* never reached */
    pthread_cleanup_pop(0)
    return NULL;
}

static void *th_cancel_cond(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_cancel_mutex);
    pthread_cleanup_push(th_cleanup_unlock, &g_cancel_mutex)
    sem_post(&g_th_ready);
    for (;;) pthread_cond_wait(&g_cancel_cond, &g_cancel_mutex);
    pthread_cleanup_pop(1)
    return NULL;
}

static void *th_cancel_sleep(void *arg) { (void)arg; sem_post(&g_th_ready); sleep(600); g_cancel_progress = -1; return NULL; }

static void *th_cancel_disabled(void *arg) {
    int old = -1;
    (void)arg;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old);
    CHECK(old == PTHREAD_CANCEL_ENABLE);
    sem_post(&g_th_ready);
    for (int i = 0; i < 3000 && !g_cancel_go; i++) usleep(2000);    /* cancellation is pending the whole time but must not act */
    g_cancel_progress = 1;
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_testcancel();                                           /* acts now */
    g_cancel_progress = -1;
    return NULL;
}

static void test_threads_cancel(void) {
    TH_SECTION("pthread_cancel: blocked read, condition wait, sleep, disabled state");
    sem_init(&g_th_ready, 0, 0);
    pipe(g_cancel_pipe);
    pthread_t t;
    void *ret = NULL;
    g_cancel_cleanup = g_cancel_progress = g_cancel_go = 0;

    CHECK(pthread_create(&t, NULL, th_cancel_read, NULL) == 0);
    sem_wait(&g_th_ready);
    CHECK(pthread_cancel(t) == 0);
    CHECK(pthread_join(t, &ret) == 0 && ret == PTHREAD_CANCELED);
    CHECK(g_cancel_cleanup == 1 && g_cancel_progress == 0);

    CHECK(pthread_create(&t, NULL, th_cancel_cond, NULL) == 0);
    sem_wait(&g_th_ready);
    CHECK(pthread_cancel(t) == 0);
    CHECK(pthread_join(t, &ret) == 0 && ret == PTHREAD_CANCELED);
    CHECK(g_cancel_cleanup == 2);
    CHECK(pthread_mutex_trylock(&g_cancel_mutex) == 0);             /* the condition wait gave the mutex back before the handler ran */
    pthread_mutex_unlock(&g_cancel_mutex);

    CHECK(pthread_create(&t, NULL, th_cancel_sleep, NULL) == 0);
    sem_wait(&g_th_ready);
    CHECK(pthread_cancel(t) == 0);
    CHECK(pthread_join(t, &ret) == 0 && ret == PTHREAD_CANCELED && g_cancel_progress == 0);

    CHECK(pthread_create(&t, NULL, th_cancel_disabled, NULL) == 0);
    sem_wait(&g_th_ready);
    CHECK(pthread_cancel(t) == 0);
    for (int i = 0; i < 20; i++) usleep(2000);                      /* it must still be running, not cancelled */
    CHECK(g_cancel_progress == 0);
    g_cancel_go = 1;
    CHECK(pthread_join(t, &ret) == 0 && ret == PTHREAD_CANCELED && g_cancel_progress == 1);
    close(g_cancel_pipe[0]);
    close(g_cancel_pipe[1]);
}

static void test_threads_proc(void) {
    TH_SECTION("/proc shows the threads");
    sem_init(&g_th_hold, 0, 0);
    pthread_t t[3];
    for (int i = 0; i < 3; i++) pthread_create(&t[i], NULL, th_hold, NULL);
    char buf[2048];
    int fd = open("/proc/self/threads", O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0) close(fd);
    CHECK(n > 0);
    int lines = 0, has_main = 0;
    if (n > 0) {
        buf[n] = 0;
        for (char *p = buf; *p; p++) if (*p == '\n') lines++;
        char first[16];
        snprintf(first, sizeof first, "%d ", (int)getpid());
        has_main = strstr(buf, first) != NULL;
    }
    CHECK(lines == 4 && has_main);
    fd = open("/proc/self/status", O_RDONLY);
    n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0) close(fd);
    CHECK(n > 0 && (buf[n] = 0, strstr(buf, "Threads:\t4") != NULL));
    fd = open("/proc/tasks", O_RDONLY);
    char big[4096];
    n = fd >= 0 ? read(fd, big, sizeof big - 1) : -1;
    if (fd >= 0) close(fd);
    CHECK(n > 0 && (big[n] = 0, strstr(big, " tid=") != NULL));
    for (int i = 0; i < 3; i++) sem_post(&g_th_hold);
    for (int i = 0; i < 3; i++) pthread_join(t[i], NULL);
}

/* ---- C11 <threads.h> ---- */

#include <threads.h>

static mtx_t c11_m;
static cnd_t c11_c;
static int c11_flag, c11_once_runs;
static once_flag c11_once = ONCE_FLAG_INIT;
static thread_local int c11_tl = 5;

static int c11_worker(void *arg) { return (int)(long)arg * 3; }
static void c11_once_fn(void) { c11_once_runs++; }
static int c11_signaller(void *arg) {
    (void)arg;
    mtx_lock(&c11_m);
    c11_flag = 1;
    cnd_signal(&c11_c);
    mtx_unlock(&c11_m);
    c11_tl = 9;
    return 0;
}

static void test_threads_c11(void) {
    TH_SECTION("C11 threads.h: thrd, mtx, cnd, tss, call_once, thread_local");
    thrd_t t;
    int res = 0;
    CHECK(thrd_create(&t, c11_worker, (void *)14) == thrd_success && thrd_join(t, &res) == thrd_success && res == 42);
    CHECK(mtx_init(&c11_m, mtx_plain) == thrd_success && cnd_init(&c11_c) == thrd_success);
    CHECK(thrd_create(&t, c11_signaller, NULL) == thrd_success);
    mtx_lock(&c11_m);
    while (!c11_flag) cnd_wait(&c11_c, &c11_m);
    mtx_unlock(&c11_m);
    CHECK(thrd_join(t, NULL) == thrd_success && c11_tl == 5);       /* the thread's thread_local write was its own */
    call_once(&c11_once, c11_once_fn);
    call_once(&c11_once, c11_once_fn);
    CHECK(c11_once_runs == 1);
    mtx_t rm;
    CHECK(mtx_init(&rm, mtx_recursive | mtx_timed) == thrd_success);
    CHECK(mtx_lock(&rm) == thrd_success && mtx_lock(&rm) == thrd_success && mtx_unlock(&rm) == thrd_success && mtx_unlock(&rm) == thrd_success);
    struct timespec ts;
    th_abs_after(&ts, 20);
    mtx_lock(&c11_m);
    CHECK(cnd_timedwait(&c11_c, &c11_m, &ts) == thrd_timedout);
    mtx_unlock(&c11_m);
    tss_t k;
    CHECK(tss_create(&k, NULL) == thrd_success && tss_set(k, (void *)5) == thrd_success && tss_get(k) == (void *)5);
    tss_delete(k);
    CHECK(thrd_equal(thrd_current(), thrd_current()));
}

static void test_threads(void) {
    test_threads_c11();
    test_threads_basic();
    test_threads_proc();
    test_threads_sync();
    test_threads_local();
    test_threads_tls();
    test_threads_many();
    test_threads_signals();
    test_threads_sigwait();
    test_threads_cancel();
    test_threads_timeouts();
    test_threads_processes();
    test_threads_stress();
}
