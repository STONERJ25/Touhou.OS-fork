/*
 * poll, ppoll, select and pselect through a statically linked C library, on pipes, socket pairs and a listening socket: what each reports before and after
 * data arrives, POLLHUP and POLLNVAL, negative descriptors, timeouts (the time that passes, and the time left that select gives back), EINTR from a signal
 * handler, and the signal mask of ppoll and pselect. Prints only what the manual pages promise (never a time), so a real Linux prints the same lines
 * (tools/linux-oracle.py); tests/case_linux_net.py compares.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile int usr1, alarms;

static void on_usr1(int sig) { (void)sig; usr1++; }
static void on_alarm(int sig) { (void)sig; alarms++; }

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int has(short revents, short bit) { return (revents & bit) != 0; }

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);

    /* poll on a pipe */
    int p[2];
    pipe(p);
    struct pollfd pf[2] = { { p[0], POLLIN, 0 }, { p[1], POLLOUT, 0 } };
    int n = poll(pf, 2, 0);
    printf("empty pipe: %d ready; read end POLLIN %d; write end POLLOUT %d\n", n, has(pf[0].revents, POLLIN), has(pf[1].revents, POLLOUT));
    write(p[1], "abc", 3);
    n = poll(pf, 2, 0);
    printf("after a write: %d ready; read end POLLIN %d\n", n, has(pf[0].revents, POLLIN));
    char buf[16];
    read(p[0], buf, sizeof buf);
    pf[0].events = POLLIN;
    n = poll(pf, 1, 0);
    printf("drained: %d ready\n", n);
    close(p[1]);
    n = poll(pf, 1, 0);
    printf("the writer closed: %d ready; POLLHUP %d\n", n, has(pf[0].revents, POLLHUP));
    close(p[0]);

    /* POLLNVAL, negative descriptors, no descriptors */
    struct pollfd bad[3] = { { 999, POLLIN, 0 }, { -1, POLLIN, 0x7fff }, { 0, 0, 0 } };
    n = poll(bad, 2, 0);
    printf("a descriptor that is not open: %d ready; POLLNVAL %d; a negative descriptor gets revents %d\n", n, has(bad[0].revents, POLLNVAL), bad[1].revents);

    /* timeouts: the call lasts about as long as it was asked to */
    long t0 = now_ms();
    n = poll(NULL, 0, 150);
    long dt = now_ms() - t0;
    printf("poll(NULL, 0, 150): %d, took at least 150 ms: %d, under 10 s: %d\n", n, dt >= 145, dt < 10000);
    int q[2];
    pipe(q);
    struct pollfd pq = { q[0], POLLIN, 0 };
    t0 = now_ms();
    n = poll(&pq, 1, 120);
    dt = now_ms() - t0;
    printf("timeout on a quiet pipe: %d, took at least 120 ms: %d\n", n, dt >= 115);

    /* a descriptor that becomes ready while poll sleeps: a child writes after a while */
    pid_t c = fork();
    if (c == 0) {
        usleep(200000);
        write(q[1], "x", 1);
        _exit(0);
    }
    t0 = now_ms();
    n = poll(&pq, 1, 10000);
    dt = now_ms() - t0;
    printf("woken by a write from another process: %d, POLLIN %d, well before the timeout: %d\n", n, has(pq.revents, POLLIN), dt < 5000);
    waitpid(c, NULL, 0);

    /* a signal ends poll with EINTR */
    alarm(1);
    errno = 0;
    n = poll(NULL, 0, 10000);
    int e = errno;
    printf("SIGALRM during poll: %d errno %d (EINTR %d), the handler ran %d times\n", n, e, EINTR, alarms);

    /* select */
    fd_set rs, ws, es;
    FD_ZERO(&rs);
    FD_ZERO(&ws);
    FD_ZERO(&es);
    FD_SET(q[0], &rs);
    FD_SET(q[1], &ws);
    struct timeval tv = { 0, 0 };
    n = select((q[0] > q[1] ? q[0] : q[1]) + 1, &rs, &ws, &es, &tv);
    printf("select, data waiting (one byte) and room to write: %d ready; read %d write %d except %d\n", n, FD_ISSET(q[0], &rs) != 0, FD_ISSET(q[1], &ws) != 0,
           FD_ISSET(q[0], &es) != 0);
    read(q[0], buf, sizeof buf);
    FD_ZERO(&rs);
    FD_SET(q[0], &rs);
    tv.tv_sec = 0;
    tv.tv_usec = 300000;
    t0 = now_ms();
    n = select(q[0] + 1, &rs, NULL, NULL, &tv);
    dt = now_ms() - t0;
    printf("select on a quiet pipe with 300 ms: %d ready, set cleared %d, took at least 295 ms: %d, time left is under the original: %d\n", n, !FD_ISSET(q[0], &rs),
           dt >= 295, tv.tv_sec == 0 && tv.tv_usec < 300000);
    errno = 0;
    int closed = dup(q[0]);
    close(closed);
    FD_ZERO(&rs);
    FD_SET(closed, &rs);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    n = select(closed + 1, &rs, NULL, NULL, &tv);
    printf("select with a descriptor that is not open: %d errno %d (EBADF %d)\n", n, errno, EBADF);
    errno = 0;
    n = select(-1, NULL, NULL, NULL, &tv);
    printf("select with a negative nfds: %d errno %d (EINVAL %d)\n", n, errno, EINVAL);
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    t0 = now_ms();
    n = select(0, NULL, NULL, NULL, &tv);
    dt = now_ms() - t0;
    printf("select as a sleep: %d, took at least 95 ms: %d\n", n, dt >= 95);

    /* a socket pair and a listener */
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    struct pollfd ps = { sv[0], POLLIN | POLLOUT, 0 };
    n = poll(&ps, 1, 0);
    printf("socketpair, nothing sent: %d ready; POLLIN %d POLLOUT %d\n", n, has(ps.revents, POLLIN), has(ps.revents, POLLOUT));
    write(sv[1], "hi", 2);
    ps.events = POLLIN;
    n = poll(&ps, 1, 2000);
    printf("after the other end wrote: %d ready; POLLIN %d\n", n, has(ps.revents, POLLIN));
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(ls, (struct sockaddr *)&addr, sizeof addr);
    listen(ls, 4);
    socklen_t al = sizeof addr;
    getsockname(ls, (struct sockaddr *)&addr, &al);
    struct pollfd pl = { ls, POLLIN, 0 };
    n = poll(&pl, 1, 0);
    printf("listener with nobody connecting: %d ready\n", n);
    int cl = socket(AF_INET, SOCK_STREAM, 0);
    connect(cl, (struct sockaddr *)&addr, sizeof addr);
    n = poll(&pl, 1, 5000);
    printf("after a connect: %d ready; POLLIN %d\n", n, has(pl.revents, POLLIN));
    FD_ZERO(&rs);
    FD_SET(ls, &rs);
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    n = select(ls + 1, &rs, NULL, NULL, &tv);
    printf("select sees the pending connection too: %d ready, set %d\n", n, FD_ISSET(ls, &rs) != 0);

    /* ppoll and pselect with a signal mask: SIGUSR1 is blocked and pending, the mask given to the call lets it through */
    sigset_t block, empty, old;
    sigemptyset(&empty);
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block, &old);
    kill(getpid(), SIGUSR1);
    usr1 = 0;
    struct timespec ts = { 5, 0 };
    errno = 0;
    n = ppoll(NULL, 0, &ts, &empty);
    e = errno;
    printf("ppoll with an empty mask and SIGUSR1 pending: %d errno %d (EINTR %d), handler ran %d time\n", n, e, EINTR, usr1);
    sigset_t now;
    sigprocmask(SIG_BLOCK, NULL, &now);
    printf("afterwards SIGUSR1 is blocked again: %d\n", sigismember(&now, SIGUSR1) == 1);
    kill(getpid(), SIGUSR1);
    usr1 = 0;
    ts.tv_sec = 5;
    ts.tv_nsec = 0;
    errno = 0;
    n = pselect(0, NULL, NULL, NULL, &ts, &empty);
    e = errno;
    printf("pselect the same way: %d errno %d (EINTR %d), handler ran %d time\n", n, e, EINTR, usr1);
    sigprocmask(SIG_BLOCK, NULL, &now);
    printf("afterwards SIGUSR1 is blocked again: %d\n", sigismember(&now, SIGUSR1) == 1);
    sigprocmask(SIG_SETMASK, &old, NULL);
    sigemptyset(&old);
    sigprocmask(SIG_UNBLOCK, &block, NULL);                           /* the pending signal from before, if any, is delivered here */
    printf("ppoll with nothing pending and a short timeout: %d\n", ppoll(NULL, 0, &(struct timespec){ 0, 50000000 }, NULL));
    return 0;
}
