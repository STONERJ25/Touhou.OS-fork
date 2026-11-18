/*
 * IPv6 through a statically linked C library, over ::1 only (no name server, no network card): a TCP echo with the address calls (sockaddr_in6, getsockname, getpeername,
 * accept's address), the IPV6_UNICAST_HOPS and IPV6_V6ONLY options with their limits, a dual-stack socket bound to :: that takes an IPv4 client with a v4-mapped peer address
 * (and one with IPV6_V6ONLY that refuses it), a socket that connects to ::ffff:127.0.0.1, UDP with its addresses and the "connection refused" of a closed port, the errors
 * of a wrong address family or length, numeric getaddrinfo and getnameinfo, and the text forms of inet_pton and inet_ntop.
 * Prints only what the manual pages promise (no port numbers, no timings), so a real Linux prints the same lines (tools/linux-oracle.py); tests/case_linux_net.py compares.
 * Every call is made in a statement of its own, and its result and errno are read before anything is printed (the order in which a call's arguments are evaluated is not defined).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

/* a call that is expected to fail: what it returned and the errno it left, next to the errno that the manual page promises */
#define FAILS(what, call, ename)                                                                         \
    do {                                                                                                 \
        errno = 0;                                                                                       \
        long r_ = (long)(call);                                                                          \
        int e_ = errno;                                                                                  \
        printf("%s: %ld, errno %d (%s %d)\n", what, r_, e_, #ename, ename);                              \
    } while (0)

static void say_addr(const char *what, const struct sockaddr_in6 *a, socklen_t len) {
    char text[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &a->sin6_addr, text, sizeof text);
    printf("%s: family %d, address %s, port is set %d, flowinfo %u, scope id %u, length %u\n", what, a->sin6_family, text, ntohs(a->sin6_port) != 0, a->sin6_flowinfo, a->sin6_scope_id, (unsigned)len);
}

static struct sockaddr_in6 make6(const char *text, unsigned port) {
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof a);
    a.sin6_family = AF_INET6;
    a.sin6_port = htons((unsigned short)port);
    inet_pton(AF_INET6, text, &a.sin6_addr);
    return a;
}

static struct sockaddr_in make4(const char *text, unsigned port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, text, &a.sin_addr);
    return a;
}

static int get_int(int fd, int level, int name) {
    int v = -12345;
    socklen_t n = sizeof v;
    if (getsockopt(fd, level, name, &v, &n) < 0) return -99999;
    return v;
}

static int set_int(int fd, int level, int name, int v) { return setsockopt(fd, level, name, &v, sizeof v); }

static int bound6(int type, const char *addr, int v6only, unsigned *port) {
    int fd = socket(AF_INET6, type, 0);
    if (fd < 0) return -1;
    if (v6only >= 0) set_int(fd, IPPROTO_IPV6, IPV6_V6ONLY, v6only);
    struct sockaddr_in6 a = make6(addr, 0);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    socklen_t len = sizeof a;
    getsockname(fd, (struct sockaddr *)&a, &len);
    *port = ntohs(a.sin6_port);
    return fd;
}

int main(void) {
    unsigned port;
    int r, g;
    char buf[64];
    ssize_t n;

    /* --- a TCP echo over ::1 */
    int l = socket(AF_INET6, SOCK_STREAM, 0);
    printf("socket: %s\n", l >= 0 ? "ok" : "failed");
    {
        int t = get_int(l, SOL_SOCKET, SO_TYPE), d = get_int(l, SOL_SOCKET, SO_DOMAIN), p = get_int(l, SOL_SOCKET, SO_PROTOCOL);
        printf("SO_TYPE: %d, SO_DOMAIN: %d, SO_PROTOCOL: %d\n", t, d, p);
    }
    g = get_int(l, IPPROTO_IPV6, IPV6_V6ONLY);
    printf("IPV6_V6ONLY by default: %d\n", g);
    set_int(l, SOL_SOCKET, SO_REUSEADDR, 1);
    struct sockaddr_in6 a = make6("::1", 0);
    r = bind(l, (struct sockaddr *)&a, sizeof a);
    printf("bind to ::1 and a free port: %d\n", r);
    socklen_t len = sizeof a;
    memset(&a, 0, sizeof a);
    r = getsockname(l, (struct sockaddr *)&a, &len);
    printf("getsockname: %d\n", r);
    say_addr("the listener", &a, len);
    port = ntohs(a.sin6_port);
    r = listen(l, 5);
    printf("listen: %d\n", r);
    int c = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 to = make6("::1", port);
    r = connect(c, (struct sockaddr *)&to, sizeof to);
    printf("connect: %d\n", r);
    struct sockaddr_in6 peer;
    len = sizeof peer;
    memset(&peer, 0xAA, sizeof peer);
    int s = accept(l, (struct sockaddr *)&peer, &len);
    printf("accept: %s\n", s >= 0 ? "ok" : "failed");
    say_addr("accept's peer address", &peer, len);
    struct sockaddr_in6 mine, theirs;
    len = sizeof mine;
    getsockname(c, (struct sockaddr *)&mine, &len);
    say_addr("the client's own address", &mine, len);
    len = sizeof theirs;
    getpeername(c, (struct sockaddr *)&theirs, &len);
    say_addr("the client's peer address", &theirs, len);
    printf("the peer port is the listener's: %d, accept's peer port is the client's own port: %d\n", ntohs(theirs.sin6_port) == port, peer.sin6_port == mine.sin6_port);
    n = write(c, "hello over IPv6", 15);
    printf("write: %d\n", (int)n);
    n = read(s, buf, sizeof buf);
    printf("read: %d '%.*s'\n", (int)n, (int)(n > 0 ? n : 0), buf);
    n = write(s, "and back", 8);
    printf("write back: %d\n", (int)n);
    n = read(c, buf, sizeof buf);
    printf("read: %d '%.*s'\n", (int)n, (int)(n > 0 ? n : 0), buf);
    shutdown(c, SHUT_WR);
    n = read(s, buf, sizeof buf);
    printf("end of file after shutdown(SHUT_WR): %d\n", (int)n);
    close(s);
    close(c);
    close(l);

    /* --- the options and their limits */
    int u = socket(AF_INET6, SOCK_DGRAM, 0);
    g = get_int(u, IPPROTO_IPV6, IPV6_UNICAST_HOPS);
    printf("IPV6_UNICAST_HOPS by default: %d\n", g);
    r = set_int(u, IPPROTO_IPV6, IPV6_UNICAST_HOPS, 17);
    g = get_int(u, IPPROTO_IPV6, IPV6_UNICAST_HOPS);
    printf("set 17: %d, read back %d\n", r, g);
    FAILS("set 256", set_int(u, IPPROTO_IPV6, IPV6_UNICAST_HOPS, 256), EINVAL);
    r = set_int(u, IPPROTO_IPV6, IPV6_UNICAST_HOPS, -1);
    g = get_int(u, IPPROTO_IPV6, IPV6_UNICAST_HOPS);
    printf("set -1 (the default again): %d, read back %d\n", r, g);
    r = set_int(u, IPPROTO_IPV6, IPV6_RECVPKTINFO, 1);
    g = get_int(u, IPPROTO_IPV6, IPV6_RECVPKTINFO);
    printf("IPV6_RECVPKTINFO set: %d, read back %d\n", r, g);
    r = set_int(u, IPPROTO_IPV6, IPV6_V6ONLY, 1);
    g = get_int(u, IPPROTO_IPV6, IPV6_V6ONLY);
    printf("IPV6_V6ONLY set before bind: %d, read back %d\n", r, g);
    a = make6("::", 0);
    r = bind(u, (struct sockaddr *)&a, sizeof a);
    printf("bind to :: %d\n", r);
    FAILS("IPV6_V6ONLY set after bind", set_int(u, IPPROTO_IPV6, IPV6_V6ONLY, 0), EINVAL);
    close(u);

    /* --- a dual-stack listener takes an IPv4 client, with a v4-mapped peer address; with IPV6_V6ONLY it does not */
    l = bound6(SOCK_STREAM, "::", -1, &port);
    printf("listener on :: (dual-stack): %s\n", l >= 0 ? "ok" : "failed");
    listen(l, 5);
    struct sockaddr_in to4 = make4("127.0.0.1", port);
    c = socket(AF_INET, SOCK_STREAM, 0);
    r = connect(c, (struct sockaddr *)&to4, sizeof to4);
    printf("an IPv4 client connects: %d\n", r);
    len = sizeof peer;
    s = accept(l, (struct sockaddr *)&peer, &len);
    say_addr("the IPv4 client as accept shows it", &peer, len);
    len = sizeof mine;
    getsockname(s, (struct sockaddr *)&mine, &len);
    say_addr("the accepted socket's own address", &mine, len);
    n = write(c, "ipv4", 4);
    printf("write: %d\n", (int)n);
    n = read(s, buf, sizeof buf);
    printf("read: %d '%.*s'\n", (int)n, (int)(n > 0 ? n : 0), buf);
    close(s);
    close(c);
    to = make6("::1", port);
    c = socket(AF_INET6, SOCK_STREAM, 0);
    r = connect(c, (struct sockaddr *)&to, sizeof to);
    printf("an IPv6 client connects: %d\n", r);
    len = sizeof peer;
    s = accept(l, (struct sockaddr *)&peer, &len);
    say_addr("the IPv6 client as accept shows it", &peer, len);
    close(s);
    close(c);
    close(l);
    l = bound6(SOCK_STREAM, "::", 1, &port);
    listen(l, 5);
    to4 = make4("127.0.0.1", port);
    c = socket(AF_INET, SOCK_STREAM, 0);
    FAILS("an IPv4 client of a V6ONLY listener", connect(c, (struct sockaddr *)&to4, sizeof to4), ECONNREFUSED);
    close(c);
    close(l);

    /* --- an IPv6 socket that connects to a v4-mapped address talks IPv4 */
    int l4 = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a4 = make4("127.0.0.1", 0);
    bind(l4, (struct sockaddr *)&a4, sizeof a4);
    len = sizeof a4;
    getsockname(l4, (struct sockaddr *)&a4, &len);
    listen(l4, 5);
    c = socket(AF_INET6, SOCK_STREAM, 0);
    to = make6("::ffff:127.0.0.1", ntohs(a4.sin_port));
    r = connect(c, (struct sockaddr *)&to, sizeof to);
    printf("connect to ::ffff:127.0.0.1: %d\n", r);
    struct sockaddr_in peer4;
    len = sizeof peer4;
    s = accept(l4, (struct sockaddr *)&peer4, &len);
    printf("the IPv4 server sees family %d, address 127.0.0.1: %d, length %u\n", peer4.sin_family, peer4.sin_addr.s_addr == htonl(INADDR_LOOPBACK), (unsigned)len);
    len = sizeof mine;
    getsockname(c, (struct sockaddr *)&mine, &len);
    say_addr("the client's own address", &mine, len);
    len = sizeof theirs;
    getpeername(c, (struct sockaddr *)&theirs, &len);
    say_addr("the client's peer address", &theirs, len);
    close(s);
    close(c);
    close(l4);
    c = socket(AF_INET6, SOCK_STREAM, 0);
    set_int(c, IPPROTO_IPV6, IPV6_V6ONLY, 1);
    to = make6("::ffff:127.0.0.1", 9);
    FAILS("a V6ONLY socket to a v4-mapped address", connect(c, (struct sockaddr *)&to, sizeof to), ENETUNREACH);
    close(c);

    /* --- UDP */
    int us = bound6(SOCK_DGRAM, "::1", -1, &port), uc = socket(AF_INET6, SOCK_DGRAM, 0);
    to = make6("::1", port);
    n = sendto(uc, "datagram", 8, 0, (struct sockaddr *)&to, sizeof to);
    printf("sendto: %d\n", (int)n);
    struct sockaddr_in6 from;
    len = sizeof from;
    n = recvfrom(us, buf, sizeof buf, 0, (struct sockaddr *)&from, &len);
    printf("recvfrom: %d '%.*s'\n", (int)n, (int)(n > 0 ? n : 0), buf);
    say_addr("the sender", &from, len);
    n = sendto(us, "answer", 6, 0, (struct sockaddr *)&from, len);
    printf("answer: %d\n", (int)n);
    n = recv(uc, buf, sizeof buf, 0);
    printf("recv: %d '%.*s'\n", (int)n, (int)(n > 0 ? n : 0), buf);
    n = sendto(uc, "0123456789", 10, 0, (struct sockaddr *)&to, sizeof to);
    r = (int)recvfrom(us, buf, 4, MSG_TRUNC, NULL, NULL);
    printf("MSG_TRUNC with a short buffer: sent %d, recvfrom says %d\n", (int)n, r);
    close(us);
    r = connect(uc, (struct sockaddr *)&to, sizeof to);
    printf("connect: %d\n", r);
    n = send(uc, "x", 1, 0);
    printf("send to the closed port: %d\n", (int)n);
    struct pollfd p = { uc, POLLIN, 0 };
    poll(&p, 1, 3000);
    FAILS("recv after the refusal", recv(uc, buf, sizeof buf, 0), ECONNREFUSED);
    close(uc);

    /* --- wrong addresses */
    int w = socket(AF_INET6, SOCK_DGRAM, 0);
    a4 = make4("127.0.0.1", 0);
    FAILS("bind of a sockaddr_in to an IPv6 socket", bind(w, (struct sockaddr *)&a4, sizeof a4), EINVAL);
    a = make6("::1", 0);
    FAILS("bind with a length that is too short", bind(w, (struct sockaddr *)&a, 10), EINVAL);
    a.sin6_family = AF_INET;
    FAILS("bind with another family", bind(w, (struct sockaddr *)&a, sizeof a), EAFNOSUPPORT);
    a = make6("2001:db8::1", 0);
    FAILS("bind to an address that is not ours", bind(w, (struct sockaddr *)&a, sizeof a), EADDRNOTAVAIL);
    a = make6("::1", 0);
    bind(w, (struct sockaddr *)&a, sizeof a);
    int w2 = socket(AF_INET6, SOCK_DGRAM, 0);
    len = sizeof a;
    getsockname(w, (struct sockaddr *)&a, &len);
    FAILS("bind to a port that is in use", bind(w2, (struct sockaddr *)&a, sizeof a), EADDRINUSE);
    close(w);
    close(w2);
    int o = socket(AF_INET6, SOCK_DGRAM, 0);
    FAILS("an unknown IPv6 option", set_int(o, IPPROTO_IPV6, 9999, 1), ENOPROTOOPT);
    close(o);

    /* --- names, numerically */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    int rc = getaddrinfo("::1", "80", &hints, &res);
    printf("getaddrinfo(::1, 80): %d\n", rc);
    if (rc == 0 && res) {
        struct sockaddr_in6 *r6 = (struct sockaddr_in6 *)res->ai_addr;
        say_addr("the result", r6, res->ai_addrlen);
        printf("ai_family %d, ai_socktype %d, ai_protocol %d, the port is 80: %d\n", res->ai_family, res->ai_socktype, res->ai_protocol, ntohs(r6->sin6_port) == 80);
        char host[64], serv[16];
        rc = getnameinfo(res->ai_addr, res->ai_addrlen, host, sizeof host, serv, sizeof serv, NI_NUMERICHOST | NI_NUMERICSERV);
        printf("getnameinfo: %d '%s' '%s'\n", rc, host, serv);
        freeaddrinfo(res);
    }
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_flags = AI_NUMERICHOST;
    rc = getaddrinfo("::1", NULL, &hints, &res);
    printf("getaddrinfo of an IPv6 address for an IPv4 request: %s\n", rc != 0 ? "refused" : "answered");
    hints.ai_family = AF_INET6;
    hints.ai_flags = AI_NUMERICHOST | AI_V4MAPPED;
    rc = getaddrinfo("127.0.0.1", NULL, &hints, &res);
    if (rc == 0) {
        char text[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)res->ai_addr)->sin6_addr, text, sizeof text);
        printf("AI_V4MAPPED turns 127.0.0.1 into %s\n", text);
        freeaddrinfo(res);
    } else {
        printf("AI_V4MAPPED: no answer\n");
    }

    /* --- the text forms */
    static const char *const forms[] = { "::", "::1", "fe80::1:2", "2001:db8::", "1:0:0:2:0:0:0:3", "::ffff:10.0.2.2", "0:0:0:0:0:0:0:1", "1:2:3:4:5:6:7:8", "FE80::A", "1::2::3", "::g", "1:2:3:4:5:6:7:8:9", ":1", "1:", "::ffff:1.2.3.256", "1.2.3.4" };
    for (unsigned i = 0; i < sizeof forms / sizeof forms[0]; i++) {
        struct in6_addr x;
        int ok = inet_pton(AF_INET6, forms[i], &x);
        char text[INET6_ADDRSTRLEN] = "";
        if (ok == 1) inet_ntop(AF_INET6, &x, text, sizeof text);
        printf("inet_pton(%s): %d%s%s\n", forms[i], ok, ok == 1 ? " -> " : "", text);
    }
    return 0;
}
