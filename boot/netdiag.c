/*
 * netdiag: with "netdiag" on the kernel command line the loader uses the firmware's own network driver (EFI_SIMPLE_NETWORK_PROTOCOL) for a few
 * seconds, before the kernel is loaded, and writes down what happened. It exists for the Dell's Intel I219, whose own driver in the OS never gets a
 * transmit descriptor completed: if the firmware's driver can send a frame (a DHCP request, answered by the router), the card and the cable are fine
 * and the registers the firmware left behind are what a working card looks like, so they are dumped here (and the first transmit descriptors the
 * firmware wrote) and handed to the kernel, which puts the report into its log (TOUHOU.LOG) with a "[Hakurei]" tag.
 *
 * The firmware only has a network driver when its network stack is on (Dell: BIOS setup, "Enable UEFI Network Stack"; the integrated NIC must be
 * enabled). With no SNP the report says so and nothing else happens. The adapter is always shut down and stopped again at the end: a card left
 * running would keep writing received frames into firmware memory that the kernel is about to reuse.
 *
 * It is also the first piece of the idea of updating the OS from the loader, over the firmware's network stack (docs/UPDATE.md).
 */
#include <stdarg.h>
#include "loader.h"

#define REPORT_BYTES  (32 * 1024)

/* ---- EFI_SIMPLE_NETWORK_PROTOCOL (UEFI 2.x, 24.1) ---- */

typedef struct { UINT8 Addr[32]; } EFI_MAC_ADDRESS;
typedef struct EFI_SIMPLE_NETWORK_PROTOCOL EFI_SIMPLE_NETWORK_PROTOCOL;

typedef struct {
    UINT32 State;                       /* 0 stopped, 1 started, 2 initialized */
    UINT32 HwAddressSize;
    UINT32 MediaHeaderSize;
    UINT32 MaxPacketSize;
    UINT32 NvRamSize;
    UINT32 NvRamAccessSize;
    UINT32 ReceiveFilterMask;
    UINT32 ReceiveFilterSetting;
    UINT32 MaxMCastFilterCount;
    UINT32 MCastFilterCount;
    EFI_MAC_ADDRESS MCastFilter[16];
    EFI_MAC_ADDRESS CurrentAddress;
    EFI_MAC_ADDRESS BroadcastAddress;
    EFI_MAC_ADDRESS PermanentAddress;
    UINT8 IfType;
    BOOLEAN MacAddressChangeable;
    BOOLEAN MultipleTxSupported;
    BOOLEAN MediaPresentSupported;
    BOOLEAN MediaPresent;
} EFI_SIMPLE_NETWORK_MODE;

struct EFI_SIMPLE_NETWORK_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Start)(EFI_SIMPLE_NETWORK_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Stop)(EFI_SIMPLE_NETWORK_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Initialize)(EFI_SIMPLE_NETWORK_PROTOCOL *This, UINTN ExtraRxBufferSize, UINTN ExtraTxBufferSize);
    EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_NETWORK_PROTOCOL *This, BOOLEAN ExtendedVerification);
    EFI_STATUS (EFIAPI *Shutdown)(EFI_SIMPLE_NETWORK_PROTOCOL *This);
    EFI_STATUS (EFIAPI *ReceiveFilters)(EFI_SIMPLE_NETWORK_PROTOCOL *This, UINT32 Enable, UINT32 Disable, BOOLEAN ResetMCastFilter,
                                        UINTN MCastFilterCnt, EFI_MAC_ADDRESS *MCastFilter);
    void *StationAddress;
    void *Statistics;
    void *MCastIpToMac;
    void *NvData;
    EFI_STATUS (EFIAPI *GetStatus)(EFI_SIMPLE_NETWORK_PROTOCOL *This, UINT32 *InterruptStatus, void **TxBuf);
    EFI_STATUS (EFIAPI *Transmit)(EFI_SIMPLE_NETWORK_PROTOCOL *This, UINTN HeaderSize, UINTN BufferSize, void *Buffer, EFI_MAC_ADDRESS *SrcAddr,
                                  EFI_MAC_ADDRESS *DestAddr, UINT16 *Protocol);
    EFI_STATUS (EFIAPI *Receive)(EFI_SIMPLE_NETWORK_PROTOCOL *This, UINTN *HeaderSize, UINTN *BufferSize, void *Buffer, EFI_MAC_ADDRESS *SrcAddr,
                                 EFI_MAC_ADDRESS *DestAddr, UINT16 *Protocol);
    void *WaitForPacket;
    EFI_SIMPLE_NETWORK_MODE *Mode;
};

_Static_assert(offsetof(EFI_SIMPLE_NETWORK_PROTOCOL, ReceiveFilters) == 48, "SNP ReceiveFilters offset");
_Static_assert(offsetof(EFI_SIMPLE_NETWORK_PROTOCOL, GetStatus) == 88, "SNP GetStatus offset");
_Static_assert(offsetof(EFI_SIMPLE_NETWORK_PROTOCOL, Transmit) == 96, "SNP Transmit offset");
_Static_assert(offsetof(EFI_SIMPLE_NETWORK_PROTOCOL, Receive) == 104, "SNP Receive offset");
_Static_assert(offsetof(EFI_SIMPLE_NETWORK_PROTOCOL, Mode) == 120, "SNP Mode offset");
_Static_assert(offsetof(EFI_SIMPLE_NETWORK_MODE, CurrentAddress) == 552, "SNP mode CurrentAddress offset");
_Static_assert(offsetof(EFI_SIMPLE_NETWORK_MODE, MediaPresent) == 652, "SNP mode MediaPresent offset");

static EFI_GUID snpGuid = { 0xA19832B9, 0xAC25, 0x11D3, { 0x9A, 0x2D, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };

#define SNP_FILTER_UNICAST   0x01u
#define SNP_FILTER_MULTICAST 0x02u
#define SNP_FILTER_BROADCAST 0x04u

/* ---- the report: everything is shown on the console and kept for the kernel ---- */

static char *g_rep;
static UINTN g_replen;

static void rpf(const char *fmt, ...) {
    char tmp[300];
    UINTN n = 0;
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt && n + 40 < sizeof tmp; fmt++) {
        if (*fmt != '%') { tmp[n++] = *fmt; continue; }
        fmt++;
        int zero = 0, width = 0, lng = 0;
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == 'l') { lng = 1; fmt++; }
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            while (*s && n + 40 < sizeof tmp) tmp[n++] = *s++;
            break;
        }
        case 'u':
        case 'x': {
            UINT64 v = lng ? va_arg(ap, UINT64) : (UINT64)va_arg(ap, UINT32);
            unsigned base = *fmt == 'x' ? 16 : 10;
            char d[24];
            int i = 23;
            d[i] = 0;
            if (!v) d[--i] = '0';
            while (v) { d[--i] = "0123456789abcdef"[v % base]; v /= base; }
            for (int len = 23 - i; len < width; len++) tmp[n++] = zero ? '0' : ' ';
            while (d[i]) tmp[n++] = d[i++];
            break;
        }
        case '%': tmp[n++] = '%'; break;
        default: break;
        }
    }
    va_end(ap);
    tmp[n] = 0;
    log_str(tmp);
    for (UINTN i = 0; i < n && g_replen + 1 < REPORT_BYTES; i++) g_rep[g_replen++] = tmp[i];
    if (g_rep) g_rep[g_replen] = 0;
}

/* ---- PCI configuration space through the legacy ports, and the card's registers ---- */

static inline void outl(UINT16 port, UINT32 v) { __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline UINT32 inl(UINT16 port) { UINT32 v; __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }

static UINT32 pci_cfg(int bus, int dev, int fn, int off) {
    outl(0xCF8, 0x80000000u | ((UINT32)bus << 16) | ((UINT32)dev << 11) | ((UINT32)fn << 8) | (UINT32)(off & 0xFC));
    return inl(0xCFC);
}

typedef struct { int bus, dev, fn; UINT16 vendor, device; UINT64 mmio; } NicLoc;

static int find_nic(NicLoc *out) {
    int found = 0;
    for (int bus = 0; bus < 8; bus++)
        for (int dev = 0; dev < 32; dev++)
            for (int fn = 0; fn < 8; fn++) {
                UINT32 id = pci_cfg(bus, dev, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (fn == 0) break; continue; }
                UINT32 cls = pci_cfg(bus, dev, fn, 8) >> 8;
                if ((cls >> 16) != 0x02) continue;                       /* class 02: network controller */
                UINT32 bar0 = pci_cfg(bus, dev, fn, 0x10);
                UINT64 base = 0;
                if (!(bar0 & 1)) {
                    base = bar0 & ~0xFULL;
                    if ((bar0 & 6) == 4) base |= (UINT64)pci_cfg(bus, dev, fn, 0x14) << 32;
                }
                rpf("netdiag: PCI %02x:%02x.%x %04x:%04x network controller, BAR0 %lx\n", bus, dev, fn, id & 0xFFFF, id >> 16, base);
                if (!found && (id & 0xFFFF) == 0x8086 && base) {
                    out->bus = bus; out->dev = dev; out->fn = fn;
                    out->vendor = (UINT16)(id & 0xFFFF); out->device = (UINT16)(id >> 16); out->mmio = base;
                    found = 1;
                }
            }
    return found;
}

static inline UINT32 reg(const NicLoc *n, UINT32 off) { return *(volatile UINT32 *)(UINTN)(n->mmio + off); }

/* The registers that matter for transmit, in the same names the kernel's driver prints, and the first descriptors of the rings. */
static void dump_nic(const NicLoc *n, const char *tag) {
    rpf("netdiag: %s: CTRL %08x STATUS %08x CTRLEXT %08x TCTL %08x TIPG %08x TXDCTL %08x TARC0 %08x\n", tag, reg(n, 0x0), reg(n, 0x8), reg(n, 0x18), reg(n, 0x400),
        reg(n, 0x410), reg(n, 0x3828), reg(n, 0x3840));
    rpf("netdiag: %s: TDBAL %08x TDBAH %08x TDLEN %u TDH %u TDT %u GPTC %u TPT %u GPRC %u\n", tag, reg(n, 0x3800), reg(n, 0x3804), reg(n, 0x3808), reg(n, 0x3810),
        reg(n, 0x3818), reg(n, 0x4080), reg(n, 0x40D4), reg(n, 0x4074));
    rpf("netdiag: %s: TXDCTL1 %08x TARC1 %08x TDLEN1 %u PBA %08x RCTL %08x RXDCTL %08x RDBAL %08x RDLEN %u RDH %u RDT %u\n", tag, reg(n, 0x3928), reg(n, 0x3940),
        reg(n, 0x3908), reg(n, 0x1000), reg(n, 0x100), reg(n, 0x2828), reg(n, 0x2800), reg(n, 0x2808), reg(n, 0x2810), reg(n, 0x2818));
    rpf("netdiag: %s: FWSM %08x SWSM/H2ME %08x EXTCNF %08x MANC %08x FEXTNVM3 %08x FEXTNVM4 %08x FEXTNVM6 %08x FEXTNVM11 %08x PCIEANACFG %08x IOSFPC %08x\n", tag,
        reg(n, 0x5B54), reg(n, 0x5B50), reg(n, 0xF00), reg(n, 0x5820), reg(n, 0x3C), reg(n, 0x24), reg(n, 0x10), reg(n, 0x5BBC), reg(n, 0xF18), reg(n, 0xF28));
    UINT64 ring = ((UINT64)reg(n, 0x3804) << 32) | reg(n, 0x3800);
    UINT32 len = reg(n, 0x3808);
    if (ring && len >= 64) {
        const UINT32 *dsc = (const UINT32 *)(UINTN)ring;
        for (int i = 0; i < 4; i++)
            rpf("netdiag: %s: tx desc %u: %08x%08x len/cso/cmd/status %08x %08x\n", tag, i, dsc[4 * i + 1], dsc[4 * i], dsc[4 * i + 2], dsc[4 * i + 3]);
    }
}

/* The same table, in the same order, as g_regmap in kernel/drivers/e1000.c: the registers of the card while the firmware's driver is receiving frames, to be laid
 * side by side with what the kernel's driver leaves behind. Nothing in the table clears when read. */
static const struct { UINT16 off; UINT8 n; } g_regmap[] = {
    { 0x0000, 8 }, { 0x0024, 7 }, { 0x00C4, 1 }, { 0x00D0, 1 }, { 0x00E0, 1 }, { 0x00F8, 1 }, { 0x0100, 1 }, { 0x0400, 2 }, { 0x0410, 1 }, { 0x0F00, 4 }, { 0x0F18, 1 },
    { 0x0F28, 1 }, { 0x1000, 3 }, { 0x100C, 1 }, { 0x1010, 1 }, { 0x2160, 4 }, { 0x2170, 1 }, { 0x2410, 13 }, { 0x2800, 12 }, { 0x3410, 13 }, { 0x3800, 12 }, { 0x3840, 1 },
    { 0x3908, 1 }, { 0x3928, 1 }, { 0x3940, 1 }, { 0x5000, 3 }, { 0x5400, 8 }, { 0x5818, 1 }, { 0x5820, 1 }, { 0x5860, 1 }, { 0x5890, 8 }, { 0x5B00, 6 }, { 0x5B50, 3 },
    { 0x5BB8, 2 },
};

static void dump_regmap(const NicLoc *n, const char *tag) {
    for (UINTN i = 0; i < sizeof g_regmap / sizeof g_regmap[0]; i++) {
        rpf("netdiag: %s: regs %04x:", tag, (UINT32)g_regmap[i].off);
        for (UINT32 k = 0; k < g_regmap[i].n; k++) rpf(" %08x", reg(n, g_regmap[i].off + 4u * k));
        rpf("\n");
    }
}

/* Switches the receiver and the transmitter of the Intel Ethernet controllers of the e1000 / e1000e / I217 to I219 families off, once, right before the firmware lets go of
 * the machine. A firmware network driver that has left its card running keeps it fetching descriptors from a ring in boot-services memory and writing received frames into
 * buffers there, memory the kernel reclaims and reuses a moment later (the fourth hardware round found the card still writing into the firmware's old buffers minutes after
 * the kernel started). The kernel's own driver resets the card when it loads, but that is a second or more later. RCTL and TCTL are at the same places in all of these. */
void nic_quiesce(void) {
    static const UINT16 ids[] = {
        0x100E, 0x100F, 0x1004, 0x1008, 0x1019, 0x101E, 0x1026, 0x1076, 0x107C, 0x1010, 0x10D3, 0x10F6,                        /* 8254x, 82546, 82574 */
        0x153A, 0x153B, 0x155A, 0x1559, 0x15A0, 0x15A1, 0x15A2, 0x15A3,                                                      /* I217, I218 */
        0x156F, 0x1570, 0x15B7, 0x15B8, 0x15B9, 0x15BB, 0x15BC, 0x15BD, 0x15BE, 0x15D6, 0x15D7, 0x15D8, 0x15E3,              /* I219 */
    };
    for (int bus = 0; bus < 4; bus++)
        for (int dev = 0; dev < 32; dev++)
            for (int fn = 0; fn < 8; fn++) {
                UINT32 id = pci_cfg(bus, dev, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (fn == 0) break; continue; }
                if ((id & 0xFFFF) != 0x8086) continue;
                int known = 0;
                for (UINTN i = 0; i < sizeof ids / sizeof ids[0]; i++)
                    if ((id >> 16) == ids[i]) known = 1;
                if (!known || !(pci_cfg(bus, dev, fn, 4) & 2)) continue;             /* not one of ours, or its memory space is not decoding */
                UINT32 bar0 = pci_cfg(bus, dev, fn, 0x10);
                if (bar0 & 1) continue;
                UINT64 base = bar0 & ~0xFULL;
                if ((bar0 & 6) == 4) base |= (UINT64)pci_cfg(bus, dev, fn, 0x14) << 32;
                if (!base) continue;
                volatile UINT32 *r = (volatile UINT32 *)(UINTN)base;
                if (r[0x8 / 4] == 0xFFFFFFFFu) continue;                              /* the registers do not answer */
                UINT32 rctl = r[0x100 / 4], tctl = r[0x400 / 4];
                r[0x100 / 4] = rctl & ~2u;
                r[0x400 / 4] = tctl & ~2u;
                log_str("loader: Ethernet controller "); log_hex(id);
                log_str(" at "); log_dec((UINT64)bus); log_str(":"); log_dec((UINT64)dev); log_str("."); log_dec((UINT64)fn);
                log_str(": receiver and transmitter switched off (RCTL "); log_hex(rctl); log_str(", TCTL "); log_hex(tctl); log_str(" before)\n");
            }
}

static void dump_pci(const NicLoc *n) {
    for (int row = 0; row < 8; row++) {
        rpf("netdiag: PCI config %02x:", row * 32);
        for (int c = 0; c < 8; c++) rpf(" %08x", pci_cfg(n->bus, n->dev, n->fn, row * 32 + c * 4));
        rpf("\n");
    }
}

/* ---- frames ---- */

static UINT16 ip_checksum(const UINT8 *p, int len) {
    UINT32 sum = 0;
    for (int i = 0; i + 1 < len; i += 2) sum += (UINT32)((p[i] << 8) | p[i + 1]);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (UINT16)~sum;
}

#define XID 0x544F5530u            /* "TOU0" */
static UINT32 g_xid = XID;         /* the transaction id of the DISCOVER being waited for */

/* The UDP checksum of the datagram at udp (ulen bytes) with the pseudo header for 0.0.0.0 > 255.255.255.255. */
static UINT16 udp_checksum(const UINT8 *udp, UINTN ulen) {
    UINT32 sum = 0xFFFFu + 0xFFFFu + 17u + (UINT32)ulen;       /* source 0.0.0.0 adds nothing, the destination 255.255.255.255 adds two 0xFFFF */
    for (UINTN i = 0; i + 1 < ulen; i += 2) sum += (UINT32)((udp[i] << 8) | udp[i + 1]);
    if (ulen & 1) sum += (UINT32)(udp[ulen - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    UINT16 c = (UINT16)~sum;
    return c ? c : 0xFFFF;
}

/* An Ethernet II / IPv4 / UDP / DHCP DISCOVER frame; returns its length. Variant 0 is the plain one the first probe always sent (UDP checksum left out, four
 * parameters asked for); variant 1 has the UDP checksum filled in; variant 2 is what the kernel's own DHCP client sends: the checksum, a client identifier, the host
 * name "gensokyo", the six parameters and a nonzero IP identification. */
static UINTN build_discover(UINT8 *f, const UINT8 *mac, int variant, UINT32 xid) {
    memset(f, 0, 400);
    memset(f, 0xFF, 6);
    memcpy(f + 6, mac, 6);
    f[12] = 0x08; f[13] = 0x00;
    UINT8 *ip = f + 14;
    UINT8 *udp = ip + 20;
    UINT8 *b = udp + 8;                                       /* BOOTP */
    b[0] = 1; b[1] = 1; b[2] = 6;
    b[4] = (UINT8)(xid >> 24); b[5] = (UINT8)(xid >> 16); b[6] = (UINT8)(xid >> 8); b[7] = (UINT8)xid;
    b[10] = 0x80;                                             /* flags: broadcast reply */
    memcpy(b + 28, mac, 6);
    b[236] = 0x63; b[237] = 0x82; b[238] = 0x53; b[239] = 0x63;
    UINT8 *o = b + 240;
    *o++ = 53; *o++ = 1; *o++ = 1;                            /* DHCPDISCOVER */
    if (variant >= 2) {
        *o++ = 61; *o++ = 7; *o++ = 1; memcpy(o, mac, 6); o += 6;
        *o++ = 12; *o++ = 8; memcpy(o, "gensokyo", 8); o += 8;
        *o++ = 55; *o++ = 6; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 15; *o++ = 28; *o++ = 51;
    } else {
        *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 15;
    }
    *o++ = 255;
    UINTN blen = 300;                                         /* BOOTP minimum is 300 bytes */
    UINTN ulen = 8 + blen;
    UINTN iplen = 20 + ulen;
    ip[0] = 0x45; ip[2] = (UINT8)(iplen >> 8); ip[3] = (UINT8)iplen; ip[8] = 64; ip[9] = 17;
    if (variant >= 2) { ip[4] = 0x5A; ip[5] = 0x17; }
    ip[16] = ip[17] = ip[18] = ip[19] = 0xFF;
    UINT16 ck = ip_checksum(ip, 20);
    ip[10] = (UINT8)(ck >> 8); ip[11] = (UINT8)ck;
    udp[1] = 68; udp[3] = 67; udp[4] = (UINT8)(ulen >> 8); udp[5] = (UINT8)ulen;
    if (variant >= 1) { UINT16 uc = udp_checksum(udp, ulen); udp[6] = (UINT8)(uc >> 8); udp[7] = (UINT8)uc; }
    return 14 + iplen;
}

/* Looks at a received frame: is it the DHCP answer to our DISCOVER? */
static int dhcp_answer(const UINT8 *f, UINTN len, UINT32 *yiaddr, UINT32 *server, int *type) {
    if (len < 14 + 20 + 8 + 240 || f[12] != 0x08 || f[13] != 0x00 || f[23] != 17) return 0;
    UINTN ihl = (UINTN)(f[14] & 15) * 4;
    const UINT8 *udp = f + 14 + ihl;
    if (udp + 8 + 240 > f + len || udp[2] != 0 || udp[3] != 68) return 0;
    const UINT8 *b = udp + 8;
    if (b[0] != 2 || b[4] != (g_xid >> 24) || b[5] != ((g_xid >> 16) & 0xFF) || b[6] != ((g_xid >> 8) & 0xFF) || b[7] != (g_xid & 0xFF)) return 0;
    *yiaddr = ((UINT32)b[16] << 24) | ((UINT32)b[17] << 16) | ((UINT32)b[18] << 8) | b[19];
    *server = 0;
    *type = 0;
    for (const UINT8 *o = b + 240; o + 1 < f + len && *o != 255;) {
        if (*o == 0) { o++; continue; }
        if (o + 2 + o[1] > f + len) break;
        if (*o == 53 && o[1] >= 1) *type = o[2];
        if (*o == 54 && o[1] == 4) *server = ((UINT32)o[2] << 24) | ((UINT32)o[3] << 16) | ((UINT32)o[4] << 8) | o[5];
        o += 2 + o[1];
    }
    return 1;
}

static void put_ip(UINT32 a) { rpf("%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255); }

static void stall_ms(UINTN ms) { g_bs->Stall(ms * 1000); }

/* What the answer looked like on the wire: to whom it was sent (a broadcast, or the card's own address), and what it carries. */
static void dump_answer(const UINT8 *f, UINTN len) {
    UINTN ihl = (UINTN)(f[14] & 15) * 4;
    const UINT8 *udp = f + 14 + ihl, *b = udp + 8;
    rpf("netdiag: the answer: %u bytes, to %02x:%02x:%02x:%02x:%02x:%02x from %02x:%02x:%02x:%02x:%02x:%02x, IP %u.%u.%u.%u > %u.%u.%u.%u ttl %u, UDP checksum %02x%02x, flags %02x%02x\n",
        (UINT32)len, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[26], f[27], f[28], f[29], f[30], f[31], f[32], f[33], f[22], udp[6], udp[7],
        b[10], b[11]);
    rpf("netdiag: the answer carries:");
    for (const UINT8 *o = b + 240; o + 1 < f + len && *o != 255;) {
        if (*o == 0) { o++; continue; }
        if (o + 2 + o[1] > f + len) break;
        if ((*o == 1 || *o == 3 || *o == 6 || *o == 54) && o[1] >= 4) {
            rpf(" %s ", *o == 1 ? "mask" : *o == 3 ? "router" : *o == 6 ? "dns" : "server");
            put_ip(((UINT32)o[2] << 24) | ((UINT32)o[3] << 16) | ((UINT32)o[4] << 8) | o[5]);
        } else if (*o == 51 && o[1] == 4) {
            rpf(" lease %u s", ((UINT32)o[2] << 24) | ((UINT32)o[3] << 16) | ((UINT32)o[4] << 8) | o[5]);
        }
        o += 2 + o[1];
    }
    rpf("\n");
}

/* The statistics registers that say what really went over the wire (they clear when read). */
static void dump_counters(const NicLoc *n, const char *tag) {
    rpf("netdiag: %s: frames sent GPTC %u TPT %u, received GPRC %u BPRC %u MPRC %u; CRC %u align %u symbol %u rxerr %u missed %u no-buffer %u; pause frames received %u sent %u\n", tag,
        reg(n, 0x4080), reg(n, 0x40D4), reg(n, 0x4074), reg(n, 0x4078), reg(n, 0x407C), reg(n, 0x4000), reg(n, 0x4004), reg(n, 0x4008), reg(n, 0x400C), reg(n, 0x4010),
        reg(n, 0x40A0), reg(n, 0x4050), reg(n, 0x4054));
}

/* ---- the probe ---- */

void netdiag_run(UINT64 *report_phys, UINT64 *report_len) {
    EFI_PHYSICAL_ADDRESS mem = 0;
    if (EFI_ERROR(g_bs->AllocatePages(AllocateAnyPages, EfiLoaderData, REPORT_BYTES / 4096, &mem))) return;
    g_rep = (char *)(UINTN)mem;
    g_rep[0] = 0;
    *report_phys = mem;

    rpf("netdiag: the firmware's own network driver, for a moment (\"netdiag\" on the command line)\n");
    NicLoc nic = {0};
    int have_nic = find_nic(&nic);
    if (have_nic) dump_pci(&nic);

    EFI_SIMPLE_NETWORK_PROTOCOL *snp = 0;
    EFI_STATUS s = g_bs->LocateProtocol(&snpGuid, 0, (void **)&snp);
    if (EFI_ERROR(s) || !snp || !snp->Mode) {
        rpf("netdiag: the firmware has no EFI_SIMPLE_NETWORK_PROTOCOL (status %lx): its network stack is off (Dell BIOS: Enable UEFI Network Stack)\n", (UINT64)s);
        if (have_nic) dump_nic(&nic, "as the firmware left it");
        *report_len = g_replen;
        return;
    }
    EFI_SIMPLE_NETWORK_MODE *m = snp->Mode;
    rpf("netdiag: SNP revision %lx, state %u, hw address size %u, header size %u, max packet %u, filters %x/%x, media present %u (supported %u)\n", snp->Revision,
        m->State, m->HwAddressSize, m->MediaHeaderSize, m->MaxPacketSize, m->ReceiveFilterMask, m->ReceiveFilterSetting, m->MediaPresent, m->MediaPresentSupported);
    rpf("netdiag: MAC %02x:%02x:%02x:%02x:%02x:%02x\n", m->CurrentAddress.Addr[0], m->CurrentAddress.Addr[1], m->CurrentAddress.Addr[2], m->CurrentAddress.Addr[3],
        m->CurrentAddress.Addr[4], m->CurrentAddress.Addr[5]);
    if (have_nic) dump_nic(&nic, "before the firmware's driver starts");
    if (have_nic) dump_regmap(&nic, "registers before the firmware's driver starts");

    UINT32 state0 = m->State;
    if (m->State == 0) { s = snp->Start(snp); rpf("netdiag: Start -> %lx\n", (UINT64)s); }
    if (m->State == 1) { s = snp->Initialize(snp, 0, 0); rpf("netdiag: Initialize -> %lx\n", (UINT64)s); }
    if (m->State != 2) {
        rpf("netdiag: the adapter did not reach the initialized state (%u); giving up\n", m->State);
        goto done;
    }
    s = snp->ReceiveFilters(snp, m->ReceiveFilterMask & (SNP_FILTER_UNICAST | SNP_FILTER_BROADCAST), 0, 0, 0, 0);
    rpf("netdiag: ReceiveFilters(unicast+broadcast) -> %lx, setting now %x\n", (UINT64)s, m->ReceiveFilterSetting);

    /* the link: autonegotiation takes a few seconds */
    for (int i = 0; i < 80 && m->MediaPresentSupported && !m->MediaPresent; i++) {
        UINT32 is; void *tb;
        snp->GetStatus(snp, &is, &tb);
        stall_ms(100);
    }
    rpf("netdiag: media present %u\n", m->MediaPresent);
    if (have_nic) dump_nic(&nic, "after Initialize");
    if (m->MediaPresentSupported && !m->MediaPresent) {
        rpf("netdiag: the firmware sees no link (cable? switch?): nothing is sent\n");
        goto done;
    }

    static UINT8 frame[512] __attribute__((aligned(16)));
    static UINT8 rx[2048] __attribute__((aligned(16)));
    g_xid = XID;
    UINTN flen = build_discover(frame, m->CurrentAddress.Addr, 0, XID);
    int sent_ok = 0, answered = 0;
    for (int attempt = 0; attempt < 5 && !answered; attempt++) {         /* a card that has only just come up may lose the first answers */
        s = snp->Transmit(snp, 0, flen, frame, 0, 0, 0);
        rpf("netdiag: Transmit(DHCP DISCOVER, %u bytes) -> %lx\n", (UINT32)flen, (UINT64)s);
        if (EFI_ERROR(s)) continue;
        void *done = 0;
        int i;
        for (i = 0; i < 200 && !done; i++) {                       /* up to 2 s for the descriptor to come back */
            UINT32 is = 0;
            snp->GetStatus(snp, &is, &done);
            if (!done) stall_ms(10);
        }
        rpf("netdiag: the transmit buffer %s after %u ms\n", done ? "came back" : "did NOT come back", (UINT32)(i * 10));
        if (done) sent_ok++;
        if (have_nic && attempt == 0) dump_nic(&nic, done ? "after the firmware transmitted" : "the firmware's transmit did not complete");
        UINT32 nframes = 0, ntypes[4] = { 0, 0, 0, 0 };
        UINT16 types[4] = { 0, 0, 0, 0 };
        for (i = 0; i < 300 && !answered; i++) {                   /* up to 3 s for an answer */
            UINTN hdr = 0, sz = sizeof rx;
            EFI_MAC_ADDRESS src, dst;
            UINT16 proto = 0;
            s = snp->Receive(snp, &hdr, &sz, rx, &src, &dst, &proto);
            if (s == EFI_NOT_READY) { stall_ms(10); continue; }
            if (EFI_ERROR(s)) { rpf("netdiag: Receive -> %lx\n", (UINT64)s); break; }
            nframes++;
            if (sz >= 14) {                                         /* a tally of what the firmware's driver hands over, by ethertype */
                UINT16 et = (UINT16)((rx[12] << 8) | rx[13]);
                int k;
                for (k = 0; k < 4 && types[k] && types[k] != et; k++) { }
                if (k < 4) { types[k] = et; ntypes[k]++; }
            }
            UINT32 yi, sv;
            int ty;
            if (dhcp_answer(rx, sz, &yi, &sv, &ty)) {
                rpf("netdiag: DHCP %s from ", ty == 2 ? "OFFER" : ty == 5 ? "ACK" : "answer");
                put_ip(sv);
                rpf(" offering ");
                put_ip(yi);
                rpf("\n");
                dump_answer(rx, sz);
                answered = 1;
            }
        }
        rpf("netdiag: received %u frames in that wait", nframes);
        for (int k = 0; k < 4 && types[k]; k++) rpf(", ethertype %04x x%u", types[k], ntypes[k]);
        rpf("\n");
    }
    if (answered && have_nic) dump_counters(&nic, "after the first exchange");
    if (answered && have_nic) dump_regmap(&nic, "registers while the firmware's driver receives");
    /* the same question asked the way the kernel's own DHCP client asks it: if the router answers the plain one and not this, the frame is what it dislikes, not the driver */
    for (int v = 1; v <= 2 && answered; v++) {
        static const char *const what[3] = { "", "the UDP checksum filled in", "as the kernel's own client sends it: checksum, client id, host name, six parameters, an IP id" };
        g_xid = XID + (UINT32)v;
        flen = build_discover(frame, m->CurrentAddress.Addr, v, g_xid);
        int got = 0;
        UINT32 seen = 0, bootp = 0;
        for (int rep = 0; rep < 3 && !got; rep++) {
            s = snp->Transmit(snp, 0, flen, frame, 0, 0, 0);
            if (EFI_ERROR(s)) { rpf("netdiag: variant %u: Transmit -> %lx\n", (UINT32)v, (UINT64)s); continue; }
            for (int i = 0; i < 150 && !got; i++) {                  /* up to 1.5 s for the answer */
                UINTN hdr = 0, sz = sizeof rx;
                EFI_MAC_ADDRESS src, dst;
                UINT16 proto = 0;
                s = snp->Receive(snp, &hdr, &sz, rx, &src, &dst, &proto);
                if (s == EFI_NOT_READY) { stall_ms(10); continue; }
                if (EFI_ERROR(s)) break;
                seen++;
                if (sz > 42 && rx[12] == 0x08 && rx[13] == 0x00 && rx[23] == 17 && rx[36] == 0 && rx[37] == 68) bootp++;
                UINT32 yi, sv;
                int ty;
                if (dhcp_answer(rx, sz, &yi, &sv, &ty)) { got = 1; dump_answer(rx, sz); }
            }
        }
        rpf("netdiag: DISCOVER variant %u (%s): %s; %u frames came in meanwhile, %u of them UDP to port 68\n", (UINT32)v, what[v], got ? "ANSWERED" : "NO ANSWER in three tries", seen, bootp);
    }
    if (answered && have_nic) dump_counters(&nic, "after the variants");
    rpf("netdiag: result: the firmware's driver %s, %s\n", sent_ok ? "transmitted" : "did NOT transmit", answered ? "and the network answered" : "and nothing answered");
    if (have_nic) dump_nic(&nic, "at the end");

done:
    snp->Shutdown(snp);                                           /* the card must not keep DMA-ing into memory the kernel will reuse */
    if (state0 == 0) snp->Stop(snp);
    rpf("netdiag: adapter shut down (state %u)\n", m->State);
    if (have_nic) dump_nic(&nic, "after Shutdown");
    *report_len = g_replen;
}
