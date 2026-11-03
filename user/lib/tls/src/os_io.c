/*
 * The operating-system side of libtls: random numbers, the clocks, the socket calls, files. The same file builds for TouhouOS
 * and, for the host tests, for Linux (only the source of random numbers differs).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/random.h>
#else
#include <gensokyo.h>
#endif
#include "conn.h"

/* TouhouOS: getrandom() draws from the kernel's ChaCha20 generator (kernel/lib/random.c), seeded from RDRAND and timing. */
int tls_os_random(void *buf, size_t n) {
    u8 *p = buf;
    while (n) {
        size_t k = n > 256 ? 256 : n;
#ifdef __linux__
        long r = (long)getrandom(p, k, 0);
#else
        long r = gap_getrandom(p, k);
#endif
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

int64_t tls_os_time(void) { return (int64_t)time(NULL); }

int64_t tls_os_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

long tls_os_recv(int fd, void *buf, size_t n) {
    for (;;) {
        ssize_t r = recv(fd, buf, n, MSG_DONTWAIT);
        if (r >= 0) return (long)r;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return TLS_WANT_READ;
        return TLS_E_IO;
    }
}

long tls_os_send(int fd, const void *buf, size_t n) {
    for (;;) {
        ssize_t r = send(fd, buf, n, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (r > 0) return (long)r;
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return TLS_WANT_WRITE;
        return TLS_E_IO;
    }
}

int tls_os_wait(int fd, int want, int timeout_ms) {
    struct pollfd p;
    int64_t deadline = timeout_ms < 0 ? 0 : tls_os_monotonic_ms() + timeout_ms;
    for (;;) {
        p.fd = fd;
        p.events = want == TLS_WANT_WRITE ? POLLOUT : POLLIN;
        p.revents = 0;
        int left = -1;
        if (timeout_ms >= 0) {
            int64_t l = deadline - tls_os_monotonic_ms();
            left = l < 0 ? 0 : (int)l;
        }
        int r = poll(&p, 1, left);
        if (r > 0) return 0;                                         /* ready, or hung up / failed: the next call says which */
        if (r == 0) return TLS_E_TIMEOUT;
        if (errno != EINTR) return TLS_E_IO;
    }
}

int tls_os_read_file(const char *path, size_t max, u8 **out, size_t *outlen) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? TLS_E_NO_TRUST : TLS_E_IO;
    size_t cap = 65536, len = 0;
    u8 *buf = malloc(cap);
    if (!buf) { close(fd); return TLS_E_NOMEM; }
    for (;;) {
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { free(buf); close(fd); return TLS_E_IO; }
        if (r == 0) break;
        len += (size_t)r;
        if (len == cap) {
            if (cap >= max) { free(buf); close(fd); return TLS_E_IO; }
            u8 *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); close(fd); return TLS_E_NOMEM; }
            buf = nb;
            cap *= 2;
        }
    }
    close(fd);
    *out = buf;
    *outlen = len;
    return 0;
}

const char *tls_os_getenv(const char *name) { return getenv(name); }
