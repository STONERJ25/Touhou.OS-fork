/*
 * SIGSYS (signal 31) as an ordinary signal, through a statically linked C library. TouhouOS kept signal 31 out of sight of Linux programs until 2026-11-09, because its
 * own thread library uses it as pthread_cancel's request; now it is the native personality's alone. A real Linux prints the same lines (tools/linux-oracle.py);
 * tests/case_linux_signals.py compares.
 *
 * What a program does with a signal it may use: install a handler with sigaction and read the siginfo, raise it, kill, sigqueue and pthread_kill it at itself, ask what
 * sigaction knows, block it and see it pending and delivered at the unblock, ignore it, take it with sigwaitinfo and sigtimedwait, wait for it in sigsuspend, get it as
 * the notification of a POSIX timer, aim it at one thread or at a process whose main thread blocks it, mask it with a full set, and die of it by the default action.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t hits;
static volatile int last_signo, last_code, last_value;
static volatile pid_t last_pid, last_tid;

static void on_sys(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    last_signo = sig;
    last_code = si->si_code;
    last_pid = si->si_pid;
    last_value = si->si_value.sival_int;
    last_tid = (pid_t)syscall(SYS_gettid);
    hits++;
}

static void install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sys;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    printf("sigaction(SIGSYS): %d\n", sigaction(SIGSYS, &sa, NULL));
}

static void report(const char *what, int expected_code) {
    printf("%s: %d signal(s), signo %d, code %d%s, sent by this process %d\n", what, (int)hits, last_signo, last_code, last_code == expected_code ? " (as expected)" : " (NOT as expected)",
           last_pid == getpid());
    hits = 0;
}

static void nap(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

static void wait_for_hit(void) {
    for (int i = 0; i < 5000 && !hits; i++) nap(1);
}

/* a thread that has SIGSYS unblocked (its creator blocked it) and waits for the handler to run in it */
static volatile pid_t thread_tid;
static void *waiter(void *arg) {
    (void)arg;
    thread_tid = (pid_t)syscall(SYS_gettid);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGSYS);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    wait_for_hit();
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    install();

    /* the ways to send it to oneself */
    raise(SIGSYS);
    report("raise", SI_TKILL);
    kill(getpid(), SIGSYS);
    report("kill", SI_USER);
    kill(getpid(), 31);
    report("kill with the number 31", SI_USER);
    pthread_kill(pthread_self(), SIGSYS);
    report("pthread_kill (self)", SI_TKILL);
    union sigval v;
    v.sival_int = 4242;
    sigqueue(getpid(), SIGSYS, v);
    printf("sigqueue: %d signal(s), signo %d, code %d%s, value %d\n", (int)hits, last_signo, last_code, last_code == SI_QUEUE ? " (as expected)" : " (NOT as expected)", last_value);
    hits = 0;

    /* what sigaction knows */
    struct sigaction old;
    memset(&old, 0, sizeof old);
    int rc = sigaction(SIGSYS, NULL, &old);
    printf("sigaction query: %d, handler is ours %d, SA_SIGINFO %d\n", rc, old.sa_sigaction == on_sys, (old.sa_flags & SA_SIGINFO) != 0);

    /* blocked: pending until it is unblocked */
    sigset_t set, cur, pend;
    sigemptyset(&set);
    sigaddset(&set, SIGSYS);
    sigprocmask(SIG_BLOCK, &set, NULL);
    sigprocmask(SIG_SETMASK, NULL, &cur);
    printf("blocked: in the mask %d, neighbours 30 and 32 in the mask %d %d\n", sigismember(&cur, SIGSYS), sigismember(&cur, 30), sigismember(&cur, 32));
    raise(SIGSYS);
    sigpending(&pend);
    printf("blocked: delivered at once %d, pending %d\n", (int)hits, sigismember(&pend, SIGSYS));
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    report("unblocked", SI_TKILL);
    sigpending(&pend);
    printf("unblocked: pending now %d\n", sigismember(&pend, SIGSYS));

    /* a full set blocks it too, and reads back */
    sigset_t all;
    sigfillset(&all);
    sigprocmask(SIG_SETMASK, &all, NULL);
    sigprocmask(SIG_SETMASK, NULL, &cur);
    printf("full mask: SIGSYS in it %d, SIGUSR1 in it %d\n", sigismember(&cur, SIGSYS), sigismember(&cur, SIGUSR1));
    sigemptyset(&cur);
    sigprocmask(SIG_SETMASK, &cur, NULL);
    sigprocmask(SIG_SETMASK, NULL, &cur);
    printf("empty mask: SIGSYS in it %d\n", sigismember(&cur, SIGSYS));

    /* ignored: nothing happens, signal() hands back the handler it replaces */
    void (*prev)(int) = signal(SIGSYS, SIG_IGN);
    printf("signal(SIG_IGN): the previous handler was ours %d\n", (void *)prev == (void *)on_sys);
    raise(SIGSYS);
    printf("ignored: %d signal(s)\n", (int)hits);
    install();

    /* taken with sigwaitinfo and sigtimedwait while blocked */
    sigprocmask(SIG_BLOCK, &set, NULL);
    kill(getpid(), SIGSYS);
    siginfo_t si;
    memset(&si, 0, sizeof si);
    int r = sigwaitinfo(&set, &si);
    printf("sigwaitinfo: %d, signo %d, code %d, sent by this process %d, handler ran %d\n", r, si.si_signo, si.si_code, si.si_pid == getpid(), (int)hits);
    struct timespec zero = { 0, 0 };
    errno = 0;
    r = sigtimedwait(&set, NULL, &zero);
    printf("sigtimedwait with nothing pending: %d, %s\n", r, errno == EAGAIN ? "EAGAIN" : "something else");

    /* the notification of a POSIX timer */
    timer_t tm;
    struct sigevent sev;
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGSYS;
    printf("timer_create with SIGSYS: %d\n", timer_create(CLOCK_MONOTONIC, &sev, &tm));
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = 20 * 1000000;
    timer_settime(tm, 0, &its, NULL);
    memset(&si, 0, sizeof si);
    r = sigwaitinfo(&set, &si);
    printf("timer: sigwaitinfo %d, signo %d, code %d%s\n", r, si.si_signo, si.si_code, si.si_code == SI_TIMER ? " (as expected)" : " (NOT as expected)");
    timer_delete(tm);

    /* sigsuspend: the signal is sent while it is blocked and runs its handler when the mask is swapped */
    hits = 0;
    pid_t kid = fork();
    if (kid == 0) {
        nap(30);
        kill(getppid(), SIGSYS);
        _exit(0);
    }
    sigset_t none;
    sigemptyset(&none);
    errno = 0;
    r = sigsuspend(&none);
    int suspend_errno = errno;
    sigset_t after;
    sigprocmask(SIG_SETMASK, NULL, &after);
    printf("sigsuspend: %d, %s, handler ran %d, blocked again afterwards %d\n", r, suspend_errno == EINTR ? "EINTR" : "something else", (int)hits, sigismember(&after, SIGSYS));
    waitpid(kid, NULL, 0);
    hits = 0;

    /* one thread, and a process whose main thread blocks it (SIGSYS is blocked here since the sigwaitinfo above) */
    pthread_t th;
    pthread_create(&th, NULL, waiter, NULL);
    while (!thread_tid) nap(1);
    nap(20);
    pthread_kill(th, SIGSYS);
    pthread_join(th, NULL);
    printf("pthread_kill (other thread): %d signal(s), the handler ran in that thread %d, code %d\n", (int)hits, last_tid == thread_tid, last_code);
    hits = 0;
    thread_tid = 0;
    pthread_create(&th, NULL, waiter, NULL);
    while (!thread_tid) nap(1);
    nap(20);
    kill(getpid(), SIGSYS);
    pthread_join(th, NULL);
    printf("kill with the main thread blocking it: %d signal(s), the handler ran in the other thread %d, code %d\n", (int)hits, last_tid == thread_tid, last_code);
    hits = 0;

    /* the default action ends the process by the signal */
    kid = fork();
    if (kid == 0) {
        signal(SIGSYS, SIG_DFL);
        sigprocmask(SIG_UNBLOCK, &set, NULL);
        raise(SIGSYS);
        _exit(3);
    }
    int st = 0;
    waitpid(kid, &st, 0);
    printf("default action: the child %s, signal %d\n", WIFSIGNALED(st) ? "was killed by a signal" : "was not killed by a signal", WIFSIGNALED(st) ? WTERMSIG(st) : -1);
    return 0;
}
