/*
 * ACPI, the small version. The tables are read once at boot:
 *
 *   RSDP -> XSDT (or RSDT) -> FADT   power registers (PM1a/PM1b control blocks), the reset register
 *                          -> DSDT   the \_S5 object: the sleep type that means "soft off"
 *                          -> MADT   how many processors there are (only the boot one is used)
 *
 * There is no AML interpreter: _S5 is found by scanning the DSDT for the byte pattern that every firmware uses.
 * Real machines (unlike QEMU's fixed ports) need this to power off, and many only reset through the FADT.
 */
#include "kernel.h"
#include "acpi.h"
#include "arch.h"
#include "cmdline.h"
#include "cpu.h"
#include "mm.h"
#include "vfs.h"
#include "bootlog.h"
#include "initcall.h"
#include "input.h"
#include "task.h"

typedef struct {
    char sig[8]; u8 checksum; char oem[6]; u8 revision; u32 rsdt; u32 length; u64 xsdt; u8 ext_checksum; u8 reserved[3];
} PACKED Rsdp;

typedef struct {
    char sig[4]; u32 length; u8 revision; u8 checksum; char oem[6]; char oem_table[8]; u32 oem_rev; u32 creator; u32 creator_rev;
} PACKED SdtHeader;

typedef struct { u8 space, width, offset, access; u64 address; } PACKED Gas;     /* generic address structure */

enum { GAS_MEMORY = 0, GAS_IO = 1, GAS_PCI = 2 };

#define PM1_SLP_EN      (1u << 13)
#define PM1_SLP_TYP(v)  ((u16)(((v) & 7u) << 10))
#define PM1_SCI_EN      1u
#define FADT_RESET_SUP  (1u << 10)

static struct {
    bool present;
    u64 rsdp;
    u8 revision;
    char oem[7];
    u16 pm1a_cnt, pm1b_cnt;             /* I/O ports, 0 = none */
    u16 pm1a_evt, pm1b_evt;             /* PM1 event blocks: status at the port, enable halfway into the block */
    u8 pm1_evt_len;
    bool button_fixed;                  /* the power button is fixed hardware (a status bit we can poll) */
    u32 smi_cmd;
    u8 acpi_enable;
    bool have_s5;
    u8 s5_a, s5_b;
    bool reset_ok;
    Gas reset_reg;
    u8 reset_value;
    int cpus;
    u32 ioapic[8];                      /* MADT: register window addresses of the I/O APICs */
    int nioapic;
    u16 pm_tmr;                         /* PM timer I/O port (3.579545 MHz), 0 = none */
    bool tmr_wide;                      /* the timer counts 32 bits instead of 24 */
    u16 gpe0_blk;                       /* general-purpose event block 0 (I/O): status bytes, then as many enable bytes; 0 = none */
    u8 gpe0_len;                        /* its length in bytes (both halves) */
    bool pme_armed;                     /* acpi_wake_arm() enabled the card's wake event (PME_B0) */
    const SdtHeader *aml[10];           /* the DSDT and the SSDTs, kept mapped: wake-up events are looked up in them (acpi_wake_arm) */
    int naml;
    int bad_checksums, tables;          /* tables seen / tables whose checksum did not add up (used anyway, like Linux does) */
} A;

static bool ram_mapped(u64 phys, u64 len) {
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

/* The kernel's direct map only covers RAM-like regions; firmware sometimes parks tables elsewhere. */
static const void *acpi_map(u64 phys, u64 len) {
    if (!phys || phys + len < phys || phys + len > (1ULL << 48)) return NULL;       /* a garbage pointer must not become a mapping */
    if (ram_mapped(phys, len)) return phys_to_virt(phys);
    return ioremap(phys, len, PTE_PCD);                 /* firmware memory outside the direct map: uncached, in case it is device memory after all */
}

static bool checksum_ok(const void *p, size_t len) {
    u8 sum = 0;
    for (size_t i = 0; i < len; i++) sum = (u8)(sum + ((const u8 *)p)[i]);
    return sum == 0;
}

/* Maps a whole table; NULL if the header is nonsense. */
static const SdtHeader *map_table(u64 phys) {
    const SdtHeader *h = acpi_map(phys, sizeof(SdtHeader));
    if (!h || h->length < sizeof(SdtHeader) || h->length > (16u << 20)) return NULL;
    const SdtHeader *t = acpi_map(phys, h->length);
    if (t) {
        A.tables++;
        if (!checksum_ok(t, h->length)) A.bad_checksums++;      /* firmware with a wrong sum is common; the table is still read */
    }
    return t;
}

static bool sig_is(const SdtHeader *h, const char *s) { return !memcmp(h->sig, s, 4); }

/* \_S5 is a package of the sleep types for PM1a and PM1b: Name(_S5_, Package(){ a, b, 0, 0 }). */
static bool find_s5(const SdtHeader *t) {
    const u8 *p = (const u8 *)t, *end = p + t->length;
    for (const u8 *q = p + sizeof(SdtHeader); q + 8 < end; q++) {
        if (memcmp(q, "_S5_", 4)) continue;
        const u8 *before = q - 1;
        bool named = *before == 0x08 || (before > p && before[-1] == 0x08 && *before == '\\');     /* NameOp [RootChar] */
        if (!named || q[4] != 0x12) continue;                                                      /* PackageOp */
        const u8 *s = q + 5;
        s += ((*s & 0xC0) >> 6) + 2;                                                               /* PkgLength bytes + NumElements */
        if (s + 4 >= end) continue;
        if (*s == 0x0A) s++;                                                                       /* BytePrefix */
        A.s5_a = *s++;
        if (*s == 0x0A) s++;
        A.s5_b = *s;
        A.have_s5 = true;
        return true;
    }
    return false;
}

static void parse_fadt(const SdtHeader *t) {
    const u8 *b = (const u8 *)t;
    u32 len = t->length;
    A.revision = t->revision;
    if (len >= 52) A.smi_cmd = *(const u32 *)(b + 48);
    if (len >= 54) A.acpi_enable = b[52];
    u64 pm1a = len >= 68 ? *(const u32 *)(b + 64) : 0, pm1b = len >= 72 ? *(const u32 *)(b + 68) : 0;
    if (len >= 184 + 12) {                                            /* the extended GAS fields win when they describe I/O ports */
        const Gas *xa = (const Gas *)(b + 172), *xb = (const Gas *)(b + 184);
        if (xa->address && xa->space == GAS_IO) pm1a = xa->address;
        if (xb->address && xb->space == GAS_IO) pm1b = xb->address;
    }
    A.pm1a_cnt = pm1a <= 0xFFFF ? (u16)pm1a : 0;
    A.pm1b_cnt = pm1b <= 0xFFFF ? (u16)pm1b : 0;
    u32 flags = len >= 116 ? *(const u32 *)(b + 112) : 0;
    u32 tmr = len >= 80 ? *(const u32 *)(b + 76) : 0;                  /* PM_TMR_BLK */
    if (len >= 208 + 12) {
        const Gas *xt = (const Gas *)(b + 208);                       /* X_PM_TMR_BLK */
        if (xt->address && xt->space == GAS_IO) tmr = (u32)xt->address;
    }
    A.pm_tmr = tmr && tmr <= 0xFFFF ? (u16)tmr : 0;
    A.tmr_wide = (flags & (1u << 8)) != 0;                             /* TMR_VAL_EXT */
    u32 gpe0 = len >= 84 ? *(const u32 *)(b + 80) : 0;                 /* GPE0_BLK, GPE0_BLK_LEN */
    A.gpe0_len = len >= 93 ? b[92] : 0;
    if (len >= 220 + 12) {
        const Gas *xg = (const Gas *)(b + 220);                       /* X_GPE0_BLK */
        if (xg->address && xg->space == GAS_IO) gpe0 = (u32)xg->address;
    }
    A.gpe0_blk = gpe0 && gpe0 <= 0xFFFF ? (u16)gpe0 : 0;
    A.pm1a_evt = len >= 60 && *(const u32 *)(b + 56) <= 0xFFFF ? (u16)*(const u32 *)(b + 56) : 0;
    A.pm1b_evt = len >= 64 && *(const u32 *)(b + 60) <= 0xFFFF ? (u16)*(const u32 *)(b + 60) : 0;
    A.pm1_evt_len = len >= 89 ? b[88] : 4;
    A.button_fixed = !(flags & (1u << 4));                           /* FADT flag PWR_BUTTON set: a control method, which needs AML */
    if ((flags & FADT_RESET_SUP) && len >= 129) {
        memcpy(&A.reset_reg, b + 116, sizeof A.reset_reg);
        A.reset_value = b[128];
        A.reset_ok = A.reset_reg.space == GAS_IO || A.reset_reg.space == GAS_PCI;
    }
    u64 dsdt = len >= 44 ? *(const u32 *)(b + 40) : 0;
    if (len >= 148) { u64 x = *(const u64 *)(b + 140); if (x) dsdt = x; }
    const SdtHeader *d = dsdt ? map_table(dsdt) : NULL;
    if (d && sig_is(d, "DSDT")) {
        find_s5(d);
        if (A.naml < 10) A.aml[A.naml++] = d;
    }
}

static void parse_madt(const SdtHeader *t) {
    const u8 *p = (const u8 *)t + 44, *end = (const u8 *)t + t->length;
    while (p + 2 <= end && p[1] >= 2 && p + p[1] <= end) {
        if (p[0] == 0 && p[1] >= 8 && (*(const u32 *)(p + 4) & 3)) A.cpus++;               /* processor local APIC: enabled / online-capable */
        else if (p[0] == 9 && p[1] >= 16 && (*(const u32 *)(p + 8) & 3)) A.cpus++;         /* x2APIC */
        else if (p[0] == 1 && p[1] >= 12 && A.nioapic < 8) A.ioapic[A.nioapic++] = *(const u32 *)(p + 4);     /* I/O APIC: its register window */
        p += p[1];
    }
}

/* The I/O APICs the firmware describes (physical addresses of their register windows); returns how many, at most `max`. */
int acpi_ioapics(u32 *addrs, int max) {
    int n = A.nioapic < max ? A.nioapic : max;
    for (int i = 0; i < n; i++) addrs[i] = A.ioapic[i];
    return n;
}

/* `n` bytes read from I/O ports, as "11 22 33" (the event blocks, for the log). */
static void hex_bytes(u16 port, u16 n, char *out, size_t cap) {
    size_t o = 0;
    for (u16 i = 0; i < n && o + 4 < cap; i++) o += (size_t)snprintf(out + o, cap - o, "%s%02x", i ? " " : "", inb((u16)(port + i)));
    if (!n && cap) out[0] = 0;
}

void acpi_init(void) {
    u64 rsdp_phys = g_boot.rsdp_phys;
    if (!rsdp_phys) { klog("Gensokyo", "acpi: the firmware did not provide any tables"); return; }
    const Rsdp *r = acpi_map(rsdp_phys, sizeof(Rsdp));
    if (!r || memcmp(r->sig, "RSD PTR ", 8) || !checksum_ok(r, 20)) { klog("Gensokyo", "acpi: bad RSDP at %#lx, ignoring ACPI", (unsigned long)rsdp_phys); return; }
    A.rsdp = rsdp_phys;
    if (r->revision >= 2 && !checksum_ok(r, sizeof(Rsdp))) klog("Gensokyo", "acpi: the RSDP's extended checksum is wrong (used anyway)");
    memcpy(A.oem, r->oem, 6);
    A.oem[6] = 0;

    bool use_xsdt = r->revision >= 2 && r->xsdt;
    const SdtHeader *root = map_table(use_xsdt ? r->xsdt : r->rsdt);
    if (!root || (!sig_is(root, "XSDT") && !sig_is(root, "RSDT"))) { klog("Gensokyo", "acpi: no usable root table"); return; }
    size_t esize = sig_is(root, "XSDT") ? 8 : 4;
    size_t n = (root->length - sizeof(SdtHeader)) / esize;
    const u8 *entries = (const u8 *)root + sizeof(SdtHeader);
    for (size_t i = 0; i < n; i++) {
        u64 phys = esize == 8 ? ((const u64 *)entries)[i] : ((const u32 *)entries)[i];      /* unaligned reads are fine on x86 */
        const SdtHeader *t = map_table(phys);
        if (!t) continue;
        if (sig_is(t, "FACP")) { parse_fadt(t); A.present = true; }
        else if (sig_is(t, "APIC")) parse_madt(t);
        else if (sig_is(t, "SSDT")) {
            if (!A.have_s5) find_s5(t);                                                      /* a few firmwares keep _S5 here */
            if (A.naml < 10) A.aml[A.naml++] = t;
        }
    }
    char rst[48];
    acpi_describe_reset(rst, sizeof rst);
    klog("Gensokyo", "acpi: rev %u, oem '%s', %d processor%s, PM1a_CNT %#x, S5 %s, reset register: %s", A.revision, A.oem, A.cpus,
         A.cpus == 1 ? "" : "s", A.pm1a_cnt, A.have_s5 ? "known" : "unknown", rst);
    klog("Gensokyo", "acpi: %d tables read via the %s%s, PM timer %#x", A.tables, use_xsdt ? "XSDT" : "RSDT",
         A.bad_checksums ? " (some have a bad checksum; used anyway)" : "", A.pm_tmr);
    if (A.pm1a_evt) {                                       /* what woke the machine: the status bits the firmware left (the power button, a card's PME ...) */
        u16 half = (u16)(A.gpe0_len / 2);
        char gs[56] = "", ge[56] = "";
        if (A.gpe0_blk && half) { hex_bytes(A.gpe0_blk, half, gs, sizeof gs); hex_bytes((u16)(A.gpe0_blk + half), half, ge, sizeof ge); }
        klog("Gensokyo", "acpi: wake status at start: PM1 status %04x enable %04x; event block %#x status %s, enable %s", inw(A.pm1a_evt), inw((u16)(A.pm1a_evt + A.pm1_evt_len / 2)),
             A.gpe0_blk, gs, ge);
    }
}

/* The ACPI PM timer, for calibrating the TSC when the PIT is not there. */
bool acpi_pm_timer(u16 *port, bool *wide) {
    if (!A.present || !A.pm_tmr) return false;
    *port = A.pm_tmr;
    *wide = A.tmr_wide;
    return true;
}

int acpi_cpu_count(void) { return A.cpus; }

static void settle(void) { for (int i = 0; i < 200000; i++) { inb(0x80); } }

bool acpi_poweroff(void) {
    if (!A.present || !A.pm1a_cnt || !A.have_s5) return false;
    if (!(inw(A.pm1a_cnt) & PM1_SCI_EN) && A.smi_cmd && A.acpi_enable) {          /* hand the hardware over to ACPI first */
        outb((u16)A.smi_cmd, A.acpi_enable);
        for (int i = 0; i < 300 && !(inw(A.pm1a_cnt) & PM1_SCI_EN); i++) { for (int j = 0; j < 1000; j++) inb(0x80); }
    }
    u16 v = inw(A.pm1a_cnt);
    v = (u16)((v & ~(7u << 10)) | PM1_SLP_TYP(A.s5_a) | PM1_SLP_EN);
    outw(A.pm1a_cnt, v);
    if (A.pm1b_cnt) {
        u16 w = inw(A.pm1b_cnt);
        w = (u16)((w & ~(7u << 10)) | PM1_SLP_TYP(A.s5_b) | PM1_SLP_EN);
        outw(A.pm1b_cnt, w);
    }
    settle();
    return false;                                                                  /* still running: the caller tries the next way */
}

bool acpi_can_poweroff(void) { return A.present && A.pm1a_cnt && A.have_s5; }

/* Is the FADT's reset register the SMI command port (the Dell OptiPlex 7050 says io 0xb2 = 0x73)? Writing it is a software SMI: the firmware's handler is expected to reset the
 * machine, and on that Dell it never comes back (a system management interrupt wants every processor to arrive in SMM, and the other processors were put in the INIT state at
 * boot). Such a register is not part of the default restart order (`reboot acpi` and `reboot=acpi` still use it). "acpi.resetsmi" on the command line pretends, for the tests. */
bool acpi_reset_is_smi(void) {
    if (!A.reset_ok || A.reset_reg.space != GAS_IO) return false;
    return A.reset_reg.address == 0xB2 || (A.smi_cmd && A.reset_reg.address == A.smi_cmd) || cmdline_flag("acpi.resetsmi");
}

void acpi_describe_reset(char *out, size_t n) {
    if (!A.reset_ok) { snprintf(out, n, "%s", A.present ? "none the kernel can use" : "no ACPI"); return; }
    snprintf(out, n, "%s %#lx = %#x", A.reset_reg.space == GAS_IO ? "io" : "pci", (unsigned long)A.reset_reg.address, A.reset_value);
}

bool acpi_reset(void) {
    if (!A.reset_ok) return false;
    u64 addr = A.reset_reg.address;
    if (A.reset_reg.space == GAS_IO && addr <= 0xFFFF) {
        outb((u16)addr, A.reset_value);
    } else if (A.reset_reg.space == GAS_PCI) {                                      /* address: 0 | device << 32 | function << 16 | register */
        u32 dev = (u32)(addr >> 32) & 0x1F, fn = (u32)(addr >> 16) & 7, reg = (u32)addr & 0xFC;
        outl(0xCF8, 0x80000000u | (dev << 11) | (fn << 8) | reg);
        outb((u16)(0xCFC + (addr & 3)), A.reset_value);
    } else {
        return false;
    }
    settle();
    return false;
}

/* ---------------------------------------------------------------- Wake on LAN, the chipset's half (the card's is e_shutdown() in drivers/e1000.c) */

/* The LAN controller built into the Intel PCH (the Dell's I219) has no wake pin of its own: when it raises its power-management event (PME) the chipset latches "PME_B0" in the
 * general-purpose event block, and an operating system that wants the machine woken by it sets that event's enable bit before it goes to sleep. The firmware names the event in
 * the device's _PRW object: 0x6D on the Intel PCH, status and enable bit 109 of the block, which is byte 13, bit 5. There is no AML interpreter here, so the tables are only searched
 * for _PRW packages whose first element is a one-byte number, and what they name is logged beside the block's bytes before and after: if the machine does not wake, the next
 * guess is made from that line. A stale status bit is cleared first (write 1): a latched one would count as a wake event the moment the machine is off. */
#define GPE_PME_B0 0x6D

void acpi_wake_arm(bool nic_armed) {
    if (!nic_armed) return;
    if (!A.present || !A.gpe0_blk || A.gpe0_len < 2) { klog("Gensokyo", "wake-up: the firmware describes no event block: the chipset cannot be told to listen"); return; }
    u16 half = (u16)(A.gpe0_len / 2), sts = A.gpe0_blk, en = (u16)(A.gpe0_blk + half);
    u16 byte = GPE_PME_B0 / 8;
    u8 bit = (u8)(1u << (GPE_PME_B0 % 8));
    char named[80];
    size_t o = 0;
    int found = 0;
    named[0] = 0;
    for (int t = 0; t < A.naml; t++) {                                  /* the _PRW objects: NameOp "_PRW" PackageOp PkgLength NumElements BytePrefix event ... */
        const u8 *p = (const u8 *)A.aml[t], *end = p + A.aml[t]->length;
        for (const u8 *q = p + sizeof(SdtHeader) + 1; q + 10 < end; q++) {
            u8 ev;
            if (q[0] == '_' && !memcmp(q, "_PRW", 4) && q[-1] == 0x08 && q[4] == 0x12 && !(q[5] & 0xC0) && q[6] >= 2 && q[7] == 0x0A) ev = q[8];                     /* Name (_PRW, Package () { ev, ... }) */
            else if (q[0] == 'G' && !memcmp(q, "GPRW", 4) && q[-1] == 0xA4 && q[4] == 0x0A && q[6] == 0x0A) ev = q[5];                                           /* Return (GPRW (ev, state)), the Intel reference code's helper */
            else continue;
            char one[8];
            snprintf(one, sizeof one, "%02x", ev);
            if (strstr(named, one) || o + 4 >= sizeof named) continue;                      /* (a number seen already, or no room) */
            o += (size_t)snprintf(named + o, sizeof named - o, "%s%s", o ? " " : "", one);
            found++;
        }
    }
    if (byte >= half) { klog("Gensokyo", "wake-up: event %#x is outside the firmware's %u-byte event block at %#x (_PRW events: %s)", GPE_PME_B0, (unsigned)half, sts, found ? named : "none found"); return; }
    char s0[56], e0[56];
    hex_bytes(sts, half, s0, sizeof s0);
    hex_bytes(en, half, e0, sizeof e0);
    A.pme_armed = true;
    (void)bit;
    klog("Gensokyo", "wake-up: event %#x (PME_B0) is enabled in the event block at %#x as the last step before the power goes (now: status %s, enable %s); wake events the tables name: %s", GPE_PME_B0,
         sts, s0, e0, found ? named : "none found");
}

/* The last step before the power goes, after the cards have cleared their own wake cause and gone to sleep (netdev_shutdown_all, second call): the event's latched status is cleared (a
 * status left from an earlier wake would count as a new event) and its enable bit is set. Enabling it this late keeps a magic packet during the shutdown from raising an event nobody
 * handles. Nothing checks that the status stays clear: a machine that wakes again at once is back on the network, which is better for a machine nobody sits at than one that stays off. */
void acpi_wake_settle(void) {
    if (!A.pme_armed) return;
    u16 half = (u16)(A.gpe0_len / 2), sts = A.gpe0_blk, en = (u16)(A.gpe0_blk + half);
    u16 byte = GPE_PME_B0 / 8;
    u8 bit = (u8)(1u << (GPE_PME_B0 % 8));
    char s0[56], e0[56], s1[56], e1[56];
    hex_bytes(sts, half, s0, sizeof s0);
    hex_bytes(en, half, e0, sizeof e0);
    outb((u16)(sts + byte), bit);                                       /* forget a latched event */
    outb((u16)(en + byte), (u8)(inb((u16)(en + byte)) | bit));
    hex_bytes(sts, half, s1, sizeof s1);
    hex_bytes(en, half, e1, sizeof e1);
    klog("Gensokyo", "wake-up: event %#x (PME_B0) enabled: status %s -> %s; enable %s -> %s", GPE_PME_B0, s0, s1, e0, e1);
}

static size_t proc_acpi(char *buf, size_t cap) {
    size_t n = 0;
    n += (size_t)snprintf(buf + n, cap - n, "rsdp:        %#lx\n", (unsigned long)A.rsdp);
    n += (size_t)snprintf(buf + n, cap - n, "revision:    %u\n", A.revision);
    n += (size_t)snprintf(buf + n, cap - n, "oem:         %s\n", A.oem);
    n += (size_t)snprintf(buf + n, cap - n, "processors:  %d (only the boot processor is used)\n", A.cpus);
    n += (size_t)snprintf(buf + n, cap - n, "pm1a_cnt:    %#x\n", A.pm1a_cnt);
    n += (size_t)snprintf(buf + n, cap - n, "pm1b_cnt:    %#x\n", A.pm1b_cnt);
    n += (size_t)snprintf(buf + n, cap - n, "poweroff:    %s\n", A.have_s5 ? "S5 (acpi)" : "legacy ports only");
    char rst[48], order[64];
    acpi_describe_reset(rst, sizeof rst);
    restart_order_text(order, sizeof order);
    n += (size_t)snprintf(buf + n, cap - n, "reset:       acpi register %s%s; restart order: %s (`reboot WAY` and \"reboot=\" on the command line choose; efi%s and triple only when named)\n",
                          rst, acpi_reset_is_smi() ? ", the SMI command port: only when named" : "", order, efi_reset_known() ? "" : " (no ResetSystem from the loader)");
    n += (size_t)snprintf(buf + n, cap - n, "button:      %s\n", A.button_fixed ? "polled (fixed hardware)" : "control method (not handled)");
    return n;
}

/* ---------------------------------------------------------------- the power button */

#define PM1_PWRBTN_STS (1u << 8)
#define PM1_PWRBTN_EN  (1u << 8)

/* Polled like every other device in this kernel. With a desktop running the press is delivered as KEY_POWER (the shell
 * asks what to do); without one the machine closes up on its own. */
static void power_button_pressed(void) {
    klog("Gensokyo", "the power button was pressed");
    if (g_input_grab > 0) {
        u64 fl = irq_save();
        input_emit(EV_KEY, KEY_POWER, 1);
        input_emit(EV_KEY, KEY_POWER, 0);
        irq_restore(fl);
        return;
    }
    klog("Gensokyo", "no desktop is running: powering off");
    machine_prepare_poweroff();                                 /* Wake on LAN: before the log goes to the disk */
    bootlog_shutdown();                                         /* the records first: a stick pulled after this finds its volume clean */
    vfs_sync_all();
    machine_poweroff();
}

static void button_thread(void *arg) {
    (void)arg;
    for (;;) {
        task_sleep_ms(200);
        u16 st = inw(A.pm1a_evt);
        if (A.pm1b_evt) st |= inw(A.pm1b_evt);
        if (!(st & PM1_PWRBTN_STS)) continue;
        outw(A.pm1a_evt, PM1_PWRBTN_STS);                           /* write 1 to clear */
        if (A.pm1b_evt) outw(A.pm1b_evt, PM1_PWRBTN_STS);
        power_button_pressed();
    }
}

static void acpi_late_init(void) {
    procfs_register("acpi", proc_acpi);
    if (A.present && A.pm1a_evt && A.button_fixed) {
        u16 en = (u16)(A.pm1a_evt + A.pm1_evt_len / 2);              /* QEMU only latches the press while the enable bit is set */
        outw(en, (u16)(inw(en) | PM1_PWRBTN_EN));
        outw(A.pm1a_evt, PM1_PWRBTN_STS);                           /* forget a press from before we were listening */
        task_create_detached("acpi-button", button_thread, NULL);
    }
}
INITCALL(acpi_late_init);
