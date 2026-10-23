/*
 * Realtek RTL8168 / RTL8111 (PCIe) and RTL8169 gigabit Ethernet -- the "r8169" family that most desktop boards carry.
 * QEMU has no such device, so this driver has never run; it is written against the datasheet's programming model and what
 * Linux's r8169 does for every chip revision: soft reset, unlock the configuration registers (Cfg9346), program the MAC
 * address / descriptor rings / receive and transmit configuration, enable the receiver and transmitter, relock. It is small
 * on purpose: polled from the Yukari thread, 64 receive and 32 transmit descriptors of the "C+" format, 2 KiB buffers, no
 * interrupts, checksum offload and VLAN stripping off.
 *
 * What it does not do (and Linux does, per chip revision): the long tables of PHY and ERI/OCP register tuning of the
 * RTL8168g/h/ep generation. The firmware has already brought the PHY up for its own network boot, so link and traffic should
 * work without them; throughput or energy saving may not be ideal. If the registers read as all ones, the soft reset does
 * not finish, or the MAC address is invalid, the driver logs one line and leaves the card alone.
 */
#include "net.h"
#include "pci.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "irq.h"
#include "initcall.h"

#define RTL_VENDOR 0x10EC

#define R_MAC0     0x00
#define R_TXDESC_LO 0x20
#define R_TXDESC_HI 0x24
#define R_CHIPCMD  0x37
#define R_TXPOLL   0x38
#define R_INTRMASK 0x3C
#define R_INTRSTAT 0x3E
#define R_TXCONFIG 0x40
#define R_RXCONFIG 0x44
#define R_CFG9346  0x50
#define R_PHYAR    0x60
#define R_PHYSTAT  0x6C
#define R_RXMAXSIZE 0xDA
#define R_CPLUSCMD 0xE0
#define R_RXDESC_LO 0xE4
#define R_RXDESC_HI 0xE8
#define R_MAXTXPKT 0xEC

#define CMD_RESET  0x10
#define CMD_RXEN   0x08
#define CMD_TXEN   0x04
#define TXPOLL_NPQ 0x40                 /* "go": the normal-priority transmit queue has work */
#define CFG9346_UNLOCK 0xC0
#define CFG9346_LOCK   0x00
#define PHYSTAT_LINK   0x02

#define DESC_OWN   (1u << 31)
#define DESC_END   (1u << 30)
#define DESC_FIRST (1u << 29)
#define DESC_LAST  (1u << 28)
#define RX_RES     (1u << 21)           /* receive error summary */

#define NRX   64
#define NTX   32
#define BUFSZ 2048

typedef struct { u32 opts1, opts2; u64 addr; } PACKED Desc;

typedef struct R69 {
    PciDev *pci;
    volatile u8 *mmio;
    u16 io;                             /* used instead of mmio when the card shows no memory BAR */
    Desc *rx, *tx;
    paddr_t rx_phys, tx_phys, rxbuf_phys, txbuf_phys;
    u8 *rxbuf, *txbuf;
    u16 rx_next, tx_head, tx_tail;
} R69;

static inline u8  r8(R69 *r, u16 o)  { return r->mmio ? *(volatile u8 *)(r->mmio + o) : inb((u16)(r->io + o)); }
static inline u16 r16(R69 *r, u16 o) { return r->mmio ? *(volatile u16 *)(r->mmio + o) : inw((u16)(r->io + o)); }
static inline u32 r32(R69 *r, u16 o) { return r->mmio ? *(volatile u32 *)(r->mmio + o) : inl((u16)(r->io + o)); }
static inline void w8(R69 *r, u16 o, u8 v)   { if (r->mmio) *(volatile u8 *)(r->mmio + o) = v; else outb((u16)(r->io + o), v); }
static inline void w16(R69 *r, u16 o, u16 v) { if (r->mmio) *(volatile u16 *)(r->mmio + o) = v; else outw((u16)(r->io + o), v); }
static inline void w32(R69 *r, u16 o, u32 v) { if (r->mmio) *(volatile u32 *)(r->mmio + o) = v; else outl((u16)(r->io + o), v); }

/* The PHY sits behind PHYAR: write = bit 31 set with register and data, then bit 31 clears when done; read = register number,
 * then bit 31 sets when the data is valid. */
static int phy_read(R69 *r, u8 reg, u16 *val) {
    w32(r, R_PHYAR, (u32)reg << 16);
    for (int i = 0; i < 200; i++) {
        udelay(100);
        u32 v = r32(r, R_PHYAR);
        if (v & 0x80000000u) { *val = (u16)v; return 0; }
    }
    return -1;
}

static int phy_write(R69 *r, u8 reg, u16 val) {
    w32(r, R_PHYAR, 0x80000000u | (u32)reg << 16 | val);
    for (int i = 0; i < 200; i++) {
        udelay(100);
        if (!(r32(r, R_PHYAR) & 0x80000000u)) return 0;
    }
    return -1;
}

/* ---- netdev ---- */

static void tx_reclaim(R69 *r) {
    while (r->tx_head != r->tx_tail && !(r->tx[r->tx_head].opts1 & DESC_OWN)) r->tx_head = (u16)((r->tx_head + 1) % NTX);
}

static int r_tx(NetDev *d, NetBuf *nb) {
    R69 *r = d->priv;
    tx_reclaim(r);
    u16 next = (u16)((r->tx_tail + 1) % NTX);
    if (next == r->tx_head || nb->len > BUFSZ || nb->len < 14) { d->tx_dropped++; nb_free(nb); return -ENOBUFS; }
    u16 i = r->tx_tail;
    memcpy(r->txbuf + (size_t)i * BUFSZ, nb_data(nb), nb->len);
    r->tx[i].addr = r->txbuf_phys + (u64)i * BUFSZ;
    r->tx[i].opts2 = 0;
    barrier();
    r->tx[i].opts1 = DESC_OWN | DESC_FIRST | DESC_LAST | (i == NTX - 1 ? DESC_END : 0) | nb->len;      /* the card owns it from this write on */
    barrier();
    r->tx_tail = next;
    w8(r, R_TXPOLL, TXPOLL_NPQ);
    d->tx_packets++;
    d->tx_bytes += nb->len;
    nb_free(nb);
    return 0;
}

static void r_poll(NetDev *d) {
    R69 *r = d->priv;
    int budget = 64;
    while (budget-- > 0) {
        Desc *ds = &r->rx[r->rx_next];
        u32 o1 = ds->opts1;
        if (o1 & DESC_OWN) break;                                // still the card's
        barrier();
        u32 len = o1 & 0x3FFF;                                   // includes the 4-byte CRC
        bool ok = (o1 & DESC_FIRST) && (o1 & DESC_LAST) && !(o1 & RX_RES) && len >= 14 + 4 && len <= BUFSZ;
        if (ok) {
            NetBuf *nb = nb_alloc();
            size_t flen = len - 4;
            if (nb && flen <= nb_room(nb)) {
                memcpy(nb_put(nb, flen), r->rxbuf + (size_t)r->rx_next * BUFSZ, flen);
                nb->dev = d;
                ds->opts2 = 0;
                barrier();
                ds->opts1 = DESC_OWN | (r->rx_next == NRX - 1 ? DESC_END : 0) | BUFSZ;
                r->rx_next = (u16)((r->rx_next + 1) % NRX);
                net_rx(d, nb);
                continue;
            }
            nb_free(nb);
            d->rx_dropped++;
        } else {
            d->rx_errors++;
        }
        ds->opts2 = 0;
        barrier();
        ds->opts1 = DESC_OWN | (r->rx_next == NRX - 1 ? DESC_END : 0) | BUFSZ;
        r->rx_next = (u16)((r->rx_next + 1) % NRX);
    }
    tx_reclaim(r);
    net_set_link(d, (r8(r, R_PHYSTAT) & PHYSTAT_LINK) != 0);
}

/* ---- probing ---- */

static void r_probe(PciDev *p) {
    R69 *r = kzalloc(sizeof *r);
    if (!r) return;
    r->pci = p;
    pci_enable(p, PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_MASTER);
    for (int b = 0; b < 6 && !r->mmio; b++)                      // prefer the memory BAR (registers 256 bytes or more)
        if (!p->bar[b].io && p->bar[b].size >= 256) r->mmio = pci_map_bar(p, b);
    if (!r->mmio) {
        for (int b = 0; b < 6; b++) if (p->bar[b].io && p->bar[b].size >= 128) { r->io = (u16)p->bar[b].base; break; }
        if (!r->io) goto fail;
    }
    if (r32(r, R_TXCONFIG) == 0xFFFFFFFFu) goto fail;            // the registers do not answer
    u32 xid = (r32(r, R_TXCONFIG) >> 20) & 0x7CF;                // chip revision code, for the log only

    w8(r, R_CHIPCMD, CMD_RESET);
    int i;
    for (i = 0; i < 1000 && (r8(r, R_CHIPCMD) & CMD_RESET); i++) udelay(100);
    if (i == 1000) goto fail;

    u8 mac[6];
    for (int k = 0; k < 6; k++) mac[k] = r8(r, (u16)(R_MAC0 + k));
    if ((mac[0] & 1) || !(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) || (mac[0] & mac[1] & mac[2] & mac[3] & mac[4] & mac[5]) == 0xFF) goto fail;

    void *ring = dma_alloc(4096, &r->rx_phys);                   // 64 + 32 descriptors of 16 bytes fit in one page; 256-byte aligned
    r->rxbuf = dma_alloc((size_t)NRX * BUFSZ, &r->rxbuf_phys);
    r->txbuf = dma_alloc((size_t)NTX * BUFSZ, &r->txbuf_phys);
    if (!ring || !r->rxbuf || !r->txbuf) goto fail;
    r->rx = ring;
    r->tx = (Desc *)((u8 *)ring + 2048);
    r->tx_phys = r->rx_phys + 2048;
    for (int k = 0; k < NRX; k++) {
        r->rx[k].addr = r->rxbuf_phys + (u64)k * BUFSZ;
        r->rx[k].opts1 = DESC_OWN | (k == NRX - 1 ? DESC_END : 0) | BUFSZ;
    }
    for (int k = 0; k < NTX; k++) r->tx[k].addr = r->txbuf_phys + (u64)k * BUFSZ;

    w8(r, R_CFG9346, CFG9346_UNLOCK);
    w16(r, R_INTRMASK, 0);                                       // no interrupts; the Yukari thread polls
    w16(r, R_INTRSTAT, 0xFFFF);
    w16(r, R_CPLUSCMD, r16(r, R_CPLUSCMD) & (u16) ~((1u << 5) | (1u << 6)));     // receive checksum offload and VLAN stripping off
    w16(r, R_RXMAXSIZE, 1600);
    w8(r, R_MAXTXPKT, 0x3F);
    w32(r, R_TXDESC_HI, (u32)(r->tx_phys >> 32));
    w32(r, R_TXDESC_LO, (u32)r->tx_phys);
    w32(r, R_RXDESC_HI, (u32)(r->rx_phys >> 32));
    w32(r, R_RXDESC_LO, (u32)r->rx_phys);
    w32(r, R_TXCONFIG, (7u << 8) | (3u << 24));                  // 2048-byte DMA bursts, normal inter-frame gap
    w32(r, R_RXCONFIG, (7u << 13) | (7u << 8) | 0x0E);           // no FIFO threshold, unlimited bursts; broadcast, multicast, our address
    w8(r, R_CHIPCMD, CMD_RXEN | CMD_TXEN);
    w32(r, R_TXCONFIG, (7u << 8) | (3u << 24));                  // older revisions only take these once the units are enabled
    w32(r, R_RXCONFIG, (7u << 13) | (7u << 8) | 0x0E);
    w8(r, R_CFG9346, CFG9346_LOCK);

    u16 bmcr;                                                    // power the PHY up if something switched it off (BMCR bit 11)
    if (phy_write(r, 0x1F, 0) == 0 && phy_read(r, 0, &bmcr) == 0 && (bmcr & 0x0800)) {
        phy_write(r, 0, (u16)((bmcr & ~0x0800) | 0x1200));
        klog("Yukari", "Realtek r8169 at %02x:%02x.%x: the PHY was powered down; switched it on", p->bus, p->dev, p->fn);
    }

    NetDev *d = kzalloc(sizeof *d);
    if (!d) goto fail;
    memcpy(d->mac, mac, 6);
    snprintf(d->driver, sizeof d->driver, "r8169");
    d->mtu = 1500;
    d->flags = NDF_BROADCAST;
    if (r8(r, R_PHYSTAT) & PHYSTAT_LINK) d->flags |= NDF_LINK;
    d->tx = r_tx;
    d->poll = r_poll;
    d->priv = r;
    p->driver = "r8169";
    p->driver_data = d;
    netdev_register("eth", d);
    klog("Yukari", "%s: Realtek %04x (revision code %03x) at %02x:%02x.%x, MAC %02x:%02x:%02x:%02x:%02x:%02x", d->name, p->device, xid, p->bus, p->dev,
         p->fn, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return;
fail:
    klog("Yukari", "Realtek %04x at %02x:%02x.%x did not answer sensibly; leaving it alone", p->device, p->bus, p->dev, p->fn);
    if (r->rx) dma_free(r->rx, 4096);
    if (r->rxbuf) dma_free(r->rxbuf, (size_t)NRX * BUFSZ);
    if (r->txbuf) dma_free(r->txbuf, (size_t)NTX * BUFSZ);
    kfree(r);
}

static void r8169_init(void) {
    static const u16 ids[] = { 0x8168, 0x8169, 0x8167, 0x8161 };       // RTL8111/8168/8411, RTL8169, RTL8110, RTL8111 variant
    for (unsigned i = 0; i < ARRAY_SIZE(ids); i++)
        for (PciDev *p = pci_find_id(RTL_VENDOR, ids[i], NULL); p; p = pci_find_id(RTL_VENDOR, ids[i], p))
            if (!p->driver && p->class_code == 2) r_probe(p);
}
INITCALL(r8169_init);
