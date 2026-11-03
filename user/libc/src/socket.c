/* Sockets (UNIX-domain and Internet) and pseudo-terminals: thin wrappers over the Gensokyo system calls. */
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <pty.h>
#include "syscall_priv.h"

int socket(int domain, int type, int protocol) { return (int)__ret(__sc3(SYS_socket, domain, type, protocol)); }

int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    if (!addr) { errno = EFAULT; return -1; }
    return (int)__ret(__sc3(SYS_bind, fd, (long)addr, (long)len));
}

int listen(int fd, int backlog) { return (int)__ret(__sc2(SYS_listen, fd, backlog)); }

int accept(int fd, struct sockaddr *addr, socklen_t *len) {
    if (addr && len && *len >= sizeof(sa_family_t)) addr->sa_family = 0;
    int r = (int)__ret(__sc3(SYS_accept, fd, (long)addr, (long)len));
    if (r >= 0 && addr && len && *len >= sizeof(sa_family_t) && addr->sa_family == 0) { addr->sa_family = AF_UNIX; *len = sizeof(sa_family_t); }
    return r;
}

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    if (!addr) { errno = EFAULT; return -1; }
    return (int)__ret(__sc3(SYS_connect, fd, (long)addr, (long)len));
}

int shutdown(int fd, int how) { return (int)__ret(__sc2(SYS_shutdown, fd, how)); }

int socketpair(int domain, int type, int protocol, int sv[2]) {
    (void)protocol;
    if (domain != AF_UNIX) { errno = EAFNOSUPPORT; return -1; }
    return (int)__ret(__sc2(SYS_socketpair, type, (long)sv));
}

ssize_t send(int fd, const void *buf, size_t n, int flags) { return (ssize_t)__ret(__sc6(SYS_sendto, fd, (long)buf, (long)n, flags, 0, 0)); }
ssize_t recv(int fd, void *buf, size_t n, int flags) { return (ssize_t)__ret(__sc6(SYS_recvfrom, fd, (long)buf, (long)n, flags, 0, 0)); }

ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *to, socklen_t tolen) {
    return (ssize_t)__ret(__sc6(SYS_sendto, fd, (long)buf, (long)n, flags, (long)to, (long)tolen));
}

ssize_t recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *from, socklen_t *fromlen) {
    return (ssize_t)__ret(__sc6(SYS_recvfrom, fd, (long)buf, (long)n, flags, (long)from, (long)fromlen));
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len) { return (int)__ret(__sc3(SYS_getsockname, fd, (long)addr, (long)len)); }
int getpeername(int fd, struct sockaddr *addr, socklen_t *len) { return (int)__ret(__sc3(SYS_getpeername, fd, (long)addr, (long)len)); }

int setsockopt(int fd, int level, int name, const void *val, socklen_t len) {
    return (int)__ret(__sc6(SYS_setsockopt, fd, level, name, (long)val, (long)len, 0));
}

int getsockopt(int fd, int level, int name, void *val, socklen_t *len) {
    return (int)__ret(__sc6(SYS_getsockopt, fd, level, name, (long)val, (long)len, 0));
}

int getpeercred(int fd, struct gpeercred *cred) { return ioctl(fd, SOCK_PEERCRED, cred); }

int gap_netctl(unsigned long cmd, void *arg) { return (int)__ret(__sc2(SYS_netctl, (long)cmd, (long)arg)); }

/* select() on top of poll(): descriptors are limited to FD_SETSIZE (64), like the kernel's descriptor table. */
int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv) {
    struct gpollfd p[FD_SETSIZE];
    int n = 0;
    if (nfds > FD_SETSIZE) nfds = FD_SETSIZE;
    for (int fd = 0; fd < nfds; fd++) {
        short ev = 0;
        if (rd && FD_ISSET(fd, rd)) ev |= POLLIN;
        if (wr && FD_ISSET(fd, wr)) ev |= POLLOUT;
        if (ex && FD_ISSET(fd, ex)) ev |= POLLERR;
        if (!ev) continue;
        p[n].fd = fd;
        p[n].events = ev;
        p[n].revents = 0;
        n++;
    }
    int ms = -1;
    if (tv) ms = (int)(tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000);
    int r = poll(p, (nfds_t)n, ms);
    if (r < 0) return -1;
    if (rd) FD_ZERO(rd);
    if (wr) FD_ZERO(wr);
    if (ex) FD_ZERO(ex);
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (rd && (p[i].revents & (POLLIN | POLLHUP))) { FD_SET(p[i].fd, rd); count++; }
        if (wr && (p[i].revents & (POLLOUT | POLLERR))) { FD_SET(p[i].fd, wr); count++; }
        if (ex && (p[i].revents & (POLLERR | POLLNVAL))) { FD_SET(p[i].fd, ex); count++; }
    }
    return count;
}

/* ---------------------------------------------------------------- pseudo-terminals */

int ptyopen(char *slave_path, size_t size) { return (int)__ret(__sc2(SYS_ptyopen, (long)slave_path, (long)size)); }

int openpty(int *master, int *slave, char *name, const struct termios *term, const struct winsize *ws) {
    char path[40];
    int m = ptyopen(path, sizeof path);
    if (m < 0) return -1;
    int s = open(path, O_RDWR | O_NOCTTY);
    if (s < 0) { int e = errno; close(m); errno = e; return -1; }
    if (term) tcsetattr(s, TCSANOW, term);
    if (ws) ioctl(s, TIOCSWINSZ, ws);
    if (name) strcpy(name, path);
    *master = m;
    *slave = s;
    return 0;
}
