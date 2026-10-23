/*
 * nettest: exercises the network stack from user space over the loopback interface (no NIC needed). Prints one line per
 * check and "NETTEST PASS" or "NETTEST FAIL" at the end; tests/case_net.py runs it.
 *
 *   nettest            everything
 *   nettest NAME       only the check called NAME (udp, tcp, bulk, refused, nonblock, close, half, many, opts, ping, loss, fuzz)
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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
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

static struct sockaddr_in lo_addr(unsigned port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

/* a listening TCP socket on 127.0.0.1 with a kernel-chosen port */
static int listener(int backlog, unsigned *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = lo_addr(0);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, backlog) < 0) { close(fd); return -1; }
    socklen_t len = sizeof a;
    getsockname(fd, (struct sockaddr *)&a, &len);
    *port = ntohs(a.sin_port);
    return fd;
}

static int dial(unsigned port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = lo_addr(port);
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

/* ---------------------------------------------------------------- checks */

static void t_udp(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0), c = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(s >= 0 && c >= 0);
    struct sockaddr_in a = lo_addr(0), from;
    CHECK(bind(s, (struct sockaddr *)&a, sizeof a) == 0);
    socklen_t len = sizeof a;
    CHECK(getsockname(s, (struct sockaddr *)&a, &len) == 0 && a.sin_port != 0 && len == sizeof a);
    CHECK(sendto(c, "ping", 4, 0, (struct sockaddr *)&a, sizeof a) == 4);
    char buf[64];
    len = sizeof from;
    CHECK(recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)&from, &len) == 4 && !memcmp(buf, "ping", 4));
    CHECK(from.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && from.sin_port != 0);
    CHECK(sendto(s, "pong!", 5, 0, (struct sockaddr *)&from, sizeof from) == 5);
    CHECK(recv(c, buf, sizeof buf, 0) == 5 && !memcmp(buf, "pong!", 5));
    /* a datagram bigger than the MTU is fragmented and reassembled */
    char *big = malloc(6000), *back = malloc(6000);
    for (int i = 0; i < 6000; i++) big[i] = (char)(i * 7);
    CHECK(sendto(c, big, 6000, 0, (struct sockaddr *)&a, sizeof a) == 6000);
    CHECK(recvfrom(s, back, 6000, 0, NULL, NULL) == 6000 && !memcmp(big, back, 6000));
    /* nothing listens on the port any more: a connected UDP socket learns about it */
    close(s);
    CHECK(connect(c, (struct sockaddr *)&a, sizeof a) == 0);
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
    free(big);
    free(back);
}

static void t_tcp(void) {
    unsigned port;
    int l = listener(5, &port);
    CHECK(l >= 0);
    int c = dial(port);
    CHECK(c >= 0);
    struct sockaddr_in peer;
    socklen_t len = sizeof peer;
    int a = accept(l, (struct sockaddr *)&peer, &len);
    CHECK(a >= 0 && len == sizeof peer && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    CHECK(write_full(c, "hello gensokyo", 14) == 0);
    char buf[64];
    CHECK(read_full(a, buf, 14) == 0 && !memcmp(buf, "hello gensokyo", 14));
    CHECK(write_full(a, "welcome", 7) == 0);
    CHECK(read_full(c, buf, 7) == 0 && !memcmp(buf, "welcome", 7));
    struct sockaddr_in me, you;
    socklen_t l1 = sizeof me, l2 = sizeof you;
    CHECK(getsockname(c, (struct sockaddr *)&me, &l1) == 0 && getpeername(c, (struct sockaddr *)&you, &l2) == 0);
    CHECK(ntohs(you.sin_port) == port && me.sin_port == peer.sin_port);
    close(a);
    close(c);
    close(l);
}

static unsigned lcg(unsigned *x) { *x = *x * 1664525u + 1013904223u; return *x >> 8; }

static void t_bulk(void) {
    unsigned port;
    int l = listener(5, &port);
    CHECK(l >= 0);
    const size_t total = 5u * 1024 * 1024;
    pid_t pid = fork();
    if (pid == 0) {                                   /* the sender */
        int c = dial(port);
        if (c < 0) _exit(2);
        static unsigned char chunk[16384];
        unsigned seed = 12345, sum = 0;
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
    static unsigned char buf[16384];
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
    long t1 = gap_time_ms();
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(sum == theirs);
    printf("  bulk: %u bytes, checksum %08x ok, %ld ms\n", (unsigned)total, sum, t1 - t0);
    close(a);
    close(l);
}

static void t_refused(void) {
    unsigned port;
    int l = listener(1, &port);
    CHECK(l >= 0);
    close(l);                                          /* the port is free again: nobody listens */
    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = lo_addr(port);
    CHECK(connect(c, (struct sockaddr *)&a, sizeof a) < 0 && errno == ECONNREFUSED);
    close(c);
}

static void t_nonblock(void) {
    unsigned port;
    int l = listener(5, &port);
    CHECK(l >= 0);
    fcntl(l, F_SETFL, O_NONBLOCK);
    CHECK(accept(l, NULL, NULL) < 0 && errno == EAGAIN);
    struct pollfd p = { l, POLLIN, 0 };
    CHECK(poll(&p, 1, 0) == 0);
    int c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    struct sockaddr_in a = lo_addr(port);
    int r = connect(c, (struct sockaddr *)&a, sizeof a);
    CHECK(r == 0 || (r < 0 && errno == EINPROGRESS));
    p.fd = l; p.events = POLLIN; p.revents = 0;
    CHECK(poll(&p, 1, 5000) == 1 && (p.revents & POLLIN));
    int x = accept(l, NULL, NULL);
    CHECK(x >= 0);
    struct pollfd q = { c, POLLOUT, 0 };
    CHECK(poll(&q, 1, 5000) == 1 && (q.revents & POLLOUT));
    int err = -1;
    socklen_t el = sizeof err;
    CHECK(getsockopt(c, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0);
    char buf[8];
    CHECK(read(c, buf, sizeof buf) < 0 && errno == EAGAIN);       /* nothing to read yet */
    CHECK(write(x, "go", 2) == 2);
    q.events = POLLIN; q.revents = 0;
    CHECK(poll(&q, 1, 5000) == 1 && (q.revents & POLLIN));
    CHECK(read(c, buf, sizeof buf) == 2);
    close(x);
    q.events = POLLIN; q.revents = 0;
    CHECK(poll(&q, 1, 5000) == 1 && (q.revents & POLLIN));       /* end of file is readable */
    CHECK(read(c, buf, sizeof buf) == 0);
    close(c);
    close(l);
}

static void t_close(void) {
    unsigned port;
    int l = listener(5, &port);
    CHECK(l >= 0);
    int c = dial(port);
    int a = accept(l, NULL, NULL);
    CHECK(c >= 0 && a >= 0);
    signal(SIGPIPE, SIG_IGN);
    CHECK(write_full(a, "bye", 3) == 0);
    close(a);                                          /* orderly close: we still read what was sent, then EOF */
    char buf[16];
    CHECK(read_full(c, buf, 3) == 0 && !memcmp(buf, "bye", 3));
    CHECK(read(c, buf, sizeof buf) == 0);
    /* writing to a closed peer: the first write is accepted, the RST that answers it makes the next one fail with EPIPE */
    int rc = 0, tries = 0;
    while (tries++ < 200) {
        rc = (int)write(c, "x", 1);
        if (rc < 0) break;
        usleep(5000);
    }
    CHECK(rc < 0 && (errno == EPIPE || errno == ECONNRESET));
    close(c);
    /* unread data at close() resets the connection */
    int c2 = dial(port);
    int a2 = accept(l, NULL, NULL);
    CHECK(c2 >= 0 && a2 >= 0);
    CHECK(write_full(c2, "unread", 6) == 0);
    usleep(50000);
    close(a2);
    tries = 0;
    ssize_t r = 0;
    while (tries++ < 200) { r = read(c2, buf, sizeof buf); if (r < 0) break; usleep(5000); }
    CHECK(r < 0 && errno == ECONNRESET);
    close(c2);
    close(l);
}

static void t_half(void) {
    unsigned port;
    int l = listener(5, &port);
    int c = dial(port);
    int a = accept(l, NULL, NULL);
    CHECK(l >= 0 && c >= 0 && a >= 0);
    CHECK(write_full(c, "request", 7) == 0);
    CHECK(shutdown(c, SHUT_WR) == 0);
    char buf[32];
    CHECK(read_full(a, buf, 7) == 0);
    CHECK(read(a, buf, sizeof buf) == 0);                          /* the client's FIN is an end of file ... */
    CHECK(write_full(a, "response", 8) == 0);                      /* ... but the server may still answer */
    CHECK(read_full(c, buf, 8) == 0 && !memcmp(buf, "response", 8));
    close(a);
    CHECK(read(c, buf, sizeof buf) == 0);
    close(c);
    close(l);
}

static void t_many(void) {
    unsigned port;
    int l = listener(64, &port);
    CHECK(l >= 0);
    int cs[24], as[24];
    for (int i = 0; i < 24; i++) {
        cs[i] = dial(port);
        CHECK(cs[i] >= 0);
    }
    for (int i = 0; i < 24; i++) {
        as[i] = accept(l, NULL, NULL);
        CHECK(as[i] >= 0);
    }
    for (int i = 0; i < 24; i++) {
        char m[16], b[16];
        int n = snprintf(m, sizeof m, "conn-%d", i);
        CHECK(write_full(cs[i], m, (size_t)n) == 0);
        CHECK(read_full(as[i], b, (size_t)n) == 0 && !memcmp(b, m, (size_t)n));
    }
    for (int i = 0; i < 24; i++) { close(cs[i]); close(as[i]); }
    close(l);
}

static void t_opts(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s >= 0);
    int v = 1, got = 0;
    socklen_t n = sizeof got;
    CHECK(setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &v, sizeof v) == 0);
    CHECK(getsockopt(s, SOL_SOCKET, SO_REUSEADDR, &got, &n) == 0 && got == 1);
    CHECK(setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &v, sizeof v) == 0);
    got = 0; n = sizeof got;
    CHECK(getsockopt(s, IPPROTO_TCP, TCP_NODELAY, &got, &n) == 0 && got == 1);
    got = 0;
    CHECK(getsockopt(s, SOL_SOCKET, SO_TYPE, &got, &n) == 0 && got == SOCK_STREAM);
    CHECK(setsockopt(s, SOL_SOCKET, 9999, &v, sizeof v) < 0 && errno == ENOPROTOOPT);
    close(s);
    /* receive timeout */
    unsigned port;
    int l = listener(5, &port);
    int c = dial(port);
    int a = accept(l, NULL, NULL);
    CHECK(c >= 0 && a >= 0);
    struct timeval tv = { 0, 200000 };
    CHECK(setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0);
    char b[4];
    long t0 = gap_time_ms();
    CHECK(read(c, b, sizeof b) < 0 && errno == EAGAIN);
    CHECK(gap_time_ms() - t0 >= 150);
    close(a); close(c); close(l);
    /* address in use / reuse */
    int s1 = socket(AF_INET, SOCK_DGRAM, 0), s2 = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a1 = lo_addr(0);
    CHECK(bind(s1, (struct sockaddr *)&a1, sizeof a1) == 0);
    socklen_t al = sizeof a1;
    getsockname(s1, (struct sockaddr *)&a1, &al);
    CHECK(bind(s2, (struct sockaddr *)&a1, sizeof a1) < 0 && errno == EADDRINUSE);
    close(s1); close(s2);
    /* a privileged port needs root */
    if (getuid() != 0) {
        int p = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in pa = lo_addr(80);
        CHECK(bind(p, (struct sockaddr *)&pa, sizeof pa) < 0 && errno == EACCES);
        close(p);
    }
}

static unsigned short csum16(const void *data, size_t n) {
    const unsigned char *p = data;
    unsigned long sum = 0;
    while (n > 1) { sum += (unsigned long)(p[0] << 8 | p[1]); p += 2; n -= 2; }
    if (n) sum += (unsigned long)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned short)~sum;
}

static void t_ping(void) {
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    CHECK(s >= 0);
    unsigned char pkt[16 + 8];
    memset(pkt, 0, sizeof pkt);
    pkt[0] = 8;                                         /* echo request */
    pkt[6] = 0; pkt[7] = 1;                             /* sequence 1 */
    memcpy(pkt + 8, "gensokyo-ping!!", 15);
    unsigned short c = csum16(pkt, sizeof pkt);
    pkt[2] = (unsigned char)(c >> 8); pkt[3] = (unsigned char)c;
    struct sockaddr_in a = lo_addr(0);
    CHECK(sendto(s, pkt, sizeof pkt, 0, (struct sockaddr *)&a, sizeof a) == (ssize_t)sizeof pkt);
    struct pollfd p = { s, POLLIN, 0 };
    CHECK(poll(&p, 1, 5000) == 1);
    unsigned char rep[128];
    ssize_t r = recv(s, rep, sizeof rep, 0);
    CHECK(r >= 20 + 8);
    unsigned ihl = (rep[0] & 15) * 4u;
    CHECK(rep[ihl] == 0 && rep[ihl + 7] == 1 && !memcmp(rep + ihl + 8, "gensokyo-ping!!", 15));
    close(s);
}

/* Datagrams and a stream across a lossy, duplicating, reordering loopback (/proc/net/tune must be set by the caller). */
static void t_loss(void) {
    unsigned port;
    int l = listener(5, &port);
    CHECK(l >= 0);
    const size_t total = 400 * 1024;
    pid_t pid = fork();
    if (pid == 0) {
        int c = dial(port);
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
    printf("  loss: %u bytes intact in %ld ms\n", (unsigned)total, gap_time_ms() - t0);
    close(a);
    close(l);
}

/* Malformed frames through /proc/net/inject: the kernel must shrug them all off. */
static unsigned short ip_csum_(const unsigned char *h, size_t n) { return csum16(h, n); }

static void send_frame(int fd, const unsigned char *ip, size_t n) {
    unsigned char fr[2048];
    memset(fr, 0, 12);
    fr[12] = 0x08; fr[13] = 0x00;
    memcpy(fr + 14, ip, n);
    if (write(fd, fr, 14 + n) < 0) { /* ignore */ }
}

static void t_fuzz(void) {
    int fd = open("/proc/net/inject", O_WRONLY);
    CHECK(fd >= 0);
    unsigned seed = 4242;
    unsigned char ip[1600];
    for (int round = 0; round < 3000; round++) {
        size_t n = 1 + lcg(&seed) % 120;
        for (size_t i = 0; i < n; i++) ip[i] = (unsigned char)lcg(&seed);
        switch (round % 6) {
        case 0: break;                                           /* pure noise */
        case 1: ip[0] = 0x45; break;                             /* plausible version, random everything else */
        default: {
            if (n < 40) n = 40;
            memset(ip, 0, n);
            ip[0] = 0x45 + (unsigned char)(lcg(&seed) % 3);
            ip[2] = (unsigned char)(n >> 8); ip[3] = (unsigned char)n;
            ip[8] = 64;
            ip[9] = (unsigned char)(round % 6 == 2 ? 6 : round % 6 == 3 ? 17 : round % 6 == 4 ? 1 : 6);
            ip[12] = 127; ip[15] = 1; ip[16] = 127; ip[19] = 1;
            if (lcg(&seed) % 3 == 0) { ip[6] = (unsigned char)(lcg(&seed) & 0x3F); ip[7] = (unsigned char)lcg(&seed); }
            unsigned short c = ip_csum_(ip, 20);
            if (lcg(&seed) % 4) { ip[10] = (unsigned char)(c >> 8); ip[11] = (unsigned char)c; }
            for (size_t i = 20; i < n; i++) if (lcg(&seed) % 3 == 0) ip[i] = (unsigned char)lcg(&seed);
            ip[20 + 12] = (unsigned char)((lcg(&seed) % 16) << 4);           /* TCP data offset: sometimes nonsense */
            break;
        }
        }
        send_frame(fd, ip, n);
    }
    close(fd);
    /* the stack must still work */
    unsigned port;
    int l = listener(5, &port);
    int c = dial(port);
    int a = accept(l, NULL, NULL);
    CHECK(l >= 0 && c >= 0 && a >= 0);
    CHECK(write_full(c, "still alive", 11) == 0);
    char buf[16];
    CHECK(read_full(a, buf, 11) == 0 && !memcmp(buf, "still alive", 11));
    close(a); close(c); close(l);
}

typedef struct { const char *name; void (*fn)(void); } Check;

int main(int argc, char **argv) {
    static const Check checks[] = {
        { "udp", t_udp }, { "tcp", t_tcp }, { "bulk", t_bulk }, { "refused", t_refused }, { "nonblock", t_nonblock },
        { "close", t_close }, { "half", t_half }, { "many", t_many }, { "opts", t_opts }, { "ping", t_ping },
    };
    static const Check only_on_request[] = { { "loss", t_loss }, { "fuzz", t_fuzz } };       /* both need root and say so on /proc/net */
    const char *want = argc > 1 ? argv[1] : NULL;
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
    if (!ran) { printf("nettest: no such check\n"); return 2; }
    printf("NETTEST %s\n", g_failed ? "FAIL" : "PASS");
    return g_failed ? 1 : 0;
}
