/* pidfd (docs/COMPAT.md, "Descriptors, FIFOs, signalfd and pidfd"): pidfd_open, poll readiness at exit (and after the reap), pidfd_send_signal (signal 0, SIGTERM,
 * a siginfo to ourselves, ESRCH after the reap), waitid(P_PIDFD), /proc/self/fdinfo's Pid line, and the errors (EINVAL, ESRCH, EBADF). Written from pidfd_open(2),
 * pidfd_send_signal(2), waitid(2) and proc(5); the calls go through syscall(2) so that the program does not depend on the C library's wrappers. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define P_PIDFD_ 3
static long p_open(long pid, long flags) { return syscall(434, pid, flags); }
static long p_send(long fd, long sig, siginfo_t *info, long flags) { return syscall(424, fd, sig, info, flags); }

static void fact(const char *what, long v) { printf("%s: %ld\n", what, v); fflush(stdout); }
static const char *ename(int e) { return e == ESRCH ? "ESRCH" : e == EINVAL ? "EINVAL" : e == EBADF ? "EBADF" : e == ECHILD ? "ECHILD" : e == 0 ? "none" : "other"; }
static void err(const char *what, long v) { printf("%s: %ld (%s)\n", what, v, v < 0 ? ename(errno) : "none"); fflush(stdout); errno = 0; }

static int ready(int fd, int ms) { struct pollfd p = { .fd = fd, .events = POLLIN }; int r = poll(&p, 1, ms); return r == 1 && (p.revents & POLLIN); }

static long fdinfo_pid(int fd) {
    char path[64], line[128];
    snprintf(path, sizeof path, "/proc/self/fdinfo/%d", fd);
    FILE *f = fopen(path, "r");
    long v = -100;
    while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "Pid:", 4)) v = atol(line + 4);
    if (f) fclose(f);
    return v;
}

int main(void) {
    int self = (int)p_open(getpid(), 0);
    fact("pidfd of ourselves", self >= 0);
    fact("close-on-exec", (fcntl(self, F_GETFD) & FD_CLOEXEC) != 0);
    fact("fdinfo Pid is ours", fdinfo_pid(self) == getpid());
    err("pid 0", p_open(0, 0));
    err("pid -1", p_open(-1, 0));
    err("unknown flags", p_open(getpid(), 1));
    err("no such process", p_open(0x7ffffff0, 0));
    int nb = (int)p_open(getpid(), O_NONBLOCK);
    fact("PIDFD_NONBLOCK accepted", nb >= 0 && (fcntl(nb, F_GETFL) & O_NONBLOCK) != 0);

    /* a signal through the descriptor, with a siginfo, to ourselves */
    sigset_t m; sigemptyset(&m); sigaddset(&m, SIGUSR1);
    sigprocmask(SIG_BLOCK, &m, NULL);
    siginfo_t si;
    memset(&si, 0, sizeof si);
    si.si_signo = SIGUSR1; si.si_code = SI_QUEUE; si.si_value.sival_int = 77; si.si_pid = getpid(); si.si_uid = getuid();
    err("send SIGUSR1 with a siginfo to ourselves", p_send(self, SIGUSR1, &si, 0));
    siginfo_t got;
    fact("sigwaitinfo", sigwaitinfo(&m, &got) == SIGUSR1);
    fact("value", got.si_value.sival_int);
    fact("code", got.si_code);
    err("an undefined flag", p_send(self, 0, NULL, 0x40));

    /* a child killed through its pidfd */
    int hold[2];
    pipe(hold);
    pid_t kid = fork();
    if (kid == 0) { char c; close(hold[1]); read(hold[0], &c, 1); _exit(0); }
    close(hold[0]);
    int pfd = (int)p_open(kid, 0);
    fact("pidfd of the child", pfd >= 0);
    fact("fdinfo Pid is the child", fdinfo_pid(pfd) == kid);
    fact("ready while it runs", ready(pfd, 0));
    err("signal 0", p_send(pfd, 0, NULL, 0));
    err("SIGTERM", p_send(pfd, SIGTERM, NULL, 0));
    fact("ready after it died", ready(pfd, 5000));
    memset(&si, 0, sizeof si);
    err("waitid(P_PIDFD)", waitid(P_PIDFD_, pfd, &si, WEXITED));
    fact("waitid: pid is the child", si.si_pid == kid);
    fact("waitid: code (CLD_KILLED)", si.si_code);
    fact("waitid: status (SIGTERM)", si.si_status);
    fact("still ready after the reap", ready(pfd, 0));
    err("signal after the reap", p_send(pfd, SIGTERM, NULL, 0));
    err("waitid again", waitid(P_PIDFD_, pfd, &si, WEXITED | WNOHANG));
    close(pfd);
    close(hold[1]);

    /* a child that exits by itself */
    kid = fork();
    if (kid == 0) _exit(3);
    pfd = (int)p_open(kid, 0);
    fact("ready when it has exited", ready(pfd, 5000));
    memset(&si, 0, sizeof si);
    err("waitid(P_PIDFD)", waitid(P_PIDFD_, pfd, &si, WEXITED));
    fact("waitid: code (CLD_EXITED)", si.si_code);
    fact("waitid: status", si.si_status);

    /* not a pidfd */
    int p[2];
    pipe(p);
    err("send through a pipe", p_send(p[0], 0, NULL, 0));
    err("send through a closed descriptor", p_send(900, 0, NULL, 0));
    err("waitid on a pipe", waitid(P_PIDFD_, p[0], &si, WEXITED | WNOHANG));
    err("waitid on a closed descriptor", waitid(P_PIDFD_, 900, &si, WEXITED | WNOHANG));
    return 0;
}
