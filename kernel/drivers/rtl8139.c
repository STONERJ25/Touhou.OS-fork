/*
 * Realtek RTL8139 ("Fast Ethernet", QEMU's `-device rtl8139`): one 8 KiB receive ring that the card fills with
 * [status, length, frame] records, four transmit slots, programmed through I/O ports and polled from the Yukari thread.
 */
#include "net.h"
#include "pci.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "irq.h"
#include "initcall.h"

#define RTL_VENDOR 0x10EC
#define RTL_8139   0x8139

#define R_IDR0   0x00
#define R_TSD0   0x10
#define R_TSAD0  0x20
#define R_RBSTART 0x30
#define R_CR     0x37
#define R_CAPR   0x38
#define R_IMR    0x3C
#define R_ISR    0x3E
#define R_TCR    0x40
#define R_RCR    0x44
#define R_CONFIG1 0x52
#define R_MSR    0x58

#define CR_BUFE  0x01
#define CR_TE    0x04
#define CR_RE    0x08
#define CR_RST   0x10
#define TSD_OWN  (1u << 13)
#define TSD_TOK  (1u << 15)
#define RX_OK    0x0001

#define RX_RING  8192
#define RX_EXTRA (16 + 1600)            /* the card writes a frame that straddles the end of the ring past it (WRAP mode) */
#define TX_BUF   2048

typedef struct R8139 {
    PciDev *pci;
    u16 io;
    u8 *rx;
    paddr_t rx_phys;
    u8 *txbuf;
    paddr_t tx_phys;
    u32 rx_off;                         /* where the next record starts in the ring */
    u8  cur_tx;                         /* next transmit slot */
    bool tx_busy[4];
} R8139;

static inline u8  in8(R8139 *r, u16 reg) { return inb((u16)(r->io + reg)); }
static inline u16 in16(R8139 *r, u16 reg) { return inw((u16)(r->io + reg)); }
static inline u32 in32(R8139 *r, u16 reg) { return inl((u16)(r->io + reg)); }
static inline void out8(R8139 *r, u16 reg, u8 v) { outb((u16)(r->io + reg), v); }
static inline void out16(R8139 *r, u16 reg, u16 v) { outw((u16)(r->io + reg), v); }
static inline void out32(R8139 *r, u16 reg, u32 v) { outl((u16)(r->io + reg), v); }

static int r_tx(NetDev *d, NetBuf *nb) {
    R8139 *r = d->priv;
    u8 slot = r->cur_tx;
    if (r->tx_busy[slot]) {                                      /* the slot we want is still in flight: has the card finished with it? */
        u32 st = in32(r, (u16)(R_TSD0 + 4 * slot));
        if (!(st & TSD_OWN)) { d->tx_dropped++; nb_free(nb); return -ENOBUFS; }
        r->tx_busy[slot] = false;
        if (!(st & TSD_TOK)) d->tx_errors++;
    }
    if (nb->len > TX_BUF || nb->len < 14) { d->tx_errors++; nb_free(nb); return -EMSGSIZE; }
    memcpy(r->txbuf + (size_t)slot * TX_BUF, nb_data(nb), nb->len);
    barrier();
    out32(r, (u16)(R_TSAD0 + 4 * slot), (u32)(r->tx_phys + (u64)slot * TX_BUF));
    out32(r, (u16)(R_TSD0 + 4 * slot), nb->len & 0x1FFF);       /* length with OWN clear starts the transmission */
    r->tx_busy[slot] = true;
    r->cur_tx = (u8)((slot + 1) & 3);
    d->tx_packets++;
    d->tx_bytes += nb->len;
    nb_free(nb);
    return 0;
}

static void r_poll(NetDev *d) {
    R8139 *r = d->priv;
    int budget = 64;
    while (budget-- > 0 && !(in8(r, R_CR) & CR_BUFE)) {
        u8 *rec = r->rx + r->rx_off;
        u16 status = (u16)(rec[0] | rec[1] << 8);
        u16 len = (u16)(rec[2] | rec[3] << 8);                   /* includes the 4-byte CRC */
        if (len < 4 + 14 || len > 1600 + 4 || !(status & RX_OK)) {
            d->rx_errors++;                                      /* the ring is out of step: restart the receiver rather than guess */
            out8(r, R_CR, CR_TE);
            r->rx_off = 0;
            out16(r, R_CAPR, (u16)(0 - 16));
            out8(r, R_CR, CR_TE | CR_RE);
            break;
        }
        size_t flen = (size_t)len - 4;
        NetBuf *nb = nb_alloc();
        if (nb && flen <= nb_room(nb)) {
            memcpy(nb_put(nb, flen), rec + 4, flen);
            nb->dev = d;
        } else {
            nb_free(nb);
            nb = NULL;
            d->rx_dropped++;
        }
        r->rx_off = (r->rx_off + len + 4 + 3) & ~3u;
        r->rx_off %= RX_RING;
        out16(r, R_CAPR, (u16)(r->rx_off - 16));
        if (nb) net_rx(d, nb);
    }
    net_set_link(d, !(in8(r, R_MSR) & 0x04));                    /* MSR bit 2 is the inverse of the link state */
}

static void r_probe(PciDev *p) {
    R8139 *r = kzalloc(sizeof *r);
    if (!r) return;
    r->pci = p;
    pci_enable(p, PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_MASTER);
    if (!p->bar[0].io || !p->bar[0].base) goto fail;
    r->io = (u16)p->bar[0].base;
    out8(r, R_CONFIG1, 0x00);                                    /* power on */
    out8(r, R_CR, CR_RST);
    for (int i = 0; i < 1000 && (in8(r, R_CR) & CR_RST); i++) udelay(100);
    if (in8(r, R_CR) & CR_RST) goto fail;
    r->rx = dma_alloc(RX_RING + RX_EXTRA, &r->rx_phys);
    r->txbuf = dma_alloc(4 * TX_BUF, &r->tx_phys);
    if (!r->rx || !r->txbuf) goto fail;
    NetDev *d = kzalloc(sizeof *d);
    if (!d) goto fail;
    for (int i = 0; i < 6; i++) d->mac[i] = in8(r, (u16)(R_IDR0 + i));
    if ((d->mac[0] & 1) || !(d->mac[0] | d->mac[1] | d->mac[2] | d->mac[3] | d->mac[4] | d->mac[5])) { kfree(d); goto fail; }
    out32(r, R_RBSTART, (u32)r->rx_phys);
    out16(r, R_IMR, 0);                                          /* no interrupts: the Yukari thread polls */
    out16(r, R_ISR, 0xFFFF);
    out32(r, R_TCR, 0x03000600);                                 /* normal inter-frame gap, 1024-byte DMA bursts */
    out8(r, R_CR, CR_TE | CR_RE);
    out32(r, R_RCR, 0xE78E);                                     /* broadcast, multicast and our address; WRAP; no thresholds */
    r->rx_off = 0;
    snprintf(d->driver, sizeof d->driver, "rtl8139");
    d->mtu = 1500;
    d->flags = NDF_BROADCAST;
    if (!(in8(r, R_MSR) & 0x04)) d->flags |= NDF_LINK;
    d->tx = r_tx;
    d->poll = r_poll;
    d->priv = r;
    p->driver = "rtl8139";
    p->driver_data = d;
    netdev_register("eth", d);
    klog("Yukari", "%s: Realtek RTL8139 at %02x:%02x.%x, MAC %02x:%02x:%02x:%02x:%02x:%02x", d->name, p->bus, p->dev, p->fn,
         d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
    return;
fail:
    klog("Yukari", "RTL8139 at %02x:%02x.%x did not answer sensibly; leaving it alone", p->bus, p->dev, p->fn);
    if (r->rx) dma_free(r->rx, RX_RING + RX_EXTRA);
    if (r->txbuf) dma_free(r->txbuf, 4 * TX_BUF);
    kfree(r);
}

static void rtl8139_init(void) {
    if (net_cards_disabled()) return;
    for (PciDev *p = pci_find_id(RTL_VENDOR, RTL_8139, NULL); p; p = pci_find_id(RTL_VENDOR, RTL_8139, p))
        if (!p->driver) r_probe(p);
}
INITCALL(rtl8139_init);
