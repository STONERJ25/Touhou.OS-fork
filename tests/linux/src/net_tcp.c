/*
 * TCP through a statically linked C library: a listener, a server thread that echoes, a client, exact byte counts for small and large transfers, MSG_PEEK and
 * MSG_DONTWAIT, shutdown and end of file, sendmsg and recvmsg with several buffers, writev and readv, the address calls and the options a program sets.
 * Prints only what the manual pages promise (no port numbers, no timings), so a real Linux prints the same lines (tools/linux-oracle.py); tests/case_linux_net.py
 * compares.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static int listener, port;
static unsigned long server_total;
static int server_saw_eof;

/* The server: accepts one connection and echoes everything back until the client shuts down its sending side, then closes. */
static void *server(void *arg) {
    (void)arg;
    struct sockaddr_in peer;
    socklen_t len = sizeof peer;
    int c = accept(listener, (struct sockaddr *)&peer, &len);
    if (c < 0) return NULL;
    char buf[8192];
    for (;;) {
        ssize_t n = read(c, buf, sizeof buf);
        if (n <= 0) { server_saw_eof = n == 0; break; }
        server_total += (unsigned long)n;
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(c, buf + off, (size_t)(n - off));
            if (w <= 0) break;
            off += w;
        }
    }
    close(c);
    return NULL;
}

/* reads exactly n bytes (or until end of file) */
static ssize_t read_full(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r <= 0) return got ? (ssize_t)got : r;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

/* waits (at most ten seconds) until the descriptor has something to read */
static void wait_readable(int fd) {
    struct pollfd p = { fd, POLLIN, 0 };
    poll(&p, 1, 10000);
}

static unsigned checksum(const unsigned char *p, size_t n) {
    unsigned a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

int main(void) {
    listener = socket(AF_INET, SOCK_STREAM, 0);
    printf("socket: %s\n", listener >= 0 ? "ok" : "failed");
    int type = 0;
    socklen_t tl = sizeof type;
    int r0 = getsockopt(listener, SOL_SOCKET, SO_TYPE, &type, &tl);
    printf("SO_TYPE: %d, length %u\n", r0 == 0 ? type : -1, tl);

    int one = 1;
    printf("SO_REUSEADDR: %d\n", setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    printf("bind to a free port on 127.0.0.1: %d\n", bind(listener, (struct sockaddr *)&sa, sizeof sa));
    printf("listen: %d\n", listen(listener, 8));
    socklen_t sl = sizeof sa;
    int gs = getsockname(listener, (struct sockaddr *)&sa, &sl);
    printf("getsockname: %d, family %d, address %s, port assigned %d, length %u\n", gs, sa.sin_family, inet_ntoa(sa.sin_addr), ntohs(sa.sin_port) != 0, sl);
    port = ntohs(sa.sin_port);
    int acceptconn = 0;
    tl = sizeof acceptconn;
    getsockopt(listener, SOL_SOCKET, SO_ACCEPTCONN, &acceptconn, &tl);
    printf("SO_ACCEPTCONN on the listener: %d\n", acceptconn);

    pthread_t th;
    pthread_create(&th, NULL, server, NULL);

    int c = socket(AF_INET, SOCK_STREAM, 0);
    printf("connect: %d\n", connect(c, (struct sockaddr *)&sa, sizeof sa));
    struct sockaddr_in me, peer;
    socklen_t ml = sizeof me, pl = sizeof peer;
    getsockname(c, (struct sockaddr *)&me, &ml);
    getpeername(c, (struct sockaddr *)&peer, &pl);
    printf("client local address %s, peer address %s, peer port is the listener's: %d\n", inet_ntoa(me.sin_addr), inet_ntoa(peer.sin_addr),
           ntohs(peer.sin_port) == port);

    int nodelay = 1, keepalive = 1, idle = 30, rcv = 65536, snd = 65536;
    struct linger lg = { 1, 5 };
    printf("TCP_NODELAY %d, SO_KEEPALIVE %d, TCP_KEEPIDLE %d, SO_RCVBUF %d, SO_SNDBUF %d, SO_LINGER %d\n", setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay),
           setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof keepalive), setsockopt(c, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle),
           setsockopt(c, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv), setsockopt(c, SOL_SOCKET, SO_SNDBUF, &snd, sizeof snd),
           setsockopt(c, SOL_SOCKET, SO_LINGER, &lg, sizeof lg));
    int v = 0;
    tl = sizeof v;
    getsockopt(c, IPPROTO_TCP, TCP_NODELAY, &v, &tl);
    printf("read back: nodelay %d", v != 0);
    v = 0; tl = sizeof v;
    getsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &v, &tl);
    printf(", keepalive %d", v != 0);
    v = 0; tl = sizeof v;
    getsockopt(c, IPPROTO_TCP, TCP_KEEPIDLE, &v, &tl);
    printf(", keepidle %d", v);
    struct linger lg2 = { 0, 0 };
    tl = sizeof lg2;
    getsockopt(c, SOL_SOCKET, SO_LINGER, &lg2, &tl);
    printf(", linger %d/%d\n", lg2.l_onoff != 0, lg2.l_linger);
    errno = 0;
    int bad = setsockopt(c, SOL_SOCKET, 9999, &one, sizeof one);
    printf("an unknown option: %d, errno %d (ENOPROTOOPT %d)\n", bad, errno, ENOPROTOOPT);

    char buf[64];
    printf("write hello: %zd\n", write(c, "hello", 5));
    memset(buf, 0, sizeof buf);
    printf("echo: %zd '%s'\n", read_full(c, buf, 5), buf);

    /* MSG_PEEK leaves the data in place */
    send(c, "abcdef", 6, 0);
    wait_readable(c);
    memset(buf, 0, sizeof buf);
    ssize_t pk = recv(c, buf, 3, MSG_PEEK);
    printf("MSG_PEEK: %zd '%s'", pk, buf);
    memset(buf, 0, sizeof buf);
    ssize_t rd = recv(c, buf, 6, MSG_WAITALL);
    printf(", then MSG_WAITALL read: %zd '%s'\n", rd, buf);

    errno = 0;
    ssize_t nb = recv(c, buf, sizeof buf, MSG_DONTWAIT);
    printf("MSG_DONTWAIT with nothing to read: %zd, errno %d (EAGAIN %d)\n", nb, errno, EAGAIN);

    /* sendmsg with three pieces, recvmsg into two buffers */
    char p1[] = "one-", p2[] = "two-", p3[] = "three";
    struct iovec iov3[3] = { { p1, 4 }, { p2, 4 }, { p3, 5 } };
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = iov3;
    mh.msg_iovlen = 3;
    printf("sendmsg of 4+4+5 bytes: %zd\n", sendmsg(c, &mh, 0));
    char r1[5] = { 0 }, r2[16] = { 0 };
    struct iovec iov2[2] = { { r1, 4 }, { r2, 9 } };
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = iov2;
    mh.msg_iovlen = 2;
    ssize_t rm = recvmsg(c, &mh, MSG_WAITALL);
    printf("recvmsg into 4+9 bytes: %zd '%s' '%s', msg_flags %d, msg_namelen %u\n", rm, r1, r2, mh.msg_flags, mh.msg_namelen);

    /* writev and readv */
    struct iovec wv[2] = { { "xy", 2 }, { "zzz", 3 } };
    printf("writev of 2+3 bytes: %zd\n", writev(c, wv, 2));
    char a1[3] = { 0 }, a2[8] = { 0 };
    struct iovec rv[2] = { { a1, 2 }, { a2, 3 } };
    wait_readable(c);
    printf("readv into 2+3 bytes: %zd '%s' '%s'\n", readv(c, rv, 2), a1, a2);

    /* sendfile: from a file to the socket, with an explicit offset (the file position stays) and without one (it moves) */
    int tfile = open("/tmp/net_tcp_sendfile", O_CREAT | O_RDWR | O_TRUNC, 0600);
    unsigned char *blk = malloc(100000), *back = malloc(100000);
    for (size_t i = 0; i < 100000; i++) blk[i] = (unsigned char)(i * 5 + (i >> 9));
    write(tfile, blk, 100000);
    off_t off = 1000;
    ssize_t sf = sendfile(c, tfile, &off, 50000);
    printf("sendfile of 50000 bytes from offset 1000: %zd, the offset moved on to 51000: %d, the file position was left alone: %d\n", sf, off == 51000,
           lseek(tfile, 0, SEEK_CUR) == 100000);
    ssize_t bk = read_full(c, back, 50000);
    printf("the echo has the file's bytes: %zd bytes, equal %d\n", bk, bk == 50000 && memcmp(back, blk + 1000, 50000) == 0);
    lseek(tfile, 2000, SEEK_SET);
    sf = sendfile(c, tfile, NULL, 3000);
    printf("sendfile without an offset: %zd, the file position moved to 5000: %d\n", sf, lseek(tfile, 0, SEEK_CUR) == 5000);
    bk = read_full(c, back, 3000);
    printf("the echo: %zd bytes, equal %d\n", bk, bk == 3000 && memcmp(back, blk + 2000, 3000) == 0);
    off = 99000;
    sf = sendfile(c, tfile, &off, 5000);
    printf("sendfile past the end of the file is cut short: %zd, the offset is 100000: %d\n", sf, off == 100000);
    read_full(c, back, 1000);
    int pp2[2];
    pipe(pp2);
    errno = 0;
    sf = sendfile(pp2[1], 999, NULL, 10);
    printf("sendfile from a descriptor that is not open: %zd errno %d (EBADF %d)\n", sf, errno, EBADF);
    sf = sendfile(pp2[1], tfile, NULL, 10);
    printf("sendfile into a pipe: %zd\n", sf);
    close(pp2[0]);
    close(pp2[1]);
    close(tfile);
    unlink("/tmp/net_tcp_sendfile");
    free(blk);
    free(back);

    /* a large transfer in both directions at once: the echo comes back while the rest is still being sent */
    size_t big = 1000000;
    unsigned char *out = malloc(big), *in = malloc(big);
    for (size_t i = 0; i < big; i++) out[i] = (unsigned char)(i * 7 + (i >> 8));
    size_t sent = 0, got = 0;
    while (got < big) {
        if (sent < big) {
            size_t n = big - sent > 20000 ? 20000 : big - sent;
            ssize_t w = send(c, out + sent, n, MSG_DONTWAIT);
            if (w > 0) sent += (size_t)w;
        }
        ssize_t r = recv(c, in + got, big - got, MSG_DONTWAIT);
        if (r > 0) got += (size_t)r;
        else if (r == 0) break;
        else if (errno != EAGAIN) break;
        else usleep(1000);
    }
    printf("large transfer: sent %zu, echoed back %zu, checksum equal %d\n", sent, got, checksum(out, big) == checksum(in, got == big ? big : 0));

    /* shutdown: the server sees the end of file, answers by closing, and the client then sees it too */
    printf("shutdown(SHUT_WR): %d\n", shutdown(c, SHUT_WR));
    ssize_t eof = read(c, buf, sizeof buf);
    printf("read after the server closed: %zd\n", eof);
    pthread_join(th, NULL);
    printf("the server read %lu bytes in all and saw end of file: %d\n", server_total, server_saw_eof);
    errno = 0;
    ssize_t late = send(c, "late", 4, MSG_NOSIGNAL);
    printf("send after our own shutdown: %zd, errno %d (EPIPE %d)\n", late, errno, EPIPE);
    int err = -1;
    tl = sizeof err;
    getsockopt(c, SOL_SOCKET, SO_ERROR, &err, &tl);
    printf("SO_ERROR: %d\n", err);
    printf("close: %d, %d\n", close(c), close(listener));
    free(out);
    free(in);
    return 0;
}
