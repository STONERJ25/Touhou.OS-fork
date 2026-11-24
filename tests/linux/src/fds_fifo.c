/* Named pipes (docs/COMPAT.md, "Descriptors, FIFOs, signalfd and pidfd"): mkfifo and mknod(S_IFIFO) on tmpfs, the open rules of fifo(7) and POSIX open (a blocking
 * open meets its partner, O_RDWR never waits, O_NONBLOCK reading succeeds and writing without a reader is ENXIO), end of file, SIGPIPE/EPIPE, poll/select/epoll
 * readiness with POLLHUP, fstat, lseek's ESPIPE, FIONREAD, unlink while open, and a ping-pong between two processes over two FIFOs. Written from fifo(7), pipe(7),
 * mkfifo(3), open(2) and poll(2); every wait is either bounded or has its partner guaranteed, so nothing can block forever. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void fact(const char *what, long v) { printf("%s: %ld\n", what, v); fflush(stdout); }
static const char *ename(int e) {
    switch (e) { case 0: return "none"; case ENXIO: return "ENXIO"; case EEXIST: return "EEXIST"; case EPIPE: return "EPIPE"; case ESPIPE: return "ESPIPE"; case EAGAIN: return "EAGAIN"; default: return "other"; }
}
static void err(const char *what, long v) { printf("%s: %ld (%s)\n", what, v, v < 0 ? ename(errno) : "none"); fflush(stdout); errno = 0; }
static const char *pollbits(short r) {
    static char b[64];
    b[0] = 0;
    if (r & POLLIN) strcat(b, "IN ");
    if (r & POLLOUT) strcat(b, "OUT ");
    if (r & POLLHUP) strcat(b, "HUP ");
    if (r & POLLERR) strcat(b, "ERR ");
    if (!b[0]) strcat(b, "none");
    return b;
}
static void showpoll(const char *what, int fd) {
    struct pollfd p = { .fd = fd, .events = POLLIN | POLLOUT };
    poll(&p, 1, 0);
    printf("%s: %s\n", what, pollbits(p.revents));
    fflush(stdout);
}

static volatile int g_pipes;
static void on_pipe(int s) { (void)s; g_pipes++; }

#define F1 "/tmp/fds_fifo_a"
#define F2 "/tmp/fds_fifo_b"

int main(void) {
    unlink(F1); unlink(F2);
    err("mkfifo", mkfifo(F1, 0644));
    err("mkfifo again", mkfifo(F1, 0644));
    err("mknod S_IFIFO", mknod(F2, S_IFIFO | 0600, 0));
    struct stat st;
    stat(F1, &st);
    fact("stat: is a FIFO", S_ISFIFO(st.st_mode));
    fact("stat: permissions", st.st_mode & 0777);
    fact("stat: size", (long)st.st_size);

    /* the non-blocking open rules */
    errno = 0;
    err("write-only, non-blocking, no reader", open(F1, O_WRONLY | O_NONBLOCK));
    int r = open(F1, O_RDONLY | O_NONBLOCK);
    fact("read-only, non-blocking, no writer: opened", r >= 0);
    char buf[64];
    err("read with no writer (end of file)", read(r, buf, sizeof buf));
    int w = open(F1, O_WRONLY | O_NONBLOCK);
    fact("write-only, non-blocking, with a reader: opened", w >= 0);
    showpoll("reader, empty", r);
    showpoll("writer, empty", w);
    fact("write 5", write(w, "hello", 5));
    int n = -1;
    ioctl(r, FIONREAD, &n);
    fact("FIONREAD", n);
    showpoll("reader, 5 bytes", r);
    fstat(w, &st);
    fact("fstat: is a FIFO", S_ISFIFO(st.st_mode));
    err("lseek", lseek(r, 0, SEEK_SET));
    struct epoll_event ev = { .events = EPOLLIN, .data.u32 = 7 }, got;
    int ep = epoll_create1(0);
    epoll_ctl(ep, EPOLL_CTL_ADD, r, &ev);
    fact("epoll: ready", epoll_wait(ep, &got, 1, 0));
    fact("epoll: data", got.data.u32);
    fd_set rd;
    FD_ZERO(&rd); FD_SET(r, &rd);
    struct timeval tv = { 0, 0 };
    fact("select: readable", select(r + 1, &rd, NULL, NULL, &tv));
    close(w);
    showpoll("reader, writer gone, data left", r);
    fact("read", read(r, buf, sizeof buf));
    showpoll("reader, writer gone, empty", r);
    err("read (end of file)", read(r, buf, sizeof buf));
    w = open(F1, O_WRONLY | O_NONBLOCK);
    close(r);
    showpoll("writer, reader gone", w);
    signal(SIGPIPE, on_pipe);
    err("write without a reader", write(w, "x", 1));
    fact("SIGPIPE caught", g_pipes);
    close(w);
    close(ep);

    /* O_RDWR never waits and is both ends */
    int rw = open(F1, O_RDWR);
    fact("read-write: opened", rw >= 0);
    fact("read-write: write", write(rw, "abc", 3));
    fact("read-write: read", read(rw, buf, sizeof buf));
    close(rw);

    /* data does not outlive the last opener */
    rw = open(F1, O_RDWR);
    write(rw, "lost", 4);
    close(rw);
    r = open(F1, O_RDONLY | O_NONBLOCK);
    err("read after everybody closed (data gone)", read(r, buf, sizeof buf));
    close(r);

    /* blocking opens meet: the child writes, the parent reads */
    pid_t kid = fork();
    if (kid == 0) {
        int cw = open(F1, O_WRONLY);                    /* waits for the parent's reader */
        write(cw, "from the child", 14);
        close(cw);
        _exit(0);
    }
    r = open(F1, O_RDONLY);                             /* waits for the child's writer */
    fact("blocking open met its writer", r >= 0);
    int total = 0, k;
    while ((k = read(r, buf + total, sizeof buf - total)) > 0) total += k;
    buf[total] = 0;
    printf("read until end of file: %s (%d)\n", buf, k);
    close(r);
    waitpid(kid, NULL, 0);

    /* ping-pong over two FIFOs */
    kid = fork();
    if (kid == 0) {
        int in = open(F1, O_RDONLY), out = open(F2, O_WRONLY);
        int v;
        while (read(in, &v, sizeof v) == sizeof v) { v++; write(out, &v, sizeof v); }
        _exit(0);
    }
    int out = open(F1, O_WRONLY), in = open(F2, O_RDONLY);
    int v = 0, ok = 1;
    for (int i = 0; i < 200; i++) {
        int before = v;
        write(out, &v, sizeof v);
        if (read(in, &v, sizeof v) != sizeof v || v != before + 1) ok = 0;
    }
    fact("ping-pong: 200 rounds", ok);
    fact("ping-pong: value", v);
    unlink(F1);                                         /* unlink while open: the open files go on */
    write(out, &v, sizeof v);
    fact("after unlink: still answers", read(in, &v, sizeof v) == sizeof v && v == 201);
    close(out);
    int st2;
    waitpid(kid, &st2, 0);
    fact("child exit", WIFEXITED(st2) ? WEXITSTATUS(st2) : -1);
    close(in);
    errno = 0;
    err("open after unlink", open(F1, O_RDONLY | O_NONBLOCK));
    unlink(F2);
    return 0;
}
