/*
 * The Linux calls that wait for events (docs/COMPAT.md): poll, ppoll, select, pselect6, epoll_create, epoll_create1, epoll_ctl, epoll_wait, epoll_pwait,
 * epoll_pwait2, eventfd, eventfd2, timerfd_create, timerfd_settime and timerfd_gettime. This file converts arguments and results (the packed 12-byte
 * struct epoll_event, itimerspec, fd_set bitmaps, timeouts that come back as the time left); the waiting itself is the readiness interface of include/pollq.h
 * (kernel/fs/pollq.c) and the descriptor types of kernel/fs/{epoll,eventfd,timerfd}.c.
 *
 * Written from the manual pages poll(2), select(2), epoll(7), epoll_create(2), epoll_ctl(2), epoll_wait(2), eventfd(2), timerfd_create(2) and signal(7) (for
 * what a signal mask given to ppoll, pselect6 and epoll_pwait means); the behaviour is checked against a real Linux by tests/linux/src/net_*.c.
 */
#include "linux.h"
#include "kernel.h"
#include "cpu.h"
#include "irq.h"
#include "numbers.h"
#include "netabi.h"
#include "calls.h"
#include "evfd.h"
#include "pollq.h"

static inline Process *me(void) { return current->proc; }

/* ------------------------------------------------------------------ small helpers */

static long install(File *f, bool cloexec) {
    long fd = fd_install(me(), f, 0, cloexec ? FD_CLOEXEC : 0);
    if (fd < 0) file_put(f);
    return fd;
}

/* A relative timeout in milliseconds (negative: never) as the uptime deadline the wait functions take. */
static u64 deadline_ms(long ms) {
    if (ms < 0) return ~0ULL;
    u64 now = uptime_ms();
    return (u64)ms >= ~0ULL - now ? ~0ULL : now + (u64)ms;
}

/* The time left until a deadline, in milliseconds (0 once it has passed). */
static u64 left_ms(u64 deadline) {
    u64 now = uptime_ms();
    return deadline == ~0ULL ? 0 : deadline > now ? deadline - now : 0;
}

/* The signal mask a ppoll, pselect6 or epoll_pwait call runs under. Linux's sigset has bit n-1 for signal n, the kernel's bit n (signal 64 in bit 0):
 * kernel/linux/signal.c converts (lnx_sigset_to_kernel), as for rt_sigsuspend. The old mask is remembered the way
 * sigsuspend remembers it (Task.saved_mask): signal_deliver puts it back on the way out of the call, after a handler has run if there was one. A signal that the new
 * mask lets through and that is already pending ends the wait at once (pollq_wait and ep_wait look at the pending signals under the mask of the moment). */
static long sigmask_swap(u64 usig, u64 size) {
    if (!usig) return 0;
    if (size != 8) return -EINVAL;
    u64 set;
    if (copy_from_user(&set, (const void *)usig, sizeof set)) return -EFAULT;
    Task *t = current;
    u64 n = lnx_sigset_to_kernel(set) & ~(sigbit(SIGKILL) | sigbit(SIGSTOP));
    u64 f = irq_save();
    t->saved_mask = t->sig_blocked;
    t->saved_mask_valid = true;
    t->sig_blocked = n;
    irq_restore(f);
    return 0;
}

/* ------------------------------------------------------------------ poll and ppoll */

static long poll_user(u64 ufds, u64 nfds, u64 deadline) {
    if (nfds > PQ_MAX_ENTRIES) return -EINVAL;
    struct lnx_pollfd *u = kmalloc(nfds ? nfds * sizeof *u : 1);
    PollEntry *e = kmalloc(nfds ? nfds * sizeof *e : 1);
    long rc = (u && e) ? 0 : -ENOMEM;
    if (!rc && nfds && copy_from_user(u, (const void *)ufds, nfds * sizeof *u)) rc = -EFAULT;
    if (!rc) {
        for (u64 i = 0; i < nfds; i++) {
            e[i].fd = u[i].fd;
            e[i].events = u[i].events;
        }
        rc = pollq_wait(e, (int)nfds, deadline, PQ_SKIP_NEGATIVE);
        if (rc >= 0) {
            for (u64 i = 0; i < nfds; i++) u[i].revents = e[i].revents;
            if (nfds && copy_to_user((void *)ufds, u, nfds * sizeof *u)) rc = -EFAULT;
        }
    }
    kfree(u);
    kfree(e);
    return rc;
}

/* poll(fds, nfds, timeout): the timeout is an int in milliseconds, negative for never. */
long lnx_poll(TrapFrame *tf) { return poll_user(A0(tf), A1(tf), deadline_ms((long)(int)A2(tf))); }

/* A timespec timeout at `uts` (NULL: never) as a deadline; *has says whether the program gave one (it gets the time left back). */
static long timeout_ts(u64 uts, u64 *deadline, bool *has) {
    *has = uts != 0;
    *deadline = ~0ULL;
    return uts ? deadline_from_timespec((const void *)uts, deadline) : 0;
}

static void timeout_ts_back(u64 uts, u64 deadline) {
    u64 ms = left_ms(deadline);
    struct lnx_timespec ts = { (s64)(ms / 1000), (s64)(ms % 1000) * 1000000 };
    copy_to_user((void *)uts, &ts, sizeof ts);
}

/* ppoll(fds, nfds, timeout, sigmask, sigsetsize): poll with a timespec that comes back as the time left, and a signal mask for the duration of the call. */
long lnx_ppoll(TrapFrame *tf) {
    u64 deadline;
    bool has;
    long rc = timeout_ts(A2(tf), &deadline, &has);
    if (rc) return rc;
    rc = sigmask_swap(A3(tf), A4(tf));
    if (rc) return rc;
    rc = poll_user(A0(tf), A1(tf), deadline);
    if (has && rc >= 0) timeout_ts_back(A2(tf), deadline);
    return rc;
}

/* ------------------------------------------------------------------ select and pselect6 */

#define FDSET_WORDS (LNX_FD_SETSIZE / 64)
#define SEL_IN  (POLLIN | POLLHUP | POLLERR)            /* what select(2) calls "readable" */
#define SEL_OUT (POLLOUT | POLLERR)
#define SEL_EX  (POLLPRI)

static bool bit_get(const u64 *set, int fd) { return (set[fd / 64] >> (fd % 64)) & 1; }
static void bit_set(u64 *set, int fd) { set[fd / 64] |= 1ULL << (fd % 64); }

/* select over three bitmaps: each descriptor named in any of them becomes one entry; afterwards the sets hold the descriptors that are ready in that sense, and the
 * result counts the bits. A descriptor that is not open is EBADF before anything waits. On a signal the sets are left as they were. */
static long select_user(int nfds, u64 ur, u64 uw, u64 ue, u64 deadline) {
    if (nfds < 0) return -EINVAL;
    if (nfds > LNX_FD_SETSIZE) nfds = LNX_FD_SETSIZE;
    if (nfds > MAX_FD) nfds = MAX_FD;                                 /* the descriptor table ends here; Linux, too, ignores the part of the sets beyond its table */
    size_t bytes = (size_t)((nfds + 63) / 64) * 8;
    u64 in[3][FDSET_WORDS], out[3][FDSET_WORDS];
    const u64 uset[3] = { ur, uw, ue };
    memset(in, 0, sizeof in);
    memset(out, 0, sizeof out);
    for (int k = 0; k < 3; k++)
        if (uset[k] && bytes && copy_from_user(in[k], (const void *)uset[k], bytes)) return -EFAULT;
    int n = 0;
    for (int fd = 0; fd < nfds; fd++)
        if (bit_get(in[0], fd) || bit_get(in[1], fd) || bit_get(in[2], fd)) {
            if (!fd_get(me(), fd)) return -EBADF;
            n++;
        }
    PollEntry *e = kmalloc(n ? (size_t)n * sizeof *e : 1);
    if (!e) return -ENOMEM;
    int i = 0;
    for (int fd = 0; fd < nfds; fd++) {
        short ev = 0;
        if (bit_get(in[0], fd)) ev |= SEL_IN;
        if (bit_get(in[1], fd)) ev |= SEL_OUT;
        if (bit_get(in[2], fd)) ev |= SEL_EX;
        if (!ev) continue;
        e[i].fd = fd;
        e[i].events = ev;
        i++;
    }
    long rc = pollq_wait(e, n, deadline, 0);
    if (rc >= 0) {
        long count = 0;
        for (i = 0; i < n; i++) {
            int fd = e[i].fd;
            short r = e[i].revents;
            if (bit_get(in[0], fd) && (r & SEL_IN)) { bit_set(out[0], fd); count++; }
            if (bit_get(in[1], fd) && (r & SEL_OUT)) { bit_set(out[1], fd); count++; }
            if (bit_get(in[2], fd) && (r & SEL_EX)) { bit_set(out[2], fd); count++; }
        }
        rc = count;
        for (int k = 0; k < 3; k++)
            if (uset[k] && bytes && copy_to_user((void *)uset[k], out[k], bytes)) rc = -EFAULT;
    }
    kfree(e);
    return rc;
}

/* A timeout of sec seconds and nsec nanoseconds from now as an uptime deadline in milliseconds; too large to count is "never". */
static u64 deadline_after(u64 sec, u64 nsec) {
    u64 now = uptime_ms();
    if (sec > (~0ULL >> 2) / 1000) return ~0ULL;
    u64 ms = sec * 1000 + nsec / 1000000 + ((nsec % 1000000) ? 1 : 0);
    return ms >= ~0ULL - now ? ~0ULL : now + ms;
}

/* select(nfds, readfds, writefds, exceptfds, timeout): the timeout is a timeval that comes back as the time left. */
long lnx_select(TrapFrame *tf) {
    u64 deadline = ~0ULL;
    bool has = A4(tf) != 0;
    if (has) {
        struct lnx_timeval tv;
        if (copy_from_user(&tv, (const void *)A4(tf), sizeof tv)) return -EFAULT;
        if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000) return -EINVAL;
        deadline = deadline_after((u64)tv.tv_sec, (u64)tv.tv_usec * 1000);
    }
    long rc = select_user((int)A0(tf), A1(tf), A2(tf), A3(tf), deadline);
    if (has && rc >= 0 && deadline != ~0ULL) {
        u64 ms = left_ms(deadline);
        struct lnx_timeval tv = { (s64)(ms / 1000), (s64)(ms % 1000) * 1000 };
        copy_to_user((void *)A4(tf), &tv, sizeof tv);
    }
    return rc;
}

/* pselect6(nfds, readfds, writefds, exceptfds, timeout, {sigmask, size}): select with a timespec and a signal mask; the last argument points to a pair. */
long lnx_pselect6(TrapFrame *tf) {
    u64 deadline;
    bool has;
    long rc = timeout_ts(A4(tf), &deadline, &has);
    if (rc) return rc;
    if (A5(tf)) {
        struct { u64 mask, size; } sig;
        if (copy_from_user(&sig, (const void *)A5(tf), sizeof sig)) return -EFAULT;
        rc = sigmask_swap(sig.mask, sig.size);
        if (rc) return rc;
    }
    rc = select_user((int)A0(tf), A1(tf), A2(tf), A3(tf), deadline);
    if (has && rc >= 0 && deadline != ~0ULL) timeout_ts_back(A4(tf), deadline);
    return rc;
}

/* ------------------------------------------------------------------ epoll */

long lnx_epoll_create1(TrapFrame *tf) {
    int flags = (int)A0(tf);
    if (flags & ~LNX_EPOLL_CLOEXEC) return -EINVAL;
    File *f;
    long rc = ep_create(&f);
    return rc ? rc : install(f, (flags & LNX_EPOLL_CLOEXEC) != 0);
}

/* epoll_create(size): the size is a hint that has been ignored since Linux 2.6.8, but it must be positive. */
long lnx_epoll_create(TrapFrame *tf) {
    if ((int)A0(tf) <= 0) return -EINVAL;
    File *f;
    long rc = ep_create(&f);
    return rc ? rc : install(f, false);
}

long lnx_epoll_ctl(TrapFrame *tf) {
    int epfd = (int)A0(tf), op = (int)A1(tf), fd = (int)A2(tf);
    struct lnx_epoll_event ev;
    memset(&ev, 0, sizeof ev);
    if (op != LNX_EPOLL_CTL_DEL && copy_from_user(&ev, (const void *)A3(tf), sizeof ev)) return -EFAULT;
    File *epf = fd_get_ref(me(), epfd);
    if (!epf) return -EBADF;
    File *target = fd_get_ref(me(), fd);
    if (!target) { file_put(epf); return -EBADF; }
    long rc;
    if (!ep_is(epf) || epfd == fd) rc = -EINVAL;
    else if (op != LNX_EPOLL_CTL_ADD && op != LNX_EPOLL_CTL_DEL && op != LNX_EPOLL_CTL_MOD) rc = -EINVAL;
    else rc = ep_ctl(epf, op, fd, target, ev.events, ev.data);
    file_put(target);
    file_put(epf);
    return rc;
}

#define EP_BATCH 512                            /* events handed over by one call at most; a program that asks for more gets this many */

static long epoll_wait_user(int epfd, u64 uevents, int maxevents, u64 deadline) {
    if (maxevents <= 0 || maxevents > 0x7fffffff / (int)sizeof(struct lnx_epoll_event)) return -EINVAL;
    File *epf = fd_get_ref(me(), epfd);
    if (!epf) return -EBADF;
    if (!ep_is(epf)) { file_put(epf); return -EINVAL; }
    int cap = maxevents < EP_BATCH ? maxevents : EP_BATCH;
    struct EpEvent kstack[16];                                  /* an event loop calls this all the time, mostly for a few events: no allocation for those */
    struct lnx_epoll_event ustack[16];
    struct EpEvent *kb = cap <= 16 ? kstack : kmalloc((size_t)cap * sizeof *kb);
    struct lnx_epoll_event *ub = cap <= 16 ? ustack : kmalloc((size_t)cap * sizeof *ub);
    long rc = (kb && ub) ? 0 : -ENOMEM;
    if (!rc) {
        rc = ep_wait(epf, kb, cap, deadline);
        if (rc > 0) {
            for (long i = 0; i < rc; i++) {
                ub[i].events = kb[i].events;
                ub[i].data = kb[i].data;
            }
            if (copy_to_user((void *)uevents, ub, (size_t)rc * sizeof *ub)) rc = -EFAULT;
        }
    }
    if (kb != kstack) kfree(kb);
    if (ub != ustack) kfree(ub);
    file_put(epf);
    return rc;
}

/* epoll_wait(epfd, events, maxevents, timeout): the timeout is an int in milliseconds, negative for never. */
long lnx_epoll_wait(TrapFrame *tf) { return epoll_wait_user((int)A0(tf), A1(tf), (int)A2(tf), deadline_ms((long)(int)A3(tf))); }

/* epoll_pwait(epfd, events, maxevents, timeout, sigmask, sigsetsize): epoll_wait under a signal mask. */
long lnx_epoll_pwait(TrapFrame *tf) {
    long rc = sigmask_swap(A4(tf), A5(tf));
    if (rc) return rc;
    return epoll_wait_user((int)A0(tf), A1(tf), (int)A2(tf), deadline_ms((long)(int)A3(tf)));
}

/* epoll_pwait2(epfd, events, maxevents, timespec, sigmask, sigsetsize): the same with a timespec (NULL: never). */
long lnx_epoll_pwait2(TrapFrame *tf) {
    u64 deadline;
    bool has;
    long rc = timeout_ts(A3(tf), &deadline, &has);
    if (rc) return rc;
    rc = sigmask_swap(A4(tf), A5(tf));
    if (rc) return rc;
    return epoll_wait_user((int)A0(tf), A1(tf), (int)A2(tf), deadline);
}

/* ------------------------------------------------------------------ eventfd and timerfd */

long lnx_eventfd2(TrapFrame *tf) {
    int flags = (int)A1(tf);
    if (flags & ~(LNX_EFD_SEMAPHORE | LNX_EFD_CLOEXEC | LNX_EFD_NONBLOCK)) return -EINVAL;
    File *f;
    long rc = eventfd_create((u32)A0(tf), (flags & LNX_EFD_SEMAPHORE ? EVFD_SEMAPHORE : 0) | (flags & LNX_EFD_NONBLOCK ? O_NONBLOCK : 0), &f);
    return rc ? rc : install(f, (flags & LNX_EFD_CLOEXEC) != 0);
}

long lnx_eventfd(TrapFrame *tf) {
    TrapFrame t = *tf;
    t.rsi = 0;
    return lnx_eventfd2(&t);
}

long lnx_timerfd_create(TrapFrame *tf) {
    int clock = (int)A0(tf), flags = (int)A1(tf);
    if (flags & ~(LNX_TFD_CLOEXEC | LNX_TFD_NONBLOCK)) return -EINVAL;
    if (clock != LNX_CLOCK_REALTIME && clock != LNX_CLOCK_MONOTONIC && clock != LNX_CLOCK_BOOTTIME) return -EINVAL;
    File *f;
    long rc = timerfd_make(clock == LNX_CLOCK_REALTIME ? 0 : 1, (flags & LNX_TFD_NONBLOCK) != 0, &f);
    return rc ? rc : install(f, (flags & LNX_TFD_CLOEXEC) != 0);
}

/* The File behind a descriptor that must be a timerfd: EBADF, or EINVAL for another kind of file. */
static long timerfd_ref(int fd, File **out) {
    File *f = fd_get_ref(me(), fd);
    if (!f) return -EBADF;
    if (!timerfd_is(f)) { file_put(f); return -EINVAL; }
    *out = f;
    return 0;
}

static long spec_from_ts(const struct lnx_timespec *ts, s64 *ns) {
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000) return -EINVAL;
    *ns = ts->tv_sec > 9000000000LL ? 9000000000LL * 1000000000LL : ts->tv_sec * 1000000000LL + ts->tv_nsec;
    return 0;
}

static void ts_from_ns(s64 ns, struct lnx_timespec *ts) {
    ts->tv_sec = ns / 1000000000;
    ts->tv_nsec = ns % 1000000000;
}

long lnx_timerfd_settime(TrapFrame *tf) {
    int flags = (int)A1(tf);
    if (flags & ~(LNX_TFD_TIMER_ABSTIME | LNX_TFD_TIMER_CANCEL_ON_SET)) return -EINVAL;
    struct lnx_itimerspec nw;
    if (copy_from_user(&nw, (const void *)A2(tf), sizeof nw)) return -EFAULT;
    struct TfdSpec spec;
    long rc = spec_from_ts(&nw.it_interval, &spec.interval_ns);
    if (!rc) rc = spec_from_ts(&nw.it_value, &spec.value_ns);
    if (rc) return rc;
    File *f;
    rc = timerfd_ref((int)A0(tf), &f);
    if (rc) return rc;
    struct TfdSpec old;
    rc = timerfd_set(f, (flags & LNX_TFD_TIMER_ABSTIME) != 0, &spec, &old);
    file_put(f);
    if (!rc && A3(tf)) {
        struct lnx_itimerspec o;
        ts_from_ns(old.interval_ns, &o.it_interval);
        ts_from_ns(old.value_ns, &o.it_value);
        if (copy_to_user((void *)A3(tf), &o, sizeof o)) rc = -EFAULT;
    }
    return rc;
}

long lnx_timerfd_gettime(TrapFrame *tf) {
    File *f;
    long rc = timerfd_ref((int)A0(tf), &f);
    if (rc) return rc;
    struct TfdSpec cur;
    rc = timerfd_get(f, &cur);
    file_put(f);
    if (!rc) {
        struct lnx_itimerspec o;
        ts_from_ns(cur.interval_ns, &o.it_interval);
        ts_from_ns(cur.value_ns, &o.it_value);
        if (copy_to_user((void *)A1(tf), &o, sizeof o)) rc = -EFAULT;
    }
    return rc;
}
