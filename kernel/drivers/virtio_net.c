/*
 * virtio-net: the paravirtual NIC of QEMU and friends. Supports the legacy (I/O port, device 0x1000) and the modern
 * (PCI capabilities, device 0x1041) transports; polled from the Yukari thread, no interrupts. One receive queue, one transmit
 * queue, no offloads: the 12/10-byte virtio_net_hdr in front of every frame is all zeroes.
 */
#include "net.h"
#include "pci.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "initcall.h"
#include "irq.h"

#define VIRTIO_VENDOR       0x1AF4
#define VIRTIO_NET_LEGACY   0x1000
#define VIRTIO_NET_MODERN   0x1041

#define ST_ACK        1
#define ST_DRIVER     2
#define ST_DRIVER_OK  4
#define ST_FEATURES_OK 8
#define ST_FAILED     0x80

#define F_NET_MAC     (1u << 5)
#define F_NET_STATUS  (1u << 16)

#define VRING_DESC_F_WRITE 2

typedef struct { u64 addr; u32 len; u16 flags; u16 next; } PACKED VDesc;

typedef struct VQ {
    u16 size;
    VDesc *desc;
    volatile u16 *avail_flags, *avail_idx, *avail_ring;
    volatile u16 *used_flags, *used_idx;
    volatile u32 *used_ring;            /* pairs of (id, len) */
    u16 last_used;
    u16 avail_shadow;
    paddr_t phys;
    void *mem;
    size_t bytes;
} VQ;

#define RX_BUFS   128
#define TX_SLOTS  64
#define BUF_SIZE  2048

typedef struct VNet {
    PciDev *pci;
    bool modern;
    u16 io;                             /* legacy: I/O base */
    volatile u8 *common, *notify, *isr, *devcfg;
    u32 notify_mult;
    u16 noff[2];                        /* modern: queue_notify_off of the receive and transmit queue */
    VQ rx, tx;
    u8 *rxbuf, *txbuf;
    paddr_t rxbuf_phys, txbuf_phys;
    u8  tx_busy[TX_SLOTS];
    size_t hdr_len;
    bool has_status;
    int  tx_inflight;
} VNet;

static size_t vq_bytes(u16 n) {
    size_t a = ALIGN_UP((size_t)16 * n + 6 + 2 * (size_t)n, 4096);
    size_t b = ALIGN_UP((size_t)6 + 8 * (size_t)n, 4096);
    return a + b;
}

static bool vq_setup(VQ *q, u16 n) {
    q->size = n;
    q->bytes = vq_bytes(n);
    q->mem = dma_alloc(q->bytes, &q->phys);
    if (!q->mem) return false;
    u8 *base = q->mem;
    q->desc = (VDesc *)base;
    size_t avail_off = (size_t)16 * n;
    q->avail_flags = (volatile u16 *)(base + avail_off);
    q->avail_idx = (volatile u16 *)(base + avail_off + 2);
    q->avail_ring = (volatile u16 *)(base + avail_off + 4);
    size_t used_off = ALIGN_UP((size_t)16 * n + 6 + 2 * (size_t)n, 4096);
    q->used_flags = (volatile u16 *)(base + used_off);
    q->used_idx = (volatile u16 *)(base + used_off + 2);
    q->used_ring = (volatile u32 *)(base + used_off + 4);
    q->last_used = 0;
    q->avail_shadow = 0;
    return true;
}

/* ---- transport access ---- */

static void st_write(VNet *v, u8 s) { if (v->modern) v->common[0x14] = s; else outb((u16)(v->io + 0x12), s); }
static u8   st_read(VNet *v) { return v->modern ? v->common[0x14] : inb((u16)(v->io + 0x12)); }

static void kick(VNet *v, int q) {
    if (v->modern) {
        *(volatile u16 *)(v->notify + (size_t)v->noff[q] * v->notify_mult) = (u16)q;
    } else {
        outw((u16)(v->io + 0x10), (u16)q);
    }
}

static void rd_mac(VNet *v, u8 *mac) {
    for (int i = 0; i < 6; i++) mac[i] = v->modern ? v->devcfg[i] : inb((u16)(v->io + 0x14 + i));
}

static u16 rd_link_status(VNet *v) {
    return v->modern ? *(volatile u16 *)(v->devcfg + 6) : inw((u16)(v->io + 0x14 + 6));
}

static void post_rx(VNet *v, u16 idx) {
    VQ *q = &v->rx;
    q->desc[idx].addr = v->rxbuf_phys + (u64)idx * BUF_SIZE;
    q->desc[idx].len = BUF_SIZE;
    q->desc[idx].flags = VRING_DESC_F_WRITE;
    q->desc[idx].next = 0;
    q->avail_ring[q->avail_shadow % q->size] = idx;
    barrier();
    q->avail_shadow++;
    *q->avail_idx = q->avail_shadow;
}

/* ---- netdev ---- */

static int vnet_tx(NetDev *d, NetBuf *nb) {
    VNet *v = d->priv;
    VQ *q = &v->tx;
    /* reclaim what the device has finished with */
    while (q->last_used != *q->used_idx) {
        u32 id = q->used_ring[2 * (q->last_used % q->size)];
        if (id < TX_SLOTS && v->tx_busy[id]) { v->tx_busy[id] = 0; v->tx_inflight--; }
        q->last_used++;
    }
    if (nb->len > BUF_SIZE - 16) { d->tx_errors++; nb_free(nb); return -EMSGSIZE; }
    int slot = -1;
    for (int i = 0; i < TX_SLOTS; i++) if (!v->tx_busy[i]) { slot = i; break; }
    if (slot < 0) { d->tx_dropped++; nb_free(nb); return -ENOBUFS; }
    u8 *buf = v->txbuf + (size_t)slot * BUF_SIZE;
    memset(buf, 0, v->hdr_len);
    memcpy(buf + v->hdr_len, nb_data(nb), nb->len);
    q->desc[slot].addr = v->txbuf_phys + (u64)slot * BUF_SIZE;
    q->desc[slot].len = (u32)(v->hdr_len + nb->len);
    q->desc[slot].flags = 0;
    q->desc[slot].next = 0;
    v->tx_busy[slot] = 1;
    v->tx_inflight++;
    q->avail_ring[q->avail_shadow % q->size] = (u16)slot;
    barrier();
    q->avail_shadow++;
    *q->avail_idx = q->avail_shadow;
    barrier();
    kick(v, 1);
    d->tx_packets++;
    d->tx_bytes += nb->len;
    nb_free(nb);
    return 0;
}

static void vnet_poll(NetDev *d) {
    VNet *v = d->priv;
    VQ *q = &v->rx;
    int budget = 64;
    bool posted = false;
    while (q->last_used != *q->used_idx && budget-- > 0) {
        barrier();
        u32 id = q->used_ring[2 * (q->last_used % q->size)];
        u32 len = q->used_ring[2 * (q->last_used % q->size) + 1];
        q->last_used++;
        if (id >= RX_BUFS) continue;
        if (len > v->hdr_len && len <= BUF_SIZE) {
            NetBuf *nb = nb_alloc();
            size_t flen = len - v->hdr_len;
            if (nb && flen <= nb_room(nb)) {
                memcpy(nb_put(nb, flen), v->rxbuf + (size_t)id * BUF_SIZE + v->hdr_len, flen);
                nb->dev = d;
                post_rx(v, (u16)id);
                posted = true;
                net_rx(d, nb);
                continue;
            }
            nb_free(nb);
            d->rx_dropped++;
        } else {
            d->rx_errors++;
        }
        post_rx(v, (u16)id);
        posted = true;
    }
    if (posted) kick(v, 0);
    if (v->has_status) net_set_link(d, (rd_link_status(v) & 1) != 0);
}

/* ---- probing ---- */

static bool find_caps(VNet *v) {
    PciDev *p = v->pci;
    u8 ptr = pci_read8(p, 0x34) & 0xFC;
    bool have_common = false, have_notify = false, have_dev = false;
    for (int guard = 0; ptr && guard < 48; guard++) {
        u8 id = pci_read8(p, ptr);
        u8 next = pci_read8(p, ptr + 1) & 0xFC;
        if (id == PCI_CAP_VENDOR) {
            u8 type = pci_read8(p, ptr + 3), bar = pci_read8(p, ptr + 4);
            u32 off = pci_read32(p, ptr + 8), len = pci_read32(p, ptr + 12);
            if (bar < 6 && !p->bar[bar].io) {
                u8 *base = pci_map_bar(p, bar);
                if (base && (u64)off + len <= p->bar[bar].size) {
                    if (type == 1) { v->common = base + off; have_common = true; }
                    else if (type == 2) { v->notify = base + off; v->notify_mult = pci_read32(p, ptr + 16); have_notify = true; }
                    else if (type == 3) v->isr = base + off;
                    else if (type == 4) { v->devcfg = base + off; have_dev = true; }
                }
            }
        }
        ptr = next;
    }
    return have_common && have_notify && have_dev;
}

static void vnet_probe(PciDev *p) {
    VNet *v = kzalloc(sizeof *v);
    if (!v) return;
    v->pci = p;
    pci_enable(p, PCI_CMD_MEM | PCI_CMD_IO | PCI_CMD_MASTER);
    v->modern = p->device == VIRTIO_NET_MODERN || (p->revision >= 1 && find_caps(v));
    if (v->modern && !v->common && !find_caps(v)) goto fail;
    if (!v->modern) {
        if (!p->bar[0].io) goto fail;
        v->io = (u16)p->bar[0].base;
    }
    /* reset, then the handshake */
    st_write(v, 0);
    for (int i = 0; i < 1000 && st_read(v) != 0; i++) udelay(100);
    st_write(v, ST_ACK);
    st_write(v, ST_ACK | ST_DRIVER);
    u32 feat_lo, feat_hi = 0;
    if (v->modern) {
        *(volatile u32 *)(v->common + 0x00) = 0; barrier(); feat_lo = *(volatile u32 *)(v->common + 0x04);
        *(volatile u32 *)(v->common + 0x00) = 1; barrier(); feat_hi = *(volatile u32 *)(v->common + 0x04);
        if (!(feat_hi & 1)) goto fail;                           /* no VIRTIO_F_VERSION_1: not a modern device after all */
    } else {
        feat_lo = inl(v->io + 0x00);
    }
    u32 want_lo = feat_lo & (F_NET_MAC | F_NET_STATUS);
    v->has_status = (want_lo & F_NET_STATUS) != 0;
    if (v->modern) {
        *(volatile u32 *)(v->common + 0x08) = 0; *(volatile u32 *)(v->common + 0x0C) = want_lo;
        *(volatile u32 *)(v->common + 0x08) = 1; *(volatile u32 *)(v->common + 0x0C) = 1;     /* VERSION_1 */
        st_write(v, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
        if (!(st_read(v) & ST_FEATURES_OK)) goto fail;
    } else {
        outl(v->io + 0x04, want_lo);
    }
    v->hdr_len = v->modern ? 12 : 10;

    /* queues */
    for (int qi = 0; qi < 2; qi++) {
        VQ *q = qi == 0 ? &v->rx : &v->tx;
        u16 n;
        if (v->modern) {
            *(volatile u16 *)(v->common + 0x16) = (u16)qi;
            barrier();
            n = *(volatile u16 *)(v->common + 0x18);
        } else {
            outw(v->io + 0x0E, (u16)qi);
            n = inw(v->io + 0x0C);
        }
        if (n == 0 || n > 1024 || (n & (n - 1))) goto fail;
        if (n < (qi == 0 ? RX_BUFS : TX_SLOTS)) goto fail;
        if (!vq_setup(q, n)) goto fail;
        if (v->modern) {
            u64 desc = q->phys, avail = q->phys + (u64)16 * n;
            u64 used = q->phys + ALIGN_UP((size_t)16 * n + 6 + 2 * (size_t)n, 4096);
            *(volatile u16 *)(v->common + 0x18) = n;
            *(volatile u64 *)(v->common + 0x20) = desc;
            *(volatile u64 *)(v->common + 0x28) = avail;
            *(volatile u64 *)(v->common + 0x30) = used;
            u16 off = *(volatile u16 *)(v->common + 0x1E);
            v->noff[qi] = off;
            *(volatile u16 *)(v->common + 0x1C) = 1;
        } else {
            outl(v->io + 0x08, (u32)(q->phys >> 12));
        }
    }
    v->rxbuf = dma_alloc((size_t)RX_BUFS * BUF_SIZE, &v->rxbuf_phys);
    v->txbuf = dma_alloc((size_t)TX_SLOTS * BUF_SIZE, &v->txbuf_phys);
    if (!v->rxbuf || !v->txbuf) goto fail;

    NetDev *d = kzalloc(sizeof *d);
    if (!d) goto fail;
    strlcpy(d->driver, v->modern ? "virtio-net" : "virtio-net(l)", sizeof d->driver);
    if (want_lo & F_NET_MAC) rd_mac(v, d->mac);
    else { d->mac[0] = 0x52; d->mac[1] = 0x54; d->mac[2] = 0x00; d->mac[3] = 0x12; d->mac[4] = 0x34; d->mac[5] = 0x56; }
    d->mtu = 1500;
    d->flags = NDF_BROADCAST;
    d->tx = vnet_tx;
    d->poll = vnet_poll;
    d->priv = v;
    for (u16 i = 0; i < RX_BUFS; i++) post_rx(v, i);
    st_write(v, ST_ACK | ST_DRIVER | (v->modern ? ST_FEATURES_OK : 0) | ST_DRIVER_OK);
    kick(v, 0);
    p->driver = "virtio-net";
    p->driver_data = d;
    if (!v->has_status || (rd_link_status(v) & 1)) d->flags |= NDF_LINK;
    netdev_register("eth", d);
    klog("Yukari", "%s: %s at %02x:%02x.%x, MAC %02x:%02x:%02x:%02x:%02x:%02x", d->name, d->driver, p->bus, p->dev, p->fn,
         d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
    return;
fail:
    klog("Yukari", "virtio-net at %02x:%02x.%x could not be set up; leaving it alone", p->bus, p->dev, p->fn);
    if (v->common || v->io) st_write(v, ST_FAILED);
    kfree(v);
}

static void virtio_net_init(void) {
    for (PciDev *p = pci_find_id(VIRTIO_VENDOR, 0xFFFF, NULL); p; p = pci_find_id(VIRTIO_VENDOR, 0xFFFF, p)) {
        if (p->driver) continue;
        if (p->device == VIRTIO_NET_LEGACY || p->device == VIRTIO_NET_MODERN) vnet_probe(p);
    }
}
INITCALL(virtio_net_init);
