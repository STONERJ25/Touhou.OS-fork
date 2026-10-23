/* UNIX-domain sockets and pseudo-terminals: thin wrappers over the Gensokyo system calls. */
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <pty.h>
#include "syscall_priv.h"

int socket(int domain, int type, int protocol) { return (int)__ret(__sc3(SYS_socket, domain, type, protocol)); }

int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    (void)len;
    if (!addr || addr->sa_family != AF_UNIX) { errno = EAFNOSUPPORT; return -1; }
    return (int)__ret(__sc2(SYS_bind, fd, (long)((const struct sockaddr_un *)addr)->sun_path));
}

int listen(int fd, int backlog) { return (int)__ret(__sc2(SYS_listen, fd, backlog)); }

int accept(int fd, struct sockaddr *addr, socklen_t *len) {
    int r = (int)__ret(__sc1(SYS_accept, fd));
    if (r >= 0 && addr && len && *len >= sizeof(sa_family_t)) { addr->sa_family = AF_UNIX; *len = sizeof(sa_family_t); }
    return r;
}

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    (void)len;
    if (!addr || addr->sa_family != AF_UNIX) { errno = EAFNOSUPPORT; return -1; }
    return (int)__ret(__sc2(SYS_connect, fd, (long)((const struct sockaddr_un *)addr)->sun_path));
}

int shutdown(int fd, int how) { return (int)__ret(__sc2(SYS_shutdown, fd, how)); }

int socketpair(int domain, int type, int protocol, int sv[2]) {
    (void)protocol;
    if (domain != AF_UNIX) { errno = EAFNOSUPPORT; return -1; }
    return (int)__ret(__sc2(SYS_socketpair, type, (long)sv));
}

ssize_t send(int fd, const void *buf, size_t n, int flags) { (void)flags; return write(fd, buf, n); }
ssize_t recv(int fd, void *buf, size_t n, int flags) { (void)flags; return read(fd, buf, n); }

int getpeercred(int fd, struct gpeercred *cred) { return ioctl(fd, SOCK_PEERCRED, cred); }

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
