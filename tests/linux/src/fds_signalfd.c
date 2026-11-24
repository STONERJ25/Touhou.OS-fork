/* signalfd (docs/COMPAT.md, "Descriptors, FIFOs, signalfd and pidfd"): blocked signals read as struct signalfd_siginfo records. EAGAIN with nothing pending, EINVAL
 * for a short buffer and bad arguments, raise/kill/sigqueue from this process and from a child, SIGCHLD with the child's status, several records in one read, a
 * replaced mask, poll and epoll readiness, SFD_CLOEXEC. Written from signalfd(2), sigqueue(3) and signal(7); the program prints only what those pages promise. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

static void fact(const char *what, long v) { printf("%s: %ld\n", what, v); fflush(stdout); }
static const char *ename(int e) { return e == EAGAIN ? "EAGAIN" : e == EINVAL ? "EINVAL" : e == EBADF ? "EBADF" : e == 0 ? "none" : "other"; }
static void err(const char *what, long v) { printf("%s: %ld (%s)\n", what, v, v < 0 ? ename(errno) : "none"); fflush(stdout); errno = 0; }

static int cmp(const void *a, const void *b) {
    const struct signalfd_siginfo *x = a, *y = b;
    if (x->ssi_signo != y->ssi_signo) return x->ssi_signo < y->ssi_signo ? -1 : 1;
    return x->ssi_int < y->ssi_int ? -1 : x->ssi_int > y->ssi_int;
}

int main(void) {
    sigset_t block, m;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1); sigaddset(&block, SIGUSR2); sigaddset(&block, SIGRTMIN + 1); sigaddset(&block, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block, NULL);

    sigemptyset(&m); sigaddset(&m, SIGUSR1);
    int sfd = signalfd(-1, &m, SFD_NONBLOCK | SFD_CLOEXEC);
    fact("signalfd made", sfd >= 0);
    fact("close-on-exec", (fcntl(sfd, F_GETFD) & FD_CLOEXEC) != 0);
    fact("non-blocking", (fcntl(sfd, F_GETFL) & O_NONBLOCK) != 0);
    struct signalfd_siginfo si[4];
    err("read, nothing pending", read(sfd, si, sizeof si));
    err("read, 64-byte buffer", read(sfd, si, 64));
    struct pollfd pf = { .fd = sfd, .events = POLLIN };
    fact("poll, nothing pending", poll(&pf, 1, 0));

    raise(SIGUSR1);
    fact("poll after raise", poll(&pf, 1, 0));
    fact("poll revents POLLIN", (pf.revents & POLLIN) != 0);
    fact("read after raise", read(sfd, si, sizeof si));
    fact("ssi_signo is SIGUSR1", si[0].ssi_signo == SIGUSR1);
    fact("ssi_code (SI_TKILL)", si[0].ssi_code);
    fact("ssi_pid is ours", si[0].ssi_pid == (unsigned)getpid());
    fact("ssi_uid is ours", si[0].ssi_uid == getuid());
    sigset_t pend;
    sigpending(&pend);
    fact("SIGUSR1 still pending", sigismember(&pend, SIGUSR1));

    /* a second call with the descriptor replaces the set */
    sigaddset(&m, SIGUSR2); sigaddset(&m, SIGRTMIN + 1); sigaddset(&m, SIGKILL);
    fact("signalfd(sfd, ...) returns sfd", signalfd(sfd, &m, 0) == sfd);
    for (int i = 3; i >= 1; i--) { union sigval v = { .sival_int = 40 + i }; sigqueue(getpid(), SIGRTMIN + 1, v); }
    kill(getpid(), SIGUSR2);
    long n = read(sfd, si, sizeof si);
    fact("one read, four records (bytes)", n);
    qsort(si, (size_t)n / sizeof si[0], sizeof si[0], cmp);
    for (int i = 0; i < n / (long)sizeof si[0]; i++)
        printf("record: signal %s, code %d, int %d, from us %d\n", si[i].ssi_signo == SIGUSR2 ? "SIGUSR2" : si[i].ssi_signo == (unsigned)SIGRTMIN + 1 ? "SIGRTMIN+1" : "other",
               si[i].ssi_code, si[i].ssi_signo == SIGUSR2 ? 0 : si[i].ssi_int, si[i].ssi_pid == (unsigned)getpid());
    sigpending(&pend);
    fact("nothing of the set pending", sigismember(&pend, SIGUSR2) + sigismember(&pend, SIGRTMIN + 1));

    /* kill from a child, and the child's SIGCHLD on a second signalfd; the reads block */
    sigset_t c; sigemptyset(&c); sigaddset(&c, SIGCHLD);
    int cfd = signalfd(-1, &c, 0);
    fcntl(sfd, F_SETFL, 0);
    pid_t parent = getpid(), kid = fork();
    if (kid == 0) { kill(parent, SIGUSR1); _exit(7); }
    fact("blocking read: bytes", read(sfd, si, sizeof si[0]));
    fact("from the child: signal is SIGUSR1", si[0].ssi_signo == SIGUSR1);
    fact("from the child: code (SI_USER)", si[0].ssi_code);
    fact("from the child: pid is the child", si[0].ssi_pid == (unsigned)kid);
    fact("SIGCHLD read: bytes", read(cfd, si, sizeof si[0]));
    fact("SIGCHLD: signal", si[0].ssi_signo == SIGCHLD);
    fact("SIGCHLD: code (CLD_EXITED)", si[0].ssi_code);
    fact("SIGCHLD: status", si[0].ssi_status);
    fact("SIGCHLD: pid is the child", si[0].ssi_pid == (unsigned)kid);
    int st;
    fact("waitpid", waitpid(kid, &st, 0) == kid && WEXITSTATUS(st) == 7);

    /* epoll */
    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.u32 = 5 }, got;
    epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &ev);
    fact("epoll, nothing pending", epoll_wait(ep, &got, 1, 0));
    raise(SIGUSR2);
    fact("epoll after raise", epoll_wait(ep, &got, 1, 2000));
    fact("epoll data", got.data.u32);
    read(sfd, si, sizeof si);
    fact("epoll after the read", epoll_wait(ep, &got, 1, 0));

    /* bad arguments */
    sigemptyset(&m); sigaddset(&m, SIGUSR1);
    err("bad flags", signalfd(-1, &m, 0x10));
    int p[2];
    pipe(p);
    err("a pipe is not a signalfd", signalfd(p[0], &m, 0));
    err("a closed descriptor", signalfd(900, &m, 0));
    err("sizemask 4", syscall(289, -1, &m, 4, 0));
    sigemptyset(&m); sigaddset(&m, SIGKILL); sigaddset(&m, SIGSTOP);
    int kfd = signalfd(-1, &m, SFD_NONBLOCK);
    fact("SIGKILL and SIGSTOP accepted (ignored)", kfd >= 0);
    err("and never read", read(kfd, si, sizeof si));
    return 0;
}
