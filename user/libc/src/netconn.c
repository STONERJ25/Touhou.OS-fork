/*
 * netconn: connect to the first address of a list that answers, without letting a broken address family block the other.
 *
 * Follows RFC 8305 ("Happy Eyeballs Version 2"), simplified:
 *   - section 4: the list getaddrinfo gives (already in RFC 6724 order) is interleaved by family, the first address's family first, then
 *     alternating (a "First Address Family Count" of 1), each family keeping its own order;
 *   - section 5: attempts are staggered: the next address is started when the last one has had stagger_ms (250 by default, the RFC's
 *     recommended "Connection Attempt Delay") without an answer, or at once when an attempt fails; attempts already running go on, and the
 *     first connection to complete wins, the others are closed;
 *   - each attempt has its own limit (2 s by default) so a black hole costs that and no more, and the whole has an optional limit.
 * Left out: the asynchronous DNS part of section 3 (the resolver asks AAAA and A one after the other, and returns both before connecting),
 * the "Resolution Delay", NAT64 synthesis (section 7), and remembering round-trip times between calls.
 *
 * Only POSIX calls are used (socket, fcntl, connect, poll, getsockopt, clock_gettime), so tests/host/test_netaddr.py builds this same file
 * against the host's C library and drives it over real sockets.
 */
#include <netconn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define MAX_INFLIGHT 8

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void netconn_interleave(const struct addrinfo **v, int n) {
    if (n < 3) return;                                                  /* two addresses are already as interleaved as they can be */
    const struct addrinfo *tmp[NETCONN_MAX];
    if (n > NETCONN_MAX) n = NETCONN_MAX;
    int first = v[0]->ai_family, a = 0, b = 0, k = 0;                   /* a: next of the first family, b: next of the others */
    while (k < n) {
        while (a < n && v[a]->ai_family != first) a++;
        while (b < n && v[b]->ai_family == first) b++;
        if (a < n) tmp[k++] = v[a++];
        if (b < n && k < n) tmp[k++] = v[b++];
    }
    memcpy(v, tmp, (size_t)n * sizeof v[0]);
}

typedef struct { int fd; long started; const struct addrinfo *ai; } Attempt;

/* Starts a non-blocking connection; 1: running, 0: connected at once, -1: failed at once (errno). */
static int start(const struct addrinfo *ai, int *fdp) {
    int fd = socket(ai->ai_family, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    *fdp = fd;
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) return 0;
    if (errno == EINPROGRESS || errno == EAGAIN) return 1;
    int e = errno;
    close(fd);
    errno = e;
    return -1;
}

static int finish(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    return fd;
}

int netconn_connect(const struct addrinfo *list, int attempt_ms, int stagger_ms, int total_ms, int *err, const struct addrinfo **used) {
    const struct addrinfo *v[NETCONN_MAX];
    int n = 0;
    for (const struct addrinfo *ai = list; ai && n < NETCONN_MAX; ai = ai->ai_next)
        if ((ai->ai_family == AF_INET || ai->ai_family == AF_INET6) && (ai->ai_socktype == SOCK_STREAM || ai->ai_socktype == 0)) v[n++] = ai;
    netconn_interleave(v, n);
    if (attempt_ms <= 0) attempt_ms = NETCONN_ATTEMPT_MS;
    if (stagger_ms <= 0) stagger_ms = NETCONN_STAGGER_MS;

    Attempt run[MAX_INFLIGHT];
    int nrun = 0, next = 0, last_err = 0, saw_other = 0;
    long t0 = now_ms(), last_start = 0;
    int dummy;
    if (!err) err = &dummy;
    *err = ENOENT;
    if (!n) { *err = EADDRNOTAVAIL; return -1; }

    for (;;) {
        long t = now_ms();
        /* start the next address: nothing running, or the last one has had its head start */
        while (next < n && nrun < MAX_INFLIGHT && (nrun == 0 || t - last_start >= stagger_ms)) {
            int fd = -1, r = start(v[next], &fd);
            if (r == 0) {
                for (int i = 0; i < nrun; i++) close(run[i].fd);
                if (used) *used = v[next];
                return finish(fd);
            }
            if (r < 0) { last_err = errno; saw_other = 1; next++; continue; }   /* failed at once (no route, say): the next one now */
            run[nrun++] = (Attempt){ fd, t, v[next] };
            last_start = t;
            next++;
        }
        /* attempts that had their time; once every address has been started, the ones still running may use the whole limit (total_ms),
         * so a single slow server is not cut off at attempt_ms */
        int limit = next >= n && total_ms > 0 ? total_ms : attempt_ms;
        for (int i = 0; i < nrun; ) {
            if (t - run[i].started >= limit) { close(run[i].fd); run[i] = run[--nrun]; if (!last_err) last_err = ETIMEDOUT; continue; }
            i++;
        }
        if (nrun == 0 && next >= n) break;
        if (total_ms > 0 && t - t0 >= total_ms) break;
        if (nrun == 0) continue;                                         /* the timed-out ones made room: start the next at once */

        int wait = limit;
        for (int i = 0; i < nrun; i++) { long left = run[i].started + limit - t; if (left < wait) wait = (int)left; }
        if (next < n) { long left = last_start + stagger_ms - t; if (left < wait) wait = (int)left; }
        if (total_ms > 0) { long left = t0 + total_ms - t; if (left < wait) wait = (int)left; }
        if (wait < 0) wait = 0;
        struct pollfd p[MAX_INFLIGHT];
        for (int i = 0; i < nrun; i++) { p[i].fd = run[i].fd; p[i].events = POLLOUT; p[i].revents = 0; }
        int pr = poll(p, (nfds_t)nrun, wait);
        if (pr < 0 && errno != EINTR) { last_err = errno; saw_other = 1; break; }
        if (pr <= 0) continue;
        for (int i = nrun - 1; i >= 0; i--) {
            if (!p[i].revents) continue;
            int e = 0;
            socklen_t el = sizeof e;
            if (getsockopt(run[i].fd, SOL_SOCKET, SO_ERROR, &e, &el) < 0) e = errno;
            if (e == 0 && !(p[i].revents & (POLLERR | POLLHUP | POLLNVAL))) {
                int fd = run[i].fd;
                for (int k = 0; k < nrun; k++) if (k != i) close(run[k].fd);
                if (used) *used = run[i].ai;
                return finish(fd);
            }
            if (e == 0) e = ECONNREFUSED;
            last_err = e;
            saw_other = 1;
            close(run[i].fd);
            run[i] = run[--nrun];
            p[i] = p[nrun];
            last_start = 0;                                              /* a failure lets the next address start at once */
        }
    }
    for (int i = 0; i < nrun; i++) close(run[i].fd);
    *err = saw_other && last_err ? last_err : ETIMEDOUT;
    return -1;
}

int netconn_tcp(const char *host, int port, int attempt_ms, int total_ms, char *ip, size_t ipcap, int *gai, int *err) {
    int dummy_g, dummy_e;
    if (!gai) gai = &dummy_g;
    if (!err) err = &dummy_e;
    *gai = 0;
    *err = 0;
    char name[256];
    size_t hl = strlen(host);
    if (hl >= 2 && host[0] == '[' && host[hl - 1] == ']') host++, hl -= 2;     /* "[::1]" as a URL writes it */
    if (hl >= sizeof name) { *gai = EAI_NONAME; return -1; }
    memcpy(name, host, hl);
    name[hl] = 0;
    char service[8];
    snprintf(service, sizeof service, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    int rc = getaddrinfo(name, service, &hints, &res);
    if (rc == EAI_NONAME || rc == EAI_FAIL || (rc == 0 && !res)) {     /* a machine with no address at all can still reach its own names (localhost) */
        if (res) freeaddrinfo(res);
        res = NULL;
        hints.ai_flags = 0;
        int rc2 = getaddrinfo(name, service, &hints, &res);
        if (rc2 == 0) rc = 0;
    }
    if (rc != 0 || !res) { *gai = rc ? rc : EAI_NONAME; return -1; }
    const struct addrinfo *won = NULL;
    int fd = netconn_connect(res, attempt_ms, 0, total_ms, err, &won);
    if (fd >= 0 && ip && ipcap) {
        ip[0] = 0;
        if (won->ai_family == AF_INET6) inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)won->ai_addr)->sin6_addr, ip, (socklen_t)ipcap);
        else inet_ntop(AF_INET, &((const struct sockaddr_in *)won->ai_addr)->sin_addr, ip, (socklen_t)ipcap);
    }
    freeaddrinfo(res);
    return fd;
}
