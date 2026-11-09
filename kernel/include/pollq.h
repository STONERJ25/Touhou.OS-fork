/*
 * Readiness: how poll(2), select(2) and epoll find out which descriptors can be read or written, and wait for that to change.
 *
 * A file type takes part by providing two things (members of FileOps; a device node provides them in VNodeOps):
 *
 *   int        poll(File *f)    the bits of POLLIN, POLLOUT, POLLERR, POLLHUP, POLLPRI and POLLRDHUP that hold right now. It may sleep (the network lock).
 *   WaitQueue *pollwq(File *f)  the queue the file wakes (wq_wake_all or wq_wake_one) every time the answer of poll() may have changed: data arrived or was read,
 *                               space appeared, the peer closed, a connection completed. A spurious wake-up is harmless; a missing one is a hang. NULL means "I
 *                               cannot tell you": a waiter then looks again every few milliseconds, as the native poll always did. The queue must stay valid
 *                               until the File is released, and when the answer to pollwq() changes (an unconnected socket that starts to listen) the old
 *                               queue must be woken once at that moment, so that waiters attach to the new one.
 *
 * The rest is done here and in kernel/fs/epoll.c: a PollWaiter is attached to the queue (the hook is in wq_wake_one and wq_wake_all, so every wait queue in the
 * kernel can be watched without its owner doing anything but the wake-up it already does); poll and select wait on all their queues at once (pollq_wait); epoll
 * keeps its registrations between calls and is told by the same waiters. A file that is released (the last descriptor closed) is removed from every epoll set it
 * was in (pollq_file_released).
 *
 * The callback of a PollWaiter runs inside wq_wake_*, with interrupts off, possibly in an interrupt handler or with the network lock held: it sets a flag, links
 * a node into a list and wakes a task, and nothing else.
 */
#ifndef K_POLLQ_H
#define K_POLLQ_H

#include "vfs.h"

#ifndef POLLPRI
#define POLLPRI     0x002
#endif
#define POLLRDNORM  0x040
#define POLLRDBAND  0x080
#define POLLWRNORM  0x100
#define POLLWRBAND  0x200
#define POLLRDHUP   0x2000           /* the peer shut down its sending side or closed: Linux's EPOLLRDHUP */

typedef struct PollWaiter {
    struct PollWaiter *next, **pprev;       /* the list on WaitQueue.pw */
    WaitQueue *q;                           /* the queue it is on (NULL: not attached) */
    void (*fn)(struct PollWaiter *w);       /* runs on every wake-up of q */
    void *ctx;
} PollWaiter;

void pollwaiter_attach(PollWaiter *w, WaitQueue *q);     /* w->fn and w->ctx are set; q is not NULL */
void pollwaiter_detach(PollWaiter *w);                   /* harmless when not attached */

/* One descriptor of a poll or select call. The caller fills fd and events; revents is the result (POLLNVAL for a descriptor that is not open). */
typedef struct PollEntry {
    int   fd;
    short events;
    short revents;
    File *file;                             /* private to pollq_wait */
    WaitQueue *q;
    PollWaiter w;
} PollEntry;

#define PQ_SKIP_NEGATIVE 1                  /* a negative fd is ignored (revents 0, not counted), as POSIX poll says; the native poll counts it as POLLNVAL */

/* Waits until one of the n entries is ready (events | POLLERR | POLLHUP matches), the deadline (uptime in ms, ~0ULL for never) passes, or a signal needs attention
 * (-EINTR). Returns the number of ready entries; every revents is set. */
long pollq_wait(PollEntry *e, int n, u64 deadline, unsigned flags);

/* The same for the native poll(2) structure. */
long pollq_poll(struct gpollfd *fds, u64 nfds, u64 deadline, unsigned flags);

/* The most you can ask an fd for in one call: the descriptor table is 64 entries long, so poll and select cap the vector at this. */
#define PQ_MAX_ENTRIES 1024

#endif
