/* The second round on descriptors (docs/FDS_REVIEW.md; docs/COMPAT.md, "Descriptors, FIFOs, signalfd and pidfd"): pidfd_send_signal to a zombie, pidfd_getfd,
 * what poll says about a FIFO before any writer has opened, after one came and went and after another came, a FIFO opened O_RDWR polled for writing (and woken when
 * a reader makes room), and poll on a signalfd and a pidfd that has to be woken by the signal and by the exit. Written from pidfd_send_signal(2), pidfd_getfd(2),
 * fifo(7), poll(2), signalfd(2) and pidfd_open(2); the pidfd calls go through syscall(2) so that the program does not depend on the C library's wrappers. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long p_open(long pid, long flags) { return syscall(434, pid, flags); }
static long p_send(long fd, long sig, siginfo_t *info, long flags) { return syscall(424, fd, sig, info, flags); }
static long p_getfd(long fd, long target, long flags) { return syscall(438, fd, target, flags); }

static void fact(const char *what, long v) { printf("%s: %ld\n", what, v); fflush(stdout); }
static const char *ename(int e) { return e == ESRCH ? "ESRCH" : e == EINVAL ? "EINVAL" : e == EBADF ? "EBADF" : e == EPERM ? "EPERM" : e == ENOENT ? "ENOENT" : e == 0 ? "none" : "other"; }
static void err(const char *what, long v) { printf("%s: %s\n", what, v < 0 ? ename(errno) : "ok"); fflush(stdout); errno = 0; }

static long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static void polled(const char *what, int fd, short ev, int ms) {
    struct pollfd p = { .fd = fd, .events = ev };
    int r = poll(&p, 1, ms);
    char b[64] = "";
    if (p.revents & POLLIN) strcat(b, "IN ");
    if (p.revents & POLLOUT) strcat(b, "OUT ");
    if (p.revents & POLLHUP) strcat(b, "HUP ");
    if (p.revents & POLLERR) strcat(b, "ERR ");
    if (p.revents & POLLNVAL) strcat(b, "NVAL ");
    printf("%s: %d [%s]\n", what, r, b);
    fflush(stdout);
}

static void zombie(void) {
    pid_t c = fork();
    if (!c) _exit(3);
    int fd = (int)p_open(c, 0);
    siginfo_t si;
    memset(&si, 0, sizeof si);
    fact("waitid WNOWAIT leaves the zombie", waitid(P_PID, (id_t)c, &si, WEXITED | WNOWAIT));
    err("signal 0 to a zombie", p_send(fd, 0, NULL, 0));
    err("SIGTERM to a zombie", p_send(fd, SIGTERM, NULL, 0));
    int st = 0;
    fact("reaped it", waitpid(c, &st, 0) == c);
    fact("its status is still exit 3", WIFEXITED(st) && WEXITSTATUS(st) == 3);
    err("signal 0 after the reap", p_send(fd, 0, NULL, 0));
    close(fd);
}

static void getfd(void) {
    int pp[2];
    if (pipe(pp)) exit(1);
    dup2(pp[1], 77);                                /* the child keeps the writing end as 77; we close both of ours */
    pid_t c = fork();
    if (!c) { for (;;) pause(); }
    close(pp[1]);
    close(77);
    int pfd = (int)p_open(c, 0);
    err("pidfd_getfd with flags", p_getfd(pfd, 77, 1));
    err("pidfd_getfd of a closed number", p_getfd(pfd, 500, 0));
    err("pidfd_getfd through a pipe", p_getfd(pp[0], 77, 0));
    err("pidfd_getfd through nothing", p_getfd(999, 77, 0));
    int got = (int)p_getfd(pfd, 77, 0);
    err("pidfd_getfd", got);
    fact("it is close-on-exec", (fcntl(got, F_GETFD) & FD_CLOEXEC) != 0);
    fact("wrote through it", write(got, "k", 1));
    char ch = 0;
    fact("read from the pipe", read(pp[0], &ch, 1));
    fact("the same byte", ch == 'k');
    kill(c, SIGKILL);
    waitid(P_PID, (id_t)c, NULL, WEXITED | WNOWAIT);
    err("pidfd_getfd of a zombie", p_getfd(pfd, 77, 0));
    waitpid(c, NULL, 0);
    err("pidfd_getfd after the reap", p_getfd(pfd, 77, 0));
    close(got);
    close(pfd);
    close(pp[0]);
}

static void fifo(void) {
    const char *path = "/tmp/fds_more.fifo";
    unlink(path);
    fact("mkfifo", mkfifo(path, 0600));
    int r = open(path, O_RDONLY | O_NONBLOCK);
    polled("reader, no writer yet", r, POLLIN, 0);
    char ch;
    fact("read with no writer yet", read(r, &ch, 1));
    int w = open(path, O_WRONLY | O_NONBLOCK);
    polled("reader, a writer and no data", r, POLLIN, 0);
    polled("writer", w, POLLOUT, 0);
    fact("write", write(w, "a", 1));
    polled("reader, data", r, POLLIN, 0);
    close(w);
    polled("reader, data and the writer gone", r, POLLIN, 0);
    fact("read", read(r, &ch, 1));
    polled("reader, the writer gone", r, POLLIN, 0);
    w = open(path, O_WRONLY | O_NONBLOCK);
    polled("reader, a second writer", r, POLLIN, 0);
    close(r);
    polled("writer, the reader gone", w, POLLOUT, 0);
    close(w);

    int rw = open(path, O_RDWR | O_NONBLOCK);       /* both ends in one descriptor: never hangs up, writable while there is room */
    polled("O_RDWR, empty", rw, POLLIN | POLLOUT, 0);
    char block[4096];
    memset(block, 'z', sizeof block);
    long total = 0;
    for (;;) { ssize_t n = write(rw, block, sizeof block); if (n <= 0) break; total += n; }
    fact("filled it", total > 0);
    polled("O_RDWR, full", rw, POLLIN | POLLOUT, 0);
    pid_t c = fork();
    if (!c) {                                       /* after a while a reader takes all of it; it stays (its close would wake everybody anyway) */
        usleep(300000);
        int fd = open(path, O_RDONLY);
        long left = total;
        while (left > 0) { ssize_t n = read(fd, block, sizeof block); if (n <= 0) break; left -= n; }
        for (;;) pause();
    }
    long t0 = now_ms();
    polled("O_RDWR, waiting for room", rw, POLLOUT, 20000);
    fact("woken well before the timeout", now_ms() - t0 < 10000);
    kill(c, SIGKILL);
    waitpid(c, NULL, 0);
    close(rw);
    unlink(path);
}

static void wakeups(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, NULL);
    int sfd = signalfd(-1, &set, SFD_NONBLOCK);
    pid_t parent = getpid();
    pid_t c = fork();
    if (!c) { usleep(300000); kill(parent, SIGUSR1); usleep(300000); _exit(0); }
    int pfd = (int)p_open(c, 0);
    polled("signalfd, nothing yet", sfd, POLLIN, 0);
    polled("pidfd, still running", pfd, POLLIN, 0);
    long t0 = now_ms();
    polled("signalfd, waiting for the signal", sfd, POLLIN, 20000);
    fact("woken well before the timeout", now_ms() - t0 < 10000);
    struct signalfd_siginfo si;
    fact("one record", read(sfd, &si, sizeof si) == (ssize_t)sizeof si);
    fact("SIGUSR1 from the child", si.ssi_signo == SIGUSR1 && (pid_t)si.ssi_pid == c);
    t0 = now_ms();
    polled("pidfd, waiting for the exit", pfd, POLLIN, 20000);
    fact("woken well before the timeout", now_ms() - t0 < 10000);
    waitpid(c, NULL, 0);
    close(pfd);
    close(sfd);
}

static void *sleeper(void *arg) { *(volatile pid_t *)arg = (pid_t)syscall(SYS_gettid); for (;;) pause(); return NULL; }

/* pidfd_open(2): the id of a thread that is not the thread group's leader is ENOENT on the Linux of the oracle (6.18; EINVAL on older ones); the leader's id is the pid. */
static void threadid(void) {
    static volatile pid_t tid;
    pthread_t th;
    pthread_create(&th, NULL, sleeper, (void *)&tid);
    while (!tid) usleep(1000);
    fact("a second thread has an id of its own", tid != getpid());
    err("pidfd_open of a thread id", p_open(tid, 0));
    err("pidfd_open of the first thread's id", p_open(syscall(SYS_gettid), 0));
    pthread_cancel(th);
    pthread_join(th, NULL);
}

int main(void) {
    threadid();
    zombie();
    getfd();
    fifo();
    wakeups();
    printf("done\n");
    return 0;
}
