/*
 * Intel gigabit Ethernet, the legacy-descriptor family: e1000 (82540EM / 82545EM / 82544GC, QEMU's `-device e1000`), e1000e
 * (82574L, QEMU's `-device e1000e`) and the PCH LAN parts of real desktops (I217, I218, I219: the Dell target's I219-LM).
 *
 * Polled from the Yukari thread: 256 receive and 256 transmit descriptors, legacy format, 2 KiB buffers, no interrupts, no
 * offloads. Link changes are noticed by polling STATUS.LU every tick, so an unplugged cable never blocks anything and
 * a cable plugged in later is picked up (the network core then restarts DHCP).
 *
 * The PCH parts are the risky ones (their PHY is shared with the management engine, "ME"). For them this driver:
 *   - never issues a global reset and never resets the PHY (the firmware left the link working; Linux's reset path needs the
 *     ME handshakes below and a dozen PHY workarounds that cannot be tried here);
 *   - tells the ME that a driver owns the card (CTRL_EXT.DRV_LOAD) and, on the parts that have it (I218 LM/V, I218-3, I219),
 *     asks the ME to leave ULP (ultra low power) mode, which would keep the link down, with the H2ME/FWSM handshake of
 *     Linux's e1000_disable_ulp_lpt_lp(), bounded to 300 ms;
 *   - looks at the PHY control register through MDIC under the software semaphore (EXTCNF_CTRL.SWFLAG) and clears a
 *     power-down bit that an earlier OS (Windows at shutdown) may have left, with every wait bounded;
 *   - programs every receive/transmit register that matters explicitly instead of trusting what the firmware left;
 *   - watches the transmit ring (the SPT/KBL parts can hang it) and restarts it after two seconds without progress.
 * If the registers do not answer or the MAC address looks wrong it logs one line and leaves the card alone.
 * None of the PCH paths could be exercised in QEMU: see docs/NETWORK.md for what is unverified.
 */
#include "net.h"
#include "pci.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "irq.h"
#include "initcall.h"
#include "cmdline.h"

#define INTEL 0x8086

#define R_CTRL    0x0000
#define R_STATUS  0x0008
#define R_FEXTNVM6 0x0010
#define R_FEXTNVM4 0x0024
#define R_MDIC    0x0020
#define R_CTRLEXT 0x0018
#define R_ICR     0x00C0
#define R_IMC     0x00D8
#define R_RCTL    0x0100
#define R_TCTL    0x0400
#define R_TIPG    0x0410
#define R_PBA     0x1000
#define R_EEMNGCTL 0x1010
#define R_EXTCNF  0x0F00
#define R_PCIEANACFG 0x0F18
#define R_IOSFPC  0x0F28
#define R_RDBAL   0x2800
#define R_RDBAH   0x2804
#define R_RDLEN   0x2808
#define R_RDH     0x2810
#define R_RDT     0x2818
#define R_RDTR    0x2820
#define R_RXDCTL  0x2828
#define R_RADV    0x282C
#define R_TDBAL   0x3800
#define R_TDBAH   0x3804
#define R_TDLEN   0x3808
#define R_TDH     0x3810
#define R_TDT     0x3818
#define R_TIDV    0x3820
#define R_TXDCTL  0x3828
#define R_TADV    0x382C
#define R_TARC0   0x3840
#define R_TDLEN1  0x3908                /* the second transmit queue: nothing is queued on it, but the errata want it set up like the first */
#define R_TXDCTL1 0x3928
#define R_TARC1   0x3940
#define R_FEXTNVM11 0x5BBC
#define R_RXCSUM  0x5000
#define R_RFCTL   0x5008
#define R_MTA     0x5200
#define R_RAL0    0x5400
#define R_RAH0    0x5404
#define R_MRQC    0x5818
#define R_H2ME    0x5B50
#define R_FWSM    0x5B54
#define R_GPRC    0x4074                /* statistics: good packets received / transmitted, total packets transmitted */
#define R_GPTC    0x4080
#define R_TPT     0x40D4

#define CTRL_SLU   (1u << 6)
#define CTRL_ASDE  (1u << 5)
#define CTRL_ILOS  (1u << 7)
#define CTRL_RST   (1u << 26)
#define CTRL_VME   (1u << 30)
#define CTRL_PHYRST (1u << 31)
#define CTRL_LRST  (1u << 3)
#define CTRL_FRCSPD (1u << 11)
#define CTRL_FRCDPX (1u << 12)
#define CTRLEXT_DRV_LOAD (1u << 28)
#define STATUS_FD  (1u << 0)
#define STATUS_LU  (1u << 1)
#define RCTL_EN    (1u << 1)
#define RCTL_BAM   (1u << 15)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)
#define QUEUE_ENABLE (1u << 25)
#define DESC_WB    0x01010000u          /* GRAN = descriptors, WTHRESH = 1: write each descriptor back as soon as it is done */
#define EXTCNF_SWFLAG (1u << 5)
#define H2ME_ULP   0x00000800u
#define H2ME_ENFORCE 0x00001000u
#define FWSM_FW_VALID 0x00008000u
#define FWSM_ULP_CFG_DONE 0x00000400u
#define FEXTNVM6_K1_OFF   0x80000000u   /* Sunrise Point / Kaby Lake: the part says in PCIEANACFG whether K1 must stay off at 1 Gb/s (a MAC with K1 on can miss DMA completions) */
#define FEXTNVM11_NO_MULR 0x00002000u   /* "disable MULR fix": Linux sets it before every descriptor ring flush */
#define PCI_DESC_RING_STATUS 0xE4       /* config space: bit 8 = the card wants its descriptor rings flushed before it can be reset */
#define FLUSH_DESC_REQUIRED  0x0100u
#define TCTL_MULR  (1u << 28)

#define NDESC   256
#define BUFSZ   2048
#define NTXVAR  8                       /* ways of starting the transmit unit, tried one after the other when it hangs (g_txvar) */
#define TX_GIVE_UP (3 * NTXVAR)         /* restarts of a card that never transmits before the driver stops trying (three rounds of the ladder) */

typedef struct { u64 addr; u16 len; u16 csum; u8 status; u8 errors; u16 special; } PACKED RxDesc;
typedef struct { u64 addr; u16 len; u8 cso; u8 cmd; u8 status; u8 css; u16 special; } PACKED TxDesc;

typedef struct EDev {
    PciDev *pci;
    volatile u8 *mmio;
    bool pch;                           /* I217 / I218 / I219: be careful */
    bool e1000e;                        /* 82574L and the PCH parts: queue-enable bits in RXDCTL/TXDCTL */
    bool spt;                           /* I219 on Sunrise Point / Kaby Lake: transmit hang errata */
    bool ulp;                           /* the part can be in ULP mode and the ME has to be asked to leave it */
    RxDesc *rx;
    TxDesc *tx;
    paddr_t rx_phys, tx_phys, rxbuf_phys, txbuf_phys;
    u8 *rxbuf, *txbuf;
    u16 rx_next;                        /* next descriptor to examine */
    u16 tx_head, tx_tail;               /* software view: tail = next to fill, head = oldest unfinished */
    u64 tx_stuck_since;                 /* when the transmit ring last had work and made no progress; 0 = fine */
    u32 tx_resets;
    int last_link;                      /* -1 unknown, 0 down, 1 up */
    int txvar;                          /* which way of starting the transmit unit is in use (see g_txvar); the watchdog moves on to the next when it hangs */
    int txstall;                        /* test hook "e1000.txstall=N": the doorbell is ignored until variant N (a pretend hang) */
    bool var_reported;                  /* "transmit works with variant N" has been said */
    bool no_errata;                     /* variant 3: leave out the errata bits of the PCH parts */
    bool tx_ok;                         /* the card has completed at least one frame since the last variant was applied */
    bool kick_dhcp;                     /* the first completed frame after a hang: start DHCP again at once instead of at its next retry */
    bool gave_up;                       /* every variant has been tried TX_GIVE_UP times: no more restarts */
    u64 link_since;                     /* when the link last came up (ms), 0 while it is down */
    u64 last_probe;                     /* when the last probe frame was queued (ms) */
    const char *model;                  /* "I219-LM5" */
} EDev;

static void e_dump(const char *who, EDev *e, const char *why, bool full);
static void tx_apply_variant(NetDev *d, EDev *e, int var);
static void pch_link_fixes(EDev *e);

static inline u32 rr(EDev *e, u32 reg) { return *(volatile u32 *)(e->mmio + reg); }
static inline void wr(EDev *e, u32 reg, u32 v) { *(volatile u32 *)(e->mmio + reg) = v; }
static inline void flush(EDev *e) { (void)rr(e, R_STATUS); }

static const struct { u16 id; u8 kind; bool ulp; const char *name; } g_ids[] = {
    { 0x100E, 0, 0, "82540EM" }, { 0x100F, 0, 0, "82545EM" }, { 0x1004, 0, 0, "82544GC" }, { 0x1008, 0, 0, "82544EI" }, { 0x1019, 0, 0, "82547EI" },
    { 0x101E, 0, 0, "82540EP" }, { 0x1026, 0, 0, "82545GM" }, { 0x1076, 0, 0, "82541GI" }, { 0x107C, 0, 0, "82541PI" }, { 0x1010, 0, 0, "82546EB" },
    { 0x10D3, 1, 0, "82574L" }, { 0x10F6, 1, 0, "82574LA" },
    { 0x153A, 2, 0, "I217-LM" }, { 0x153B, 2, 0, "I217-V" }, { 0x155A, 2, 1, "I218-LM" }, { 0x1559, 2, 1, "I218-V" }, { 0x15A0, 2, 0, "I218-LM2" },
    { 0x15A1, 2, 0, "I218-V2" }, { 0x15A2, 2, 1, "I218-LM3" }, { 0x15A3, 2, 1, "I218-V3" },
    { 0x156F, 3, 1, "I219-LM" }, { 0x1570, 3, 1, "I219-V" }, { 0x15B7, 3, 1, "I219-LM2" }, { 0x15B8, 3, 1, "I219-V2" }, { 0x15B9, 3, 1, "I219-LM3" },
    { 0x15BB, 3, 1, "I219-LM7" }, { 0x15BC, 3, 1, "I219-V7" }, { 0x15BD, 3, 1, "I219-LM6" }, { 0x15BE, 3, 1, "I219-V6" }, { 0x15D6, 3, 1, "I219-V5" },
    { 0x15D7, 3, 1, "I219-LM4" }, { 0x15D8, 3, 1, "I219-V4" }, { 0x15E3, 3, 1, "I219-LM5" },
};

/* ---- transmit and receive rings ---- */

static void tx_program(EDev *e) {
    wr(e, R_TDBAL, (u32)e->tx_phys);
    wr(e, R_TDBAH, (u32)(e->tx_phys >> 32));
    wr(e, R_TDLEN, NDESC * 16);
    wr(e, R_TDH, 0);
    wr(e, R_TDT, 0);
    wr(e, R_TIDV, 0);
    wr(e, R_TADV, 0);
    wr(e, R_TIPG, e->pch ? 0x00602008u : 0x0060200Au);
    if (e->e1000e) wr(e, R_TXDCTL, DESC_WB | (e->no_errata ? 0 : (1u << 22)) | QUEUE_ENABLE | 0x1Fu);     /* bit 22: count descriptors still to be processed */
    if (e->pch && !e->no_errata) wr(e, R_TXDCTL1, rr(e, R_TXDCTL) & ~QUEUE_ENABLE);                     /* Linux: "set txdctl the same for both queues" (queue 1 stays unused) */
    wr(e, R_TCTL, TCTL_EN | TCTL_PSP | (0x0Fu << 4) | (0x40u << 12));
    flush(e);
}

static void tx_reclaim(EDev *e) {
    u16 before = e->tx_head;
    while (e->tx_head != e->tx_tail && (e->tx[e->tx_head].status & 1)) {
        e->tx[e->tx_head].status = 0;
        e->tx_head = (u16)((e->tx_head + 1) % NDESC);
    }
    /* A card that fetched the descriptors but never wrote the done bit back (the write-back setting differs between parts) still moved its head
     * pointer: trust that too, as long as it lies between our head and tail. */
    u32 hw = rr(e, R_TDH);
    u16 pending = (u16)((e->tx_tail + NDESC - e->tx_head) % NDESC);
    u16 taken = (u16)((hw + NDESC - e->tx_head) % NDESC);
    if (hw < NDESC && pending && taken <= pending && taken) {
        for (u16 k = 0; k < taken; k++) { e->tx[e->tx_head].status = 0; e->tx_head = (u16)((e->tx_head + 1) % NDESC); }
    }
    if (e->tx_head != before) {
        e->tx_stuck_since = 0;
        e->tx_ok = true;
        if (e->tx_resets && !e->var_reported) {
            e->var_reported = true;
            e->kick_dhcp = true;
            klog("Yukari", "transmit works with variant %d", e->txvar);
        }
    }
}

/* Puts one frame on the ring and rings the doorbell. */
static int tx_enqueue(EDev *e, const void *data, u16 len) {
    u16 next = (u16)((e->tx_tail + 1) % NDESC);
    if (next == e->tx_head || len > BUFSZ) return -1;
    u16 i = e->tx_tail;
    memcpy(e->txbuf + (size_t)i * BUFSZ, data, len);
    e->tx[i].addr = e->txbuf_phys + (u64)i * BUFSZ;
    e->tx[i].len = len;
    e->tx[i].cso = 0;
    e->tx[i].cmd = 0x0B;                                         /* EOP | IFCS | RS */
    e->tx[i].status = 0;
    e->tx[i].css = 0;
    e->tx[i].special = 0;
    barrier();
    if (e->tx_head == e->tx_tail) e->tx_stuck_since = net_now();
    e->tx_tail = next;
    if (e->txstall <= e->txvar) wr(e, R_TDT, next);                /* (test hook: a pretend hang swallows the doorbell until the ladder reaches variant txstall) */
    return 0;
}

static int e_tx(NetDev *d, NetBuf *nb) {
    EDev *e = d->priv;
    tx_reclaim(e);
    if (tx_enqueue(e, nb_data(nb), nb->len)) { d->tx_dropped++; nb_free(nb); return -ENOBUFS; }
    d->tx_packets++;
    d->tx_bytes += nb->len;
    nb_free(nb);
    return 0;
}

/* A card that has not completed a single frame since it was (re)started gets one of its own every 1.5 s once the link has been up for 3 s, so that the
 * transmit variants walk on at the pace of the watchdog and not at the pace of the upper layers' retries (DHCP pauses for a minute after its first rounds).
 * A broadcast frame with the IEEE 802 "local experimental" ethertype; nobody answers it and nothing on a LAN minds it. */
static void tx_probe(NetDev *d, EDev *e) {
    u8 f[60] = { 0 };
    memset(f, 0xFF, 6);
    memcpy(f + 6, d->mac, 6);
    f[12] = 0x88;
    f[13] = 0xB5;
    memcpy(f + 14, "TouhouOS transmit probe", 23);
    tx_enqueue(e, f, sizeof f);
}

/* A transmit queue that has work and has not moved for two seconds is restarted (the I219 on Sunrise Point and Kaby Lake is
 * known to hang it). The frames in it are lost; TCP resends them. */
static void tx_watchdog(NetDev *d, EDev *e) {
    tx_reclaim(e);
    u64 now = net_now();
    if (e->kick_dhcp) {
        e->kick_dhcp = false;
        if (!d->addr) dhcp_start(d);                                /* transmit just came back: do not wait for the next DHCP retry */
    }
    if (e->last_link == 1 && !e->tx_ok && !e->gave_up && e->tx_head == e->tx_tail && e->link_since && now - e->link_since > 3000 && now - e->last_probe > 1500) {
        e->last_probe = now;
        tx_probe(d, e);
    }
    if (e->tx_head == e->tx_tail || e->last_link != 1) { e->tx_stuck_since = 0; return; }       /* nothing queued, or no link: a card without carrier owes nobody a completion */
    if (!e->tx_stuck_since) { e->tx_stuck_since = now; return; }
    if (now - e->tx_stuck_since < 2000) return;
    if (e->tx_resets >= TX_GIVE_UP) {
        if (!e->gave_up) {
            e->gave_up = true;
            klog("Yukari", "%s: the transmit unit did not come back after %u restarts (all %d variants, three rounds); not restarting it again", d->name, e->tx_resets, NTXVAR);
            e_dump(d->name, e, "given up", true);
        }
        e->tx_stuck_since = now;
        return;
    }
    d->tx_errors += (u64)((e->tx_tail + NDESC - e->tx_head) % NDESC);
    if (e->tx_resets < NTXVAR + 2) {
        klog("Yukari", "%s: the transmit queue stopped moving (variant %d)", d->name, e->txvar);
        e_dump(d->name, e, "stuck", true);
    }
    e->tx_resets++;
    e->var_reported = false;
    e->tx_ok = false;
    e->txvar = (e->txvar + 1) % NTXVAR;                            /* every new hang tries the next way of starting the transmit unit */
    tx_apply_variant(d, e, e->txvar);
}

static void e_poll(NetDev *d) {
    EDev *e = d->priv;
    int budget = 64;
    while (budget-- > 0) {
        RxDesc *r = &e->rx[e->rx_next];
        if (!(r->status & 1)) break;                             /* DD: the card is done with it */
        barrier();
        u16 len = r->len;
        bool ok = (r->status & 2) && !r->errors && len >= 14 && len <= BUFSZ;
        if (ok) {
            NetBuf *nb = nb_alloc();
            if (nb && len <= nb_room(nb)) {
                memcpy(nb_put(nb, len), e->rxbuf + (size_t)e->rx_next * BUFSZ, len);
                nb->dev = d;
                r->status = 0;
                u16 done = e->rx_next;
                e->rx_next = (u16)((e->rx_next + 1) % NDESC);
                wr(e, R_RDT, done);                              /* give the descriptor back before the packet is processed */
                net_rx(d, nb);
                continue;
            }
            nb_free(nb);
            d->rx_dropped++;
        } else {
            d->rx_errors++;
        }
        r->status = 0;
        u16 done = e->rx_next;
        e->rx_next = (u16)((e->rx_next + 1) % NDESC);
        wr(e, R_RDT, done);
    }
    u32 st = rr(e, R_STATUS);
    int up = (st & STATUS_LU) != 0;
    if (up != e->last_link) {
        e->last_link = up;
        e->link_since = up ? net_now() : 0;
        if (up) {
            static const int mbps[4] = { 10, 100, 1000, 1000 };
            klog("Yukari", "%s: link up, %d Mb/s %s duplex", d->name, mbps[(st >> 6) & 3], (st & STATUS_FD) ? "full" : "half");
            if (e->pch) pch_link_fixes(e);                           /* what Linux redoes on every link-up of these parts */
        }
    }
    net_set_link(d, up);
    tx_watchdog(d, e);
}

/* ---- the PCH extras (see the header comment) ---- */

static bool swflag_acquire(EDev *e) {
    int i;
    for (i = 0; i < 1000 && (rr(e, R_EXTCNF) & EXTCNF_SWFLAG); i++) udelay(1000);       /* the ME or the firmware holds it */
    if (i == 1000) return false;
    wr(e, R_EXTCNF, rr(e, R_EXTCNF) | EXTCNF_SWFLAG);
    for (i = 0; i < 1000; i++) {
        if (rr(e, R_EXTCNF) & EXTCNF_SWFLAG) return true;
        udelay(1000);
    }
    return false;
}

static void swflag_release(EDev *e) { wr(e, R_EXTCNF, rr(e, R_EXTCNF) & ~EXTCNF_SWFLAG); }

static int mdic_op(EDev *e, u8 reg, u16 data, bool write, u16 *out) {
    u32 cmd = (u32)reg << 16 | 1u << 21 | (write ? 1u << 26 : 2u << 26) | (write ? data : 0);       /* PHY address 1 on every PCH part */
    wr(e, R_MDIC, cmd);
    for (int i = 0; i < 1920; i++) {                                                               /* about 96 ms, like Linux */
        udelay(50);
        u32 v = rr(e, R_MDIC);
        if (v & (1u << 28)) {
            if (v & (1u << 30)) return -1;
            if (out) *out = (u16)v;
            return 0;
        }
    }
    return -1;
}

/* Powers the PHY up if something switched it off, and says what it found. Page 0 is selected first (register 31): the
 * firmware may have left another page selected, and register 0 on that page is a different register. */
static void pch_phy_check(EDev *e, const char *name) {
    if (!swflag_acquire(e)) { klog("Yukari", "%s: the PHY semaphore stays busy; leaving the PHY alone", name); return; }
    u16 bmcr = 0, bmsr = 0;
    if (mdic_op(e, 31, 0, true, NULL) == 0 && mdic_op(e, 0, 0, false, &bmcr) == 0 && mdic_op(e, 1, 0, false, &bmsr) == 0) {
        if (bmcr & 0x0800) {                                      /* BMCR.PDOWN */
            bmcr = (u16)((bmcr & ~0x0800) | 0x1200);              /* power up, autonegotiation on and restarted */
            if (mdic_op(e, 0, bmcr, true, NULL) == 0) klog("Yukari", "%s: the PHY was powered down; switched it on", name);
        }
    } else {
        klog("Yukari", "%s: the PHY does not answer on MDIC (the ME may own it); relying on what the firmware set up", name);
    }
    swflag_release(e);
}

/* Linux's e1000_disable_ulp_lpt_lp() for the case where the ME is alive: ask it to take the PHY out of ULP mode and wait for it. */
static void pch_leave_ulp(EDev *e, const char *name) {
    if (!(rr(e, R_FWSM) & FWSM_FW_VALID)) {
        klog("Yukari", "%s: no management engine answers; ULP state unknown (not touched)", name);
        return;
    }
    wr(e, R_H2ME, (rr(e, R_H2ME) & ~H2ME_ULP) | H2ME_ENFORCE);
    int i;
    for (i = 0; i < 30 && (rr(e, R_FWSM) & FWSM_ULP_CFG_DONE); i++) udelay(10000);
    wr(e, R_H2ME, rr(e, R_H2ME) & ~H2ME_ENFORCE);
    if (i == 30) klog("Yukari", "%s: the ME did not finish leaving ULP mode within 300 ms", name);
}

/* ---- the transmit variants ---- */

static const char *const g_txvar[NTXVAR] = {
    "the plain restart (ring programmed again, queue enabled, transmitter on)",
    "the transmit unit switched off and idle first, queue enable polled",
    "a MAC reset (the PHY is left alone), then everything programmed again",
    "no errata bits (TARC0, TXDCTL bit 22, IOSFPC), the unit switched off first",
    "the descriptor rings flushed the way Linux does it for this part, then a MAC reset",
    "a MAC and PHY reset, then everything programmed again",
    "the card put to sleep (D3hot) and woken, then a MAC reset",
    "receive stopped, transmit started on its own first, then receive again",
};

/* What a photograph of the screen has to say about a card that does not transmit: the registers that matter and the statistics counters (GPTC grows
 * if frames really leave the card, even when the descriptors never come back as done). `full` adds the power, link and second-queue registers and
 * the descriptor the card is stuck on. */
static void e_dump(const char *who, EDev *e, const char *why, bool full) {
    klog("Yukari", "%s: %s: CTRL %08x STATUS %08x CTRLEXT %08x TCTL %08x TXDCTL %08x TARC0 %08x", who, why, rr(e, R_CTRL), rr(e, R_STATUS),
         rr(e, R_CTRLEXT), rr(e, R_TCTL), rr(e, R_TXDCTL), rr(e, R_TARC0));
    klog("Yukari", "%s: %s: TDBAL %08x TDLEN %u TDH %u TDT %u (software %u/%u) GPTC %u TPT %u GPRC %u FWSM %08x EXTCNF %08x pci04 %08x pciE4 %08x", who, why,
         rr(e, R_TDBAL), rr(e, R_TDLEN), rr(e, R_TDH), rr(e, R_TDT), e->tx_head, e->tx_tail, rr(e, R_GPTC), rr(e, R_TPT), rr(e, R_GPRC),
         e->pch ? rr(e, R_FWSM) : 0u, e->pch ? rr(e, R_EXTCNF) : 0u, pci_read32(e->pci, 0x04), pci_read32(e->pci, PCI_DESC_RING_STATUS));
    if (!full) return;
    u8 pcie = pci_find_cap(e->pci, PCI_CAP_PCIE), pm = pci_find_cap(e->pci, PCI_CAP_PM);
    u16 h = e->tx_head % NDESC;
    const TxDesc *td = e->tx ? &e->tx[h] : NULL;
    klog("Yukari", "%s: %s: FEXTNVM6 %08x FEXTNVM11 %08x PCIEANACFG %08x PBA %08x TXDCTL1 %08x TARC1 %08x TDLEN1 %u RXDCTL %08x RCTL %08x RDH %u RDT %u", who, why,
         rr(e, R_FEXTNVM6), rr(e, R_FEXTNVM11), rr(e, R_PCIEANACFG), rr(e, R_PBA), rr(e, R_TXDCTL1), rr(e, R_TARC1), rr(e, R_TDLEN1), rr(e, R_RXDCTL),
         rr(e, R_RCTL), rr(e, R_RDH), rr(e, R_RDT));
    klog("Yukari", "%s: %s: pcie cap %02x LNKCTL %04x LNKSTA %04x, pm cap %02x PMCSR %04x, tx[%u] cmd %02x status %02x len %u", who, why, pcie,
         pcie ? pci_read16(e->pci, (u16)(pcie + 0x10)) : 0, pcie ? pci_read16(e->pci, (u16)(pcie + 0x12)) : 0, pm, pm ? pci_read16(e->pci, (u16)(pm + 4)) : 0, h,
         td ? td->cmd : 0, td ? td->status : 0, td ? td->len : 0);
}

/* What Linux redoes for the Sunrise Point / Kaby Lake parts at probe and at every link-up: the K1 power state of the internal link must stay off at
 * gigabit speed when the part says so (PCIEANACFG bit 31), or the MAC can miss DMA completions, which looks exactly like a transmit queue that hangs;
 * and the "MULR fix" is switched off, as it is before every descriptor ring flush. */
static void pch_link_fixes(EDev *e) {
    if (!e->pch || e->no_errata) return;
    wr(e, R_FEXTNVM4, (rr(e, R_FEXTNVM4) & ~0x7u) | 0x7u);          /* "I217 packet loss": beacon duration 8 us, redone by Linux at every link-up */
    if (!e->spt) return;
    u32 f6 = rr(e, R_FEXTNVM6);
    f6 = (rr(e, R_PCIEANACFG) & FEXTNVM6_K1_OFF) ? (f6 | FEXTNVM6_K1_OFF) : (f6 & ~FEXTNVM6_K1_OFF);
    wr(e, R_FEXTNVM6, f6);
    wr(e, R_FEXTNVM11, rr(e, R_FEXTNVM11) | FEXTNVM11_NO_MULR);
}

/* The transmit unit off and idle: the state in which a driver may reprogram the ring. */
static void tx_quiesce(EDev *e) {
    wr(e, R_TCTL, TCTL_PSP);
    wr(e, R_TXDCTL, rr(e, R_TXDCTL) & ~QUEUE_ENABLE);
    flush(e);
    for (int i = 0; i < 100 && (rr(e, R_TXDCTL) & QUEUE_ENABLE); i++) udelay(100);
    udelay(2000);
}

static void tx_enable_wait(EDev *e) {
    for (int i = 0; i < 100 && !(rr(e, R_TXDCTL) & QUEUE_ENABLE); i++) udelay(100);
}

static bool wait_clear(EDev *e, u32 reg, u32 mask, int ms) {
    for (int i = 0; i < ms * 10; i++) {
        if (!(rr(e, reg) & mask)) return true;
        udelay(100);
    }
    return false;
}

/* The MAC (not the PHY: the ME and the firmware set that up) back to its power-on state, the way Linux's e1000_reset_hw_ich8lan does it for the MAC part. */
static void mac_reset(EDev *e) {
    wr(e, R_IMC, 0xFFFFFFFFu);
    wr(e, R_RCTL, 0);
    wr(e, R_TCTL, TCTL_PSP);
    flush(e);
    udelay(10000);
    bool owned = e->pch && swflag_acquire(e);
    wr(e, R_CTRL, rr(e, R_CTRL) | CTRL_RST);
    udelay(20000);
    wait_clear(e, R_CTRL, CTRL_RST, 100);
    if (owned) swflag_release(e);
    udelay(20000);                                                 /* the card reloads its address registers from the NVM */
    wr(e, R_IMC, 0xFFFFFFFFu);
    (void)rr(e, R_ICR);
    if (e->e1000e) wr(e, R_CTRLEXT, rr(e, R_CTRLEXT) | CTRLEXT_DRV_LOAD);
}

/* The MAC and the PHY reset together (CTRL.RST | PHY_RST under the PHY semaphore), as Linux does it when the ME does not forbid it; the link drops and comes back. */
static void mac_phy_reset(EDev *e) {
    wr(e, R_IMC, 0xFFFFFFFFu);
    wr(e, R_RCTL, 0);
    wr(e, R_TCTL, TCTL_PSP);
    flush(e);
    udelay(10000);
    bool owned = e->pch && swflag_acquire(e);
    wr(e, R_CTRL, rr(e, R_CTRL) | CTRL_RST | CTRL_PHYRST);
    udelay(20000);
    wait_clear(e, R_CTRL, CTRL_RST, 100);
    if (owned) swflag_release(e);
    for (int i = 0; i < 200 && !(rr(e, R_EEMNGCTL) & (1u << 18)); i++) udelay(1000);     /* CFGDONE_0: the PHY has loaded its configuration */
    udelay(20000);
    wr(e, R_IMC, 0xFFFFFFFFu);
    (void)rr(e, R_ICR);
    if (e->e1000e) wr(e, R_CTRLEXT, rr(e, R_CTRLEXT) | CTRLEXT_DRV_LOAD);
}

/* Linux's e1000_flush_desc_rings() for the Sunrise Point and Kaby Lake parts: a card that is reset with descriptors still in its rings can hang
 * afterwards. Program the transmit ring, push one dummy descriptor through it, give the receive unit a moment with sane thresholds. Linux does this only
 * when the bit in config space asks for it; the variant that uses it here does it anyway and logs what the bit said. */
static void flush_desc_rings(EDev *e, const char *who) {
    wr(e, R_FEXTNVM11, rr(e, R_FEXTNVM11) | FEXTNVM11_NO_MULR);
    u16 before = pci_read16(e->pci, PCI_DESC_RING_STATUS);
    memset(e->tx, 0, (size_t)NDESC * 16);
    tx_program(e);
    e->tx[0].addr = e->tx_phys;                                    /* what is sent does not matter: the ring itself will do */
    e->tx[0].len = 512;
    e->tx[0].cmd = 0x02;                                           /* IFCS, no end of packet: nothing leaves the card */
    barrier();
    wr(e, R_TDT, 1);
    udelay(250);
    u16 mid = pci_read16(e->pci, PCI_DESC_RING_STATUS);
    if (mid & FLUSH_DESC_REQUIRED) {
        u32 rctl = rr(e, R_RCTL);
        wr(e, R_RCTL, rctl & ~RCTL_EN);
        flush(e);
        udelay(150);
        u32 rx = rr(e, R_RXDCTL);
        rx &= 0xFFFFC000u;
        rx |= 0x1Fu | (1u << 8) | (1u << 24);                      /* prefetch threshold 31, host threshold 1, units of descriptors */
        wr(e, R_RXDCTL, rx);
        wr(e, R_RCTL, rctl | RCTL_EN);                             /* momentarily on for the new thresholds to take effect */
        flush(e);
        udelay(150);
        wr(e, R_RCTL, rctl & ~RCTL_EN);
    }
    klog("Yukari", "%s: descriptor ring flush: config-space status %04x before, %04x after the transmit ring, %04x at the end", who, before, mid,
         pci_read16(e->pci, PCI_DESC_RING_STATUS));
}

/* The function put into D3hot and woken again (pci_set_d0 puts the BARs back if the transition reset them): as near to a function reset as this part has. */
static void d3_cycle(EDev *e) {
    u8 pm = pci_find_cap(e->pci, PCI_CAP_PM);
    if (!pm) return;
    u16 pmcsr = pci_read16(e->pci, (u16)(pm + 4));
    pci_write16(e->pci, (u16)(pm + 4), (u16)((pmcsr & ~0x8003u) | 3));
    udelay(5000);
    pci_set_d0(e->pci);
    pci_enable(e->pci, PCI_CMD_MEM | PCI_CMD_MASTER);
}

/* Everything between "the card answers" and "frames can flow": link up, filters, the PCH extras, the receive ring, the transmit ring. Used at probe
 * and again after a MAC reset by transmit variant 2. */
static void hw_setup(EDev *e) {
    /* link up, auto speed detection; forced speed and duplex off */
    u32 ctrl = rr(e, R_CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    ctrl &= ~(CTRL_LRST | CTRL_PHYRST | CTRL_ILOS | CTRL_VME | CTRL_FRCSPD | CTRL_FRCDPX);
    wr(e, R_CTRL, ctrl);
    for (int i = 0; i < 128; i++) wr(e, R_MTA + 4 * (u32)i, 0);

    if (e->pch && !e->no_errata) {                               /* Linux's e1000_initialize_hw_bits_ich8lan */
        wr(e, R_CTRLEXT, rr(e, R_CTRLEXT) | (1u << 22));
        wr(e, R_TXDCTL, rr(e, R_TXDCTL) | (1u << 22));
        wr(e, R_TARC0, rr(e, R_TARC0) | (1u << 23) | (1u << 24) | (1u << 26) | (1u << 27));
        wr(e, R_RFCTL, 0x00C0u);                                 /* NFS filtering off, extended descriptors off */
        wr(e, R_MRQC, 0);                                        /* no receive-side scaling */
        wr(e, R_TXDCTL1, rr(e, R_TXDCTL1) | (1u << 22));         /* the second transmit queue is set up like the first (Linux: "program both queues") */
        u32 t1 = rr(e, R_TARC1);
        t1 = (rr(e, R_TCTL) & TCTL_MULR) ? (t1 & ~(1u << 28)) : (t1 | (1u << 28));
        wr(e, R_TARC1, t1 | (1u << 24) | (1u << 26) | (1u << 30));
    }
    if (e->spt && !e->no_errata) {                               /* SPT / KBL silicon errata: keep the transmit queue from hanging */
        wr(e, R_PBA, 26);                                        /* 26 KB of the packet buffer for receive, the rest transmit: what Linux programs for these parts (units are off here) */
        wr(e, R_IOSFPC, rr(e, R_IOSFPC) | 0x00010000u);
        u32 t = rr(e, R_TARC0);
        t &= ~(3u << 28);
        t |= (1u << 29);
        wr(e, R_TARC0, t);
        pch_link_fixes(e);
    }

    /* receive */
    for (int i = 0; i < NDESC; i++) { e->rx[i].status = 0; e->rx[i].errors = 0; }
    e->rx_next = 0;
    wr(e, R_RDBAL, (u32)e->rx_phys);
    wr(e, R_RDBAH, (u32)(e->rx_phys >> 32));
    wr(e, R_RDLEN, NDESC * 16);
    wr(e, R_RDH, 0);
    wr(e, R_RDT, 0);
    wr(e, R_RDTR, 0);
    wr(e, R_RADV, 0);
    if (e->e1000e) wr(e, R_RXDCTL, DESC_WB | QUEUE_ENABLE | 0x1Fu);
    wr(e, R_RXCSUM, 0);
    wr(e, R_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);                 /* 2048-byte buffers, legacy descriptors, broadcast accepted, CRC stripped */
    wr(e, R_RDT, NDESC - 1);
    tx_program(e);
}

static void tx_apply_variant(NetDev *d, EDev *e, int var) {
    klog("Yukari", "%s: transmit variant %d: %s", d->name, var, g_txvar[var]);
    e->tx_head = e->tx_tail = 0;
    e->tx_stuck_since = 0;
    e->no_errata = (var == 3);
    switch (var) {
    case 1:
        tx_quiesce(e);
        memset(e->tx, 0, (size_t)NDESC * 16);
        tx_program(e);
        tx_enable_wait(e);
        break;
    case 2:
        mac_reset(e);
        memset(e->tx, 0, (size_t)NDESC * 16);
        hw_setup(e);
        tx_enable_wait(e);
        break;
    case 3:
        tx_quiesce(e);
        wr(e, R_TARC0, rr(e, R_TARC0) & ~((1u << 23) | (1u << 24) | (1u << 26) | (1u << 27) | (1u << 29)));
        if (e->spt) wr(e, R_IOSFPC, rr(e, R_IOSFPC) & ~0x00010000u);
        memset(e->tx, 0, (size_t)NDESC * 16);
        tx_program(e);
        tx_enable_wait(e);
        break;
    case 4:
        flush_desc_rings(e, d->name);
        mac_reset(e);
        memset(e->tx, 0, (size_t)NDESC * 16);
        hw_setup(e);
        tx_enable_wait(e);
        break;
    case 5:
        mac_phy_reset(e);
        if (e->ulp) pch_leave_ulp(e, d->name);
        if (e->pch) pch_phy_check(e, d->name);
        memset(e->tx, 0, (size_t)NDESC * 16);
        hw_setup(e);
        tx_enable_wait(e);
        break;
    case 6:
        d3_cycle(e);
        mac_reset(e);
        memset(e->tx, 0, (size_t)NDESC * 16);
        hw_setup(e);
        tx_enable_wait(e);
        break;
    case 7:
        wr(e, R_RCTL, 0);
        tx_quiesce(e);
        memset(e->tx, 0, (size_t)NDESC * 16);
        tx_program(e);
        tx_enable_wait(e);
        udelay(5000);
        wr(e, R_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
        break;
    default:
        wr(e, R_TCTL, TCTL_PSP);
        flush(e);
        udelay(2000);
        memset(e->tx, 0, (size_t)NDESC * 16);
        tx_program(e);
        break;
    }
    e_dump(d->name, e, "after", false);
}

/* ---- probing ---- */

static void e_probe(PciDev *p, int kind, bool ulp, const char *name) {
    EDev *e = kzalloc(sizeof *e);
    if (!e) return;
    e->pci = p;
    e->pch = kind >= 2;
    e->e1000e = kind >= 1;
    e->spt = kind == 3;
    e->ulp = ulp;
    e->model = name;
    e->last_link = -1;
    pci_enable(p, PCI_CMD_MEM | PCI_CMD_MASTER);
    e->mmio = pci_map_bar(p, 0);
    if (!e->mmio) goto fail;
    u32 st = rr(e, R_STATUS);
    if (st == 0xFFFFFFFFu) goto fail;                              /* the registers do not answer */
    if (e->pch) e_dump(name, e, "as the firmware left it", false);

    wr(e, R_IMC, 0xFFFFFFFFu);
    wr(e, R_RCTL, 0);
    wr(e, R_TCTL, TCTL_PSP);
    flush(e);
    udelay(10000);
    if (e->pch) {
        wr(e, R_CTRLEXT, rr(e, R_CTRLEXT) | CTRLEXT_DRV_LOAD);     /* "a driver owns this card now" */
        if (e->ulp) pch_leave_ulp(e, name);
        pch_phy_check(e, name);
        (void)rr(e, R_ICR);
    } else {                                                       /* a global reset gives a known starting point on the QEMU models */
        wr(e, R_CTRL, rr(e, R_CTRL) | CTRL_RST);
        udelay(20000);
        if (!wait_clear(e, R_CTRL, CTRL_RST, 100)) goto fail;
        wr(e, R_IMC, 0xFFFFFFFFu);
        (void)rr(e, R_ICR);
        if (e->e1000e) wr(e, R_CTRLEXT, rr(e, R_CTRLEXT) | CTRLEXT_DRV_LOAD);
    }

    u32 ral = rr(e, R_RAL0), rah = rr(e, R_RAH0);
    u8 mac[6] = { (u8)ral, (u8)(ral >> 8), (u8)(ral >> 16), (u8)(ral >> 24), (u8)rah, (u8)(rah >> 8) };
    if ((mac[0] & 1) || !(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) || (mac[0] & mac[1] & mac[2] & mac[3] & mac[4] & mac[5]) == 0xFF) goto fail;

    void *ring = dma_alloc((size_t)NDESC * 16 * 2, &e->rx_phys);       /* receive ring, then transmit ring right behind it */
    if (!ring) goto fail;
    e->rx = ring;
    e->tx = (TxDesc *)((u8 *)ring + (size_t)NDESC * 16);
    e->tx_phys = e->rx_phys + (size_t)NDESC * 16;
    e->rxbuf = dma_alloc((size_t)NDESC * BUFSZ, &e->rxbuf_phys);
    e->txbuf = dma_alloc((size_t)NDESC * BUFSZ, &e->txbuf_phys);
    if (!e->rxbuf || !e->txbuf) goto fail;
    for (int i = 0; i < NDESC; i++) e->rx[i].addr = e->rxbuf_phys + (u64)i * BUFSZ;

    char opt[8];                                                   /* test and experiment hooks: e1000.txvar=N starts at transmit variant N, e1000.txstall=N pretends a hang until variant N */
    if (cmdline_value("e1000.txvar", opt, sizeof opt) && opt[0] >= '0' && opt[0] < '0' + NTXVAR) e->txvar = opt[0] - '0';
    if (cmdline_value("e1000.txstall", opt, sizeof opt) && opt[0] >= '0' && opt[0] <= '9') e->txstall = opt[0] - '0';
    e->no_errata = (e->txvar == 3);
    hw_setup(e);

    NetDev *d = kzalloc(sizeof *d);
    if (!d) goto fail;
    snprintf(d->driver, sizeof d->driver, "%s", e->pch ? "e1000e(pch)" : e->e1000e ? "e1000e" : "e1000");
    memcpy(d->mac, mac, 6);
    d->mtu = 1500;
    d->flags = NDF_BROADCAST;
    d->tx = e_tx;
    d->poll = e_poll;
    d->priv = e;
    if (rr(e, R_STATUS) & STATUS_LU) d->flags |= NDF_LINK;
    p->driver = "e1000";
    p->driver_data = d;
    if (e->txvar) tx_apply_variant(d, e, e->txvar);                /* an experiment asked to start at a later variant */
    netdev_register("eth", d);
    klog("Yukari", "%s: Intel %s at %02x:%02x:%02x.%x, MAC %02x:%02x:%02x:%02x:%02x:%02x%s", d->name, name, 0, p->bus, p->dev, p->fn,
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (rr(e, R_STATUS) & STATUS_LU) ? "" : " (no carrier yet: the link is polled)");
    return;
fail:
    klog("Yukari", "Intel %s at %02x:%02x.%x did not answer sensibly; leaving it alone", name, p->bus, p->dev, p->fn);
    if (e->mmio) { wr(e, R_RCTL, 0); wr(e, R_TCTL, 0); }
    if (e->rx) dma_free(e->rx, (size_t)NDESC * 32);
    if (e->rxbuf) dma_free(e->rxbuf, (size_t)NDESC * BUFSZ);
    if (e->txbuf) dma_free(e->txbuf, (size_t)NDESC * BUFSZ);
    kfree(e);
}

static void e1000_init(void) {
    if (net_cards_disabled()) return;
    for (PciDev *p = pci_find_id(INTEL, 0xFFFF, NULL); p; p = pci_find_id(INTEL, 0xFFFF, p)) {
        if (p->driver || p->class_code != 2) continue;
        for (unsigned i = 0; i < ARRAY_SIZE(g_ids); i++)
            if (g_ids[i].id == p->device) {
                int kind = g_ids[i].kind;
                bool ulp = g_ids[i].ulp;
                if (kind == 1 && cmdline_flag("e1000.pch")) { kind = 3; ulp = true; }     /* test hook: run the PCH code paths on QEMU's 82574L */
                e_probe(p, kind, ulp, g_ids[i].name);
                break;
            }
    }
}
INITCALL(e1000_init);
