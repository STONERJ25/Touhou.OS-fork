/*
 * Signals through a statically linked C library: handlers with and without siginfo, blocking and pending, SIG_IGN and SA_RESETHAND, the mask inside a handler,
 * sigsuspend, alarm and pause, a SIGSEGV handler that leaves with siglongjmp (si_addr and si_code of the fault), the alternate signal stack, floating-point state
 * that survives a handler, a system call restarted (SA_RESTART) or not (EINTR), the default action of SIGTERM and SIGABRT seen by wait, and SIGCHLD. A real Linux
 * prints the same lines (tools/linux-oracle.py); tests/case_linux.py compares. The handlers only set variables; everything is printed from main.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t hits, last_sig, last_code, blocked_inside, flag, stack_ok;
static volatile unsigned long last_addr;
static sigjmp_buf env;
static char altstack[32768];

static void on_signal(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    hits++;
    last_sig = si->si_signo;
    last_code = si->si_code;
    sigset_t cur;
    sigprocmask(SIG_BLOCK, NULL, &cur);
    blocked_inside = sigismember(&cur, sig);
}

static void plain(int sig) { hits++; last_sig = sig; }

static void on_segv(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    last_sig = sig;
    last_code = si->si_code;
    last_addr = (unsigned long)si->si_addr;
    siglongjmp(env, 1);
}

static void on_alt(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si; (void)uc;
    char here;
    stack_ok = (&here >= altstack && &here < altstack + sizeof altstack);
    hits++;
}

static void clobber(int sig) {
    (void)sig;
    __asm__ volatile("pxor %%xmm0, %%xmm0\n\tpxor %%xmm1, %%xmm1\n\tpxor %%xmm2, %%xmm2" ::: "xmm0", "xmm1", "xmm2");
    flag = 1;
}

static void nap(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

static void block(int sig) {
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, sig);
    sigprocmask(SIG_BLOCK, &s, NULL);
}

static void unblock(int sig) {
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, sig);
    sigprocmask(SIG_UNBLOCK, &s, NULL);
}

static void install(int sig, void (*h)(int, siginfo_t *, void *), int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = h;
    sa.sa_flags = flags | SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

int main(void) {
    install(SIGUSR1, on_signal, 0);
    raise(SIGUSR1);
    printf("SIGUSR1 delivered: hits %d, signo %d, blocked inside the handler %d\n", (int)hits, (int)last_sig, (int)blocked_inside);

    sigset_t set, pend;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, NULL);
    raise(SIGUSR1);
    sigpending(&pend);
    printf("blocked: hits %d, pending %d\n", (int)hits, sigismember(&pend, SIGUSR1));
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    printf("unblocked: hits %d\n", (int)hits);

    signal(SIGUSR1, SIG_IGN);
    raise(SIGUSR1);
    printf("ignored: hits %d\n", (int)hits);

    struct sigaction once;
    memset(&once, 0, sizeof once);
    once.sa_handler = plain;
    once.sa_flags = SA_RESETHAND;
    sigaction(SIGUSR2, &once, NULL);
    raise(SIGUSR2);
    struct sigaction now;
    sigaction(SIGUSR2, NULL, &now);
    printf("SA_RESETHAND: hits %d, the handler is default again: %d\n", (int)hits, now.sa_handler == SIG_DFL);

    /* sigsuspend: a child sends SIGUSR2 to the parent after a short sleep */
    install(SIGUSR2, on_signal, 0);
    block(SIGUSR2);
    pid_t parent = getpid();
    pid_t c = fork();
    if (c == 0) { nap(300); kill(parent, SIGUSR2); _exit(0); }
    sigset_t empty;
    sigemptyset(&empty);
    hits = 0;
    errno = 0;
    int r = sigsuspend(&empty);
    printf("sigsuspend: %d, errno %d, hits %d, signo %d\n", r, errno, (int)hits, (int)last_sig);
    waitpid(c, NULL, 0);
    unblock(SIGUSR2);

    /* alarm and pause */
    install(SIGALRM, on_signal, 0);
    hits = 0;
    alarm(1);
    errno = 0;
    r = pause();
    printf("pause after alarm: %d, errno %d, hits %d, signo %d\n", r, errno, (int)hits, (int)last_sig);

    /* a fault: SIGSEGV with the address and the reason, left with siglongjmp */
    install(SIGSEGV, on_segv, SA_NODEFER);
    if (sigsetjmp(env, 1) == 0) {
        *(volatile int *)0x10 = 1;
        printf("not reached\n");
    }
    printf("SIGSEGV: signo %d, code %d (1 = unmapped), address %#lx\n", (int)last_sig, (int)last_code, last_addr);
    sigset_t cur;
    sigprocmask(SIG_BLOCK, NULL, &cur);
    printf("the mask was restored by siglongjmp: %d\n", !sigismember(&cur, SIGSEGV));

    /* the alternate signal stack */
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0 };
    printf("sigaltstack: %d\n", sigaltstack(&ss, NULL));
    install(SIGUSR1, on_alt, SA_ONSTACK);
    hits = 0;
    raise(SIGUSR1);
    printf("handler ran on the alternate stack: %d (hits %d)\n", (int)stack_ok, (int)hits);

    /* floating-point state survives a handler that clobbers it */
    struct sigaction fa;
    memset(&fa, 0, sizeof fa);
    fa.sa_handler = clobber;
    sigaction(SIGALRM, &fa, NULL);
    double val = 3.140625, out = 0;
    flag = 0;
    alarm(1);
    __asm__ volatile(
        "movsd %2, %%xmm0\n"
        "1: cmpl $0, %1\n"
        "je 1b\n"
        "movsd %%xmm0, %0\n"
        : "=m"(out) : "m"(flag), "m"(val) : "xmm0", "memory");
    printf("xmm0 survived the handler: %d\n", out == val);

    /* SA_RESTART: a read that waits for a pipe is started again after the handler; without it the read fails with EINTR */
    int p[2];
    pipe(p);
    struct sigaction ra;
    memset(&ra, 0, sizeof ra);
    ra.sa_handler = plain;
    ra.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &ra, NULL);
    c = fork();
    if (c == 0) { nap(2000); write(p[1], "x", 1); _exit(0); }
    hits = 0;
    alarm(1);
    char b;
    errno = 0;
    ssize_t n = read(p[0], &b, 1);
    printf("SA_RESTART read: %zd, handler ran %d times\n", n, (int)hits);
    waitpid(c, NULL, 0);
    ra.sa_flags = 0;
    sigaction(SIGALRM, &ra, NULL);
    c = fork();
    if (c == 0) { nap(2000); write(p[1], "y", 1); _exit(0); }
    hits = 0;
    alarm(1);
    errno = 0;
    n = read(p[0], &b, 1);
    printf("read without SA_RESTART: %zd, errno %d (EINTR %d), handler ran %d times\n", n, errno, errno == EINTR, (int)hits);
    waitpid(c, NULL, 0);

    /* default actions seen by the parent */
    signal(SIGUSR1, SIG_DFL);
    signal(SIGALRM, SIG_DFL);
    fflush(stdout);
    c = fork();
    if (c == 0) { raise(SIGTERM); _exit(0); }
    int status = 0;
    waitpid(c, &status, 0);
    printf("child raised SIGTERM: signaled %d, signal %d\n", WIFSIGNALED(status) ? 1 : 0, WTERMSIG(status));
    c = fork();
    if (c == 0) { abort(); }
    waitpid(c, &status, 0);
    printf("child called abort: signaled %d, signal %d\n", WIFSIGNALED(status) ? 1 : 0, WTERMSIG(status));

    /* SIGCHLD when a child exits */
    install(SIGCHLD, on_signal, 0);
    hits = 0;
    c = fork();
    if (c == 0) _exit(0);
    waitpid(c, NULL, 0);
    nap(50);
    printf("SIGCHLD: hits %d, signo %d\n", (int)hits, (int)last_sig);
    return 0;
}
