/* The operating-system side of libtls: random numbers, the clock, sockets and files. (The host tests replace this file.) */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "gensokyo.h"
#include "tlsconn.h"

/* getrandom() draws from the kernel's ChaCha20 generator (kernel/lib/random.c), seeded from RDSEED/RDRAND and timing jitter. */
int tls_os_random(void *buf, size_t n) {
    u8 *p = buf;
    while (n) {
        int r = gap_getrandom(p, n > 4096 ? 4096 : n);
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* /proc/random ends with "ready: yes" once the kernel has collected 128 bits it believes in. */
int tls_os_random_ready(void) {
    int fd = open("/proc/random", O_RDONLY);
    if (fd < 0) return -1;
    char text[768];
    ssize_t n = read(fd, text, sizeof text - 1);
    close(fd);
    if (n <= 0) return -1;
    text[n] = 0;
    if (strstr(text, "ready: yes")) return 1;
    if (strstr(text, "ready: no")) return 0;
    return -1;
}

int64_t tls_os_time(void) { return (int64_t)time(NULL); }

static long fd_send(void *user, const void *buf, size_t len) {
    int fd = (int)(long)user;
    for (;;) {
        ssize_t w = send(fd, buf, len, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        return (long)w;
    }
}

static long fd_recv(void *user, void *buf, size_t len) {
    int fd = (int)(long)user;
    for (;;) {
        ssize_t r = recv(fd, buf, len, 0);
        if (r < 0 && errno == EINTR) continue;
        return (long)r;
    }
}

void tls_io_fd(tls_io *io, int fd) {
    io->send = fd_send;
    io->recv = fd_recv;
    io->user = (void *)(long)fd;
}

tls_conn *tls_connect_fd(int fd, const tls_config *cfg, int *err) {
    tls_io io;
    tls_io_fd(&io, fd);
    tls_conn *c = tls_new(&io, cfg);
    if (!c) {
        if (err) *err = TLS_E_NOMEM;
        return NULL;
    }
    int rc = tls_handshake(c);
    if (rc) {
        if (err) *err = rc;
        tls_free(c);
        return NULL;
    }
    if (err) *err = 0;
    return c;
}

int tls_ctx_load_file(tls_ctx *ctx, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return TLS_E_IO;
    size_t cap = 65536, len = 0;
    u8 *buf = malloc(cap);
    if (!buf) { close(fd); return TLS_E_NOMEM; }
    for (;;) {
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0) { free(buf); close(fd); return TLS_E_IO; }
        if (r == 0) break;
        len += (size_t)r;
        if (len == cap) {
            if (cap >= (16u << 20)) { free(buf); close(fd); return TLS_E_IO; }
            u8 *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); close(fd); return TLS_E_NOMEM; }
            buf = nb;
            cap *= 2;
        }
    }
    close(fd);
    int rc = (len > 0 && buf[0] == 0x30) ? tls_ctx_load_der(ctx, buf, len) : tls_ctx_load_pem(ctx, buf, len);
    free(buf);
    return rc;
}

int tls_ctx_load_default_ca(tls_ctx *ctx) { return tls_ctx_load_file(ctx, "/etc/ssl/certs/ca-bundle.pem"); }
