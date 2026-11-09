/*
 * Who sent a signal, through a statically linked C library: the siginfo a handler or sigwaitinfo receives for kill, tkill, tgkill and raise, pthread_kill, alarm,
 * a broken pipe and sigqueue, the SIGCHLD of a child that exits, is killed, stops and continues (si_code, si_pid, si_uid, si_status), waitid with its id types and
 * options, and kill(2) to a whole process group (kill(0, sig) and kill(-pgid, sig)). A real Linux prints the same lines (tools/linux-oracle.py);
 * tests/case_linux_signals.py compares. Children report what they saw through a pipe; everything is printed by the parent.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t g_pid;
static uid_t g_uid;

struct seen { int signo, code, pid_is_sender, status; unsigned uid; };
static volatile sig_atomic_t hits;
static struct seen last;
static volatile pid_t expect_sender;

static void note(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    last.signo = sig;
    last.code = si->si_code;
    last.pid_is_sender = si->si_pid == expect_sender;
    last.uid = si->si_uid;
    last.status = si->si_status;
    hits++;
}

static void install(int sig, int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = note;
    sa.sa_flags = flags | SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

static const char *who(unsigned uid) { return uid == 0 ? "zero" : uid == g_uid ? "mine" : "other"; }

static void show(const char *what, int expected_code) {
    printf("%s: %d signal(s), signo %d, code %d%s, sender is the expected process %d, uid is %s\n", what, (int)hits, last.signo, last.code,
           last.code == expected_code ? " (as expected)" : " (NOT as expected)", last.pid_is_sender, who(last.uid));
    hits = 0;
}

static void nap(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------ SIGCHLD through sigwaitinfo: deterministic, no handler involved */

static sigset_t chld_set;

static void chld_event(const char *what, pid_t child) {
    siginfo_t si;
    int r = sigwaitinfo(&chld_set, &si);
    printf("SIGCHLD after %s: %d, signo %d, code %d, from the child %d, uid is mine %d, status %d, times are not negative %d\n", what, r, si.si_signo, si.si_code,
           si.si_pid == child, si.si_uid == g_uid, si.si_status, si.si_utime >= 0 && si.si_stime >= 0);
}

static pid_t spawn_sleeper(void) {
    pid_t c = fork();
    if (c == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        for (;;) pause();
    }
    return c;
}

static void wait_info(const char *what, idtype_t type, id_t id, int options) {
    siginfo_t si;
    memset(&si, 0x55, sizeof si);
    errno = 0;
    int r = waitid(type, id, &si, options);
    if (r < 0) { printf("waitid %s: -1, errno %d\n", what, errno); return; }
    printf("waitid %s: 0, signo %d, code %d, pid %s, uid %s, status %d\n", what, si.si_signo, si.si_code, si.si_pid == 0 ? "zero" : "set",
           si.si_pid == 0 ? "-" : (si.si_uid == g_uid ? "mine" : "other"), si.si_status);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_pid = getpid();
    g_uid = getuid();
    setpgid(0, 0);                                      /* our own process group: kill(0, sig) below must not reach whoever started us */

    /* 1. kill(2): SI_USER, the sender's pid and user */
    int p[2];
    pipe(p);
    pid_t c = fork();
    if (c == 0) {
        install(SIGUSR1, 0);
        expect_sender = getppid();
        write(p[1], "r", 1);                            /* ready */
        while (!hits) pause();
        write(p[1], &last, sizeof last);
        _exit(0);
    }
    char b;
    read(p[0], &b, 1);
    kill(c, SIGUSR1);
    struct seen s;
    read(p[0], &s, sizeof s);
    waitpid(c, NULL, 0);
    printf("kill(2): signo %d, code %d (SI_USER %d), sender is the parent %d, uid is %s\n", s.signo, s.code, SI_USER, s.pid_is_sender, who(s.uid));

    /* 2. raise, tgkill, tkill, pthread_kill: SI_TKILL with our own pid */
    install(SIGUSR2, 0);
    expect_sender = g_pid;
    hits = 0;
    raise(SIGUSR2);
    show("raise", SI_TKILL);
    syscall(SYS_tgkill, g_pid, (pid_t)syscall(SYS_gettid), SIGUSR2);
    show("tgkill", SI_TKILL);
    syscall(SYS_tkill, (pid_t)syscall(SYS_gettid), SIGUSR2);
    show("tkill", SI_TKILL);
    pthread_kill(pthread_self(), SIGUSR2);
    show("pthread_kill", SI_TKILL);
    kill(g_pid, SIGUSR2);
    show("kill of ourselves", SI_USER);
    union sigval v;
    v.sival_int = 5;
    sigqueue(g_pid, SIGUSR2, v);
    show("sigqueue", SI_QUEUE);

    /* 3. the kernel's own signals: an alarm has no sender (SI_KERNEL, pid 0); a broken pipe is the process's own doing (SI_USER with its own pid) */
    install(SIGALRM, 0);
    expect_sender = 0;
    alarm(1);
    pause();
    show("alarm", SI_KERNEL);
    install(SIGPIPE, 0);
    expect_sender = g_pid;
    int q[2];
    pipe(q);
    close(q[0]);
    errno = 0;
    ssize_t n = write(q[1], "x", 1);
    printf("write to a broken pipe: %zd, errno %d (EPIPE %d)\n", n, errno, errno == EPIPE);
    show("SIGPIPE", SI_USER);
    signal(SIGPIPE, SIG_DFL);

    /* 4. SIGCHLD: a child exits, is killed, stops and continues */
    sigemptyset(&chld_set);
    sigaddset(&chld_set, SIGCHLD);
    sigprocmask(SIG_BLOCK, &chld_set, NULL);
    c = fork();
    if (c == 0) _exit(7);
    chld_event("exit(7)", c);
    struct rusage ru;
    memset(&ru, 0x55, sizeof ru);                       /* a buffer that is not left as it was is a buffer that wait4 filled in */
    int status = 0;
    pid_t w4 = wait4(c, &status, 0, &ru);
    printf("wait4 with a rusage: the child, exited with 7: %d, its times are small: %d\n", w4 == c && WIFEXITED(status) && WEXITSTATUS(status) == 7,
           ru.ru_utime.tv_sec == 0 && ru.ru_stime.tv_sec == 0 && ru.ru_utime.tv_usec >= 0 && ru.ru_utime.tv_usec < 900000);
    c = spawn_sleeper();
    nap(100);
    kill(c, SIGTERM);
    chld_event("SIGTERM", c);
    waitpid(c, NULL, 0);
    c = spawn_sleeper();
    nap(100);
    kill(c, SIGSTOP);
    chld_event("SIGSTOP", c);
    pid_t w = waitpid(c, &status, WUNTRACED);
    printf("waitpid(WUNTRACED): %s, stopped %d, stop signal %d\n", w == c ? "the child" : "someone else", WIFSTOPPED(status) ? 1 : 0, WSTOPSIG(status));
    kill(c, SIGCONT);
    chld_event("SIGCONT", c);
    w = waitpid(c, &status, WCONTINUED);
    printf("waitpid(WCONTINUED): %s, continued %d\n", w == c ? "the child" : "someone else", WIFCONTINUED(status) ? 1 : 0);
    kill(c, SIGKILL);
    chld_event("SIGKILL", c);
    waitpid(c, NULL, 0);

    /* the same through a handler: SA_SIGINFO on SIGCHLD */
    sigprocmask(SIG_UNBLOCK, &chld_set, NULL);
    install(SIGCHLD, 0);
    c = fork();
    if (c == 0) _exit(3);
    expect_sender = c;
    for (int i = 0; i < 200 && !hits; i++) nap(10);
    show("SIGCHLD handler for exit(3)", CLD_EXITED);
    printf("status seen by the handler: %d\n", last.status);
    waitpid(c, NULL, 0);
    signal(SIGCHLD, SIG_DFL);

    /* 5. waitid */
    sigprocmask(SIG_BLOCK, &chld_set, NULL);
    c = fork();
    if (c == 0) _exit(5);
    chld_event("exit(5) for waitid", c);
    wait_info("P_PID WEXITED|WNOWAIT", P_PID, c, WEXITED | WNOWAIT);
    wait_info("P_PID WEXITED (reaps)", P_PID, c, WEXITED);
    wait_info("P_PID again", P_PID, c, WEXITED);
    c = spawn_sleeper();
    nap(100);
    wait_info("P_ALL WEXITED|WNOHANG, a child still runs", P_ALL, 0, WEXITED | WNOHANG);
    kill(c, SIGSTOP);
    chld_event("SIGSTOP for waitid", c);
    wait_info("P_PID WSTOPPED|WNOWAIT", P_PID, c, WSTOPPED | WNOWAIT);
    wait_info("P_PID WSTOPPED", P_PID, c, WSTOPPED);
    wait_info("P_PID WSTOPPED|WNOHANG, already reported", P_PID, c, WSTOPPED | WNOHANG);
    kill(c, SIGCONT);
    chld_event("SIGCONT for waitid", c);
    wait_info("P_PID WCONTINUED", P_PID, c, WCONTINUED);
    kill(c, SIGTERM);
    chld_event("SIGTERM for waitid", c);
    wait_info("P_PGID WEXITED (the group of the caller: 0)", P_PGID, 0, WEXITED);
    wait_info("P_ALL, nothing left", P_ALL, 0, WEXITED);
    wait_info("with no event wanted", P_ALL, 0, WNOHANG);
    wait_info("a bad id type", (idtype_t)99, 0, WEXITED);
    wait_info("P_PID 0", P_PID, 0, WEXITED);
    sigprocmask(SIG_UNBLOCK, &chld_set, NULL);

    /* 6. kill(0, sig): everybody in the caller's group, the caller included; kill(-pgid, sig): a group of its own */
    install(SIGUSR1, 0);
    expect_sender = g_pid;
    hits = 0;
    int rep[2];
    pipe(rep);
    pid_t kids[2];
    for (int i = 0; i < 2; i++) {
        kids[i] = fork();
        if (kids[i] == 0) {
            hits = 0;
            write(rep[1], "r", 1);                      /* ready */
            while (!hits) pause();
            struct seen k = last;
            write(rep[1], &k, sizeof k);
            _exit(0);
        }
    }
    read(rep[0], &b, 1);
    read(rep[0], &b, 1);
    printf("kill(0, SIGUSR1): %d\n", kill(0, SIGUSR1));
    show("the sender itself", SI_USER);
    for (int i = 0; i < 2; i++) {
        read(rep[0], &s, sizeof s);
        printf("a child of the group: signo %d, code %d, sender is the parent %d\n", s.signo, s.code, s.pid_is_sender);
        waitpid(-1, NULL, 0);
    }
    for (int i = 0; i < 2; i++) kids[i] = spawn_sleeper();
    setpgid(kids[0], kids[0]);
    setpgid(kids[1], kids[0]);
    nap(100);
    printf("kill(-group, 0): %d\n", kill(-kids[0], 0));
    printf("kill(-group, SIGTERM): %d\n", kill(-kids[0], SIGTERM));
    for (int i = 0; i < 2; i++) {
        pid_t d = waitpid(-1, &status, 0);
        printf("a member of the other group: ended %d, by signal %d\n", d > 0, WIFSIGNALED(status) ? WTERMSIG(status) : -1);
    }
    errno = 0;
    int r = kill(-kids[0], 0);
    printf("kill of an empty group: %d, errno %d (ESRCH %d)\n", r, errno, errno == ESRCH);
    return 0;
}
