/*
 * eventfd: a 64-bit counter behind a file descriptor, written from eventfd(2).
 *
 * write() adds the 8-byte number it is given to the counter; read() returns the counter and sets it to zero (or, in semaphore mode, returns 1 and takes 1 away),
 * and waits while the counter is zero. A write that would take the counter past 0xfffffffffffffffe waits until a read makes room. The descriptor is readable while
 * the counter is not zero and writable while it can take at least one more, so it works with poll, select and epoll: that is what makes it the usual way for one
 * thread to wake another thread's event loop.
 */
#include "evfd.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "process.h"

#define EVFD_MAX 0xfffffffffffffffeULL          /* the largest value the counter may hold */

typedef struct Eventfd {
    u64 counter;
    bool semaphore;
    WaitQueue wq;                               /* readers wait for a value, writers for room; both wake it */
} Eventfd;

static ssize_t efd_read(File *f, void *buf, size_t n) {
    if (n < sizeof(u64)) return -EINVAL;
    Eventfd *e = f->priv;
    u64 fl = irq_save();
    while (e->counter == 0) {
        if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EAGAIN; }
        if (signal_pending()) { irq_restore(fl); return -EINTR; }
        current->intr = true;
        wq_block(&e->wq);
        current->intr = false;
    }
    u64 v;
    if (e->semaphore) { v = 1; e->counter--; }
    else { v = e->counter; e->counter = 0; }
    wq_wake_all(&e->wq);
    irq_restore(fl);
    memcpy(buf, &v, sizeof v);
    return sizeof v;
}

static ssize_t efd_write(File *f, const void *buf, size_t n) {
    if (n < sizeof(u64)) return -EINVAL;
    Eventfd *e = f->priv;
    u64 v;
    memcpy(&v, buf, sizeof v);
    if (v == ~0ULL) return -EINVAL;
    u64 fl = irq_save();
    while (e->counter > EVFD_MAX - v) {
        if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EAGAIN; }
        if (signal_pending()) { irq_restore(fl); return -EINTR; }
        current->intr = true;
        wq_block(&e->wq);
        current->intr = false;
    }
    e->counter += v;
    wq_wake_all(&e->wq);
    irq_restore(fl);
    return sizeof v;
}

static int efd_poll(File *f) {
    Eventfd *e = f->priv;
    int m = 0;
    u64 fl = irq_save();
    if (e->counter > 0) m |= POLLIN;
    if (e->counter < EVFD_MAX) m |= POLLOUT;
    irq_restore(fl);
    return m;
}

static WaitQueue *efd_pollwq(File *f) { return &((Eventfd *)f->priv)->wq; }

static void efd_close(File *f) { kfree(f->priv); }

static const FileOps efd_fops = { .read = efd_read, .write = efd_write, .close = efd_close, .poll = efd_poll, .pollwq = efd_pollwq };
static const VNodeOps efd_node_ops = { 0 };

long eventfd_create(u64 initval, int flags, File **out) {
    Eventfd *e = kzalloc(sizeof *e);
    if (!e) return -ENOMEM;
    e->counter = initval;
    e->semaphore = (flags & EVFD_SEMAPHORE) != 0;
    wq_init(&e->wq);
    VNode *v = vnode_alloc(&efd_node_ops, NULL, S_IFIFO | 0600);
    File *f = v ? file_new(v, O_RDWR | (flags & O_NONBLOCK), &efd_fops, e) : NULL;
    if (!f) {
        if (v) vnode_put(v);
        kfree(e);
        return -ENOMEM;
    }
    *out = f;
    return 0;
}
