/*
 * epoll instances: a set of registrations (descriptor, event mask, user data) that stays between calls, and a ready list of the registrations that something has
 * happened to. Written from epoll(7), epoll_ctl(2) and epoll_wait(2).
 *
 * How it knows: every registration attaches a PollWaiter to the wait queue its file wakes (include/pollq.h). When the file wakes that queue the waiter puts the
 * registration on the ready list and wakes whoever sleeps in epoll_wait (or watches the epoll descriptor itself, which is how epoll sets nest). epoll_wait takes the
 * ready list, asks each file what is true now and reports what matches the mask:
 *   - level-triggered (the default): a registration that matched stays on the ready list, so the next call looks at it again;
 *   - EPOLLET: it leaves the list and comes back only when the file wakes the queue again, so a program must read until EAGAIN;
 *   - EPOLLONESHOT: it is reported once and then disabled until EPOLL_CTL_MOD.
 * A registration is taken off the list *before* the file is asked, so a change that happens while the answer is being worked out puts it back: no edge is lost.
 *
 * Lifetime. A registration names its file without holding a reference (closing the last descriptor must close the socket, whatever an epoll set thinks), so the
 * File keeps a list of its registrations and file_put calls pollq_file_released before the file goes. An item has a reference count of its own: one for being
 * registered, one for each epoll_wait that is working on it, so that a close or an EPOLL_CTL_DEL in another thread cannot free it under that call. Everything
 * that links or unlinks runs with interrupts off (the lock of a single-CPU kernel); item_unlink does all the unlinking in one such stretch and says whether it was
 * the one that did it.
 *
 * A file type without a wait queue cannot wake anyone: its registrations are put on the ready list again every time epoll_wait is about to sleep, and the sleep
 * is cut to a few milliseconds.
 */
#include "evfd.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "process.h"
#include "irq.h"
#include "initcall.h"

#define EP_TICK_MS 2

typedef struct EpItem EpItem;
typedef struct Epoll Epoll;

struct EpItem {
    EpItem *next;                       /* Epoll.items */
    EpItem *rnext, *rprev;              /* the ready list; valid while on_ready */
    EpItem *txnext;                     /* the chain epoll_wait is working through */
    EpItem *fnext, **fpprev;            /* File.poll_watch, the registrations that watch one file */
    Epoll *ep;
    File *file;                         /* not counted: valid while the item is linked, or while the caller holds a reference of its own */
    int  fd;                            /* the descriptor number it was registered under: with the file, the key of the registration */
    u32  events;                        /* the requested mask, with EP_ET and EP_ONESHOT */
    u64  data;
    int  refs;
    bool linked, on_ready, disabled, ticking;
    u32  last;                          /* ticking items: what was true when they were last reported (for the edge-triggered kind) */
    PollWaiter w;
};

struct Epoll {
    WaitQueue wq;                       /* epoll_wait sleeps here; whoever watches the epoll descriptor is attached here */
    EpItem *items;
    EpItem *ready_head, *ready_tail;
    Mutex ctl;                          /* one epoll_ctl at a time per instance: the existence checks and the changes are one step */
};

static const FileOps ep_fops;
static const VNodeOps ep_node_ops;
static Mutex g_nest;                    /* one epoll_ctl that nests an epoll in another at a time: the loop check walks the whole tree */

bool ep_is(const File *f) { return f && f->fops == &ep_fops; }

/* ---------------------------------------------------------------- the ready list (IRQs off) */

static void ready_add(EpItem *it) {
    if (it->on_ready || !it->linked) return;
    Epoll *ep = it->ep;
    it->on_ready = true;
    it->rnext = NULL;
    it->rprev = ep->ready_tail;
    if (ep->ready_tail) ep->ready_tail->rnext = it; else ep->ready_head = it;
    ep->ready_tail = it;
}

static void ready_add_head(EpItem *it) {
    if (it->on_ready || !it->linked) return;
    Epoll *ep = it->ep;
    it->on_ready = true;
    it->rprev = NULL;
    it->rnext = ep->ready_head;
    if (ep->ready_head) ep->ready_head->rprev = it; else ep->ready_tail = it;
    ep->ready_head = it;
}

static void ready_del(EpItem *it) {
    if (!it->on_ready) return;
    Epoll *ep = it->ep;
    if (it->rprev) it->rprev->rnext = it->rnext; else ep->ready_head = it->rnext;
    if (it->rnext) it->rnext->rprev = it->rprev; else ep->ready_tail = it->rprev;
    it->on_ready = false;
    it->rnext = it->rprev = NULL;
}

/* The file woke its queue: the registration has something to report (maybe). Runs inside wq_wake_*, IRQs off. */
static void item_wake(PollWaiter *w) {
    EpItem *it = w->ctx;
    if (!it->disabled) ready_add(it);
    wq_wake_all(&it->ep->wq);
}

/* ---------------------------------------------------------------- items */

/* Takes the item out of every list, detaches its waiter and drops the registration's reference... that last step is the caller's (item_put), because it has to
 * happen with interrupts on. Returns false if somebody else had already done it. IRQs off. */
static bool item_unlink(EpItem *it) {
    if (!it->linked) return false;
    it->linked = false;
    ready_del(it);
    pollwaiter_detach(&it->w);
    for (EpItem **pp = &it->ep->items; *pp; pp = &(*pp)->next)
        if (*pp == it) { *pp = it->next; break; }
    if (it->fpprev) {
        *it->fpprev = it->fnext;
        if (it->fnext) it->fnext->fpprev = it->fpprev;
    }
    it->fnext = NULL;
    it->fpprev = NULL;
    return true;
}

static void item_put(EpItem *it) {
    u64 f = irq_save();
    int r = --it->refs;
    irq_restore(f);
    if (r == 0) kfree(it);
}

static EpItem *item_find(Epoll *ep, File *file, int fd) {            /* IRQs off */
    for (EpItem *it = ep->items; it; it = it->next)
        if (it->file == file && it->fd == fd) return it;
    return NULL;
}

/* What is true of the item's file now. The caller holds a reference on the file. First make sure the item is on the file's current queue (it can change, for a
 * socket that starts to listen), and only then look: a change after the attach is a wake-up, a change before it is in the answer. */
static u32 item_poll(EpItem *it) {
    WaitQueue *q = vfs_pollwq(it->file);
    u64 f = irq_save();
    if (it->linked && q != it->w.q) {
        pollwaiter_detach(&it->w);
        if (q) pollwaiter_attach(&it->w, q);
    }
    it->ticking = q == NULL;
    irq_restore(f);
    u32 m = (u32)vfs_poll(it->file);
    if (m & POLLIN) m |= POLLRDNORM;
    if (m & POLLOUT) m |= POLLWRNORM;
    return m;
}

#define ALWAYS (POLLERR | POLLHUP)

/* ---------------------------------------------------------------- epoll_wait */

/* A reference on the file if it is not already on its way out (the last reference gone, the release hook not yet run). IRQs off. */
static bool file_try_ref(File *f) {
    if (f->refs <= 0) return false;
    f->refs++;
    return true;
}

/* Reports up to max events. Works through a private chain of the registrations that were on the ready list; the ones it has no room for go back to the front of
 * the list, in their order, so that a busy descriptor early in the list cannot starve the others. */
static int ep_collect(Epoll *ep, struct EpEvent *out, int max) {
    u64 fl = irq_save();
    EpItem *chain = NULL, **tail = &chain;
    while (ep->ready_head) {
        EpItem *it = ep->ready_head;
        ready_del(it);
        it->refs++;
        it->txnext = NULL;
        *tail = it;
        tail = &it->txnext;
    }
    irq_restore(fl);

    int n = 0;
    EpItem *it = chain;
    for (; it && n < max; ) {
        EpItem *next = it->txnext;
        File *file = NULL;
        fl = irq_save();
        if (it->linked && !it->disabled && file_try_ref(it->file)) file = it->file;
        irq_restore(fl);
        if (file) {
            u32 m = item_poll(it) & (it->events | ALWAYS);
            bool edge = true;
            if (m && it->ticking && (it->events & EP_ET)) { edge = (m & ~it->last) != 0; it->last = m; }       /* no wake-ups to count: compare with the last answer */
            if (!m) it->last = 0;
            if (m && edge) {
                out[n].events = m;
                out[n].data = it->data;
                n++;
                fl = irq_save();
                if (it->events & EP_ONESHOT) it->disabled = true;
                else if (!(it->events & EP_ET)) ready_add(it);                      /* level-triggered: ask again next time */
                irq_restore(fl);
            }
            file_put(file);                                                          /* may release the file and, through the hook, this item: our reference keeps its memory */
        }
        item_put(it);
        it = next;
    }
    if (it) {                                                                        /* no room left: the rest keeps its place at the front */
        EpItem *rev = NULL;
        for (EpItem *r = it, *rn; r; r = rn) { rn = r->txnext; r->txnext = rev; rev = r; }
        for (EpItem *r = rev, *rn; r; r = rn) {
            rn = r->txnext;
            fl = irq_save();
            ready_add_head(r);
            irq_restore(fl);
            item_put(r);
        }
    }
    return n;
}

/* Puts the registrations of files without a wait queue on the ready list (they get no wake-ups, so they are looked at every time). True if there are any. */
static bool ep_add_ticking(Epoll *ep) {
    bool any = false;
    u64 fl = irq_save();
    for (EpItem *it = ep->items; it; it = it->next)
        if (it->ticking && !it->disabled) { ready_add(it); any = true; }
    irq_restore(fl);
    return any;
}

long ep_wait(File *epf, struct EpEvent *out, int max, u64 deadline) {
    Epoll *ep = epf->priv;
    Process *p = current->proc;
    for (;;) {
        bool tick = ep_add_ticking(ep);
        int n = ep_collect(ep, out, max);
        if (n) return n;
        u64 now = uptime_ms();
        if (now >= deadline) return 0;
        if (signal_pending_now(p)) return -EINTR;
        u64 fl = irq_save();
        if (!ep->ready_head) {                                                       /* nothing arrived while we were looking: sleep until something does */
            u64 ms = deadline == ~0ULL ? ~0ULL : deadline - now;
            if (tick && ms > EP_TICK_MS) ms = EP_TICK_MS;
            current->intr = true;
            if (ms == ~0ULL) wq_block(&ep->wq);
            else wq_block_timeout(&ep->wq, ms);
            current->intr = false;
        }
        irq_restore(fl);
    }
}

/* ---------------------------------------------------------------- epoll_ctl */

/* Does the epoll `from` hold `target`, directly or through other epoll sets? Returns the depth of the deepest chain below `from` (1 for a plain set), or -1 if
 * the target is found. Called with IRQs off; the depth is bounded so that a corrupt tree could not recurse for ever. */
static int ep_walk(Epoll *from, Epoll *target, int level) {
    int deepest = 1;
    if (level > EP_MAX_DEPTH) return level;
    for (EpItem *it = from->items; it; it = it->next) {
        if (!ep_is(it->file)) continue;
        Epoll *inner = it->file->priv;
        if (inner == target) return -1;
        int d = ep_walk(inner, target, level + 1);
        if (d < 0) return -1;
        if (d + 1 > deepest) deepest = d + 1;
    }
    return deepest;
}

long ep_ctl(File *epf, int op, int fd, File *tf, u32 events, u64 data) {
    Epoll *ep = epf->priv;
    if (tf == epf) return -EINVAL;
    if (!tf->fops && tf->vn && (S_ISREG(tf->vn->mode) || S_ISDIR(tf->vn->mode))) return -EPERM;       /* files that cannot be polled */
    long rc = 0;
    mutex_lock(&ep->ctl);
    u64 fl = irq_save();
    EpItem *it = item_find(ep, tf, fd);
    irq_restore(fl);

    if (op == EP_CTL_ADD) {
        if (it) { rc = -EEXIST; goto out; }
        if (ep_is(tf)) {                                                              /* an epoll set inside an epoll set: no loops, and not too deep */
            mutex_lock(&g_nest);
            fl = irq_save();
            int d = ep_walk(tf->priv, ep, 1);
            irq_restore(fl);
            mutex_unlock(&g_nest);
            if (d < 0) { rc = -ELOOP; goto out; }
            if (d >= EP_MAX_DEPTH) { rc = -EINVAL; goto out; }
        }
        it = kzalloc(sizeof *it);
        if (!it) { rc = -ENOMEM; goto out; }
        it->ep = ep;
        it->file = tf;
        it->fd = fd;
        it->events = events;
        it->data = data;
        it->refs = 1;
        it->w.fn = item_wake;
        it->w.ctx = it;
        fl = irq_save();
        it->linked = true;
        it->next = ep->items;
        ep->items = it;
        it->fnext = tf->poll_watch;
        if (it->fnext) it->fnext->fpprev = &it->fnext;
        tf->poll_watch = it;
        it->fpprev = (EpItem **)&tf->poll_watch;
        irq_restore(fl);
    } else if (op == EP_CTL_DEL) {
        if (!it) { rc = -ENOENT; goto out; }
        fl = irq_save();
        bool did = item_unlink(it);
        irq_restore(fl);
        if (did) item_put(it);
        goto out;
    } else if (op == EP_CTL_MOD) {
        if (!it) { rc = -ENOENT; goto out; }
        fl = irq_save();
        it->events = events;
        it->data = data;
        it->disabled = false;                                                         /* the way to re-arm a one-shot registration */
        it->last = 0;
        irq_restore(fl);
    } else {
        rc = -EINVAL;
        goto out;
    }

    /* a new or changed registration may be ready already: put it on the list so the next epoll_wait sees it */
    if (item_poll(it) & (events | ALWAYS)) {
        fl = irq_save();
        ready_add(it);
        wq_wake_all(&ep->wq);
        irq_restore(fl);
    }
out:
    mutex_unlock(&ep->ctl);
    return rc;
}

/* ---------------------------------------------------------------- the epoll file */

/* The epoll descriptor is readable when some registration has something to report. The ready list may hold registrations whose files have settled since, so
 * the first few are asked again. */
static int ep_poll(File *f) {
    Epoll *ep = f->priv;
    EpItem *probe[8];
    int np = 0;
    u64 fl = irq_save();
    for (EpItem *it = ep->ready_head; it && np < 8; it = it->rnext) {
        it->refs++;
        probe[np++] = it;
    }
    irq_restore(fl);
    int m = 0;
    for (int i = 0; i < np; i++) {
        EpItem *it = probe[i];
        File *file = NULL;
        fl = irq_save();
        if (it->linked && !it->disabled && file_try_ref(it->file)) file = it->file;
        irq_restore(fl);
        if (file) {
            if (item_poll(it) & (it->events | ALWAYS)) m |= POLLIN;
            file_put(file);
        }
        item_put(it);
    }
    return m;
}

static WaitQueue *ep_pollwq(File *f) { return &((Epoll *)f->priv)->wq; }

static void ep_close(File *f) {
    Epoll *ep = f->priv;
    for (;;) {
        u64 fl = irq_save();
        EpItem *it = ep->items;
        bool did = it && item_unlink(it);
        irq_restore(fl);
        if (!did) break;
        item_put(it);
    }
    kfree(ep);
}

static const FileOps ep_fops = { .close = ep_close, .poll = ep_poll, .pollwq = ep_pollwq };
static const VNodeOps ep_node_ops = { 0 };

long ep_create(File **out) {
    Epoll *ep = kzalloc(sizeof *ep);
    if (!ep) return -ENOMEM;
    wq_init(&ep->wq);
    mutex_init(&ep->ctl);
    VNode *v = vnode_alloc(&ep_node_ops, NULL, S_IFIFO | 0600);
    File *f = v ? file_new(v, O_RDWR, &ep_fops, ep) : NULL;
    if (!f) {
        if (v) vnode_put(v);
        kfree(ep);
        return -ENOMEM;
    }
    *out = f;
    return 0;
}

/* The last reference of `f` is gone and file_put is about to close it: every epoll registration that names it must go first (the file is in the lists of the
 * epoll sets, and its wait queue is about to be freed). */
void pollq_file_released(File *f) {
    for (;;) {
        u64 fl = irq_save();
        EpItem *it = f->poll_watch;
        bool did = it && item_unlink(it);
        irq_restore(fl);
        if (!did) break;
        item_put(it);
    }
}

static void epoll_init(void) { mutex_init(&g_nest); }
INITCALL(epoll_init);
