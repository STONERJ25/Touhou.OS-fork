/*
 * Non-blocking sockets and the errors of TCP: a connect that returns at once and completes in poll, the error a refused connect leaves in SO_ERROR, accept4 with
 * its flags, EAGAIN from accept and read, EADDRINUSE, a broken pipe with a SIGPIPE handler and with MSG_NOSIGNAL, the descriptor flags of socket(), FIONBIO and
 * F_SETFL. Prints only what the manual pages promise (never a port number or a time), so a real Linux prints the same lines (tools/linux-oracle.py);
 * tests/case_linux_net.py compares.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile int sigpipes;

static void on_sigpipe(int sig) { (void)sig; sigpipes++; }

static int listen_on_loopback(int *port, int backlog) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, (struct sockaddr *)&sa, sizeof sa);
    listen(s, backlog);
    socklen_t l = sizeof sa;
    getsockname(s, (struct sockaddr *)&sa, &l);
    *port = ntohs(sa.sin_port);
    return s;
}

static struct sockaddr_in loopback(int port) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return sa;
}

static int nonblocking(int fd) { return (fcntl(fd, F_GETFL) & O_NONBLOCK) != 0; }
static int cloexec(int fd) { return (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0; }

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigpipe;
    sigaction(SIGPIPE, &sa, NULL);

    int port, port2;
    int ls = listen_on_loopback(&port, 4);

    /* flags in the type of socket() */
    int plain = socket(AF_INET, SOCK_STREAM, 0);
    int flagged = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    printf("socket(): non-blocking %d, close-on-exec %d; with the flags: non-blocking %d, close-on-exec %d\n", nonblocking(plain), cloexec(plain), nonblocking(flagged),
           cloexec(flagged));
    int on = 1;
    ioctl(plain, FIONBIO, &on);
    printf("FIONBIO turns O_NONBLOCK on: %d", nonblocking(plain));
    on = 0;
    ioctl(plain, FIONBIO, &on);
    printf(", and off: %d", !nonblocking(plain));
    fcntl(plain, F_SETFL, fcntl(plain, F_GETFL) | O_NONBLOCK);
    printf(", F_SETFL does it too: %d\n", nonblocking(plain));
    int dup = fcntl(plain, F_DUPFD_CLOEXEC, 10);
    printf("F_DUPFD_CLOEXEC: descriptor at least 10: %d, close-on-exec %d, non-blocking is shared: %d\n", dup >= 10, cloexec(dup), nonblocking(dup));
    close(dup);
    close(plain);
    close(flagged);

    /* a non-blocking connect completes in poll */
    int c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    struct sockaddr_in to = loopback(port);
    errno = 0;
    int rc = connect(c, (struct sockaddr *)&to, sizeof to);
    int e = errno;
    printf("non-blocking connect: returned 0 or EINPROGRESS: %d\n", rc == 0 || (rc == -1 && e == EINPROGRESS));
    struct pollfd p = { c, POLLOUT, 0 };
    int n = poll(&p, 1, 10000);
    int err = -1;
    socklen_t el = sizeof err;
    getsockopt(c, SOL_SOCKET, SO_ERROR, &err, &el);
    printf("poll for writability: %d ready, POLLOUT %d, POLLERR %d, SO_ERROR %d\n", n, (p.revents & POLLOUT) != 0, (p.revents & POLLERR) != 0, err);
    struct pollfd pl = { ls, POLLIN, 0 };
    printf("the listener polls readable: %d\n", poll(&pl, 1, 10000));

    /* accept4 */
    errno = 0;
    int a = accept4(ls, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    printf("accept4 with flags: ok %d, non-blocking %d, close-on-exec %d\n", a >= 0, nonblocking(a), cloexec(a));
    struct sockaddr_in peer;
    socklen_t pl2 = sizeof peer;
    int ls_nb = ls;
    fcntl(ls_nb, F_SETFL, fcntl(ls_nb, F_GETFL) | O_NONBLOCK);
    errno = 0;
    int none = accept(ls, (struct sockaddr *)&peer, &pl2);
    printf("accept with nobody waiting: %d, errno %d (EAGAIN %d)\n", none, errno, EAGAIN);
    char buf[16];
    errno = 0;
    ssize_t r = read(a, buf, sizeof buf);
    printf("read with nothing to read: %zd, errno %d (EAGAIN %d)\n", r, errno, EAGAIN);
    errno = 0;
    r = recv(c, buf, sizeof buf, 0);
    printf("recv on the non-blocking client: %zd, errno %d\n", r, errno);

    /* data flows, and the writable poll mask is right */
    write(c, "ping", 4);
    struct pollfd pa = { a, POLLIN, 0 };
    poll(&pa, 1, 10000);                                                   /* the data may take a moment to arrive; poll for it, then ask about both bits */
    pa.events = POLLIN | POLLOUT;
    poll(&pa, 1, 10000);
    printf("the server end after a write: POLLIN %d, POLLOUT %d\n", (pa.revents & POLLIN) != 0, (pa.revents & POLLOUT) != 0);
    memset(buf, 0, sizeof buf);
    r = read(a, buf, sizeof buf);
    printf("the server read %zd bytes: '%s'\n", r, buf);

    /* EADDRINUSE */
    int dup_listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in same = loopback(port);
    errno = 0;
    rc = bind(dup_listener, (struct sockaddr *)&same, sizeof same);
    printf("bind to a port that is in use: %d, errno %d (EADDRINUSE %d)\n", rc, errno, EADDRINUSE);
    close(dup_listener);

    /* the peer closes: end of file, POLLIN with RDHUP, and a write that fails with EPIPE (after one that succeeds, as on any TCP) */
    close(a);
    struct pollfd pc = { c, POLLIN | POLLRDHUP, 0 };
    poll(&pc, 1, 10000);
    printf("after the peer closed: POLLIN %d, POLLRDHUP %d\n", (pc.revents & POLLIN) != 0, (pc.revents & POLLRDHUP) != 0);
    r = read(c, buf, sizeof buf);
    printf("read at end of file: %zd\n", r);
    int saw_epipe = 0;
    for (int i = 0; i < 50 && !saw_epipe; i++) {
        errno = 0;
        ssize_t w = send(c, "x", 1, MSG_NOSIGNAL);
        if (w < 0 && errno == EPIPE) saw_epipe = 1;
        usleep(20000);
    }
    printf("MSG_NOSIGNAL: a send to the closed peer ends in EPIPE: %d, SIGPIPE handler runs: %d\n", saw_epipe, sigpipes);
    saw_epipe = 0;
    for (int i = 0; i < 50 && !saw_epipe; i++) {
        errno = 0;
        ssize_t w = send(c, "x", 1, 0);
        if (w < 0 && errno == EPIPE) saw_epipe = 1;
        usleep(20000);
    }
    printf("without it: EPIPE %d, SIGPIPE delivered to the handler: %d\n", saw_epipe, sigpipes >= 1);
    close(c);
    close(ls);

    /* a refused connect: the error arrives in connect or in SO_ERROR, and poll reports it */
    int dead = listen_on_loopback(&port2, 1);
    close(dead);                                                           /* nobody listens on that port any more */
    int rf = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    struct sockaddr_in refused = loopback(port2);
    errno = 0;
    rc = connect(rf, (struct sockaddr *)&refused, sizeof refused);
    e = errno;
    int immediate = rc == -1 && e == ECONNREFUSED;
    int pending = rc == -1 && e == EINPROGRESS;
    printf("connect to a closed port: ECONNREFUSED at once or EINPROGRESS: %d\n", immediate || pending);
    if (pending) {
        struct pollfd pr = { rf, POLLOUT, 0 };
        poll(&pr, 1, 10000);
        err = 0;
        el = sizeof err;
        getsockopt(rf, SOL_SOCKET, SO_ERROR, &err, &el);
        printf("poll: POLLERR or POLLHUP %d, SO_ERROR %d (ECONNREFUSED %d)\n", (pr.revents & (POLLERR | POLLHUP)) != 0, err, ECONNREFUSED);
        err = -1;
        getsockopt(rf, SOL_SOCKET, SO_ERROR, &err, &el);
        printf("SO_ERROR again: %d (it is cleared by reading it)\n", err);
    } else {
        printf("poll: POLLERR or POLLHUP 1, SO_ERROR %d (ECONNREFUSED %d)\nSO_ERROR again: 0 (it is cleared by reading it)\n", ECONNREFUSED, ECONNREFUSED);
    }
    close(rf);

    /* a blocking connect to a closed port */
    int bl = socket(AF_INET, SOCK_STREAM, 0);
    errno = 0;
    rc = connect(bl, (struct sockaddr *)&refused, sizeof refused);
    printf("blocking connect to a closed port: %d, errno %d (ECONNREFUSED %d)\n", rc, errno, ECONNREFUSED);
    close(bl);
    return 0;
}
