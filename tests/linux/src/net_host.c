/*
 * A statically linked C program that talks to servers on another machine: TCP to a Python server (a greeting, a request and its answer, 300000 bytes with a
 * checksum, shutdown and end of file), a UDP exchange, a DNS query that the program builds by hand and sends over UDP, numeric getaddrinfo, and a refused
 * connect. The test (tests/case_linux_net.py) starts the servers on the host and runs this in the guest against 10.0.2.2, which is the host as QEMU's user
 * networking shows it; the expected text is written there, not taken from a real Linux, because the peer is the test's own Python.
 *
 *     net_host HOST TCP_PORT UDP_PORT DNS_PORT CLOSED_PORT
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
#include <sys/socket.h>
#include <unistd.h>

static struct sockaddr_in addr_of(const char *host, int port) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, host, &sa.sin_addr);
    return sa;
}

static unsigned adler(const unsigned char *p, size_t n) {
    unsigned a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

/* reads up to and including the next newline */
static int read_line(int fd, char *out, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r <= 0) break;
        out[n++] = c;
        if (c == '\n') break;
    }
    out[n] = 0;
    return (int)n;
}

int main(int argc, char **argv) {
    if (argc < 6) { printf("usage: net_host HOST TCP_PORT UDP_PORT DNS_PORT CLOSED_PORT\n"); return 2; }
    const char *host = argv[1];
    int tcp_port = atoi(argv[2]), udp_port = atoi(argv[3]), dns_port = atoi(argv[4]), closed_port = atoi(argv[5]);
    char line[256];

    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in to = addr_of(host, tcp_port);
    printf("connect: %d\n", connect(s, (struct sockaddr *)&to, sizeof to));
    struct sockaddr_in me;
    socklen_t ml = sizeof me;
    getsockname(s, (struct sockaddr *)&me, &ml);
    printf("the source address is a real interface address, not loopback: %d\n", me.sin_addr.s_addr != htonl(INADDR_LOOPBACK) && me.sin_addr.s_addr != 0);
    struct sockaddr_in peer;
    socklen_t pl = sizeof peer;
    getpeername(s, (struct sockaddr *)&peer, &pl);
    printf("the peer is the server we named: %d\n", peer.sin_addr.s_addr == to.sin_addr.s_addr && peer.sin_port == to.sin_port);
    read_line(s, line, sizeof line);
    printf("greeting: %s", line);
    write(s, "ping 1\n", 7);
    read_line(s, line, sizeof line);
    printf("answer: %s", line);

    size_t big = 300000;
    unsigned char *data = malloc(big);
    for (size_t i = 0; i < big; i++) data[i] = (unsigned char)((i * 31) ^ (i >> 5));
    write(s, "BIG 300000\n", 11);
    size_t sent = 0;
    while (sent < big) {
        ssize_t w = write(s, data + sent, big - sent > 30000 ? 30000 : big - sent);
        if (w <= 0) break;
        sent += (size_t)w;
    }
    read_line(s, line, sizeof line);
    char expect[64];
    snprintf(expect, sizeof expect, "adler:%08x\n", adler(data, big));
    printf("sent %zu bytes; the server's checksum equals ours: %d\n", sent, strcmp(line, expect) == 0);
    printf("shutdown(SHUT_WR): %d\n", shutdown(s, SHUT_WR));
    read_line(s, line, sizeof line);
    printf("last words: %s", line);
    printf("then end of file: %zd\n", read(s, line, sizeof line));
    close(s);
    free(data);

    /* UDP */
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in us = addr_of(host, udp_port);
    sendto(u, "dgram from the guest", 20, 0, (struct sockaddr *)&us, sizeof us);
    struct pollfd p = { u, POLLIN, 0 };
    printf("poll for the answer: %d\n", poll(&p, 1, 20000));
    char buf[512];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    memset(buf, 0, sizeof buf);
    ssize_t r = recvfrom(u, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
    printf("answer: %zd bytes '%s', from the server's port: %d, address: %s\n", r, buf, from.sin_port == us.sin_port, inet_ntoa(from.sin_addr));
    close(u);

    /* a DNS query built by hand: header, the name as labels, type A, class IN */
    int d = socket(AF_INET, SOCK_DGRAM, 0);
    unsigned char q[512];
    size_t n = 0;
    q[n++] = 0x12; q[n++] = 0x34;                                      /* id */
    q[n++] = 0x01; q[n++] = 0x00;                                      /* recursion desired */
    q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0;
    const char *labels[] = { "gensokyo", "test" };
    for (int i = 0; i < 2; i++) {
        q[n++] = (unsigned char)strlen(labels[i]);
        memcpy(q + n, labels[i], strlen(labels[i]));
        n += strlen(labels[i]);
    }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 1;                    /* A, IN */
    struct sockaddr_in ds = addr_of(host, dns_port);
    sendto(d, q, n, 0, (struct sockaddr *)&ds, sizeof ds);
    struct pollfd pd = { d, POLLIN, 0 };
    poll(&pd, 1, 20000);
    unsigned char a[512];
    r = recv(d, a, sizeof a, 0);
    if (r >= 12 + 4) {
        int an = a[6] << 8 | a[7];
        printf("DNS answer: id matches %d, %d answer, address %u.%u.%u.%u\n", a[0] == 0x12 && a[1] == 0x34, an, a[r - 4], a[r - 3], a[r - 2], a[r - 1]);
    } else {
        printf("DNS answer: none (%zd)\n", r);
    }
    close(d);

    /* names that need no server */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    int g = getaddrinfo("10.0.2.2", "80", &hints, &res);
    if (g == 0) {
        struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
        printf("getaddrinfo of a numeric address: 0, %s port %d\n", inet_ntoa(sin->sin_addr), ntohs(sin->sin_port));
        freeaddrinfo(res);
    } else {
        printf("getaddrinfo of a numeric address: %d\n", g);
    }
    int pr = inet_pton(AF_INET, "192.0.2.9", &me.sin_addr);
    const char *pn = inet_ntop(AF_INET, &me.sin_addr, line, sizeof line);
    printf("inet_pton: %d, inet_ntop: %s\n", pr, pn);

    /* a closed port on the host is refused */
    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in closed = addr_of(host, closed_port);
    errno = 0;
    int rc = connect(c, (struct sockaddr *)&closed, sizeof closed);
    printf("connect to a port nobody listens on: %d errno %d (ECONNREFUSED %d)\n", rc, errno, ECONNREFUSED);
    close(c);
    return 0;
}
