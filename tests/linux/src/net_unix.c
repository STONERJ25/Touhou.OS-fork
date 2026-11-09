/*
 * UNIX-domain stream sockets through a statically linked C library: socketpair and its flags, SO_PEERCRED, names (a path in /tmp, a name in the abstract
 * namespace), getsockname and getpeername lengths, bind and connect errors, accept4, shutdown and end of file, EPIPE, MSG_PEEK, MSG_DONTWAIT, a large transfer
 * between two threads (the queue fills and drains), and the poll bits. Prints only what unix(7) and the other manual pages promise, so a real Linux prints the
 * same lines (tools/linux-oracle.py); tests/case_linux_net.py compares.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define PATH_A "/tmp/net_unix_test.sock"
#define PATH_B "/tmp/net_unix_client.sock"

static int cloexec(int fd) { return (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0; }
static int nonblocking(int fd) { return (fcntl(fd, F_GETFL) & O_NONBLOCK) != 0; }

static socklen_t make_addr(struct sockaddr_un *sa, const char *path) {
    memset(sa, 0, sizeof *sa);
    sa->sun_family = AF_UNIX;
    strcpy(sa->sun_path, path);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1);
}

/* the abstract name "\0net-unix-abstract": its length is part of the address */
static socklen_t make_abstract(struct sockaddr_un *sa) {
    memset(sa, 0, sizeof *sa);
    sa->sun_family = AF_UNIX;
    memcpy(sa->sun_path, "\0net-unix-abstract", 18);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 18);
}

static void show_name(const char *what, int fd, int peer) {
    struct sockaddr_un sa;
    memset(&sa, 0x55, sizeof sa);
    socklen_t l = sizeof sa;
    int r = peer ? getpeername(fd, (struct sockaddr *)&sa, &l) : getsockname(fd, (struct sockaddr *)&sa, &l);
    printf("%s: %d, family %d, length %u", what, r, r == 0 ? sa.sun_family : -1, r == 0 ? l : 0);
    if (r == 0 && l > 2) {
        if (sa.sun_path[0]) printf(", path %s", sa.sun_path);
        else printf(", abstract name %.*s", (int)(l - 3), sa.sun_path + 1);
    }
    printf("\n");
}

static void wait_readable(int fd) {
    struct pollfd p = { fd, POLLIN, 0 };
    poll(&p, 1, 10000);
}

static unsigned checksum(const unsigned char *p, size_t n) {
    unsigned a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

#define BIG 300000
static unsigned char big_out[BIG], big_in[BIG];

static void *writer(void *arg) {
    int fd = (int)(long)arg;
    size_t sent = 0;
    while (sent < BIG) {
        ssize_t w = write(fd, big_out + sent, BIG - sent > 7000 ? 7000 : BIG - sent);
        if (w <= 0) break;
        sent += (size_t)w;
    }
    close(fd);
    return NULL;
}

static volatile int sigpipes;
static void on_sigpipe(int sig) { (void)sig; sigpipes++; }

int main(void) {
    struct sigaction sig;
    memset(&sig, 0, sizeof sig);
    sig.sa_handler = on_sigpipe;
    sigaction(SIGPIPE, &sig, NULL);

    /* socketpair */
    int sv[2];
    printf("socketpair: %d\n", socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    char buf[64];
    write(sv[0], "ping", 4);
    memset(buf, 0, sizeof buf);
    ssize_t r = read(sv[1], buf, sizeof buf);
    printf("0 -> 1: %zd '%s'", r, buf);
    write(sv[1], "pong!", 5);
    memset(buf, 0, sizeof buf);
    r = read(sv[0], buf, sizeof buf);
    printf(", 1 -> 0: %zd '%s'\n", r, buf);
    show_name("getsockname of a socketpair end", sv[0], 0);
    show_name("getpeername of a socketpair end", sv[0], 1);
    struct ucred uc;
    socklen_t ul = sizeof uc;
    int pc = getsockopt(sv[0], SOL_SOCKET, SO_PEERCRED, &uc, &ul);
    printf("SO_PEERCRED: %d, length %u, pid is ours %d, uid is ours %d, gid is ours %d\n", pc, ul, uc.pid == getpid(), uc.uid == getuid(), uc.gid == getgid());
    int type = 0;
    socklen_t tl = sizeof type;
    getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &type, &tl);
    printf("SO_TYPE %d, SO_ERROR ", type);
    int err = -1;
    tl = sizeof err;
    getsockopt(sv[0], SOL_SOCKET, SO_ERROR, &err, &tl);
    printf("%d\n", err);

    /* MSG_PEEK, MSG_WAITALL, MSG_DONTWAIT */
    send(sv[0], "abcdef", 6, 0);
    wait_readable(sv[1]);
    memset(buf, 0, sizeof buf);
    r = recv(sv[1], buf, 4, MSG_PEEK);
    printf("MSG_PEEK: %zd '%s'", r, buf);
    memset(buf, 0, sizeof buf);
    r = recv(sv[1], buf, 6, MSG_WAITALL);
    printf(", MSG_WAITALL: %zd '%s'", r, buf);
    errno = 0;
    r = recv(sv[1], buf, sizeof buf, MSG_DONTWAIT);
    printf(", MSG_DONTWAIT on an empty queue: %zd errno %d (EAGAIN %d)\n", r, errno, EAGAIN);

    /* shutdown and end of file */
    send(sv[0], "last", 4, 0);
    printf("shutdown(SHUT_WR) on end 0: %d\n", shutdown(sv[0], SHUT_WR));
    memset(buf, 0, sizeof buf);
    r = read(sv[1], buf, sizeof buf);
    printf("end 1 still reads what was sent: %zd '%s'", r, buf);
    r = read(sv[1], buf, sizeof buf);
    printf(", then end of file: %zd\n", r);
    errno = 0;
    r = send(sv[0], "more", 4, MSG_NOSIGNAL);
    printf("end 0 sending after its own shutdown: %zd errno %d (EPIPE %d)\n", r, errno, EPIPE);
    write(sv[1], "still open the other way", 24);
    memset(buf, 0, sizeof buf);
    r = read(sv[0], buf, sizeof buf);
    printf("the other direction still works: %zd\n", r);
    struct pollfd pe = { sv[1], POLLIN | POLLOUT | POLLRDHUP, 0 };
    poll(&pe, 1, 1000);
    printf("poll on end 1 after end 0 shut down its sending: POLLIN %d, POLLOUT %d, POLLRDHUP %d, POLLHUP %d\n", (pe.revents & POLLIN) != 0, (pe.revents & POLLOUT) != 0,
           (pe.revents & POLLRDHUP) != 0, (pe.revents & POLLHUP) != 0);

    /* closing one end */
    close(sv[0]);
    r = read(sv[1], buf, sizeof buf);
    printf("after end 0 closed: read gives %zd", r);
    errno = 0;
    r = send(sv[1], "x", 1, MSG_NOSIGNAL);
    printf(", MSG_NOSIGNAL send gives %zd errno %d (EPIPE %d), SIGPIPE count %d", r, errno, EPIPE, sigpipes);
    errno = 0;
    r = write(sv[1], "x", 1);
    printf("; write gives %zd errno %d, SIGPIPE count %d\n", r, errno, sigpipes);
    close(sv[1]);

    /* flags in the type */
    int fl[2];
    socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fl);
    printf("socketpair with flags: non-blocking %d %d, close-on-exec %d %d", nonblocking(fl[0]), nonblocking(fl[1]), cloexec(fl[0]), cloexec(fl[1]));
    errno = 0;
    r = read(fl[0], buf, sizeof buf);
    printf(", an empty non-blocking read: %zd errno %d\n", r, errno);
    close(fl[0]);
    close(fl[1]);

    /* a named socket */
    unlink(PATH_A);
    unlink(PATH_B);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    socklen_t sl = make_addr(&sa, PATH_A);
    printf("bind %s: %d\n", PATH_A, bind(srv, (struct sockaddr *)&sa, sl));
    show_name("getsockname of the bound socket", srv, 0);
    errno = 0;
    show_name("getpeername of a socket that is not connected", srv, 1);
    printf("listen: %d\n", listen(srv, 4));
    int s2 = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    int rc = bind(s2, (struct sockaddr *)&sa, sl);
    printf("bind to the same path: %d errno %d (EADDRINUSE %d)\n", rc, errno, EADDRINUSE);
    close(s2);
    int acc = 0;
    tl = sizeof acc;
    getsockopt(srv, SOL_SOCKET, SO_ACCEPTCONN, &acc, &tl);
    printf("SO_ACCEPTCONN: %d\n", acc);

    int cli = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un mine;
    socklen_t ml = make_addr(&mine, PATH_B);
    printf("the client binds %s: %d\n", PATH_B, bind(cli, (struct sockaddr *)&mine, ml));
    printf("connect: %d\n", connect(cli, (struct sockaddr *)&sa, sl));
    errno = 0;
    rc = connect(cli, (struct sockaddr *)&sa, sl);
    printf("connect again: %d errno %d (EISCONN %d)\n", rc, errno, EISCONN);
    struct sockaddr_un from;
    memset(&from, 0, sizeof from);
    socklen_t fl2 = sizeof from;
    int conn = accept4(srv, (struct sockaddr *)&from, &fl2, SOCK_NONBLOCK | SOCK_CLOEXEC);
    printf("accept4: ok %d, non-blocking %d, close-on-exec %d, the client's name is %s (length %u)\n", conn >= 0, nonblocking(conn), cloexec(conn), from.sun_path, fl2);
    show_name("accepted socket, getsockname", conn, 0);
    show_name("accepted socket, getpeername", conn, 1);
    show_name("client, getsockname", cli, 0);
    show_name("client, getpeername", cli, 1);
    struct ucred cu;
    ul = sizeof cu;
    getsockopt(conn, SOL_SOCKET, SO_PEERCRED, &cu, &ul);
    printf("SO_PEERCRED on the accepted socket: pid is ours %d, uid is ours %d\n", cu.pid == getpid(), cu.uid == getuid());
    struct pollfd pcn = { conn, POLLIN | POLLOUT, 0 };
    poll(&pcn, 1, 1000);
    printf("accepted socket, nothing sent yet: POLLIN %d, POLLOUT %d\n", (pcn.revents & POLLIN) != 0, (pcn.revents & POLLOUT) != 0);
    write(cli, "hello server", 12);
    wait_readable(conn);
    memset(buf, 0, sizeof buf);
    r = read(conn, buf, sizeof buf);
    printf("the server read %zd '%s'\n", r, buf);
    fcntl(srv, F_SETFL, fcntl(srv, F_GETFL) | O_NONBLOCK);
    errno = 0;
    int none = accept(srv, NULL, NULL);
    printf("accept on a non-blocking listener with nobody waiting: %d errno %d (EAGAIN %d)\n", none, errno, EAGAIN);
    close(conn);
    close(cli);

    /* the errors of connect */
    close(srv);
    int late = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    rc = connect(late, (struct sockaddr *)&sa, sl);
    printf("connect to a socket file nobody listens on: %d errno %d (ECONNREFUSED %d)\n", rc, errno, ECONNREFUSED);
    unlink(PATH_A);
    errno = 0;
    rc = connect(late, (struct sockaddr *)&sa, sl);
    printf("connect to a path that does not exist: %d errno %d (ENOENT %d)\n", rc, errno, ENOENT);
    close(late);
    unlink(PATH_B);

    /* the abstract namespace */
    int as = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un ab;
    socklen_t al = make_abstract(&ab);
    printf("bind to an abstract name: %d\n", bind(as, (struct sockaddr *)&ab, al));
    show_name("getsockname of it", as, 0);
    listen(as, 2);
    int as2 = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    rc = bind(as2, (struct sockaddr *)&ab, al);
    printf("bind the same abstract name again: %d errno %d (EADDRINUSE %d)\n", rc, errno, EADDRINUSE);
    close(as2);
    int ac = socket(AF_UNIX, SOCK_STREAM, 0);
    printf("connect to it: %d\n", connect(ac, (struct sockaddr *)&ab, al));
    int aconn = accept(as, NULL, NULL);
    write(ac, "abstract", 8);
    wait_readable(aconn);
    memset(buf, 0, sizeof buf);
    r = read(aconn, buf, sizeof buf);
    printf("through the abstract name: %zd '%s'\n", r, buf);
    close(aconn);
    close(ac);
    close(as);
    ac = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    rc = connect(ac, (struct sockaddr *)&ab, al);
    printf("after the listener closed, the name is gone: %d errno %d (ECONNREFUSED %d)\n", rc, errno, ECONNREFUSED);
    close(ac);

    /* errors of socket() and bind() arguments */
    errno = 0;
    rc = socket(AF_UNIX, SOCK_STREAM, 2);
    printf("socket with a protocol other than 0: %d errno %d (EPROTONOSUPPORT %d)\n", rc, errno, EPROTONOSUPPORT);
    errno = 0;
    rc = socket(AF_UNIX, SOCK_STREAM | 0x7000, 0);
    printf("socket with stray type bits: %d errno %d (EINVAL %d)\n", rc, errno, EINVAL);
    errno = 0;
    rc = socket(AF_INET6, SOCK_STREAM, 0);
    printf("socket(AF_INET6) is either available or EAFNOSUPPORT: %d\n", rc >= 0 || errno == EAFNOSUPPORT);
    if (rc >= 0) close(rc);
    errno = 0;
    int nl = socket(AF_NETLINK, SOCK_DGRAM, 0);
    printf("socket(AF_NETLINK) is either available or EAFNOSUPPORT/EPROTONOSUPPORT: %d\n", nl >= 0 || errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT);
    if (nl >= 0) close(nl);

    /* a big transfer: a writer thread fills the queue faster than the reader drains it */
    for (size_t i = 0; i < BIG; i++) big_out[i] = (unsigned char)(i * 13 + (i >> 7));
    int bp[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, bp);
    pthread_t th;
    pthread_create(&th, NULL, writer, (void *)(long)bp[0]);
    size_t got = 0;
    while (got < BIG) {
        r = read(bp[1], big_in + got, BIG - got);
        if (r <= 0) break;
        got += (size_t)r;
        if (got < 50000) usleep(2000);                      /* slow at first: the writer blocks on a full queue */
    }
    pthread_join(th, NULL);
    printf("large transfer: %zu bytes arrived, checksum equal %d, then end of file %zd\n", got, checksum(big_out, BIG) == checksum(big_in, got == BIG ? BIG : 0),
           read(bp[1], buf, sizeof buf));
    close(bp[1]);
    return 0;
}
