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
#define R_FEXTNVM3 0x003C
#define R_KMRNCTRLSTA 0x0034
#define R_MANC    0x5820
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
#define R_IMS     0x00D0
#define R_LTRV    0x00F8
#define R_RDFH    0x2410                /* the receive data FIFO: head, tail, saved head and tail, packet count (frames the MAC has taken in and the DMA has not yet moved) */
#define R_RDFT    0x2418
#define R_RDFPC   0x2430
#define R_PSRCTL  0x2170
#define R_MANC2H  0x5860
#define R_MDEF0   0x5890
#define R_GCR     0x5B00                /* PCIe control: bits 0-5 let the card write/read descriptors and data of the receive and transmit rings without snooping the CPU's caches */
#define GCR_NO_SNOOP 0x3Fu

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
#define RCTL_UPE   (1u << 3)
#define RCTL_MPE   (1u << 4)
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
#define STATUS_PHYRA (1u << 10)         /* "PHY reset asserted": set by a reset, Linux clears it by hand */
#define EXTCNF_GATE_PHY_CFG 0x00000080u
#define FWSM_PCIM2PCI 0x01000000u       /* the ME is using the PCIm-to-PCI arbiter: a host register write now may be lost */
#define CTRL_LANPHYPC_OVERRIDE (1u << 16)
#define CTRL_LANPHYPC_VALUE    (1u << 17)
#define CTRLEXT_LPCD (1u << 14)

#define RXERR_FATAL 0x97u               /* receive descriptor errors that mean a damaged frame: CE SE SEQ CXE RXE. TCPE (0x20) and IPE (0x40) are checksum verdicts only, which the stack makes for itself */

#define NDESC   256
#define BUFSZ   2048
#define NTXVAR  12                      /* ways of starting the transmit unit, tried one after the other when it hangs (g_txvar) */
#define TX_GIVE_UP (2 * NTXVAR)         /* restarts of a card that never transmits before the driver stops trying (two rounds of the ladder) */

/* Ways of starting the receive unit (g_rxvar), tried one after the other when the card counts good frames and none of them ever reaches memory (the Dell's I219
 * did exactly that in the third hardware round: GPRC grew, RDH stayed at 0) or whose frames arrive empty (the fourth round: descriptors completed with the right length and
 * flags, the buffers held zeros, because the card was still fetching descriptors from the ring the firmware's driver had left in it). Step 0 is how the unit is started
 * at probe, after a MAC reset; a step with a transmit variant resets the card with it. */
#define RXF_CYCLE   1u                  /* the queue is switched off (and the bit polled) before the ring registers are written, and on again after them: igb, ixgbe and i40e do so */
#define RXF_FWREGS  2u                  /* RXDCTL, RDTR, RADV, RFCTL and RXCSUM as the firmware's driver had them (it received frames with those) */
#define RXF_ENFIRST 4u                  /* the receiver switched on before the ring is programmed, the queue after it */
#define RXF_PROMISC 8u                  /* unicast and multicast promiscuous: if frames arrive now, the address filter is what the card disagrees with */
#define RXF_QLAST   16u                 /* the queue switched on only after the receiver and the tail pointer */
#define RXF_MNG2H   32u                 /* management traffic is passed to the host as well (MANC2H): if the ME were keeping the frames, they would come now */
#define NRXVAR      12
#define RX_FIRST_MS 7000                /* no frame in memory this long after the link came up: the first step; later steps follow every RX_STEP_MS */
#define RX_STEP_MS  5000
#define CNT_GPRC    18                  /* index of "gprc" in g_cnt */

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
    bool arb_guard;                     /* the ME is alive on a part that shares the PCIm-to-PCI arbiter with it: every register write waits for FWSM to say it is free */
    bool defer_tx;                      /* variant 9: queue and transmitter are enabled only some time after the reset, once the link is back */
    bool txdctl_simple;                 /* variant 11: TXDCTL without write-back granularity or thresholds */
    u64 tx_enable_at;                   /* when a deferred transmitter is switched on (ms), 0 = nothing deferred */
    u64 link_since;                     /* when the link last came up (ms), 0 while it is down */
    u64 last_probe;                     /* when the last probe frame was queued (ms) */
    u32 arb_waits;                      /* register writes that had to wait for the ME */
    u32 tail_fixes;                     /* tail register writes that did not read back as written */
    u32 fw_tctl, fw_txdctl, fw_txdctl1, fw_tarc0, fw_tarc1, fw_tipg;     /* the transmit registers as the firmware left them (variant 8 puts them back) */
    const char *model;                  /* "I219-LM5" */
    bool debug;                         /* "nicdebug" on the command line: a trace of the first frames in both directions, more counter lines */
    u64 cnt[40];                        /* the card's statistics registers (they clear when read): running totals, see g_cnt */
    u64 cnt_next;                       /* when the next counter line is due (ms), 0 = none */
    u32 cnt_step;                       /* how many counter lines have been written since the link came up */
    u32 rx_traced, tx_traced;           /* frames traced so far */
    u32 rx_bits[8];                     /* receive descriptors per error bit: CE SE SEQ - CXE TCPE IPE RXE */
    u32 rx_fatal, rx_noeop, rx_nobuf;   /* frames thrown away: a damaging error bit, no end-of-packet, no packet buffer or too long for one */
    u32 tx_done;                        /* transmit descriptors the card has finished with (its head pointer moved past them, or the done bit came back) */
    int rxvar;                          /* the step of the receive ladder in use (g_rxvar); 0 = the start at probe */
    u32 rx_flags;                       /* the RXF_ flags hw_setup() starts the receive unit with */
    int rxgarbage;                      /* test hook "e1000.rxgarbage=N": every received frame counts as blank until step N (a pretend card that completes descriptors and fills no buffer) */
    u32 rx_zero;                        /* frames that arrived with a blank Ethernet header (a descriptor completed, the buffer holds zeros) */
    u32 rx_good_total;                  /* frames that arrived with contents */
    u32 rx_good_at;                     /* rx_good_total when the present step began */
    bool cyc_logged;                    /* what the queue-enable bit did when the queue was switched off has been logged once */
    u64 probe_at;                       /* when the probe finished (ms) */
    bool nolink_tried;                  /* the PHY power cycle for a link that never came back after the probe-time reset has been tried */
    bool reset_at_probe;                /* the MAC was reset before the rings were programmed */
    int rxstall;                        /* test hook "e1000.rxstall=N": the receive ring gets no descriptors until step N (a pretend dead receiver) */
    u32 rx_dd_total;                    /* receive descriptors the card has completed since the driver started, good or damaged */
    u64 rx_step_at;                     /* when it began (ms) */
    u64 rx_hw_at;                       /* the card's count of good frames when it began */
    u64 rx_check_at;                    /* when the receive watchdog last looked */
    u32 rx_resets;                      /* steps of the ladder taken */
    bool rx_ok;                         /* a frame has reached memory since the present step began */
    bool rx_gave_up;                    /* the ladder is exhausted (or pointless: nothing arrives at all) */
    bool rx_kick;                       /* the first frame after a restart: start DHCP again at once */
    u64 rx_poke_at;                     /* when DHCP is started again to provoke an answer after a step (ms), 0 = nothing due: a quiet network would not send a frame by itself */
    u32 icr_seen;                       /* interrupt causes seen (the ICR is read for the log; nothing is enabled) */
    u32 fw_rxdctl, fw_rdtr, fw_radv, fw_rfctl, fw_rxcsum;       /* the receive registers as the firmware left them */
    u64 fw_ring_phys;                   /* the firmware driver's receive ring (RDBAL/RDBAH as it left them), a copy of it taken at probe, and where it is mapped: */
    u32 fw_ring_len;                    /* if the card still writes there after this driver's start, its ring registers did not take effect */
    const u8 *fw_ring_virt;
    u8 *fw_ring_copy;
    bool gone;                          /* e_shutdown() put the card to sleep (D3hot, armed to wake the machine): nothing touches its registers any more */
    bool wol_armed;                     /* ... and it was armed to wake the machine */
    bool asleep;                        /* ... and the last step has been done: the wake cause is cleared and the card is in D3hot */
} EDev;

static void e_dump(const char *who, EDev *e, const char *why, bool full);
static void tx_apply_variant(NetDev *d, EDev *e, int var);
static void pch_link_fixes(EDev *e);

static inline u32 rr(EDev *e, u32 reg) { return *(volatile u32 *)(e->mmio + reg); }

/* Linux's __ew32_prepare(): on the Sunrise Point parts the ME uses the same arbiter (PCIm-to-PCI) as the host's register writes, and a write that collides
 * with it can be lost. FWSM says when the arbiter is busy; a write waits for that, at most 100 ms. */
static inline void arb_wait(EDev *e) {
    if (!e->arb_guard) return;
    u32 i = 2000;
    while ((*(volatile u32 *)(e->mmio + R_FWSM) & FWSM_PCIM2PCI) && --i) udelay(50);
    if (i < 2000) e->arb_waits++;
}

static inline void wr(EDev *e, u32 reg, u32 v) { arb_wait(e); *(volatile u32 *)(e->mmio + reg) = v; }
static inline void flush(EDev *e) { (void)rr(e, R_STATUS); }

/* The cache lines of [p, p+n) written back and dropped. A card that writes descriptors or data without snooping the CPU's caches (the GCR register can ask for
 * that, and the firmware's driver may have) is not seen by a CPU whose cache still holds the line, and a dirty line can be written back over what the card wrote:
 * whether that happens depends on which cache sets the ring lands in, which is the kind of thing that works in one boot and not in the next. */
static inline void cache_drop(const void *p, size_t n) {
    u64 a = (u64)p & ~63UL, end = (u64)p + n;
    __asm__ volatile ("mfence" ::: "memory");
    for (; a < end; a += 64) __asm__ volatile ("clflush (%0)" :: "r"(a) : "memory");
    __asm__ volatile ("mfence" ::: "memory");
}

/* A tail register write, checked the way Linux checks it on these parts ("ME firmware caused invalid TDT/RDT"): it must read back as written. */
static void tail_write(EDev *e, u32 reg, u32 v, const char *what) {
    wr(e, reg, v);
    if (e->arb_guard && rr(e, reg) != v) {
        e->tail_fixes++;
        if (e->tail_fixes <= 6) klog("Yukari", "%s: %s did not hold: wrote %u, reads %u (the ME interfered); writing it again", e->model, what, v, rr(e, reg));
        wr(e, reg, v);
    }
}

/* Transmit control with its other bits left alone: the reset default and the firmware set MULR (multiple outstanding requests) and more, and the transmit
 * DMA of this part may depend on them. Linux changes only the collision threshold and the pad bit. */
static void tctl_set(EDev *e, bool on) {
    u32 t = rr(e, R_TCTL);
    t &= ~(TCTL_EN | (0xFFu << 4) | (0x3FFu << 12));
    t |= TCTL_PSP | (0x0Fu << 4) | (0x40u << 12) | (on ? TCTL_EN : 0);
    wr(e, R_TCTL, t);
}

static void tctl_off(EDev *e) { wr(e, R_TCTL, rr(e, R_TCTL) & ~TCTL_EN); }
static void rctl_off(EDev *e) { wr(e, R_RCTL, rr(e, R_RCTL) & ~RCTL_EN); }

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

/* Memory the card can certainly reach: below 4 GiB if the allocator has any (it hands out whatever block it has; the misses are kept until the end so that
 * the next try gets another one). If every try lands high the high block is used after all, as before. */
static void *dma_alloc_below_4g(size_t size, paddr_t *phys) {
    void *miss[8];
    int nmiss = 0;
    void *v = NULL;
    for (;;) {
        paddr_t p;
        v = dma_alloc(size, &p);
        if (!v) break;
        if (p + ALIGN_UP(size, (size_t)PAGE_SIZE) <= 0x100000000ull || nmiss == 8) { *phys = p; break; }
        miss[nmiss++] = v;
    }
    while (nmiss) dma_free(miss[--nmiss], size);
    return v;
}

/* ---- diagnostics: what the card says it did, and a look at the first frames (docs/NETWORK.md, "Round 3") ---- */

/* The statistics registers clear when they are read; the totals are kept here. Which of them count what the log should say about a card that will not talk:
 * gptc/tpt are frames that really left the card, gprc/bprc/mprc what arrived and was good, crc/algn/sym/rxerr/missed/rnbc what arrived damaged or found no room,
 * xon/xoff the flow-control (pause) frames in both directions (a pause frame from the switch stops the transmitter, one from us stops the switch). */
static const struct { const char *name; u16 reg; } g_cnt[] = {
    { "crc", 0x4000 }, { "algn", 0x4004 }, { "sym", 0x4008 }, { "rxerr", 0x400C }, { "missed", 0x4010 }, { "scoll", 0x4014 }, { "mcoll", 0x401C },
    { "latecol", 0x4020 }, { "coll", 0x4028 }, { "defer", 0x4030 }, { "nocrs", 0x4034 }, { "seqerr", 0x4038 }, { "cext", 0x403C }, { "lenerr", 0x4040 },
    { "xon_rx", 0x4048 }, { "xon_tx", 0x404C }, { "xoff_rx", 0x4050 }, { "xoff_tx", 0x4054 }, { "gprc", 0x4074 }, { "bprc", 0x4078 }, { "mprc", 0x407C },
    { "gptc", 0x4080 }, { "rnbc", 0x40A0 }, { "runt", 0x40A4 }, { "frag", 0x40A8 }, { "oversize", 0x40AC }, { "jabber", 0x40B0 }, { "tpr", 0x40D0 },
    { "tpt", 0x40D4 }, { "mptc", 0x40F0 }, { "bptc", 0x40F4 },
};
#define NCNT ((int)(sizeof g_cnt / sizeof g_cnt[0]))
static const u32 g_cnt_gap_ms[] = { 3000, 5000, 12000, 25000, 45000 };         /* after the link came up: 3 s, 8 s, 20 s, 45 s, 90 s */

/* One line about a frame: addresses, type, and for ARP and IPv4 (UDP, DHCP) the part that says what it is. */
static void frame_brief(const u8 *f, u32 len, char *out, size_t cap) {
    if (len < 14) { snprintf(out, cap, "runt, %u bytes", len); return; }
    u32 et = (u32)f[12] << 8 | f[13];
    int o = snprintf(out, cap, "%02x%02x%02x%02x%02x%02x < %02x%02x%02x%02x%02x%02x type %04x", f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9],
                     f[10], f[11], et);
    if (o < 0 || (size_t)o >= cap) return;
    char *p = out + o;
    size_t left = cap - (size_t)o;
    if (et == 0x0806 && len >= 42) {
        snprintf(p, left, " ARP op %u %u.%u.%u.%u > %u.%u.%u.%u", f[21], f[28], f[29], f[30], f[31], f[38], f[39], f[40], f[41]);
    } else if (et == 0x0800 && len >= 34) {
        u32 ihl = (u32)(f[14] & 15) * 4;
        int n = snprintf(p, left, " IPv4 proto %u %u.%u.%u.%u > %u.%u.%u.%u", f[23], f[26], f[27], f[28], f[29], f[30], f[31], f[32], f[33]);
        if (n < 0 || (size_t)n >= left) return;
        p += n;
        left -= (size_t)n;
        if (f[23] == 17 && ihl >= 20 && len >= 14 + ihl + 8) {
            u32 u = 14 + ihl, sp = (u32)f[u] << 8 | f[u + 1], dp = (u32)f[u + 2] << 8 | f[u + 3];
            n = snprintf(p, left, " udp %u>%u csum %02x%02x", sp, dp, f[u + 6], f[u + 7]);
            if (n < 0 || (size_t)n >= left) return;
            p += n;
            left -= (size_t)n;
            if ((sp == 67 || sp == 68) && len >= u + 8 + 241) {
                u32 b = u + 8, type = 0;
                for (u32 i = b + 240; i + 2 < len && f[i] != 255;) {
                    if (f[i] == 0) { i++; continue; }
                    if (f[i] == 53 && f[i + 1] == 1) type = f[i + 2];
                    i += 2u + f[i + 1];
                }
                snprintf(p, left, " DHCP op %u type %u xid %02x%02x%02x%02x yiaddr %u.%u.%u.%u", f[b], type, f[b + 4], f[b + 5], f[b + 6], f[b + 7], f[b + 16], f[b + 17],
                         f[b + 18], f[b + 19]);
            }
        }
    }
}

/* The registers worth laying side by side with the firmware's driver's (boot/netdiag.c keeps the same table, in the same order, and prints it while the firmware's
 * driver is receiving frames): where the two logs differ is where to look. Nothing in the table clears when read. */
static const struct { u16 off; u8 n; } g_regmap[] = {
    { 0x0000, 8 }, { 0x0024, 7 }, { 0x00C4, 1 }, { 0x00D0, 1 }, { 0x00E0, 1 }, { 0x00F8, 1 }, { 0x0100, 1 }, { 0x0400, 2 }, { 0x0410, 1 }, { 0x0F00, 4 }, { 0x0F18, 1 },
    { 0x0F28, 1 }, { 0x1000, 3 }, { 0x100C, 1 }, { 0x1010, 1 }, { 0x2160, 4 }, { 0x2170, 1 }, { 0x2410, 13 }, { 0x2800, 12 }, { 0x3410, 13 }, { 0x3800, 12 }, { 0x3840, 1 },
    { 0x3908, 1 }, { 0x3928, 1 }, { 0x3940, 1 }, { 0x5000, 3 }, { 0x5400, 8 }, { 0x5818, 1 }, { 0x5820, 1 }, { 0x5860, 1 }, { 0x5890, 8 }, { 0x5B00, 6 }, { 0x5B50, 3 },
    { 0x5BB8, 2 },
};

static void regmap(const char *who, EDev *e, const char *why) {
    for (unsigned i = 0; i < ARRAY_SIZE(g_regmap); i++) {
        char line[200];
        size_t o = (size_t)snprintf(line, sizeof line, "%s: %s: regs %04x:", who, why, g_regmap[i].off);
        for (unsigned k = 0; k < g_regmap[i].n && o + 12 < sizeof line; k++) o += (size_t)snprintf(line + o, sizeof line - o, " %08x", rr(e, g_regmap[i].off + 4u * k));
        klog("Yukari", "%s", line);
    }
}

/* The receive side as the card and the driver see it, and a look into the ring and the first buffers: a frame that was DMA'd without the descriptor being
 * written back (or into another place) shows up as data in a buffer whose descriptor says nothing. */
static void rx_state(const char *who, EDev *e, const char *why) {
    e->icr_seen |= rr(e, R_ICR);
    klog("Yukari", "%s: %s: RXDCTL %08x RCTL %08x RDBAL %08x RDLEN %u RDH %u RDT %u RDTR %08x RADV %08x RFCTL %08x RXCSUM %08x MRQC %08x PSRCTL %08x (software: next %u, step %d)",
         who, why, rr(e, R_RXDCTL), rr(e, R_RCTL), rr(e, R_RDBAL), rr(e, R_RDLEN), rr(e, R_RDH), rr(e, R_RDT), rr(e, R_RDTR), rr(e, R_RADV), rr(e, R_RFCTL), rr(e, R_RXCSUM),
         rr(e, R_MRQC), rr(e, R_PSRCTL), e->rx_next, e->rxvar);
    klog("Yukari", "%s: %s: receive FIFO head %08x tail %08x packets %08x; MANC %08x MANC2H %08x MDEF0-3 %08x %08x %08x %08x; FWSM %08x LTRV %08x IMS %08x; causes seen %08x", who, why,
         rr(e, R_RDFH), rr(e, R_RDFT), rr(e, R_RDFPC), rr(e, R_MANC), rr(e, R_MANC2H), rr(e, R_MDEF0), rr(e, R_MDEF0 + 4), rr(e, R_MDEF0 + 8), rr(e, R_MDEF0 + 12),
         rr(e, R_FWSM), rr(e, R_LTRV), rr(e, R_IMS), e->icr_seen);
    unsigned dd = 0, first = 0, touched = 0;
    cache_drop(e->rx, (size_t)NDESC * 16);                          /* (the look must not be fooled by the cache it is checking for) */
    for (unsigned i = 0; i < NDESC; i++) cache_drop(e->rxbuf + (size_t)i * BUFSZ, 64);
    for (unsigned i = 0; i < NDESC; i++)
        if (e->rx[i].status & 1) { if (!dd) first = i; dd++; }
    for (unsigned i = 0; i < NDESC; i++) {
        const u32 *w = (const u32 *)(e->rxbuf + (size_t)i * BUFSZ);
        for (unsigned k = 0; k < 16; k++)
            if (w[k]) { touched++; break; }
    }
    const u32 *d0 = (const u32 *)&e->rx[0];
    klog("Yukari", "%s: %s: ring: %u of %d descriptors say done (first %u); %u of the %d buffers hold data in their first 64 bytes; descriptor 0 = %08x %08x %08x %08x; %lu good frames counted by the card, %u completed, %u with contents, %u blank",
         who, why, dd, NDESC, first, touched, NDESC, d0[1], d0[0], d0[3], d0[2], (unsigned long)e->cnt[CNT_GPRC], e->rx_dd_total, e->rx_good_total, e->rx_zero);
    if (e->fw_ring_virt) {                                          /* is the card still writing into the ring the firmware's driver left behind? */
        const u32 *was = (const u32 *)e->fw_ring_copy, *now = (const u32 *)e->fw_ring_virt;
        unsigned n = e->fw_ring_len / 16, changed = 0, at = 0;
        cache_drop(e->fw_ring_virt, e->fw_ring_len);
        for (unsigned i = 0; i < n; i++)
            if (memcmp(now + 4 * i, was + 4 * i, 16)) { if (!changed) at = i; changed++; }
        klog("Yukari", "%s: %s: the firmware's old receive ring (%#lx, %u descriptors): %u changed since this driver started%s", who, why, (unsigned long)e->fw_ring_phys, n, changed,
             changed ? "" : " (none: nothing writes there)");
        if (changed) klog("Yukari", "%s: %s: first changed, descriptor %u: now %08x %08x %08x %08x, at start %08x %08x %08x %08x", who, why, at, now[4 * at + 1], now[4 * at], now[4 * at + 3],
                          now[4 * at + 2], was[4 * at + 1], was[4 * at], was[4 * at + 3], was[4 * at + 2]);
    }
}

static bool frame_blank(const u8 *f) {
    for (int i = 0; i < 14; i++)
        if (f[i]) return false;
    return true;
}

static void cnt_log(NetDev *d, EDev *e) {
    for (int i = 0; i < NCNT; i++) e->cnt[i] += rr(e, g_cnt[i].reg);
    char line[220];
    size_t o = (size_t)snprintf(line, sizeof line, "%s: counters since link up:", d->name);
    bool any = false;
    for (int i = 0; i < NCNT; i++) {
        if (!e->cnt[i]) continue;
        any = true;
        if (o + 30 > sizeof line) { klog("Yukari", "%s", line); o = (size_t)snprintf(line, sizeof line, "%s: counters (more):", d->name); }
        o += (size_t)snprintf(line + o, sizeof line - o, " %s %lu", g_cnt[i].name, (unsigned long)e->cnt[i]);
    }
    if (!any) o += (size_t)snprintf(line + o, sizeof line - o, " none: the card has counted no frame in either direction");
    klog("Yukari", "%s", line);
    klog("Yukari", "%s: nic: STATUS %08x CTRL %08x RCTL %08x TCTL %08x RXCSUM %08x RAL0 %08x RAH0 %08x TDH %u TDT %u (software %u/%u) RDH %u RDT %u (next %u)", d->name,
         rr(e, R_STATUS), rr(e, R_CTRL), rr(e, R_RCTL), rr(e, R_TCTL), rr(e, R_RXCSUM), rr(e, R_RAL0), rr(e, R_RAH0), rr(e, R_TDH), rr(e, R_TDT), e->tx_head, e->tx_tail,
         rr(e, R_RDH), rr(e, R_RDT), e->rx_next);
    klog("Yukari", "%s: driver: %u transmit descriptors finished by the card; sent %lu (%lu errors), received %lu (%lu errors, %lu dropped); descriptor error bits ce %u se %u seq %u cxe %u tcpe %u ipe %u rxe %u; thrown away: "
         "%u damaged, %u without end of packet, %u without a buffer, %u blank", d->name, e->tx_done, (unsigned long)d->tx_packets, (unsigned long)d->tx_errors, (unsigned long)d->rx_packets,
         (unsigned long)d->rx_errors, (unsigned long)d->rx_dropped, e->rx_bits[0], e->rx_bits[1], e->rx_bits[2], e->rx_bits[4], e->rx_bits[5], e->rx_bits[6], e->rx_bits[7],
         e->rx_fatal, e->rx_noeop, e->rx_nobuf, e->rx_zero);
    if (e->pch || e->debug) rx_state(d->name, e, "receive side");
    if (e->pch && e->cnt_step == 0) regmap(d->name, e, "registers 3 s after link up");
}

/* ---- transmit and receive rings ---- */

static void tx_ring_regs(EDev *e) {
    wr(e, R_TDBAL, (u32)e->tx_phys);
    wr(e, R_TDBAH, (u32)(e->tx_phys >> 32));
    wr(e, R_TDLEN, NDESC * 16);
    wr(e, R_TDH, 0);
    wr(e, R_TDT, 0);
    wr(e, R_TIDV, 0);
    wr(e, R_TADV, 0);
}

static void tx_program(EDev *e) {
    tx_ring_regs(e);
    if (!e->pch) wr(e, R_TIPG, 0x0060200Au);                    /* the PCH parts keep the interpacket gap the reset (or the firmware) gave them, as with Linux */
    if (e->e1000e) {
        u32 t = e->txdctl_simple ? 0x1Fu : (DESC_WB | (e->no_errata ? 0 : (1u << 22)) | 0x1Fu);      /* bit 22: count descriptors still to be processed */
        if (!e->defer_tx) t |= QUEUE_ENABLE;
        wr(e, R_TXDCTL, t);
    }
    if (e->pch && !e->no_errata) wr(e, R_TXDCTL1, rr(e, R_TXDCTL) & ~QUEUE_ENABLE);                     /* Linux: "set txdctl the same for both queues" (queue 1 stays unused) */
    if (!e->defer_tx) tctl_set(e, true);
    flush(e);
}

/* The steps of the receive ladder. flags: how the receive unit is started (RXF_); txvar: the transmit ladder's variant that resets the card first (0 = none, the step only
 * starts the unit again). The order: the probe's own start, the MAC resets (which clear the DMA engines, the cure for a card still working on the firmware's ring), the
 * cheap variations, and last the steps that touch the PHY. */
static const struct { u32 flags; u8 txvar; const char *name; } g_rxvar[NRXVAR] = {
    { RXF_CYCLE | RXF_FWREGS, 0, "queue cycled around the ring registers, the other receive registers as the firmware's driver had them" },
    { RXF_CYCLE, 2, "a MAC reset (the PHY is left alone), then everything programmed again with this driver's own thresholds" },
    { RXF_CYCLE | RXF_FWREGS, 4, "the descriptor rings flushed the way Linux does it for this part, then a MAC reset" },
    { RXF_CYCLE, 0, "queue cycled around the ring registers, this driver's own thresholds (write back each descriptor at once)" },
    { RXF_CYCLE | RXF_FWREGS | RXF_ENFIRST, 0, "receiver switched on before the ring is programmed, the queue after it" },
    { RXF_CYCLE | RXF_FWREGS | RXF_QLAST, 0, "queue switched on last, after the receiver and the tail pointer" },
    { 0, 0, "ring programmed with the queue left enabled and the receiver off (how the earlier images did it)" },
    { RXF_CYCLE | RXF_FWREGS | RXF_PROMISC, 0, "promiscuous receive (every unicast and multicast frame), queue cycled" },
    { RXF_CYCLE | RXF_FWREGS | RXF_MNG2H, 0, "management traffic passed to the host as well (MANC2H), in case the ME is keeping the frames" },
    { RXF_CYCLE | RXF_FWREGS, 6, "the card put to sleep (D3hot) and woken, then a MAC reset" },
    { RXF_CYCLE, 5, "a MAC and PHY reset, then everything programmed again" },
    { RXF_CYCLE | RXF_FWREGS, 10, "the PHY's power cycled (LANPHYPC toggle), then a MAC reset" },
};

/* The receive unit from "off" to "running", the way the RXF_ flags say. The rings and buffers exist already; everything the card needs is written here. */
static void rx_start(EDev *e, u32 fl) {
    bool fw = (fl & RXF_FWREGS) && e->pch && (e->fw_rxdctl & QUEUE_ENABLE);      /* only where the firmware's driver had the queue on: otherwise there is nothing to copy */
    u32 rctl = rr(e, R_RCTL);
    if (rctl & RCTL_EN) {                                           /* the receiver off, and a moment for a frame in flight (Linux waits 10 ms) */
        wr(e, R_RCTL, rctl & ~RCTL_EN);
        flush(e);
        udelay(10000);
    }
    if (e->e1000e && (fl & RXF_CYCLE)) {                            /* the queue off, and wait until the card says it is */
        wr(e, R_RXDCTL, rr(e, R_RXDCTL) & ~QUEUE_ENABLE);
        flush(e);
        int i;
        for (i = 0; i < 100 && (rr(e, R_RXDCTL) & QUEUE_ENABLE); i++) udelay(100);
        if (!e->cyc_logged && e->pch) {
            e->cyc_logged = true;
            klog("Yukari", "%s: the receive queue's enable bit %s when it was switched off (RXDCTL %08x after %d us)", e->model, i < 100 ? "cleared" : "did NOT clear", rr(e, R_RXDCTL), i * 100);
        }
    }
    for (int i = 0; i < NDESC; i++) { e->rx[i].status = 0; e->rx[i].errors = 0; e->rx[i].len = 0; }
    cache_drop(e->rx, (size_t)NDESC * 16);                          /* nothing of ours may be left dirty in a cache while the card writes to the ring */
    e->rx_next = 0;
    u32 on = RCTL_EN | RCTL_BAM | RCTL_SECRC | ((fl & RXF_PROMISC) ? (RCTL_UPE | RCTL_MPE) : 0);   /* 2048-byte buffers, legacy descriptors, CRC stripped */
    if (fl & RXF_ENFIRST) wr(e, R_RCTL, on);
    wr(e, R_RDBAL, (u32)e->rx_phys);
    wr(e, R_RDBAH, (u32)(e->rx_phys >> 32));
    wr(e, R_RDLEN, NDESC * 16);
    wr(e, R_RDH, 0);
    wr(e, R_RDT, 0);
    wr(e, R_RDTR, fw ? e->fw_rdtr : 0);
    wr(e, R_RADV, fw ? e->fw_radv : 0);
    if (fw) wr(e, R_RFCTL, e->fw_rfctl & ~(1u << 15));              /* (legacy descriptors: no extended status) */
    wr(e, R_RXCSUM, fw ? e->fw_rxcsum : 0);
    if (fl & RXF_MNG2H) wr(e, R_MANC2H, rr(e, R_MANC2H) | 0xFFu);
    u32 rxdctl = (fw ? e->fw_rxdctl : (DESC_WB | 0x1Fu)) | QUEUE_ENABLE;
    if (e->e1000e && !(fl & RXF_QLAST)) {
        wr(e, R_RXDCTL, rxdctl);
        if (fl & RXF_CYCLE)
            for (int i = 0; i < 100 && !(rr(e, R_RXDCTL) & QUEUE_ENABLE); i++) udelay(100);
    } else if (e->e1000e) {
        wr(e, R_RXDCTL, rxdctl & ~QUEUE_ENABLE);                     /* (the thresholds go in now, the enable bit later) */
    }
    if (!(fl & RXF_ENFIRST)) wr(e, R_RCTL, on);
    flush(e);
    if (e->rxstall <= e->rxvar) tail_write(e, R_RDT, NDESC - 1, "RDT");           /* (the test hook leaves a stalled receiver without descriptors) */
    if (e->e1000e && (fl & RXF_QLAST)) {
        wr(e, R_RXDCTL, rxdctl);
        for (int i = 0; i < 100 && !(rr(e, R_RXDCTL) & QUEUE_ENABLE); i++) udelay(100);
    }
}

/* The descriptors the card has finished with become free again (no log line: the panic path's transmit uses this too). */
static void tx_reclaim_quiet(EDev *e) {
    while (e->tx_head != e->tx_tail && (e->tx[e->tx_head].status & 1)) {
        e->tx[e->tx_head].status = 0;
        e->tx_head = (u16)((e->tx_head + 1) % NDESC);
        e->tx_done++;
    }
    /* A card that fetched the descriptors but never wrote the done bit back (the write-back setting differs between parts) still moved its head
     * pointer: trust that too, as long as it lies between our head and tail. */
    u32 hw = rr(e, R_TDH);
    u16 pending = (u16)((e->tx_tail + NDESC - e->tx_head) % NDESC);
    u16 taken = (u16)((hw + NDESC - e->tx_head) % NDESC);
    if (hw < NDESC && pending && taken <= pending && taken) {
        for (u16 k = 0; k < taken; k++) { e->tx[e->tx_head].status = 0; e->tx_head = (u16)((e->tx_head + 1) % NDESC); e->tx_done++; }
    }
}

static void tx_reclaim(EDev *e) {
    u16 before = e->tx_head;
    tx_reclaim_quiet(e);
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
    if (e->debug && e->tx_traced < 60) {
        char brief[160];
        frame_brief(data, len, brief, sizeof brief);
        klog("Yukari", "%s: tx #%u (%u bytes, ring %u/%u, TDH %u): %s", e->model, ++e->tx_traced, len, e->tx_head, e->tx_tail, rr(e, R_TDH), brief);
    }
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
    if (e->txstall <= e->txvar) tail_write(e, R_TDT, next, "TDT");  /* (test hook: a pretend hang swallows the doorbell until the ladder reaches variant txstall) */
    return 0;
}

static int e_tx(NetDev *d, NetBuf *nb) {
    EDev *e = d->priv;
    if (e->gone) { d->tx_dropped++; nb_free(nb); return -ENETDOWN; }
    tx_reclaim(e);
    if (tx_enqueue(e, nb_data(nb), nb->len)) { d->tx_dropped++; nb_free(nb); return -ENOBUFS; }
    d->tx_packets++;
    d->tx_bytes += nb->len;
    nb_free(nb);
    return 0;
}

/* The panic path's transmit (NetDev.netpoll_tx; the netconsole, netcons.c): interrupts are off and whatever the panic interrupted may have
 * been in the middle of e_tx, so no lock, no allocation, no log line and every wait bounded. The frame goes onto the ring like any other, and
 * the call waits (at most 20 ms) until the card has taken it, so that the next one finds room; -ETIMEDOUT when it did not (a stuck transmit
 * unit: the caller stops trying). At worst a frame of the ordinary path that was being put on the ring is overwritten; the ring itself stays
 * consistent. */
static int e_netpoll_tx(NetDev *d, const void *frame, size_t len) {
    EDev *e = d->priv;
    if (!e || len < ETH_HLEN || len > BUFSZ) return -EINVAL;
    if (e->gone) return -ENETDOWN;                                  /* e_shutdown() quiesced the card (and put it to sleep): nothing can be sent, so do not wait for it */
    for (int i = 0; i < 200; i++) {                                 /* room on the ring */
        tx_reclaim_quiet(e);
        if ((u16)((e->tx_tail + 1) % NDESC) != e->tx_head) break;
        udelay(100);
    }
    u16 slot = e->tx_tail, next = (u16)((slot + 1) % NDESC);
    if (next == e->tx_head) return -ENOBUFS;
    memcpy(e->txbuf + (size_t)slot * BUFSZ, frame, len);
    e->tx[slot].addr = e->txbuf_phys + (u64)slot * BUFSZ;
    e->tx[slot].len = (u16)len;
    e->tx[slot].cso = 0;
    e->tx[slot].cmd = 0x0B;                                         /* EOP | IFCS | RS */
    e->tx[slot].status = 0;
    e->tx[slot].css = 0;
    e->tx[slot].special = 0;
    barrier();
    e->tx_tail = next;
    wr(e, R_TDT, next);
    if (e->arb_guard && rr(e, R_TDT) != next) wr(e, R_TDT, next);  /* (the ME can swallow a tail write on these parts, see tail_write) */
    for (int i = 0; i < 200 && !(e->tx[slot].status & 1) && rr(e, R_TDH) != next; i++) udelay(100);
    if (!(e->tx[slot].status & 1) && rr(e, R_TDH) != next) return -ETIMEDOUT;
    d->tx_packets++;
    d->tx_bytes += len;
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
    if (e->tx_enable_at && now >= e->tx_enable_at) {                /* variant 9: the transmitter goes on only now, with the link back */
        e->tx_enable_at = 0;
        e->defer_tx = false;
        if (e->e1000e) wr(e, R_TXDCTL, rr(e, R_TXDCTL) | QUEUE_ENABLE);
        tctl_set(e, true);
        flush(e);
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
            klog("Yukari", "%s: the transmit unit did not come back after %u restarts (all %d variants, two rounds); not restarting it again", d->name, e->tx_resets, NTXVAR);
            e_dump(d->name, e, "given up", true);
        }
        e->tx_stuck_since = now;
        return;
    }
    d->tx_errors += (u64)((e->tx_tail + NDESC - e->tx_head) % NDESC);
    klog("Yukari", "%s: the transmit queue stopped moving (variant %d)", d->name, e->txvar);
    e_dump(d->name, e, "stuck", e->tx_resets < NTXVAR);               /* the long dump once per variant, the short one after that */
    e->tx_resets++;
    e->var_reported = false;
    e->tx_ok = false;
    e->txvar = (e->txvar + 1) % NTXVAR;                            /* every new hang tries the next way of starting the transmit unit */
    tx_apply_variant(d, e, e->txvar);
}

/* One step of the receive ladder (g_rxvar): a step with a transmit variant resets the card first (that variant ends in hw_setup(), which starts the receive unit with
 * the step's flags), the others only start the receive unit again. */
static void rx_apply_step(NetDev *d, EDev *e, int step) {
    e->rxvar = step;
    klog("Yukari", "%s: receive step %d: %s", d->name, step, g_rxvar[step].name);
    if (g_rxvar[step].txvar) {
        e->rx_flags = g_rxvar[step].flags;
        tx_apply_variant(d, e, g_rxvar[step].txvar);
        e->rx_flags = g_rxvar[0].flags;
    } else {
        rx_start(e, g_rxvar[step].flags);
    }
    e->rx_good_at = e->rx_good_total;
    e->rx_step_at = net_now();
    e->rx_hw_at = e->cnt[CNT_GPRC];
    e->rx_ok = false;
    e->rx_resets++;
    e->rx_poke_at = net_now() + 300;                                 /* a DISCOVER now: the router's answer proves the step, whatever else is on the wire */
}

/* A card whose receive side looks dead: the link is up, the card counts good frames (GPRC) or hands over frames with blank contents, and not one frame with contents
 * has reached memory. The steps of the ladder follow each other every few seconds until one does; the steps that reset the card need the evidence that frames really
 * arrive (a quiet network is not a dead card). Only the PCH parts are watched (and a QEMU card with a test hook). */
static void rx_watchdog(NetDev *d, EDev *e) {
    if (!(e->pch || e->rxstall || e->rxgarbage) || e->last_link != 1 || !e->link_since) return;
    if (e->rx_good_total != e->rx_good_at && !e->rx_ok) {            /* a frame with contents has reached memory since this step began */
        e->rx_ok = true;
        if (e->rx_resets) {
            klog("Yukari", "%s: receive works with step %d (%s), after %u steps of the ladder", d->name, e->rxvar, g_rxvar[e->rxvar].name, e->rx_resets);
            e->rx_kick = true;
        }
    }
    u64 now = net_now();
    if (now - e->rx_check_at < 250) return;
    e->rx_check_at = now;
    e->cnt[CNT_GPRC] += rr(e, R_GPRC);                               /* it clears when read: cnt_log() adds the other counters to the same totals */
    if (e->rx_poke_at && now >= e->rx_poke_at) {
        e->rx_poke_at = 0;
        if (!d->addr && !e->rx_ok) dhcp_start(d);
    }
    if (e->rx_kick && e->rx_ok) {
        e->rx_kick = false;
        if (!d->addr) dhcp_start(d);                                 /* receive just came back: do not wait for the next DHCP retry (a moment after the first frame, so that an answer in flight is not cut off) */
    }
    if (e->rx_ok || e->rx_gave_up) return;
    u64 since = e->rx_step_at > e->link_since ? e->rx_step_at : e->link_since;
    u64 all = e->cnt[CNT_GPRC], here = e->cnt[CNT_GPRC] - e->rx_hw_at;
    bool evidence = all >= 3 || e->rx_zero >= 3 || e->rxstall || e->rxgarbage;     /* frames really arrive at the card */
    u64 limit = e->rx_resets ? RX_STEP_MS : (evidence ? RX_FIRST_MS / 2 : RX_FIRST_MS);    /* a card that has visibly counted frames is not waited for as long */
    if (now - since < limit) return;
    int next = e->rxvar + 1;
    if (next >= NRXVAR) {
        e->rx_gave_up = true;
        klog("Yukari", "%s: no step of the receive ladder brought a frame with contents into memory (the card counted %lu good frames in all, %u arrived blank); leaving the card as it is",
             d->name, (unsigned long)all, e->rx_zero);
        rx_state(d->name, e, "receive ladder used up");
        regmap(d->name, e, "registers at the end of the receive ladder");
        return;
    }
    if (g_rxvar[next].txvar && !evidence) {
        e->rx_gave_up = true;
        klog("Yukari", "%s: no frame has reached memory, but the card has counted only %lu good frames: a quiet network, not a dead card; the card is not reset", d->name, (unsigned long)all);
        return;
    }
    klog("Yukari", "%s: no frame with contents has reached memory for %lu s although the card counted %lu good frames (%lu since step %d began) and %u arrived blank", d->name,
         (unsigned long)((now - since) / 1000), (unsigned long)all, (unsigned long)here, e->rxvar, e->rx_zero);
    if (!e->rx_resets) rx_state(d->name, e, "no frame with contents in memory");
    rx_apply_step(d, e, next);
}

static void e_poll(NetDev *d) {
    EDev *e = d->priv;
    if (e->gone) return;
    int budget = 64;
    while (budget-- > 0) {
        RxDesc *r = &e->rx[e->rx_next];
        cache_drop(r, sizeof *r);                                /* (see cache_drop) */
        if (!(r->status & 1)) break;                             /* DD: the card is done with it */
        barrier();
        e->rx_dd_total++;
        u16 len = r->len;
        cache_drop(e->rxbuf + (size_t)e->rx_next * BUFSZ, len <= BUFSZ ? len : BUFSZ);
        for (int b = 0; b < 8; b++) if (r->errors & (1u << b)) e->rx_bits[b]++;
        bool blank = len >= 14 && len <= BUFSZ && (frame_blank(e->rxbuf + (size_t)e->rx_next * BUFSZ) || e->rxgarbage > e->rxvar);   /* a descriptor that says "frame" over a buffer of zeros */
        bool ok = (r->status & 2) && !(r->errors & RXERR_FATAL) && len >= 14 && len <= BUFSZ && !blank;
        if (blank) e->rx_zero++;
        else if (ok) e->rx_good_total++;
        if (e->debug && e->rx_traced < 100) {
            char brief[160];
            frame_brief(e->rxbuf + (size_t)e->rx_next * BUFSZ, len, brief, sizeof brief);
            klog("Yukari", "%s: rx #%u (%u bytes, status %02x errors %02x%s): %s", d->name, ++e->rx_traced, len, r->status, r->errors, ok ? "" : ", THROWN AWAY", brief);
        }
        if (!ok) { if (!(r->status & 2)) e->rx_noeop++; else if (r->errors & RXERR_FATAL) e->rx_fatal++; }
        if (blank && e->rx_zero == 1) klog("Yukari", "%s: a frame arrived with a blank Ethernet header (%u bytes, status %02x): the card completed the descriptor and the buffer holds zeros", d->name, len, r->status);
        if (ok) {
            NetBuf *nb = nb_alloc();
            if (!nb || len > nb_room(nb)) e->rx_nobuf++;
            if (nb && len <= nb_room(nb)) {
                memcpy(nb_put(nb, len), e->rxbuf + (size_t)e->rx_next * BUFSZ, len);
                nb->dev = d;
                r->status = 0;
                cache_drop(r, sizeof *r);                        /* (no dirty line of ours may be written back over the card's next write to this descriptor) */
                u16 done = e->rx_next;
                e->rx_next = (u16)((e->rx_next + 1) % NDESC);
                tail_write(e, R_RDT, done, "RDT");               /* give the descriptor back before the packet is processed */
                net_rx(d, nb);
                continue;
            }
            nb_free(nb);
            d->rx_dropped++;
        } else {
            d->rx_errors++;
        }
        r->status = 0;
        cache_drop(r, sizeof *r);
        u16 done = e->rx_next;
        e->rx_next = (u16)((e->rx_next + 1) % NDESC);
        tail_write(e, R_RDT, done, "RDT");
    }
    u32 st = rr(e, R_STATUS);
    int up = (st & STATUS_LU) != 0;
    if (up != e->last_link) {
        e->last_link = up;
        e->link_since = up ? net_now() : 0;
        e->cnt_step = 0;
        e->cnt_next = up && (e->pch || e->debug) ? net_now() + g_cnt_gap_ms[0] : 0;      /* the counter lines of a card that is about to be tried on real hardware */
        if (up) {
            static const int mbps[4] = { 10, 100, 1000, 1000 };
            klog("Yukari", "%s: link up, %d Mb/s %s duplex", d->name, mbps[(st >> 6) & 3], (st & STATUS_FD) ? "full" : "half");
            if (e->pch) pch_link_fixes(e);                           /* what Linux redoes on every link-up of these parts */
        }
    }
    net_set_link(d, up);
    if (e->reset_at_probe && !up && !e->nolink_tried && net_now() - e->probe_at > 12000) {      /* the reset at probe has never been tried on this part: if the link did not come back, power-cycle the PHY once */
        e->nolink_tried = true;
        klog("Yukari", "%s: no link 12 s after the probe-time MAC reset; trying the PHY power cycle once (a cable that is not plugged in costs nothing)", d->name);
        tx_apply_variant(d, e, 10);
    }
    tx_watchdog(d, e);
    rx_watchdog(d, e);
    if (e->cnt_next && net_now() >= e->cnt_next) {
        cnt_log(d, e);
        e->cnt_step++;
        u32 gaps = (u32)(sizeof g_cnt_gap_ms / sizeof g_cnt_gap_ms[0]);
        e->cnt_next = e->cnt_step < gaps ? net_now() + g_cnt_gap_ms[e->cnt_step] : (e->debug && e->cnt_step < gaps + 12 ? net_now() + 10000 : 0);
    }
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
    "the transmit registers exactly as the firmware's driver left them (TCTL, TXDCTL, TARC0/1, TIPG)",
    "the Linux reset path: MAC and PHY reset, packet buffer 26 KB, PHY config ungated, transmitter switched on 1.5 s later",
    "the PHY's power cycled (LANPHYPC toggle), then a MAC reset",
    "a plain transmit queue (TXDCTL without write-back granularity or thresholds)",
};

/* The PHY as the MAC sees it through MDIC (IEEE registers on page 0) and the Kumeran K1 configuration: the link partner's abilities, the speed that was
 * negotiated, whether K1 is on. Taken under the software semaphore; says so when the ME owns the PHY and MDIC does not answer. */
static void phy_dump(EDev *e, const char *who, const char *why) {
    if (!swflag_acquire(e)) { klog("Yukari", "%s: %s: the PHY semaphore is busy; no PHY dump", who, why); return; }
    static const u8 regs[10] = { 0, 1, 2, 3, 4, 5, 6, 9, 10, 15 };
    u16 v[10] = { 0 };
    bool ok = mdic_op(e, 31, 0, true, NULL) == 0;
    for (int i = 0; ok && i < 10; i++)
        if (mdic_op(e, regs[i], 0, false, &v[i]) != 0) ok = false;
    wr(e, R_KMRNCTRLSTA, (7u << 16) | (1u << 21));                /* Kumeran register 7 (K1 config), read request */
    udelay(5);
    u32 k1 = rr(e, R_KMRNCTRLSTA) & 0xFFFFu;
    swflag_release(e);
    if (!ok) { klog("Yukari", "%s: %s: the PHY does not answer on MDIC (KMRN K1_CONFIG %04x)", who, why, k1); return; }
    klog("Yukari", "%s: %s: PHY BMCR %04x BMSR %04x ID %04x %04x ANAR %04x ANLPAR %04x ANER %04x GBCR %04x GBSR %04x EXTSTAT %04x; KMRN K1_CONFIG %04x", who, why, v[0], v[1],
         v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], k1);
}

/* What a photograph of the screen has to say about a card that does not transmit: the registers that matter and the statistics counters (GPTC grows
 * if frames really leave the card, even when the descriptors never come back as done). `full` adds the power, link and second-queue registers, the
 * descriptor the card is stuck on and the PHY. */
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
    klog("Yukari", "%s: %s: TIPG %08x MANC %08x FEXTNVM3 %08x FEXTNVM4 %08x | software: rx next %u, tx %u/%u; guard %s, arbiter waits %u, tail writes that did not hold %u", who, why,
         rr(e, R_TIPG), e->pch ? rr(e, R_MANC) : 0u, e->pch ? rr(e, R_FEXTNVM3) : 0u, e->pch ? rr(e, R_FEXTNVM4) : 0u, e->rx_next, e->tx_head, e->tx_tail,
         e->arb_guard ? "on" : "off", e->arb_waits, e->tail_fixes);
    if (e->pch) phy_dump(e, who, why);
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
    tctl_off(e);
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
static void clear_phyra(EDev *e) {
    u32 s = rr(e, R_STATUS);
    if (s & STATUS_PHYRA) wr(e, R_STATUS, s & ~STATUS_PHYRA);
}

static void mac_reset(EDev *e) {
    wr(e, R_IMC, 0xFFFFFFFFu);
    rctl_off(e);
    tctl_off(e);
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
    if (e->pch) clear_phyra(e);                                    /* a reset leaves "PHY reset asserted" set; Linux clears it by hand */
    if (e->e1000e) wr(e, R_CTRLEXT, rr(e, R_CTRLEXT) | CTRLEXT_DRV_LOAD);
}

/* The MAC and the PHY reset together (CTRL.RST | PHY_RST under the PHY semaphore), as Linux does it when the ME does not forbid it; the link drops and comes back. */
static void mac_phy_reset(EDev *e) {
    wr(e, R_IMC, 0xFFFFFFFFu);
    rctl_off(e);
    tctl_off(e);
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
    if (e->pch) clear_phyra(e);
    if (e->spt && !e->no_errata) {                               /* SPT / KBL silicon errata: keep the transmit queue from hanging */
        wr(e, R_IOSFPC, rr(e, R_IOSFPC) | 0x00010000u);
        u32 t = rr(e, R_TARC0);
        t &= ~(3u << 28);
        t |= (1u << 29);
        wr(e, R_TARC0, t);
        pch_link_fixes(e);
    }

    rx_start(e, e->rx_flags);
    tx_program(e);
}

/* Linux's e1000_toggle_lanphypc_pch_lpt(): the pin that powers the PHY is pulled low and let go again, which makes the PHY and the ME agree on who owns it afresh. */
static void lanphypc_toggle(EDev *e) {
    wr(e, R_FEXTNVM3, (rr(e, R_FEXTNVM3) & ~0x0C000000u) | 0x08000000u);      /* PHY config counter 50 ms */
    u32 c = rr(e, R_CTRL);
    c |= CTRL_LANPHYPC_OVERRIDE;
    c &= ~CTRL_LANPHYPC_VALUE;
    wr(e, R_CTRL, c);
    flush(e);
    udelay(20);
    wr(e, R_CTRL, c & ~CTRL_LANPHYPC_OVERRIDE);
    flush(e);
    for (int i = 0; i < 25 && !(rr(e, R_CTRLEXT) & CTRLEXT_LPCD); i++) udelay(5500);     /* LPCD: the pin change is done */
    udelay(31000);
}

static void tx_apply_variant(NetDev *d, EDev *e, int var) {
    klog("Yukari", "%s: transmit variant %d: %s", d->name, var, g_txvar[var]);
    e->tx_head = e->tx_tail = 0;
    e->tx_stuck_since = 0;
    e->no_errata = (var == 3 || var == 8);
    e->defer_tx = false;
    e->txdctl_simple = false;
    e->tx_enable_at = 0;
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
        rctl_off(e);
        tx_quiesce(e);
        memset(e->tx, 0, (size_t)NDESC * 16);
        tx_program(e);
        tx_enable_wait(e);
        udelay(5000);
        wr(e, R_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
        break;
    case 8:                                                         /* nothing of ours: the registers the firmware's driver had left */
        tx_quiesce(e);
        memset(e->tx, 0, (size_t)NDESC * 16);
        tx_ring_regs(e);
        wr(e, R_TXDCTL, e->fw_txdctl);
        wr(e, R_TXDCTL1, e->fw_txdctl1);
        wr(e, R_TARC0, e->fw_tarc0);
        wr(e, R_TARC1, e->fw_tarc1);
        if (e->fw_tipg) wr(e, R_TIPG, e->fw_tipg);
        wr(e, R_TCTL, e->fw_tctl | TCTL_EN);
        flush(e);
        break;
    case 9:                                                         /* the Linux reset path, the transmitter switched on only after the link is back */
        mac_phy_reset(e);
        clear_phyra(e);
        if (e->ulp) pch_leave_ulp(e, d->name);
        if (e->pch) pch_phy_check(e, d->name);
        wr(e, R_EXTCNF, rr(e, R_EXTCNF) & ~EXTCNF_GATE_PHY_CFG);
        wr(e, R_PBA, 26);
        memset(e->tx, 0, (size_t)NDESC * 16);
        e->defer_tx = true;
        hw_setup(e);
        e->tx_enable_at = net_now() + 1500;
        break;
    case 10:
        lanphypc_toggle(e);
        mac_reset(e);
        if (e->ulp) pch_leave_ulp(e, d->name);
        if (e->pch) pch_phy_check(e, d->name);
        memset(e->tx, 0, (size_t)NDESC * 16);
        hw_setup(e);
        tx_enable_wait(e);
        break;
    case 11:
        tx_quiesce(e);
        memset(e->tx, 0, (size_t)NDESC * 16);
        e->txdctl_simple = true;
        tx_program(e);
        tx_enable_wait(e);
        break;
    default:
        tctl_off(e);
        flush(e);
        udelay(2000);
        memset(e->tx, 0, (size_t)NDESC * 16);
        tx_program(e);
        break;
    }
    e_dump(d->name, e, "after", false);
}

/* ---- Wake on LAN ----
 *
 * Before the machine is switched off the card is armed to wake it for a magic packet (6 bytes of FF, then the station address 16 times; the filter also needs the frame to pass the address filter, hence RCTL.BAM
 * for a broadcast). A card of this family can do it in two places: the MAC (wake-up filter control WUFC, wake-up control WUC) and, on the I217, I218 and I219 whose MAC is powered down in Sx, the PHY, which keeps
 * listening: it has its own copies of the receive address and receive control and a wake-up filter on "page 800" of its registers, reached through the port-control page (769) where register 17 enables access to
 * the wake-up page (bit 2) and arms the PHY's wake-up mode (bit 4), and then through an address opcode (register 17h) and a data opcode (register 12h). The sequence follows the I219 datasheet's wake-up chapter.
 * Then the card is told to let go of its driver (CTRL_EXT.DRV_LOAD off), PCI wake (PME) is enabled and the function goes to D3hot. The other half, the chipset's wake event, is acpi_wake_arm().
 * "e1000.wol=off" on the kernel command line leaves the card as it is. Every step is logged: if the machine does not wake, the log of the shutdown (TOUHOU.PRV on the next start) says how far it got. */
#define R_WUC           0x5800
#define R_WUFC          0x5808
#define R_WUS           0x5810
#define R_PHYCTRL       0x0F10
#define R_FEXTNVM       0x0028
#define CTRL_ADVD3WUC   (1u << 20)      /* advertise D3 wake-up capability */
#define WUC_APME        (1u << 0)
#define WUC_PME_EN      (1u << 1)
#define WUC_PME_STATUS  (1u << 2)
#define WUC_APMPME      (1u << 3)       /* assert PME on an advanced-power-management wake-up */
#define WUC_PHY_WAKE    (1u << 8)
#define WUFC_MAG        (1u << 1)
#define PHYCTRL_D0A_LPLU    (1u << 1)
#define PHYCTRL_NOND0A_LPLU (1u << 2)
#define PHYCTRL_NOND0A_GBE_DISABLE (1u << 3)
#define PHYCTRL_GBE_DISABLE (1u << 6)   /* no gigabit while the machine sleeps (a 10/100 link is enough to hear a magic packet) */
#define FEXTNVM_SW_CONFIG (1u << 27)    /* the driver, not the hardware, writes the PHY's OEM bits */
#define FEXTNVM6_REQ_PLL_CLK (1u << 8)
#define FWSM_RSPCIPHY   0x00000040u     /* software may reset the PHY (the ME allows it) */
#define PHY_OEM_BITS    25              /* page 0 */
#define OEM_LPLU        0x0004
#define OEM_GBE_DIS     0x0040
#define OEM_RESTART_AN  0x0400
#define PHY_PAGE_SELECT 31
#define PHY_PG(n)       ((u16)((n) << 5))
#define PHY_PAGE_PORTCTRL 769
#define PHY_PAGE_WAKE   800
#define PHY_WAKE_ENABLE_REG 17
#define WAKE_ENABLE     (1u << 2)       /* access to the wake-up page */
#define WAKE_HOST       (1u << 4)       /* the PHY's host wake-up mode */
#define WAKE_ME         (1u << 5)
#define WAKE_ADDR_OP    0x11
#define WAKE_DATA_OP    0x12

static bool phy_wake_open(EDev *e, u16 *saved) {
    u16 v;
    if (mdic_op(e, PHY_PAGE_SELECT, PHY_PG(PHY_PAGE_PORTCTRL), true, NULL) || mdic_op(e, PHY_WAKE_ENABLE_REG, 0, false, &v)) return false;
    *saved = v;
    v = (u16)((v | WAKE_ENABLE) & ~(WAKE_ME | WAKE_HOST));      /* both wake-up modes off while the registers are written: no power state change in between */
    return mdic_op(e, PHY_WAKE_ENABLE_REG, v, true, NULL) == 0 && mdic_op(e, PHY_PAGE_SELECT, PHY_PG(PHY_PAGE_WAKE), true, NULL) == 0;
}

static bool phy_wake_put(EDev *e, u8 reg, u16 val) {
    return mdic_op(e, WAKE_ADDR_OP, reg, true, NULL) == 0 && mdic_op(e, WAKE_DATA_OP, val, true, NULL) == 0;
}

static bool phy_wake_get(EDev *e, u8 reg, u16 *val) {
    return mdic_op(e, WAKE_ADDR_OP, reg, true, NULL) == 0 && mdic_op(e, WAKE_DATA_OP, 0, false, val) == 0;
}

/* Back to the port-control page; with `arm` the PHY's host wake-up mode is switched on as well, without it the register is put back as it was found. */
static bool phy_wake_close(EDev *e, u16 saved, bool arm) {
    u16 v = arm ? (u16)(saved | WAKE_ENABLE | WAKE_HOST) : saved;
    return mdic_op(e, PHY_PAGE_SELECT, PHY_PG(PHY_PAGE_PORTCTRL), true, NULL) == 0 && mdic_op(e, PHY_WAKE_ENABLE_REG, v, true, NULL) == 0;
}

/* The PHY side: the station address (receive address 0), the receive control bits that decide which frames reach the wake-up filter, and the filter itself. The registers are
 * read back before the page is closed and logged: if the machine does not wake, the first question is whether these writes took effect. */
static bool pch_phy_wake(EDev *e, const char *name, u32 rctl) {
    static const u8 regs[7] = { 16, 17, 18, 19, 0, 2, 1 };       /* receive address 0 (low, high, RAH, valid), receive control, WUFC, WUC */
    u16 saved = 0, back[7] = { 0 }, brctl = 0;
    bool got = false;
    if (!swflag_acquire(e)) { klog("Yukari", "%s: wake-up: the PHY semaphore stays busy", name); return false; }
    bool ok = phy_wake_open(e, &saved);
    if (ok) {
        u32 ral = rr(e, R_RAL0), rah = rr(e, R_RAH0);
        if (rctl & RCTL_UPE) brctl |= 0x0001;                                    /* unicast promiscuous */
        if (rctl & RCTL_MPE) brctl |= 0x0002;                                    /* multicast promiscuous */
        brctl |= (u16)(((rctl >> 12) & 3u) << 3);                                /* multicast offset */
        if (rctl & RCTL_BAM) brctl |= 0x0020;                                    /* accept broadcast */
        if (rctl & (1u << 23)) brctl |= 0x0040;                                  /* pass MAC control frames */
        ok = phy_wake_put(e, 16, (u16)ral) && phy_wake_put(e, 17, (u16)(ral >> 16)) && phy_wake_put(e, 18, (u16)rah) && phy_wake_put(e, 19, (u16)((rah & (1u << 31)) >> 16)) &&
             phy_wake_put(e, 0, brctl);
        wr(e, R_WUFC, WUFC_MAG);
        wr(e, R_WUC, WUC_PHY_WAKE | WUC_APMPME | WUC_PME_STATUS | WUC_PME_EN | WUC_APME);
        ok = ok && phy_wake_put(e, 2, WUFC_MAG) && phy_wake_put(e, 1, WUC_PME_EN | WUC_APME);
        got = true;
        for (int i = 0; i < 7; i++) got = got && phy_wake_get(e, regs[i], &back[i]);
        ok = phy_wake_close(e, saved, ok) && ok;
    } else {
        wr(e, R_WUFC, WUFC_MAG);                                                 /* the PHY's wake-up page does not answer: the MAC's own registers are still worth setting */
        wr(e, R_WUC, WUC_PME_EN | WUC_APME);
    }
    swflag_release(e);
    if (!got) klog("Yukari", "%s: wake-up, PHY side: %s (the wake-up page %s)", name, ok ? "written, not read back" : "FAILED", ok ? "answered the writes only" : "did not answer");
    else klog("Yukari", "%s: wake-up, PHY side %s: receive address 0 %04x %04x %04x (valid %04x), receive control %04x (wanted %04x), WUFC %04x, WUC %04x; 769.17 was %04x", name,
              ok ? "written" : "FAILED", back[0], back[1], back[2], back[3], back[4], brctl, back[5], back[6], saved);
    return ok;
}

/* Linux's e1000_oem_bits_config_ich8lan(hw, false): on the parts whose NVM leaves it to the driver (FEXTNVM bit 27), the PHY is told that the sleep state has no gigabit and to
 * negotiate again, so that the link which survives the power-off is a 10/100 one. */
static void pch_oem_bits(EDev *e, const char *name) {
    u32 fe = rr(e, R_FEXTNVM), pc = rr(e, R_PHYCTRL);
    if (!(fe & FEXTNVM_SW_CONFIG)) { klog("Yukari", "%s: wake-up: FEXTNVM %08x, PHY_CTRL %08x: the hardware sets the PHY's OEM bits", name, fe, pc); return; }
    if (!swflag_acquire(e)) { klog("Yukari", "%s: wake-up: the PHY semaphore stays busy; OEM bits left", name); return; }
    u16 v = 0, was = 0;
    bool ok = mdic_op(e, PHY_PAGE_SELECT, 0, true, NULL) == 0 && mdic_op(e, PHY_OEM_BITS, 0, false, &v) == 0;
    if (ok) {
        was = v;
        v = (u16)(v & ~(OEM_GBE_DIS | OEM_LPLU));
        if (pc & (PHYCTRL_GBE_DISABLE | PHYCTRL_NOND0A_GBE_DISABLE)) v |= OEM_GBE_DIS;
        if (pc & (PHYCTRL_D0A_LPLU | PHYCTRL_NOND0A_LPLU)) v |= OEM_LPLU;
        if (rr(e, R_FWSM) & FWSM_RSPCIPHY) v |= OEM_RESTART_AN;                  /* (only when the firmware lets software restart the PHY) */
        ok = mdic_op(e, PHY_OEM_BITS, v, true, NULL) == 0;
    }
    swflag_release(e);
    klog("Yukari", "%s: wake-up: FEXTNVM %08x, PHY_CTRL %08x: OEM bits %04x -> %04x%s", name, fe, pc, was, v, ok ? "" : " FAILED");
}

/* What woke the machine, and clearing it. The card keeps the cause of a wake-up (the MAC's WUS, the PHY's own status on page 800) until software writes it back, and a cause that is still
 * there when the machine is switched off again wakes it at once: the second and third hardware cycles did exactly that, the Dell switched itself on the moment it was off. So the cause is
 * read, logged and cleared at start (it also says what woke this start: bit 1 is a magic packet) and again as the last step before the power goes. With `arm` the PHY's wake-up mode stays
 * armed when its page is closed (the last step), otherwise the page is left as it was found. The PCI power-management status is cleared too. */
static void wake_cause(EDev *e, const char *name, const char *when, bool arm) {
    u32 wuc = rr(e, R_WUC), wufc = rr(e, R_WUFC), wus = rr(e, R_WUS);
    u16 pwus = 0, saved = 0;
    bool seen = false, wrote = false;
    if (e->pch && swflag_acquire(e)) {
        if (phy_wake_open(e, &saved)) {
            seen = phy_wake_get(e, 3, &pwus);                                    /* the PHY's wake-up status */
            wrote = phy_wake_put(e, 3, 0xFFFF);
            (void)phy_wake_close(e, saved, arm);
        }
        swflag_release(e);
    }
    wr(e, R_WUS, 0xFFFFFFFFu);                                                   /* (write 1 to clear) */
    u8 pm = pci_find_cap(e->pci, PCI_CAP_PM);
    u16 pmcsr = pm ? pci_read16(e->pci, (u16)(pm + 4)) : 0;
    if (pm && (pmcsr & 0x8000u)) pci_write16(e->pci, (u16)(pm + 4), pmcsr);     /* (the same: the PME status bit) */
    char phy[48] = "";
    if (e->pch) snprintf(phy, sizeof phy, ", PHY status %s%04x", seen ? "" : "unreadable ", pwus);
    klog("Yukari", "%s: wake-up status %s: WUS %08x%s%s, WUC %08x WUFC %08x, PMCSR %04x; cleared%s", name, when, wus, ((wus | pwus) & 2u) ? " (a magic packet)" : "", phy, wuc, wufc, pmcsr,
         (wrote || !e->pch) ? "" : " (the PHY's not)");
}

static bool wol_disabled(void) {
    char v[8];
    return cmdline_value("e1000.wol", v, sizeof v) && (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "no"));
}

/* The last step, interrupts off, the card still awake: the wake cause is cleared once more (a magic packet that came in during the shutdown, or one the card kept from before, would wake the
 * machine the moment it is off), then the function goes to D3hot with PME enabled. */
static bool e_shutdown_last(EDev *e, const char *name) {
    if (!e->wol_armed || e->asleep) return e->wol_armed;
    e->asleep = true;
    wake_cause(e, name, "at the last step", true);
    u8 pm = pci_find_cap(e->pci, PCI_CAP_PM);
    if (pm) {
        u16 pmcsr = pci_read16(e->pci, (u16)(pm + 4));
        pci_write16(e->pci, (u16)(pm + 4), (u16)((pmcsr | 0x8100u) | 3u));      /* PME status cleared (write 1), PME enabled, D3hot */
        klog("Yukari", "%s: the card sleeps: PMCSR %04x -> %04x (PME on, D3hot)", name, pmcsr, pci_read16(e->pci, (u16)(pm + 4)));
    } else {
        klog("Yukari", "%s: no power-management capability: the card stays in D0", name);
    }
    return true;
}

/* Called twice (netdev_shutdown_all): the first call, early in the shutdown, programs the card and logs what it did so that the lines reach the disk; the second, the last thing before the
 * power goes, clears the wake cause and puts the card to sleep (e_shutdown_last). */
static bool e_shutdown(NetDev *d) {
    EDev *e = d->priv;
    const char *name = d->name;
    if (!e) return false;
    if (e->gone) return e_shutdown_last(e, name);
    if (wol_disabled()) return false;
    e->gone = true;                                                              /* from here on e_poll and e_tx leave the card alone */
    u32 rctl = rr(e, R_RCTL);
    rctl_off(e);                                                                 /* receive DMA stops: the card has to be quiet before it sleeps */
    tctl_off(e);
    flush(e);
    udelay(10000);
    wr(e, R_CTRL, rr(e, R_CTRL) | CTRL_ADVD3WUC);
    bool ok;
    if (e->pch) {
        wr(e, R_FEXTNVM6, rr(e, R_FEXTNVM6) & ~FEXTNVM6_REQ_PLL_CLK);            /* (the I217-and-later suspend workaround: the PLL clock is requested only when needed) */
        wr(e, R_PHYCTRL, rr(e, R_PHYCTRL) | PHYCTRL_GBE_DISABLE);
        pch_oem_bits(e, name);
        ok = pch_phy_wake(e, name, rctl);
        wr(e, R_CTRLEXT, rr(e, R_CTRLEXT) & ~CTRLEXT_DRV_LOAD);                  /* "no driver owns this card any more": the firmware (the ME) takes over */
    } else {
        wr(e, R_WUFC, WUFC_MAG);
        wr(e, R_WUC, WUC_PME_EN);
        ok = true;
    }
    wr(e, R_RCTL, rctl | RCTL_EN | RCTL_BAM);                                    /* (Linux's e1000_setup_rctl: the receiver is switched on again for the sleep) */
    flush(e);
    u32 wufc = rr(e, R_WUFC), wuc = rr(e, R_WUC), ctrl = rr(e, R_CTRL), status = rr(e, R_STATUS);        /* (read back: they are what the card will wake on) */
    klog("Yukari", "%s: wake-up %s: magic packets for %02x:%02x:%02x:%02x:%02x:%02x%s (WUFC %08x WUC %08x CTRL %08x STATUS %08x, RCTL was %08x); the card sleeps as the last step", name,
         ok ? "armed" : "armed in part", d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5], e->pch ? " in the MAC and the PHY" : "", wufc, wuc, ctrl, status, rctl);
    e->wol_armed = true;
    return true;                                                                 /* tried: the chipset's half (acpi_wake_arm) is worth doing even if a step failed */
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
    e->debug = cmdline_flag("nicdebug");
    e->arb_guard = e->spt && (rr(e, R_FWSM) & FWSM_FW_VALID);      /* the ME is there and shares the register arbiter with the host */
    e->fw_tctl = rr(e, R_TCTL);
    e->fw_txdctl = rr(e, R_TXDCTL);
    e->fw_txdctl1 = rr(e, R_TXDCTL1);
    e->fw_tarc0 = rr(e, R_TARC0);
    e->fw_tarc1 = rr(e, R_TARC1);
    e->fw_tipg = rr(e, R_TIPG);
    e->fw_rxdctl = rr(e, R_RXDCTL);
    e->fw_rdtr = rr(e, R_RDTR) & ~0x80000000u;                     /* (bit 31 is the write-only "flush partial descriptor" strobe) */
    e->fw_radv = rr(e, R_RADV);
    e->fw_rfctl = rr(e, R_RFCTL);
    e->fw_rxcsum = rr(e, R_RXCSUM);
    if (e->pch) {
        e_dump(name, e, "as the firmware left it", true);
        regmap(name, e, "registers as the firmware left them");
        e->fw_ring_phys = ((u64)rr(e, R_RDBAH) << 32) | rr(e, R_RDBAL);
        e->fw_ring_len = rr(e, R_RDLEN);
        if (e->fw_ring_phys && !(e->fw_ring_phys & 15) && e->fw_ring_len >= 16 && e->fw_ring_len <= 4096) {      /* a copy of the firmware's ring, if that memory is mapped at all */
            const u8 *v = phys_to_virt(e->fw_ring_phys);
            if (!vmm_translate(vmm_kernel_pml4(), (u64)v, NULL) || !vmm_translate(vmm_kernel_pml4(), (u64)v + e->fw_ring_len - 1, NULL))
                v = ioremap(e->fw_ring_phys, e->fw_ring_len, 0);       /* (the direct map does not cover the firmware's memory: map it for the look) */
            if (v) {
                e->fw_ring_copy = kmalloc(e->fw_ring_len);
                if (e->fw_ring_copy) { memcpy(e->fw_ring_copy, v, e->fw_ring_len); e->fw_ring_virt = v; }
            }
        }
        u32 gcr = rr(e, R_GCR);
        if (gcr & GCR_NO_SNOOP) {
            klog("Yukari", "%s: GCR %08x lets the card bypass the CPU's caches for ring and buffer accesses; switching that off", name, gcr);
            wr(e, R_GCR, gcr & ~GCR_NO_SNOOP);
        }
    }

    wr(e, R_IMC, 0xFFFFFFFFu);
    rctl_off(e);
    tctl_off(e);                                                   /* (only the enable bits: the rest is what the firmware or the reset default chose) */
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

    wake_cause(e, name, "at start", false);                        /* what woke this start (a magic packet?), and a clean slate for the next power-off */

    u32 ral = rr(e, R_RAL0), rah = rr(e, R_RAH0);
    u8 mac[6] = { (u8)ral, (u8)(ral >> 8), (u8)(ral >> 16), (u8)(ral >> 24), (u8)rah, (u8)(rah >> 8) };
    if ((mac[0] & 1) || !(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) || (mac[0] & mac[1] & mac[2] & mac[3] & mac[4] & mac[5]) == 0xFF) goto fail;

    void *ring = dma_alloc_below_4g((size_t)NDESC * 16 * 2, &e->rx_phys);   /* receive ring, then transmit ring right behind it (all of it below 4 GiB, whatever the card's idea of 64-bit DMA) */
    if (!ring) goto fail;
    e->rx = ring;
    e->tx = (TxDesc *)((u8 *)ring + (size_t)NDESC * 16);
    e->tx_phys = e->rx_phys + (size_t)NDESC * 16;
    e->rxbuf = dma_alloc_below_4g((size_t)NDESC * BUFSZ, &e->rxbuf_phys);
    e->txbuf = dma_alloc_below_4g((size_t)NDESC * BUFSZ, &e->txbuf_phys);
    if (!e->rxbuf || !e->txbuf) goto fail;
    memset(e->rxbuf, 0, (size_t)NDESC * BUFSZ);                   /* (so that a buffer holding data later was written by the card) */
    for (int i = 0; i < NDESC; i++) e->rx[i].addr = e->rxbuf_phys + (u64)i * BUFSZ;
    if (e->pch) klog("Yukari", "%s: receive ring at %#lx, transmit ring at %#lx, receive buffers at %#lx, transmit buffers at %#lx; register write guard %s", name,
                     (unsigned long)e->rx_phys, (unsigned long)e->tx_phys, (unsigned long)e->rxbuf_phys, (unsigned long)e->txbuf_phys, e->arb_guard ? "on" : "off");

    /* test and experiment hooks: e1000.txvar=N starts at transmit variant N, e1000.txstall=N pretends a hang until variant N */
    long v = cmdline_int("e1000.txvar", 0);
    if (v > 0 && v < NTXVAR) e->txvar = (int)v;
    v = cmdline_int("e1000.txstall", 0);
    if (v > 0 && v < NTXVAR) e->txstall = (int)v;
    v = cmdline_int("e1000.rxstall", 0);                           /* e1000.rxstall=N: the receiver gets no descriptors until step N of the receive ladder */
    if (v > 0 && v < NRXVAR) e->rxstall = (int)v;
    e->no_errata = (e->txvar == 3 || e->txvar == 8);
    v = cmdline_int("e1000.rxgarbage", 0);                         /* e1000.rxgarbage=N: received frames count as blank until step N of the receive ladder */
    if (v > 0 && v < NRXVAR) e->rxgarbage = (int)v;
    e->rx_flags = g_rxvar[0].flags;
    if (e->pch && !cmdline_flag("e1000.noreset")) {                /* a clean slate: the firmware's driver left the card running on rings of its own, and a card that is not reset keeps fetching */
        if (pci_read16(e->pci, PCI_DESC_RING_STATUS) & FLUSH_DESC_REQUIRED) flush_desc_rings(e, name);        /* descriptors from them (the fourth round: frames written into the firmware's old buffers) */
        mac_reset(e);
        if (rr(e, R_RAL0) != ral || (rr(e, R_RAH0) & 0xFFFFu) != (rah & 0xFFFFu)) {
            klog("Yukari", "%s: the reset reloaded the station address as %08x %08x; putting ours back", name, rr(e, R_RAL0), rr(e, R_RAH0));
            wr(e, R_RAL0, ral);
            wr(e, R_RAH0, (rah & 0xFFFFu) | (1u << 31));
        }
        if (e->fw_tipg) wr(e, R_TIPG, e->fw_tipg);                 /* what the firmware tuned and the reset reverted, put back: transmit worked with these (hw_setup() adds its own bits on top, as before) */
        wr(e, R_TARC0, e->fw_tarc0);
        wr(e, R_TARC1, e->fw_tarc1);
        wr(e, R_TXDCTL1, e->fw_txdctl1);
        wr(e, R_TCTL, e->fw_tctl & ~TCTL_EN);
        klog("Yukari", "%s: the MAC was reset before its rings were programmed (e1000.noreset leaves it as the firmware left it)", name);
        e->reset_at_probe = true;
    }
    hw_setup(e);

    NetDev *d = kzalloc(sizeof *d);
    if (!d) goto fail;
    snprintf(d->driver, sizeof d->driver, "%s", e->pch ? "e1000e(pch)" : e->e1000e ? "e1000e" : "e1000");
    memcpy(d->mac, mac, 6);
    d->mtu = 1500;
    d->flags = NDF_BROADCAST;
    d->tx = e_tx;
    d->poll = e_poll;
    d->shutdown = e_shutdown;
    d->netpoll_tx = e_netpoll_tx;
    d->priv = e;
    if (rr(e, R_STATUS) & STATUS_LU) d->flags |= NDF_LINK;
    p->driver = "e1000";
    p->driver_data = d;
    e->probe_at = net_now();
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
