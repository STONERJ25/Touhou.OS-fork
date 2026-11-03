/*
 * virtio-blk over PCI (Nitori's courier between the guest and the host disk).
 *
 * Both transports are supported: the legacy I/O-port interface (transitional devices, PCI id 1af4:1001) and the modern
 * capability-based MMIO layout (virtio 1.0, id 1af4:1042; transitional devices expose it too and we prefer it).
 * One virtqueue, one request in flight, completion by polling (no interrupts, see docs/ARCHITECTURE.md). Every wait has
 * a timeout; a device that stops answering is reset (so it can no longer DMA into freed buffers) and marked dead.
 *
 * The device is a block device with 512-byte or blk_size-byte logical sectors. The wire protocol counts 512-byte units
 * regardless, so the LBA is scaled when blk_size is larger.
 */
#include "blk.h"
#include "pci.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "irq.h"
#include "task.h"

#define VIRTIO_VENDOR      0x1AF4
#define VIRTIO_BLK_LEGACY  0x1001
#define VIRTIO_BLK_MODERN  0x1042

/* device status bits */
#define VS_ACK          0x01
#define VS_DRIVER       0x02
#define VS_DRIVER_OK    0x04
#define VS_FEATURES_OK  0x08
#define VS_NEEDS_RESET  0x40
#define VS_FAILED       0x80

/* feature bits (device-specific part) */
#define VBLK_F_SIZE_MAX  (1u << 1)
#define VBLK_F_SEG_MAX   (1u << 2)
#define VBLK_F_RO        (1u << 5)
#define VBLK_F_BLK_SIZE  (1u << 6)
#define VBLK_F_FLUSH     (1u << 9)
/* transport features */
#define VIRTIO_F_VERSION_1_HI (1u << 0)         /* bit 32 = bit 0 of the upper word */

/* request types and status */
#define VBLK_T_IN     0
#define VBLK_T_OUT    1
#define VBLK_T_FLUSH  4
#define VBLK_S_OK     0
#define VBLK_S_IOERR  1
#define VBLK_S_UNSUPP 2

/* descriptor flags */
#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2
#define VRING_AVAIL_F_NO_INTERRUPT 1

/* legacy register offsets (I/O BAR 0) */
#define LG_DEV_FEATURES 0x00
#define LG_DRV_FEATURES 0x04
#define LG_QUEUE_PFN    0x08
#define LG_QUEUE_SIZE   0x0C
#define LG_QUEUE_SEL    0x0E
#define LG_QUEUE_NOTIFY 0x10
#define LG_STATUS       0x12
#define LG_ISR          0x13
#define LG_CONFIG       0x14

/* modern common configuration structure offsets */
#define CM_DEV_FEATURE_SEL 0x00
#define CM_DEV_FEATURE     0x04
#define CM_DRV_FEATURE_SEL 0x08
#define CM_DRV_FEATURE     0x0C
#define CM_NUM_QUEUES      0x12
#define CM_STATUS          0x14
#define CM_CONFIG_GEN      0x15
#define CM_QUEUE_SEL       0x16
#define CM_QUEUE_SIZE      0x18
#define CM_QUEUE_ENABLE    0x1C
#define CM_QUEUE_NOTIFY_OFF 0x1E
#define CM_QUEUE_DESC      0x20
#define CM_QUEUE_DRIVER    0x28
#define CM_QUEUE_DEVICE    0x30

/* virtio PCI capability cfg_type values */
#define VCAP_COMMON 1
#define VCAP_NOTIFY 2
#define VCAP_ISR    3
#define VCAP_DEVICE 4

#define MAX_QUEUE       256
#define MAX_REQ_BYTES   (128 * 1024)
#define MAX_SEGS        32
#define IO_TIMEOUT_MS   10000

typedef struct { u64 addr; u32 len; u16 flags; u16 next; } PACKED VringDesc;
typedef struct { u32 id; u32 len; } PACKED VringUsedElem;
typedef struct { u32 type; u32 reserved; u64 sector; } PACKED VblkHeader;

typedef struct Seg { paddr_t phys; u32 len; } Seg;

typedef struct VDisk {
    PciDev *pci;
    bool modern, dead;
    u16 io;                             /* legacy: I/O port base */
    volatile u8 *common, *isr, *notify_base, *devcfg;     /* modern: mapped structures */
    u32 notify_mult;
    u16 notify_off;
    u16 qsize;
    void *qmem;
    size_t qbytes;
    paddr_t qphys;
    volatile VringDesc *desc;
    volatile u16 *avail_flags, *avail_idx, *avail_ring;
    volatile u16 *used_idx;
    volatile VringUsedElem *used_ring;
    u16 last_used;
    u8 *req;                            /* header (16 bytes) + status byte, one DMA page */
    paddr_t req_phys;
    u32 seg_max, size_max;
    bool flush_ok;
    Mutex lock;
} VDisk;

/* ---------------------------------------------------------------------------------------------- register access */

static u8 rd8(VDisk *d, u32 off) {
    return d->modern ? *(volatile u8 *)(d->common + off) : inb((u16)(d->io + off));
}
static u16 rd16(VDisk *d, u32 off) {
    return d->modern ? *(volatile u16 *)(d->common + off) : inw((u16)(d->io + off));
}
static u32 rd32(VDisk *d, u32 off) {
    return d->modern ? *(volatile u32 *)(d->common + off) : inl((u16)(d->io + off));
}
static void wr8(VDisk *d, u32 off, u8 v) {
    if (d->modern) *(volatile u8 *)(d->common + off) = v; else outb((u16)(d->io + off), v);
}
static void wr16(VDisk *d, u32 off, u16 v) {
    if (d->modern) *(volatile u16 *)(d->common + off) = v; else outw((u16)(d->io + off), v);
}
static void wr32(VDisk *d, u32 off, u32 v) {
    if (d->modern) *(volatile u32 *)(d->common + off) = v; else outl((u16)(d->io + off), v);
}

static void wr64(VDisk *d, u32 off, u64 v) {            /* two 32-bit accesses: every device accepts these */
    wr32(d, off, (u32)v);
    wr32(d, off + 4, (u32)(v >> 32));
}

/* device-specific configuration (virtio_blk_config) */
static u32 cfg32(VDisk *d, u32 off) {
    return d->modern ? *(volatile u32 *)(d->devcfg + off) : inl((u16)(d->io + LG_CONFIG + off));
}

static u8 get_status(VDisk *d) { return rd8(d, d->modern ? CM_STATUS : LG_STATUS); }
static void set_status(VDisk *d, u8 v) { wr8(d, d->modern ? CM_STATUS : LG_STATUS, v); }

/* Resets the device and waits for it to acknowledge; after this it no longer touches guest memory. */
static bool device_reset(VDisk *d) {
    set_status(d, 0);
    u64 end = uptime_ms() + 1000;
    while (get_status(d) != 0) {
        if (uptime_ms() > end) return false;
        task_sleep_ms(1);
    }
    return true;
}

/* ---------------------------------------------------------------------------------------------- virtqueue */

static bool queue_setup(VDisk *d) {
    u16 max;
    if (d->modern) {
        wr16(d, CM_QUEUE_SEL, 0);
        max = rd16(d, CM_QUEUE_SIZE);
    } else {
        wr16(d, LG_QUEUE_SEL, 0);
        max = rd16(d, LG_QUEUE_SIZE);
    }
    if (max < 4 || (max & (max - 1))) return false;             /* split rings need a power-of-two size */
    u16 qs = max > MAX_QUEUE ? MAX_QUEUE : max;
    if (!d->modern && qs != max) return false;                  /* the legacy size is fixed by the device */

    size_t avail_off = (size_t)qs * sizeof(VringDesc);
    size_t used_off = ALIGN_UP(avail_off + 4 + 2 * (size_t)qs + 2, 4096);
    size_t total = used_off + 4 + 8 * (size_t)qs + 2;
    paddr_t phys;
    void *mem = dma_alloc(total, &phys);
    if (!mem) return false;
    d->qmem = mem;
    d->qbytes = total;
    d->qphys = phys;
    d->qsize = qs;
    d->desc = (volatile VringDesc *)mem;
    d->avail_flags = (volatile u16 *)((u8 *)mem + avail_off);
    d->avail_idx = d->avail_flags + 1;
    d->avail_ring = d->avail_flags + 2;
    d->used_idx = (volatile u16 *)((u8 *)mem + used_off + 2);
    d->used_ring = (volatile VringUsedElem *)((u8 *)mem + used_off + 4);
    d->last_used = 0;
    *d->avail_flags = VRING_AVAIL_F_NO_INTERRUPT;

    if (d->modern) {
        wr16(d, CM_QUEUE_SIZE, qs);
        wr64(d, CM_QUEUE_DESC, phys);
        wr64(d, CM_QUEUE_DRIVER, phys + avail_off);
        wr64(d, CM_QUEUE_DEVICE, phys + used_off);
        d->notify_off = rd16(d, CM_QUEUE_NOTIFY_OFF);
        wr16(d, CM_QUEUE_ENABLE, 1);
    } else {
        if ((phys >> 12) > 0xFFFFFFFFULL) return false;
        wr32(d, LG_QUEUE_PFN, (u32)(phys >> 12));
    }
    return true;
}

static void queue_notify(VDisk *d) {
    if (d->modern) *(volatile u16 *)(d->notify_base + (u64)d->notify_off * d->notify_mult) = 0;
    else outw((u16)(d->io + LG_QUEUE_NOTIFY), 0);
}

/* Submits one request (header, data segments, status) and polls for completion. Called with d->lock held. */
static int submit(VDisk *d, u32 type, u64 sector512, const Seg *segs, int nseg, bool dev_writes) {
    if (d->dead) return -EIO;
    if ((u32)nseg + 2 > d->qsize) return -EIO;
    VblkHeader *h = (VblkHeader *)d->req;
    h->type = type;
    h->reserved = 0;
    h->sector = sector512;
    volatile u8 *status = d->req + sizeof(VblkHeader);
    *status = 0xFF;

    volatile VringDesc *desc = d->desc;
    desc[0].addr = d->req_phys;
    desc[0].len = sizeof(VblkHeader);
    desc[0].flags = VRING_DESC_F_NEXT;
    desc[0].next = 1;
    for (int i = 0; i < nseg; i++) {
        desc[1 + i].addr = segs[i].phys;
        desc[1 + i].len = segs[i].len;
        desc[1 + i].flags = (u16)(VRING_DESC_F_NEXT | (dev_writes ? VRING_DESC_F_WRITE : 0));
        desc[1 + i].next = (u16)(2 + i);
    }
    desc[1 + nseg].addr = d->req_phys + sizeof(VblkHeader);
    desc[1 + nseg].len = 1;
    desc[1 + nseg].flags = VRING_DESC_F_WRITE;
    desc[1 + nseg].next = 0;

    u16 idx = *d->avail_idx;
    d->avail_ring[idx % d->qsize] = 0;                          /* head of the chain */
    barrier();
    *d->avail_idx = (u16)(idx + 1);
    barrier();
    queue_notify(d);

    u64 start = uptime_ms(), now = start;
    u32 spins = 0;
    while (*d->used_idx == d->last_used) {
        pause();
        if (++spins % 256 == 0) {
            now = uptime_ms();
            if (now - start > IO_TIMEOUT_MS) {
                klog("Kappa", "virtio-blk: request timed out, resetting the device");
                d->dead = true;
                device_reset(d);
                return -EIO;
            }
            if (now - start >= 2) task_sleep_ms(1);             /* long request: stop hogging the CPU */
        }
    }
    barrier();
    d->last_used++;
    u8 st = *status;
    if (st == VBLK_S_OK) return 0;
    return st == VBLK_S_UNSUPP ? -ENOSYS : -EIO;
}

/* Splits [buf, buf+len) into physically contiguous segments, honouring the device's size_max. */
static int build_segs(const void *buf, size_t len, Seg *segs, u32 size_max) {
    int n = 0;
    const u8 *p = buf;
    while (len) {
        size_t chunk = PAGE_SIZE - ((u64)p & (PAGE_SIZE - 1));
        if (chunk > len) chunk = len;
        paddr_t ph = virt_to_phys(p);
        if (n && segs[n - 1].phys + segs[n - 1].len == ph && (!size_max || segs[n - 1].len + chunk <= size_max)) {
            segs[n - 1].len += (u32)chunk;
        } else {
            if (n == MAX_SEGS) return -1;
            segs[n].phys = ph;
            segs[n].len = (u32)chunk;
            n++;
        }
        p += chunk;
        len -= chunk;
    }
    return n;
}

static int vd_rw(BlockDev *bd, bool write, u64 lba, u32 count, void *buf) {
    VDisk *d = bd->priv;
    u32 ss = bd->sector_size;
    int rc = 0;
    mutex_lock(&d->lock);
    u8 *p = buf;
    while (count && !rc) {
        u32 n = count;
        if ((u64)n * ss > MAX_REQ_BYTES) n = MAX_REQ_BYTES / ss;
        Seg segs[MAX_SEGS];
        int nseg;
        while ((nseg = build_segs(p, (size_t)n * ss, segs, d->size_max)) < 0 || nseg + 2 > d->qsize) {
            n /= 2;
            if (!n) { rc = -EIO; break; }
        }
        if (rc) break;
        rc = submit(d, write ? VBLK_T_OUT : VBLK_T_IN, lba * (ss / 512), segs, nseg, !write);
        lba += n;
        count -= n;
        p += (size_t)n * ss;
    }
    mutex_unlock(&d->lock);
    return rc;
}

static int vd_read(BlockDev *bd, u64 lba, u32 count, void *buf) { return vd_rw(bd, false, lba, count, buf); }
static int vd_write(BlockDev *bd, u64 lba, u32 count, const void *buf) { return vd_rw(bd, true, lba, count, (void *)buf); }

static int vd_flush(BlockDev *bd) {
    VDisk *d = bd->priv;
    if (!d->flush_ok) return 0;
    mutex_lock(&d->lock);
    int rc = submit(d, VBLK_T_FLUSH, 0, NULL, 0, false);
    mutex_unlock(&d->lock);
    return rc;
}

/* ---------------------------------------------------------------------------------------------- probing */

typedef struct { u8 bar; u32 offset, length; bool found; } CapLoc;

/* Walks the vendor capabilities (several share one id, so pci_find_cap is not enough). */
static void find_caps(PciDev *p, CapLoc *common, CapLoc *notify, CapLoc *isr, CapLoc *devcfg, u32 *notify_mult) {
    u8 ptr = 0;
    if (pci_read16(p, 6) & 0x10) ptr = pci_read8(p, 0x34) & 0xFC;
    for (int guard = 0; ptr && guard < 48; guard++) {
        u8 id = pci_read8(p, ptr), next = pci_read8(p, ptr + 1) & 0xFC;
        if (id == PCI_CAP_VENDOR && pci_read8(p, ptr + 2) >= 16) {
            u8 type = pci_read8(p, ptr + 3);
            CapLoc loc = { pci_read8(p, ptr + 4), pci_read32(p, ptr + 8), pci_read32(p, ptr + 12), true };
            if (type == VCAP_COMMON && !common->found) *common = loc;
            else if (type == VCAP_NOTIFY && !notify->found && pci_read8(p, ptr + 2) >= 20) {
                *notify = loc;
                *notify_mult = pci_read32(p, ptr + 16);
            } else if (type == VCAP_ISR && !isr->found) *isr = loc;
            else if (type == VCAP_DEVICE && !devcfg->found) *devcfg = loc;
        }
        ptr = next;
    }
}

static volatile u8 *map_cap(PciDev *p, const CapLoc *c, u32 min_len) {
    if (!c->found || c->bar > 5 || c->length < min_len) return NULL;
    u8 *base = pci_map_bar(p, c->bar);
    if (!base) return NULL;
    if ((u64)c->offset + c->length > p->bar[c->bar].size) return NULL;
    return base + c->offset;
}

static void disk_name(int idx, char *out, size_t n) {
    char suffix[4];
    int k = 0;
    int v = idx;
    do { suffix[k++] = (char)('a' + v % 26); v = v / 26 - 1; } while (v >= 0 && k < 3);
    size_t o = 0;
    out[o++] = 'v'; out[o++] = 'd';
    while (k && o + 1 < n) out[o++] = suffix[--k];
    out[o] = 0;
}

static bool probe(PciDev *p, int index) {
    VDisk *d = kzalloc(sizeof *d);
    if (!d) return false;
    mutex_init(&d->lock);
    d->pci = p;

    CapLoc common = { 0 }, notify = { 0 }, isr = { 0 }, devcfg = { 0 };
    u32 mult = 0;
    find_caps(p, &common, &notify, &isr, &devcfg, &mult);

    pci_enable(p, PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_MASTER);
    if (common.found && notify.found && devcfg.found) {
        d->common = map_cap(p, &common, 0x38);
        d->notify_base = map_cap(p, &notify, 2);
        d->devcfg = map_cap(p, &devcfg, 24);
        d->isr = map_cap(p, &isr, 1);
        d->notify_mult = mult;
        d->modern = d->common && d->notify_base && d->devcfg;
    }
    if (!d->modern) {
        if (!p->bar[0].io || !p->bar[0].base) { klog("Kappa", "virtio-blk %02x:%02x.%x: no usable transport", p->bus, p->dev, p->fn); goto fail; }
        d->io = (u16)p->bar[0].base;
    }
    if (p->device == VIRTIO_BLK_MODERN && !d->modern) goto fail;           /* a modern-only device cannot be driven via legacy */

    if (!device_reset(d)) { klog("Kappa", "virtio-blk %02x:%02x.%x: reset timed out", p->bus, p->dev, p->fn); goto fail; }
    set_status(d, VS_ACK);
    set_status(d, VS_ACK | VS_DRIVER);

    const u32 want = VBLK_F_SIZE_MAX | VBLK_F_SEG_MAX | VBLK_F_RO | VBLK_F_BLK_SIZE | VBLK_F_FLUSH;
    u32 dev_lo, dev_hi = 0;
    if (d->modern) {
        wr32(d, CM_DEV_FEATURE_SEL, 0);
        dev_lo = rd32(d, CM_DEV_FEATURE);
        wr32(d, CM_DEV_FEATURE_SEL, 1);
        dev_hi = rd32(d, CM_DEV_FEATURE);
        if (!(dev_hi & VIRTIO_F_VERSION_1_HI)) { klog("Kappa", "virtio-blk: device lacks VERSION_1"); goto fail; }
        wr32(d, CM_DRV_FEATURE_SEL, 0);
        wr32(d, CM_DRV_FEATURE, dev_lo & want);
        wr32(d, CM_DRV_FEATURE_SEL, 1);
        wr32(d, CM_DRV_FEATURE, VIRTIO_F_VERSION_1_HI);
        set_status(d, VS_ACK | VS_DRIVER | VS_FEATURES_OK);
        if (!(get_status(d) & VS_FEATURES_OK)) { klog("Kappa", "virtio-blk: device rejected our features"); goto fail; }
    } else {
        dev_lo = rd32(d, LG_DEV_FEATURES);
        wr32(d, LG_DRV_FEATURES, dev_lo & want);
    }
    u32 feat = dev_lo & want;
    d->flush_ok = feat & VBLK_F_FLUSH;

    d->req = dma_alloc(PAGE_SIZE, &d->req_phys);
    if (!d->req || !queue_setup(d)) { klog("Kappa", "virtio-blk %02x:%02x.%x: no usable virtqueue", p->bus, p->dev, p->fn); goto fail_reset; }
    set_status(d, get_status(d) | VS_DRIVER_OK);

    /* configuration: capacity (512-byte units), limits, block size */
    u64 capacity = 0;
    u32 blk_size = 512;
    for (int tries = 0; tries < 3; tries++) {
        u8 gen = d->modern ? *(volatile u8 *)(d->common + CM_CONFIG_GEN) : 0;
        capacity = (u64)cfg32(d, 0) | (u64)cfg32(d, 4) << 32;
        if (feat & VBLK_F_SIZE_MAX) d->size_max = cfg32(d, 8);
        if (feat & VBLK_F_SEG_MAX) d->seg_max = cfg32(d, 12);
        if (feat & VBLK_F_BLK_SIZE) blk_size = cfg32(d, 20);
        if (!d->modern || gen == *(volatile u8 *)(d->common + CM_CONFIG_GEN)) break;
    }
    if (d->size_max && d->size_max < 512) d->size_max = 0;              /* nonsense value: treat as unlimited */
    if (!(feat & VBLK_F_BLK_SIZE) || blk_size < 512 || blk_size > 4096 || (blk_size & (blk_size - 1))) blk_size = 512;
    if (!capacity) { klog("Kappa", "virtio-blk %02x:%02x.%x: reports no capacity", p->bus, p->dev, p->fn); goto fail_reset; }

    BlockDev *bd = kzalloc(sizeof *bd);
    if (!bd) goto fail_reset;
    disk_name(index, bd->name, sizeof bd->name);
    strlcpy(bd->model, "Virtio block device", sizeof bd->model);
    bd->driver = "virtio-blk";
    bd->sector_size = blk_size;
    bd->sectors = capacity / (blk_size / 512);
    bd->readonly = feat & VBLK_F_RO;
    bd->read = vd_read;
    bd->write = vd_write;
    bd->flush = vd_flush;
    bd->priv = d;
    p->driver = "virtio-blk";
    p->driver_data = d;
    klog("Kappa", "virtio-blk %02x:%02x.%x: %s transport, queue %u, %s%s", p->bus, p->dev, p->fn, d->modern ? "modern" : "legacy",
         d->qsize, d->flush_ok ? "flush" : "no flush", bd->readonly ? ", read-only" : "");
    blk_register(bd);
    return true;

fail_reset:
    set_status(d, get_status(d) | VS_FAILED);
    device_reset(d);
fail:
    if (d->qmem) dma_free(d->qmem, d->qbytes);
    if (d->req) dma_free(d->req, PAGE_SIZE);
    kfree(d);
    return false;
}

void virtio_blk_init(void) {
    int index = 0;
    for (PciDev *p = pci_first(); p; p = pci_next(p)) {
        if (p->vendor != VIRTIO_VENDOR || p->driver) continue;
        if (p->device != VIRTIO_BLK_LEGACY && p->device != VIRTIO_BLK_MODERN) continue;
        if (probe(p, index)) index++;
    }
}
