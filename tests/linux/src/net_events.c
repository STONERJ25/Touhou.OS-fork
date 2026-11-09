/*
 * eventfd and timerfd through a statically linked C library: counters and semaphores, the errors of read and write, wake-ups between threads through poll and
 * epoll, timers that tick at an interval (a count within a tolerance, never an exact time), one-shot timers, absolute deadlines, timerfd_gettime and the old
 * value that timerfd_settime hands back, and the errors of timerfd_create and timerfd_settime. Prints only what the manual pages promise, so a real Linux prints
 * the same lines (tools/linux-oracle.py); tests/case_linux_net.py compares.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int has(short revents, short bit) { return (revents & bit) != 0; }

/* ---- two threads wake each other through eventfds */

static int to_worker, to_main;
static int worker_rounds;

static void *worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 50; i++) {
        uint64_t v;
        if (read(to_worker, &v, sizeof v) != sizeof v) break;
        worker_rounds++;
        v = 1;
        if (write(to_main, &v, sizeof v) != sizeof v) break;
    }
    return NULL;
}

static int wake_fd;

static void *waker(void *arg) {
    (void)arg;
    usleep(200000);
    uint64_t v = 7;
    write(wake_fd, &v, sizeof v);
    return NULL;
}

int main(void) {
    uint64_t v;

    /* a plain counter */
    int ef = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    printf("eventfd: ok %d, non-blocking %d, close-on-exec %d\n", ef >= 0, (fcntl(ef, F_GETFL) & O_NONBLOCK) != 0, (fcntl(ef, F_GETFD) & FD_CLOEXEC) != 0);
    errno = 0;
    ssize_t r = read(ef, &v, sizeof v);
    printf("read of an empty counter: %zd errno %d (EAGAIN %d)\n", r, errno, EAGAIN);
    struct pollfd pf = { ef, POLLIN | POLLOUT, 0 };
    poll(&pf, 1, 0);
    printf("poll of an empty counter: POLLIN %d POLLOUT %d\n", has(pf.revents, POLLIN), has(pf.revents, POLLOUT));
    v = 3;
    write(ef, &v, sizeof v);
    v = 4;
    write(ef, &v, sizeof v);
    pf.revents = 0;
    poll(&pf, 1, 0);
    printf("poll after two writes: POLLIN %d POLLOUT %d\n", has(pf.revents, POLLIN), has(pf.revents, POLLOUT));
    v = 0;
    r = read(ef, &v, sizeof v);
    printf("read: %zd, value %llu\n", r, (unsigned long long)v);
    errno = 0;
    r = read(ef, &v, sizeof v);
    printf("the counter was reset: %zd errno %d\n", r, errno);
    errno = 0;
    char small[4];
    r = read(ef, small, sizeof small);
    printf("read into 4 bytes: %zd errno %d (EINVAL %d)\n", r, errno, EINVAL);
    errno = 0;
    r = write(ef, small, sizeof small);
    printf("write of 4 bytes: %zd errno %d\n", r, errno);
    v = 0xffffffffffffffffULL;
    errno = 0;
    r = write(ef, &v, sizeof v);
    printf("write of 0xffffffffffffffff: %zd errno %d\n", r, errno);
    v = 0xfffffffffffffffeULL;
    r = write(ef, &v, sizeof v);
    v = 1;
    errno = 0;
    ssize_t r2 = write(ef, &v, sizeof v);
    printf("a counter at its largest value: %zd, and one more: %zd errno %d (EAGAIN %d)\n", r, r2, errno, EAGAIN);
    pf.revents = 0;
    poll(&pf, 1, 0);
    printf("poll at the largest value: POLLIN %d POLLOUT %d\n", has(pf.revents, POLLIN), has(pf.revents, POLLOUT));
    read(ef, &v, sizeof v);
    close(ef);

    /* the initial value, and semaphore mode */
    int sem = eventfd(3, EFD_SEMAPHORE | EFD_NONBLOCK);
    printf("semaphore with initial value 3, reads:");
    for (int i = 0; i < 5; i++) {
        errno = 0;
        r = read(sem, &v, sizeof v);
        if (r == sizeof v) printf(" %llu", (unsigned long long)v);
        else printf(" error %d", errno);
    }
    v = 2;
    write(sem, &v, sizeof v);
    printf("; after writing 2:");
    for (int i = 0; i < 3; i++) {
        errno = 0;
        r = read(sem, &v, sizeof v);
        if (r == sizeof v) printf(" %llu", (unsigned long long)v);
        else printf(" error %d", errno);
    }
    printf("\n");
    close(sem);
    errno = 0;
    int badflags = eventfd(0, 0x7000000);
    printf("eventfd with stray flags: %d errno %d\n", badflags, errno);

    /* a blocking read woken by another thread, and a ping-pong of 50 rounds */
    to_worker = eventfd(0, 0);
    to_main = eventfd(0, 0);
    pthread_t th;
    pthread_create(&th, NULL, worker, NULL);
    int pong = 0;
    for (int i = 0; i < 50; i++) {
        v = 1;
        write(to_worker, &v, sizeof v);
        if (read(to_main, &v, sizeof v) == sizeof v) pong++;
    }
    pthread_join(th, NULL);
    printf("ping-pong between two threads: %d rounds, the worker saw %d\n", pong, worker_rounds);
    close(to_worker);
    close(to_main);

    /* epoll waits for an eventfd that another thread writes */
    wake_fd = eventfd(0, 0);
    int ep = epoll_create1(0);
    struct epoll_event ev = { EPOLLIN, { .u32 = 42 } };
    epoll_ctl(ep, EPOLL_CTL_ADD, wake_fd, &ev);
    pthread_create(&th, NULL, waker, NULL);
    long t0 = now_ms();
    struct epoll_event out;
    int n = epoll_wait(ep, &out, 1, 10000);
    long dt = now_ms() - t0;
    printf("epoll_wait woken by a thread's write: %d event, user data %u, took at least 190 ms: %d, well before the timeout: %d\n", n, out.data.u32, dt >= 190, dt < 5000);
    v = 0;
    read(wake_fd, &v, sizeof v);
    printf("the value written: %llu\n", (unsigned long long)v);
    pthread_join(th, NULL);
    close(ep);
    close(wake_fd);

    /* timerfd: creation errors */
    errno = 0;
    int badclock = timerfd_create(12345, 0);
    printf("timerfd_create with a bad clock: %d errno %d (EINVAL %d)\n", badclock, errno, EINVAL);
    errno = 0;
    int badtf = timerfd_create(CLOCK_MONOTONIC, 0x7000000);
    printf("timerfd_create with stray flags: %d errno %d\n", badtf, errno);
    int tf = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    printf("timerfd_create: ok %d, non-blocking %d, close-on-exec %d\n", tf >= 0, (fcntl(tf, F_GETFL) & O_NONBLOCK) != 0, (fcntl(tf, F_GETFD) & FD_CLOEXEC) != 0);
    errno = 0;
    r = read(tf, &v, sizeof v);
    printf("read of a timer that is not armed: %zd errno %d (EAGAIN %d)\n", r, errno, EAGAIN);
    struct itimerspec cur;
    cur.it_value.tv_sec = 99;
    timerfd_gettime(tf, &cur);
    printf("timerfd_gettime of a disarmed timer: value %ld.%09ld interval %ld.%09ld\n", (long)cur.it_value.tv_sec, cur.it_value.tv_nsec, (long)cur.it_interval.tv_sec,
           cur.it_interval.tv_nsec);
    errno = 0;
    struct itimerspec bad = { { 0, 5 }, { 0, 1000000000 } };
    int rc = timerfd_settime(tf, 0, &bad, NULL);
    printf("timerfd_settime with nanoseconds out of range: %d errno %d\n", rc, errno);
    errno = 0;
    rc = timerfd_settime(tf, 0x40, &cur, NULL);
    printf("timerfd_settime with stray flags: %d errno %d\n", rc, errno);
    errno = 0;
    rc = timerfd_settime(0, 0, &cur, NULL);
    printf("timerfd_settime on a descriptor that is not a timer: %d errno %d (EINVAL %d)\n", rc, errno, EINVAL);

    /* an interval timer: expirations are counted */
    struct itimerspec its = { { 0, 50000000 }, { 0, 50000000 } };
    printf("arm at 50 ms, interval 50 ms: %d\n", timerfd_settime(tf, 0, &its, NULL));
    timerfd_gettime(tf, &cur);
    printf("timerfd_gettime: the interval is 50 ms %d, time left is at most 50 ms %d\n", cur.it_interval.tv_sec == 0 && cur.it_interval.tv_nsec == 50000000,
           cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec <= 50000000);
    t0 = now_ms();
    while (now_ms() - t0 < 520) usleep(20000);
    v = 0;
    r = read(tf, &v, sizeof v);
    printf("after about 520 ms: read %zd bytes, between 6 and 14 expirations (about 10): %d\n", r, v >= 6 && v <= 14);
    errno = 0;
    r = read(tf, &v, sizeof v);
    printf("reading again at once: %zd errno %d\n", r, errno);
    struct itimerspec old;
    its.it_interval.tv_nsec = 0;
    its.it_value.tv_nsec = 400000000;
    timerfd_settime(tf, 0, &its, &old);
    printf("settime hands back the old setting: interval 50 ms %d, time left at most 50 ms %d\n", old.it_interval.tv_nsec == 50000000,
           old.it_value.tv_sec == 0 && old.it_value.tv_nsec <= 50000000);

    /* poll sees the timer go off; a one-shot timer fires once */
    struct pollfd pt = { tf, POLLIN, 0 };
    n = poll(&pt, 1, 0);
    printf("poll right after arming for 400 ms: %d ready\n", n);
    t0 = now_ms();
    n = poll(&pt, 1, 10000);
    dt = now_ms() - t0;
    printf("poll with a long timeout: %d ready, POLLIN %d, took at least 350 ms: %d, well before the timeout: %d\n", n, has(pt.revents, POLLIN), dt >= 350, dt < 5000);
    v = 0;
    read(tf, &v, sizeof v);
    printf("the one-shot timer fired %llu time\n", (unsigned long long)v);
    usleep(150000);
    errno = 0;
    r = read(tf, &v, sizeof v);
    printf("and not again: %zd errno %d\n", r, errno);
    timerfd_gettime(tf, &cur);
    printf("timerfd_gettime after it fired: time left is zero %d\n", cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec == 0);

    /* disarming; an absolute deadline; blocking reads; epoll */
    its.it_value.tv_sec = 60;
    its.it_value.tv_nsec = 0;
    timerfd_settime(tf, 0, &its, NULL);
    memset(&its, 0, sizeof its);
    timerfd_settime(tf, 0, &its, &old);
    printf("disarm: the old time left was about a minute %d\n", old.it_value.tv_sec >= 58 && old.it_value.tv_sec <= 60);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    its.it_value.tv_sec = now.tv_sec;
    its.it_value.tv_nsec = now.tv_nsec + 200000000;
    if (its.it_value.tv_nsec >= 1000000000) { its.it_value.tv_sec++; its.it_value.tv_nsec -= 1000000000; }
    printf("absolute deadline 200 ms from now: %d\n", timerfd_settime(tf, TFD_TIMER_ABSTIME, &its, NULL));
    fcntl(tf, F_SETFL, fcntl(tf, F_GETFL) & ~O_NONBLOCK);
    t0 = now_ms();
    v = 0;
    r = read(tf, &v, sizeof v);
    dt = now_ms() - t0;
    printf("a blocking read: %zd bytes, count %llu, took at least 150 ms: %d, under 5 s: %d\n", r, (unsigned long long)v, dt >= 150, dt < 5000);
    clock_gettime(CLOCK_MONOTONIC, &now);
    its.it_value.tv_sec = now.tv_sec - 5;                            /* a deadline in the past goes off at once */
    its.it_value.tv_nsec = 0;
    timerfd_settime(tf, TFD_TIMER_ABSTIME, &its, NULL);
    v = 0;
    r = read(tf, &v, sizeof v);
    printf("an absolute deadline in the past: read %zd bytes, count %llu\n", r, (unsigned long long)v);

    ep = epoll_create1(0);
    ev.events = EPOLLIN;
    ev.data.u32 = 7;
    epoll_ctl(ep, EPOLL_CTL_ADD, tf, &ev);
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = 100000000;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 100000000;
    timerfd_settime(tf, 0, &its, NULL);
    int ticks = 0;
    t0 = now_ms();
    while (ticks < 5 && now_ms() - t0 < 20000) {
        n = epoll_wait(ep, &out, 1, 5000);
        if (n == 1) {
            v = 0;
            read(tf, &v, sizeof v);
            ticks += (int)v;
        }
    }
    dt = now_ms() - t0;
    printf("epoll_wait on a 100 ms timer: at least 5 expirations counted %d, took at least 450 ms: %d\n", ticks >= 5, dt >= 450);
    close(ep);
    close(tf);

    /* CLOCK_REALTIME with an absolute time already past */
    int rt = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
    struct itimerspec past = { { 0, 0 }, { 1000, 0 } };
    timerfd_settime(rt, TFD_TIMER_ABSTIME, &past, NULL);
    struct pollfd prt = { rt, POLLIN, 0 };
    poll(&prt, 1, 5000);                                                  /* the expiration is counted a moment after the call returns, on a real Linux too */
    v = 0;
    r = read(rt, &v, sizeof v);
    printf("CLOCK_REALTIME, an absolute time long ago: read %zd bytes, count %llu\n", r, (unsigned long long)v);
    close(rt);
    return 0;
}
