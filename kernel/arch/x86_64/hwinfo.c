/*
 * Facts for the hardware report (user/bin/kappabox/hwreport.c, docs/HARDWARE_BREADTH.md) that no other /proc file gives:
 *
 *   /proc/hwinfo   what the firmware told the loader (UEFI vendor and revision, Secure Boot, the graphics modes offered), the memory
 *                  map by type, the TSC rate, the ACPI tables with their OEM ids and the MADT summary, the interrupt set-up, and which
 *                  well-known devices the AML mentions (found by looking at the bytes, not by running anything)
 *   /proc/pcicaps  per PCI function: MSI and MSI-X capabilities and the PCIe link (speed and width, negotiated and possible)
 *
 * Everything the firmware leaves in memory that the page allocator later takes back (the ACPI tables are often in such memory) is read
 * once, early, by hwinfo_early(), called from kmain() right after acpi_init(); the text is generated from those records on every read.
 * One format throughout: "key: value" lines, a dot between the parts of a key, so that a script can read them.
 *
 * ACPI structures follow the ACPI specification (the system description table header, the FADT's DSDT pointers, the MADT's entry types
 * 0, 1, 2, 4, 5 and 9); the PCI capability layouts follow the PCI Local Bus and PCI Express base specifications (MSI: capability 05h,
 * MSI-X: 11h, PCI Express: 10h with the link capabilities at +0Ch and the link status at +12h).
 */
#include "kernel.h"
#include "dmi.h"
#include "mm.h"
#include "pci.h"
#include "acpi.h"
#include "cpuinfo.h"
#include "irq.h"
#include "vfs.h"

/* ------------------------------------------------------------------ output */

typedef struct { char *b; size_t cap, n; } Sink;

static void put(Sink *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(Sink *s, const char *fmt, ...) {
    if (s->n + 1 >= s->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(s->b + s->n, s->cap - s->n, fmt, ap);
    va_end(ap);
    if (w > 0) s->n += (size_t)w < s->cap - s->n ? (size_t)w : s->cap - s->n - 1;
}

/* ------------------------------------------------------------------ ACPI, read once at boot */

#define MAX_TAB     48
#define MAX_OVER    24
#define MAX_IOAPIC  8

typedef struct { char sig[5]; u8 rev; char oem[7]; char oemtab[9]; u32 oemrev, len; } AcpiTab;

static struct {
    bool have;
    u8   rsdp_rev;
    char rsdp_oem[7];
    bool xsdt;
    int  ntab;
    AcpiTab tab[MAX_TAB];
    bool have_madt;
    u32  lapic_addr, madt_flags;
    int  lapic_total, lapic_enabled, x2apic_total, x2apic_enabled, nmi;
    int  nioapic;
    struct { u8 id; u32 addr, gsi; } ioapic[MAX_IOAPIC];
    int  nover;                             /* overrides in the table */
    struct { u8 bus, src; u32 gsi; u16 flags; } over[MAX_OVER];
    u32  aml_found;                         /* bit per entry of g_aml_ids */
    int  thermal_zones;
    int  aml_tables;
} H;

static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u64 rd64(const u8 *p) { return (u64)rd32(p) | (u64)rd32(p + 4) << 32; }

static bool in_ram(u64 phys, u64 len) {
    u32 n;
    const BootMemRegion *r = boot_mmap(&n);
    for (u32 i = 0; i < n; i++) {
        u32 t = r[i].type;
        if (t != BOOTMEM_USABLE && t != BOOTMEM_BOOT_RECLAIMABLE && t != BOOTMEM_ACPI_RECLAIMABLE && t != BOOTMEM_ACPI_NVS &&
            t != BOOTMEM_KERNEL && t != BOOTMEM_MODULE)
            continue;
        if (phys >= r[i].base && phys + len <= r[i].base + r[i].length) return true;
    }
    return false;
}

/* The kernel's direct map covers RAM only; a table the firmware parked elsewhere is mapped uncached for the moment of reading. */
static const u8 *map_phys(u64 phys, u64 len) {
    if (!phys || phys + len < phys || phys + len > (1ULL << 48)) return NULL;
    if (in_ram(phys, len)) return (const u8 *)phys_to_virt(phys);
    return (const u8 *)ioremap(phys, len, PTE_PCD);
}

/* A table whose header and whole length can be read; NULL if the header is nonsense. */
static const u8 *map_table(u64 phys) {
    const u8 *h = map_phys(phys, 36);
    if (!h) return NULL;
    u32 len = rd32(h + 4);
    if (len < 36 || len > (16u << 20)) return NULL;
    return map_phys(phys, len);
}

static void trim_copy(char *out, const u8 *src, size_t n) {
    size_t k = 0;
    for (size_t i = 0; i < n && src[i]; i++) out[k++] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : '?';
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
}

static void note_table(const u8 *t) {
    if (H.ntab >= MAX_TAB) return;
    AcpiTab *a = &H.tab[H.ntab++];
    trim_copy(a->sig, t, 4);
    a->rev = t[8];
    trim_copy(a->oem, t + 10, 6);
    trim_copy(a->oemtab, t + 16, 8);
    a->oemrev = rd32(t + 24);
    a->len = rd32(t + 4);
}

static void parse_madt(const u8 *t) {
    u32 len = rd32(t + 4);
    H.have_madt = true;
    H.lapic_addr = rd32(t + 36);
    H.madt_flags = rd32(t + 40);
    for (u32 off = 44; off + 2 <= len;) {
        const u8 *e = t + off;
        u8 type = e[0], elen = e[1];
        if (elen < 2 || off + elen > len) break;
        switch (type) {
        case 0:  if (elen >= 8) { H.lapic_total++; if (rd32(e + 4) & 3) H.lapic_enabled++; } break;      /* bit 0 enabled, bit 1 online capable */
        case 1:  if (elen >= 12 && H.nioapic < MAX_IOAPIC) { H.ioapic[H.nioapic].id = e[2]; H.ioapic[H.nioapic].addr = rd32(e + 4); H.ioapic[H.nioapic].gsi = rd32(e + 8); H.nioapic++; } break;
        case 2:  if (elen >= 10) {
                     if (H.nover < MAX_OVER) { H.over[H.nover].bus = e[2]; H.over[H.nover].src = e[3]; H.over[H.nover].gsi = rd32(e + 4); H.over[H.nover].flags = rd16(e + 8); }
                     H.nover++;
                 }
                 break;
        case 3: case 4: H.nmi++; break;
        case 9:  if (elen >= 16) { H.x2apic_total++; if (rd32(e + 8) & 3) H.x2apic_enabled++; } break;
        default: break;
        }
        off += elen;
    }
}

/* Ids the AML names that matter to an operating system without an interpreter (which this kernel does not have): the AML is only looked
 * at as bytes. An id appears either as a string ("ACPI0003") or, for the PNP vendor, as the compressed EISA id that the EisaId() macro
 * makes (a DWordPrefix 0Ch, then 41h D0h and the four hex digits of the product as two bytes). A hint that a device is described, not proof
 * that it works or that its method would run. */
static const struct { const char *id; const char *name; u8 eisa[2]; } g_aml_ids[] = {
    { "PNP0C0A", "battery", { 0x0C, 0x0A } },           { "PNP0C0D", "lid", { 0x0C, 0x0D } },
    { "PNP0C0C", "power-button", { 0x0C, 0x0C } },      { "PNP0C0E", "sleep-button", { 0x0C, 0x0E } },
    { "PNP0C09", "embedded-controller", { 0x0C, 0x09 } }, { "PNP0C0B", "fan", { 0x0C, 0x0B } },
    { "ACPI0003", "ac-adapter", { 0, 0 } },             { "PNP0C50", "i2c-hid", { 0x0C, 0x50 } },
    { "ACPI0C50", "i2c-hid", { 0, 0 } },                { "PNP0303", "ps2-keyboard", { 0x03, 0x03 } },
    { "PNP0F13", "ps2-mouse", { 0x0F, 0x13 } },
};
#define NAML ((int)ARRAY_SIZE(g_aml_ids))

static bool name_seg_char(u8 c, bool first) { return c == '_' || (c >= 'A' && c <= 'Z') || (!first && c >= '0' && c <= '9'); }

static void scan_aml(const u8 *t) {
    u32 len = rd32(t + 4);
    H.aml_tables++;
    for (u32 i = 36; i + 8 < len; i++) {
        u8 c = t[i];
        if (c == 'P' || c == 'A') {                                     /* the string forms */
            for (int k = 0; k < NAML; k++) {
                const char *id = g_aml_ids[k].id;
                size_t il = strlen(id);
                if (i + il + 1 < len && !memcmp(t + i, id, il) && t[i + il] == 0) H.aml_found |= 1u << k;
            }
        } else if (c == 0x0C && t[i + 1] == 0x41 && t[i + 2] == 0xD0) {   /* DWordPrefix, "PNP" */
            for (int k = 0; k < NAML; k++)
                if (g_aml_ids[k].eisa[0] && t[i + 3] == g_aml_ids[k].eisa[0] && t[i + 4] == g_aml_ids[k].eisa[1]) H.aml_found |= 1u << k;
        } else if (c == 0x5B && t[i + 1] == 0x85) {                     /* ThermalZoneOp, PkgLength, NameSeg */
            const u8 *p = t + i + 2;
            p += 1 + (*p >> 6);
            if (p + 4 < t + len && name_seg_char(p[0], true) && name_seg_char(p[1], false) && name_seg_char(p[2], false) && name_seg_char(p[3], false))
                H.thermal_zones++;
        }
    }
}

static void walk_acpi(void) {
    u64 rsdp = g_boot.rsdp_phys;
    if (!rsdp) return;
    const u8 *r = map_phys(rsdp, 36);
    if (!r || memcmp(r, "RSD PTR ", 8)) return;
    H.rsdp_rev = r[15];
    trim_copy(H.rsdp_oem, r + 9, 6);
    H.xsdt = r[15] >= 2 && rd64(r + 24);
    const u8 *root = map_table(H.xsdt ? rd64(r + 24) : rd32(r + 16));
    if (!root || (memcmp(root, "XSDT", 4) && memcmp(root, "RSDT", 4))) return;
    H.have = true;
    note_table(root);
    size_t esize = !memcmp(root, "XSDT", 4) ? 8 : 4;
    size_t n = (rd32(root + 4) - 36) / esize;
    for (size_t i = 0; i < n; i++) {
        u64 phys = esize == 8 ? rd64(root + 36 + i * 8) : rd32(root + 36 + i * 4);
        const u8 *t = map_table(phys);
        if (!t) continue;
        note_table(t);
        if (!memcmp(t, "APIC", 4)) parse_madt(t);
        else if (!memcmp(t, "SSDT", 4)) scan_aml(t);
        else if (!memcmp(t, "FACP", 4)) {
            u32 flen = rd32(t + 4);
            u64 dsdt = flen >= 44 ? rd32(t + 40) : 0;
            if (flen >= 148 && rd64(t + 140)) dsdt = rd64(t + 140);
            const u8 *d = dsdt ? map_table(dsdt) : NULL;
            if (d && !memcmp(d, "DSDT", 4)) { note_table(d); scan_aml(d); }
        }
    }
}

/* ------------------------------------------------------------------ /proc/hwinfo */

static const char *sb_name(void) { return g_boot.secure_boot == BOOT_SB_ON ? "on" : g_boot.secure_boot == BOOT_SB_OFF ? "off" : "unknown"; }

static const char *gop_format(unsigned f) {
    static const char *const n[] = { "rgb", "bgr", "bitmask", "blt-only" };
    return f < 4 ? n[f] : "unknown";
}

static void hw_firmware(Sink *s) {
    if (g_boot.version < 6) { put(s, "firmware: not available (the loader is older than the kernel and does not pass it on)\n"); return; }
    put(s, "firmware.vendor: %s\n", g_boot.fw_vendor[0] ? g_boot.fw_vendor : "unknown");
    put(s, "firmware.revision: %#x\n", g_boot.fw_revision);
    u32 rev = g_boot.uefi_revision;
    put(s, "firmware.uefi: %u.%u.%u\n", rev >> 16, (rev & 0xFFFF) / 10, (rev & 0xFFFF) % 10);
    put(s, "firmware.secureboot: %s\n", sb_name());
    if (g_boot.setup_mode != 0xFF) put(s, "firmware.setup_mode: %s\n", g_boot.setup_mode ? "yes (no platform key enrolled)" : "no");
    if (g_boot.fb.base) {
        put(s, "gop.in_use: %ux%u, %s, pitch %u, at %#lx\n", g_boot.fb.width, g_boot.fb.height, g_boot.fb.format == BOOTFB_RGBX ? "rgbx" : "bgrx",
            g_boot.fb.pitch, (unsigned long)g_boot.fb.base);
    } else {
        put(s, "gop.in_use: none (no usable graphics output)\n");
    }
    put(s, "gop.modes: %u offered, %u listed\n", g_boot.gop_count, g_boot.gop_listed);
    for (unsigned i = 0; i < g_boot.gop_listed && i < BOOT_GOP_MAX; i++)
        put(s, "gop.mode.%u: %ux%u %s%s\n", g_boot.gop[i].index, g_boot.gop[i].w, g_boot.gop[i].h, gop_format(g_boot.gop[i].format),
            g_boot.gop[i].index == g_boot.gop_current ? " (current)" : "");
}

/* The memory map as the loader handed it over, summed by type. Taken early: pmm_reclaim_boot_memory() later turns the loader's own memory into
 * ordinary free RAM, and the report should show what the firmware said, not what the allocator made of it. */
static const struct { u32 type; const char *name; } g_mem_kinds[] = {
    { BOOTMEM_USABLE, "usable" }, { BOOTMEM_BOOT_RECLAIMABLE, "loader" }, { BOOTMEM_ACPI_RECLAIMABLE, "acpi-reclaim" },
    { BOOTMEM_ACPI_NVS, "acpi-nvs" }, { BOOTMEM_RESERVED, "reserved" }, { BOOTMEM_MMIO, "device" }, { BOOTMEM_KERNEL, "kernel" },
    { BOOTMEM_MODULE, "initrd" },
};
static struct { u64 bytes; u32 regions; } g_mem_sum[ARRAY_SIZE(g_mem_kinds)];
static u32 g_mem_regions;
static u64 g_mem_top;

static void snapshot_memory(void) {
    u32 n;
    const BootMemRegion *r = boot_mmap(&n);
    g_mem_regions = n;
    for (u32 i = 0; i < n; i++) {
        u32 t = r[i].type;
        for (unsigned k = 0; k < ARRAY_SIZE(g_mem_kinds); k++)
            if (g_mem_kinds[k].type == t) { g_mem_sum[k].bytes += r[i].length; g_mem_sum[k].regions++; }
        bool ram = t == BOOTMEM_USABLE || t == BOOTMEM_BOOT_RECLAIMABLE || t == BOOTMEM_ACPI_RECLAIMABLE || t == BOOTMEM_KERNEL || t == BOOTMEM_MODULE;
        if (ram && r[i].base + r[i].length > g_mem_top) g_mem_top = r[i].base + r[i].length;
    }
}

static void hw_memory(Sink *s) {
    for (unsigned k = 0; k < ARRAY_SIZE(g_mem_kinds); k++)
        put(s, "memory.map.%s: %lu KiB in %u region%s\n", g_mem_kinds[k].name, (unsigned long)(g_mem_sum[k].bytes >> 10), g_mem_sum[k].regions,
            g_mem_sum[k].regions == 1 ? "" : "s");
    put(s, "memory.regions: %u (as the loader handed them over)\n", g_mem_regions);
    put(s, "memory.ram_top: %#lx\n", (unsigned long)g_mem_top);
    put(s, "memory.managed: %lu KiB (what the kernel's allocator owns now)\n", (unsigned long)(pmm_total_pages() * 4));
}

/* "battery lid thermal-zone(3)": the well-known devices the AML mentions, each name once */
static void aml_names(char *names, size_t cap) {
    size_t k = 0;
    names[0] = 0;
    for (int i = 0; i < NAML; i++) {
        if (!(H.aml_found & (1u << i)) || strstr(names, g_aml_ids[i].name)) continue;
        k += (size_t)snprintf(names + k, cap - k, "%s%s", k ? " " : "", g_aml_ids[i].name);
        if (k >= cap) k = cap - 1;
    }
    if (H.thermal_zones) k += (size_t)snprintf(names + k, cap - k, "%sthermal-zone(%d)", k ? " " : "", H.thermal_zones);
    if (k >= cap) k = cap - 1;
    names[k] = 0;
}

static void hw_acpi(Sink *s) {
    if (!H.have) { put(s, "acpi: not available (the firmware gave no usable tables)\n"); return; }
    put(s, "acpi.rsdp: revision %u, oem %s, root table %s\n", H.rsdp_rev, H.rsdp_oem, H.xsdt ? "XSDT" : "RSDT");
    put(s, "acpi.tables: %d\n", H.ntab);
    for (int i = 0; i < H.ntab; i++)
        put(s, "acpi.table.%d: %s rev %u oem %s oemtable %s oemrev %#x len %u\n", i, H.tab[i].sig, H.tab[i].rev, H.tab[i].oem[0] ? H.tab[i].oem : "-",
            H.tab[i].oemtab[0] ? H.tab[i].oemtab : "-", H.tab[i].oemrev, H.tab[i].len);
    if (!H.have_madt) { put(s, "acpi.madt: not present\n"); return; }
    put(s, "acpi.madt.lapic_address: %#x\n", H.lapic_addr);
    put(s, "acpi.madt.pcat_compat: %s\n", (H.madt_flags & 1) ? "yes (dual 8259 present)" : "no");
    put(s, "acpi.madt.lapics: %d (%d enabled)\n", H.lapic_total, H.lapic_enabled);
    if (H.x2apic_total) put(s, "acpi.madt.x2apics: %d (%d enabled)\n", H.x2apic_total, H.x2apic_enabled);
    put(s, "acpi.madt.nmi: %d\n", H.nmi);
    put(s, "acpi.madt.ioapics: %d\n", H.nioapic);
    for (int i = 0; i < H.nioapic; i++) put(s, "acpi.madt.ioapic.%d: id %u at %#x, interrupts from %u\n", i, H.ioapic[i].id, H.ioapic[i].addr, H.ioapic[i].gsi);
    put(s, "acpi.madt.overrides: %d\n", H.nover);
    for (int i = 0; i < H.nover && i < MAX_OVER; i++)
        put(s, "acpi.madt.override.%d: bus %u irq %u -> gsi %u flags %#x\n", i, H.over[i].bus, H.over[i].src, H.over[i].gsi, H.over[i].flags);
    char names[160];
    aml_names(names, sizeof names);
    put(s, "acpi.aml.devices: %s\n", names[0] ? names : "none");
    put(s, "acpi.aml.note: found by reading the bytes of %d DSDT/SSDT tables, not by running them (this kernel has no AML interpreter): a hint that a device is described\n", H.aml_tables);
}

static void hw_interrupts(Sink *s) {
    /* What the kernel does today (kernel/arch/x86_64/irq.c, lapic.c, docs/HARDWARE_AUDIT.md C10): the 8259 pair is the interrupt controller,
     * reached through the local APIC's virtual wire; every I/O APIC input is masked at start-up; devices are polled; one processor runs.
     * Change these lines when MSI, I/O APIC routing or SMP arrive. */
    put(s, "interrupts.controller: 8259 PIC through the local APIC virtual wire (LINT0 ExtINT)\n");
    u32 addrs[MAX_IOAPIC];
    put(s, "interrupts.ioapics: %d found, every input masked, none routed\n", acpi_ioapics(addrs, MAX_IOAPIC));
    put(s, "interrupts.msi: not used (devices are polled)\n");
    put(s, "interrupts.processors: %d in the MADT, only the boot processor is used\n", acpi_cpu_count());
}

static size_t gen_hwinfo(char *buf, size_t cap) {
    Sink s = { buf, cap, 0 };
    buf[0] = 0;
    hw_firmware(&s);
    hw_memory(&s);
    put(&s, "tsc.mhz: %lu\n", (unsigned long)timer_tsc_mhz());
    hw_acpi(&s);
    hw_interrupts(&s);
    return s.n;
}

/* ------------------------------------------------------------------ /proc/pcicaps */

static const char *link_speed(unsigned code) {
    switch (code) {
    case 1: return "2.5GT/s";   case 2: return "5GT/s";     case 3: return "8GT/s";
    case 4: return "16GT/s";    case 5: return "32GT/s";    case 6: return "64GT/s";
    default: return NULL;
    }
}

static void put_link(Sink *s, const char *key, unsigned speed, unsigned width) {
    const char *sp = link_speed(speed);
    if (sp && width) put(s, " %s=%s:x%u", key, sp, width);
    else put(s, " %s=-", key);
}

static size_t gen_pcicaps(char *buf, size_t cap) {
    Sink s = { buf, cap, 0 };
    buf[0] = 0;
    for (PciDev *d = pci_first(); d && s.n + 120 < cap; d = pci_next(d)) {
        u8 msi = pci_find_cap(d, PCI_CAP_MSI), msix = pci_find_cap(d, PCI_CAP_MSIX), pcie = pci_find_cap(d, PCI_CAP_PCIE);
        put(&s, "%02x:%02x.%x: %04x:%04x msi=%s msix=%s pcie=%s", d->bus, d->dev, d->fn, d->vendor, d->device, msi ? "yes" : "no", msix ? "yes" : "no", pcie ? "yes" : "no");
        if (pcie) {
            u32 cap_reg = pci_read32(d, (u16)(pcie + 0x0C));
            u16 status = pci_read16(d, (u16)(pcie + 0x12));
            put_link(&s, "link", status & 0xF, (status >> 4) & 0x3F);
            put_link(&s, "max", cap_reg & 0xF, (cap_reg >> 4) & 0x3F);
        }
        put(&s, "\n");
    }
    return s.n;
}

/* ------------------------------------------------------------------ start-up */

/* After acpi_init() and before pmm_reclaim_boot_memory(): the SMBIOS copy and the ACPI tables may be in memory the allocator gets back. */
void hwinfo_early(void) {
    snapshot_memory();
    dmi_init();
    walk_acpi();
    procfs_register("hwinfo", gen_hwinfo);
    procfs_register("pcicaps", gen_pcicaps);
    char names[160];
    aml_names(names, sizeof names);
    klog("Gensokyo", "hwinfo: %d ACPI tables listed (%d processors, %d I/O APICs in the MADT); the AML mentions: %s", H.ntab, H.lapic_enabled + H.x2apic_enabled,
         H.nioapic, names[0] ? names : "no well-known device");
}
