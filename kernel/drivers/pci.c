#include "pci.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "vfs.h"

#define CFG_ADDR 0xCF8
#define CFG_DATA 0xCFC

static PciDev *g_head, *g_tail;
static int g_count;
static bool g_bus_seen[256];

/* ---------------------------------------------------------------- config space */

static u32 cfg_read32(u8 bus, u8 dev, u8 fn, u16 off) {
    u64 f = irq_save();
    outl(CFG_ADDR, 0x80000000u | (u32)bus << 16 | (u32)dev << 11 | (u32)fn << 8 | (off & 0xFCu));
    u32 v = inl(CFG_DATA);
    irq_restore(f);
    return v;
}

static void cfg_write32(u8 bus, u8 dev, u8 fn, u16 off, u32 v) {
    u64 f = irq_save();
    outl(CFG_ADDR, 0x80000000u | (u32)bus << 16 | (u32)dev << 11 | (u32)fn << 8 | (off & 0xFCu));
    outl(CFG_DATA, v);
    irq_restore(f);
}

u32 pci_read32(const PciDev *d, u16 off) { return cfg_read32(d->bus, d->dev, d->fn, off); }
u16 pci_read16(const PciDev *d, u16 off) { return (u16)(cfg_read32(d->bus, d->dev, d->fn, off) >> ((off & 2) * 8)); }
u8  pci_read8(const PciDev *d, u16 off)  { return (u8)(cfg_read32(d->bus, d->dev, d->fn, off) >> ((off & 3) * 8)); }

void pci_write32(const PciDev *d, u16 off, u32 v) { cfg_write32(d->bus, d->dev, d->fn, off, v); }

void pci_write16(const PciDev *d, u16 off, u16 v) {
    u32 shift = (off & 2) * 8;
    u32 cur = cfg_read32(d->bus, d->dev, d->fn, off);
    cur = (cur & ~(0xFFFFu << shift)) | ((u32)v << shift);
    cfg_write32(d->bus, d->dev, d->fn, off, cur);
}

void pci_write8(const PciDev *d, u16 off, u8 v) {
    u32 shift = (off & 3) * 8;
    u32 cur = cfg_read32(d->bus, d->dev, d->fn, off);
    cur = (cur & ~(0xFFu << shift)) | ((u32)v << shift);
    cfg_write32(d->bus, d->dev, d->fn, off, cur);
}

/* ---------------------------------------------------------------- enumeration */

static void probe_bars(PciDev *d, int nbars) {
    u16 cmd = pci_read16(d, 4);
    pci_write16(d, 4, cmd & (u16) ~(PCI_CMD_IO | PCI_CMD_MEM));         /* never size a BAR that is decoding */
    for (int i = 0; i < nbars; i++) {
        u16 reg = (u16)(0x10 + 4 * i);
        u32 orig = pci_read32(d, reg);
        if (orig & 1) {                                                  /* I/O space */
            pci_write32(d, reg, 0xFFFFFFFFu);
            u32 m = pci_read32(d, reg);
            pci_write32(d, reg, orig);
            u32 size = (~(m & ~3u) + 1) & 0xFFFF;
            if (size) { d->bar[i].io = true; d->bar[i].base = orig & ~3u; d->bar[i].size = size; }
            continue;
        }
        u32 type = (orig >> 1) & 3;
        bool pf = orig & 8;
        if (type == 2 && i + 1 < nbars) {                                /* 64-bit memory BAR uses two registers */
            u16 reg_hi = (u16)(reg + 4);
            u32 orig_hi = pci_read32(d, reg_hi);
            pci_write32(d, reg, 0xFFFFFFFFu);
            pci_write32(d, reg_hi, 0xFFFFFFFFu);
            u32 mlo = pci_read32(d, reg), mhi = pci_read32(d, reg_hi);
            pci_write32(d, reg, orig);
            pci_write32(d, reg_hi, orig_hi);
            u64 mask = ((u64)mhi << 32) | (mlo & ~0xFu);
            u64 size = ~mask + 1;
            if (mask && size) {
                d->bar[i].mem64 = true;
                d->bar[i].prefetch = pf;
                d->bar[i].base = ((u64)orig_hi << 32) | (orig & ~0xFu);
                d->bar[i].size = size;
            }
            i++;                                                         /* the upper half is not a BAR of its own */
            continue;
        }
        pci_write32(d, reg, 0xFFFFFFFFu);
        u32 m = pci_read32(d, reg);
        pci_write32(d, reg, orig);
        u32 size = ~(m & ~0xFu) + 1;
        if ((m & ~0xFu) && size) { d->bar[i].prefetch = pf; d->bar[i].base = orig & ~0xFu; d->bar[i].size = size; }
    }
    pci_write16(d, 4, cmd);
}

static void scan_bus(int bus, int depth);

/* The firmware's own drivers have stopped, but a controller it left running could still write into memory the firmware
 * owned and this kernel is about to reuse (the loader hands that memory back after the buses have been scanned). Bus
 * mastering is switched off on the kinds of device that DMA by themselves; a driver turns it on again when it claims one. */
static void quiesce_dma(PciDev *d) {
    if ((d->header_type & 0x7F) != 0) return;                           /* endpoints only: bridges must keep forwarding */
    bool dmas = d->class_code == 0x01 || d->class_code == 0x02 || d->class_code == 0x04 ||
                (d->class_code == 0x0C && d->subclass == 0x03);         /* storage, network, audio, USB */
    if (!dmas) return;
    u16 cmd = pci_read16(d, 4);
    if (cmd & PCI_CMD_MASTER) pci_write16(d, 4, (u16)(cmd & ~PCI_CMD_MASTER));
}

static void scan_function(int bus, int dev, int fn, int depth) {
    u32 id = cfg_read32((u8)bus, (u8)dev, (u8)fn, 0);
    if ((id & 0xFFFF) == 0xFFFF || id == 0) return;
    PciDev *d = kzalloc(sizeof *d);
    if (!d) return;
    d->bus = (u8)bus; d->dev = (u8)dev; d->fn = (u8)fn;
    d->vendor = (u16)id; d->device = (u16)(id >> 16);
    u32 cls = cfg_read32((u8)bus, (u8)dev, (u8)fn, 8);
    d->revision = (u8)cls; d->progif = (u8)(cls >> 8); d->subclass = (u8)(cls >> 16); d->class_code = (u8)(cls >> 24);
    d->header_type = (u8)(cfg_read32((u8)bus, (u8)dev, (u8)fn, 0x0C) >> 16);
    u32 ir = cfg_read32((u8)bus, (u8)dev, (u8)fn, 0x3C);
    d->irq_line = (u8)ir; d->irq_pin = (u8)(ir >> 8);
    int kind = d->header_type & 0x7F;
    quiesce_dma(d);
    /* Sizing a BAR means switching the device's memory decoding off and writing all ones into it. The display adapter is scanning
     * out the screen this kernel is drawing on, through the very window those BARs describe, so it is left strictly alone (Linux
     * does the same for the boot display: "mmio_always_on"). No driver here wants its BARs; the framebuffer comes from the loader. */
    if (kind == 0 && d->class_code != 0x03) probe_bars(d, 6);
    else if (kind == 1) probe_bars(d, 2);
    if (g_tail) g_tail->next = d; else g_head = d;
    g_tail = d;
    g_count++;
    if (kind == 1 && depth < 8) {
        int sec = (cfg_read32((u8)bus, (u8)dev, (u8)fn, 0x18) >> 8) & 0xFF;
        if (sec) scan_bus(sec, depth + 1);
    }
}

static void scan_bus(int bus, int depth) {
    if (g_bus_seen[bus]) return;
    g_bus_seen[bus] = true;
    for (int dev = 0; dev < 32; dev++) {
        u32 id = cfg_read32((u8)bus, (u8)dev, 0, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        scan_function(bus, dev, 0, depth);
        if (cfg_read32((u8)bus, (u8)dev, 0, 0x0C) & 0x800000)           /* multi-function */
            for (int fn = 1; fn < 8; fn++) scan_function(bus, dev, fn, depth);
    }
}

PciDev *pci_first(void) { return g_head; }
PciDev *pci_next(PciDev *d) { return d->next; }

PciDev *pci_find_class(u8 class_code, u8 subclass, int progif, PciDev *after) {
    for (PciDev *d = after ? after->next : g_head; d; d = d->next)
        if (d->class_code == class_code && d->subclass == subclass && (progif < 0 || d->progif == progif)) return d;
    return NULL;
}

PciDev *pci_find_id(u16 vendor, u16 device, PciDev *after) {
    for (PciDev *d = after ? after->next : g_head; d; d = d->next)
        if (d->vendor == vendor && (device == 0xFFFF || d->device == device)) return d;
    return NULL;
}

void pci_enable(PciDev *d, u16 bits) {
    u16 cmd = pci_read16(d, 4);
    cmd = (u16)((cmd | bits | PCI_CMD_INTX_OFF));
    pci_write16(d, 4, cmd);
}

u8 pci_find_cap(const PciDev *d, u8 cap_id) {
    if (!(pci_read16(d, 6) & 0x10)) return 0;
    u8 ptr = pci_read8(d, 0x34) & 0xFC;
    for (int guard = 0; ptr && guard < 48; guard++) {
        u16 hdr = pci_read16(d, ptr);
        if ((u8)hdr == cap_id) return ptr;
        ptr = (u8)(hdr >> 8) & 0xFC;
    }
    return 0;
}

void *pci_map_bar(PciDev *d, int bar) {
    if (bar < 0 || bar > 5 || d->bar[bar].io || !d->bar[bar].size || !d->bar[bar].base) return NULL;
    if (!d->bar[bar].virt) d->bar[bar].virt = ioremap(d->bar[bar].base, d->bar[bar].size, PTE_PCD);
    return d->bar[bar].virt;
}

/* ---------------------------------------------------------------- DMA helpers */

extern u64 g_kernel_phys, g_kernel_virt;
extern char __kernel_start[], __kernel_end[];

paddr_t virt_to_phys(const void *v) {
    u64 a = (u64)v;
    if (a >= g_hhdm && a < g_hhdm + pmm_ram_top()) return a - g_hhdm;
    if (a >= (u64)__kernel_start && a < (u64)__kernel_end) return g_kernel_phys + (a - g_kernel_virt);
    panic("virt_to_phys: %p is neither in the direct map nor in the kernel image (a stack buffer?)", v);
}

void *dma_alloc(size_t size, paddr_t *phys) {
    size_t pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    unsigned order = 0;
    while (((size_t)1 << order) < pages) order++;
    if (order > PMM_MAX_ORDER) return NULL;
    paddr_t p = pmm_alloc_below(order, 0x100000000UL);       /* DMA stays in the first 4 GiB: not every controller does 64-bit addresses */
    if (!p) return NULL;
    void *v = phys_to_virt(p);
    memset(v, 0, (size_t)PAGE_SIZE << order);
    if (phys) *phys = p;
    return v;
}

void dma_free(void *virt, size_t size) {
    if (!virt) return;
    size_t pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    unsigned order = 0;
    while (((size_t)1 << order) < pages) order++;
    pmm_free(virt_to_phys(virt), order);
}

/* ---------------------------------------------------------------- /proc/pci and init */

static const char *class_name(const PciDev *d) {
    switch (d->class_code << 8 | d->subclass) {
    case 0x0100: return "SCSI controller";
    case 0x0101: return "IDE controller";
    case 0x0106: return d->progif == 1 ? "SATA controller (AHCI)" : "SATA controller";
    case 0x0108: return d->progif == 2 ? "NVMe controller" : "Non-volatile memory controller";
    case 0x0200: return "Ethernet controller";
    case 0x0300: return "VGA compatible controller";
    case 0x0380: return "Display controller";
    case 0x0401: return "Multimedia audio controller";
    case 0x0403: return "Audio device (HDA)";
    case 0x0600: return "Host bridge";
    case 0x0601: return "ISA bridge";
    case 0x0604: return "PCI bridge";
    case 0x0c03: return d->progif == 0x30 ? "USB controller (xHCI)" : d->progif == 0x20 ? "USB controller (EHCI)" :
                        d->progif == 0x10 ? "USB controller (OHCI)" : d->progif == 0 ? "USB controller (UHCI)" : "USB controller";
    case 0x0c05: return "SMBus";
    default: return "device";
    }
}

size_t pci_describe(char *buf, size_t cap) {
    size_t n = 0;
    for (PciDev *d = g_head; d && n + 160 < cap; d = d->next) {
        n += (size_t)snprintf(buf + n, cap - n, "%02x:%02x.%x %02x%02x:%02x %04x:%04x %s", d->bus, d->dev, d->fn, d->class_code, d->subclass,
                              d->progif, d->vendor, d->device, class_name(d));
        if (d->driver) n += (size_t)snprintf(buf + n, cap - n, "  [%s]", d->driver);
        n += (size_t)snprintf(buf + n, cap - n, "\n");
        for (int i = 0; i < 6 && n + 90 < cap; i++)
            if (d->bar[i].size)
                n += (size_t)snprintf(buf + n, cap - n, "    bar%d %s %#lx size %#lx%s\n", i, d->bar[i].io ? "io " : "mem", (unsigned long)d->bar[i].base,
                                      (unsigned long)d->bar[i].size, d->bar[i].prefetch ? " prefetch" : "");
    }
    return n;
}

void pci_init(void) {
    scan_bus(0, 0);
    klog("Marisa", "pci: %d devices found on the bus", g_count);
    for (PciDev *d = g_head; d; d = d->next)
        klog("Marisa", "pci %02x:%02x.%x %04x:%04x %s", d->bus, d->dev, d->fn, d->vendor, d->device, class_name(d));
}
