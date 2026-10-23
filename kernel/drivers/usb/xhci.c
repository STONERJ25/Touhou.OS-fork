/*
 * xHCI host controller driver (Intel PCH, QEMU qemu-xhci and anything else that follows the xHCI 1.x specification).
 *
 * Polled design: the controller's event ring is read by the usbd thread, interrupts stay masked. Where the specification and
 * Linux (drivers/usb/host/xhci*.c) differ in practice this follows Linux: BIOS hand-off, halt/reset sequencing, Address Device
 * with BSR first, Evaluate Context for the control endpoint size, hub slot context update, 64-bit registers as two dwords.
 *
 * Memory safety rules: every address given to the controller comes from dma_alloc() (and is checked against the controller's
 * addressing ability); nothing the controller may still write to is freed unless Disable Slot succeeded.
 */
#include "usb.h"
#include "xhci.h"
#include "kernel.h"
#include "cpu.h"
#include "irq.h"
#include "mm.h"
#include "task.h"
#include "abi.h"
#include "cmdline.h"

#define RING_TRBS     256               /* one 4 KiB page per ring; the last entry is the link TRB */
/* Test aid, kernel command line "usbring=N" (16..256): use only the first N entries of every ring so that the link TRB / cycle bit
 * wrap-around happens after N - 1 TRBs instead of 255. Rings are always allocated as a full page. */
static u32 g_ring_trbs = RING_TRBS;
#define CMD_TIMEOUT   5000              /* Linux XHCI_CMD_DEFAULT_TIMEOUT */
#define CTL_BUF_SIZE  4096
#define MAX_SLOTS_HW  255

typedef struct XRing {
    volatile Trb *trb;
    paddr_t phys;
    u32 enq;                            /* index of the next entry to fill */
    u32 cycle;                          /* producer cycle state */
} XRing;

typedef struct XEp {
    XRing *ring;
    UsbEndpoint *uep;                   /* interrupt endpoints that we poll */
    u8 *buf;
    paddr_t buf_phys;
    u32 buf_len;
    bool queued;                        /* a TD is outstanding */
    volatile bool bulk_wait, bulk_done; /* a bulk transfer is in flight / has finished (bulk endpoints have no callback) */
    u32  bulk_cc, bulk_resid;
    bool need_recover;
    bool dead;                          /* stop polling (too many errors, or being torn down) */
    u8 errs;
} XEp;

typedef struct XSlot {
    u8 id;
    UsbDevice *dev;
    u8 *out, *in;                       /* output (device) and input context pages */
    paddr_t out_phys, in_phys;
    XEp ep[32];                         /* by device context index; ep[1] is the default control pipe */
    struct {                            /* control transfer in flight on ep[1] */
        volatile bool active, done;
        paddr_t data_trb, status_trb;
        u32 code, resid;
        bool have_data;
    } cw;
} XSlot;

typedef struct XProto {
    u8 major, minor;                    /* BCD revision of the protocol the port speaks */
    u8 slot_type;
} XProto;

typedef struct Xhci {
    UsbHc hc;                           /* must stay first */
    PciDev *pci;
    volatile u8 *cap, *op, *rt;
    volatile u32 *db;
    size_t bar_size;
    u16  hciversion;
    u32  hcs1, hcs2, hcs3, hcc1;
    u32  max_slots, max_ports;
    u32  csz;                           /* context size in bytes: 32 or 64 */
    bool ac64, running, quiet_dead;
    u32  legacy_off;                    /* USB Legacy Support capability offset, 0 if none */

    XProto *proto;                      /* [max_ports + 1] */
    u8   id_of[2][8];                   /* [protocol major - 2][USB_SPEED_*] -> speed id */
    u8   speed_of[2][16];               /* [protocol major - 2][speed id] -> USB_SPEED_* */

    u64 *dcbaa;
    paddr_t dcbaa_phys;
    u64 *sp_array;                      /* scratchpad pointer array */
    paddr_t sp_array_phys;
    void **sp_pages;
    u32  nsp;

    XRing cmd;
    volatile Trb *evt;
    paddr_t evt_phys;
    u32  evt_deq, evt_cycle;
    void *erst;
    paddr_t erst_phys;

    XSlot **slots;                      /* [max_slots + 1] */

    /* the one command in flight */
    volatile bool cmd_done, cmd_pending;
    paddr_t cmd_trb;
    u32  cmd_cc, cmd_slot;

    u8  *ctl_buf;                       /* bounce page for control transfers */
    paddr_t ctl_phys;
    u8  *bulk_buf;                      /* 64 KiB aligned bounce buffer for bulk transfers (allocated on first use) */
    paddr_t bulk_phys;

    bool need_service;
    u64  last_health;
    u32  bad_events;
    u64  late_scan_at;                  /* uptime of the one-off second look at all ports (0 = done) */
    u8  *stuck;                         /* [max_ports + 1] USB 3 ports whose link training never finished: warm reset them */
} Xhci;

typedef struct { u32 cc; u32 slot; } CmdResult;

static inline Xhci *XH(UsbHc *hc) { return (Xhci *)hc; }

/* ---------------------------------------------------------------- register access */

static inline u32 rd32(volatile u8 *base, u32 off) { return *(volatile u32 *)(base + off); }
static inline void wr32(volatile u8 *base, u32 off, u32 v) { *(volatile u32 *)(base + off) = v; }
static inline u64 rd64(volatile u8 *base, u32 off) { u64 lo = rd32(base, off); u64 hi = rd32(base, off + 4); return lo | (hi << 32); }
/* Linux's lo_hi_writeq: low dword first, then high. Safe on every controller. */
static inline void wr64(volatile u8 *base, u32 off, u64 v) { wr32(base, off, (u32)v); wr32(base, off + 4, (u32)(v >> 32)); }
static inline void mb(void) { __asm__ volatile ("mfence" ::: "memory"); }

static inline void ring_db(Xhci *x, u32 doorbell, u32 target) {
    mb();                                           /* TRBs and contexts must be visible before the controller is told */
    x->db[doorbell] = target;
}

/* Test aid, kernel command line "usbfault=ctl:N" or "usbfault=cmd:N": the doorbell of the Nth control transfer / command (counted
 * from boot, the start-up No-Op is command 1) is never rung, so the timeout and recovery paths run exactly as they would for a
 * device or controller that stops answering. */
static u32 g_fault_ctl, g_fault_cmd, g_ctl_count, g_cmd_count;

static const char *cc_name(u32 cc) {
    switch (cc) {
    case CC_SUCCESS: return "success";               case CC_DATA_BUFFER: return "data buffer error";
    case CC_BABBLE: return "babble";                 case CC_USB_TRANSACTION: return "transaction error";
    case CC_TRB_ERROR: return "TRB error";           case CC_STALL: return "stall";
    case CC_RESOURCE: return "resource error";       case CC_BANDWIDTH: return "bandwidth error";
    case CC_NO_SLOTS: return "no slots available";   case CC_SLOT_NOT_ENABLED: return "slot not enabled";
    case CC_EP_NOT_ENABLED: return "endpoint not enabled"; case CC_SHORT_PACKET: return "short packet";
    case CC_PARAMETER: return "parameter error";     case CC_CONTEXT_STATE: return "context state error";
    case CC_INCOMPATIBLE: return "incompatible device"; case CC_CMD_ABORTED: return "command aborted";
    case CC_CMD_RING_STOPPED: return "command ring stopped"; case CC_STOPPED: return "stopped";
    case CC_MAX_EXIT_LATENCY: return "max exit latency too large"; case CC_SPLIT_TRANSACTION: return "split transaction error";
    default: return "?";
    }
}

/* ---------------------------------------------------------------- failure handling */

static void x_die(Xhci *x, const char *why) {
    if (x->hc.dead) return;
    x->hc.dead = true;
    strlcpy(x->hc.status, why, sizeof x->hc.status);
    klog("Kappa", "xhci %02x:%02x.%x: %s -- USB is switched off for this controller", x->pci->bus, x->pci->dev, x->pci->fn, why);
    if (x->op && x->running) {                      /* best effort: stop it from touching memory */
        u32 cmd = rd32(x->op, XOP_USBCMD);
        if (cmd != 0xFFFFFFFF) wr32(x->op, XOP_USBCMD, cmd & ~(CMD_RUN | CMD_INTE));
    }
}

/* ---------------------------------------------------------------- DMA memory */

static void *x_dma(Xhci *x, size_t size, paddr_t *phys) {
    paddr_t p;
    void *v = dma_alloc(size, &p);
    if (!v) return NULL;
    /* The device sees raw physical addresses: refuse anything it cannot reach or that is not RAM at all. */
    if (p & 63 || p + size > pmm_ram_top() || (!x->ac64 && p + size > 0x100000000ull)) {
        klog("Kappa", "xhci: dma buffer at %#lx is not usable by this controller%s", (unsigned long)p, x->ac64 ? "" : " (32-bit DMA only)");
        dma_free(v, size);
        return NULL;
    }
    *phys = p;
    return v;
}

static void x_dma_free(void *v, size_t size) { dma_free(v, size); }

static XRing *ring_alloc(Xhci *x) {
    XRing *r = kzalloc(sizeof *r);
    if (!r) return NULL;
    void *mem = x_dma(x, RING_TRBS * sizeof(Trb), &r->phys);
    if (!mem) { kfree(r); return NULL; }
    r->trb = mem;
    r->cycle = 1;
    volatile Trb *l = &r->trb[g_ring_trbs - 1];     /* link back to the start, toggling the cycle bit */
    l->param_lo = (u32)r->phys;
    l->param_hi = (u32)(r->phys >> 32);
    l->control = TRB_TYPE(TRB_LINK) | TRB_TC;
    return r;
}

static void ring_free(XRing *r) {
    if (!r) return;
    x_dma_free((void *)r->trb, RING_TRBS * sizeof(Trb));
    kfree(r);
}

/* Appends one TRB; the cycle bit is written last so the controller never sees a half written entry. Returns its address. */
static paddr_t ring_push(XRing *r, u64 param, u32 status, u32 control) {
    volatile Trb *t = &r->trb[r->enq];
    paddr_t phys = r->phys + (paddr_t)r->enq * sizeof(Trb);
    t->param_lo = (u32)param;
    t->param_hi = (u32)(param >> 32);
    t->status = status;
    barrier();
    t->control = control | r->cycle;
    if (++r->enq == g_ring_trbs - 1) {              /* next entry is the link TRB: hand it over and wrap */
        volatile Trb *l = &r->trb[g_ring_trbs - 1];
        l->param_lo = (u32)r->phys;
        l->param_hi = (u32)(r->phys >> 32);
        l->status = 0;
        barrier();
        l->control = TRB_TYPE(TRB_LINK) | TRB_TC | r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
    return phys;
}

static inline paddr_t ring_enq_phys(const XRing *r) { return r->phys + (paddr_t)r->enq * sizeof(Trb); }

/* ---------------------------------------------------------------- event ring */

static void handle_transfer_event(Xhci *x, const Trb *ev);

static void ring_update_erdp(Xhci *x) {
    u64 v = (x->evt_phys + (u64)x->evt_deq * sizeof(Trb)) | ERDP_EHB;     /* EHB is RW1C: writing 1 clears "handler busy" */
    wr64(x->rt, XRT_IR(0) + XIR_ERDP, v);
}

static void process_events(Xhci *x) {
    if (!x->evt) return;
    int n = 0;
    while (n < 256) {
        volatile Trb *t = &x->evt[x->evt_deq];
        u32 control = t->control;
        if ((control & TRB_CYCLE) != x->evt_cycle) break;
        barrier();
        Trb ev = { t->param_lo, t->param_hi, t->status, control };
        if (++x->evt_deq == g_ring_trbs) { x->evt_deq = 0; x->evt_cycle ^= 1; }
        n++;
        x->hc.nevents++;
        switch (TRB_TYPE_OF(control)) {
        case TRB_EV_CMD_COMPLETE: {
            paddr_t trb = ev.param_lo | ((u64)ev.param_hi << 32);
            if (x->cmd_pending && trb == x->cmd_trb) {
                x->cmd_cc = ev.status >> 24;
                x->cmd_slot = control >> 24;
                x->cmd_done = true;
            }
            break;
        }
        case TRB_EV_PORT_STATUS: {
            u32 port = ev.param_lo >> 24;
            if (port >= 1 && port <= x->max_ports) usb_hub_mark_port(&x->hc.root, (int)port);
            break;
        }
        case TRB_EV_TRANSFER:
            handle_transfer_event(x, &ev);
            break;
        case TRB_EV_HOST_CTRL:
            if (x->bad_events++ < 3) klog("Kappa", "xhci: host controller event, completion code %u (%s)", ev.status >> 24, cc_name(ev.status >> 24));
            break;
        default:
            break;                                  /* bandwidth request, doorbell, device notification, MFINDEX wrap */
        }
    }
    if (n) ring_update_erdp(x);
}

/* Cheap health check, at most once per millisecond: a dead controller must be noticed, not waited on. */
static void x_health(Xhci *x) {
    u64 now = uptime_ms();
    if (now == x->last_health || !x->running) return;
    x->last_health = now;
    u32 sts = rd32(x->op, XOP_USBSTS);
    if (sts == 0xFFFFFFFFu) { x_die(x, "controller stopped responding (registers read as all ones)"); return; }
    if (sts & (STS_HSE | STS_HCE)) { x_die(x, sts & STS_HSE ? "host system error" : "host controller error"); return; }
    if (sts & STS_HCH) { x_die(x, "controller halted by itself"); return; }
    if (sts & (STS_EINT | STS_PCD)) wr32(x->op, XOP_USBSTS, sts & (STS_EINT | STS_PCD));
}

static void x_pump(Xhci *x) {
    process_events(x);
    x_health(x);
}

/* Waits for *flag, pumping events. Short waits spin (commands finish in microseconds), longer ones sleep. */
static int wait_flag(Xhci *x, volatile bool *flag, u32 timeout_ms) {
    u64 end = uptime_ms() + timeout_ms;
    for (int spin = 0;; spin++) {
        x_pump(x);
        if (*flag) return 0;
        if (x->hc.dead) return -ENODEV;
        if (uptime_ms() >= end) return -ETIMEDOUT;
        if (spin < 100) udelay(10);
        else task_sleep_ms(1);
    }
}

/* Sleeps for `ms`, keeping the event ring drained (other devices' reports must not wait for an enumeration). */
static void x_sleep(Xhci *x, u32 ms) {
    for (u32 i = 0; i < ms && !x->hc.dead; i++) {
        task_sleep_ms(1);
        x_pump(x);
    }
}

/* ---------------------------------------------------------------- commands */

static void cmd_ring_abort(Xhci *x) {
    /* CRCR.CA: stop the command ring. The stuck command stays in the ring, so afterwards the dequeue pointer is moved past it. */
    u64 crcr = rd64(x->op, XOP_CRCR);
    wr64(x->op, XOP_CRCR, (crcr & ~(u64)(CRCR_CRR | CRCR_CS)) | CRCR_CA);
    u64 end = uptime_ms() + 1000;
    while (rd32(x->op, XOP_CRCR) & CRCR_CRR) {
        process_events(x);
        if (uptime_ms() >= end) { x_die(x, "command ring does not stop (controller is dead?)"); return; }
        task_sleep_ms(1);
    }
    process_events(x);
    wr64(x->op, XOP_CRCR, ring_enq_phys(&x->cmd) | x->cmd.cycle);          /* resume after the aborted command */
    usb_dbg("xhci: command ring aborted and restarted");
}

/* Runs one command and waits for its completion event. 0 = success, -EIO = the controller said no (res->cc tells why). */
static int xhci_cmd(Xhci *x, u32 control, u64 param, CmdResult *res) {
    CmdResult dummy;
    if (!res) res = &dummy;
    res->cc = 0;
    res->slot = 0;
    if (x->hc.dead) return -ENODEV;
    x->cmd_done = false;
    x->cmd_trb = ring_push(&x->cmd, param, 0, control);
    x->cmd_pending = true;
    if (g_fault_cmd && ++g_cmd_count == g_fault_cmd) usb_dbg("xhci: fault injection: command doorbell not rung");
    else ring_db(x, 0, 0);
    int rc = wait_flag(x, &x->cmd_done, CMD_TIMEOUT);
    x->cmd_pending = false;
    if (rc == -ETIMEDOUT) {
        klog("Kappa", "xhci: command type %u timed out", TRB_TYPE_OF(control));
        cmd_ring_abort(x);
        return -ETIMEDOUT;
    }
    if (rc) return rc;
    res->cc = x->cmd_cc;
    res->slot = x->cmd_slot;
    if (res->cc != CC_SUCCESS) {
        usb_dbg("xhci: command type %u failed: %s (%u)", TRB_TYPE_OF(control), cc_name(res->cc), res->cc);
        return -EIO;
    }
    return 0;
}

/* ---------------------------------------------------------------- context helpers */

static inline volatile u32 *ctx_in_ctrl(Xhci *x, XSlot *s) { (void)x; return (volatile u32 *)s->in; }
static inline volatile u32 *ctx_in_slot(Xhci *x, XSlot *s) { return (volatile u32 *)(s->in + x->csz); }
static inline volatile u32 *ctx_in_ep(Xhci *x, XSlot *s, int dci) { return (volatile u32 *)(s->in + x->csz * (1 + dci)); }
static inline volatile u32 *ctx_out_slot(Xhci *x, XSlot *s) { (void)x; return (volatile u32 *)s->out; }
static inline volatile u32 *ctx_out_ep(Xhci *x, XSlot *s, int dci) { return (volatile u32 *)(s->out + x->csz * dci); }

static void in_ctx_zero(Xhci *x, XSlot *s) { memset(s->in, 0, 33 * x->csz); }

static void copy_ctx(volatile u32 *dst, volatile u32 *src, u32 bytes) {
    for (u32 i = 0; i < bytes / 4; i++) dst[i] = src[i];
}

/* ---------------------------------------------------------------- slots */

static XSlot *dev_slot(UsbDevice *dev) { return dev->hcpriv; }

static void slot_free_memory(Xhci *x, XSlot *s) {
    for (int i = 1; i < 32; i++) {
        if (s->ep[i].ring) ring_free(s->ep[i].ring);
        if (s->ep[i].buf) x_dma_free(s->ep[i].buf, 4096);
    }
    if (s->in) x_dma_free(s->in, 4096);
    if (s->out) x_dma_free(s->out, 4096);
    kfree(s);
}

static int xhci_alloc_dev(UsbHc *hc, UsbDevice *dev) {
    Xhci *x = XH(hc);
    CmdResult r;
    u32 slot_type = x->proto[dev->root_port].slot_type;
    int rc = xhci_cmd(x, TRB_TYPE(TRB_CMD_ENABLE_SLOT) | ((slot_type & 0x1F) << 16), 0, &r);
    if (rc) return rc == -EIO && r.cc == CC_NO_SLOTS ? -ENOSPC : rc;
    u32 id = r.slot;
    if (id < 1 || id > x->max_slots || x->slots[id]) {                  /* the controller handed out something impossible */
        klog("Kappa", "xhci: Enable Slot returned slot %u (already in use or out of range)", id);
        if (id >= 1 && id <= x->max_slots && !x->slots[id]) xhci_cmd(x, TRB_TYPE(TRB_CMD_DISABLE_SLOT) | TRB_SLOT(id), 0, NULL);
        return -EIO;
    }
    XSlot *s = kzalloc(sizeof *s);
    if (!s) { xhci_cmd(x, TRB_TYPE(TRB_CMD_DISABLE_SLOT) | TRB_SLOT(id), 0, NULL); return -ENOMEM; }
    s->id = (u8)id;
    s->dev = dev;
    s->out = x_dma(x, 4096, &s->out_phys);
    s->in = x_dma(x, 4096, &s->in_phys);
    s->ep[1].ring = ring_alloc(x);
    if (!s->out || !s->in || !s->ep[1].ring) {
        slot_free_memory(x, s);
        xhci_cmd(x, TRB_TYPE(TRB_CMD_DISABLE_SLOT) | TRB_SLOT(id), 0, NULL);
        return -ENOMEM;
    }
    x->slots[id] = s;
    x->dcbaa[id] = s->out_phys;                     /* the controller reads this when Address Device runs */
    dev->slot = (u8)id;
    dev->hcpriv = s;
    if (!dev->speed_id) dev->speed_id = x->id_of[x->proto[dev->root_port].major == 3 ? 1 : 0][dev->speed];
    return 0;
}

static int xhci_address_dev(UsbHc *hc, UsbDevice *dev, bool bsr) {
    Xhci *x = XH(hc);
    XSlot *s = dev_slot(dev);
    if (!s) return -ENODEV;
    XRing *r0 = s->ep[1].ring;

    in_ctx_zero(x, s);
    volatile u32 *ctrl = ctx_in_ctrl(x, s), *slot = ctx_in_slot(x, s), *ep0 = ctx_in_ep(x, s, 1);
    ctrl[1] = 0x3;                                  /* add flags: slot context (A0) and default control endpoint (A1) */

    u32 dw0 = SLOT_DW0_ROUTE(dev->route) | SLOT_DW0_SPEED(dev->speed_id) | SLOT_DW0_ENTRIES(1);
    u32 dw2 = 0;
    if (dev->tt_hub && dev->tt_hub->hcpriv) {       /* low/full speed device below a high speed hub: its transaction translator */
        dw2 = SLOT_DW2_TT(((XSlot *)dev->tt_hub->hcpriv)->id, dev->tt_port);
        if (dev->tt_hub->hub && dev->tt_hub->hub->mtt) dw0 |= SLOT_DW0_MTT;
    }
    slot[0] = dw0;
    slot[1] = SLOT_DW1_ROOTPORT(dev->root_port);
    slot[2] = dw2;

    /* The ring may already have carried traffic (BSR pass): start the endpoint where the ring currently is. */
    paddr_t deq = ring_enq_phys(r0);
    ep0[0] = 0;
    ep0[1] = EP_DW1_CERR(3) | EP_DW1_TYPE(EP_TYPE_CONTROL) | EP_DW1_MPS(dev->mps0);
    ep0[2] = ((u32)deq & ~0xFu) | r0->cycle;
    ep0[3] = (u32)(deq >> 32);
    ep0[4] = EP_DW4_AVG(8);

    CmdResult res;
    int rc = xhci_cmd(x, TRB_TYPE(TRB_CMD_ADDRESS) | TRB_SLOT(s->id) | (bsr ? TRB_BSR : 0), s->in_phys, &res);
    if (rc) {
        if (rc == -EIO) klog("Kappa", "usb %s: Address Device%s failed: %s", dev->path, bsr ? " (BSR)" : "", cc_name(res.cc));
        return rc;
    }
    return 0;
}

static int xhci_set_ep0_size(UsbHc *hc, UsbDevice *dev, u16 mps) {
    Xhci *x = XH(hc);
    XSlot *s = dev_slot(dev);
    if (!s) return -ENODEV;
    in_ctx_zero(x, s);
    ctx_in_ctrl(x, s)[1] = 0x2;                     /* A1: the control endpoint only */
    volatile u32 *ep0 = ctx_in_ep(x, s, 1);
    copy_ctx(ep0, ctx_out_ep(x, s, 1), x->csz);
    ep0[1] = (ep0[1] & 0xFFFFu) | EP_DW1_MPS(mps);
    int rc = xhci_cmd(x, TRB_TYPE(TRB_CMD_EVALUATE) | TRB_SLOT(s->id), s->in_phys, NULL);
    if (!rc) dev->mps0 = mps;
    return rc;
}

/* ---------------------------------------------------------------- endpoint recovery */

/* Gets an endpoint going again after an error or a timeout: stop it, clear a halt, and point it past everything queued. */
static void ep_recover(Xhci *x, XSlot *s, int dci, bool stop) {
    XRing *ring = s->ep[dci].ring;
    if (!ring || x->hc.dead) return;
    if (stop) xhci_cmd(x, TRB_TYPE(TRB_CMD_STOP_EP) | TRB_SLOT(s->id) | TRB_EP(dci), 0, NULL);
    xhci_cmd(x, TRB_TYPE(TRB_CMD_RESET_EP) | TRB_SLOT(s->id) | TRB_EP(dci), 0, NULL);       /* "context state error" if not halted: fine */
    paddr_t deq = ring_enq_phys(ring);
    CmdResult r;
    int rc = xhci_cmd(x, TRB_TYPE(TRB_CMD_SET_DEQ) | TRB_SLOT(s->id) | TRB_EP(dci), deq | ring->cycle, &r);
    if (rc) usb_dbg("xhci: slot %u ep %d: Set TR Dequeue Pointer failed (%s)", s->id, dci, cc_name(r.cc));
}

/* ---------------------------------------------------------------- control transfers */

static int cc_to_errno(u32 cc) {
    switch (cc) {
    case CC_SUCCESS: case CC_SHORT_PACKET: return 0;
    case CC_STALL: return -EPIPE;
    default: return -EIO;
    }
}

static int xhci_control(UsbHc *hc, UsbDevice *dev, const UsbSetup *su, void *buf, u32 timeout_ms) {
    Xhci *x = XH(hc);
    XSlot *s = dev_slot(dev);
    if (!s || x->hc.dead) return -ENODEV;
    u16 len = su->wLength;
    bool in = su->bmRequestType & USB_DIR_IN;
    if (len > CTL_BUF_SIZE) return -EINVAL;
    XRing *ring = s->ep[1].ring;

    if (len && !in) memcpy(x->ctl_buf, buf, len);
    else if (len) memset(x->ctl_buf, 0, len);

    u64 setup = (u64)su->bmRequestType | ((u64)su->bRequest << 8) | ((u64)su->wValue << 16) | ((u64)su->wIndex << 32) | ((u64)su->wLength << 48);
    s->cw.active = true;
    s->cw.done = false;
    s->cw.code = 0;
    s->cw.resid = 0;
    s->cw.have_data = false;
    s->cw.data_trb = 0;
    ring_push(ring, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (len ? (in ? TRB_TRT_IN : TRB_TRT_OUT) : 0));
    if (len)
        s->cw.data_trb = ring_push(ring, x->ctl_phys, len, TRB_TYPE(TRB_DATA) | TRB_IOC | (in ? (TRB_DIR_IN | TRB_ISP) : 0));
    s->cw.status_trb = ring_push(ring, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | ((in && len) ? 0 : TRB_DIR_IN));
    if (g_fault_ctl && ++g_ctl_count == g_fault_ctl) usb_dbg("usb %s: fault injection: control doorbell not rung", dev->path);
    else ring_db(x, s->id, 1);

    int rc = wait_flag(x, &s->cw.done, timeout_ms);
    s->cw.active = false;
    if (rc == -ETIMEDOUT) {
        usb_dbg("usb %s: control request %02x/%02x timed out", dev->path, su->bmRequestType, su->bRequest);
        ep_recover(x, s, 1, true);
        return -ETIMEDOUT;
    }
    if (rc) return rc;
    int err = cc_to_errno(s->cw.code);
    if (err) {
        usb_dbg("usb %s: control request %02x/%02x failed: %s", dev->path, su->bmRequestType, su->bRequest, cc_name(s->cw.code));
        ep_recover(x, s, 1, false);                 /* the endpoint halted; clear it and skip the dead TD */
        return err;
    }
    int actual = len;
    if (s->cw.have_data) actual = len - (int)s->cw.resid;
    if (actual < 0) actual = 0;
    if (in && actual) memcpy(buf, x->ctl_buf, (size_t)actual);
    return actual;
}

/* ---------------------------------------------------------------- endpoints */

static u32 ep_xhci_type(const UsbEndpoint *ep) {
    bool in = ep->addr & USB_DIR_IN;
    switch (ep->type) {
    case USB_EP_ISOC: return in ? EP_TYPE_ISOC_IN : EP_TYPE_ISOC_OUT;
    case USB_EP_BULK: return in ? EP_TYPE_BULK_IN : EP_TYPE_BULK_OUT;
    case USB_EP_INT:  return in ? EP_TYPE_INT_IN : EP_TYPE_INT_OUT;
    default: return EP_TYPE_CONTROL;
    }
}

static u32 fls32(u32 v) { u32 n = 0; while (v) { n++; v >>= 1; } return n; }

/* bInterval -> xHCI interval exponent (2^n * 125 us), as in Linux xhci_get_endpoint_interval(). */
static u32 ep_interval(const UsbDevice *dev, const UsbEndpoint *ep) {
    if (ep->type != USB_EP_INT && ep->type != USB_EP_ISOC) return 0;
    u32 bi = ep->interval ? ep->interval : 1;
    switch (dev->speed) {
    case USB_SPEED_HIGH: case USB_SPEED_SUPER: case USB_SPEED_SUPER_PLUS:
        return (bi < 1 ? 1 : bi > 16 ? 16 : bi) - 1;                    /* 2^(bInterval-1) microframes */
    default:
        if (ep->type == USB_EP_ISOC) return (bi < 1 ? 1 : bi > 16 ? 16 : bi) - 1;
        {
            u32 e = fls32(bi * 8) - 1;                                  /* frames -> microframes, rounded down to a power of two */
            return e < 3 ? 3 : e > 10 ? 10 : e;
        }
    }
}

static int xhci_add_endpoints(UsbHc *hc, UsbDevice *dev, UsbEndpoint **eps, int n) {
    Xhci *x = XH(hc);
    XSlot *s = dev_slot(dev);
    if (!s) return -ENODEV;
    int last = 1, done = 0, rc;
    in_ctx_zero(x, s);
    volatile u32 *ctrl = ctx_in_ctrl(x, s);
    for (int i = 0; i < n; i++) {
        UsbEndpoint *ep = eps[i];
        int dci = ep->dci;
        if (dci < 2 || dci > 31 || s->ep[dci].ring) { rc = -EINVAL; goto fail; }
        if (ep->type == USB_EP_ISOC) { rc = -ENOTSUP; goto fail; }      /* out of scope */
        XEp *xe = &s->ep[dci];
        xe->ring = ring_alloc(x);
        if (!xe->ring) { rc = -ENOMEM; goto fail; }
        done++;
        xe->uep = ep;
        ep->hc = xe;
        ctrl[1] |= 1u << dci;
        if (dci > last) last = dci;

        volatile u32 *ec = ctx_in_ep(x, s, dci);
        u32 burst = 0, mult = 0, esit;
        if (dev->speed == USB_SPEED_HIGH) burst = ep->extra_tx;                     /* extra transactions per microframe */
        if (dev->speed >= USB_SPEED_SUPER) { burst = ep->burst; mult = 0; }
        if (dev->speed >= USB_SPEED_SUPER && ep->bytes_per_interval) esit = ep->bytes_per_interval;
        else esit = ep->mps * (burst + 1);
        u32 avg = ep->type == USB_EP_BULK ? 1024 : esit;
        ec[0] = EP_DW0_MULT(mult) | EP_DW0_INTERVAL(ep_interval(dev, ep)) | EP_DW0_ESIT_HI(esit);
        ec[1] = EP_DW1_CERR(3) | EP_DW1_TYPE(ep_xhci_type(ep)) | EP_DW1_BURST(burst) | EP_DW1_MPS(ep->mps);
        paddr_t deq = xe->ring->phys;
        ec[2] = ((u32)deq & ~0xFu) | xe->ring->cycle;
        ec[3] = (u32)(deq >> 32);
        ec[4] = EP_DW4_AVG(avg) | EP_DW4_ESIT_LO(esit);
    }
    /* The slot context comes from what the controller currently has, with the "last valid context" raised to cover the new ones. */
    volatile u32 *slot = ctx_in_slot(x, s);
    copy_ctx(slot, ctx_out_slot(x, s), x->csz);
    int have = (int)(slot[0] >> 27);
    if (have > last) last = have;
    slot[0] = (slot[0] & ~SLOT_DW0_ENTRIES_MASK) | SLOT_DW0_ENTRIES(last);
    ctrl[1] |= 1u;                                                      /* A0 */
    CmdResult r;
    rc = xhci_cmd(x, TRB_TYPE(TRB_CMD_CONFIGURE) | TRB_SLOT(s->id), s->in_phys, &r);
    if (rc) {
        if (rc == -EIO) {
            klog("Kappa", "usb %s: Configure Endpoint failed: %s", dev->path, cc_name(r.cc));
            rc = (r.cc == CC_BANDWIDTH || r.cc == CC_RESOURCE) ? -ENOSPC : -EIO;
        }
        goto fail;
    }
    for (int i = 0; i < n; i++) eps[i]->configured = true;
    return 0;

fail:                                                                   /* nothing was configured: forget the rings we made */
    for (int i = 0; i < done; i++) {
        XEp *xe = eps[i]->hc;
        if (xe && xe->ring) { ring_free(xe->ring); xe->ring = NULL; xe->uep = NULL; }
        eps[i]->hc = NULL;
    }
    return rc;
}

/* Hub bits of the slot context (Hub, MTT, Number of Ports, TT think time). Linux's xhci_update_hub_device() issues Configure
 * Endpoint for xHCI 1.0 and later and Evaluate Context for 0.95 and earlier; the same order is used here, and since a controller
 * may accept the command without applying the fields (QEMU does) the output context is checked and the other command tried. */
static int xhci_update_hub(UsbHc *hc, UsbDevice *dev, int nports, int ttt, bool mtt) {
    Xhci *x = XH(hc);
    XSlot *s = dev_slot(dev);
    if (!s) return -ENODEV;
    bool configure_first = x->hciversion > 0x95;
    for (int attempt = 0; attempt < 2; attempt++) {
        in_ctx_zero(x, s);
        volatile u32 *ctrl = ctx_in_ctrl(x, s), *slot = ctx_in_slot(x, s);
        copy_ctx(slot, ctx_out_slot(x, s), x->csz);
        ctrl[1] = 1u;                                                   /* A0 only */
        slot[0] |= SLOT_DW0_HUB;
        if (mtt) slot[0] |= SLOT_DW0_MTT; else slot[0] &= ~SLOT_DW0_MTT;
        slot[1] = (slot[1] & 0x00FFFFFFu) | SLOT_DW1_NPORTS(nports);
        if (dev->speed == USB_SPEED_HIGH) slot[2] = (slot[2] & ~SLOT_DW2_TTT(3)) | SLOT_DW2_TTT(ttt & 3);
        slot[3] = 0;
        bool use_configure = (attempt == 0) == configure_first;
        u32 type = use_configure ? TRB_CMD_CONFIGURE : TRB_CMD_EVALUATE;
        CmdResult r;
        int rc = xhci_cmd(x, TRB_TYPE(type) | TRB_SLOT(s->id), s->in_phys, &r);
        if (rc == -ETIMEDOUT || rc == -ENODEV) return rc;
        if (rc == 0 && (ctx_out_slot(x, s)[0] & SLOT_DW0_HUB)) return 0;
        usb_dbg("usb %s: hub bit not applied by %s (%s)", dev->path, use_configure ? "Configure Endpoint" : "Evaluate Context", rc ? cc_name(r.cc) : "no effect");
    }
    return 0;           /* the controller ignored it (QEMU does); routing through the hub works regardless there, so carry on */
}

static void queue_int_td(Xhci *x, XSlot *s, XEp *xe, int dci) {
    ring_push(xe->ring, xe->buf_phys, xe->buf_len, TRB_TYPE(TRB_NORMAL) | TRB_ISP | TRB_IOC);
    xe->queued = true;
    ring_db(x, s->id, (u32)dci);
}

static int xhci_int_start(UsbHc *hc, UsbDevice *dev, UsbEndpoint *ep) {
    Xhci *x = XH(hc);
    XSlot *s = dev_slot(dev);
    XEp *xe = ep->hc;
    if (!s || !xe || !xe->ring) return -ENODEV;
    if (!xe->buf) {
        xe->buf = x_dma(x, 4096, &xe->buf_phys);
        if (!xe->buf) return -ENOMEM;
    }
    xe->buf_len = ep->mps ? ep->mps : 8;
    if (xe->buf_len > 1024) xe->buf_len = 1024;
    xe->errs = 0;
    xe->dead = false;
    queue_int_td(x, s, xe, ep->dci);
    return 0;
}

/* ---------------------------------------------------------------- bulk transfers */

#define BULK_MAX 65536

/* One bulk TD through a 64 KiB aligned bounce buffer (a TRB's buffer must not cross a 64 KiB boundary). Returns the number of
 * bytes moved or -errno; -EPIPE means the endpoint stalled (the caller clears the halt on the device with usb_clear_halt). */
static int xhci_bulk(UsbHc *hc, UsbDevice *dev, UsbEndpoint *ep, void *buf, u32 len, u32 timeout_ms) {
    Xhci *x = XH(hc);
    XSlot *s = dev_slot(dev);
    XEp *xe = ep->hc;
    if (!s || x->hc.dead) return -ENODEV;
    if (!xe || !xe->ring || ep->type != USB_EP_BULK || xe->uep != ep) return -EINVAL;
    if (len == 0 || len > BULK_MAX) return -EINVAL;
    if (!x->bulk_buf) {
        void *b = x_dma(x, BULK_MAX, &x->bulk_phys);
        if (!b) return -ENOMEM;
        if (x->bulk_phys & (BULK_MAX - 1)) { dma_free(b, BULK_MAX); x->bulk_phys = 0; return -ENOMEM; }
        x->bulk_buf = b;
    }
    bool in = ep->addr & USB_DIR_IN;
    if (!in) memcpy(x->bulk_buf, buf, len);
    xe->bulk_done = false;
    xe->bulk_wait = true;
    ring_push(xe->ring, x->bulk_phys, len, TRB_TYPE(TRB_NORMAL) | TRB_IOC | (in ? TRB_ISP : 0));
    ring_db(x, s->id, ep->dci);
    int rc = wait_flag(x, &xe->bulk_done, timeout_ms);
    xe->bulk_wait = false;
    if (rc == -ETIMEDOUT) {
        usb_dbg("usb %s: bulk transfer on endpoint %02x timed out", dev->path, ep->addr);
        ep_recover(x, s, ep->dci, true);
        return -ETIMEDOUT;
    }
    if (rc) return rc;
    if (xe->bulk_cc != CC_SUCCESS && xe->bulk_cc != CC_SHORT_PACKET) {
        usb_dbg("usb %s: bulk transfer on endpoint %02x failed: %s", dev->path, ep->addr, cc_name(xe->bulk_cc));
        ep_recover(x, s, ep->dci, false);
        return cc_to_errno(xe->bulk_cc);
    }
    int actual = (int)len - (int)xe->bulk_resid;
    if (actual < 0) actual = 0;
    if (in && actual) memcpy(buf, x->bulk_buf, (size_t)actual);
    return actual;
}

/* ---------------------------------------------------------------- transfer events */

static void handle_transfer_event(Xhci *x, const Trb *ev) {
    u32 slot = ev->control >> 24, dci = (ev->control >> 16) & 0x1F;
    u32 code = ev->status >> 24, resid = ev->status & 0xFFFFFF;
    paddr_t trb = ev->param_lo | ((u64)ev->param_hi << 32);
    XSlot *s = slot >= 1 && slot <= x->max_slots ? x->slots[slot] : NULL;
    if (!s || !s->dev || dci < 1 || dci > 31) return;

    if (dci == 1) {                                                     /* default control pipe */
        if (!s->cw.active) return;                                      /* leftovers of a transfer we gave up on */
        if (code != CC_SUCCESS && code != CC_SHORT_PACKET) { s->cw.code = code; s->cw.done = true; return; }
        if (s->cw.data_trb && trb == s->cw.data_trb) { s->cw.resid = resid; s->cw.have_data = true; return; }
        if (trb == s->cw.status_trb) { s->cw.code = CC_SUCCESS; s->cw.done = true; }
        return;
    }

    XEp *xe = &s->ep[dci];
    if (xe->bulk_wait) {                                                /* a bulk transfer: just record how it ended */
        xe->bulk_cc = code;
        xe->bulk_resid = resid;
        xe->bulk_done = true;
        return;
    }
    if (!xe->uep || !xe->queued) return;
    xe->queued = false;
    UsbDevice *dev = s->dev;
    if (code == CC_SUCCESS || code == CC_SHORT_PACKET) {
        xe->errs = 0;
        int len = (int)xe->buf_len - (int)resid;
        if (len < 0) len = 0;
        if (!dev->dead && xe->uep->cb) xe->uep->cb(dev, xe->uep, xe->buf, len, xe->uep->cb_ctx);
        if (!dev->dead && !xe->dead) queue_int_td(x, s, xe, (int)dci);
        return;
    }
    if (code == CC_STOPPED || code == CC_STOPPED_LENGTH || code == CC_STOPPED_SHORT || code == CC_CMD_ABORTED) return;
    if (dev->dead || xe->dead) return;
    if (++xe->errs <= 4) {
        usb_dbg("usb %s: endpoint %02x error: %s", dev->path, xe->uep->addr, cc_name(code));
        xe->need_recover = true;
        x->need_service = true;
    } else {
        xe->dead = true;
        klog("Kappa", "usb %s: endpoint %02x keeps failing (%s); no longer polled", dev->path, xe->uep->addr, cc_name(code));
    }
}

static void late_port_scan(Xhci *x);

static void xhci_service(UsbHc *hc) {
    Xhci *x = XH(hc);
    if (x->hc.dead) return;
    if (x->late_scan_at && uptime_ms() >= x->late_scan_at) {
        x->late_scan_at = 0;
        late_port_scan(x);
    }
    if (!x->need_service) return;
    x->need_service = false;
    for (u32 i = 1; i <= x->max_slots; i++) {
        XSlot *s = x->slots[i];
        if (!s || !s->dev) continue;
        for (int d = 2; d < 32; d++) {
            XEp *xe = &s->ep[d];
            if (!xe->need_recover) continue;
            xe->need_recover = false;
            if (s->dev->dead || xe->dead) continue;
            ep_recover(x, s, d, false);
            if (!s->dev->dead && !x->hc.dead) queue_int_td(x, s, xe, d);
            x->need_service = true;                                     /* re-check next round: recovery pumps events */
        }
    }
}

/* ---------------------------------------------------------------- freeing a device */

static void xhci_free_dev(UsbHc *hc, UsbDevice *dev) {
    Xhci *x = XH(hc);
    XSlot *s = dev_slot(dev);
    if (!s) return;
    dev->hcpriv = NULL;
    for (int i = 2; i < 32; i++) s->ep[i].dead = true;
    if (x->hc.dead) { s->dev = NULL; return; }                          /* the controller may still use these pages: leak them */
    CmdResult r;
    int rc = xhci_cmd(x, TRB_TYPE(TRB_CMD_DISABLE_SLOT) | TRB_SLOT(s->id), 0, &r);
    s->dev = NULL;                                                      /* the UsbDevice goes away right after this returns */
    if (rc && !(rc == -EIO && r.cc == CC_SLOT_NOT_ENABLED)) {
        klog("Kappa", "usb %s: could not disable slot %u (%d); leaking its memory", dev->path, s->id, rc);
        return;
    }
    x->dcbaa[s->id] = 0;
    x->slots[s->id] = NULL;
    slot_free_memory(x, s);
}

/* ---------------------------------------------------------------- root hub */

static inline u32 portsc_read(Xhci *x, int port) { return rd32(x->op, XOP_PORTSC(port)); }

static int root_get_status(UsbHub *hub, int port, UsbPortStatus *st) {
    Xhci *x = XH(hub->hc);
    memset(st, 0, sizeof *st);
    u32 v = portsc_read(x, port);
    if (v == 0xFFFFFFFFu) return -ENODEV;
    int proto = x->proto[port].major == 3 ? 1 : 0;
    st->connected = v & PORTSC_CCS;
    st->enabled = v & PORTSC_PED;
    st->resetting = v & PORTSC_PR;
    st->powered = v & PORTSC_PP;
    st->overcurrent = v & PORTSC_OCA;
    u32 pls = (v & PORTSC_PLS_MASK) >> PORTSC_PLS_SHIFT;
    st->link_state = (u8)pls;
    u32 id = (v & PORTSC_SPEED_MASK) >> PORTSC_SPEED_SHIFT;
    st->speed_id = (u8)id;
    st->speed = id ? x->speed_of[proto][id] : USB_SPEED_NONE;
    if (proto && !(st->connected && st->enabled && pls == PLS_U0))
        st->warm_reset = (v & PORTSC_CAS) || (pls == PLS_INACTIVE) || (pls == PLS_COMPLIANCE) || (x->stuck[port] && !st->connected);
    if (v & PORTSC_CSC) st->change |= UPC_CONNECT;
    if (v & PORTSC_PEC) st->change |= UPC_ENABLE;
    if (v & PORTSC_PRC) st->change |= UPC_RESET;
    if (v & PORTSC_WRC) st->change |= UPC_BH_RESET;
    if (v & PORTSC_OCC) st->change |= UPC_OVERCUR;
    if (v & PORTSC_PLC) st->change |= UPC_LINK;
    if (v & PORTSC_CEC) st->change |= UPC_CONFIG;
    return 0;
}

static void root_clear_change(UsbHub *hub, int port, u32 change) {
    Xhci *x = XH(hub->hc);
    u32 v = portsc_read(x, port);
    if (v == 0xFFFFFFFFu) return;
    u32 w = v & PORTSC_PRESERVE;                                        /* never write PED/PR/WPR/change bits back by accident */
    if (change & UPC_CONNECT) w |= PORTSC_CSC;
    if (change & UPC_ENABLE) w |= PORTSC_PEC;
    if (change & UPC_RESET) w |= PORTSC_PRC;
    if (change & UPC_BH_RESET) w |= PORTSC_WRC;
    if (change & UPC_OVERCUR) w |= PORTSC_OCC;
    if (change & UPC_LINK) w |= PORTSC_PLC;
    if (change & UPC_CONFIG) w |= PORTSC_CEC;
    wr32(x->op, XOP_PORTSC(port), w);
}

static int root_reset(UsbHub *hub, int port, bool warm, UsbPortStatus *out) {
    Xhci *x = XH(hub->hc);
    u32 v = portsc_read(x, port);
    if (v == 0xFFFFFFFFu) return -ENODEV;
    if (!(v & PORTSC_CCS) && !warm) return -ENODEV;
    if (warm) x->stuck[port] = 0;
    wr32(x->op, XOP_PORTSC(port), (v & PORTSC_PRESERVE) | (warm ? PORTSC_WPR : PORTSC_PR));
    u64 end = uptime_ms() + (warm ? 4000 : 1500);
    for (;;) {
        task_sleep_ms(5);
        x_pump(x);
        if (x->hc.dead) return -ENODEV;
        v = portsc_read(x, port);
        if (v == 0xFFFFFFFFu) return -ENODEV;
        if (!(v & PORTSC_PR) && (v & (warm ? (PORTSC_PRC | PORTSC_WRC) : PORTSC_PRC))) break;      /* a warm reset reports through WRC */
        if (!warm && !(v & PORTSC_CCS)) return -ENODEV;                 /* unplugged during the reset */
        if (uptime_ms() >= end) {
            usb_dbg("xhci: port %d reset did not finish (portsc %08x)", port, v);
            return -ETIMEDOUT;
        }
    }
    wr32(x->op, XOP_PORTSC(port), (v & PORTSC_PRESERVE) | PORTSC_PRC | PORTSC_WRC | PORTSC_CSC | PORTSC_PEC | PORTSC_PLC);
    x_sleep(x, warm ? 100 : 50);                                        /* reset recovery time (TRSTRCY is 10 ms; Linux waits 50) */
    int rc = root_get_status(hub, port, out);
    if (rc) return rc;
    if (!out->connected) return -ENODEV;
    if (!out->enabled) {
        usb_dbg("xhci: port %d not enabled after reset (portsc %08x)", port, portsc_read(x, port));
        return -EIO;
    }
    return 0;
}

static const UsbHubOps root_ops = { root_get_status, root_clear_change, root_reset, NULL };

/* One more look at every port a couple of seconds after start-up, for anything the first scan and the events missed: a connect
 * whose event was swallowed, or a USB 3 link that sits in Polling / compliance mode and never trains (Linux warm-resets those). */
static void late_port_scan(Xhci *x) {
    UsbHub *root = &x->hc.root;
    for (u32 i = 1; i <= x->max_ports; i++) {
        UsbPortStatus st;
        if (root_get_status(root, (int)i, &st)) continue;
        UsbPort *pt = &root->port[i - 1];
        if (x->proto[i].major == 3 && !st.connected && !pt->dev && (st.link_state == PLS_POLLING || st.link_state == PLS_COMPLIANCE)) {
            usb_dbg("xhci: port %u still in link state %u two seconds after start: warm reset", i, st.link_state);
            x->stuck[i] = 1;
            usb_hub_mark_port(root, (int)i);
        } else if (st.connected && !pt->dev && !pt->failed) {
            usb_hub_mark_port(root, (int)i);
        }
    }
}

/* ---------------------------------------------------------------- start-up */

static int x_wait_reg(Xhci *x, u32 off, u32 mask, u32 want, u32 timeout_ms) {
    u64 end = uptime_ms() + timeout_ms;
    for (;;) {
        u32 v = rd32(x->op, off);
        if (v == 0xFFFFFFFFu) return -ENODEV;
        if ((v & mask) == want) return 0;
        if (uptime_ms() >= end) return -ETIMEDOUT;
        task_sleep_ms(1);
    }
}

/* A controller that was started and then failed a sanity check is stopped again so it cannot touch memory we no longer track. */
static void x_stop(Xhci *x) {
    if (!x->running || !x->op) return;
    u32 cmd = rd32(x->op, XOP_USBCMD);
    if (cmd != 0xFFFFFFFFu) wr32(x->op, XOP_USBCMD, cmd & ~(CMD_RUN | CMD_INTE));
    x->running = false;
}

#define FAIL(x, ...) do { snprintf((x)->hc.status, sizeof (x)->hc.status, __VA_ARGS__); x_stop(x); return -EIO; } while (0)

/* Walks the extended capability list: Supported Protocol (port speeds, slot types) and USB Legacy Support. */
static void parse_ext_caps(Xhci *x) {
    u32 xecp = (x->hcc1 >> 16) & 0xFFFF;
    u32 off = xecp * 4;
    for (int guard = 0; xecp && guard < 64; guard++) {
        if (off + 16 > x->bar_size) break;
        u32 d0 = rd32(x->cap, off);
        u32 id = d0 & 0xFF, next = (d0 >> 8) & 0xFF;
        if (id == XECP_LEGACY) x->legacy_off = off;
        if (id == XECP_PROTOCOL && rd32(x->cap, off + 4) == 0x20425355u) {          /* "USB " */
            u32 major = d0 >> 24, minor = (d0 >> 16) & 0xFF;
            u32 d2 = rd32(x->cap, off + 8), d3 = rd32(x->cap, off + 12);
            u32 first = d2 & 0xFF, count = (d2 >> 8) & 0xFF, psic = (d2 >> 28) & 0xF;
            int pi = major == 3 ? 1 : 0;
            if (major == 2 || major == 3) {
                for (u32 p = first; p < first + count && p <= x->max_ports; p++) {
                    if (p < 1) continue;
                    x->proto[p].major = (u8)major;
                    x->proto[p].minor = (u8)minor;
                    x->proto[p].slot_type = (u8)(d3 & 0x1F);
                }
                for (u32 i = 0; i < psic && off + 16 + 4 * (i + 1) <= x->bar_size; i++) {
                    u32 psi = rd32(x->cap, off + 16 + 4 * i);
                    u32 psiv = psi & 0xF, exp = (psi >> 4) & 3, mant = psi >> 16;
                    u64 bps = (u64)mant;
                    for (u32 k = 0; k < exp; k++) bps *= 1000;
                    u8 cls;
                    if (major == 3) cls = bps >= 9000000000ull ? USB_SPEED_SUPER_PLUS : USB_SPEED_SUPER;
                    else cls = bps >= 400000000ull ? USB_SPEED_HIGH : bps >= 10000000ull ? USB_SPEED_FULL : USB_SPEED_LOW;
                    if (psiv && psiv < 16) { x->speed_of[pi][psiv] = cls; x->id_of[pi][cls] = (u8)psiv; }
                }
            }
        }
        if (!next) break;
        off += next * 4;
    }
}

/* Take the controller away from the firmware (USB Legacy Support capability), as Linux's quirk_usb_handoff_xhci(). Without
 * this the BIOS keeps servicing the controller from SMM and fights us for it. */
static void bios_handoff(Xhci *x) {
    if (!x->legacy_off) return;
    u32 off = x->legacy_off;
    u32 val = rd32(x->cap, off);
    if (val & LEGACY_BIOS_OWNED) {
        wr32(x->cap, off, val | LEGACY_OS_OWNED);
        bool ok = false;
        for (int i = 0; i < 1000; i++) {                                // up to one second
            if (!(rd32(x->cap, off) & LEGACY_BIOS_OWNED)) { ok = true; break; }
            task_sleep_ms(1);
        }
        if (!ok) {
            klog("Kappa", "xhci %02x:%02x.%x: firmware did not release the controller within 1 s; taking it anyway", x->pci->bus, x->pci->dev, x->pci->fn);
            wr32(x->cap, off, (val | LEGACY_OS_OWNED) & ~LEGACY_BIOS_OWNED);
        } else {
            klog("Kappa", "xhci %02x:%02x.%x: took the controller over from the firmware", x->pci->bus, x->pci->dev, x->pci->fn);
        }
    }
    /* Turn off every SMI the firmware may have enabled for this controller and acknowledge pending ones. */
    u32 ctl = rd32(x->cap, off + LEGACY_CTLSTS);
    ctl &= LEGACY_KEEP_MASK;
    ctl |= LEGACY_SMI_EVENTS;
    wr32(x->cap, off + LEGACY_CTLSTS, ctl);
}

/* Intel ports that can be routed to EHCI or xHCI: hand them all to xHCI (Linux usb_enable_intel_xhci_ports()). A no-op on
 * chipsets without the routing registers (Sunrise Point and later read zeros here). */
static void intel_route_ports(Xhci *x) {
    PciDev *p = x->pci;
    if (p->vendor != 0x8086) return;
    switch (p->device) {                                                /* the chipsets that have an EHCI to share ports with: 7, 8 and 9 series */
    case 0x1E31: case 0x8C31: case 0x9C31: case 0x8CB1: case 0x9CB1: break;
    default: return;                                                    /* Sunrise Point and newer are xHCI only: nothing to route */
    }
    u32 usb3prm = pci_read32(p, 0xDC), usb2prm = pci_read32(p, 0xD4);
    if (usb3prm == 0xFFFFFFFFu || usb2prm == 0xFFFFFFFFu) return;
    if (usb3prm) pci_write32(p, 0xD8, usb3prm);                         /* USB3_PSSEN: SuperSpeed terminations on */
    if (usb2prm) pci_write32(p, 0xD0, usb2prm);                         /* XUSB2PR: USB 2 data lines to xHCI */
    if (usb3prm || usb2prm) klog("Kappa", "xhci %02x:%02x.%x: routed %#x usb2 / %#x usb3 ports to xHCI", p->bus, p->dev, p->fn, usb2prm, usb3prm);
}

static int x_halt_reset(Xhci *x) {
    /* Wait for the controller to be ready before touching operational registers (Linux waits up to 5 s). */
    int rc = x_wait_reg(x, XOP_USBSTS, STS_CNR, 0, 3000);
    if (rc) FAIL(x, "controller not ready after 3 s (USBSTS %08x)", rc == -ENODEV ? 0xFFFFFFFFu : rd32(x->op, XOP_USBSTS));
    u32 cmd = rd32(x->op, XOP_USBCMD);
    if (!(rd32(x->op, XOP_USBSTS) & STS_HCH)) {
        wr32(x->op, XOP_USBCMD, cmd & ~(CMD_RUN | CMD_INTE | CMD_HSEE | CMD_EWE));
        rc = x_wait_reg(x, XOP_USBSTS, STS_HCH, STS_HCH, 200);
        if (rc) FAIL(x, "controller does not halt");
    }
    wr32(x->op, XOP_USBCMD, rd32(x->op, XOP_USBCMD) | CMD_RESET);
    if (x->pci->vendor == 0x8086) task_sleep_ms(2);                      /* Intel hosts need a pause before the first poll */
    rc = x_wait_reg(x, XOP_USBCMD, CMD_RESET, 0, 2000);
    if (rc) FAIL(x, "host controller reset did not finish");
    rc = x_wait_reg(x, XOP_USBSTS, STS_CNR, 0, 2000);
    if (rc) FAIL(x, "controller not ready after reset");
    return 0;
}

static void speed_tables_default(Xhci *x) {
    memset(x->id_of, 0, sizeof x->id_of);
    memset(x->speed_of, 0, sizeof x->speed_of);
    x->id_of[0][USB_SPEED_FULL] = 1; x->id_of[0][USB_SPEED_LOW] = 2; x->id_of[0][USB_SPEED_HIGH] = 3;
    x->id_of[1][USB_SPEED_SUPER] = 4; x->id_of[1][USB_SPEED_SUPER_PLUS] = 5;
    x->speed_of[0][1] = USB_SPEED_FULL; x->speed_of[0][2] = USB_SPEED_LOW; x->speed_of[0][3] = USB_SPEED_HIGH;
    x->speed_of[1][4] = USB_SPEED_SUPER; x->speed_of[1][5] = USB_SPEED_SUPER_PLUS;
    /* A USB 3 root port may report a USB 2 style id on odd controllers and vice versa: keep the other family's defaults too. */
    for (int a = 0; a < 2; a++) for (int i = 1; i < 6; i++) if (!x->speed_of[a][i]) x->speed_of[a][i] = x->speed_of[1 - a][i];
}

static int xhci_start(UsbHc *hc) {
    Xhci *x = XH(hc);
    PciDev *p = x->pci;
    void *bar = pci_map_bar(p, 0);
    if (!bar) FAIL(x, "BAR 0 is missing or unassigned");
    x->cap = bar;
    x->bar_size = p->bar[0].size;
    u32 caps = rd32(x->cap, 0);
    if (caps == 0xFFFFFFFFu || caps == 0) FAIL(x, "registers read as %#x (device not responding)", caps);
    u32 caplen = caps & 0xFF;
    x->hciversion = (u16)(caps >> 16);
    if (caplen < 0x10 || caplen > 0x200 || caplen + 0x500 > x->bar_size) FAIL(x, "implausible CAPLENGTH %#x (BAR %#lx bytes)", caplen, (unsigned long)x->bar_size);
    x->op = x->cap + caplen;
    x->hcs1 = rd32(x->cap, XCAP_HCSPARAMS1);
    x->hcs2 = rd32(x->cap, XCAP_HCSPARAMS2);
    x->hcs3 = rd32(x->cap, XCAP_HCSPARAMS3);
    x->hcc1 = rd32(x->cap, XCAP_HCCPARAMS1);
    u32 dboff = rd32(x->cap, XCAP_DBOFF) & ~3u, rtsoff = rd32(x->cap, XCAP_RTSOFF) & ~0x1Fu;
    if (dboff + 4 * 256 > x->bar_size || rtsoff + 0x40 > x->bar_size) FAIL(x, "doorbell/runtime offsets %#x/%#x outside the BAR", dboff, rtsoff);
    x->db = (volatile u32 *)(x->cap + dboff);
    x->rt = x->cap + rtsoff;
    x->max_slots = x->hcs1 & 0xFF;
    x->max_ports = x->hcs1 >> 24;
    x->ac64 = x->hcc1 & HCC1_AC64;
    x->csz = (x->hcc1 & HCC1_CSZ) ? 64 : 32;
    if (!x->max_slots || !x->max_ports) FAIL(x, "controller reports %u slots and %u ports", x->max_slots, x->max_ports);

    x->proto = kzalloc((x->max_ports + 1) * sizeof(XProto));
    x->slots = kzalloc((x->max_slots + 1) * sizeof(XSlot *));
    x->stuck = kzalloc(x->max_ports + 1);
    if (!x->proto || !x->slots || !x->stuck) FAIL(x, "out of memory");
    speed_tables_default(x);
    parse_ext_caps(x);
    for (u32 i = 1; i <= x->max_ports; i++) if (!x->proto[i].major) x->proto[i].major = 2;

    bios_handoff(x);
    intel_route_ports(x);
    int rc = x_halt_reset(x);
    if (rc) return rc;

    u32 pagesize = rd32(x->op, XOP_PAGESIZE);
    if (!(pagesize & 1)) FAIL(x, "controller does not support 4 KiB pages (PAGESIZE %#x)", pagesize);

    /* Device context base array, scratchpad buffers, rings. */
    x->dcbaa = x_dma(x, (x->max_slots + 1) * 8, &x->dcbaa_phys);
    x->nsp = (((x->hcs2 >> 21) & 0x1F) << 5) | ((x->hcs2 >> 27) & 0x1F);
    if (x->nsp) {
        x->sp_array = x_dma(x, x->nsp * 8, &x->sp_array_phys);
        x->sp_pages = kzalloc(x->nsp * sizeof(void *));
        if (!x->sp_array || !x->sp_pages) FAIL(x, "out of memory (scratchpad)");
        for (u32 i = 0; i < x->nsp; i++) {
            paddr_t pp;
            x->sp_pages[i] = x_dma(x, 4096, &pp);
            if (!x->sp_pages[i]) FAIL(x, "out of memory (scratchpad buffer %u)", i);
            x->sp_array[i] = pp;
        }
        x->dcbaa[0] = x->sp_array_phys;
    }
    XRing *cr = ring_alloc(x);
    x->evt = x_dma(x, RING_TRBS * sizeof(Trb), &x->evt_phys);
    x->erst = x_dma(x, 4096, &x->erst_phys);
    x->ctl_buf = x_dma(x, CTL_BUF_SIZE, &x->ctl_phys);
    if (!x->dcbaa || !cr || !x->evt || !x->erst || !x->ctl_buf) FAIL(x, "out of DMA memory");
    x->cmd = *cr;
    kfree(cr);
    x->evt_deq = 0;
    x->evt_cycle = 1;
    ((u32 *)x->erst)[0] = (u32)x->evt_phys;                             /* ERST entry: segment base, size, reserved */
    ((u32 *)x->erst)[1] = (u32)(x->evt_phys >> 32);
    ((u32 *)x->erst)[2] = g_ring_trbs;
    ((u32 *)x->erst)[3] = 0;

    wr32(x->op, XOP_CONFIG, (rd32(x->op, XOP_CONFIG) & ~0xFFu) | x->max_slots);
    wr64(x->op, XOP_DCBAAP, x->dcbaa_phys);
    wr64(x->op, XOP_CRCR, x->cmd.phys | CRCR_RCS);
    wr32(x->rt, XRT_IR(0) + XIR_ERSTSZ, 1);
    wr64(x->rt, XRT_IR(0) + XIR_ERDP, x->evt_phys);
    wr64(x->rt, XRT_IR(0) + XIR_ERSTBA, x->erst_phys);                  /* writing the base address enables the event ring */
    wr32(x->rt, XRT_IR(0) + XIR_IMAN, 1);                               /* clear IP, leave IE = 0: we poll */
    wr32(x->op, XOP_USBSTS, STS_RW1C);

    wr32(x->op, XOP_USBCMD, CMD_RUN);                                   /* INTE stays 0 */
    rc = x_wait_reg(x, XOP_USBSTS, STS_HCH, 0, 500);
    if (rc) FAIL(x, "controller does not start");
    x->running = true;

    rc = xhci_cmd(x, TRB_TYPE(TRB_CMD_NOOP), 0, NULL);                  /* does the command ring work at all? */
    if (rc) FAIL(x, "command ring does not answer (No-Op failed: %d)", rc);

    /* Root hub. */
    UsbHub *root = &x->hc.root;
    root->ops = &root_ops;
    root->hc = hc;
    root->dev = NULL;
    root->nports = (int)x->max_ports;
    int usb3 = 0;
    for (u32 i = 1; i <= x->max_ports; i++) if (x->proto[i].major == 3) usb3++;
    root->ss = false;
    root->port = kzalloc(x->max_ports * sizeof(UsbPort));
    if (!root->port) FAIL(x, "out of memory");

    bool powered_any = false;
    for (u32 i = 1; i <= x->max_ports; i++) {
        u32 v = portsc_read(x, (int)i);
        if (!(v & PORTSC_PP)) { wr32(x->op, XOP_PORTSC(i), (v & PORTSC_PRESERVE) | PORTSC_PP); powered_any = true; }
    }
    if (powered_any) task_sleep_ms(150);                                // power-good time
    snprintf(hc->desc, sizeof hc->desc, "xHCI %04x:%04x at %02x:%02x.%x, v%x.%02x, %u ports (%d usb2, %d usb3), %u slots, %u-byte contexts%s",
             p->vendor, p->device, p->bus, p->dev, p->fn, x->hciversion >> 8, x->hciversion & 0xFF, x->max_ports, (int)x->max_ports - usb3, usb3,
             x->max_slots, x->csz, x->ac64 ? "" : ", 32-bit DMA");
    klog("Kappa", "%s, scratchpad %u", hc->desc, x->nsp);
    task_sleep_ms(100);                                                 // let USB 2 ports debounce and USB 3 links train

    /* Look at every port once, then let the change events drive everything. */
    char list[96] = "";
    int occupied = 0;
    for (u32 i = 1; i <= x->max_ports; i++) {
        UsbPortStatus st;
        root_get_status(root, (int)i, &st);
        if (st.change) root_clear_change(root, (int)i, st.change);
        if (st.connected || st.warm_reset) {
            usb_hub_mark_port(root, (int)i);
            occupied++;
            size_t l = strlen(list);
            if (l + 8 < sizeof list) snprintf(list + l, sizeof list - l, " %u", i);
        }
    }
    klog("Kappa", "xhci %02x:%02x.%x: %d of %u ports have something attached%s%s", p->bus, p->dev, p->fn, occupied, x->max_ports,
         occupied ? ":" : "", list);
    x->late_scan_at = uptime_ms() + 2000;
    usb_dbg("xhci: CAPLENGTH %#x HCSPARAMS %08x %08x %08x HCCPARAMS1 %08x DBOFF %#x RTSOFF %#x USBSTS %08x", caplen, x->hcs1, x->hcs2, x->hcs3, x->hcc1,
            dboff, rtsoff, rd32(x->op, XOP_USBSTS));
    return 0;
}

/* One line for the log when something is wrong: every root port as "number:PORTSC" (decode with xHCI 1.1 table 5-27). */
static size_t xhci_port_summary(UsbHc *hc, char *buf, size_t cap) {
    Xhci *x = XH(hc);
    size_t n = 0;
    buf[0] = 0;
    if (!x->op) return 0;
    for (u32 i = 1; i <= x->max_ports && n + 16 < cap; i++)
        n += (size_t)snprintf(buf + n, cap - n, "%s%u:%x", i > 1 ? " " : "", i, rd32(x->op, XOP_PORTSC(i)));
    return n;
}

static void xhci_poll(UsbHc *hc) { x_pump(XH(hc)); }

static const UsbHcOps xhci_ops = {
    .start = xhci_start,
    .poll = xhci_poll,
    .service = xhci_service,
    .alloc_dev = xhci_alloc_dev,
    .address_dev = xhci_address_dev,
    .set_ep0_size = xhci_set_ep0_size,
    .add_endpoints = xhci_add_endpoints,
    .update_hub = xhci_update_hub,
    .free_dev = xhci_free_dev,
    .control = xhci_control,
    .int_start = xhci_int_start,
    .bulk = xhci_bulk,
    .port_summary = xhci_port_summary,
};

UsbHc *xhci_create(PciDev *pci, int busnum) {
    Xhci *x = kzalloc(sizeof *x);
    if (!x) return NULL;
    x->pci = pci;
    x->hc.ops = &xhci_ops;
    x->hc.pci = pci;
    x->hc.busnum = busnum;
    snprintf(x->hc.desc, sizeof x->hc.desc, "xHCI %04x:%04x at %02x:%02x.%x", pci->vendor, pci->device, pci->bus, pci->dev, pci->fn);
    char ringopt[8];
    if (cmdline_value("usbring", ringopt, sizeof ringopt)) {
        u32 n = 0;
        for (const char *q = ringopt; *q >= '0' && *q <= '9'; q++) n = n * 10 + (u32)(*q - '0');
        if (n >= 16 && n <= RING_TRBS) g_ring_trbs = n;
    }
    char fault[24];
    if (cmdline_value("usbfault", fault, sizeof fault) && (!strncmp(fault, "ctl:", 4) || !strncmp(fault, "cmd:", 4))) {
        u32 n = 0;
        for (const char *q = fault + 4; *q >= '0' && *q <= '9'; q++) n = n * 10 + (u32)(*q - '0');
        if (fault[1] == 't') g_fault_ctl = n; else g_fault_cmd = n;
    }
    /* Make sure the function is in D0 and decodes memory; the BIOS may have left it asleep. */
    u8 pm = pci_find_cap(pci, PCI_CAP_PM);
    if (pm) {
        u16 pmcsr = pci_read16(pci, (u16)(pm + 4));
        if (pmcsr & 3) { pci_write16(pci, (u16)(pm + 4), (u16)(pmcsr & ~3u)); udelay(10000); }
    }
    pci_enable(pci, PCI_CMD_MEM | PCI_CMD_MASTER);
    pci->driver = "xhci";
    return &x->hc;
}
