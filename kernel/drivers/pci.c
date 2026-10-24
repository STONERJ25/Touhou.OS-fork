#include "pci.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "irq.h"
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

/* The size of a BAR from what it reads back after all ones were written: the lowest address bit the device lets us set (the bits
 * below it read as zero). Not "~mask + 1": that is wrong whenever the bits ABOVE the device's address width read as zero too, which
 * 64-bit BARs of devices with fewer than 64 address bits (many) and 32-bit BARs of small devices do; the result was a size of
 * exabytes and an ioremap that never came back. This is what Linux's pci_size() does. */
static u64 bar_size(u64 readback_masked) { return readback_masked & (~readback_masked + 1); }

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
            u32 size = (u32)bar_size(m & ~3u & 0xFFFFu);                 /* port numbers are 16 bits */
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
            u64 size = bar_size(((u64)mhi << 32) | (mlo & ~0xFu));
            if (size) {
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
        u64 size = bar_size(m & ~0xFu);
        if (size) { d->bar[i].prefetch = pf; d->bar[i].base = orig & ~0xFu; d->bar[i].size = size; }
    }
    pci_write16(d, 4, cmd);
}

/*
 * The memory BAR of a display adapter that holds [addr, addr + len): the window a write-combining MTRR for the framebuffer
 * would cover, found when it is needed and not at scan time (see scan_function: display BARs are never sized there). Sizing
 * a BAR that decodes is what Linux does for the boot display ("mmio_always_on"): all ones go in and the old value comes
 * back within microseconds, with interrupts off and nothing drawing, so the scanout is not touched (it reads the display
 * memory through the GPU's own path, not through this BAR).
 */
bool pci_display_bar(paddr_t addr, u64 len, paddr_t *base_out, u64 *size_out, bool *prefetch_out) {
    for (PciDev *d = g_head; d; d = d->next) {
        if (d->class_code != 0x03 || (d->header_type & 0x7F) != 0) continue;
        if (!(pci_read16(d, 4) & PCI_CMD_MEM)) continue;                 /* not decoding memory: not the screen */
        for (int i = 0; i < 6; i++) {
            u16 reg = (u16)(0x10 + 4 * i);
            u32 lo = pci_read32(d, reg);
            if (lo & 1) continue;                                        /* I/O space */
            bool wide = ((lo >> 1) & 3) == 2 && i < 5;
            u32 hi = wide ? pci_read32(d, (u16)(reg + 4)) : 0;
            u64 base = ((u64)hi << 32) | (lo & ~0xFu);
            if (wide) i++;
            if (!base || base > addr) continue;
            u64 f = irq_save();
            pci_write32(d, reg, 0xFFFFFFFFu);
            if (wide) pci_write32(d, (u16)(reg + 4), 0xFFFFFFFFu);
            u32 slo = pci_read32(d, reg), shi = wide ? pci_read32(d, (u16)(reg + 4)) : 0;
            pci_write32(d, reg, lo);
            if (wide) pci_write32(d, (u16)(reg + 4), hi);
            irq_restore(f);
            u64 size = bar_size(((u64)shi << 32) | (slo & ~0xFu));
            if (size < PAGE_SIZE || addr - base >= size || len > size - (addr - base)) continue;
            *base_out = base;
            *size_out = size;
            *prefetch_out = (lo & 8) != 0;
            return true;
        }
    }
    return false;
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

/*
 * Brings a function that the firmware (or a previous OS) left in a low-power state back to D0, the only state in which its
 * registers work. PCI Power Management spec 1.2: the Power State field of PMCSR (capability offset +4, bits 1:0), and after
 * D3hot -> D0 the function needs 10 ms before it may be accessed (D2: 200 microseconds, D1: none; 1 ms covers both). Unless
 * the NoSoftReset bit (bit 3) says otherwise that transition is a soft reset, and a reset function has lost its BARs and
 * command register, so those are written back the way they were (Linux does the same: pci_restore_bars). PME_Status (bit
 * 15) is write-1-to-clear and is written as 0 so that a pending wake-up event is not discarded by accident.
 * Returns the state the function was in (0 = it already was in D0, or has no power management capability).
 */
int pci_set_d0(PciDev *d) {
    u8 pm = pci_find_cap(d, PCI_CAP_PM);
    if (!pm) return 0;
    u16 pmcsr = pci_read16(d, (u16)(pm + 4));
    int was = pmcsr & 3;
    if (!was || pmcsr == 0xFFFF) return 0;                              /* in D0 already, or not answering at all */
    u32 bars[6];
    for (int i = 0; i < 6; i++) bars[i] = pci_read32(d, (u16)(0x10 + 4 * i));
    u16 cmd = pci_read16(d, 4);
    pci_write16(d, (u16)(pm + 4), (u16)(pmcsr & ~(0x8003u)));
    udelay(was == 3 ? 10000 : 1000);
    u16 after = pci_read16(d, (u16)(pm + 4));
    if (after & 3) {
        klog("Marisa", "pci %02x:%02x.%x %04x:%04x stays in power state D%u after being told to wake up", d->bus, d->dev, d->fn, d->vendor,
             d->device, after & 3);
        return was;
    }
    if (!(pmcsr & 8)) {                                                 /* soft reset: put back what the reset took away */
        for (int i = 0; i < 6; i++)
            if (pci_read32(d, (u16)(0x10 + 4 * i)) != bars[i]) pci_write32(d, (u16)(0x10 + 4 * i), bars[i]);
        if (pci_read16(d, 4) != cmd) pci_write16(d, 4, cmd);
    }
    klog("Marisa", "pci %02x:%02x.%x %04x:%04x was asleep (power state D%d): moved to D0", d->bus, d->dev, d->fn, d->vendor, d->device, was);
    return was;
}

void pci_enable(PciDev *d, u16 bits) {
    pci_set_d0(d);                              /* every driver comes through here: a function left in D3 by the firmware must be awake first */
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

/* ---------------------------------------------------------------- self test */

#define PCHECK(cond) do { if (!(cond)) { fails++; printk("  FAIL pci.c:%d: %s\n", __LINE__, #cond); } } while (0)

/* "selftest": BAR sizing from read-backs a real device can produce, and the wake-up from D3hot on a real (emulated) function. */
int pci_selftest(void) {
    int fails = 0;
    klog("selftest", "pci: BAR sizes from read-backs, D3hot -> D0");
    PCHECK(bar_size(0xFFFFFFFFFFFF0000UL) == 0x10000);          /* 64-bit BAR whose upper dword is fully writable */
    PCHECK(bar_size(0x0000FFFFFFFF0000UL) == 0x10000);          /* the upper bits read zero beyond a 48-bit address width: "~mask + 1" gave 0xFFFF000000010000 */
    PCHECK(bar_size(0x000000FFF0000000UL) == 0x10000000);       /* 256 MiB aperture of a device with 40 address bits */
    PCHECK(bar_size(0xFFFFC000UL) == 0x4000);                   /* 32-bit BAR, 16 KiB */
    PCHECK(bar_size(0x0FFFC000UL) == 0x4000);                   /* a small device that only implements 28 address bits */
    PCHECK(bar_size(0) == 0);                                   /* not implemented */

    /* The first function on bus 0 with a power management capability: put it to sleep the way firmware does, wake it, compare. */
    for (int dev = 0; dev < 32; dev++) {
        PciDev t;
        memset(&t, 0, sizeof t);
        t.bus = 0; t.dev = (u8)dev; t.fn = 0;
        u32 id = pci_read32(&t, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        t.vendor = (u16)id; t.device = (u16)(id >> 16);
        u8 pm = pci_find_cap(&t, PCI_CAP_PM);
        if (!pm) continue;
        u32 bar0 = pci_read32(&t, 0x10);
        u16 cmd = pci_read16(&t, 4);
        pci_write16(&t, (u16)(pm + 4), (u16)((pci_read16(&t, (u16)(pm + 4)) & ~0x8003u) | 3));       /* D3hot */
        if ((pci_read16(&t, (u16)(pm + 4)) & 3) != 3) { klog("selftest", "pci: 00:%02x.0 does not accept D3hot, skipped", dev); continue; }
        u64 t0 = uptime_ms();
        int was = pci_set_d0(&t);
        u64 took = uptime_ms() - t0;
        PCHECK(was == 3);
        PCHECK((pci_read16(&t, (u16)(pm + 4)) & 3) == 0);
        PCHECK(pci_read32(&t, 0x10) == bar0 && pci_read16(&t, 4) == cmd);
        PCHECK(took >= 7);                                      /* the 10 ms the specification demands after D3hot (the millisecond clock ticks, so a little slack) */
        PCHECK(pci_set_d0(&t) == 0);                            /* already awake: nothing to do */
        klog("selftest", "pci: 00:%02x.0 %04x:%04x woke from D3hot in %lu ms", dev, t.vendor, t.device, (unsigned long)took);
        break;
    }
    klog("selftest", "pci: %s", fails ? "FAIL" : "ok");
    return fails;
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
