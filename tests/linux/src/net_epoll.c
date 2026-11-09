/*
 * epoll through a statically linked C library: the errors of epoll_create and epoll_ctl, level-triggered, edge-triggered and one-shot registrations on pipes
 * and sockets, the user data that comes back, EPOLLHUP and EPOLLRDHUP, a registration that goes away when its descriptor is closed (and one that stays while a
 * duplicate is open), nested epoll sets and their loop check, timeouts and signals in epoll_wait, epoll_pwait, and a loop that serves ten TCP clients with
 * edge-triggered reads. Prints only what the manual pages promise (never a time or a count of wake-ups), so a real Linux prints the same lines
 * (tools/linux-oracle.py); tests/case_linux_net.py compares.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static volatile int alarms;
static void on_alarm(int sig) { (void)sig; alarms++; }

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int add(int ep, int fd, unsigned events, unsigned long data) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = events;
    ev.data.u64 = data;
    return epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
}

static int mod(int ep, int fd, unsigned events, unsigned long data) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = events;
    ev.data.u64 = data;
    return epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev);
}

/* waits for one event at most and reports it: "n events" and the bits and data of the first */
static int wait1(int ep, int timeout_ms, unsigned *events, unsigned long *data) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    int n = epoll_wait(ep, &ev, 1, timeout_ms);
    if (n > 0) { *events = ev.events; *data = ev.data.u64; }
    else { *events = 0; *data = 0; }
    return n;
}

static void nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);

    /* creation and the errors of epoll_ctl */
    errno = 0;
    int bad = epoll_create(0);
    printf("epoll_create(0): %d errno %d (EINVAL %d)\n", bad, errno, EINVAL);
    errno = 0;
    bad = epoll_create1(0x40);
    printf("epoll_create1 with stray flags: %d errno %d\n", bad, errno);
    int ep = epoll_create1(EPOLL_CLOEXEC);
    printf("epoll_create1(EPOLL_CLOEXEC): ok %d, close-on-exec %d\n", ep >= 0, (fcntl(ep, F_GETFD) & FD_CLOEXEC) != 0);
    int old = epoll_create(8);
    printf("epoll_create(8): ok %d, close-on-exec %d\n", old >= 0, (fcntl(old, F_GETFD) & FD_CLOEXEC) != 0);
    close(old);
    int p[2];
    pipe(p);
    struct epoll_event ev = { EPOLLIN, { .u64 = 1 } };
    errno = 0;
    int rc = epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev);
    printf("add a pipe: %d\n", rc);
    errno = 0;
    rc = epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev);
    printf("add it again: %d errno %d (EEXIST %d)\n", rc, errno, EEXIST);
    errno = 0;
    rc = epoll_ctl(ep, EPOLL_CTL_MOD, p[1], &ev);
    printf("modify a descriptor that was never added: %d errno %d (ENOENT %d)\n", rc, errno, ENOENT);
    errno = 0;
    rc = epoll_ctl(ep, EPOLL_CTL_DEL, p[1], NULL);
    printf("delete one: %d errno %d\n", rc, errno);
    errno = 0;
    rc = epoll_ctl(ep, EPOLL_CTL_ADD, ep, &ev);
    printf("add the epoll descriptor to itself: %d errno %d (EINVAL %d)\n", rc, errno, EINVAL);
    errno = 0;
    rc = epoll_ctl(p[0], EPOLL_CTL_ADD, p[1], &ev);
    printf("epoll_ctl on a descriptor that is not an epoll set: %d errno %d\n", rc, errno);
    errno = 0;
    rc = epoll_ctl(ep, EPOLL_CTL_ADD, 999, &ev);
    printf("add a descriptor that is not open: %d errno %d (EBADF %d)\n", rc, errno, EBADF);
    errno = 0;
    rc = epoll_ctl(ep, 77, p[1], &ev);
    printf("an unknown operation: %d errno %d\n", rc, errno);
    errno = 0;
    rc = epoll_ctl(ep, EPOLL_CTL_ADD, p[1], NULL);
    printf("add with no event structure: %d errno %d (EFAULT %d)\n", rc, errno, EFAULT);
    int reg = open("/tmp/net_epoll_regular", O_CREAT | O_RDWR, 0600);
    errno = 0;
    rc = epoll_ctl(ep, EPOLL_CTL_ADD, reg, &ev);
    printf("add a regular file: %d errno %d (EPERM %d)\n", rc, errno, EPERM);
    close(reg);
    unlink("/tmp/net_epoll_regular");
    struct epoll_event out[4];
    errno = 0;
    rc = epoll_wait(ep, out, 0, 0);
    printf("epoll_wait with maxevents 0: %d errno %d\n", rc, errno);
    errno = 0;
    rc = epoll_wait(p[0], out, 4, 0);
    printf("epoll_wait on a pipe: %d errno %d\n", rc, errno);

    /* level-triggered: the event stays while the data does */
    unsigned events;
    unsigned long data;
    int n = wait1(ep, 0, &events, &data);
    printf("nothing written yet: %d events\n", n);
    write(p[1], "abc", 3);
    n = wait1(ep, 1000, &events, &data);
    printf("after a write: %d event, EPOLLIN %d, user data %lu\n", n, (events & EPOLLIN) != 0, data);
    n = wait1(ep, 0, &events, &data);
    printf("asked again, level-triggered: %d event\n", n);
    errno = 0;
    rc = epoll_wait(ep, NULL, 4, 0);
    printf("epoll_wait with no buffer while an event is waiting: %d errno %d (EFAULT %d)\n", rc, errno, EFAULT);
    char buf[16];
    read(p[0], buf, 1);
    n = wait1(ep, 0, &events, &data);
    printf("one byte read, two left: %d event\n", n);
    read(p[0], buf, sizeof buf);
    n = wait1(ep, 0, &events, &data);
    printf("all read: %d events\n", n);

    /* modify: other user data, other events; delete */
    mod(ep, p[0], EPOLLIN, 77);
    write(p[1], "x", 1);
    n = wait1(ep, 1000, &events, &data);
    printf("after EPOLL_CTL_MOD: user data %lu\n", data);
    read(p[0], buf, sizeof buf);
    printf("EPOLL_CTL_DEL: %d\n", epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL));
    write(p[1], "x", 1);
    n = wait1(ep, 50, &events, &data);
    printf("after the delete, data waiting: %d events\n", n);
    read(p[0], buf, sizeof buf);

    /* edge-triggered: one event per arrival, however much is left unread */
    add(ep, p[0], EPOLLIN | EPOLLET, 5);
    write(p[1], "12345", 5);
    n = wait1(ep, 1000, &events, &data);
    printf("edge-triggered after a write: %d event\n", n);
    n = wait1(ep, 50, &events, &data);
    printf("asked again with the data unread: %d events\n", n);
    write(p[1], "6", 1);
    n = wait1(ep, 1000, &events, &data);
    printf("another write: %d event\n", n);
    read(p[0], buf, sizeof buf);
    epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL);

    /* one-shot: reported once, until it is re-armed */
    add(ep, p[0], EPOLLIN | EPOLLONESHOT, 9);
    write(p[1], "a", 1);
    n = wait1(ep, 1000, &events, &data);
    printf("one-shot: %d event\n", n);
    write(p[1], "b", 1);
    n = wait1(ep, 50, &events, &data);
    printf("one-shot, more data: %d events\n", n);
    mod(ep, p[0], EPOLLIN | EPOLLONESHOT, 9);
    n = wait1(ep, 1000, &events, &data);
    printf("after EPOLL_CTL_MOD re-armed it: %d event\n", n);
    read(p[0], buf, sizeof buf);
    epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL);

    /* EPOLLOUT, and EPOLLERR / EPOLLHUP that need not be asked for */
    add(ep, p[1], EPOLLOUT, 2);
    n = wait1(ep, 1000, &events, &data);
    printf("write end of a pipe: %d event, EPOLLOUT %d\n", n, (events & EPOLLOUT) != 0);
    epoll_ctl(ep, EPOLL_CTL_DEL, p[1], NULL);
    add(ep, p[0], EPOLLIN, 3);
    close(p[1]);
    n = wait1(ep, 1000, &events, &data);
    printf("the writer closed: %d event, EPOLLHUP %d (not asked for)\n", n, (events & EPOLLHUP) != 0);
    epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL);
    close(p[0]);

    /* a socket pair: data, then the peer shuts down */
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    add(ep, sv[0], EPOLLIN | EPOLLRDHUP, 4);
    write(sv[1], "hello", 5);
    n = wait1(ep, 1000, &events, &data);
    printf("socketpair, data: EPOLLIN %d EPOLLRDHUP %d\n", (events & EPOLLIN) != 0, (events & EPOLLRDHUP) != 0);
    read(sv[0], buf, sizeof buf);
    shutdown(sv[1], SHUT_WR);
    n = wait1(ep, 1000, &events, &data);
    printf("the peer shut down writing: EPOLLIN %d EPOLLRDHUP %d EPOLLHUP %d\n", (events & EPOLLIN) != 0, (events & EPOLLRDHUP) != 0, (events & EPOLLHUP) != 0);
    epoll_ctl(ep, EPOLL_CTL_DEL, sv[0], NULL);
    close(sv[0]);
    close(sv[1]);

    /* closing the descriptor removes the registration; a duplicate keeps it alive */
    pipe(p);
    add(ep, p[0], EPOLLIN, 6);
    int dupfd = dup(p[0]);
    close(p[0]);
    write(p[1], "z", 1);
    n = wait1(ep, 1000, &events, &data);
    printf("the original closed, a duplicate still open: %d event\n", n);
    close(dupfd);
    n = wait1(ep, 50, &events, &data);
    printf("every descriptor of the file closed: %d events\n", n);
    close(p[1]);

    /* a TCP connection closed while registered */
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(ls, (struct sockaddr *)&addr, sizeof addr);
    listen(ls, 16);
    socklen_t al = sizeof addr;
    getsockname(ls, (struct sockaddr *)&addr, &al);
    int c1 = socket(AF_INET, SOCK_STREAM, 0);
    connect(c1, (struct sockaddr *)&addr, sizeof addr);
    int s1 = accept(ls, NULL, NULL);
    add(ep, s1, EPOLLIN | EPOLLRDHUP, 8);
    close(c1);
    n = wait1(ep, 5000, &events, &data);
    printf("TCP, the client closed: EPOLLIN %d EPOLLRDHUP %d\n", (events & EPOLLIN) != 0, (events & EPOLLRDHUP) != 0);
    close(s1);
    n = wait1(ep, 50, &events, &data);
    printf("TCP, the server closed its end: %d events\n", n);

    /* nested sets */
    int inner = epoll_create1(0), outer = epoll_create1(0);
    pipe(p);
    add(inner, p[0], EPOLLIN, 100);
    printf("an epoll set added to another: %d\n", add(outer, inner, EPOLLIN, 200));
    n = wait1(outer, 0, &events, &data);
    printf("nothing happened: %d events\n", n);
    write(p[1], "q", 1);
    n = wait1(outer, 1000, &events, &data);
    printf("data in the inner set's pipe: the outer set reports %d event, user data %lu\n", n, data);
    n = wait1(inner, 1000, &events, &data);
    printf("and the inner set: user data %lu\n", data);
    errno = 0;
    rc = add(inner, outer, EPOLLIN, 1);
    printf("adding the outer set to the inner one: %d errno %d (ELOOP %d)\n", rc, errno, ELOOP);
    close(inner);
    close(outer);
    close(p[0]);
    close(p[1]);

    /* timeouts and signals */
    long t0 = now_ms();
    n = wait1(ep, 150, &events, &data);
    long dt = now_ms() - t0;
    printf("epoll_wait with 150 ms and nothing to report: %d, took at least 150 ms: %d\n", n, dt >= 145);
    alarm(1);
    errno = 0;
    n = wait1(ep, 10000, &events, &data);
    int e = errno;
    printf("SIGALRM during epoll_wait: %d errno %d (EINTR %d), the handler ran %d time\n", n, e, EINTR, alarms);
    sigset_t block, empty;
    sigemptyset(&block);
    sigaddset(&block, SIGALRM);
    sigemptyset(&empty);
    sigprocmask(SIG_BLOCK, &block, NULL);
    alarm(1);
    struct epoll_event one;
    errno = 0;
    n = epoll_pwait(ep, &one, 1, 10000, &empty);
    e = errno;
    printf("epoll_pwait with SIGALRM unblocked for the call: %d errno %d, the handler ran %d times in all\n", n, e, alarms);
    sigset_t now;
    sigprocmask(SIG_BLOCK, NULL, &now);
    printf("afterwards SIGALRM is blocked again: %d\n", sigismember(&now, SIGALRM) == 1);
    sigprocmask(SIG_UNBLOCK, &block, NULL);
    struct epoll_event two;
    t0 = now_ms();
    n = epoll_pwait2(ep, &two, 1, &(struct timespec){ 0, 120000000 }, NULL);
    dt = now_ms() - t0;
    printf("epoll_pwait2 with a timespec of 120 ms and nothing to report: %d, took at least 115 ms: %d\n", n, dt >= 115);
    close(ep);

    /* ten clients served by one thread with edge-triggered reads */
    int ep2 = epoll_create1(0);
    int lsn = socket(AF_INET, SOCK_STREAM, 0);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(lsn, (struct sockaddr *)&addr, sizeof addr);
    listen(lsn, 16);
    al = sizeof addr;
    getsockname(lsn, (struct sockaddr *)&addr, &al);
    nonblock(lsn);
    add(ep2, lsn, EPOLLIN | EPOLLET, 0);
    int clients[10];
    char msg[10][64];
    for (int i = 0; i < 10; i++) {
        clients[i] = socket(AF_INET, SOCK_STREAM, 0);
        connect(clients[i], (struct sockaddr *)&addr, sizeof addr);
        snprintf(msg[i], sizeof msg[i], "client %d says hello, and again, and a third time", i);
        send(clients[i], msg[i], strlen(msg[i]), 0);
    }
    int accepted = 0, served = 0, replies_sent = 0, hup_seen = 0;
    size_t got[64] = { 0 };
    char acc[64][256];
    bool done[64] = { false };
    long deadline = now_ms() + 30000;
    while (served < 10 && now_ms() < deadline) {
        struct epoll_event evs[8];
        int k = epoll_wait(ep2, evs, 8, 1000);
        for (int i = 0; i < k; i++) {
            if (evs[i].data.u64 == 0) {
                for (;;) {
                    int fd = accept(lsn, NULL, NULL);
                    if (fd < 0) break;
                    nonblock(fd);
                    add(ep2, fd, EPOLLIN | EPOLLET | EPOLLRDHUP, (unsigned long)fd);
                    accepted++;
                }
            } else {
                int fd = (int)evs[i].data.u64;
                for (;;) {                                                                 /* edge-triggered: read until there is nothing more */
                    char scratch[256];
                    ssize_t r = done[fd] ? read(fd, scratch, sizeof scratch) : read(fd, acc[fd] + got[fd], sizeof acc[fd] - got[fd]);
                    if (r <= 0) break;
                    if (!done[fd]) got[fd] += (size_t)r;
                }
                if (!done[fd] && got[fd] == 48) {                                          /* the whole message has arrived: answer it in capitals */
                    char reply[64];
                    for (size_t j = 0; j < got[fd]; j++) reply[j] = (char)(acc[fd][j] >= 'a' && acc[fd][j] <= 'z' ? acc[fd][j] - 32 : acc[fd][j]);
                    write(fd, reply, got[fd]);
                    done[fd] = true;
                    served++;
                    replies_sent++;
                }
            }
        }
    }
    printf("served: %d accepted, %d messages answered\n", accepted, replies_sent);
    int ok = 0;
    for (int i = 0; i < 10; i++) {
        char r[300];
        size_t have = 0, want = strlen(msg[i]);
        while (have < want) {
            ssize_t k = read(clients[i], r + have, want - have);
            if (k <= 0) break;
            have += (size_t)k;
        }
        char expect[300];
        for (size_t j = 0; j < want; j++) expect[j] = (char)(msg[i][j] >= 'a' && msg[i][j] <= 'z' ? msg[i][j] - 32 : msg[i][j]);
        if (have == want && !memcmp(r, expect, want)) ok++;
    }
    printf("every client got its own message back, in capitals: %d of 10\n", ok);
    for (int i = 0; i < 10; i++) close(clients[i]);
    deadline = now_ms() + 30000;
    while (hup_seen < 10 && now_ms() < deadline) {
        struct epoll_event evs[8];
        int k = epoll_wait(ep2, evs, 8, 1000);
        for (int i = 0; i < k; i++)
            if (evs[i].data.u64 != 0 && (evs[i].events & (EPOLLRDHUP | EPOLLHUP))) {
                hup_seen++;
                close((int)evs[i].data.u64);                                              /* closing it removes it from the set */
            }
    }
    printf("all ten clients hung up and were noticed: %d\n", hup_seen);
    close(lsn);
    close(ep2);
    return 0;
}
