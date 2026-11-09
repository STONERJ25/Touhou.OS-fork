/*
 * Readiness for poll and select (the interface is described in include/pollq.h).
 *
 * A file type says which wait queue it wakes when its readiness may have changed; poll and select attach a PollWaiter to the queue of every descriptor they were
 * given, look at the descriptors, and sleep on one private queue that all those waiters wake. The order matters and is the same as in every wake-up-based wait:
 * clear the "something happened" flag, look at the state, and only then sleep if the flag is still clear -- a change that comes between the look and the sleep sets
 * the flag (interrupts are off from the test to the sleep), so it cannot be lost.
 *
 * A file without a queue (some devices) cannot wake anyone; a call that holds one looks again every few milliseconds, which is all the native poll ever did.
 */
#include "pollq.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "process.h"
#include "irq.h"

#define POLL_TICK_MS 2                      /* how often a call that holds a file without a queue looks again */

/* ------------------------------------------------------------------ the observers of a wait queue */

void pollwaiter_attach(PollWaiter *w, WaitQueue *q) {
    u64 f = irq_save();
    w->q = q;
    w->next = q->pw;
    if (w->next) w->next->pprev = &w->next;
    q->pw = w;
    w->pprev = &q->pw;
    irq_restore(f);
}

void pollwaiter_detach(PollWaiter *w) {
    u64 f = irq_save();
    if (w->q) {
        *w->pprev = w->next;
        if (w->next) w->next->pprev = w->pprev;
        w->q = NULL;
        w->next = NULL;
        w->pprev = NULL;
    }
    irq_restore(f);
}

/* Called by wq_wake_one and wq_wake_all (IRQs off) when the queue has observers. A callback does not detach itself, but the loop reads the link first anyway. */
void pollq_fire(WaitQueue *q) {
    for (PollWaiter *w = q->pw; w; ) {
        PollWaiter *next = w->next;
        w->fn(w);
        w = next;
    }
}

/* ------------------------------------------------------------------ poll and select */

typedef struct Sleeper {
    WaitQueue wq;                           /* the call sleeps here; every observer wakes it */
    volatile bool hit;                      /* a wake-up happened since the flag was last cleared */
} Sleeper;

static void sleeper_wake(PollWaiter *w) {
    Sleeper *s = w->ctx;
    s->hit = true;
    wq_wake_all(&s->wq);
}

long pollq_wait(PollEntry *e, int n, u64 deadline, unsigned flags) {
    Process *p = current->proc;
    Sleeper sl;
    wq_init(&sl.wq);
    sl.hit = false;
    for (int i = 0; i < n; i++) {
        e[i].revents = 0;
        e[i].file = NULL;
        e[i].q = NULL;
        e[i].w.q = NULL;
        e[i].w.fn = sleeper_wake;
        e[i].w.ctx = &sl;
        if (e[i].fd < 0) {
            if (!(flags & PQ_SKIP_NEGATIVE)) e[i].revents = POLLNVAL;
            continue;
        }
        e[i].file = fd_get_ref(p, e[i].fd);
        if (!e[i].file) e[i].revents = POLLNVAL;
    }

    long ready, rc = 0;
    for (;;) {
        u64 f = irq_save();
        sl.hit = false;
        irq_restore(f);
        bool tick = false;
        ready = 0;
        for (int i = 0; i < n; i++) {
            if (!e[i].file) {
                if (e[i].revents == POLLNVAL) ready++;
                continue;
            }
            WaitQueue *q = vfs_pollwq(e[i].file);                    /* be on the file's queue before looking at its state; it may change (an unconnected socket) */
            if (q != e[i].q) {
                pollwaiter_detach(&e[i].w);
                e[i].q = q;
                if (q) pollwaiter_attach(&e[i].w, q);
            }
            if (!q) tick = true;
            e[i].revents = (short)(vfs_poll(e[i].file) & (e[i].events | POLLERR | POLLHUP));
            if (e[i].revents) ready++;
        }
        if (ready) break;
        u64 now = uptime_ms();
        if (now >= deadline) break;
        if (signal_pending_now(p)) { rc = -EINTR; break; }
        f = irq_save();
        if (!sl.hit) {
            u64 ms = deadline == ~0ULL ? ~0ULL : deadline - now;
            if (tick && ms > POLL_TICK_MS) ms = POLL_TICK_MS;
            current->intr = true;
            if (ms == ~0ULL) wq_block(&sl.wq);
            else wq_block_timeout(&sl.wq, ms);
            current->intr = false;
        }
        irq_restore(f);
    }

    for (int i = 0; i < n; i++) {
        pollwaiter_detach(&e[i].w);
        if (e[i].file) file_put(e[i].file);
        e[i].file = NULL;
    }
    return rc ? rc : ready;
}

long pollq_poll(struct gpollfd *fds, u64 nfds, u64 deadline, unsigned flags) {
    if (nfds > PQ_MAX_ENTRIES) return -EINVAL;
    PollEntry local[16];
    PollEntry *e = nfds <= 16 ? local : kmalloc(nfds * sizeof *e);
    if (!e) return -ENOMEM;
    for (u64 i = 0; i < nfds; i++) {
        e[i].fd = fds[i].fd;
        e[i].events = fds[i].events;
    }
    long r = pollq_wait(e, (int)nfds, deadline, flags);
    for (u64 i = 0; i < nfds; i++) fds[i].revents = e[i].revents;
    if (e != local) kfree(e);
    return r;
}
