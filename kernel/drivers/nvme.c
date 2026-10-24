/*
 * NVMe driver: finds every NVM Express controller on the PCI bus, brings it up (disable, admin queue, enable, identify),
 * creates one polled I/O queue pair and registers each active namespace as a BlockDev (nvme0n1, nvme0n2, nvme1n1, ...).
 *
 * Design in one paragraph: polled, one I/O queue pair per controller, shared by all its namespaces and protected by a
 * mutex so any number of threads can issue I/O. A request is cut into commands of at most 128 KiB (or MDTS, whichever is
 * smaller); up to `window` of them are kept in flight at once (a sliding window over command ids 0..window-1, each with its
 * own PRP list page), completions are matched by command id and the phase tag. Buffers that are not dword aligned go
 * through a bounce buffer. Waiting is a short spin followed by 1 ms sleeps, so idle costs nothing and SSD-speed
 * transfers still flow. A controller that sets CSTS.CFS or stops answering is disabled and every later request fails fast
 * with -EIO; nothing ever waits forever. No interrupts, no hot-plug, no power-state management, no shutdown
 * notification (see docs/BLOCKDEVICES.md).
 */
#include "kernel.h"
#include "blk.h"
#include "pci.h"
#include "mm.h"
#include "task.h"
#include "irq.h"
#include "cpu.h"
#include "cmdline.h"
#include "vfs.h"
#include "blkdrv_util.h"

void blk_selftest(const char *driver);          /* blk_selftest.c: optional in-kernel I/O check driven by the command line */

/* ---------------------------------------------------------------- registers and commands */

#define NVME_CAP     0x00           /* 64 bit */
#define NVME_VS      0x08
#define NVME_INTMS   0x0C
#define NVME_CC      0x14
#define NVME_CSTS    0x1C
#define NVME_AQA     0x24
#define NVME_ASQ     0x28           /* 64 bit */
#define NVME_ACQ     0x30           /* 64 bit */
#define NVME_DB      0x1000

#define CC_EN        0x00000001u
#define CC_IOSQES(n) ((u32)(n) << 16)
#define CC_IOCQES(n) ((u32)(n) << 20)
#define CSTS_RDY     0x00000001u
#define CSTS_CFS     0x00000002u

#define ADM_DELETE_SQ  0x00
#define ADM_CREATE_SQ  0x01
#define ADM_DELETE_CQ  0x04
#define ADM_CREATE_CQ  0x05
#define ADM_IDENTIFY   0x06
#define ADM_SET_FEAT   0x09
#define NVM_FLUSH      0x00
#define NVM_WRITE      0x01
#define NVM_READ       0x02

typedef struct {
    u8  opc;
    u8  flags;                      /* FUSE, PSDT (0: PRPs) */
    u16 cid;
    u32 nsid;
    u64 rsvd;
    u64 mptr;
    u64 prp1;
    u64 prp2;
    u32 cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} NvmeCmd;

typedef struct {
    u32 dw0;
    u32 dw1;
    u16 sq_head;
    u16 sq_id;
    u16 cid;
    u16 status;                     /* bit 0 phase tag, bits 15:1 status field */
} NvmeCqe;

_Static_assert(sizeof(NvmeCmd) == 64, "NVMe submission entries are 64 bytes");
_Static_assert(sizeof(NvmeCqe) == 16, "NVMe completion entries are 16 bytes");

/* ---------------------------------------------------------------- limits and timeouts */

#define NVME_MAX_XFER      (128u << 10)         /* bytes per command, never more (MDTS may lower it) */
#define NVME_WINDOW_MAX    16                   /* commands in flight per request */
#define NVME_IO_DEPTH      128
#define NVME_ADMIN_DEPTH   16
#define NVME_BOUNCE_BYTES  (128u << 10)
#define NVME_MAX_NS        32

#define NVME_RETRIES       3                    /* extra attempts for a command that failed with the do-not-retry bit clear */

#define T_ADMIN_MS         15000
#define T_IO_MS            15000
#define T_FLUSH_MS         30000

/* ---------------------------------------------------------------- state */

typedef struct NvmeCtrl NvmeCtrl;

typedef struct {
    u16  id, depth;
    u16  sq_tail, cq_head;
    u8   phase;
    NvmeCmd *sq;  paddr_t sq_phys;
    NvmeCqe *cq;  paddr_t cq_phys;
    volatile u32 *sq_db, *cq_db;
} NvmeQueue;

typedef struct {
    NvmeCtrl *ctrl;
    u32  nsid;
    u32  sector_size;
    u64  sectors;
    BlockDev *bd;
} NvmeNs;

struct NvmeCtrl {
    PciDev *pci;
    volatile u8 *regs;
    int    index;
    u64    cap;
    u32    vs;
    u32    db_stride;                           /* bytes between doorbells */
    u32    ready_timeout_ms;
    u32    mqes;                                /* queue entries supported */

    NvmeQueue admin, io;
    Mutex  admin_lock, io_lock;
    u16    admin_cid;

    void  *ident;   paddr_t ident_phys;         /* 4 KiB identify buffer */
    void  *prp;     paddr_t prp_phys;           /* one PRP list page per window slot */
    u8    *bounce;  paddr_t bounce_phys;        /* allocated on first use */
    u32    max_xfer;
    u32    window;

    char   model[48], serial[24], firmware[12];
    u8     mdts;
    bool   vwc;                                 /* volatile write cache present: writes are followed by a flush */
    u32    nn;
    u32    last_status;                         /* status field of the last failed admin command */

    bool   dead;
    NvmeNs *ns[NVME_MAX_NS];
    int    nns;
    u32    n_cmds, n_errors, n_flushes, n_bounced;
    NvmeCtrl *next;
};

static NvmeCtrl *g_head, *g_tail;
static int g_nctrl;

/* Test knobs (kernel command line); the defaults are the production behaviour. */
static u32  g_maxxfer = NVME_MAX_XFER;          /* nvme.maxxfer=N   bytes per command */
static u32  g_window = NVME_WINDOW_MAX;         /* nvme.window=N    commands in flight */
static u32  g_depth = NVME_IO_DEPTH;            /* nvme.depth=N     I/O queue entries (small values exercise queue wrap-around) */
static bool g_force_bounce;                     /* nvme.bounce      copy every transfer through the bounce buffer */
static u32  g_io_timeout = T_IO_MS;             /* nvme.iotimeout=N ms to wait for a read/write completion */
static u32  g_drop_after;                       /* nvme.dropcmd=N   never ring the doorbell for the N-th read/write batch (a lost command) */
static u32  g_batches;
static bool g_nosync;                           /* nvme.nosync      do not flush the write cache after every write request */
static u64  g_data_limit = DMA_LIMIT;           /* blk.dmalimit_mb=N  treat caller memory above N MiB as unreachable (tests the bounce path) */

#define REG32(c, off)       mmio_r32((c)->regs, (off))
#define WREG32(c, off, v)   mmio_w32((c)->regs, (off), (v))

static u64 reg64(NvmeCtrl *c, u32 off) { return (u64)REG32(c, off) | (u64)REG32(c, off + 4) << 32; }
static void wreg64(NvmeCtrl *c, u32 off, u64 v) { WREG32(c, off, lo32(v)); WREG32(c, off + 4, hi32(v)); }

static inline u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static inline u64 le64(const u8 *p) { return (u64)le32(p) | (u64)le32(p + 4) << 32; }

/* ---------------------------------------------------------------- queues */

static bool cq_ready(const NvmeQueue *q) {
    return ((((volatile const NvmeCqe *)&q->cq[q->cq_head])->status) & 1) == q->phase;
}

/* Takes the entry at the head of the completion queue (the caller checked cq_ready). The doorbell is rung by cq_ring. */
static void cq_pop(NvmeQueue *q, NvmeCqe *out) {
    volatile const NvmeCqe *e = &q->cq[q->cq_head];
    barrier();                                      /* the phase bit was read first; only now look at the rest of the entry */
    out->dw0 = e->dw0;
    out->dw1 = e->dw1;
    out->sq_head = e->sq_head;
    out->sq_id = e->sq_id;
    out->cid = e->cid;
    out->status = e->status;
    if (++q->cq_head == q->depth) { q->cq_head = 0; q->phase ^= 1; }
}

static void cq_ring(NvmeQueue *q) { *q->cq_db = q->cq_head; }

static void sq_submit(NvmeQueue *q, const NvmeCmd *cmd) {
    memcpy(&q->sq[q->sq_tail], cmd, sizeof *cmd);
    if (++q->sq_tail == q->depth) q->sq_tail = 0;
}

static void sq_ring(NvmeQueue *q) {
    barrier();                                      /* the entries must be visible before the controller is told about them */
    *q->sq_db = q->sq_tail;
}

static bool queue_alloc(NvmeCtrl *c, NvmeQueue *q, u16 id, u16 depth) {
    memset(q, 0, sizeof *q);
    q->id = id;
    q->depth = depth;
    q->phase = 1;
    q->sq = dma_alloc_low((size_t)depth * sizeof(NvmeCmd), &q->sq_phys);
    q->cq = dma_alloc_low((size_t)depth * sizeof(NvmeCqe), &q->cq_phys);
    if (!q->sq || !q->cq) return false;
    q->sq_db = (volatile u32 *)(c->regs + NVME_DB + (2u * id) * c->db_stride);
    q->cq_db = (volatile u32 *)(c->regs + NVME_DB + (2u * id + 1) * c->db_stride);
    return true;
}

/* Waits for a completion to appear (true) or for the controller to report a fatal status / the timeout to pass (false). */
static bool wait_cqe(NvmeCtrl *c, NvmeQueue *q, u32 timeout_ms) {
    BLK_POLL(cq_ready(q) || (REG32(c, NVME_CSTS) & CSTS_CFS), timeout_ms);
    return cq_ready(q);
}

/*
 * Stops the controller so that nothing DMAs into memory we are about to reuse. A hung controller may ignore CC.EN = 0 for a
 * long time, but a PCI function with bus mastering disabled cannot issue memory requests whatever its firmware is doing, so
 * that goes first; CC = 0 then resets the controller so that whoever comes next finds it sane.
 */
static void ctrl_stop_dma(NvmeCtrl *c) {
    pci_write16(c->pci, 4, pci_read16(c->pci, 4) & (u16)~PCI_CMD_MASTER);
    WREG32(c, NVME_CC, 0);
    BLK_POLL(!(REG32(c, NVME_CSTS) & CSTS_RDY), c->ready_timeout_ms);
}

/* Makes every later request fail fast and stops the controller. */
static void ctrl_fail(NvmeCtrl *c, const char *why) {
    if (c->dead) return;
    c->dead = true;
    klog("Kappa", "nvme%d: %s (CSTS %#x); giving up on this controller", c->index, why, REG32(c, NVME_CSTS));
    ctrl_stop_dma(c);
}

static const char *admin_name(u8 opc) {
    switch (opc) {
    case ADM_CREATE_CQ: return "create I/O completion queue";
    case ADM_CREATE_SQ: return "create I/O submission queue";
    case ADM_IDENTIFY: return "identify";
    case ADM_SET_FEAT: return "set features";
    default: return "admin command";
    }
}

/* Runs one admin command and waits for it. Returns 0, -EIO (the controller said no; see last_status) or -ETIMEDOUT. */
static int nvme_admin(NvmeCtrl *c, NvmeCmd *cmd, u32 *result, u32 timeout_ms) {
    if (c->dead) return -EIO;
    mutex_lock(&c->admin_lock);
    if (c->dead) { mutex_unlock(&c->admin_lock); return -EIO; }
    NvmeQueue *q = &c->admin;
    cmd->cid = ++c->admin_cid;
    sq_submit(q, cmd);
    sq_ring(q);
    int rc;
    NvmeCqe e;
    if (!wait_cqe(c, q, timeout_ms)) {
        ctrl_fail(c, (REG32(c, NVME_CSTS) & CSTS_CFS) ? "controller fatal status" : "admin command timed out");
        rc = -ETIMEDOUT;
    } else {
        cq_pop(q, &e);
        cq_ring(q);
        u32 st = e.status >> 1;
        c->last_status = st;
        if (e.cid != cmd->cid) {
            ctrl_fail(c, "completion for the wrong command");
            rc = -EIO;
        } else if (st & 0x7FF) {
            klog("Kappa", "nvme%d: %s failed: status type %u code %#x", c->index, admin_name(cmd->opc), (st >> 8) & 7, st & 0xFF);
            rc = -EIO;
        } else {
            if (result) *result = e.dw0;
            rc = 0;
        }
    }
    mutex_unlock(&c->admin_lock);
    return rc;
}

static int nvme_identify(NvmeCtrl *c, u32 cns, u32 nsid) {
    memset(c->ident, 0, 4096);
    NvmeCmd cmd = { .opc = ADM_IDENTIFY, .nsid = nsid, .prp1 = c->ident_phys, .cdw10 = cns };
    return nvme_admin(c, &cmd, NULL, T_ADMIN_MS);
}

/* ---------------------------------------------------------------- data path */

/*
 * Fills PRP1/PRP2 for len bytes at buf (dword aligned, at most NVME_MAX_XFER). The first PRP may carry the page offset; every
 * further page is page aligned. Two pages fit in PRP1+PRP2; more need a list, one page per window slot is plenty.
 */
static void build_prps(NvmeCtrl *c, unsigned slot, const u8 *buf, size_t len, u64 *prp1, u64 *prp2) {
    paddr_t first = virt_to_phys(buf);
    size_t first_len = MIN(len, (size_t)(PAGE_SIZE - (first & (PAGE_SIZE - 1))));
    *prp1 = first;
    *prp2 = 0;
    if (len <= first_len) return;
    const u8 *next = buf + first_len;
    size_t pages = (len - first_len + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 1) { *prp2 = virt_to_phys(next); return; }
    u64 *list = (u64 *)((u8 *)c->prp + (size_t)slot * PAGE_SIZE);
    for (size_t i = 0; i < pages; i++) list[i] = virt_to_phys(next + i * PAGE_SIZE);
    *prp2 = c->prp_phys + (size_t)slot * PAGE_SIZE;
}

static void log_io_error(NvmeNs *ns, bool write, u64 lba, u32 n, u32 st) {
    NvmeCtrl *c = ns->ctrl;
    c->n_errors++;
    klog("Kappa", "nvme%d: %s of %u blocks at lba %lu on nsid %u failed: status type %u code %#x%s", c->index, write ? "write" : "read", n,
         (unsigned long)lba, ns->nsid, (st >> 8) & 7, st & 0xFF, (st & 0x4000) ? " (do not retry)" : "");
}

typedef struct {
    u64 lba;
    u32 n;
    u8 *buf;
    u8  tries;                                      /* how often this command was resubmitted after a retryable error */
} WindowSlot;

static void fill_rw(NvmeCmd *cmd, NvmeCtrl *c, const NvmeNs *ns, unsigned s, const WindowSlot *w, bool write) {
    memset(cmd, 0, sizeof *cmd);
    cmd->opc = write ? NVM_WRITE : NVM_READ;
    cmd->cid = (u16)s;
    cmd->nsid = ns->nsid;
    build_prps(c, s, w->buf, (size_t)w->n * ns->sector_size, &cmd->prp1, &cmd->prp2);
    cmd->cdw10 = lo32(w->lba);
    cmd->cdw11 = hi32(w->lba);
    cmd->cdw12 = w->n - 1;
}

/*
 * Reads or writes count sectors between the namespace and buf (dword aligned). Keeps up to c->window commands in flight:
 * submit while there is room, wait for a completion, reap everything that has completed, repeat. After the first error
 * nothing new is submitted but the commands already in flight are drained, because their PRPs point into the caller's buffer.
 * A command that fails with the do-not-retry bit clear (a busy namespace, a transient media error) is submitted again a few
 * times, as Linux does, before the request fails.
 */
static int rw_direct(NvmeNs *ns, u64 lba, u32 count, u8 *buf, bool write) {
    NvmeCtrl *c = ns->ctrl;
    NvmeQueue *q = &c->io;
    u32 ss = ns->sector_size;
    u32 max_sec = MAX(1u, c->max_xfer / ss);
    u32 free_mask = (c->window >= 32 ? 0xFFFFFFFFu : (1u << c->window) - 1);
    WindowSlot slot[NVME_WINDOW_MAX];
    unsigned inflight = 0;
    int err = 0;

    while ((count && !err) || inflight) {
        bool queued = false;
        while (count && !err && free_mask) {
            unsigned s = (unsigned)__builtin_ctz(free_mask);
            u32 n = MIN(count, max_sec);
            NvmeCmd cmd;
            slot[s] = (WindowSlot){ .lba = lba, .n = n, .buf = buf, .tries = 0 };
            fill_rw(&cmd, c, ns, s, &slot[s], write);
            sq_submit(q, &cmd);
            free_mask &= ~(1u << s);
            inflight++;
            c->n_cmds++;
            queued = true;
            lba += n;
            buf += (size_t)n * ss;
            count -= n;
        }
        if (queued && !(g_drop_after && ++g_batches == g_drop_after)) sq_ring(q);
        if (!inflight) break;

        if (!wait_cqe(c, q, g_io_timeout)) {
            ctrl_fail(c, (REG32(c, NVME_CSTS) & CSTS_CFS) ? "controller fatal status" : "I/O command timed out");
            return -EIO;
        }
        u32 retry_mask = 0;
        while (cq_ready(q)) {
            NvmeCqe e;
            cq_pop(q, &e);
            unsigned s = e.cid;
            if (s >= c->window || (free_mask & (1u << s)) || (retry_mask & (1u << s))) {
                ctrl_fail(c, "completion for a command that is not in flight");
                return -EIO;
            }
            u32 st = e.status >> 1;
            if (!(st & 0x7FF)) {
                free_mask |= 1u << s;
                inflight--;
            } else if (!(st & 0x4000) && slot[s].tries < NVME_RETRIES && !err) {
                slot[s].tries++;
                klog("Kappa", "nvme%d: %s of %u blocks at lba %lu on nsid %u: status type %u code %#x, trying again (%u of %u)", c->index,
                     write ? "write" : "read", slot[s].n, (unsigned long)slot[s].lba, ns->nsid, (st >> 8) & 7, st & 0xFF, slot[s].tries, NVME_RETRIES);
                retry_mask |= 1u << s;              /* the slot stays busy */
            } else {
                if (!err) log_io_error(ns, write, slot[s].lba, slot[s].n, st);
                err = -EIO;
                free_mask |= 1u << s;
                inflight--;
            }
        }
        cq_ring(q);
        if (retry_mask) {
            task_sleep_ms(2);                       /* give a busy namespace a moment */
            while (retry_mask) {
                unsigned s = (unsigned)__builtin_ctz(retry_mask);
                retry_mask &= retry_mask - 1;
                NvmeCmd cmd;
                fill_rw(&cmd, c, ns, s, &slot[s], write);
                sq_submit(q, &cmd);
                c->n_cmds++;
            }
            sq_ring(q);
        }
    }
    return err;
}

static bool ensure_bounce(NvmeCtrl *c) {
    if (!c->bounce) c->bounce = dma_alloc_low(NVME_BOUNCE_BYTES, &c->bounce_phys);
    return c->bounce != NULL;
}

static int nvme_rw(NvmeNs *ns, u64 lba, u32 count, void *buf, bool write) {
    NvmeCtrl *c = ns->ctrl;
    if (!count) return 0;
    if (lba >= ns->sectors || (u64)count > ns->sectors - lba) return -EINVAL;
    if (c->dead) return -EIO;

    mutex_lock(&c->io_lock);
    if (c->dead) { mutex_unlock(&c->io_lock); return -EIO; }
    int rc = 0;
    u32 ss = ns->sector_size;
    if (!g_force_bounce && !((u64)buf & 3) && buf_dma_low(buf, (size_t)count * ss, g_data_limit)) {
        rc = rw_direct(ns, lba, count, buf, write);
    } else if (!ensure_bounce(c)) {
        rc = -ENOMEM;
    } else {
        u8 *cur = buf;
        u32 chunk = NVME_BOUNCE_BYTES / ss;
        while (count && !rc) {
            u32 n = MIN(count, chunk);
            c->n_bounced++;
            if (write) memcpy(c->bounce, cur, (size_t)n * ss);
            rc = rw_direct(ns, lba, n, c->bounce, write);
            if (rc) break;
            if (!write) memcpy(cur, c->bounce, (size_t)n * ss);
            cur += (size_t)n * ss;
            lba += n;
            count -= n;
        }
    }
    mutex_unlock(&c->io_lock);
    return rc;
}

static int nvme_blk_read(BlockDev *d, u64 lba, u32 count, void *buf) { return nvme_rw(d->priv, lba, count, buf, false); }

static int nvme_blk_flush(BlockDev *d) {
    NvmeNs *ns = d->priv;
    NvmeCtrl *c = ns->ctrl;
    if (c->dead) return -EIO;
    mutex_lock(&c->io_lock);
    if (c->dead) { mutex_unlock(&c->io_lock); return -EIO; }
    NvmeQueue *q = &c->io;
    int rc = 0;
    /* A failure with the Do Not Retry bit of the status field clear is tried again, like a read or a write (NVMe 1.4, completion entry). */
    for (unsigned attempt = 0;; attempt++) {
        NvmeCmd cmd;
        memset(&cmd, 0, sizeof cmd);
        cmd.opc = NVM_FLUSH;
        cmd.cid = 0;
        cmd.nsid = ns->nsid;
        sq_submit(q, &cmd);
        sq_ring(q);
        c->n_cmds++;
        c->n_flushes++;
        if (!wait_cqe(c, q, T_FLUSH_MS)) {
            ctrl_fail(c, (REG32(c, NVME_CSTS) & CSTS_CFS) ? "controller fatal status" : "flush timed out");
            rc = -EIO;
            break;
        }
        NvmeCqe e;
        cq_pop(q, &e);
        cq_ring(q);
        u32 st = e.status >> 1;
        if (e.cid != 0) { ctrl_fail(c, "completion for the wrong command"); rc = -EIO; break; }
        if (!(st & 0x7FF)) { rc = 0; break; }
        if (!(st & 0x4000) && attempt < NVME_RETRIES) {
            klog("Kappa", "nvme%d: flush of nsid %u: status type %u code %#x, trying again (%u of %u)", c->index, ns->nsid, (st >> 8) & 7, st & 0xFF,
                 attempt + 1, NVME_RETRIES);
            task_sleep_ms(2);
            continue;
        }
        c->n_errors++;
        klog("Kappa", "nvme%d: flush of nsid %u failed: status type %u code %#x%s", c->index, ns->nsid, (st >> 8) & 7, st & 0xFF,
             (st & 0x4000) ? " (do not retry)" : "");
        rc = -EIO;
        break;
    }
    mutex_unlock(&c->io_lock);
    return rc;
}

/*
 * A write that returns is on stable media: nothing above the block layer calls blk_flush yet (sync is a no-op, power-off does not
 * flush), and a PC that is reset or powered off with data in a drive's volatile write cache loses it. So when the controller reports
 * a volatile write cache, every write request is followed by a Flush (one per request, however large). "nvme.nosync" skips it.
 */
static int nvme_blk_write(BlockDev *d, u64 lba, u32 count, const void *buf) {
    NvmeNs *ns = d->priv;
    int rc = nvme_rw(ns, lba, count, (void *)buf, true);
    if (!rc && count && ns->ctrl->vwc && !g_nosync) rc = nvme_blk_flush(d);
    return rc;
}

/* ---------------------------------------------------------------- controller bring-up */

/* Disables the controller and waits for it to say so. */
static bool ctrl_disable(NvmeCtrl *c) {
    u32 cc = REG32(c, NVME_CC);
    if ((cc & CC_EN) && !(REG32(c, NVME_CSTS) & CSTS_RDY)) {
        /* Enabled but not ready yet: the firmware was interrupted mid-initialisation, and clearing EN now has undefined results. */
        BLK_POLL((REG32(c, NVME_CSTS) & (CSTS_RDY | CSTS_CFS)) != 0, c->ready_timeout_ms);
    }
    if (cc & CC_EN) WREG32(c, NVME_CC, cc & ~CC_EN);
    return BLK_POLL(!(REG32(c, NVME_CSTS) & CSTS_RDY), c->ready_timeout_ms);
}

static bool ctrl_enable(NvmeCtrl *c) {
    /* NVM command set, 4 KiB memory pages, round robin arbitration, 64-byte submission and 16-byte completion entries. */
    WREG32(c, NVME_CC, CC_IOSQES(6) | CC_IOCQES(4) | CC_EN);
    BLK_POLL((REG32(c, NVME_CSTS) & (CSTS_RDY | CSTS_CFS)) != 0, c->ready_timeout_ms);
    return (REG32(c, NVME_CSTS) & (CSTS_RDY | CSTS_CFS)) == CSTS_RDY;
}

static void parse_controller(NvmeCtrl *c) {
    const u8 *id = c->ident;
    id_string(c->serial, sizeof c->serial, (const char *)id + 4, 20);
    id_string(c->model, sizeof c->model, (const char *)id + 24, 40);
    id_string(c->firmware, sizeof c->firmware, (const char *)id + 64, 8);
    c->mdts = id[77];
    c->nn = le32(id + 516);
    c->vwc = (id[525] & 1) != 0;
}

static bool create_io_queues(NvmeCtrl *c) {
    /* Ask for one submission and one completion queue (values are zero based); the answer says how many we really got. */
    NvmeCmd feat = { .opc = ADM_SET_FEAT, .cdw10 = 0x07, .cdw11 = 0 };
    u32 got = 0;
    if (nvme_admin(c, &feat, &got, T_ADMIN_MS) != 0 && (c->last_status & 0x7FF)) {
        /* Some controllers reject the feature; the queue pair we create next will tell us if the controller really cannot. */
        klog("Kappa", "nvme%d: set-number-of-queues was refused; trying to create the queues anyway", c->index);
    }

    u16 depth = (u16)MIN(c->mqes, (u32)MAX(2u, MIN(g_depth, 4096u)));
    if (!queue_alloc(c, &c->io, 1, depth)) return false;
    if (c->window >= depth) c->window = depth - 1;

    NvmeCmd cq = { .opc = ADM_CREATE_CQ, .prp1 = c->io.cq_phys, .cdw10 = ((u32)(depth - 1) << 16) | 1u, .cdw11 = 0x1 /* physically contiguous, no interrupts */ };
    if (nvme_admin(c, &cq, NULL, T_ADMIN_MS)) return false;
    NvmeCmd sq = { .opc = ADM_CREATE_SQ, .prp1 = c->io.sq_phys, .cdw10 = ((u32)(depth - 1) << 16) | 1u, .cdw11 = (1u << 16) | 0x1 };
    if (nvme_admin(c, &sq, NULL, T_ADMIN_MS)) return false;
    return true;
}

static void register_namespace(NvmeCtrl *c, u32 nsid) {
    if (nvme_identify(c, 0, nsid)) return;
    const u8 *id = c->ident;
    u64 nsze = le64(id);
    u8 nlbaf = id[25], flbas = id[26], dps = id[29];
    if (!nsze) return;                                      /* inactive (or empty) namespace */
    unsigned fmt = (flbas & 0xF) | (unsigned)((flbas >> 5) & 3) << 4;
    if (fmt > nlbaf) {
        klog("Kappa", "nvme%d: nsid %u reports LBA format %u of %u; skipped", c->index, nsid, fmt, nlbaf + 1);
        return;
    }
    u32 lbaf = le32(id + 128 + 4 * fmt);
    u32 ms = lbaf & 0xFFFF, lbads = (lbaf >> 16) & 0xFF;
    if (ms || dps) {
        klog("Kappa", "nvme%d: nsid %u is formatted with %u bytes of metadata per block%s -- not supported, skipped", c->index, nsid, ms,
             dps ? " and end-to-end protection" : "");
        return;
    }
    if (lbads != 9 && lbads != 12) {
        klog("Kappa", "nvme%d: nsid %u uses %lu-byte blocks -- only 512 and 4096 are supported, skipped", c->index, nsid,
             lbads < 32 ? 1ul << lbads : 0ul);
        return;
    }
    NvmeNs *ns = kzalloc(sizeof *ns);
    BlockDev *bd = kzalloc(sizeof *bd);
    if (!ns || !bd) { kfree(ns); kfree(bd); return; }
    ns->ctrl = c;
    ns->nsid = nsid;
    ns->sector_size = 1u << lbads;
    ns->sectors = nsze;
    ns->bd = bd;
    snprintf(bd->name, sizeof bd->name, "nvme%dn%u", c->index, nsid);
    strlcpy(bd->model, c->model, sizeof bd->model);
    bd->driver = "nvme";
    bd->sector_size = ns->sector_size;
    bd->sectors = ns->sectors;
    bd->read = nvme_blk_read;
    bd->write = nvme_blk_write;
    bd->flush = nvme_blk_flush;
    bd->readonly = bd->ro_policy = blk_ro_requested();
    bd->priv = ns;
    if (c->nns < NVME_MAX_NS) c->ns[c->nns++] = ns;
    klog("Kappa", "nvme%d: nsid %u: %lu MiB, %lu blocks of %u bytes", c->index, nsid, (unsigned long)(ns->sectors * ns->sector_size >> 20),
         (unsigned long)ns->sectors, ns->sector_size);
    blk_register(bd);
}

static void ctrl_probe(PciDev *pci) {
    NvmeCtrl *c = kzalloc(sizeof *c);
    if (!c) return;
    c->pci = pci;
    c->index = g_nctrl;
    mutex_init(&c->admin_lock);
    mutex_init(&c->io_lock);

    pci_enable(pci, PCI_CMD_MEM | PCI_CMD_MASTER);
    c->regs = pci_map_bar(pci, 0);
    if (!c->regs) {
        klog("Kappa", "nvme: %02x:%02x.%x %04x:%04x has no memory BAR0; skipped", pci->bus, pci->dev, pci->fn, pci->vendor, pci->device);
        goto fail;
    }
    c->cap = reg64(c, NVME_CAP);
    c->vs = REG32(c, NVME_VS);
    if (c->cap == ~0ull || c->vs == 0xFFFFFFFFu) {
        klog("Kappa", "nvme: %02x:%02x.%x %04x:%04x does not answer (registers read all ones); skipped", pci->bus, pci->dev, pci->fn, pci->vendor,
             pci->device);
        goto fail;
    }
    c->mqes = (u32)(c->cap & 0xFFFF) + 1;
    u32 to = (u32)((c->cap >> 24) & 0xFF);
    c->ready_timeout_ms = to * 500 < 2000 ? 2000 : to * 500;
    c->db_stride = 4u << ((c->cap >> 32) & 0xF);
    u32 css = (u32)((c->cap >> 37) & 0xFF);
    u32 mpsmin = (u32)((c->cap >> 48) & 0xF);
    pci->driver = "nvme";
    g_nctrl++;
    if (g_tail) g_tail->next = c; else g_head = c;
    g_tail = c;

    klog("Kappa", "nvme%d: %04x:%04x at %02x:%02x.%x, NVMe %u.%u, up to %u queue entries, doorbell stride %u, ready timeout %u ms", c->index, pci->vendor,
         pci->device, pci->bus, pci->dev, pci->fn, c->vs >> 16, (c->vs >> 8) & 0xFF, c->mqes, c->db_stride, to * 500);
    if (!(css & 1)) { klog("Kappa", "nvme%d: the controller has no NVM command set; skipped", c->index); return; }
    if (mpsmin != 0) { klog("Kappa", "nvme%d: the controller needs memory pages larger than 4 KiB; skipped", c->index); return; }
    if (c->db_stride * 4 + NVME_DB + 4 > pci->bar[0].size) { klog("Kappa", "nvme%d: BAR0 is too small for the doorbells; skipped", c->index); return; }

    c->window = MAX(1u, MIN(g_window, (u32)NVME_WINDOW_MAX));
    c->max_xfer = MAX(4096u, MIN(g_maxxfer, (u32)NVME_MAX_XFER)) & ~511u;

    /* Whatever state the firmware left the controller in, start from a disabled one. */
    if (REG32(c, NVME_CSTS) & CSTS_CFS) klog("Kappa", "nvme%d: the controller was already in a fatal state; resetting it", c->index);
    if (!ctrl_disable(c)) {
        klog("Kappa", "nvme%d: the controller does not stop (CSTS %#x); skipped", c->index, REG32(c, NVME_CSTS));
        ctrl_stop_dma(c);                           /* its firmware-era queues now sit in memory the kernel owns: no more bus mastering */
        c->dead = true;
        return;
    }

    /* CAP.MQES limits the I/O queues on PCIe (the admin queue may be up to 4096 entries), but a controller that supports only a few
     * entries per queue is happier when the admin queue stays within that too: never deeper than MQES allows. */
    u16 adepth = (u16)MAX(2u, MIN((u32)NVME_ADMIN_DEPTH, c->mqes));
    c->ident = dma_alloc_low(4096, &c->ident_phys);
    c->prp = dma_alloc_low((size_t)NVME_WINDOW_MAX * PAGE_SIZE, &c->prp_phys);
    if (!c->ident || !c->prp || !queue_alloc(c, &c->admin, 0, adepth)) { klog("Kappa", "nvme%d: out of DMA memory", c->index); return; }

    WREG32(c, NVME_AQA, ((u32)(adepth - 1) << 16) | (adepth - 1));
    wreg64(c, NVME_ASQ, c->admin.sq_phys);
    wreg64(c, NVME_ACQ, c->admin.cq_phys);
    if (!ctrl_enable(c)) {
        klog("Kappa", "nvme%d: the controller did not become ready (CSTS %#x); skipped", c->index, REG32(c, NVME_CSTS));
        ctrl_stop_dma(c);
        c->dead = true;
        return;
    }

    if (nvme_identify(c, 1, 0)) { klog("Kappa", "nvme%d: identify controller failed; skipped", c->index); return; }
    parse_controller(c);
    u8 sqes = ((const u8 *)c->ident)[512], cqes = ((const u8 *)c->ident)[513];
    if ((sqes & 0xF) > 6 || (sqes >> 4) < 6 || (cqes & 0xF) > 4 || (cqes >> 4) < 4) {
        klog("Kappa", "nvme%d: queue entry sizes %#x/%#x do not allow 64/16 bytes; skipped", c->index, sqes, cqes);
        return;
    }
    if (c->mdts) c->max_xfer = MIN(c->max_xfer, 4096u << MIN((u32)c->mdts, 16u));
    klog("Kappa", "nvme%d: \"%s\" serial %s firmware %s, namespace ids up to %u, max transfer %u KiB per command, %s", c->index, c->model,
         c->serial[0] ? c->serial : "-", c->firmware[0] ? c->firmware : "-", c->nn, c->max_xfer >> 10,
         c->vwc ? (g_nosync ? "volatile write cache (nosync: not flushed)" : "volatile write cache (flushed after every write)") : "no volatile write cache");

    if (!create_io_queues(c)) {
        klog("Kappa", "nvme%d: could not create the I/O queues; skipped", c->index);
        ctrl_fail(c, "I/O queue creation failed");
        return;
    }

    /* Active namespaces: the id list (Identify CNS 2, NVMe 1.1+). Older controllers read CNS as a single bit and would answer with
     * namespace data, so they are scanned id by id; a 1.1+ controller that refuses the list is tried with namespace 1. */
    u32 ids[NVME_MAX_NS];
    unsigned nids = 0;
    if (c->vs < 0x00010100) {
        for (u32 id = 1; id <= c->nn && nids < NVME_MAX_NS; id++) ids[nids++] = id;
    } else if (nvme_identify(c, 2, 0) == 0) {
        const u8 *list = c->ident;
        for (unsigned i = 0; i < 1024 && le32(list + 4 * i); i++) {
            if (nids < NVME_MAX_NS) ids[nids++] = le32(list + 4 * i);
            else { klog("Kappa", "nvme%d: more than %d active namespaces; the rest are ignored", c->index, NVME_MAX_NS); break; }
        }
    } else if (!c->dead) {
        ids[nids++] = 1;
    }
    for (unsigned i = 0; i < nids && !c->dead; i++) register_namespace(c, ids[i]);
    if (!c->nns && !c->dead) klog("Kappa", "nvme%d: no usable namespace", c->index);
    return;

fail:
    kfree(c);
}

/* ---------------------------------------------------------------- /proc/nvme */

static size_t gen_proc_nvme(char *buf, size_t cap) {
    size_t n = 0;
    for (NvmeCtrl *c = g_head; c && n + 300 < cap; c = c->next) {
        n += (size_t)snprintf(buf + n, cap - n, "nvme%d %02x:%02x.%x %04x:%04x \"%s\" serial %s fw %s version %u.%u cmds %u errors %u flushes %u bounced %u%s\n", c->index,
                              c->pci->bus, c->pci->dev, c->pci->fn, c->pci->vendor, c->pci->device, c->model, c->serial, c->firmware, c->vs >> 16,
                              (c->vs >> 8) & 0xFF, c->n_cmds, c->n_errors, c->n_flushes, c->n_bounced, c->dead ? "  DEAD" : "");
        for (int i = 0; i < c->nns && n + 200 < cap; i++)
            n += (size_t)snprintf(buf + n, cap - n, "  nsid %u %s %lu x %u\n", c->ns[i]->nsid, c->ns[i]->bd->name, (unsigned long)c->ns[i]->sectors,
                                  c->ns[i]->sector_size);
    }
    return n;
}

/* ---------------------------------------------------------------- entry point */

static void read_knobs(void) {
    long v;
    if ((v = cmdline_int("nvme.maxxfer", 0)) > 0) g_maxxfer = (u32)MIN(v, (long)NVME_MAX_XFER);
    if ((v = cmdline_int("nvme.window", 0)) > 0) g_window = (u32)MIN(v, (long)NVME_WINDOW_MAX);
    if ((v = cmdline_int("nvme.depth", 0)) > 0) g_depth = (u32)v;
    g_force_bounce = cmdline_flag("nvme.bounce");
    g_nosync = cmdline_flag("nvme.nosync");
    if ((v = cmdline_int("blk.dmalimit_mb", 0)) > 0) g_data_limit = (u64)v << 20;
    if ((v = cmdline_int("nvme.iotimeout", 0)) > 0) g_io_timeout = (u32)v;
    if ((v = cmdline_int("nvme.dropcmd", 0)) > 0) g_drop_after = (u32)v;
}

void nvme_init(void) {
    read_knobs();
    for (PciDev *d = pci_find_class(0x01, 0x08, 0x02, NULL); d; d = pci_find_class(0x01, 0x08, 0x02, d))
        ctrl_probe(d);
    if (g_nctrl) procfs_register("nvme", gen_proc_nvme);
    else
        for (PciDev *d = pci_find_class(0x01, 0x04, -1, NULL); d; d = pci_find_class(0x01, 0x04, -1, d))
            if (d->vendor == 0x8086) {
                klog("Kappa", "nvme: no NVMe controller is visible, but the Intel SATA controller at %02x:%02x.%x is in RAID (Intel RST) mode, which hides "
                     "NVMe drives behind it: switch SATA to AHCI in the BIOS setup", d->bus, d->dev, d->fn);
                break;
            }
    blk_selftest("nvme");
}
