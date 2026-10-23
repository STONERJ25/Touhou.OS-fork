/*
 * Intel gigabit Ethernet, the legacy-descriptor family: e1000 (82540EM / 82545EM / 82544GC, QEMU's `-device e1000`), e1000e
 * (82574L, QEMU's `-device e1000e`) and, best effort, the PCH LAN parts of real desktops (I217, I218, I219).
 *
 * Polled from the Yukari thread: 256 receive and 256 transmit descriptors, legacy format, 2 KiB buffers, no interrupts, no
 * offloads. The PCH parts are the risky ones (their PHY is shared with the management engine): for them we never issue a
 * global reset and never touch the PHY, only the MAC's own rings, and if the MAC does not answer sanely we log one line and
 * leave the card alone.
 */
#include "net.h"
#include "pci.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "irq.h"
#include "initcall.h"

#define INTEL 0x8086

#define R_CTRL    0x0000
#define R_STATUS  0x0008
#define R_CTRLEXT 0x0018
#define R_ICR     0x00C0
#define R_IMC     0x00D8
#define R_RCTL    0x0100
#define R_TCTL    0x0400
#define R_TIPG    0x0410
#define R_RDBAL   0x2800
#define R_RDBAH   0x2804
#define R_RDLEN   0x2808
#define R_RDH     0x2810
#define R_RDT     0x2818
#define R_RXDCTL  0x2828
#define R_TDBAL   0x3800
#define R_TDBAH   0x3804
#define R_TDLEN   0x3808
#define R_TDH     0x3810
#define R_TDT     0x3818
#define R_TXDCTL  0x3828
#define R_TARC0   0x3840
#define R_RXCSUM  0x5000
#define R_MTA     0x5200
#define R_RAL0    0x5400
#define R_RAH0    0x5404
#define R_IOSFPC  0x0F28

#define CTRL_FD   (1u << 0)
#define CTRL_SLU  (1u << 6)
#define CTRL_ASDE (1u << 5)
#define CTRL_ILOS (1u << 7)
#define CTRL_RST  (1u << 26)
#define CTRL_VME  (1u << 30)
#define CTRL_PHYRST (1u << 31)
#define CTRL_LRST (1u << 3)
#define STATUS_LU (1u << 1)
#define RCTL_EN   (1u << 1)
#define RCTL_BAM  (1u << 15)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN   (1u << 1)
#define TCTL_PSP  (1u << 3)
#define QUEUE_ENABLE (1u << 25)

#define NDESC   256
#define BUFSZ   2048

typedef struct { u64 addr; u16 len; u16 csum; u8 status; u8 errors; u16 special; } PACKED RxDesc;
typedef struct { u64 addr; u16 len; u8 cso; u8 cmd; u8 status; u8 css; u16 special; } PACKED TxDesc;

typedef struct EDev {
    PciDev *pci;
    volatile u8 *mmio;
    bool pch;                           /* I217 / I218 / I219: be careful */
    bool e1000e;                        /* 82574L and the PCH parts: queue-enable bits in RXDCTL/TXDCTL */
    bool spt;                           /* I219 on Sunrise Point / Kaby Lake: transmit hang errata */
    RxDesc *rx;
    TxDesc *tx;
    paddr_t rx_phys, tx_phys, rxbuf_phys, txbuf_phys;
    u8 *rxbuf, *txbuf;
    u16 rx_next;                        /* next descriptor to examine */
    u16 tx_head, tx_tail;               /* software view: tail = next to fill, head = oldest unfinished */
} EDev;

static inline u32 rr(EDev *e, u32 reg) { return *(volatile u32 *)(e->mmio + reg); }
static inline void wr(EDev *e, u32 reg, u32 v) { *(volatile u32 *)(e->mmio + reg) = v; }
static inline void flush(EDev *e) { (void)rr(e, R_STATUS); }

static const struct { u16 id; u8 kind; const char *name; } g_ids[] = {
    { 0x100E, 0, "82540EM" }, { 0x100F, 0, "82545EM" }, { 0x1004, 0, "82544GC" }, { 0x1008, 0, "82544EI" }, { 0x1019, 0, "82547EI" },
    { 0x101E, 0, "82540EP" }, { 0x1026, 0, "82545GM" }, { 0x1076, 0, "82541GI" }, { 0x107C, 0, "82541PI" }, { 0x1010, 0, "82546EB" },
    { 0x10D3, 1, "82574L" }, { 0x10F6, 1, "82574LA" },
    { 0x153A, 2, "I217-LM" }, { 0x153B, 2, "I217-V" }, { 0x155A, 2, "I218-LM" }, { 0x1559, 2, "I218-V" }, { 0x15A0, 2, "I218-LM2" },
    { 0x15A1, 2, "I218-V2" }, { 0x15A2, 2, "I218-LM3" }, { 0x15A3, 2, "I218-V3" },
    { 0x156F, 3, "I219-LM" }, { 0x1570, 3, "I219-V" }, { 0x15B7, 3, "I219-LM2" }, { 0x15B8, 3, "I219-V2" }, { 0x15B9, 3, "I219-LM3" },
    { 0x15BB, 3, "I219-LM7" }, { 0x15BC, 3, "I219-V7" }, { 0x15BD, 3, "I219-LM6" }, { 0x15BE, 3, "I219-V6" }, { 0x15D6, 3, "I219-V5" },
    { 0x15D7, 3, "I219-LM4" }, { 0x15D8, 3, "I219-V4" }, { 0x15E3, 3, "I219-LM5" },
};

/* ---- netdev ---- */

static int e_tx(NetDev *d, NetBuf *nb) {
    EDev *e = d->priv;
    /* reclaim finished descriptors */
    while (e->tx_head != e->tx_tail && (e->tx[e->tx_head].status & 1)) {
        e->tx[e->tx_head].status = 0;
        e->tx_head = (u16)((e->tx_head + 1) % NDESC);
    }
    u16 next = (u16)((e->tx_tail + 1) % NDESC);
    if (next == e->tx_head || nb->len > BUFSZ) { d->tx_dropped++; nb_free(nb); return -ENOBUFS; }
    u16 i = e->tx_tail;
    memcpy(e->txbuf + (size_t)i * BUFSZ, nb_data(nb), nb->len);
    e->tx[i].addr = e->txbuf_phys + (u64)i * BUFSZ;
    e->tx[i].len = nb->len;
    e->tx[i].cso = 0;
    e->tx[i].cmd = 0x0B;                                         /* EOP | IFCS | RS */
    e->tx[i].status = 0;
    e->tx[i].css = 0;
    e->tx[i].special = 0;
    barrier();
    e->tx_tail = next;
    wr(e, R_TDT, next);
    d->tx_packets++;
    d->tx_bytes += nb->len;
    nb_free(nb);
    return 0;
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
    net_set_link(d, (rr(e, R_STATUS) & STATUS_LU) != 0);
}

/* ---- probing ---- */

static bool wait_clear(EDev *e, u32 reg, u32 mask, int ms) {
    for (int i = 0; i < ms * 10; i++) {
        if (!(rr(e, reg) & mask)) return true;
        udelay(100);
    }
    return false;
}

static void e_probe(PciDev *p, int kind, const char *name) {
    EDev *e = kzalloc(sizeof *e);
    if (!e) return;
    e->pci = p;
    e->pch = kind >= 2;
    e->e1000e = kind >= 1;
    e->spt = kind == 3;
    pci_enable(p, PCI_CMD_MEM | PCI_CMD_MASTER);
    e->mmio = pci_map_bar(p, 0);
    if (!e->mmio) goto fail;
    u32 st = rr(e, R_STATUS);
    if (st == 0xFFFFFFFFu) goto fail;                              /* the registers do not answer */

    wr(e, R_IMC, 0xFFFFFFFFu);
    wr(e, R_RCTL, 0);
    wr(e, R_TCTL, TCTL_PSP);
    flush(e);
    udelay(10000);
    if (!e->pch) {                                               /* a global reset gives a known starting point on the QEMU models */
        wr(e, R_CTRL, rr(e, R_CTRL) | CTRL_RST);
        udelay(20000);
        if (!wait_clear(e, R_CTRL, CTRL_RST, 100)) goto fail;
        wr(e, R_IMC, 0xFFFFFFFFu);
        (void)rr(e, R_ICR);
    } else {
        (void)rr(e, R_ICR);
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

    /* link up, auto speed detection */
    u32 ctrl = rr(e, R_CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    ctrl &= ~(CTRL_LRST | CTRL_PHYRST | CTRL_ILOS | CTRL_VME);
    wr(e, R_CTRL, ctrl);
    for (int i = 0; i < 128; i++) wr(e, R_MTA + 4 * (u32)i, 0);

    if (e->pch) {                                                /* Linux's e1000_initialize_hw_bits_ich8lan */
        wr(e, R_CTRLEXT, rr(e, R_CTRLEXT) | (1u << 22));
        wr(e, R_TXDCTL, rr(e, R_TXDCTL) | (1u << 22));
        wr(e, R_TARC0, rr(e, R_TARC0) | (1u << 23) | (1u << 24) | (1u << 26) | (1u << 27));
    }
    if (e->spt) {                                                /* SPT / KBL silicon errata: keep the transmit queue from hanging */
        wr(e, R_IOSFPC, rr(e, R_IOSFPC) | 0x00010000u);
        u32 t = rr(e, R_TARC0);
        t &= ~(3u << 28);
        t |= (1u << 29);
        wr(e, R_TARC0, t);
    }

    /* receive */
    wr(e, R_RDBAL, (u32)e->rx_phys);
    wr(e, R_RDBAH, (u32)(e->rx_phys >> 32));
    wr(e, R_RDLEN, NDESC * 16);
    wr(e, R_RDH, 0);
    wr(e, R_RDT, 0);
    if (e->e1000e) wr(e, R_RXDCTL, (rr(e, R_RXDCTL) & ~0x3Fu) | QUEUE_ENABLE | 1u);
    wr(e, R_RXCSUM, 0);
    wr(e, R_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);                 /* 2048-byte buffers, broadcast accepted, CRC stripped */
    wr(e, R_RDT, NDESC - 1);
    /* transmit */
    wr(e, R_TDBAL, (u32)e->tx_phys);
    wr(e, R_TDBAH, (u32)(e->tx_phys >> 32));
    wr(e, R_TDLEN, NDESC * 16);
    wr(e, R_TDH, 0);
    wr(e, R_TDT, 0);
    wr(e, R_TIPG, 0x0060200A);
    if (e->e1000e) wr(e, R_TXDCTL, (rr(e, R_TXDCTL) & ~0x3Fu) | QUEUE_ENABLE | (1u << 24) | 1u);
    wr(e, R_TCTL, TCTL_EN | TCTL_PSP | (0x0Fu << 4) | (0x40u << 12));
    flush(e);

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
    netdev_register("eth", d);
    klog("Yukari", "%s: Intel %s at %02x:%02x:%02x.%x, MAC %02x:%02x:%02x:%02x:%02x:%02x", d->name, name, 0, p->bus, p->dev, p->fn,
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
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
    for (PciDev *p = pci_find_id(INTEL, 0xFFFF, NULL); p; p = pci_find_id(INTEL, 0xFFFF, p)) {
        if (p->driver || p->class_code != 2) continue;
        for (unsigned i = 0; i < ARRAY_SIZE(g_ids); i++)
            if (g_ids[i].id == p->device) { e_probe(p, g_ids[i].kind, g_ids[i].name); break; }
    }
}
INITCALL(e1000_init);
