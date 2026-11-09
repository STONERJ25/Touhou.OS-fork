/*
 * Real-time signals through a statically linked C library: sigqueue(3) with a value, delivery order (lowest number first, oldest first among equals), standard
 * signals that coalesce while real-time ones queue, sigwaitinfo and sigtimedwait with the siginfo they return, SA_NODEFER, SA_RESETHAND and SA_RESTART, a
 * handler that leaves with siglongjmp, sigsuspend, signals to one thread, the 64-signal masks, the default action of a real-time signal, and the errors of kill
 * and rt_sigqueueinfo. A real Linux prints the same lines (tools/linux-oracle.py); tests/case_linux_signals.py compares. The handlers only record events;
 * everything is printed from main.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAXEV 64
struct event { int sig, code, from_me, as_me; long value; };
static struct event ev[MAXEV];
static volatile int nev;
static pid_t g_pid;
static uid_t g_uid;

static void record(int sig, siginfo_t *si) {
    if (nev >= MAXEV) return;
    ev[nev].sig = sig;
    ev[nev].code = si->si_code;
    ev[nev].from_me = si->si_pid == g_pid;
    ev[nev].as_me = si->si_uid == g_uid;
    ev[nev].value = si->si_value.sival_int;
    nev++;
}

static void handler(int sig, siginfo_t *si, void *uc) { (void)uc; record(sig, si); }

static void install(int sig, void (*h)(int, siginfo_t *, void *), int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = h;
    sa.sa_flags = flags | SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

static void mask(int how, int a, int b, int c, int d) {
    sigset_t s;
    sigemptyset(&s);
    if (a) sigaddset(&s, a);
    if (b) sigaddset(&s, b);
    if (c) sigaddset(&s, c);
    if (d) sigaddset(&s, d);
    sigprocmask(how, &s, NULL);
}

static void queue(int sig, int value) {
    union sigval v;
    v.sival_int = value;
    if (sigqueue(g_pid, sig, v) != 0) printf("sigqueue(%d) failed: errno %d\n", sig, errno);
}

static void dump(const char *what) {
    printf("%s:", what);
    for (int i = 0; i < nev; i++)
        printf(" [sig %d code %d value %ld pid %s uid %s]", ev[i].sig, ev[i].code, ev[i].value, ev[i].from_me ? "me" : "other", ev[i].as_me ? "me" : "other");
    printf("%s\n", nev ? "" : " nothing");
    nev = 0;
}

static void nap(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------ nested delivery, SA_NODEFER, SA_RESETHAND */

static volatile int depth, max_depth;

static void nodefer_handler(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    record(sig, si);
    if (++depth > max_depth) max_depth = depth;
    if (si->si_value.sival_int > 0) {
        union sigval v;
        v.sival_int = si->si_value.sival_int - 1;
        sigqueue(g_pid, sig, v);                        /* queued while this handler runs: SA_NODEFER delivers it at once, inside */
    }
    depth--;
}

static sigjmp_buf jb;
static volatile int jumped;

static void jump_handler(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    record(sig, si);
    jumped++;
    siglongjmp(jb, 1);
}

static pthread_t main_thread;
static volatile int on_main, on_other;

static void thread_handler(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    record(sig, si);
    if (pthread_equal(pthread_self(), main_thread)) on_main++; else on_other++;
}

static volatile int ready;

static void *worker(void *arg) {
    (void)arg;
    sigset_t s;                                         /* a thread starts with its creator's mask: this one takes the signal the main thread blocks */
    sigemptyset(&s);
    sigaddset(&s, SIGRTMIN + 9);
    pthread_sigmask(SIG_UNBLOCK, &s, NULL);
    ready = 1;
    while (!nev) nap(5);
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);                   /* a child that is killed or leaves with _exit never loses a line, and nothing is printed twice by a fork */
    g_pid = getpid();
    g_uid = getuid();
    printf("SIGRTMIN %d, SIGRTMAX %d\n", SIGRTMIN, SIGRTMAX);

    /* 1. several real-time signals queued while blocked: delivered when unblocked; the handlers are entered lowest number first and nest, so the
     *    innermost one (the last entered) runs first -- the order in which they run is the same on every Linux */
    install(SIGRTMIN, handler, 0);
    install(SIGRTMIN + 1, handler, 0);
    install(SIGRTMIN + 2, handler, 0);
    install(SIGRTMAX, handler, 0);
    mask(SIG_BLOCK, SIGRTMIN, SIGRTMIN + 1, SIGRTMIN + 2, SIGRTMAX);
    queue(SIGRTMIN + 2, 1);
    queue(SIGRTMIN, 2);
    queue(SIGRTMIN, 3);
    queue(SIGRTMAX, 4);
    queue(SIGRTMIN + 1, 5);
    queue(SIGRTMIN, 6);
    sigset_t pend;
    sigpending(&pend);
    printf("pending: min %d min+1 %d min+2 %d max %d usr1 %d (nothing ran: %d)\n", sigismember(&pend, SIGRTMIN), sigismember(&pend, SIGRTMIN + 1),
           sigismember(&pend, SIGRTMIN + 2), sigismember(&pend, SIGRTMAX), sigismember(&pend, SIGUSR1), nev == 0);
    mask(SIG_UNBLOCK, SIGRTMIN, SIGRTMIN + 1, SIGRTMIN + 2, SIGRTMAX);
    dump("delivered");
    sigpending(&pend);
    printf("pending now: min %d max %d\n", sigismember(&pend, SIGRTMIN), sigismember(&pend, SIGRTMAX));

    /* 2. one signal number at a time: the same number cannot nest (it is blocked inside its handler), so the instances arrive in the order they were sent */
    mask(SIG_BLOCK, SIGRTMIN, 0, 0, 0);
    for (int i = 1; i <= 5; i++) queue(SIGRTMIN, 10 * i);
    mask(SIG_UNBLOCK, SIGRTMIN, 0, 0, 0);
    dump("five of one number");

    /* 3. kill(2) of a real-time signal queues as well; a standard signal sent three times is held once, and sigqueue on a pending standard signal is dropped */
    install(SIGUSR1, handler, 0);
    mask(SIG_BLOCK, SIGRTMIN + 1, SIGUSR1, 0, 0);
    for (int i = 0; i < 3; i++) kill(g_pid, SIGRTMIN + 1);
    for (int i = 0; i < 3; i++) kill(g_pid, SIGUSR1);
    queue(SIGUSR1, 99);
    mask(SIG_UNBLOCK, SIGRTMIN + 1, SIGUSR1, 0, 0);
    dump("kill: three real-time, three standard");
    mask(SIG_BLOCK, SIGUSR1, 0, 0, 0);
    queue(SIGUSR1, 7);
    queue(SIGUSR1, 8);
    mask(SIG_UNBLOCK, SIGUSR1, 0, 0, 0);
    dump("sigqueue of a standard signal twice");

    /* 4. sigwaitinfo: the siginfo of a blocked signal, without a handler */
    mask(SIG_BLOCK, SIGRTMIN + 3, 0, 0, 0);
    queue(SIGRTMIN + 3, 77);
    sigset_t wait;
    sigemptyset(&wait);
    sigaddset(&wait, SIGRTMIN + 3);
    siginfo_t info;
    int r = sigwaitinfo(&wait, &info);
    printf("sigwaitinfo: %d, signo %d, code %d (SI_QUEUE %d), value %d, pid is me %d, uid is me %d\n", r, info.si_signo, info.si_code, SI_QUEUE,
           info.si_value.sival_int, info.si_pid == g_pid, info.si_uid == g_uid);
    struct timespec zero = { 0, 0 };
    errno = 0;
    r = sigtimedwait(&wait, &info, &zero);
    printf("sigtimedwait with nothing pending and no time: %d, errno %d (EAGAIN %d)\n", r, errno, errno == EAGAIN);
    struct timespec hundred = { 0, 100 * 1000000L };
    long t0 = now_ms();
    errno = 0;
    r = sigtimedwait(&wait, &info, &hundred);
    long waited = now_ms() - t0;
    printf("sigtimedwait for 100 ms: %d, errno %d (EAGAIN %d), waited at least 90 ms: %d, at most 2 s: %d\n", r, errno, errno == EAGAIN, waited >= 90, waited < 2000);

    /* a child sends the signal later; the parent sleeps in sigtimedwait and wakes up at once with the child's pid */
    pid_t c = fork();
    if (c == 0) {
        nap(200);
        union sigval v;
        v.sival_int = 4242;
        sigqueue(getppid(), SIGRTMIN + 3, v);
        _exit(0);
    }
    struct timespec five = { 5, 0 };
    t0 = now_ms();
    r = sigtimedwait(&wait, &info, &five);
    waited = now_ms() - t0;
    printf("sigtimedwait woken by the child: %d, value %d, code %d, from the child %d, fast %d\n", r, info.si_value.sival_int, info.si_code,
           info.si_pid == c, waited < 3000);
    waitpid(c, NULL, 0);
    mask(SIG_UNBLOCK, SIGRTMIN + 3, 0, 0, 0);

    /* 5. SA_NODEFER: the handler is entered again while it runs; without it the instances run one after the other */
    install(SIGRTMIN + 4, nodefer_handler, SA_NODEFER);
    depth = max_depth = 0;
    queue(SIGRTMIN + 4, 3);
    nap(20);
    dump("SA_NODEFER");
    printf("deepest nesting with SA_NODEFER: %d\n", max_depth);
    install(SIGRTMIN + 4, nodefer_handler, 0);
    depth = max_depth = 0;
    queue(SIGRTMIN + 4, 3);
    nap(20);
    dump("without SA_NODEFER");
    printf("deepest nesting without it: %d\n", max_depth);

    /* 6. SA_RESETHAND: the second signal finds the default action again */
    install(SIGRTMIN + 5, handler, SA_RESETHAND);
    queue(SIGRTMIN + 5, 1);
    struct sigaction now;
    sigaction(SIGRTMIN + 5, NULL, &now);
    printf("SA_RESETHAND: handled once, the action is the default again: %d\n", now.sa_handler == SIG_DFL);
    dump("SA_RESETHAND");

    /* 7. a handler that leaves with siglongjmp, taking the saved mask back */
    install(SIGRTMIN + 6, jump_handler, 0);
    if (sigsetjmp(jb, 1) == 0) {
        queue(SIGRTMIN + 6, 55);
        nap(50);
        printf("not reached\n");
    }
    sigset_t cur;
    sigprocmask(SIG_BLOCK, NULL, &cur);
    printf("siglongjmp out of a real-time handler: jumped %d, the signal is unblocked again: %d\n", jumped, !sigismember(&cur, SIGRTMIN + 6));
    dump("siglongjmp");

    /* 8. sigsuspend ends with the real-time signal */
    install(SIGRTMIN + 7, handler, 0);
    mask(SIG_BLOCK, SIGRTMIN + 7, 0, 0, 0);
    c = fork();
    if (c == 0) {
        nap(200);
        union sigval v;
        v.sival_int = 31;
        sigqueue(getppid(), SIGRTMIN + 7, v);
        _exit(0);
    }
    sigset_t empty;
    sigemptyset(&empty);
    errno = 0;
    r = sigsuspend(&empty);
    printf("sigsuspend: %d, errno %d (EINTR %d)\n", r, errno, errno == EINTR);
    dump("sigsuspend");
    waitpid(c, NULL, 0);
    mask(SIG_UNBLOCK, SIGRTMIN + 7, 0, 0, 0);

    /* 9. SA_RESTART: a read that is interrupted by a real-time signal starts again; without it the read fails with EINTR */
    int p[2];
    pipe(p);
    for (int pass = 0; pass < 2; pass++) {
        install(SIGRTMIN + 8, handler, pass == 0 ? SA_RESTART : 0);
        c = fork();
        if (c == 0) {
            nap(200);
            union sigval v;
            v.sival_int = 1;
            sigqueue(getppid(), SIGRTMIN + 8, v);
            nap(600);
            write(p[1], "x", 1);
            _exit(0);
        }
        char b;
        errno = 0;
        ssize_t n = read(p[0], &b, 1);
        printf("read %s SA_RESTART: %zd, errno %d (EINTR %d)\n", pass == 0 ? "with" : "without", n, errno, errno == EINTR);
        dump("handler");
        if (n < 0) read(p[0], &b, 1);                   /* collect the byte */
        waitpid(c, NULL, 0);
    }

    /* 10. a signal for one thread, and a process-directed one that only the other thread can take */
    install(SIGRTMIN + 9, thread_handler, 0);
    main_thread = pthread_self();
    mask(SIG_BLOCK, SIGRTMIN + 9, 0, 0, 0);
    pthread_t th;
    ready = 0;
    pthread_create(&th, NULL, worker, NULL);
    while (!ready) nap(5);
    union sigval sv;
    sv.sival_int = 9;
    sigqueue(g_pid, SIGRTMIN + 9, sv);                  /* the main thread blocks it, so the worker takes it */
    pthread_join(th, NULL);
    printf("process-directed signal taken by the thread that does not block it: other %d, main %d\n", on_other, on_main);
    dump("thread");
    sigset_t tp;
    sigpending(&tp);
    printf("nothing left pending in the blocked main thread: %d\n", !sigismember(&tp, SIGRTMIN + 9));
    mask(SIG_UNBLOCK, SIGRTMIN + 9, 0, 0, 0);

    /* 11. the 64-signal masks: block everything, read it back; SIGKILL and SIGSTOP cannot be blocked. Signal 31 (SIGSYS) is left out of the count: TouhouOS keeps
     *     it for its own thread library, so a Linux program cannot block or catch it (docs/COMPAT.md) */
    sigset_t all, back;
    sigfillset(&all);
    sigprocmask(SIG_SETMASK, &all, &cur);
    sigprocmask(SIG_SETMASK, NULL, &back);
    int members = 0;
    for (int s = 1; s <= 64; s++) members += s != 31 && sigismember(&back, s) == 1;
    printf("after sigfillset: %d signals blocked of the 61 the library shows (SIGKILL %d, SIGSTOP %d, SIGRTMIN %d, SIGRTMAX %d)\n", members,
           sigismember(&back, SIGKILL), sigismember(&back, SIGSTOP), sigismember(&back, SIGRTMIN), sigismember(&back, SIGRTMAX));
    sigprocmask(SIG_SETMASK, &cur, NULL);
    sigprocmask(SIG_BLOCK, NULL, &back);
    members = 0;
    for (int s = 1; s <= 64; s++) members += s != 31 && sigismember(&back, s) == 1;
    printf("restored: %d signals blocked\n", members);

    /* 12. the default action of a real-time signal ends the process; SIG_IGN drops it */
    fflush(stdout);
    c = fork();
    if (c == 0) { signal(SIGRTMIN + 10, SIG_DFL); kill(getpid(), SIGRTMIN + 10); _exit(0); }
    int status = 0;
    waitpid(c, &status, 0);
    printf("child sent SIGRTMIN+10 with the default action: signaled %d, signal is SIGRTMIN+10: %d\n", WIFSIGNALED(status) ? 1 : 0,
           WIFSIGNALED(status) && WTERMSIG(status) == SIGRTMIN + 10);
    c = fork();
    if (c == 0) { signal(SIGRTMAX, SIG_IGN); kill(getpid(), SIGRTMAX); _exit(7); }
    waitpid(c, &status, 0);
    printf("child ignored SIGRTMAX: exited %d with %d\n", WIFEXITED(status) ? 1 : 0, WIFEXITED(status) ? WEXITSTATUS(status) : -1);

    /* 13. errors */
    errno = 0;
    r = kill(g_pid, 65);
    printf("kill with signal 65: %d, errno %d (EINVAL %d)\n", r, errno, errno == EINVAL);
    errno = 0;
    r = kill(g_pid, 0);
    printf("kill with signal 0: %d\n", r);
    errno = 0;
    r = sigqueue(g_pid, 65, sv);
    printf("sigqueue with signal 65: %d, errno %d (EINVAL %d)\n", r, errno, errno == EINVAL);
    errno = 0;
    r = kill(2000000000, SIGRTMIN);
    printf("kill of a process that does not exist: %d, errno %d (ESRCH %d)\n", r, errno, errno == ESRCH);
    c = fork();
    if (c == 0) { nap(500); _exit(0); }
    siginfo_t forged;
    memset(&forged, 0, sizeof forged);
    forged.si_signo = SIGRTMIN;
    forged.si_code = SI_USER;                           /* a program cannot pretend that kill(2) sent this, except to itself */
    errno = 0;
    r = (int)syscall(SYS_rt_sigqueueinfo, c, SIGRTMIN, &forged);
    printf("rt_sigqueueinfo with SI_USER to another process: %d, errno %d (EPERM %d)\n", r, errno, errno == EPERM);
    forged.si_code = SI_QUEUE;
    errno = 0;
    r = (int)syscall(SYS_rt_sigqueueinfo, c, 0, &forged);
    printf("rt_sigqueueinfo with SI_QUEUE and signal 0: %d\n", r);
    kill(c, SIGKILL);
    waitpid(c, NULL, 0);
    errno = 0;
    r = (int)syscall(SYS_tgkill, g_pid, 2000000000, SIGRTMIN);
    printf("tgkill of a thread that does not exist: %d, errno %d (ESRCH %d)\n", r, errno, errno == ESRCH);
    errno = 0;
    r = (int)syscall(SYS_tgkill, -1, g_pid, SIGRTMIN);
    printf("tgkill with a bad process id: %d, errno %d (EINVAL %d)\n", r, errno, errno == EINVAL);
    return 0;
}
