/*
 * UNIX-domain datagram sockets and descriptors passed in messages, through a statically linked C library: socketpair and bound datagram sockets (message
 * boundaries, truncation, the sender's name, connect and its errors), SCM_RIGHTS over streams and datagrams (a pipe, three files, a socket, a control buffer that is
 * too small, no control buffer at all, a descriptor that is not open, files that are still in flight when the receiver closes) and SCM_CREDENTIALS with
 * SO_PASSCRED. Prints only what unix(7) and cmsg(3) promise, so a real Linux prints the same lines (tools/linux-oracle.py); tests/case_linux_net.py compares.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SRV "/tmp/net_unix2_srv.sock"
#define CLI "/tmp/net_unix2_cli.sock"

static socklen_t make_addr(struct sockaddr_un *sa, const char *path) {
    memset(sa, 0, sizeof *sa);
    sa->sun_family = AF_UNIX;
    strcpy(sa->sun_path, path);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1);
}

static void wait_readable(int fd) {
    struct pollfd p = { fd, POLLIN, 0 };
    poll(&p, 1, 10000);
}

/* sends one byte and the descriptors fds[0..n) */
static ssize_t send_fds(int sock, const int *fds, int n, char byte) {
    char ctl[CMSG_SPACE(sizeof(int) * 8)];
    memset(ctl, 0, sizeof ctl);
    struct iovec iov = { &byte, 1 };
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctl;
    mh.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)n);
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)n);
    memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)n);
    return sendmsg(sock, &mh, 0);
}

/* receives a byte and up to `room` descriptors; *got is how many arrived, *flags the msg_flags */
static ssize_t recv_fds(int sock, int *fds, size_t room, int *got, int *flags, int recvflags) {
    char ctl[CMSG_SPACE(sizeof(int) * 8)];
    memset(ctl, 0, sizeof ctl);
    char byte = 0;
    struct iovec iov = { &byte, 1 };
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctl;
    mh.msg_controllen = room ? CMSG_LEN(sizeof(int) * room) : 0;                   /* exactly enough for `room` descriptors: no padding to spare */
    if (!room) mh.msg_control = NULL;
    ssize_t r = recvmsg(sock, &mh, recvflags);
    *got = 0;
    *flags = mh.msg_flags;
    for (struct cmsghdr *c = mh.msg_control ? CMSG_FIRSTHDR(&mh) : NULL; c; c = CMSG_NXTHDR(&mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int n = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            memcpy(fds + *got, CMSG_DATA(c), sizeof(int) * (size_t)n);
            *got += n;
        }
    return r;
}

int main(void) {
    char buf[256];
    unlink(SRV);
    unlink(CLI);

    /* a datagram socketpair: message boundaries */
    int sv[2];
    printf("socketpair(SOCK_DGRAM): %d\n", socketpair(AF_UNIX, SOCK_DGRAM, 0, sv));
    int type = 0;
    socklen_t tl = sizeof type;
    getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &type, &tl);
    printf("SO_TYPE %d\n", type);
    send(sv[0], "one", 3, 0);
    send(sv[0], "second", 6, 0);
    wait_readable(sv[1]);
    memset(buf, 0, sizeof buf);
    ssize_t r = recv(sv[1], buf, sizeof buf, 0);
    printf("first read: %zd '%s'", r, buf);
    memset(buf, 0, sizeof buf);
    r = recv(sv[1], buf, sizeof buf, 0);
    printf(", second read: %zd '%s'\n", r, buf);
    errno = 0;
    r = recv(sv[1], buf, sizeof buf, MSG_DONTWAIT);
    printf("nothing queued: %zd errno %d (EAGAIN %d)\n", r, errno, EAGAIN);
    struct pollfd p = { sv[1], POLLIN | POLLOUT, 0 };
    poll(&p, 1, 0);
    printf("poll on the idle end: POLLIN %d POLLOUT %d\n", (p.revents & POLLIN) != 0, (p.revents & POLLOUT) != 0);
    send(sv[0], "0123456789", 10, 0);
    p.events = POLLIN;
    poll(&p, 1, 5000);
    printf("after a send: POLLIN %d, FIONREAD-like peek length %zd\n", (p.revents & POLLIN) != 0, recv(sv[1], buf, 0, MSG_PEEK | MSG_TRUNC));
    memset(buf, 0, sizeof buf);
    r = recv(sv[1], buf, 4, MSG_PEEK);
    printf("peek 4: %zd '%s'", r, buf);
    memset(buf, 0, sizeof buf);
    r = recv(sv[1], buf, 4, 0);
    printf(", read 4: %zd '%s' (the rest of the datagram is lost)", r, buf);
    errno = 0;
    r = recv(sv[1], buf, sizeof buf, MSG_DONTWAIT);
    printf(", then %zd errno %d\n", r, errno);
    send(sv[0], "abcdefgh", 8, 0);
    wait_readable(sv[1]);
    struct iovec iov = { buf, 3 };
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    memset(buf, 0, sizeof buf);
    r = recvmsg(sv[1], &mh, 0);
    printf("recvmsg into 3 bytes: %zd '%s', MSG_TRUNC in msg_flags: %d\n", r, buf, (mh.msg_flags & MSG_TRUNC) != 0);
    struct sockaddr_un sa;
    socklen_t sl = sizeof sa;
    int g1 = getsockname(sv[0], (struct sockaddr *)&sa, &sl);
    printf("getsockname of a pair end: %d, length %u; ", g1, sl);
    sl = sizeof sa;
    int g2 = getpeername(sv[0], (struct sockaddr *)&sa, &sl);
    printf("getpeername: %d, length %u\n", g2, sl);
    char big[60000];
    memset(big, 'x', sizeof big);
    printf("a 60000-byte datagram: sent %zd", send(sv[0], big, sizeof big, 0));
    wait_readable(sv[1]);
    printf(", received %zd\n", recv(sv[1], big, sizeof big, 0));
    int sent = 0;
    errno = 0;
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    for (int i = 0; i < 100000; i++) {
        if (send(sv[0], "q", 1, 0) < 0) break;
        sent++;
    }
    printf("a full queue ends the non-blocking sends with EAGAIN after a few messages: %d, %d\n", sent >= 5, errno == EAGAIN);
    close(sv[0]);
    close(sv[1]);

    /* bound datagram sockets */
    int srv = socket(AF_UNIX, SOCK_DGRAM, 0), cli = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un sa_srv, sa_cli;
    socklen_t sl_srv = make_addr(&sa_srv, SRV), sl_cli = make_addr(&sa_cli, CLI);
    printf("bind the server: %d, the client: %d\n", bind(srv, (struct sockaddr *)&sa_srv, sl_srv), bind(cli, (struct sockaddr *)&sa_cli, sl_cli));
    printf("sendto the server: %zd\n", sendto(cli, "hello server", 12, 0, (struct sockaddr *)&sa_srv, sl_srv));
    wait_readable(srv);
    struct sockaddr_un from;
    memset(&from, 0, sizeof from);
    socklen_t fl = sizeof from;
    memset(buf, 0, sizeof buf);
    r = recvfrom(srv, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
    printf("recvfrom: %zd '%s', the sender is %s (address length %u)\n", r, buf, from.sun_path, fl);
    printf("reply to that address: %zd\n", sendto(srv, "hello client", 12, 0, (struct sockaddr *)&from, fl));
    wait_readable(cli);
    memset(buf, 0, sizeof buf);
    r = recv(cli, buf, sizeof buf, 0);
    printf("the client read: %zd '%s'\n", r, buf);
    int anon = socket(AF_UNIX, SOCK_DGRAM, 0);
    sendto(anon, "from nobody", 11, 0, (struct sockaddr *)&sa_srv, sl_srv);
    wait_readable(srv);
    memset(&from, 0x55, sizeof from);
    fl = sizeof from;
    r = recvfrom(srv, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
    printf("a sender without a name: %zd bytes, address length %u\n", r, fl);
    errno = 0;
    r = sendto(anon, "x", 1, 0, (struct sockaddr *)&(struct sockaddr_un){ AF_UNIX, "/tmp/net_unix2_nobody" }, sizeof(struct sockaddr_un));
    printf("sendto a path that does not exist: %zd errno %d (ENOENT %d)\n", r, errno, ENOENT);
    errno = 0;
    r = send(anon, "x", 1, 0);
    printf("send with no destination: %zd errno %d (ENOTCONN %d, EDESTADDRREQ %d)\n", r, errno, ENOTCONN, EDESTADDRREQ);

    /* connect on a datagram socket */
    printf("connect: %d\n", connect(anon, (struct sockaddr *)&sa_srv, sl_srv));
    printf("send: %zd\n", send(anon, "connected", 9, 0));
    wait_readable(srv);
    memset(buf, 0, sizeof buf);
    r = recv(srv, buf, sizeof buf, 0);
    printf("the server read: %zd '%s'\n", r, buf);
    sl = sizeof sa;
    memset(&sa, 0, sizeof sa);
    int g3 = getpeername(anon, (struct sockaddr *)&sa, &sl);
    printf("getpeername: %d, the server's path %s, length %u\n", g3, sa.sun_path, sl);
    struct sockaddr_un unspec;
    memset(&unspec, 0, sizeof unspec);
    unspec.sun_family = AF_UNSPEC;
    printf("connect(AF_UNSPEC) disconnects: %d\n", connect(anon, (struct sockaddr *)&unspec, sizeof unspec));
    errno = 0;
    r = send(anon, "x", 1, 0);
    printf("send after that: %zd errno %d\n", r, errno);
    connect(anon, (struct sockaddr *)&sa_srv, sl_srv);
    close(srv);
    errno = 0;
    r = send(anon, "x", 1, 0);
    printf("send to a destination that was closed: %zd errno %d (ECONNREFUSED %d)\n", r, errno, ECONNREFUSED);
    unlink(SRV);
    close(anon);
    close(cli);
    unlink(CLI);

    /* a stream listener is not a datagram destination */
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    make_addr(&sa_srv, SRV);
    bind(ls, (struct sockaddr *)&sa_srv, sl_srv);
    listen(ls, 2);
    int d = socket(AF_UNIX, SOCK_DGRAM, 0);
    errno = 0;
    int rc = connect(d, (struct sockaddr *)&sa_srv, sl_srv);
    printf("datagram connect to a stream socket: %d errno %d (EPROTOTYPE %d)\n", rc, errno, EPROTOTYPE);
    errno = 0;
    rc = listen(d, 1);
    printf("listen on a datagram socket: %d errno %d (EOPNOTSUPP %d)\n", rc, errno, EOPNOTSUPP);
    errno = 0;
    rc = accept(d, NULL, NULL);
    printf("accept on a datagram socket: %d errno %d\n", rc, errno);
    close(d);
    close(ls);
    unlink(SRV);

    /* an abstract datagram name */
    int as = socket(AF_UNIX, SOCK_DGRAM, 0), ac = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un ab;
    memset(&ab, 0, sizeof ab);
    ab.sun_family = AF_UNIX;
    memcpy(ab.sun_path, "\0net-unix2-abstract", 19);
    socklen_t al = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 19);
    printf("bind an abstract name: %d\n", bind(as, (struct sockaddr *)&ab, al));
    printf("sendto it: %zd\n", sendto(ac, "abstract dgram", 14, 0, (struct sockaddr *)&ab, al));
    wait_readable(as);
    memset(buf, 0, sizeof buf);
    printf("received: %zd '%s'\n", recv(as, buf, sizeof buf, 0), buf);
    close(as);
    close(ac);

    /* epoll on a datagram socket */
    int pair[2];
    socketpair(AF_UNIX, SOCK_DGRAM, 0, pair);
    int ep = epoll_create1(0);
    struct epoll_event ev = { EPOLLIN, { .u32 = 5 } };
    epoll_ctl(ep, EPOLL_CTL_ADD, pair[1], &ev);
    struct epoll_event out;
    printf("epoll, nothing sent: %d events\n", epoll_wait(ep, &out, 1, 0));
    send(pair[0], "x", 1, 0);
    int ne = epoll_wait(ep, &out, 1, 5000);
    printf("epoll, after a send: %d event, EPOLLIN %d\n", ne, (out.events & EPOLLIN) != 0);
    close(ep);
    close(pair[0]);
    close(pair[1]);

    /* SCM_RIGHTS over a stream: a pipe's read end */
    int st[2], pp[2], got, flags, fds[8];
    socketpair(AF_UNIX, SOCK_STREAM, 0, st);
    pipe(pp);
    printf("send the read end of a pipe: %zd\n", send_fds(st[0], &pp[0], 1, 'p'));
    wait_readable(st[1]);
    r = recv_fds(st[1], fds, 4, &got, &flags, 0);
    printf("received: %zd byte, %d descriptor, MSG_CTRUNC %d, it is a new number: %d\n", r, got, (flags & MSG_CTRUNC) != 0, got == 1 && fds[0] != pp[0]);
    write(pp[1], "through the pipe", 16);
    memset(buf, 0, sizeof buf);
    r = got == 1 ? read(fds[0], buf, sizeof buf) : -1;
    printf("reading through the received descriptor: %zd '%s'\n", r, buf);
    close(fds[0]);

    /* three files at once; MSG_CMSG_CLOEXEC */
    int files[3];
    const char *texts[3] = { "alpha", "beta", "gamma" };
    for (int i = 0; i < 3; i++) {
        char name[64];
        snprintf(name, sizeof name, "/tmp/net_unix2_f%d", i);
        files[i] = open(name, O_CREAT | O_RDWR | O_TRUNC, 0600);
        write(files[i], texts[i], strlen(texts[i]));
        unlink(name);
    }
    printf("send three files: %zd\n", send_fds(st[0], files, 3, 'f'));
    wait_readable(st[1]);
    r = recv_fds(st[1], fds, 4, &got, &flags, MSG_CMSG_CLOEXEC);
    printf("received %d descriptors, close-on-exec: ", got);
    for (int i = 0; i < got; i++) printf("%d", (fcntl(fds[i], F_GETFD) & FD_CLOEXEC) != 0);
    printf("; contents:");
    for (int i = 0; i < got; i++) {
        memset(buf, 0, sizeof buf);
        lseek(fds[i], 0, SEEK_SET);
        read(fds[i], buf, sizeof buf);
        printf(" %s", buf);
        close(fds[i]);
    }
    printf("\n");
    for (int i = 0; i < 3; i++) close(files[i]);

    /* a control buffer that is too small, and none at all */
    send_fds(st[0], files, 0, 'z');                                          /* nothing to pass: only to see the call works without files */
    wait_readable(st[1]);
    r = recv_fds(st[1], fds, 1, &got, &flags, 0);
    printf("a byte without files: %zd, %d descriptors\n", r, got);
    int two[2];
    pipe(two);
    send_fds(st[0], two, 2, 'm');
    wait_readable(st[1]);
    r = recv_fds(st[1], fds, 1, &got, &flags, 0);
    printf("two sent, room for one: %zd, %d descriptor, MSG_CTRUNC %d\n", r, got, (flags & MSG_CTRUNC) != 0);
    if (got == 1) close(fds[0]);
    send_fds(st[0], two, 2, 'n');
    wait_readable(st[1]);
    r = recv_fds(st[1], fds, 0, &got, &flags, 0);
    printf("two sent, no control buffer: %zd, %d descriptors, MSG_CTRUNC %d\n", r, got, (flags & MSG_CTRUNC) != 0);
    errno = 0;
    int badfds[1] = { 999 };
    r = send_fds(st[0], badfds, 1, 'b');
    printf("sending a descriptor that is not open: %zd errno %d (EBADF %d)\n", r, errno, EBADF);
    close(two[0]);
    close(two[1]);

    /* files in flight when the receiver closes are closed with it: the pipe sees end of file */
    int fl2[2], sock2[2];
    pipe(fl2);
    socketpair(AF_UNIX, SOCK_STREAM, 0, sock2);
    send_fds(sock2[0], &fl2[1], 1, 'w');
    close(fl2[1]);
    close(sock2[1]);                                                          /* the receiver never read it */
    struct pollfd pe = { fl2[0], POLLIN, 0 };
    int n = poll(&pe, 1, 5000);
    printf("the write end was in flight when the receiver closed: the pipe reports hang-up %d (%d ready)\n", (pe.revents & POLLHUP) != 0, n);
    close(fl2[0]);
    close(sock2[0]);

    /* a socket passed to another process-like holder: the receiver talks through it */
    int inner[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, inner);
    send_fds(st[0], &inner[1], 1, 's');
    close(inner[1]);
    wait_readable(st[1]);
    recv_fds(st[1], fds, 1, &got, &flags, 0);
    write(inner[0], "hello inner", 11);
    memset(buf, 0, sizeof buf);
    r = got == 1 ? read(fds[0], buf, sizeof buf) : -1;
    printf("a socket passed through a socket: %zd '%s'\n", r, buf);
    close(inner[0]);
    if (got == 1) close(fds[0]);

    /* descriptors in datagrams */
    int dp[2];
    socketpair(AF_UNIX, SOCK_DGRAM, 0, dp);
    pipe(pp);
    send_fds(dp[0], &pp[0], 1, 'd');
    wait_readable(dp[1]);
    r = recv_fds(dp[1], fds, 2, &got, &flags, 0);
    write(pp[1], "dgram", 5);
    memset(buf, 0, sizeof buf);
    r = got == 1 ? read(fds[0], buf, sizeof buf) : -1;
    printf("a descriptor in a datagram: %d received, read through it: %zd '%s'\n", got, r, buf);
    if (got == 1) close(fds[0]);
    close(dp[0]);
    close(dp[1]);

    /* credentials */
    int cp[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, cp);
    int one = 1;
    printf("SO_PASSCRED: %d\n", setsockopt(cp[1], SOL_SOCKET, SO_PASSCRED, &one, sizeof one));
    send(cp[0], "c", 1, 0);
    wait_readable(cp[1]);
    char ctl[CMSG_SPACE(sizeof(struct ucred))];
    memset(ctl, 0, sizeof ctl);
    char byte;
    struct iovec civ = { &byte, 1 };
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &civ;
    mh.msg_iovlen = 1;
    mh.msg_control = ctl;
    mh.msg_controllen = sizeof ctl;
    r = recvmsg(cp[1], &mh, 0);
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_CREDENTIALS) {
        struct ucred uc;
        memcpy(&uc, CMSG_DATA(c), sizeof uc);
        printf("SCM_CREDENTIALS: pid is ours %d, uid is ours %d, gid is ours %d\n", uc.pid == getpid(), uc.uid == getuid(), uc.gid == getgid());
    } else {
        printf("no SCM_CREDENTIALS arrived (%zd)\n", r);
    }
    close(cp[0]);
    close(cp[1]);
    close(st[0]);
    close(st[1]);
    close(pp[0]);
    close(pp[1]);
    return 0;
}
