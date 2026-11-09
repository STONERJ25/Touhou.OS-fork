/*
 * Interval timers, POSIX timers and clocks through a statically linked C library: setitimer and getitimer (real, virtual and profiling time), alarm, timer_create
 * with a signal, with a signal for one thread and with a thread notification (the C library's helper thread), expiry counts and overruns, absolute times,
 * timer_gettime/timer_settime/timer_delete and their errors, the clocks (clock_gettime, clock_getres, the CPU-time clocks of the process and of a thread),
 * nanosleep and clock_nanosleep. A real Linux prints the same lines (tools/linux-oracle.py); tests/case_linux_signals.py compares. Nothing here depends on exact
 * times: counts are checked against generous windows and printed as yes or no.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* sleeps for the whole time, whatever signals arrive meanwhile (a timer's signal would end a plain nanosleep early) */
static void nap(long ms) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_nsec += ms * 1000000L;
    t.tv_sec += t.tv_nsec / 1000000000L;
    t.tv_nsec %= 1000000000L;
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) {}
}

static volatile sig_atomic_t hits;
static volatile int last_code, last_signo, last_overrun, first_overrun = -1, last_value, last_tid_ok;
static int g_timerid;

static void handler(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    last_signo = sig;
    last_code = si->si_code;
    last_overrun = si->si_overrun;
    if (first_overrun < 0) first_overrun = si->si_overrun;
    last_value = si->si_value.sival_int;
    last_tid_ok = si->si_timerid == g_timerid;
    hits++;
}

static void install(int sig) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

static void block(int sig, int how) {
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, sig);
    sigprocmask(how, &s, NULL);
}

static void settimer(int which, long first_us, long interval_us) {
    struct itimerval iv;
    iv.it_value.tv_sec = first_us / 1000000;
    iv.it_value.tv_usec = first_us % 1000000;
    iv.it_interval.tv_sec = interval_us / 1000000;
    iv.it_interval.tv_usec = interval_us % 1000000;
    setitimer(which, &iv, NULL);
}

/* burns CPU time in user mode until the counter reaches n or a deadline passes */
static void spin_until(int n, long limit_ms) {
    long t0 = now_ms();
    volatile unsigned long x = 0;
    while (hits < n && now_ms() - t0 < limit_ms)
        for (int i = 0; i < 20000; i++) x += (unsigned long)i;
}

static long cpu_ns(clockid_t c) {
    struct timespec ts;
    clock_gettime(c, &ts);
    return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

/* ------------------------------------------------------------ thread helpers */

static volatile int thread_ticks, thread_on_other;
static pthread_t main_thread;

static void notify_function(union sigval v) {
    (void)v;
    thread_ticks++;
    if (!pthread_equal(pthread_self(), main_thread)) thread_on_other = 1;
}

static volatile pid_t worker_tid;
static volatile int worker_hits, worker_hits_elsewhere;

static void worker_handler(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si; (void)uc;
    if ((pid_t)syscall(SYS_gettid) == worker_tid) worker_hits++; else worker_hits_elsewhere++;
}

static volatile int worker_stop;

static void *worker(void *arg) {
    (void)arg;
    worker_tid = (pid_t)syscall(SYS_gettid);
    while (!worker_stop) nap(5);
    return NULL;
}

static volatile long spun_ns;

static void *spinner(void *arg) {
    (void)arg;
    long t0 = cpu_ns(CLOCK_THREAD_CPUTIME_ID);
    volatile unsigned long x = 0;
    long w0 = now_ms();
    while (now_ms() - w0 < 200)
        for (int i = 0; i < 20000; i++) x += (unsigned long)i;
    spun_ns = cpu_ns(CLOCK_THREAD_CPUTIME_ID) - t0;
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    main_thread = pthread_self();
    install(SIGALRM);
    install(SIGVTALRM);
    install(SIGPROF);

    /* 1. ITIMER_REAL with an interval: about twenty SIGALRM in a second, and the setting reads back */
    struct itimerval cur;
    hits = 0;
    settimer(ITIMER_REAL, 50000, 50000);
    getitimer(ITIMER_REAL, &cur);
    printf("getitimer after setitimer(50 ms, 50 ms): interval %ld us, value in (0, 50 ms]: %d\n", (long)cur.it_interval.tv_usec,
           (cur.it_value.tv_sec == 0 && cur.it_value.tv_usec > 0 && cur.it_value.tv_usec <= 50000));
    long t0 = now_ms();
    while (now_ms() - t0 < 1000) nap(10);
    settimer(ITIMER_REAL, 0, 0);
    int n = hits;
    printf("SIGALRM in one second at 50 ms: between 12 and 22: %d, code SI_KERNEL (%d): %d\n", n >= 12 && n <= 22, SI_KERNEL, last_code == SI_KERNEL);
    getitimer(ITIMER_REAL, &cur);
    printf("after stopping it: value %ld, interval %ld\n", (long)cur.it_value.tv_usec + (long)cur.it_value.tv_sec, (long)cur.it_interval.tv_usec);
    hits = 0;
    nap(150);
    printf("no more SIGALRM after it was stopped: %d\n", hits == 0);

    /* a one-shot timer, and the previous setting returned by setitimer */
    hits = 0;
    settimer(ITIMER_REAL, 100000, 0);
    nap(400);
    printf("one-shot 100 ms: exactly one SIGALRM: %d\n", hits == 1);
    struct itimerval old;
    settimer(ITIMER_REAL, 2000000, 0);
    struct itimerval two = { { 0, 0 }, { 0, 0 } };
    setitimer(ITIMER_REAL, &two, &old);
    printf("the old value comes back: between 1.5 and 2 s: %d, no interval: %d\n", old.it_value.tv_sec == 1 || (old.it_value.tv_sec == 2 && old.it_value.tv_usec == 0),
           old.it_interval.tv_sec == 0 && old.it_interval.tv_usec == 0);

    /* alarm and the real timer are the same timer */
    alarm(5);
    getitimer(ITIMER_REAL, &cur);
    printf("alarm(5) is visible in getitimer: %d\n", cur.it_value.tv_sec == 4 || cur.it_value.tv_sec == 5);
    printf("alarm(0) returns the seconds left: %u\n", alarm(0));

    /* 2. ITIMER_VIRTUAL and ITIMER_PROF count CPU time: they tick while the program computes and stay silent while it sleeps */
    hits = 0;
    settimer(ITIMER_VIRTUAL, 20000, 20000);
    nap(300);
    printf("SIGVTALRM while sleeping: %d\n", (int)hits);
    spin_until(5, 8000);
    settimer(ITIMER_VIRTUAL, 0, 0);
    printf("SIGVTALRM while computing: at least five: %d, signo %d\n", hits >= 5, last_signo);
    hits = 0;
    settimer(ITIMER_PROF, 20000, 20000);
    nap(300);
    printf("SIGPROF while sleeping: %d\n", (int)hits);
    spin_until(5, 8000);
    settimer(ITIMER_PROF, 0, 0);
    printf("SIGPROF while computing: at least five: %d, signo %d\n", hits >= 5, last_signo);

    /* 3. POSIX timers with a signal. The raw system calls tell the kernel's timer id, which a handler sees in si_timerid. */
    install(SIGRTMIN);
    struct sigevent sev;
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGRTMIN;
    sev.sigev_value.sival_int = 7;
    int id = -1;
    long r = syscall(SYS_timer_create, CLOCK_MONOTONIC, &sev, &id);
    g_timerid = id;
    printf("timer_create: %ld\n", r);
    struct itimerspec its, got;
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = 30000000;
    its.it_interval.tv_nsec = 30000000;
    hits = 0;
    syscall(SYS_timer_settime, id, 0, &its, NULL);
    syscall(SYS_timer_gettime, id, &got);
    printf("timer_gettime: interval %ld ms, value in (0, 30 ms]: %d\n", got.it_interval.tv_nsec / 1000000,
           got.it_value.tv_sec == 0 && got.it_value.tv_nsec > 0 && got.it_value.tv_nsec <= 30000000);
    nap(500);
    n = hits;
    memset(&its, 0, sizeof its);
    syscall(SYS_timer_settime, id, 0, &its, &got);
    printf("30 ms timer for half a second: between 10 and 18 signals: %d, SI_TIMER (%d): %d, value 7: %d, si_timerid is the kernel's id: %d, signo SIGRTMIN: %d\n",
           n >= 10 && n <= 18, SI_TIMER, last_code == SI_TIMER, last_value == 7, last_tid_ok, last_signo == SIGRTMIN);
    printf("the old setting came back from timer_settime: interval %ld ms\n", got.it_interval.tv_nsec / 1000000);

    /* overruns: the signal is blocked while the timer runs, so the first signal stands for all the expirations that followed it. The timer is stopped only after
     * the signal was taken: what happens to a signal that is still waiting when its timer is stopped differs between kernel versions, so the test stays away from it. */
    block(SIGRTMIN, SIG_BLOCK);
    hits = 0;
    first_overrun = -1;
    its.it_value.tv_nsec = 20000000;
    its.it_interval.tv_nsec = 20000000;
    syscall(SYS_timer_settime, id, 0, &its, NULL);
    nap(300);
    sigset_t waiting;
    sigpending(&waiting);
    block(SIGRTMIN, SIG_UNBLOCK);
    long ov = syscall(SYS_timer_getoverrun, id);
    memset(&its, 0, sizeof its);
    syscall(SYS_timer_settime, id, 0, &its, NULL);
    printf("blocked for 300 ms at 20 ms: the signal was pending: %d, delivered at unblocking: %d, its overrun at least 8: %d, timer_getoverrun agrees: %d\n",
           sigismember(&waiting, SIGRTMIN), hits >= 1, first_overrun >= 8, ov == first_overrun);

    /* a one-shot timer, and an absolute time on the monotonic clock */
    hits = 0;
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = 50000000;
    its.it_interval.tv_nsec = 0;
    syscall(SYS_timer_settime, id, 0, &its, NULL);
    nap(300);
    syscall(SYS_timer_gettime, id, &got);
    printf("one-shot 50 ms: exactly one signal: %d, expired: value %ld interval %ld\n", hits == 1, (long)got.it_value.tv_sec + got.it_value.tv_nsec, (long)got.it_interval.tv_nsec);
    struct timespec abs;
    clock_gettime(CLOCK_MONOTONIC, &abs);
    abs.tv_nsec += 100000000;
    if (abs.tv_nsec >= 1000000000) { abs.tv_sec++; abs.tv_nsec -= 1000000000; }
    memset(&its, 0, sizeof its);
    its.it_value = abs;
    hits = 0;
    t0 = now_ms();
    syscall(SYS_timer_settime, id, TIMER_ABSTIME, &its, NULL);
    while (!hits && now_ms() - t0 < 2000) nap(5);
    long took = now_ms() - t0;
    printf("absolute time 100 ms ahead: fired: %d, after 80 to 400 ms: %d\n", (int)hits, took >= 80 && took <= 400);
    printf("an absolute time in the past fires at once: ");
    abs.tv_sec -= 1;
    its.it_value = abs;
    hits = 0;
    syscall(SYS_timer_settime, id, TIMER_ABSTIME, &its, NULL);
    t0 = now_ms();
    while (!hits && now_ms() - t0 < 2000) nap(5);
    printf("%d\n", (int)hits);

    /* delete: nothing arrives afterwards, the id is gone */
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = 30000000;
    its.it_interval.tv_nsec = 30000000;
    syscall(SYS_timer_settime, id, 0, &its, NULL);
    r = syscall(SYS_timer_delete, id);
    hits = 0;
    nap(150);
    printf("timer_delete: %ld, no signal afterwards: %d\n", r, hits == 0);
    errno = 0;
    r = syscall(SYS_timer_delete, id);
    printf("timer_delete again: %ld, errno %d (EINVAL %d)\n", r, errno, errno == EINVAL);
    errno = 0;
    r = syscall(SYS_timer_gettime, id, &got);
    printf("timer_gettime of it: %ld, errno %d (EINVAL %d)\n", r, errno, errno == EINVAL);

    /* SIGEV_NONE: the timer runs and expires without a signal */
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_NONE;
    syscall(SYS_timer_create, CLOCK_REALTIME, &sev, &id);
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = 30000000;
    hits = 0;
    syscall(SYS_timer_settime, id, 0, &its, NULL);
    nap(150);
    syscall(SYS_timer_gettime, id, &got);
    printf("SIGEV_NONE: no signal: %d, expired: %d\n", hits == 0, got.it_value.tv_sec == 0 && got.it_value.tv_nsec == 0);
    syscall(SYS_timer_delete, id);

    /* 4. SIGEV_THREAD_ID: the signal goes to one thread of the process */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = worker_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGRTMIN + 1, &sa, NULL);
    pthread_t th;
    pthread_create(&th, NULL, worker, NULL);
    while (!worker_tid) nap(5);
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGRTMIN + 1;
    sev._sigev_un._tid = worker_tid;
    r = syscall(SYS_timer_create, CLOCK_MONOTONIC, &sev, &id);
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = 30000000;
    its.it_interval.tv_nsec = 30000000;
    syscall(SYS_timer_settime, id, 0, &its, NULL);
    nap(300);
    memset(&its, 0, sizeof its);
    syscall(SYS_timer_settime, id, 0, &its, NULL);
    printf("SIGEV_THREAD_ID: created %ld, signals in the worker: at least 5: %d, in other threads: %d\n", r, worker_hits >= 5, worker_hits_elsewhere);
    syscall(SYS_timer_delete, id);
    worker_stop = 1;
    pthread_join(th, NULL);

    /* the C library's own thread notification: a helper thread starts notify_function in a thread of its own at every expiry */
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = notify_function;
    timer_t tt;
    r = timer_create(CLOCK_MONOTONIC, &sev, &tt);
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = 30000000;
    its.it_interval.tv_nsec = 30000000;
    timer_settime(tt, 0, &its, NULL);
    nap(500);
    memset(&its, 0, sizeof its);
    timer_settime(tt, 0, &its, NULL);
    n = thread_ticks;
    nap(100);
    printf("SIGEV_THREAD (glibc): created %ld, between 8 and 17 calls: %d, in a thread of its own: %d, none after it was stopped: %d\n", r, n >= 8 && n <= 17,
           thread_on_other, thread_ticks == n);
    timer_delete(tt);

    /* errors and the clocks a timer can use, with the numbers Linux gives */
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGUSR1;
    static const int clocks[] = { CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_BOOTTIME, CLOCK_TAI, CLOCK_MONOTONIC_RAW, CLOCK_REALTIME_COARSE, CLOCK_MONOTONIC_COARSE,
                                  CLOCK_REALTIME_ALARM, 10, 12 };
    for (unsigned i = 0; i < sizeof clocks / sizeof clocks[0]; i++) {
        errno = 0;
        r = syscall(SYS_timer_create, clocks[i], &sev, &id);
        printf("timer_create on clock %d: %ld, errno %d\n", clocks[i], r, r < 0 ? errno : 0);
        if (r == 0) syscall(SYS_timer_delete, id);
    }
    sev.sigev_signo = 0;
    errno = 0;
    r = syscall(SYS_timer_create, CLOCK_MONOTONIC, &sev, &id);
    printf("timer_create with signal 0: %ld, errno %d\n", r, errno);
    sev.sigev_signo = 65;
    errno = 0;
    r = syscall(SYS_timer_create, CLOCK_MONOTONIC, &sev, &id);
    printf("timer_create with signal 65: %ld, errno %d\n", r, errno);
    sev.sigev_signo = SIGUSR1;
    sev.sigev_notify = 9;
    errno = 0;
    r = syscall(SYS_timer_create, CLOCK_MONOTONIC, &sev, &id);
    printf("timer_create with notification 9: %ld, errno %d\n", r, errno);
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev._sigev_un._tid = 2000000000;
    errno = 0;
    r = syscall(SYS_timer_create, CLOCK_MONOTONIC, &sev, &id);
    printf("timer_create for a thread that does not exist: %ld, errno %d\n", r, errno);
    errno = 0;
    r = syscall(SYS_timer_settime, 77, 0, &its, NULL);
    printf("timer_settime of an unknown timer: %ld, errno %d\n", r, errno);
    errno = 0;
    r = syscall(SYS_timer_getoverrun, 77);
    printf("timer_getoverrun of an unknown timer: %ld, errno %d\n", r, errno);
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_NONE;
    syscall(SYS_timer_create, CLOCK_MONOTONIC, &sev, &id);
    its.it_value.tv_nsec = 1000000000;
    errno = 0;
    r = syscall(SYS_timer_settime, id, 0, &its, NULL);
    printf("timer_settime with 10^9 nanoseconds: %ld, errno %d\n", r, errno);
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_nsec = -1;
    errno = 0;
    r = syscall(SYS_timer_settime, id, 0, &its, NULL);
    printf("timer_settime with a negative interval: %ld, errno %d\n", r, errno);
    syscall(SYS_timer_delete, id);

    /* setitimer errors */
    errno = 0;
    r = setitimer((__itimer_which_t)3, &cur, NULL);
    printf("setitimer(3): %ld, errno %d\n", r, errno);
    errno = 0;
    r = getitimer((__itimer_which_t)3, &cur);
    printf("getitimer(3): %ld, errno %d\n", r, errno);
    cur.it_value.tv_sec = 0;
    cur.it_value.tv_usec = 1000000;
    cur.it_interval.tv_sec = 0;
    cur.it_interval.tv_usec = 0;
    errno = 0;
    r = setitimer(ITIMER_REAL, &cur, NULL);
    printf("setitimer with 10^6 microseconds: %ld, errno %d\n", r, errno);
    cur.it_value.tv_usec = 0;
    cur.it_interval.tv_sec = -1;
    errno = 0;
    r = setitimer(ITIMER_REAL, &cur, NULL);
    printf("setitimer with a negative interval: %ld, errno %d\n", r, errno);

    /* 5. clocks */
    static const int known[] = { CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID, CLOCK_MONOTONIC_RAW, CLOCK_REALTIME_COARSE,
                                 CLOCK_MONOTONIC_COARSE, CLOCK_BOOTTIME, CLOCK_TAI };
    int ok = 0, res_ok = 0;
    for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++) {
        struct timespec ts, rs;
        ok += clock_gettime(known[i], &ts) == 0;
        res_ok += clock_getres(known[i], &rs) == 0 && rs.tv_sec == 0 && rs.tv_nsec > 0;
    }
    printf("clock_gettime works for %d of 9 clocks, clock_getres for %d of 9\n", ok, res_ok);
    struct timespec ts;
    errno = 0;
    r = clock_gettime(10, &ts);
    printf("clock_gettime(10): %ld, errno %d\n", r, errno);
    errno = 0;
    r = clock_getres(12, &ts);
    printf("clock_getres(12): %ld, errno %d\n", r, errno);
    printf("clock_getres with no result pointer: %d\n", clock_getres(CLOCK_MONOTONIC, NULL));
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    nap(20);
    clock_gettime(CLOCK_MONOTONIC, &b);
    long dt = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    printf("CLOCK_MONOTONIC after a 20 ms sleep: advanced by 15 to 200 ms: %d\n", dt >= 15 && dt <= 200);
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    long diff = (long)rt.tv_sec - (long)time(NULL);
    printf("CLOCK_REALTIME agrees with time(): %d\n", diff >= -1 && diff <= 1);

    long p0 = cpu_ns(CLOCK_PROCESS_CPUTIME_ID), t0c = cpu_ns(CLOCK_THREAD_CPUTIME_ID);
    nap(200);
    long p1 = cpu_ns(CLOCK_PROCESS_CPUTIME_ID), t1c = cpu_ns(CLOCK_THREAD_CPUTIME_ID);
    printf("sleeping 200 ms used under 100 ms of CPU time: process %d, thread %d\n", p1 - p0 < 100000000L, t1c - t0c < 100000000L);
    long w0 = now_ms();
    volatile unsigned long x = 0;
    while (now_ms() - w0 < 150)
        for (int i = 0; i < 20000; i++) x += (unsigned long)i;
    long p2 = cpu_ns(CLOCK_PROCESS_CPUTIME_ID), t2c = cpu_ns(CLOCK_THREAD_CPUTIME_ID);
    printf("computing for 150 ms: process CPU time grew by 100 to 300 ms: %d, thread: %d\n", p2 - p1 >= 100000000L && p2 - p1 <= 300000000L,
           t2c - t1c >= 100000000L && t2c - t1c <= 300000000L);
    pthread_create(&th, NULL, spinner, NULL);
    pthread_join(th, NULL);
    long p3 = cpu_ns(CLOCK_PROCESS_CPUTIME_ID), t3c = cpu_ns(CLOCK_THREAD_CPUTIME_ID);
    printf("another thread computing for 200 ms: the process clock saw at least 150 ms of it: %d, this thread's clock almost none: %d, the thread's own clock saw it: %d\n",
           p3 - p2 >= 150000000L, t3c - t2c < 100000000L, spun_ns >= 150000000L);

    /* times and getrusage count the same CPU time, the process's own and that of the children it waited for */
    struct tms tm0, tm1;
    struct rusage ru0, ru1, rc0, rc1, rt0, rt1;
    times(&tm0);
    getrusage(RUSAGE_SELF, &ru0);
    getrusage(RUSAGE_CHILDREN, &rc0);
    getrusage(RUSAGE_THREAD, &rt0);
    w0 = now_ms();
    while (now_ms() - w0 < 150)
        for (int i = 0; i < 20000; i++) x += (unsigned long)i;
    times(&tm1);
    getrusage(RUSAGE_SELF, &ru1);
    getrusage(RUSAGE_THREAD, &rt1);
    long spent_ticks = (tm1.tms_utime + tm1.tms_stime) - (tm0.tms_utime + tm0.tms_stime);
    long spent_us = (ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec + ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) * 1000000L +
                    (ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec) + (ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec);
    long thread_us = (rt1.ru_utime.tv_sec - rt0.ru_utime.tv_sec + rt1.ru_stime.tv_sec - rt0.ru_stime.tv_sec) * 1000000L +
                     (rt1.ru_utime.tv_usec - rt0.ru_utime.tv_usec) + (rt1.ru_stime.tv_usec - rt0.ru_stime.tv_usec);
    printf("computing for 150 ms: times() grew by 10 to 40 hundredths: %d, getrusage(SELF) by 100 to 300 ms: %d, getrusage(THREAD) too: %d\n",
           spent_ticks >= 10 && spent_ticks <= 40, spent_us >= 100000 && spent_us <= 300000, thread_us >= 100000 && thread_us <= 300000);
    pid_t kid = fork();
    if (kid == 0) {
        long k0 = now_ms();
        volatile unsigned long y = 0;
        while (now_ms() - k0 < 200)
            for (int i = 0; i < 20000; i++) y += (unsigned long)i;
        _exit(0);
    }
    waitpid(kid, NULL, 0);
    times(&tm1);
    getrusage(RUSAGE_CHILDREN, &rc1);
    long child_ticks = (tm1.tms_cutime + tm1.tms_cstime) - (tm0.tms_cutime + tm0.tms_cstime);
    long child_us = (rc1.ru_utime.tv_sec - rc0.ru_utime.tv_sec + rc1.ru_stime.tv_sec - rc0.ru_stime.tv_sec) * 1000000L +
                    (rc1.ru_utime.tv_usec - rc0.ru_utime.tv_usec) + (rc1.ru_stime.tv_usec - rc0.ru_stime.tv_usec);
    printf("a child computed for 200 ms and was waited for: times() shows 15 to 40 hundredths of it: %d, getrusage(CHILDREN) 150 to 300 ms: %d\n",
           child_ticks >= 15 && child_ticks <= 40, child_us >= 150000 && child_us <= 300000);
    errno = 0;
    r = getrusage((__rusage_who_t)2, &ru1);
    printf("getrusage with a bad who: %ld, errno %d\n", r, errno);
    errno = 0;
    r = getrusage(RUSAGE_SELF, NULL);
    printf("getrusage with no buffer: %ld, errno %d\n", r, errno);
    printf("times(NULL) works: %d\n", times(NULL) != (clock_t)-1);

    /* nanosleep and clock_nanosleep: relative, absolute, interrupted, and the errors of the clocks */
    struct timespec req = { 0, 50000000 };
    t0 = now_ms();
    r = clock_nanosleep(CLOCK_MONOTONIC, 0, &req, NULL);
    long slept = now_ms() - t0;
    printf("clock_nanosleep(MONOTONIC, 50 ms): %ld, slept 45 ms to 1 s: %d\n", r, slept >= 45 && slept < 1000);
    clock_gettime(CLOCK_MONOTONIC, &req);
    req.tv_nsec += 60000000;
    if (req.tv_nsec >= 1000000000) { req.tv_sec++; req.tv_nsec -= 1000000000; }
    t0 = now_ms();
    r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &req, NULL);
    slept = now_ms() - t0;
    printf("clock_nanosleep(MONOTONIC, absolute, 60 ms ahead): %ld, slept 45 ms to 1 s: %d\n", r, slept >= 45 && slept < 1000);
    clock_gettime(CLOCK_REALTIME, &req);
    req.tv_sec -= 5;
    r = clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &req, NULL);
    printf("clock_nanosleep(REALTIME, absolute, in the past): %ld\n", r);
    hits = 0;
    settimer(ITIMER_REAL, 60000, 0);
    struct timespec long_sleep = { 2, 0 }, rem = { 0, 0 };
    t0 = now_ms();
    r = clock_nanosleep(CLOCK_MONOTONIC, 0, &long_sleep, &rem);
    slept = now_ms() - t0;
    printf("a 2 s sleep interrupted by SIGALRM after 60 ms: %ld (EINTR %d), the handler ran %d, woke early: %d, the time left is almost two seconds: %d\n", r, (int)EINTR, (int)hits,
           slept < 1000, rem.tv_sec == 1 && rem.tv_nsec > 800000000L);
    static const int sleep_clocks[] = { CLOCK_MONOTONIC_RAW, CLOCK_REALTIME_COARSE, CLOCK_REALTIME_ALARM, 10, 12 };
    for (unsigned i = 0; i < sizeof sleep_clocks / sizeof sleep_clocks[0]; i++) {
        struct timespec tiny = { 0, 1000 };
        errno = 0;
        r = syscall(SYS_clock_nanosleep, sleep_clocks[i], 0, &tiny, NULL);
        printf("clock_nanosleep on clock %d: %ld, errno %d\n", sleep_clocks[i], r, r < 0 ? errno : 0);
    }
    struct timespec bad = { 0, 1000000000 };
    errno = 0;
    r = nanosleep(&bad, NULL);
    printf("nanosleep with 10^9 nanoseconds: %ld, errno %d\n", r, errno);
    bad.tv_nsec = 0;
    bad.tv_sec = -1;
    errno = 0;
    r = nanosleep(&bad, NULL);
    printf("nanosleep with a negative time: %ld, errno %d\n", r, errno);
    return 0;
}
