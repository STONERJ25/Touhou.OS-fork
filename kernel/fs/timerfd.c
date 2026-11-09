/*
 * timerfd: a timer that is read from a file descriptor, written from timerfd_create(2), timerfd_settime(2) and timerfd_gettime(2).
 *
 * The timer expires once or at a fixed interval; read() returns the number of expirations since the last read (or since the timer was set) as an 8-byte number,
 * and waits while there is none. The descriptor is readable while that number is not zero, so it works with poll, select and epoll. Times are kept in
 * milliseconds of uptime, which is the resolution of the kernel's clock: a CLOCK_REALTIME deadline is converted when the timer is set (a later step of the wall
 * clock does not move it), and sub-millisecond values round up to one millisecond.
 *
 * Who wakes whom: a reader blocked in read() sleeps with a timeout equal to the time left, so it needs nobody. Pollers (poll, select, epoll) are woken by a small
 * kernel thread that sleeps until the earliest expiry of the armed timers, counts the expirations and wakes the queue of each timer that went off. The thread is
 * started with the first timerfd and sleeps for ever while no timer is armed.
 */
#include "evfd.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "process.h"
#include "irq.h"
#include "ktime.h"

typedef struct Timerfd {
    struct Timerfd *next, **pprev;      /* the list of armed timers */
    int  clock;                         /* 0: CLOCK_REALTIME, 1: CLOCK_MONOTONIC */
    bool armed;
    u64  expiry;                        /* uptime in ms of the next expiration, while armed */
    u64  interval;                      /* ms between expirations; 0 for a timer that goes off once */
    u64  count;                         /* expirations that no read has returned yet */
    WaitQueue wq;
} Timerfd;

static const FileOps tfd_fops;
static const VNodeOps tfd_node_ops;
static Timerfd *g_armed;                /* guarded by irq_save */
static WaitQueue g_thread_wq;           /* the service thread sleeps here */
static bool g_thread_started;

bool timerfd_is(const File *f) { return f && f->fops == &tfd_fops; }

static void armed_link(Timerfd *t) {
    t->next = g_armed;
    if (t->next) t->next->pprev = &t->next;
    g_armed = t;
    t->pprev = &g_armed;
}

static void armed_unlink(Timerfd *t) {
    if (!t->pprev) return;
    *t->pprev = t->next;
    if (t->next) t->next->pprev = t->pprev;
    t->next = NULL;
    t->pprev = NULL;
}

/* Counts the expirations that have happened by `now`; true if there were any. IRQs off. */
static bool tfd_update(Timerfd *t, u64 now) {
    if (!t->armed || now < t->expiry) return false;
    if (t->interval) {
        u64 n = 1 + (now - t->expiry) / t->interval;
        t->count += n;
        t->expiry += n * t->interval;
    } else {
        t->count++;
        t->armed = false;
        armed_unlink(t);
    }
    return true;
}

static void tfd_thread(void *arg) {
    (void)arg;
    for (;;) {
        u64 fl = irq_save();
        u64 now = uptime_ms(), next = ~0ULL;
        for (Timerfd *t = g_armed, *n; t; t = n) {
            n = t->next;
            if (tfd_update(t, now)) wq_wake_all(&t->wq);
            if (t->armed && t->expiry < next) next = t->expiry;
        }
        if (next == ~0ULL) wq_block(&g_thread_wq);
        else wq_block_timeout(&g_thread_wq, next > now ? next - now : 1);
        irq_restore(fl);
    }
}

static ssize_t tfd_read(File *f, void *buf, size_t n) {
    if (n < sizeof(u64)) return -EINVAL;
    Timerfd *t = f->priv;
    u64 fl = irq_save();
    for (;;) {
        u64 now = uptime_ms();
        tfd_update(t, now);
        if (t->count) break;
        if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EAGAIN; }
        if (signal_pending()) { irq_restore(fl); return -EINTR; }
        current->intr = true;
        if (t->armed) wq_block_timeout(&t->wq, t->expiry > now ? t->expiry - now : 1);
        else wq_block(&t->wq);
        current->intr = false;
    }
    u64 v = t->count;
    t->count = 0;
    irq_restore(fl);
    memcpy(buf, &v, sizeof v);
    return sizeof v;
}

static int tfd_poll(File *f) {
    Timerfd *t = f->priv;
    u64 fl = irq_save();
    tfd_update(t, uptime_ms());
    int m = t->count ? POLLIN : 0;
    irq_restore(fl);
    return m;
}

static WaitQueue *tfd_pollwq(File *f) { return &((Timerfd *)f->priv)->wq; }

static void tfd_close(File *f) {
    Timerfd *t = f->priv;
    u64 fl = irq_save();
    armed_unlink(t);
    irq_restore(fl);
    kfree(t);
}

static const FileOps tfd_fops = { .read = tfd_read, .close = tfd_close, .poll = tfd_poll, .pollwq = tfd_pollwq };
static const VNodeOps tfd_node_ops = { 0 };

long timerfd_make(int clock, int nonblock, File **out) {
    if (!g_thread_started) {
        g_thread_started = true;
        wq_init(&g_thread_wq);
        if (!task_create("timerfd", tfd_thread, NULL)) { g_thread_started = false; return -ENOMEM; }
    }
    Timerfd *t = kzalloc(sizeof *t);
    if (!t) return -ENOMEM;
    t->clock = clock;
    wq_init(&t->wq);
    VNode *v = vnode_alloc(&tfd_node_ops, NULL, S_IFIFO | 0600);
    File *f = v ? file_new(v, O_RDWR | (nonblock ? O_NONBLOCK : 0), &tfd_fops, t) : NULL;
    if (!f) {
        if (v) vnode_put(v);
        kfree(t);
        return -ENOMEM;
    }
    *out = f;
    return 0;
}

static u64 ns_to_ms(s64 ns) { return ns <= 0 ? 0 : (u64)(ns / 1000000) + ((ns % 1000000) ? 1 : 0); }

static void spec_of(const Timerfd *t, u64 now, struct TfdSpec *s) {
    s->interval_ns = (s64)t->interval * 1000000;
    s->value_ns = t->armed ? (s64)(t->expiry > now ? t->expiry - now : 0) * 1000000 : 0;
}

long timerfd_set(File *f, bool absolute, const struct TfdSpec *nw, struct TfdSpec *old) {
    Timerfd *t = f->priv;
    u64 fl = irq_save();
    u64 now = uptime_ms();
    tfd_update(t, now);
    if (old) spec_of(t, now, old);
    if (t->armed) { t->armed = false; armed_unlink(t); }
    t->count = 0;                                                  /* a new setting forgets the expirations nobody has read */
    t->interval = 0;
    if (nw->value_ns > 0) {                                        /* a zero value disarms */
        u64 v = ns_to_ms(nw->value_ns), at;
        if (!absolute) at = now + (v ? v : 1);
        else if (t->clock == 0) { u64 wall = time_now_ms(); at = v > wall ? now + (v - wall) : now; }
        else at = v > now ? v : now;
        t->interval = ns_to_ms(nw->interval_ns);
        t->expiry = at;
        t->armed = true;
        armed_link(t);
        wq_wake_all(&g_thread_wq);                                 /* the thread must look at the new earliest deadline */
        if (at <= now) { tfd_update(t, now); wq_wake_all(&t->wq); }
    }
    irq_restore(fl);
    return 0;
}

long timerfd_get(File *f, struct TfdSpec *cur) {
    Timerfd *t = f->priv;
    u64 fl = irq_save();
    u64 now = uptime_ms();
    tfd_update(t, now);
    spec_of(t, now, cur);
    irq_restore(fl);
    return 0;
}
