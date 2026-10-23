/*
 * PCI bus: enumeration through the legacy 0xCF8/0xCFC mechanism (enough for every device we drive), BAR decoding,
 * capability lookup, and DMA-safe buffer allocation.
 *
 * Drivers poll (from a kernel thread) rather than taking interrupts: interrupt routing is the most fragile part of
 * real PC hardware, polling at 1 kHz costs nothing measurable and works everywhere.
 */
#ifndef K_PCI_H
#define K_PCI_H

#include "types.h"

typedef struct PciBar {
    u64  base;                  /* physical address (memory BARs) or port (I/O BARs); 0 = unimplemented */
    u64  size;
    bool io;
    bool mem64;
    bool prefetch;
    void *virt;                 /* set by pci_map_bar */
} PciBar;

typedef struct PciDev {
    u8  bus, dev, fn;
    u16 vendor, device;
    u8  class_code, subclass, progif, revision;
    u8  header_type;
    u8  irq_line, irq_pin;
    PciBar bar[6];
    void *driver_data;
    const char *driver;         /* name of the driver that claimed the device, or NULL */
    struct PciDev *next;
} PciDev;

#define PCI_CMD_IO        0x1
#define PCI_CMD_MEM       0x2
#define PCI_CMD_MASTER    0x4
#define PCI_CMD_INTX_OFF  0x400

#define PCI_CAP_PM    0x01
#define PCI_CAP_MSI   0x05
#define PCI_CAP_PCIE  0x10
#define PCI_CAP_MSIX  0x11
#define PCI_CAP_VENDOR 0x09     /* virtio capabilities live here */

void    pci_init(void);                             /* enumerate every bus; safe to call once, after the VFS exists */
size_t  pci_describe(char *buf, size_t cap);        /* text for /proc/pci */
PciDev *pci_first(void);
PciDev *pci_next(PciDev *d);
PciDev *pci_find_class(u8 class_code, u8 subclass, int progif, PciDev *after);   /* progif < 0: any */
PciDev *pci_find_id(u16 vendor, u16 device, PciDev *after);                     /* device 0xFFFF: any */

u32  pci_read32(const PciDev *d, u16 off);
u16  pci_read16(const PciDev *d, u16 off);
u8   pci_read8(const PciDev *d, u16 off);
void pci_write32(const PciDev *d, u16 off, u32 v);
void pci_write16(const PciDev *d, u16 off, u16 v);
void pci_write8(const PciDev *d, u16 off, u8 v);

void  pci_enable(PciDev *d, u16 command_bits);       /* ORs PCI_CMD_* bits into the command register, INTx stays off */
u8    pci_find_cap(const PciDev *d, u8 cap_id);      /* config-space offset of the capability, or 0 */
void *pci_map_bar(PciDev *d, int bar);               /* uncached mapping of a memory BAR (cached in d->bar[bar].virt) */

/* ---- DMA ---- */
/* Zeroed, physically contiguous, page-aligned memory reachable through the direct map. *phys receives the bus address
 * (this machine has no IOMMU translation enabled by us, so bus address == physical address). */
void *dma_alloc(size_t size, paddr_t *phys);
void  dma_free(void *virt, size_t size);
/* Physical address of any kernel pointer that lives in the direct map or in the kernel image (never a stack buffer). */
paddr_t virt_to_phys(const void *v);

#endif
