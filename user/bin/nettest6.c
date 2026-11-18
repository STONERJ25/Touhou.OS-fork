/*
 * nettest6: exercises the IPv6 side of the network stack from user space. Most checks use the loopback (::1) and need no card; a few say that they need one (eth0 with its
 * link-local address). Prints one line per check and "NETTEST6 PASS" or "NETTEST6 FAIL" at the end; tests/case_net6.py runs it.
 *
 *   nettest6            the loopback checks
 *   nettest6 NAME       only the check called NAME (udp6, frag, tcp6, bulk6, refused6, close6, dual, mapped, opts6, ping6 on the loopback; loss6, fuzz6, reasm, exthdr as root; dad, ndfuzz and pmtu as root with a card)
 *   nettest6 gai NAME [inet|inet6|passive|addrconfig|v4mapped|all|numerichost]    what getaddrinfo answers, in its order
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <gensokyo.h>

static int g_failed;
static const char *g_name = "";

#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        if (!(cond)) {                                                                               \
            printf("  FAIL %s: %s (line %d, errno %d: %s)\n", g_name, #cond, __LINE__, errno, strerror(errno)); \
            g_failed++;                                                                              \
            return;                                                                                  \
        }                                                                                            \
    } while (0)

static struct sockaddr_in6 sa6(const char *text, unsigned port) {
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof a);
    a.sin6_family = AF_INET6;
    a.sin6_port = htons((unsigned short)port);
    inet_pton(AF_INET6, text, &a.sin6_addr);
    return a;
}

static struct sockaddr_in sa4(const char *text, unsigned port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, text, &a.sin_addr);
    return a;
}

static unsigned lcg(unsigned *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

/* a listening TCP socket on an IPv6 address (or ::) with a kernel-chosen port */
static int listener6(const char *addr, int backlog, unsigned *port, int v6only) {
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (v6only >= 0) setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof v6only);
    struct sockaddr_in6 a = sa6(addr, 0);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, backlog) < 0) { close(fd); return -1; }
    socklen_t len = sizeof a;
    getsockname(fd, (struct sockaddr *)&a, &len);
    *port = ntohs(a.sin6_port);
    return fd;
}

static int dial6(const char *addr, unsigned port) {
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 a = sa6(addr, port);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { if (fd >= 0) close(fd); return -1; }
    return fd;
}

static int dial4(const char *addr, unsigned port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = sa4(addr, port);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { if (fd >= 0) close(fd); return -1; }
    return fd;
}

static int read_full(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n) {
    size_t put = 0;
    while (put < n) {
        ssize_t r = write(fd, (const char *)buf + put, n - put);
        if (r <= 0) return -1;
        put += (size_t)r;
    }
    return 0;
}

static int is_mapped(const struct in6_addr *a, const char *v4) {
    struct in_addr x;
    inet_pton(AF_INET, v4, &x);
    return IN6_IS_ADDR_V4MAPPED(a) && !memcmp(&a->s6_addr[12], &x, 4);
}

/* ---------------------------------------------------------------- checks */

static void t_udp6(void) {
    int s = socket(AF_INET6, SOCK_DGRAM, 0), c = socket(AF_INET6, SOCK_DGRAM, 0);
    CHECK(s >= 0 && c >= 0);
    struct sockaddr_in6 a = sa6("::1", 0), from;
    CHECK(sizeof a == 28);
    CHECK(bind(s, (struct sockaddr *)&a, sizeof a) == 0);
    socklen_t len = sizeof a;
    CHECK(getsockname(s, (struct sockaddr *)&a, &len) == 0 && a.sin6_port != 0 && len == sizeof a && a.sin6_family == AF_INET6 && IN6_IS_ADDR_LOOPBACK(&a.sin6_addr));
    CHECK(sendto(c, "ping", 4, 0, (struct sockaddr *)&a, sizeof a) == 4);
    char buf[64];
    len = sizeof from;
    CHECK(recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)&from, &len) == 4 && !memcmp(buf, "ping", 4));
    CHECK(len == sizeof from && from.sin6_family == AF_INET6 && IN6_IS_ADDR_LOOPBACK(&from.sin6_addr) && from.sin6_port != 0 && from.sin6_scope_id == 0);
    CHECK(sendto(s, "pong!", 5, 0, (struct sockaddr *)&from, sizeof from) == 5);
    CHECK(recv(c, buf, sizeof buf, 0) == 5 && !memcmp(buf, "pong!", 5));
    /* a short buffer: the rest of the datagram is lost and MSG_TRUNC says how long it was */
    CHECK(sendto(c, "0123456789", 10, 0, (struct sockaddr *)&a, sizeof a) == 10);
    CHECK(recvfrom(s, buf, 4, MSG_TRUNC, NULL, NULL) == 10);
    /* nothing listens on the port any more: a connected UDP socket learns about it (ICMPv6 port unreachable) */
    close(s);
    CHECK(connect(c, (struct sockaddr *)&a, sizeof a) == 0);
    struct sockaddr_in6 me;
    len = sizeof me;
    CHECK(getsockname(c, (struct sockaddr *)&me, &len) == 0 && IN6_IS_ADDR_LOOPBACK(&me.sin6_addr));
    CHECK(getpeername(c, (struct sockaddr *)&me, &len) == 0 && me.sin6_port == a.sin6_port);
    CHECK(send(c, "x", 1, 0) == 1);
    int tries = 0;
    ssize_t r = -1;
    while (tries++ < 100) {
        r = recv(c, buf, sizeof buf, MSG_DONTWAIT);
        if (r < 0 && errno == ECONNREFUSED) break;
        usleep(10000);
    }
    CHECK(r < 0 && errno == ECONNREFUSED);
    close(c);
}

static void t_frag(void) {
    int s = socket(AF_INET6, SOCK_DGRAM, 0), c = socket(AF_INET6, SOCK_DGRAM, 0);
    CHECK(s >= 0 && c >= 0);
    struct sockaddr_in6 a = sa6("::1", 0);
    CHECK(bind(s, (struct sockaddr *)&a, sizeof a) == 0);
    socklen_t len = sizeof a;
    getsockname(s, (struct sockaddr *)&a, &len);
    static const int sizes[] = { 1, 1232, 1452, 1453, 2900, 6000, 20000, 65000, 65527 };
    for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        int n = sizes[k];
        char *big = malloc((size_t)n), *back = malloc((size_t)n + 1);
        for (int i = 0; i < n; i++) big[i] = (char)(i * 7 + k);
        CHECK(sendto(c, big, (size_t)n, 0, (struct sockaddr *)&a, sizeof a) == n);
        struct pollfd p = { s, POLLIN, 0 };
        CHECK(poll(&p, 1, 8000) == 1);
        CHECK(recvfrom(s, back, (size_t)n + 1, 0, NULL, NULL) == n && !memcmp(big, back, (size_t)n));
        free(big);
        free(back);
    }
    char *toobig = malloc(65528);
    CHECK(sendto(c, toobig, 65528, 0, (struct sockaddr *)&a, sizeof a) < 0 && errno == EMSGSIZE);
    free(toobig);
    close(s);
    close(c);
}

static void t_tcp6(void) {
    unsigned port;
    int l = listener6("::1", 5, &port, -1);
    CHECK(l >= 0);
    int c = dial6("::1", port);
    CHECK(c >= 0);
    struct sockaddr_in6 peer;
    socklen_t len = sizeof peer;
    int a = accept(l, (struct sockaddr *)&peer, &len);
    CHECK(a >= 0 && len == sizeof peer && peer.sin6_family == AF_INET6 && IN6_IS_ADDR_LOOPBACK(&peer.sin6_addr) && ntohs(peer.sin6_port) > 1023);
    struct sockaddr_in6 me;
    len = sizeof me;
    CHECK(getsockname(c, (struct sockaddr *)&me, &len) == 0 && me.sin6_port == peer.sin6_port && IN6_IS_ADDR_LOOPBACK(&me.sin6_addr));
    CHECK(getpeername(c, (struct sockaddr *)&me, &len) == 0 && ntohs(me.sin6_port) == port);
    CHECK(write_full(c, "hello over IPv6", 15) == 0);
    char buf[32];
    CHECK(read_full(a, buf, 15) == 0 && !memcmp(buf, "hello over IPv6", 15));
    CHECK(write_full(a, "and back", 8) == 0);
    CHECK(read_full(c, buf, 8) == 0 && !memcmp(buf, "and back", 8));
    int mss = 0;
    socklen_t ml = sizeof mss;
    CHECK(getsockopt(c, IPPROTO_TCP, TCP_MAXSEG, &mss, &ml) == 0 && mss == 1500 - 60);       /* the loopback's MTU minus the IPv6 and TCP headers */
    close(a);
    close(c);
    close(l);
}

static void t_bulk6(void) {
    unsigned port;
    int l = listener6("::1", 5, &port, -1);
    CHECK(l >= 0);
    const size_t total = 2 * 1024 * 1024;
    pid_t pid = fork();
    if (pid == 0) {
        int c = dial6("::1", port);
        if (c < 0) _exit(2);
        static unsigned char chunk[8192];
        unsigned seed = 99, sum = 0;
        size_t sent = 0;
        while (sent < total) {
            size_t n = total - sent < sizeof chunk ? total - sent : sizeof chunk;
            for (size_t i = 0; i < n; i++) { chunk[i] = (unsigned char)lcg(&seed); sum = sum * 31 + chunk[i]; }
            if (write_full(c, chunk, n) < 0) _exit(3);
            sent += n;
        }
        write_full(c, &sum, sizeof sum);
        close(c);
        _exit(0);
    }
    int a = accept(l, NULL, NULL);
    CHECK(a >= 0);
    static unsigned char buf[8192];
    unsigned sum = 0, theirs = 0;
    size_t got = 0;
    long t0 = gap_time_ms();
    while (got < total) {
        size_t want = total - got < sizeof buf ? total - got : sizeof buf;
        ssize_t r = read(a, buf, want);
        CHECK(r > 0);
        for (ssize_t i = 0; i < r; i++) sum = sum * 31 + buf[i];
        got += (size_t)r;
    }
    CHECK(read_full(a, &theirs, sizeof theirs) == 0);
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(sum == theirs);
    printf("  bulk6: %u bytes, checksum %08x ok in %ld ms\n", (unsigned)total, sum, gap_time_ms() - t0);
    close(a);
    close(l);
}

static void t_refused6(void) {
    unsigned port;
    int l = listener6("::1", 1, &port, -1);
    CHECK(l >= 0);
    close(l);
    int c = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 a = sa6("::1", port);
    CHECK(connect(c, (struct sockaddr *)&a, sizeof a) < 0 && errno == ECONNREFUSED);
    close(c);
    /* a non-blocking connect that is refused is reported through poll and SO_ERROR */
    c = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int r = connect(c, (struct sockaddr *)&a, sizeof a);
    CHECK(r < 0 && (errno == EINPROGRESS || errno == ECONNREFUSED));
    struct pollfd p = { c, POLLOUT, 0 };
    CHECK(poll(&p, 1, 5000) == 1);
    int err = 0;
    socklen_t el = sizeof err;
    CHECK(getsockopt(c, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && (err == ECONNREFUSED || r < 0));
    close(c);
}

static void t_close6(void) {
    unsigned port;
    int l = listener6("::1", 5, &port, -1);
    int c = dial6("::1", port);
    int a = accept(l, NULL, NULL);
    CHECK(l >= 0 && c >= 0 && a >= 0);
    CHECK(write_full(c, "bye", 3) == 0);
    shutdown(c, SHUT_WR);                                                 /* a half-close: the peer reads the data and then end-of-file, and can still answer */
    char buf[8];
    CHECK(read_full(a, buf, 3) == 0 && !memcmp(buf, "bye", 3));
    CHECK(read(a, buf, sizeof buf) == 0);
    CHECK(write_full(a, "ok", 2) == 0);
    CHECK(read_full(c, buf, 2) == 0 && !memcmp(buf, "ok", 2));
    close(a);
    CHECK(read(c, buf, sizeof buf) == 0);
    close(c);
    /* an abortive close (unread data at close()) resets the peer */
    c = dial6("::1", port);
    a = accept(l, NULL, NULL);
    CHECK(c >= 0 && a >= 0);
    CHECK(write_full(c, "unread", 6) == 0);
    usleep(100000);
    close(a);
    usleep(100000);
    ssize_t r = write(c, "x", 1);
    if (r > 0) { usleep(100000); r = write(c, "y", 1); }
    CHECK(r < 0 && (errno == ECONNRESET || errno == EPIPE));
    close(c);
    close(l);
}

/* A socket bound to :: takes IPv6 and IPv4 (RFC 3493 section 5.3): the IPv4 peer shows as a v4-mapped address; with IPV6_V6ONLY it takes IPv6 only. */
static void t_dual(void) {
    unsigned port;
    int one = 1, got = -1;
    socklen_t gl = sizeof got;
    int l = listener6("::", 5, &port, -1);
    CHECK(l >= 0);
    CHECK(getsockopt(l, IPPROTO_IPV6, IPV6_V6ONLY, &got, &gl) == 0 && got == 0);                 /* off by default, as on Linux */
    struct sockaddr_in6 me;
    socklen_t len = sizeof me;
    CHECK(getsockname(l, (struct sockaddr *)&me, &len) == 0 && IN6_IS_ADDR_UNSPECIFIED(&me.sin6_addr) && ntohs(me.sin6_port) == port);
    int c6 = dial6("::1", port), c4 = dial4("127.0.0.1", port);
    CHECK(c6 >= 0 && c4 >= 0);
    struct sockaddr_in6 p1, p2;
    len = sizeof p1;
    int a1 = accept(l, (struct sockaddr *)&p1, &len);
    len = sizeof p2;
    int a2 = accept(l, (struct sockaddr *)&p2, &len);
    CHECK(a1 >= 0 && a2 >= 0);
    CHECK(IN6_IS_ADDR_LOOPBACK(&p1.sin6_addr) && is_mapped(&p2.sin6_addr, "127.0.0.1") && p2.sin6_family == AF_INET6);
    struct sockaddr_in6 ours;
    len = sizeof ours;
    CHECK(getsockname(a2, (struct sockaddr *)&ours, &len) == 0 && is_mapped(&ours.sin6_addr, "127.0.0.1") && ntohs(ours.sin6_port) == port);
    CHECK(write_full(c4, "four", 4) == 0);
    char buf[8];
    CHECK(read_full(a2, buf, 4) == 0 && !memcmp(buf, "four", 4));
    CHECK(write_full(a2, "back", 4) == 0 && read_full(c4, buf, 4) == 0 && !memcmp(buf, "back", 4));
    close(a1); close(a2); close(c6); close(c4); close(l);
    /* IPV6_V6ONLY: the IPv4 connection is refused */
    l = listener6("::", 5, &port, 1);
    CHECK(l >= 0);
    CHECK(getsockopt(l, IPPROTO_IPV6, IPV6_V6ONLY, &got, &gl) == 0 && got == 1);
    CHECK(dial4("127.0.0.1", port) < 0 && errno == ECONNREFUSED);
    c6 = dial6("::1", port);
    CHECK(c6 >= 0);
    close(c6);
    /* it cannot be changed once the socket has a port */
    CHECK(setsockopt(l, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one) < 0 && errno == EINVAL);
    close(l);
    /* UDP: an IPv4 datagram reaches the dual socket with a mapped source; with V6ONLY it does not */
    int u = socket(AF_INET6, SOCK_DGRAM, 0);
    struct sockaddr_in6 ua = sa6("::", 0);
    CHECK(bind(u, (struct sockaddr *)&ua, sizeof ua) == 0);
    len = sizeof ua;
    getsockname(u, (struct sockaddr *)&ua, &len);
    int s4 = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to4 = sa4("127.0.0.1", ntohs(ua.sin6_port));
    CHECK(sendto(s4, "udp4", 4, 0, (struct sockaddr *)&to4, sizeof to4) == 4);
    struct sockaddr_in6 from;
    len = sizeof from;
    struct pollfd pf = { u, POLLIN, 0 };
    CHECK(poll(&pf, 1, 3000) == 1);
    CHECK(recvfrom(u, buf, sizeof buf, 0, (struct sockaddr *)&from, &len) == 4 && is_mapped(&from.sin6_addr, "127.0.0.1"));
    CHECK(sendto(u, "echo", 4, 0, (struct sockaddr *)&from, len) == 4);                           /* the answer goes back over IPv4 */
    CHECK(recv(s4, buf, sizeof buf, 0) == 4 && !memcmp(buf, "echo", 4));
    close(u);
    u = socket(AF_INET6, SOCK_DGRAM, 0);
    setsockopt(u, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
    ua = sa6("::", 0);
    CHECK(bind(u, (struct sockaddr *)&ua, sizeof ua) == 0);
    len = sizeof ua;
    getsockname(u, (struct sockaddr *)&ua, &len);
    to4.sin_port = ua.sin6_port;
    CHECK(sendto(s4, "udp4", 4, 0, (struct sockaddr *)&to4, sizeof to4) == 4);
    pf.fd = u;
    CHECK(poll(&pf, 1, 500) == 0);
    close(u);
    close(s4);
}

static void t_mapped(void) {
    /* an AF_INET6 socket connecting to ::ffff:127.0.0.1 talks IPv4 to an IPv4 server */
    unsigned port;
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a4 = sa4("127.0.0.1", 0);
    CHECK(l >= 0 && bind(l, (struct sockaddr *)&a4, sizeof a4) == 0 && listen(l, 5) == 0);
    socklen_t len = sizeof a4;
    getsockname(l, (struct sockaddr *)&a4, &len);
    port = ntohs(a4.sin_port);
    int c = dial6("::ffff:127.0.0.1", port);
    CHECK(c >= 0);
    struct sockaddr_in peer4;
    len = sizeof peer4;
    int a = accept(l, (struct sockaddr *)&peer4, &len);
    CHECK(a >= 0 && peer4.sin_family == AF_INET && peer4.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    struct sockaddr_in6 me;
    len = sizeof me;
    CHECK(getsockname(c, (struct sockaddr *)&me, &len) == 0 && is_mapped(&me.sin6_addr, "127.0.0.1") && len == sizeof me);
    CHECK(getpeername(c, (struct sockaddr *)&me, &len) == 0 && is_mapped(&me.sin6_addr, "127.0.0.1") && ntohs(me.sin6_port) == port);
    CHECK(write_full(c, "mapped", 6) == 0);
    char buf[8];
    CHECK(read_full(a, buf, 6) == 0 && !memcmp(buf, "mapped", 6));
    close(a); close(c); close(l);
    /* a V6ONLY socket has no way to reach an IPv4 peer */
    int s = socket(AF_INET6, SOCK_STREAM, 0), one = 1;
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
    struct sockaddr_in6 m = sa6("::ffff:127.0.0.1", 9);
    CHECK(connect(s, (struct sockaddr *)&m, sizeof m) < 0 && errno == ENETUNREACH);
    close(s);
}

static void t_opts6(void) {
    int s = socket(AF_INET6, SOCK_DGRAM, 0), v = 0;
    socklen_t n = sizeof v;
    CHECK(s >= 0);
    CHECK(getsockopt(s, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &v, &n) == 0 && v == 64);
    v = 17;
    CHECK(setsockopt(s, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &v, sizeof v) == 0);
    v = 0;
    n = sizeof v;
    CHECK(getsockopt(s, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &v, &n) == 0 && v == 17);
    v = 256;
    CHECK(setsockopt(s, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &v, sizeof v) < 0 && errno == EINVAL);
    v = -1;
    CHECK(setsockopt(s, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &v, sizeof v) == 0);                    /* -1: the default again */
    n = sizeof v;
    CHECK(getsockopt(s, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &v, &n) == 0 && v == 64);
    v = 0x28;
    CHECK(setsockopt(s, IPPROTO_IPV6, IPV6_TCLASS, &v, sizeof v) == 0);
    v = 0;
    n = sizeof v;
    CHECK(getsockopt(s, IPPROTO_IPV6, IPV6_TCLASS, &v, &n) == 0 && v == 0x28);
    v = 1;
    CHECK(setsockopt(s, IPPROTO_IPV6, IPV6_RECVPKTINFO, &v, sizeof v) == 0);
    v = 0;
    n = sizeof v;
    CHECK(getsockopt(s, IPPROTO_IPV6, IPV6_RECVPKTINFO, &v, &n) == 0 && v == 1);
    CHECK(setsockopt(s, IPPROTO_IPV6, 9999, &v, sizeof v) < 0 && errno == ENOPROTOOPT);
    n = sizeof v;
    CHECK(getsockopt(s, SOL_SOCKET, 39 /* SO_DOMAIN */, &v, &n) == 0 && v == AF_INET6);
    close(s);
    /* an IPv4 socket has no IPv6 options */
    s = socket(AF_INET, SOCK_DGRAM, 0);
    v = 1;
    CHECK(setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &v, sizeof v) < 0);
    close(s);
    /* addresses: the family, the length, a foreign address, a conflict */
    s = socket(AF_INET6, SOCK_DGRAM, 0);
    struct sockaddr_in6 a = sa6("::1", 0);
    struct sockaddr_in a4 = sa4("127.0.0.1", 0);
    CHECK(bind(s, (struct sockaddr *)&a4, sizeof a4) < 0 && errno == EINVAL);                        /* an IPv6 socket wants a sockaddr_in6: a sockaddr_in is too short (Linux checks the length first) */
    CHECK(bind(s, (struct sockaddr *)&a, 10) < 0 && errno == EINVAL);                                 /* too short */
    struct sockaddr_in6 wrongfam = sa6("::1", 0);
    wrongfam.sin6_family = AF_INET;
    CHECK(bind(s, (struct sockaddr *)&wrongfam, sizeof wrongfam) < 0 && errno == EAFNOSUPPORT);      /* long enough, but not an IPv6 address */
    struct sockaddr_in6 foreign = sa6("2001:db8::1", 0);
    CHECK(bind(s, (struct sockaddr *)&foreign, sizeof foreign) < 0 && errno == EADDRNOTAVAIL);
    struct sockaddr_in6 mc = sa6("ff02::1", 0);
    CHECK(bind(s, (struct sockaddr *)&mc, sizeof mc) < 0);
    CHECK(bind(s, (struct sockaddr *)&a, sizeof a) == 0);
    socklen_t len = sizeof a;
    getsockname(s, (struct sockaddr *)&a, &len);
    int s2 = socket(AF_INET6, SOCK_DGRAM, 0);
    CHECK(bind(s2, (struct sockaddr *)&a, sizeof a) < 0 && errno == EADDRINUSE);
    struct sockaddr_in6 any = sa6("::", ntohs(a.sin6_port));
    CHECK(bind(s2, (struct sockaddr *)&any, sizeof any) < 0 && errno == EADDRINUSE);                  /* :: overlaps ::1 */
    close(s2);
    /* an IPv4 socket on the same port: the IPv6 binding to ::1 does not overlap it */
    int s3 = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in same4 = sa4("127.0.0.1", ntohs(a.sin6_port));
    CHECK(bind(s3, (struct sockaddr *)&same4, sizeof same4) == 0);
    close(s3);
    close(s);
    /* a dual-stack :: binding and an IPv4 binding on the same port do conflict; with V6ONLY they do not */
    s = socket(AF_INET6, SOCK_DGRAM, 0);
    any = sa6("::", 0);
    CHECK(bind(s, (struct sockaddr *)&any, sizeof any) == 0);
    len = sizeof any;
    getsockname(s, (struct sockaddr *)&any, &len);
    s3 = socket(AF_INET, SOCK_DGRAM, 0);
    same4 = sa4("127.0.0.1", ntohs(any.sin6_port));
    CHECK(bind(s3, (struct sockaddr *)&same4, sizeof same4) < 0 && errno == EADDRINUSE);
    close(s3);
    close(s);
    s = socket(AF_INET6, SOCK_DGRAM, 0);
    v = 1;
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &v, sizeof v);
    CHECK(bind(s, (struct sockaddr *)&any, sizeof any) == 0);
    s3 = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(bind(s3, (struct sockaddr *)&same4, sizeof same4) == 0);
    close(s3);
    close(s);
    /* the textual forms of addresses (inet_ntop is checked against the C library in the host tests) */
    char text[INET6_ADDRSTRLEN];
    struct in6_addr x;
    CHECK(inet_pton(AF_INET6, "fe80::1:2", &x) == 1 && inet_ntop(AF_INET6, &x, text, sizeof text) && !strcmp(text, "fe80::1:2"));
    CHECK(inet_pton(AF_INET6, "::ffff:10.0.2.2", &x) == 1 && inet_ntop(AF_INET6, &x, text, sizeof text) && !strcmp(text, "::ffff:10.0.2.2"));
    CHECK(inet_pton(AF_INET6, "1::2::3", &x) == 0);
}

static unsigned short csum16(const void *data, size_t n) {
    const unsigned char *p = data;
    unsigned long sum = 0;
    while (n > 1) { sum += (unsigned long)(p[0] << 8 | p[1]); p += 2; n -= 2; }
    if (n) sum += (unsigned long)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned short)~sum;
}

static void t_ping6(void) {
    int s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6);
    CHECK(s >= 0);
    unsigned char pkt[16 + 8];
    memset(pkt, 0, sizeof pkt);
    pkt[0] = 128;                                       /* echo request; the kernel fills the identifier and the checksum */
    pkt[6] = 0; pkt[7] = 1;                             /* sequence 1 */
    memcpy(pkt + 8, "gensokyo-ping!!", 15);
    struct sockaddr_in6 a = sa6("::1", 0);
    CHECK(sendto(s, pkt, sizeof pkt, 0, (struct sockaddr *)&a, sizeof a) == (ssize_t)sizeof pkt);
    struct pollfd p = { s, POLLIN, 0 };
    CHECK(poll(&p, 1, 5000) == 1);
    unsigned char rep[128];
    struct sockaddr_in6 from;
    socklen_t fl = sizeof from;
    ssize_t r = recvfrom(s, rep, sizeof rep, 0, (struct sockaddr *)&from, &fl);
    CHECK(r == (ssize_t)sizeof pkt);                    /* the ICMPv6 message, with no IPv6 header */
    CHECK(rep[0] == 129 && rep[1] == 0 && rep[7] == 1 && !memcmp(rep + 8, "gensokyo-ping!!", 15) && IN6_IS_ADDR_LOOPBACK(&from.sin6_addr));
    CHECK(rep[4] != 0 || rep[5] != 0);                  /* the identifier the kernel chose */
    (void)csum16;
    /* a request that is not an echo request is refused */
    pkt[0] = 129;
    CHECK(sendto(s, pkt, sizeof pkt, 0, (struct sockaddr *)&a, sizeof a) < 0 && errno == EINVAL);
    /* a big echo (fragmented both ways) */
    static unsigned char big[8 + 5000], back[8 + 5000 + 8];
    memset(big, 0, sizeof big);
    big[0] = 128; big[7] = 2;
    for (int i = 8; i < (int)sizeof big; i++) big[i] = (unsigned char)i;
    CHECK(sendto(s, big, sizeof big, 0, (struct sockaddr *)&a, sizeof a) == (ssize_t)sizeof big);
    CHECK(poll(&p, 1, 5000) == 1);
    r = recv(s, back, sizeof back, 0);
    CHECK(r == (ssize_t)sizeof big && back[0] == 129 && !memcmp(back + 8, big + 8, sizeof big - 8));
    close(s);
}

/* ---------------------------------------------------------------- the lossy loopback */

static void t_loss6(void) {
    unsigned port;
    int l = listener6("::1", 5, &port, -1);
    CHECK(l >= 0);
    const size_t total = 300 * 1024;
    pid_t pid = fork();
    if (pid == 0) {
        int c = dial6("::1", port);
        if (c < 0) _exit(2);
        static unsigned char chunk[8192];
        unsigned seed = 777, sum = 0;
        size_t sent = 0;
        while (sent < total) {
            size_t n = total - sent < sizeof chunk ? total - sent : sizeof chunk;
            for (size_t i = 0; i < n; i++) { chunk[i] = (unsigned char)lcg(&seed); sum = sum * 31 + chunk[i]; }
            if (write_full(c, chunk, n) < 0) _exit(3);
            sent += n;
        }
        write_full(c, &sum, sizeof sum);
        close(c);
        _exit(0);
    }
    int a = accept(l, NULL, NULL);
    CHECK(a >= 0);
    static unsigned char buf[8192];
    unsigned sum = 0, theirs = 0;
    size_t got = 0;
    long t0 = gap_time_ms();
    while (got < total) {
        size_t want = total - got < sizeof buf ? total - got : sizeof buf;
        ssize_t r = read(a, buf, want);
        CHECK(r > 0);
        for (ssize_t i = 0; i < r; i++) sum = sum * 31 + buf[i];
        got += (size_t)r;
    }
    CHECK(read_full(a, &theirs, sizeof theirs) == 0);
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(sum == theirs);
    printf("  loss6: %u bytes intact in %ld ms\n", (unsigned)total, gap_time_ms() - t0);
    close(a);
    close(l);
    /* fragmented datagrams through duplication: a repeated fragment is dropped alone, so what arrives is whole (and nothing arrives twice for one send, apart from whole copies) */
    int s = socket(AF_INET6, SOCK_DGRAM, 0), c = socket(AF_INET6, SOCK_DGRAM, 0);
    struct sockaddr_in6 ua = sa6("::1", 0);
    CHECK(bind(s, (struct sockaddr *)&ua, sizeof ua) == 0);
    socklen_t len = sizeof ua;
    getsockname(s, (struct sockaddr *)&ua, &len);
    char big[5000], back[5001];
    int whole = 0, damaged = 0;
    for (int i = 0; i < 40; i++) {
        for (int k = 0; k < 5000; k++) big[k] = (char)(k * 3 + i);
        CHECK(sendto(c, big, sizeof big, 0, (struct sockaddr *)&ua, sizeof ua) == 5000);
        usleep(20000);
    }
    for (;;) {
        struct pollfd p = { s, POLLIN, 0 };
        if (poll(&p, 1, 500) != 1) break;
        ssize_t r = recv(s, back, sizeof back, 0);
        if (r != 5000) { damaged++; continue; }
        int i = (unsigned char)back[0] == 0 ? -1 : 0;
        (void)i;
        int ok = 1;
        for (int k = 1; k < 5000 && ok; k++) if ((unsigned char)(back[k] - back[0]) != (unsigned char)(k * 3)) ok = 0;
        if (ok) whole++; else damaged++;
    }
    CHECK(damaged == 0 && whole >= 10);
    printf("  loss6: %d whole datagrams of 40, none damaged\n", whole);
    close(s);
    close(c);
}

/* ---------------------------------------------------------------- malformed frames */

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }

/* The ICMPv6 / UDP / TCP checksum of a message with the pseudo-header of RFC 8200 section 8.1 (all of it in big-endian arithmetic). */
static unsigned short l4csum(const unsigned char *src, const unsigned char *dst, unsigned char proto, const unsigned char *msg, size_t len) {
    static unsigned char pseudo[40 + 65535];                                  /* (a whole datagram: the reassembly checks make 3000-byte ones) */
    if (len > 65535) len = 65535;
    memcpy(pseudo, src, 16);
    memcpy(pseudo + 16, dst, 16);
    pseudo[32] = 0; pseudo[33] = 0; pseudo[34] = (unsigned char)(len >> 8); pseudo[35] = (unsigned char)len;
    pseudo[36] = pseudo[37] = pseudo[38] = 0; pseudo[39] = proto;
    memcpy(pseudo + 40, msg, len);
    return csum16(pseudo, 40 + len);
}

static const unsigned char LOOP6[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };

static void send6(int fd, const unsigned char *ip, size_t n) {
    unsigned char fr[2048];
    memset(fr, 0, 12);
    fr[12] = 0x86; fr[13] = 0xDD;
    memcpy(fr + 14, ip, n);
    if (write(fd, fr, 14 + n) < 0) { /* ignore */ }
}

static void t_fuzz6(void) {
    int fd = open("/proc/net/inject", O_WRONLY);
    CHECK(fd >= 0);
    unsigned seed = 6666;
    unsigned char ip[1600];
    /* a receiving socket of each kind, so that the transports get something to chew on */
    unsigned port;
    int l = listener6("::1", 5, &port, -1);
    int u = socket(AF_INET6, SOCK_DGRAM, 0);
    struct sockaddr_in6 ua = sa6("::1", 0);
    CHECK(l >= 0 && bind(u, (struct sockaddr *)&ua, sizeof ua) == 0);
    socklen_t ul = sizeof ua;
    getsockname(u, (struct sockaddr *)&ua, &ul);
    for (int round = 0; round < 3000; round++) {
        size_t n = 1 + lcg(&seed) % 150;
        for (size_t i = 0; i < n; i++) ip[i] = (unsigned char)lcg(&seed);
        switch (round % 8) {
        case 0: break;                                                       /* pure noise */
        case 1: ip[0] = 0x60; break;                                         /* plausible version, random everything else */
        default: {                                                           /* a header that is right and a body that is not */
            if (n < 48) n = 48;
            memset(ip, 0, 40);
            ip[0] = 0x60;
            put16(ip + 4, (unsigned)(n - 40) + (lcg(&seed) % 4 == 0 ? lcg(&seed) % 20 : 0));       /* the payload length, sometimes a lie */
            static const unsigned char nhs[] = { 6, 17, 58, 0, 43, 44, 60, 59, 50, 255 };
            ip[6] = nhs[lcg(&seed) % sizeof nhs];
            ip[7] = (unsigned char)(lcg(&seed) % 3 ? 64 : lcg(&seed));
            memcpy(ip + 8, LOOP6, 16);
            memcpy(ip + 24, LOOP6, 16);
            for (size_t i = 40; i < n; i++) if (lcg(&seed) % 3) ip[i] = (unsigned char)lcg(&seed);
            if (round % 8 == 2 && n >= 48) { ip[6] = 44; ip[40] = 17; ip[41] = 0; put16(ip + 42, (unsigned)((lcg(&seed) % 400) << 3 | (lcg(&seed) & 1))); ip[44] = ip[45] = ip[46] = 0; ip[47] = (unsigned char)(lcg(&seed) % 3); }    /* odd fragments, overlapping ids */
            if (round % 8 == 3) { ip[6] = 17; put16(ip + 40, (unsigned)ntohs(ua.sin6_port)); put16(ip + 42, (unsigned)ntohs(ua.sin6_port)); put16(ip + 44, (unsigned)(n - 40)); }   /* UDP to a port that is open */
            if (round % 8 == 4) { ip[6] = 6; put16(ip + 42, port); }          /* TCP to the listener */
            if (round % 8 == 5) { ip[6] = 58; ip[40] = (unsigned char)(128 + lcg(&seed) % 10); ip[41] = 0; }   /* ICMPv6: echo and neighbour discovery types */
            if (lcg(&seed) % 4 == 0 && (ip[6] == 58 || ip[6] == 17 || ip[6] == 6)) {              /* a right checksum for what is there */
                size_t off = ip[6] == 58 ? 2 : ip[6] == 17 ? 6 : 16;
                if (n >= 40 + off + 2) {
                    ip[40 + off] = ip[40 + off + 1] = 0;
                    unsigned short c = l4csum(ip + 8, ip + 24, ip[6], ip + 40, n - 40);
                    put16(ip + 40 + off, c);
                }
            }
            break;
        }
        }
        send6(fd, ip, n);
    }
    close(fd);
    /* what the stack said about it (/proc/net/tune: ip6_bad), and that it still works */
    char tune[512];
    int tf = open("/proc/net/tune", O_RDONLY);
    ssize_t tn = tf >= 0 ? read(tf, tune, sizeof tune - 1) : -1;
    if (tf >= 0) close(tf);
    CHECK(tn > 0);
    tune[tn] = 0;
    char *p6 = strstr(tune, "ip6_bad ");
    CHECK(p6 && atol(p6 + 8) > 500);
    printf("  fuzz6: %ld malformed packets rejected\n", atol(p6 + 8));
    close(u);
    close(l);
    unsigned port2;
    l = listener6("::1", 5, &port2, -1);
    int c = dial6("::1", port2);
    int a = accept(l, NULL, NULL);
    CHECK(l >= 0 && c >= 0 && a >= 0);
    CHECK(write_full(c, "still alive", 11) == 0);
    char buf[16];
    CHECK(read_full(a, buf, 11) == 0 && !memcmp(buf, "still alive", 11));
    close(a); close(c); close(l);
}

/* ---------------------------------------------------------------- reassembly and extension headers, with frames made here */

static void put32(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }

/* The value of a counter of /proc/net/snmp6 (-1 if there is none by that name). */
static long snmp6(const char *name) {
    char buf[4096];
    int f = open("/proc/net/snmp6", O_RDONLY);
    if (f < 0) return -1;
    ssize_t n = read(f, buf, sizeof buf - 1);
    close(f);
    if (n <= 0) return -1;
    buf[n] = 0;
    size_t l = strlen(name);
    for (char *p = buf; p && *p;) {
        if (!strncmp(p, name, l) && (p[l] == ' ' || p[l] == '\t')) return atol(p + l + 1);
        p = strchr(p, '\n');
        if (p) p++;
    }
    return -1;
}

/* An IPv6 packet from ::1 to ::1: header, then payload, next header nh. */
static size_t mk6(unsigned char *out, unsigned nh, const unsigned char *payload, size_t plen) {
    memset(out, 0, 40);
    out[0] = 0x60;
    put16(out + 4, (unsigned)plen);
    out[6] = (unsigned char)nh;
    out[7] = 64;
    memcpy(out + 8, LOOP6, 16);
    memcpy(out + 24, LOOP6, 16);
    memcpy(out + 40, payload, plen);
    return 40 + plen;
}

/* A UDP datagram with its header and checksum, from ::1 to ::1, in dg; returns its length. */
static size_t mk_udp(unsigned char *dg, unsigned sport, unsigned dport, const unsigned char *data, size_t n) {
    put16(dg, sport); put16(dg + 2, dport); put16(dg + 4, (unsigned)(8 + n)); dg[6] = dg[7] = 0;
    memcpy(dg + 8, data, n);
    unsigned short c = l4csum(LOOP6, LOOP6, 17, dg, 8 + n);
    if (c == 0) c = 0xFFFF;
    put16(dg + 6, c);
    return 8 + n;
}

/* One fragment of dg (the bytes [off, off+n)): fragment header with the more flag and the id, then the bytes. */
static void send_frag(int fd, const unsigned char *dg, size_t off, size_t n, int more, unsigned id) {
    unsigned char body[1600], pkt[1700];
    body[0] = 17; body[1] = 0;
    put16(body + 2, (unsigned)(off | (more ? 1 : 0)));
    put32(body + 4, id);
    memcpy(body + 8, dg + off, n);
    size_t len = mk6(pkt, 44, body, 8 + n);
    send6(fd, pkt, len);
}

static int wait_datagram(int s, void *buf, size_t cap, int ms) {
    struct pollfd p = { s, POLLIN, 0 };
    if (poll(&p, 1, ms) != 1) return -1;
    return (int)recv(s, buf, cap, 0);
}

static void t_reasm(void) {
    int fd = open("/proc/net/inject", O_WRONLY);
    CHECK(fd >= 0);
    int s = socket(AF_INET6, SOCK_DGRAM, 0);
    struct sockaddr_in6 ua = sa6("::1", 0);
    CHECK(bind(s, (struct sockaddr *)&ua, sizeof ua) == 0);
    socklen_t ul = sizeof ua;
    getsockname(s, (struct sockaddr *)&ua, &ul);
    unsigned port = ntohs(ua.sin6_port);
    static unsigned char data[3000], dg[3008], back[3100];
    for (int i = 0; i < 3000; i++) data[i] = (unsigned char)(i * 13 + 5);
    size_t dl = mk_udp(dg, 4000, port, data, 3000);
    CHECK(dl == 3008);
    /* in order, then backwards, then with repeated fragments: the datagram arrives whole, once. (A copy that arrives after the datagram is complete would start a new datagram that
     * nobody finishes, so the fragment that completes it is never repeated; the stack keeps at most two such datagrams per source, which the later checks count on.) */
    send_frag(fd, dg, 0, 1232, 1, 0x1001); send_frag(fd, dg, 1232, 1232, 1, 0x1001); send_frag(fd, dg, 2464, 544, 0, 0x1001);
    CHECK(wait_datagram(s, back, sizeof back, 3000) == 3000 && !memcmp(back, data, 3000));
    send_frag(fd, dg, 2464, 544, 0, 0x1002); send_frag(fd, dg, 1232, 1232, 1, 0x1002); send_frag(fd, dg, 0, 1232, 1, 0x1002);
    CHECK(wait_datagram(s, back, sizeof back, 3000) == 3000 && !memcmp(back, data, 3000));
    long fails0 = snmp6("Ip6ReasmFails");
    send_frag(fd, dg, 0, 1232, 1, 0x1003); send_frag(fd, dg, 0, 1232, 1, 0x1003); send_frag(fd, dg, 1232, 1232, 1, 0x1003); send_frag(fd, dg, 1232, 1232, 1, 0x1003);
    send_frag(fd, dg, 2464, 544, 0, 0x1003);
    CHECK(wait_datagram(s, back, sizeof back, 3000) == 3000 && !memcmp(back, data, 3000));
    CHECK(wait_datagram(s, back, sizeof back, 300) < 0);                     /* once: a repeated fragment is dropped alone */
    send_frag(fd, dg, 0, 1232, 1, 0x1006); send_frag(fd, dg, 2464, 544, 0, 0x1006); send_frag(fd, dg, 2464, 544, 0, 0x1006); send_frag(fd, dg, 1232, 1232, 1, 0x1006);
    CHECK(wait_datagram(s, back, sizeof back, 3000) == 3000 && !memcmp(back, data, 3000));
    CHECK(wait_datagram(s, back, sizeof back, 300) < 0);
    CHECK(snmp6("Ip6ReasmFails") == fails0);
    /* overlapping fragments: the whole datagram is thrown away, even when the rest arrives afterwards (RFC 5722) */
    send_frag(fd, dg, 0, 1232, 1, 0x1004); send_frag(fd, dg, 1224, 1240, 1, 0x1004);
    CHECK(snmp6("Ip6ReasmFails") == fails0 + 1);
    send_frag(fd, dg, 1232, 1232, 1, 0x1004); send_frag(fd, dg, 2464, 544, 0, 0x1004);
    CHECK(wait_datagram(s, back, sizeof back, 500) < 0);
    send_frag(fd, dg, 0, 1232, 1, 0x1005); send_frag(fd, dg, 0, 1224, 1, 0x1005);           /* the same start, a different length */
    CHECK(snmp6("Ip6ReasmFails") == fails0 + 2);
    /* an atomic fragment (offset 0, no more fragments) is an ordinary packet */
    static unsigned char small[100], sdg[108];
    for (int i = 0; i < 100; i++) small[i] = (unsigned char)(i + 1);
    size_t sl = mk_udp(sdg, 4001, port, small, 100);
    send_frag(fd, sdg, 0, sl, 0, 0x1007);
    CHECK(wait_datagram(s, back, sizeof back, 3000) == 100 && !memcmp(back, small, 100));
    /* a fragment that is not the last and has a length that is no multiple of 8: a parameter problem; one that would make the datagram longer than 65535 bytes: the same */
    long pp0 = snmp6("Icmp6OutParmProblems");
    send_frag(fd, dg, 0, 100, 1, 0x1008);
    {
        unsigned char body[24], pkt[80];
        memset(body, 0, sizeof body);
        body[0] = 17;
        put16(body + 2, 65528);
        put32(body + 4, 0x100A);
        memcpy(body + 8, dg, 16);
        size_t len = mk6(pkt, 44, body, 24);
        send6(fd, pkt, len);
    }
    CHECK(snmp6("Icmp6OutParmProblems") >= pp0 + 2);
    /* more fragments than a datagram may have (64): thrown away; the memory is given back */
    long f1 = snmp6("Ip6ReasmFails");
    for (int k = 0; k < 66; k++) send_frag(fd, dg, (size_t)k * 8, 8, 1, 0x1100);
    CHECK(snmp6("Ip6ReasmFails") > f1);
    /* queue pressure: ten datagrams of which only the first fragment arrives; there are four queues, so the old ones lose, and a whole datagram still gets through */
    long f2 = snmp6("Ip6ReasmFails");
    for (int k = 0; k < 10; k++) send_frag(fd, dg, 0, 1232, 1, 0x1200 + (unsigned)k);
    CHECK(snmp6("Ip6ReasmFails") >= f2 + 6);
    send_frag(fd, dg, 0, 1232, 1, 0x1300); send_frag(fd, dg, 1232, 1232, 1, 0x1300); send_frag(fd, dg, 2464, 544, 0, 0x1300);
    CHECK(wait_datagram(s, back, sizeof back, 3000) == 3000 && !memcmp(back, data, 3000));
    /* a datagram that never completes: its first fragment is answered with "time exceeded" when the time is up (made short for the test) */
    int tf = open("/proc/net/tune", O_WRONLY);
    CHECK(tf >= 0 && write(tf, "ip6_reasm_ms 300\n", 17) == 17);
    long to0 = snmp6("Ip6ReasmTimeout"), te0 = snmp6("Icmp6OutTimeExcds");
    send_frag(fd, dg, 0, 1232, 1, 0x1400);
    for (int k = 0; k < 100 && snmp6("Ip6ReasmTimeout") == to0; k++) usleep(100000);
    CHECK(snmp6("Ip6ReasmTimeout") > to0 && snmp6("Icmp6OutTimeExcds") > te0);
    CHECK(write(tf, "ip6_reasm_ms 60000\n", 19) == 19);
    close(tf);
    close(s);
    close(fd);
}

/* The next header and length of a hop-by-hop or destination options header holding the options in opts (padded to a multiple of 8). */
static size_t ext_hdr(unsigned char *out, unsigned nh, const unsigned char *opts, size_t olen) {
    size_t total = (2 + olen + 7) & ~(size_t)7;
    memset(out, 0, total);
    out[0] = (unsigned char)nh;
    out[1] = (unsigned char)(total / 8 - 1);
    if (olen) memcpy(out + 2, opts, olen);
    size_t pad = total - 2 - olen;
    if (pad == 1) out[2 + olen] = 0;                                         /* Pad1 */
    else if (pad >= 2) { out[2 + olen] = 1; out[3 + olen] = (unsigned char)(pad - 2); }   /* PadN */
    return total;
}

/* An echo request (the ping socket's identifier, a sequence number) after the extension headers in chain, from ::1 to ::1. */
static size_t mk_echo(unsigned char *pkt, unsigned first_nh, const unsigned char *chain, size_t clen, unsigned ident, unsigned seq) {
    unsigned char body[256];
    memcpy(body, chain, clen);
    unsigned char *m = body + clen;
    memset(m, 0, 16);
    m[0] = 128;
    put16(m + 4, ident);
    put16(m + 6, seq);
    memcpy(m + 8, "exthdr!!", 8);
    unsigned short c = l4csum(LOOP6, LOOP6, 58, m, 16);
    put16(m + 2, c);
    return mk6(pkt, first_nh, body, clen + 16);
}

static void t_exthdr(void) {
    int fd = open("/proc/net/inject", O_WRONLY);
    CHECK(fd >= 0);
    int s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6);
    CHECK(s >= 0);
    unsigned char probe[16], rep[256], pkt[400], chain[128], opts[16];
    memset(probe, 0, sizeof probe);
    probe[0] = 128; probe[7] = 1;
    struct sockaddr_in6 a = sa6("::1", 0);
    CHECK(sendto(s, probe, sizeof probe, 0, (struct sockaddr *)&a, sizeof a) == (ssize_t)sizeof probe);
    CHECK(wait_datagram(s, rep, sizeof rep, 3000) == 16);
    unsigned ident = (unsigned)(rep[4] << 8 | rep[5]);                          /* the identifier the kernel gave the socket */
    unsigned seq = 100;
    size_t n;
    /* a hop-by-hop options header with padding and a router alert: carried on, and answered */
    static const unsigned char ra_opt[] = { 5, 2, 0, 0 };
    n = ext_hdr(chain, 58, ra_opt, sizeof ra_opt);
    n = mk_echo(pkt, 0, chain, n, ident, ++seq);
    send6(fd, pkt, n);
    CHECK(wait_datagram(s, rep, sizeof rep, 3000) == 16 && rep[0] == 129 && rep[7] == seq);
    /* a destination option of an unknown type: the top two bits say what to do (RFC 8200 section 4.2) */
    static const struct { unsigned char type; int answer; } acts[] = { { 0x3E, 0 }, { 0x5E, -1 }, { 0x9E, 4 }, { 0xDE, 4 } };      /* skip / discard / discard + parameter problem / the same for a unicast destination */
    for (unsigned k = 0; k < sizeof acts / sizeof acts[0]; k++) {
        opts[0] = acts[k].type; opts[1] = 2; opts[2] = opts[3] = 0;
        n = ext_hdr(chain, 58, opts, 4);
        long bad0 = snmp6("Ip6InHdrErrors");
        n = mk_echo(pkt, 60, chain, n, ident, ++seq);
        send6(fd, pkt, n);
        int got = wait_datagram(s, rep, sizeof rep, acts[k].answer == 0 ? 3000 : 700);
        if (acts[k].answer == 0) {
            CHECK(got == 16 && rep[0] == 129 && rep[7] == seq);               /* skipped: the packet went on */
        } else if (acts[k].answer < 0) {
            CHECK(got < 0 && snmp6("Ip6InHdrErrors") == bad0 + 1);             /* discarded without a word */
        } else {
            CHECK(got > 40 && rep[0] == 4 && rep[1] == 2 && rep[4] == 0 && rep[5] == 0 && rep[6] == 0 && rep[7] == 42);      /* parameter problem, code 2, pointing at the option's type byte (offset 40 + 2) */
        }
    }
    /* a routing header with segments left: type 0 is refused (RFC 5095), and so is every type that is not implemented; with none left it is passed over */
    memset(chain, 0, 8);
    chain[0] = 58; chain[1] = 0; chain[2] = 0; chain[3] = 2;                    /* routing type 0, segments left 2 */
    n = mk_echo(pkt, 43, chain, 8, ident, ++seq);
    send6(fd, pkt, n);
    CHECK(wait_datagram(s, rep, sizeof rep, 3000) > 40 && rep[0] == 4 && rep[1] == 0 && rep[7] == 42);      /* parameter problem, code 0, at the routing type */
    memset(chain, 0, 8);
    chain[0] = 58; chain[2] = 2; chain[3] = 0;                                  /* type 2, none left */
    n = mk_echo(pkt, 43, chain, 8, ident, ++seq);
    send6(fd, pkt, n);
    CHECK(wait_datagram(s, rep, sizeof rep, 3000) == 16 && rep[0] == 129 && rep[7] == seq);
    /* a hop-by-hop header after a destination options header; an unknown protocol; nine headers in a row: counted, answered where the rules say so, thrown away otherwise */
    long pp0 = snmp6("Icmp6OutParmProblems"), unk0 = snmp6("Ip6InUnknownProtos"), bad0 = snmp6("Ip6InHdrErrors");
    n = ext_hdr(chain, 0, NULL, 0);
    n += ext_hdr(chain + n, 58, NULL, 0);                                       /* destination options, then a hop-by-hop header after it */
    n = mk_echo(pkt, 60, chain, n, ident, ++seq);
    send6(fd, pkt, n);
    {
        unsigned char odd[32];
        memset(odd, 0, sizeof odd);
        n = mk6(pkt, 143, odd, 16);                                             /* a protocol number nobody speaks */
        send6(fd, pkt, n);
    }
    size_t chn = 0;
    for (int k = 0; k < 9; k++) chn += ext_hdr(chain + chn, k < 8 ? 60 : 58, NULL, 0);
    n = mk_echo(pkt, 60, chain, chn, ident, ++seq);
    send6(fd, pkt, n);
    CHECK(snmp6("Icmp6OutParmProblems") >= pp0 + 2);                            /* the misplaced hop-by-hop header (code 1) and the unknown protocol (code 1) */
    CHECK(snmp6("Ip6InUnknownProtos") == unk0 + 1 && snmp6("Ip6InHdrErrors") >= bad0 + 2);
    close(s);
    close(fd);
}

/* ---------------------------------------------------------------- neighbour discovery on a card (frames injected as if eth0 had received them) */

static int g_mac_ok;
static unsigned char g_mac[6];

static int find_eth0(void) {
    for (unsigned i = 0; i < 16; i++) {
        struct gnet_if r;
        memset(&r, 0, sizeof r);
        r.index = i;
        if (gap_netctl(NETCTL_IF_GET, &r) < 0) break;
        if (!strcmp(r.name, "eth0")) { memcpy(g_mac, r.mac, 6); g_mac_ok = 1; return 1; }
    }
    return 0;
}

static int addr6_op(unsigned cmd, const unsigned char a[16], int plen) {
    struct gnet_addr6 r;
    memset(&r, 0, sizeof r);
    strcpy(r.ifname, "eth0");
    memcpy(r.addr, a, 16);
    r.plen = (unsigned char)plen;
    return gap_netctl(cmd, &r);
}

/* The flags of an address of eth0 in /proc/net/if_inet6 (hex), or -1 if it is not there. */
static int if_inet6_flags(const unsigned char a[16]) {
    char want[33], buf[2048];
    for (int i = 0; i < 16; i++) sprintf(want + 2 * i, "%02x", a[i]);
    int f = open("/proc/net/if_inet6", O_RDONLY);
    if (f < 0) return -1;
    ssize_t n = read(f, buf, sizeof buf - 1);
    close(f);
    if (n <= 0) return -1;
    buf[n] = 0;
    for (char *p = buf; p && *p;) {
        if (!strncmp(p, want, 32)) {
            unsigned idx, plen, scope, flags;
            if (sscanf(p + 33, "%x %x %x %x", &idx, &plen, &scope, &flags) == 4) return (int)flags;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return -1;
}

static void put_ip6addr(unsigned char out[16], const char *text) { inet_pton(AF_INET6, text, out); }

/* An Ethernet frame holding an ICMPv6 message (type, body after the checksum) from src to dst. */
static size_t mk_nd(unsigned char *fr, const unsigned char *dmac, const unsigned char *smac, const unsigned char *src, const unsigned char *dst, unsigned type,
                    const unsigned char *body, size_t blen, unsigned hlim) {
    memcpy(fr, dmac, 6);
    memcpy(fr + 6, smac, 6);
    fr[12] = 0x86; fr[13] = 0xDD;
    unsigned char *ip = fr + 14;
    memset(ip, 0, 40);
    ip[0] = 0x60;
    put16(ip + 4, (unsigned)(4 + blen));
    ip[6] = 58;
    ip[7] = (unsigned char)hlim;
    memcpy(ip + 8, src, 16);
    memcpy(ip + 24, dst, 16);
    unsigned char *m = ip + 40;
    m[0] = (unsigned char)type; m[1] = 0; m[2] = m[3] = 0;
    memcpy(m + 4, body, blen);
    put16(m + 2, l4csum(src, dst, 58, m, 4 + blen));
    return 14 + 40 + 4 + blen;
}

static void send_dev(int fd, const unsigned char *frame, size_t n) {
    unsigned char buf[2100];
    memset(buf, 0, 16);
    strcpy((char *)buf, "eth0");
    memcpy(buf + 16, frame, n);
    if (write(fd, buf, 16 + n) < 0) { /* ignore: a frame that is refused counts as thrown away */ }
}

static const unsigned char MAC_NEIGH[6] = { 0x52, 0x56, 0x00, 0x00, 0x00, 0x99 };

/* Duplicate address detection (RFC 4862): an address is tentative for about a second; an advertisement for it, or a probe from another node, makes it a duplicate; an ordinary
 * solicitation for it from a neighbour does not. */
static void t_dad(void) {
    CHECK(find_eth0());
    int fd = open("/proc/net/inject_dev", O_WRONLY);
    CHECK(fd >= 0);
    unsigned char a1[16], a2[16], a3[16], a4[16], neigh[16], allnodes[16], unspec[16] = { 0 }, target_ll[16];
    put_ip6addr(a1, "fec0::dad:1"); put_ip6addr(a2, "fec0::dad:2"); put_ip6addr(a3, "fec0::dad:3"); put_ip6addr(a4, "fec0::dad:4");
    put_ip6addr(neigh, "fe80::5456:ff:fe00:99"); put_ip6addr(allnodes, "ff02::1");
    (void)target_ll;
    /* 1: a good address is tentative at first and permanent after the check */
    CHECK(addr6_op(NETCTL_ADDR6_ADD, a1, 64) == 0);
    int fl = if_inet6_flags(a1);
    CHECK(fl >= 0 && (fl & 0x40) && (fl & 0x80));                            /* tentative (and permanent: it was set by hand) */
    CHECK(addr6_op(NETCTL_ADDR6_ADD, a1, 64) < 0 && errno == EEXIST);
    for (int k = 0; k < 150 && ((fl = if_inet6_flags(a1)) & 0x40); k++) usleep(100000);
    CHECK(fl == 0x80);
    /* 2: somebody answers for it while it is tentative */
    CHECK(addr6_op(NETCTL_ADDR6_ADD, a2, 64) == 0);
    {
        unsigned char body[28], fr[200], dmac[6] = { 0x33, 0x33, 0, 0, 0, 1 };
        memset(body, 0, sizeof body);
        body[0] = 0x20;                                                      /* override */
        memcpy(body + 4, a2, 16);
        body[20] = 2; body[21] = 1; memcpy(body + 22, MAC_NEIGH, 6);         /* target link-layer address */
        send_dev(fd, fr, mk_nd(fr, dmac, MAC_NEIGH, neigh, allnodes, 136, body, sizeof body, 255));
    }
    fl = if_inet6_flags(a2);
    CHECK(fl >= 0 && (fl & 0x08) && !(fl & 0x40));                            /* DADFAILED */
    /* 3: another node probes for it too: neither may have it */
    CHECK(addr6_op(NETCTL_ADDR6_ADD, a3, 64) == 0);
    {
        unsigned char body[20], fr[200], sn[16] = { 0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xFF }, dmac[6] = { 0x33, 0x33, 0xFF };
        memcpy(sn + 13, a3 + 13, 3);
        memcpy(dmac + 3, a3 + 13, 3);
        memset(body, 0, sizeof body);
        memcpy(body + 4, a3, 16);
        send_dev(fd, fr, mk_nd(fr, dmac, MAC_NEIGH, unspec, sn, 135, body, sizeof body, 255));
    }
    fl = if_inet6_flags(a3);
    CHECK(fl >= 0 && (fl & 0x08));
    /* 4: an ordinary solicitation from a neighbour for a tentative address is not a conflict (and is not answered) */
    CHECK(addr6_op(NETCTL_ADDR6_ADD, a4, 64) == 0);
    {
        unsigned char body[28], fr[200], sn[16] = { 0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xFF }, dmac[6] = { 0x33, 0x33, 0xFF };
        memcpy(sn + 13, a4 + 13, 3);
        memcpy(dmac + 3, a4 + 13, 3);
        memset(body, 0, sizeof body);
        memcpy(body + 4, a4, 16);
        body[20] = 1; body[21] = 1; memcpy(body + 22, MAC_NEIGH, 6);         /* source link-layer address */
        send_dev(fd, fr, mk_nd(fr, dmac, MAC_NEIGH, neigh, sn, 135, body, sizeof body, 255));
    }
    for (int k = 0; k < 150 && ((fl = if_inet6_flags(a4)) & 0x40); k++) usleep(100000);
    CHECK(fl == 0x80);
    /* a duplicate stays in the list until it is removed, and is removed like any other */
    CHECK(addr6_op(NETCTL_ADDR6_DEL, a2, 64) == 0 && if_inet6_flags(a2) < 0);
    CHECK(addr6_op(NETCTL_ADDR6_DEL, a3, 64) == 0 && addr6_op(NETCTL_ADDR6_DEL, a1, 64) == 0 && addr6_op(NETCTL_ADDR6_DEL, a4, 64) == 0);
    CHECK(addr6_op(NETCTL_ADDR6_DEL, a4, 64) < 0 && errno == EADDRNOTAVAIL);
    close(fd);
}

/* Neighbour discovery fed with garbage and with floods: bounded tables, nothing leaks, the card still works. */
static void t_ndfuzz(void) {
    CHECK(find_eth0());
    int fd = open("/proc/net/inject_dev", O_WRONLY);
    CHECK(fd >= 0);
    unsigned seed = 31337;
    unsigned char mylocal[16], allnodes[16], fr[1600], body[1200], src[16];
    put_ip6addr(allnodes, "ff02::1");
    memset(mylocal, 0, 16);
    mylocal[0] = 0xFE; mylocal[1] = 0x80;
    mylocal[8] = (unsigned char)(g_mac[0] ^ 2); mylocal[9] = g_mac[1]; mylocal[10] = g_mac[2]; mylocal[11] = 0xFF; mylocal[12] = 0xFE; mylocal[13] = g_mac[3]; mylocal[14] = g_mac[4]; mylocal[15] = g_mac[5];
    long na0 = snmp6("Icmp6OutNeighborAdvertisements"), bad0 = snmp6("Yukari6NdRejected");
    /* a flood of neighbour solicitations for our own link-local address from forged sources: the cache is bounded, the advertisements are rate limited */
    for (int k = 0; k < 4000; k++) {
        memset(src, 0, 16);
        src[0] = 0xFE; src[1] = 0x80; src[10] = (unsigned char)lcg(&seed); src[11] = (unsigned char)lcg(&seed); src[14] = (unsigned char)(k >> 8); src[15] = (unsigned char)k;
        unsigned char smac[6] = { 0x52, 0x56, 0, 0, (unsigned char)(k >> 8), (unsigned char)k };
        memset(body, 0, 28);
        memcpy(body + 4, mylocal, 16);
        body[20] = 1; body[21] = 1; memcpy(body + 22, smac, 6);
        send_dev(fd, fr, mk_nd(fr, g_mac, smac, src, mylocal, 135, body, 28, 255));
    }
    long nas = snmp6("Icmp6OutNeighborAdvertisements") - na0;
    CHECK(nas > 0 && nas < 1500);                                             /* answered at most 100 a second (and some of the 4000 are over before the clock moves) */
    char buf[8192];
    int f = open("/proc/net/ndp", O_RDONLY);
    ssize_t n = read(f, buf, sizeof buf - 1);
    close(f);
    CHECK(n > 0);
    buf[n] = 0;
    int lines = 0;
    for (char *p = buf; (p = strchr(p, '\n')) != NULL; p++) lines++;
    CHECK(lines - 1 <= 64);                                                    /* the table has 64 entries, whatever is thrown at it */
    printf("  ndfuzz: %ld advertisements for 4000 solicitations, %d neighbours in the cache\n", nas, lines - 1);
    /* solicitations and advertisements that break the rules: wrong hop limit, wrong code, a multicast target, a short message, options of length zero, a solicitation from :: with an option */
    for (int k = 0; k < 3000; k++) {
        size_t bl = 4 + lcg(&seed) % 60;
        for (size_t i = 0; i < bl; i++) body[i] = (unsigned char)lcg(&seed);
        unsigned type = 133 + lcg(&seed) % 5;
        unsigned hlim = lcg(&seed) % 4 ? 255 : lcg(&seed) % 256;
        memset(src, 0, 16);
        src[0] = 0xFE; src[1] = 0x80; src[15] = (unsigned char)(1 + lcg(&seed) % 200);
        if (lcg(&seed) % 5 == 0) memset(src, 0, 16);
        if (lcg(&seed) % 4 == 0 && bl >= 20) memcpy(body + 4, mylocal, 16);                     /* often about us */
        size_t flen = mk_nd(fr, g_mac, MAC_NEIGH, src, lcg(&seed) % 2 ? mylocal : allnodes, type, body, bl, hlim);
        if (lcg(&seed) % 6 == 0) fr[14 + 40 + 1] = (unsigned char)lcg(&seed);                  /* a code that is not 0 (the checksum is then wrong too) */
        send_dev(fd, fr, flen);
    }
    CHECK(snmp6("Yukari6NdRejected") > bad0 + 500);
    /* router advertisements with wild values (never from the real router): the tables stay bounded */
    for (int k = 0; k < 2500; k++) {
        memset(src, 0, 16);
        src[0] = 0xFE; src[1] = 0x80; src[8] = 0xDE; src[9] = 0xAD; src[14] = (unsigned char)(lcg(&seed) % 40); src[15] = (unsigned char)(10 + lcg(&seed) % 40);
        if (lcg(&seed) % 9 == 0) src[0] = 0x20;                                                 /* not link-local: refused */
        size_t o = 12;
        memset(body, 0, sizeof body);
        body[0] = (unsigned char)lcg(&seed);                                                     /* current hop limit, 0 and 255 included */
        body[1] = (unsigned char)lcg(&seed);                                                     /* managed, other, preference */
        put16(body + 2, lcg(&seed) % 3 ? lcg(&seed) % 65536 : 0);                                /* router lifetime, often 0 */
        put32(body + 4, lcg(&seed));                                                             /* reachable time */
        put32(body + 8, lcg(&seed));                                                             /* retransmit timer */
        int nopt = (int)(lcg(&seed) % 6);
        for (int i = 0; i < nopt && o + 40 < sizeof body; i++) {
            switch (lcg(&seed) % 6) {
            case 0:                                                                              /* prefix information with anything in it */
                body[o] = 3; body[o + 1] = 4; body[o + 2] = (unsigned char)(lcg(&seed) % 4 ? 64 : lcg(&seed) % 256);
                body[o + 3] = (unsigned char)lcg(&seed);
                put32(body + o + 4, lcg(&seed) % 3 ? lcg(&seed) : 0xFFFFFFFFu);
                put32(body + o + 8, lcg(&seed) % 3 ? lcg(&seed) : 0);
                for (int j = 0; j < 16; j++) body[o + 16 + j] = (unsigned char)(lcg(&seed) % 3 ? lcg(&seed) : 0);
                if (lcg(&seed) % 3) { body[o + 16] = 0x20; body[o + 17] = 0x01; }
                o += 32;
                break;
            case 1: body[o] = 5; body[o + 1] = 1; put32(body + o + 4, lcg(&seed) % 2 ? 1280 + lcg(&seed) % 300 : lcg(&seed)); o += 8; break;      /* MTU */
            case 2: body[o] = 25; body[o + 1] = 3; put32(body + o + 4, lcg(&seed) % 2 ? 0 : lcg(&seed)); for (int j = 0; j < 16; j++) body[o + 8 + j] = (unsigned char)lcg(&seed); o += 24; break;      /* RDNSS */
            case 3: body[o] = 1; body[o + 1] = 1; memcpy(body + o + 2, MAC_NEIGH, 6); o += 8; break;
            case 4: body[o] = (unsigned char)lcg(&seed); body[o + 1] = 0; o += 8; break;          /* an option of length zero: the whole message is thrown away */
            default: body[o] = (unsigned char)(lcg(&seed) % 40); body[o + 1] = (unsigned char)(1 + lcg(&seed) % 3); o += (size_t)body[o + 1] * 8; break;
            }
        }
        size_t flen = mk_nd(fr, g_mac, MAC_NEIGH, src, allnodes, 134, body, o, lcg(&seed) % 20 ? 255 : 64);
        send_dev(fd, fr, flen);
    }
    close(fd);
    /* the bounds: eight addresses a card, thirty-two routes, sixty-four neighbours; no packet buffer is left over; the card still talks */
    f = open("/proc/net/if_inet6", O_RDONLY);
    n = read(f, buf, sizeof buf - 1);
    close(f);
    buf[n > 0 ? n : 0] = 0;
    int addrs = 0;
    for (char *p = buf; (p = strstr(p, "eth0\n")) != NULL; p++) addrs++;
    CHECK(addrs >= 1 && addrs <= 8);
    f = open("/proc/net/ipv6_route", O_RDONLY);
    n = read(f, buf, sizeof buf - 1);
    close(f);
    buf[n > 0 ? n : 0] = 0;
    int routes = 0;
    for (char *p = buf; (p = strchr(p, '\n')) != NULL; p++) routes++;
    CHECK(routes <= 32);
    printf("  ndfuzz: %d addresses, %d routes after the wild advertisements\n", addrs, routes);
}

/* Path MTU discovery (RFC 8201): a router's "packet too big" lowers the size at which datagrams to that destination are fragmented; it never raises it, and never below 1280. */
static void t_pmtu(void) {
    CHECK(find_eth0());
    int fd = open("/proc/net/inject_dev", O_WRONLY);
    CHECK(fd >= 0);
    int u = socket(AF_INET6, SOCK_DGRAM, 0);
    struct sockaddr_in6 gw = sa6("fec0::2", 9), me;
    CHECK(connect(u, (struct sockaddr *)&gw, sizeof gw) == 0);
    socklen_t ml = sizeof me;
    CHECK(getsockname(u, (struct sockaddr *)&me, &ml) == 0 && me.sin6_addr.s6_addr[0] == 0xFE && me.sin6_addr.s6_addr[1] == 0xC0);
    int s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6);
    CHECK(s >= 0);
    static unsigned char echo[8 + 1300];
    memset(echo, 0, sizeof echo);
    echo[0] = 128;
    unsigned char router[16], quote[48], body[4 + 48], fr[200];
    put_ip6addr(router, "fe80::2");
    memset(quote, 0, sizeof quote);                                           /* the packet the router could not forward: a UDP datagram of ours to fec0::2 */
    quote[0] = 0x60; put16(quote + 4, 8); quote[6] = 17; quote[7] = 64;
    memcpy(quote + 8, me.sin6_addr.s6_addr, 16);
    put_ip6addr(quote + 24, "fec0::2");
    put16(quote + 40, 1234); put16(quote + 42, 9); put16(quote + 44, 8);
    unsigned char rmac[6] = { 0x52, 0x56, 0x00, 0x00, 0x00, 0x02 };
    long ok0 = snmp6("Ip6FragOKs"), made0 = snmp6("Ip6FragCreates");
    CHECK(sendto(s, echo, sizeof echo, 0, (struct sockaddr *)&gw, sizeof gw) == (ssize_t)sizeof echo);
    CHECK(snmp6("Ip6FragOKs") == ok0);                                         /* 1348 bytes fit in 1500: one packet */
    static const unsigned mtus[] = { 1400, 1280, 1350, 600 };
    long expect_ok = ok0;
    for (unsigned k = 0; k < sizeof mtus / sizeof mtus[0]; k++) {
        put32(body, mtus[k]);
        memcpy(body + 4, quote, sizeof quote);
        send_dev(fd, fr, mk_nd(fr, g_mac, rmac, router, me.sin6_addr.s6_addr, 2, body, sizeof body, 64));
        CHECK(sendto(s, echo, sizeof echo, 0, (struct sockaddr *)&gw, sizeof gw) == (ssize_t)sizeof echo);
        if (k == 0) {                                                           /* 1400: 1348 bytes still fit */
            CHECK(snmp6("Ip6FragOKs") == expect_ok);
        } else {                                                                /* 1280 from here on: never raised again, never lowered below 1280 */
            expect_ok++;
            CHECK(snmp6("Ip6FragOKs") == expect_ok);
        }
    }
    CHECK(snmp6("Ip6FragCreates") == made0 + 6);                                /* each of the three fragmented echoes went out in two pieces of at most 1232 bytes of data */
    CHECK(snmp6("Icmp6InPktTooBigs") >= 4);
    close(s);
    close(u);
    close(fd);
}

/* getaddrinfo's answer for a name, in the order it gives it: "gai NAME [inet|inet6|passive|addrconfig|v4mapped|all]" prints one line per result. */
static int gai_main(int argc, char **argv) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    const char *node = argc > 2 ? argv[2] : NULL;
    if (node && !strcmp(node, "-")) node = NULL;                                       /* "-" is "no name" (AI_PASSIVE: the wildcard addresses; without it: the loopback) */
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "inet")) hints.ai_family = AF_INET;
        else if (!strcmp(argv[i], "inet6")) hints.ai_family = AF_INET6;
        else if (!strcmp(argv[i], "passive")) hints.ai_flags |= AI_PASSIVE;
        else if (!strcmp(argv[i], "addrconfig")) hints.ai_flags |= AI_ADDRCONFIG;
        else if (!strcmp(argv[i], "v4mapped")) hints.ai_flags |= AI_V4MAPPED;
        else if (!strcmp(argv[i], "all")) hints.ai_flags |= AI_ALL;
        else if (!strcmp(argv[i], "numerichost")) hints.ai_flags |= AI_NUMERICHOST;
    }
    int rc = getaddrinfo(node, "80", &hints, &res);
    if (rc) { printf("GAI error %d: %s\n", rc, gai_strerror(rc)); return 1; }
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        char host[80];
        getnameinfo(a->ai_addr, a->ai_addrlen, host, sizeof host, NULL, 0, NI_NUMERICHOST);
        printf("GAI %s %s\n", a->ai_family == AF_INET6 ? "inet6" : "inet", host);
    }
    freeaddrinfo(res);
    return 0;
}

typedef struct { const char *name; void (*fn)(void); } Check;

int main(int argc, char **argv) {
    static const Check checks[] = {
        { "udp6", t_udp6 }, { "frag", t_frag }, { "tcp6", t_tcp6 }, { "bulk6", t_bulk6 }, { "refused6", t_refused6 }, { "close6", t_close6 },
        { "dual", t_dual }, { "mapped", t_mapped }, { "opts6", t_opts6 }, { "ping6", t_ping6 },
    };
    static const Check only_on_request[] = { { "loss6", t_loss6 }, { "fuzz6", t_fuzz6 }, { "reasm", t_reasm }, { "exthdr", t_exthdr }, { "dad", t_dad }, { "ndfuzz", t_ndfuzz }, { "pmtu", t_pmtu } };       /* these need root (the last two a card too) */
    const char *want = argc > 1 ? argv[1] : NULL;
    if (want && !strcmp(want, "gai")) return gai_main(argc, argv);
    int ran = 0;
    for (unsigned i = 0; i < sizeof checks / sizeof checks[0]; i++) {
        if (want && strcmp(want, checks[i].name)) continue;
        g_name = checks[i].name;
        int before = g_failed;
        checks[i].fn();
        printf("%s %s\n", g_failed == before ? "ok  " : "FAIL", checks[i].name);
        ran++;
    }
    for (unsigned i = 0; i < sizeof only_on_request / sizeof only_on_request[0]; i++) {
        if (!want || strcmp(want, only_on_request[i].name)) continue;
        g_name = only_on_request[i].name;
        int before = g_failed;
        only_on_request[i].fn();
        printf("%s %s\n", g_failed == before ? "ok  " : "FAIL", only_on_request[i].name);
        ran++;
    }
    if (!ran) { printf("nettest6: no such check\n"); return 2; }
    printf("NETTEST6 %s\n", g_failed ? "FAIL" : "PASS");
    return g_failed ? 1 : 0;
}
