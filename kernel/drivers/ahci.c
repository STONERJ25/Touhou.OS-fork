/*
 * AHCI (SATA) host adapter driver: finds every AHCI controller on the PCI bus, brings up each implemented port that has an
 * ATA disk on it and registers the disk as a BlockDev (sda, sdb, ...).
 *
 * Design in one paragraph: polled and single-command. Every port gets one command slot (slot 0), one command table with a
 * PRDT, and is protected by a mutex, so any number of threads can issue I/O. A request is split into commands of at most
 * 16 MiB / 65535 sectors; each command is a READ/WRITE DMA EXT with a PRDT describing the caller's buffer in physically
 * contiguous runs (bounced through a driver buffer when the buffer is odd-aligned or, on a 32-bit controller, above
 * 4 GiB). Completion is polled: a short spin, then a 1 ms sleep per poll, so an idle disk costs nothing and a fast SSD
 * still copies quickly. No interrupts, no NCQ, no hot-plug, no power management (see docs/BLOCKDEVICES.md).
 *
 * The HBA is left as the firmware configured it unless something is wrong: it is only reset (GHC.HR) when a port's
 * command engine refuses to stop, and ports are only COMRESET when their link is down or a command fails.
 */
#include "kernel.h"
#include "blk.h"
#include "pci.h"
#include "mm.h"
#include "task.h"
#include "irq.h"
#include "cpu.h"
#include "cmdline.h"
#include "vfs.h"
#include "blkdrv_util.h"

void blk_selftest(const char *driver);          /* blk_selftest.c: optional in-kernel I/O check driven by the command line */

/* ---------------------------------------------------------------- registers */

#define HBA_CAP    0x00
#define HBA_GHC    0x04
#define HBA_IS     0x08
#define HBA_PI     0x0C
#define HBA_VS     0x10
#define HBA_CAP2   0x24
#define HBA_BOHC   0x28

#define CAP_S64A   0x80000000u
#define CAP_SNCQ   0x40000000u
#define CAP_SSS    0x08000000u
#define CAP_SCLO   0x01000000u
#define CAP2_BOH   0x00000001u

#define GHC_HR     0x00000001u
#define GHC_IE     0x00000002u
#define GHC_AE     0x80000000u

#define BOHC_BOS   0x01u            /* BIOS owned semaphore */
#define BOHC_OOS   0x02u            /* OS owned semaphore */
#define BOHC_BB    0x10u            /* BIOS busy */

#define PORT_BASE(n) (0x100u + 0x80u * (n))
#define PX_CLB     0x00
#define PX_CLBU    0x04
#define PX_FB      0x08
#define PX_FBU     0x0C
#define PX_IS      0x10
#define PX_IE      0x14
#define PX_CMD     0x18
#define PX_TFD     0x20
#define PX_SIG     0x24
#define PX_SSTS    0x28
#define PX_SCTL    0x2C
#define PX_SERR    0x30
#define PX_CI      0x38

#define CMD_ST     0x00000001u
#define CMD_SUD    0x00000002u
#define CMD_POD    0x00000004u
#define CMD_CLO    0x00000008u
#define CMD_FRE    0x00000010u
#define CMD_FR     0x00004000u
#define CMD_CR     0x00008000u
#define CMD_CPD    0x00100000u
#define CMD_ICC_MASK 0xF0000000u

#define IS_TFES    0x40000000u      /* task file error */
#define IS_HBFS    0x20000000u      /* host bus fatal */
#define IS_HBDS    0x10000000u      /* host bus data */
#define IS_IFS     0x08000000u      /* interface fatal */
#define IS_OFS     0x01000000u      /* overflow */
#define IS_ERRORS  (IS_TFES | IS_HBFS | IS_HBDS | IS_IFS | IS_OFS)

#define TFD_ERR    0x01u
#define TFD_DRQ    0x08u
#define TFD_DF     0x20u
#define TFD_BSY    0x80u

#define SIG_ATA    0x00000101u
#define SIG_ATAPI  0xEB140101u
#define SIG_SEMB   0xC33C0101u
#define SIG_PM     0x96690101u

#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35
#define ATA_FLUSH_EXT      0xEA
#define ATA_FLUSH          0xE7
#define ATA_IDENTIFY       0xEC

/* ---------------------------------------------------------------- limits and timeouts */

#define AHCI_MAX_PRD       256                  /* PRDT entries in the command table */
#define AHCI_PRD_MAX_BYTES (4u << 20)           /* a PRDT entry describes at most 4 MiB */
#define AHCI_CMD_MAX_BYTES (16u << 20)          /* bytes per command (also at most 65535 sectors) */
#define AHCI_BOUNCE_BYTES  (128u << 10)

#define T_ENGINE_MS        500                  /* spec: CR/FR must clear within 500 ms */
#define T_LINK_MS          2000                 /* link up after COMRESET */
#define T_READY_MS         6000                 /* BSY clear after reset */
#define T_IDENTIFY_MS      5000
#define T_IO_MS            15000
#define T_FLUSH_MS         30000

/* ---------------------------------------------------------------- state */

typedef struct AhciHba AhciHba;

typedef struct AhciPort {
    AhciHba *hba;
    int      num;
    volatile u8 *regs;

    /* One DMA allocation: command list (1 KiB) | received FIS (256 B) | identify data (512 B) | command table (+0x1000). */
    void    *mem;
    paddr_t  mem_phys;
    size_t   mem_size;
    u8      *clb, *fis, *ident, *ctbl;
    paddr_t  clb_phys, fis_phys, ident_phys, ctbl_phys;

    u8      *bounce;                            /* allocated on first use */
    paddr_t  bounce_phys;

    Mutex    lock;                              /* one command at a time per port */
    bool     dead;                              /* gave up after an unrecoverable failure: every request fails fast */

    char     model[48], serial[24], firmware[12];
    u32      sector_size;
    u64      sectors;
    u8       flush_cmd;                         /* 0 (none), ATA_FLUSH or ATA_FLUSH_EXT */
    bool     ssd, locked;
    u32      phys_shift;                        /* log2(logical sectors per physical sector) */
    BlockDev *bd;

    u32      n_cmds, n_errors, n_resets;        /* for /proc/ahci */
} AhciPort;

struct AhciHba {
    PciDev  *pci;
    volatile u8 *mmio;
    int      index;
    u32      cap, cap2, vs, pi;
    bool     s64a;
    AhciPort *port[32];
    AhciHba *next;
};

static AhciHba *g_hba_head, *g_hba_tail;
static int g_nhba;
static int g_sd_next;

/* Test knobs (kernel command line); the defaults are the production behaviour. They exist so that paths QEMU would never
 * take on its own (many PRDs per command, command splitting, bounce buffers, HBA reset) can be exercised in the tests. */
static u32  g_maxsect = 65535;                  /* ahci.maxsect=N   sectors per command */
static u32  g_prd_len = AHCI_PRD_MAX_BYTES;     /* ahci.prdlen=N    bytes per PRDT entry */
static u32  g_prd_max = AHCI_MAX_PRD;           /* ahci.prdmax=N    PRDT entries per command */
static bool g_force_bounce;                     /* ahci.bounce      copy every transfer through the bounce buffer */
static bool g_force_hba_reset;                  /* ahci.hbareset    reset the HBA even though nothing is wrong */

#define PR(p, off)      mmio_r32((p)->regs, (off))
#define PW(p, off, v)   mmio_w32((p)->regs, (off), (v))
#define HR(h, off)      mmio_r32((h)->mmio, (off))
#define HW(h, off, v)   mmio_w32((h)->mmio, (off), (v))

/* ---------------------------------------------------------------- identify data */

typedef struct {
    char model[48], serial[24], firmware[12];
    u64  sectors;
    u32  sector_size;                           /* logical */
    u32  phys_shift;
    u8   flush_cmd;
    bool lba48, ssd, locked, atapi;
} AtaInfo;

/* Pure function of the 256 identify words, so it can be unit tested without hardware. */
static void ata_parse_identify(const u16 *id, AtaInfo *out) {
    memset(out, 0, sizeof *out);
    ata_string(out->serial, sizeof out->serial, id + 10, 10);
    ata_string(out->firmware, sizeof out->firmware, id + 23, 4);
    ata_string(out->model, sizeof out->model, id + 27, 20);
    out->atapi = (id[0] & 0x8000) != 0;

    bool cmdset_valid = (id[83] & 0xC000) == 0x4000;
    out->lba48 = cmdset_valid && (id[83] & BIT(10));
    if (out->lba48) out->sectors = ((u64)id[103] << 48 | (u64)id[102] << 32 | (u64)id[101] << 16 | id[100]) & 0x0000FFFFFFFFFFFFull;
    else out->sectors = (u32)id[60] | (u32)id[61] << 16;

    out->sector_size = 512;
    if ((id[106] & 0xC000) == 0x4000) {
        if (id[106] & BIT(12)) {                                    /* logical sector longer than 256 words: words 117-118 */
            u32 words = (u32)id[117] | (u32)id[118] << 16;
            if (words) out->sector_size = words * 2;
        }
        if (id[106] & BIT(13)) out->phys_shift = id[106] & 0xF;     /* several logical sectors per physical sector (512e) */
    }

    if (cmdset_valid) out->flush_cmd = (id[83] & BIT(13)) ? ATA_FLUSH_EXT : (id[83] & BIT(12)) ? ATA_FLUSH : 0;
    out->ssd = id[217] == 1;
    out->locked = (id[128] & BIT(1)) && (id[128] & BIT(2));
}

/* ---------------------------------------------------------------- port engine control */

static int port_stop_engine(AhciPort *p) {
    u32 cmd = PR(p, PX_CMD);
    if (cmd == 0xFFFFFFFFu) return -ENODEV;
    if (cmd & (CMD_ST | CMD_CR)) {
        PW(p, PX_CMD, cmd & ~CMD_ST);
        if (!BLK_POLL(!(PR(p, PX_CMD) & CMD_CR), T_ENGINE_MS)) return -EIO;
    }
    return 0;
}

static int port_stop_fis(AhciPort *p) {
    u32 cmd = PR(p, PX_CMD);
    if (cmd & (CMD_FRE | CMD_FR)) {
        PW(p, PX_CMD, cmd & ~CMD_FRE);
        if (!BLK_POLL(!(PR(p, PX_CMD) & CMD_FR), T_ENGINE_MS)) return -EIO;
    }
    return 0;
}

static void port_start_engine(AhciPort *p) {
    if ((PR(p, PX_TFD) & (TFD_BSY | TFD_DRQ)) && (p->hba->cap & CAP_SCLO)) {
        /* A stuck BSY/DRQ would keep the HBA from starting: command list override clears them. */
        PW(p, PX_CMD, PR(p, PX_CMD) | CMD_CLO);
        BLK_POLL(!(PR(p, PX_CMD) & CMD_CLO), T_ENGINE_MS);
    }
    PW(p, PX_CMD, PR(p, PX_CMD) | CMD_ST);
    (void)PR(p, PX_CMD);
}

static void port_program_lists(AhciPort *p) {
    PW(p, PX_CLB, lo32(p->clb_phys));
    PW(p, PX_CLBU, hi32(p->clb_phys));
    PW(p, PX_FB, lo32(p->fis_phys));
    PW(p, PX_FBU, hi32(p->fis_phys));
    PW(p, PX_CMD, PR(p, PX_CMD) | CMD_FRE);
    (void)PR(p, PX_CMD);
}

static bool port_ready(AhciPort *p) {
    u32 tfd = PR(p, PX_TFD);
    return !(tfd & (TFD_BSY | TFD_DRQ));
}

/* COMRESET: hold DET=1 for at least a millisecond, release, then wait for the PHY to report an established link. */
static int port_comreset(AhciPort *p) {
    u32 sctl = PR(p, PX_SCTL) & 0x0F0u;                         /* keep the speed limit, force IPM=3 (no partial/slumber) */
    PW(p, PX_SCTL, sctl | 0x301u);
    (void)PR(p, PX_SCTL);
    task_sleep_ms(2);
    PW(p, PX_SCTL, sctl | 0x300u);
    if (!BLK_POLL((PR(p, PX_SSTS) & 0xF) == 3, T_LINK_MS)) return -ENODEV;
    task_sleep_ms(20);                                          /* debounce: the link must stay up */
    if ((PR(p, PX_SSTS) & 0xF) != 3) return -ENODEV;
    PW(p, PX_SERR, 0xFFFFFFFFu);
    return 0;
}

/* Wakes a link that firmware parked in partial/slumber. */
static void port_wake_link(AhciPort *p) {
    u32 cmd = PR(p, PX_CMD);
    if (cmd & CMD_ICC_MASK) return;                             /* a previous change is still in progress */
    PW(p, PX_CMD, (cmd & ~CMD_ICC_MASK) | (1u << 28));         /* ICC=1: active */
    BLK_POLL(!(PR(p, PX_CMD) & CMD_ICC_MASK), 100);
    BLK_POLL(((PR(p, PX_SSTS) >> 8) & 0xF) == 1, 100);
}

/* ---------------------------------------------------------------- commands */

typedef struct {
    u8   cmd, device;
    u16  features;
    u64  lba;
    u16  count;
    bool write;
} AtaCmd;

static void build_cfis(u8 *f, const AtaCmd *c) {
    memset(f, 0, 64);
    f[0] = 0x27;                                    /* register host-to-device FIS */
    f[1] = 0x80;                                    /* C bit: this carries a command */
    f[2] = c->cmd;
    f[3] = (u8)c->features;
    f[4] = (u8)c->lba;
    f[5] = (u8)(c->lba >> 8);
    f[6] = (u8)(c->lba >> 16);
    f[7] = c->device;
    f[8] = (u8)(c->lba >> 24);
    f[9] = (u8)(c->lba >> 32);
    f[10] = (u8)(c->lba >> 40);
    f[11] = (u8)(c->features >> 8);
    f[12] = (u8)c->count;
    f[13] = (u8)(c->count >> 8);
}

static void port_recover(AhciPort *p, const char *why);

/*
 * Runs one command on slot 0 and waits for it. The caller holds p->lock and has filled the PRDT (nprd entries).
 * Any failure resets the link (port_recover) and returns -EIO; the caller never sees a half-finished command.
 */
static int ahci_issue(AhciPort *p, const AtaCmd *c, u32 nprd, u32 timeout_ms) {
    if (p->dead) return -EIO;

    if (!port_ready(p) && !BLK_POLL(port_ready(p), 1000)) {
        klog("Kappa", "ahci%d port %d: device busy before command %02x (TFD %#x)", p->hba->index, p->num, c->cmd, PR(p, PX_TFD));
        p->n_errors++;
        port_recover(p, "busy");
        return -EIO;
    }

    build_cfis(p->ctbl, c);
    volatile u32 *hdr = (volatile u32 *)p->clb;
    hdr[0] = 5u | (c->write ? 0x40u : 0u) | (nprd << 16);          /* CFL = 5 dwords, W, PRDTL */
    hdr[1] = 0;                                                    /* PRDBC */
    hdr[2] = lo32(p->ctbl_phys);
    hdr[3] = hi32(p->ctbl_phys);

    PW(p, PX_IS, PR(p, PX_IS));                                    /* drop stale status so only this command's errors count */
    barrier();
    PW(p, PX_CI, 1);
    p->n_cmds++;

    u32 is = 0;
    bool done = BLK_POLL(({ is = PR(p, PX_IS); (is & IS_ERRORS) || !(PR(p, PX_CI) & 1); }), timeout_ms);
    u32 tfd = PR(p, PX_TFD);

    const char *why = NULL;
    if (!done) why = "timeout";
    else if (is & IS_ERRORS) why = (is & IS_TFES) ? "device reported an error" : "interface or host bus error";
    else if (tfd & (TFD_ERR | TFD_DF)) why = "device reported an error";
    if (!why) return 0;

    klog("Kappa", "ahci%d port %d: command %02x (lba %lu, %u sectors) failed: %s -- IS %#x TFD %#x SERR %#x", p->hba->index, p->num, c->cmd,
         (unsigned long)c->lba, (unsigned)c->count, why, is, tfd, PR(p, PX_SERR));
    p->n_errors++;
    port_recover(p, why);
    return -EIO;
}

/* Stops the engine (which also abandons the failed command), COMRESETs the link and restarts; gives up on the port otherwise. */
static void port_recover(AhciPort *p, const char *why) {
    (void)why;
    p->n_resets++;
    if (port_stop_engine(p)) {
        klog("Kappa", "ahci%d port %d: command engine will not stop; giving up on this disk", p->hba->index, p->num);
        p->dead = true;
        return;
    }
    PW(p, PX_SERR, 0xFFFFFFFFu);
    PW(p, PX_IS, 0xFFFFFFFFu);
    if (port_comreset(p) != 0 || !BLK_POLL(port_ready(p), T_READY_MS)) {
        klog("Kappa", "ahci%d port %d: the disk did not come back after a link reset; giving up on it", p->hba->index, p->num);
        p->dead = true;
        return;
    }
    PW(p, PX_SERR, 0xFFFFFFFFu);
    PW(p, PX_IS, 0xFFFFFFFFu);
    port_start_engine(p);
    klog("Kappa", "ahci%d port %d: link reset done, disk is back", p->hba->index, p->num);
}

/*
 * Describes up to n sectors of buf in the command table's PRDT, one entry per physically contiguous run (at most g_prd_len
 * bytes each). Returns the number of whole sectors described (0: the buffer cannot be used directly, bounce it).
 */
static u32 prdt_build(AhciPort *p, const u8 *buf, u32 n, u32 *nprd) {
    u32 ss = p->sector_size;
    size_t want = (size_t)n * ss, done = 0;
    volatile u32 *prd = (volatile u32 *)(p->ctbl + 0x80);
    u32 e = 0;
    while (done < want && e < g_prd_max) {
        paddr_t ph;
        size_t run = phys_run(buf + done, want - done, g_prd_len, &ph);
        if (!p->hba->s64a && ph + run > 0x100000000ull) return 0;       /* 32-bit controller: needs a low bounce buffer */
        prd[4 * e + 0] = lo32(ph);
        prd[4 * e + 1] = hi32(ph);
        prd[4 * e + 2] = 0;
        prd[4 * e + 3] = (u32)(run - 1);                                /* byte count minus one, bit 0 set: even length */
        e++;
        done += run;
    }
    size_t excess = done % ss;                                          /* table full in the middle of a sector: drop the tail */
    while (excess && e) {
        size_t len = (size_t)prd[4 * (e - 1) + 3] + 1;
        if (len > excess) { prd[4 * (e - 1) + 3] = (u32)(len - excess - 1); excess = 0; }
        else { excess -= len; e--; }
    }
    *nprd = e;
    return e ? (u32)((done - done % ss) / ss) : 0;
}

static bool ensure_bounce(AhciPort *p) {
    if (p->bounce) return true;
    p->bounce = dma_alloc(AHCI_BOUNCE_BYTES, &p->bounce_phys);
    if (p->bounce && !p->hba->s64a && p->bounce_phys + AHCI_BOUNCE_BYTES > 0x100000000ull) {
        dma_free(p->bounce, AHCI_BOUNCE_BYTES);
        p->bounce = NULL;
    }
    return p->bounce != NULL;
}

static int ahci_rw(AhciPort *p, u64 lba, u32 count, void *buf, bool write) {
    if (!count) return 0;
    u32 ss = p->sector_size;
    if (lba >= p->sectors || (u64)count > p->sectors - lba) return -EINVAL;
    u32 max_sectors = MIN(MIN(g_maxsect, 65535u), AHCI_CMD_MAX_BYTES / ss);
    if (!max_sectors) max_sectors = 1;

    mutex_lock(&p->lock);
    int rc = 0;
    u8 *cur = buf;
    while (count && !rc) {
        if (p->dead) { rc = -EIO; break; }
        u32 n = MIN(count, max_sectors), nprd = 0;
        bool bounce = g_force_bounce || ((u64)cur & 1);
        if (!bounce) {
            u32 got = prdt_build(p, cur, n, &nprd);
            if (got) n = got; else bounce = true;
        }
        if (bounce) {
            if (!ensure_bounce(p)) { rc = -ENOMEM; break; }
            u32 got = prdt_build(p, p->bounce, MIN(n, (u32)(AHCI_BOUNCE_BYTES / ss)), &nprd);
            if (!got) { rc = -EIO; break; }
            n = got;
            if (write) memcpy(p->bounce, cur, (size_t)n * ss);
        }
        AtaCmd c = { .cmd = write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT, .device = 0x40, .lba = lba, .count = (u16)n, .write = write };
        rc = ahci_issue(p, &c, nprd, T_IO_MS);
        if (rc) break;
        if (bounce && !write) memcpy(cur, p->bounce, (size_t)n * ss);
        cur += (size_t)n * ss;
        lba += n;
        count -= n;
    }
    mutex_unlock(&p->lock);
    return rc;
}

static int ahci_blk_read(BlockDev *d, u64 lba, u32 count, void *buf) { return ahci_rw(d->priv, lba, count, buf, false); }
static int ahci_blk_write(BlockDev *d, u64 lba, u32 count, const void *buf) { return ahci_rw(d->priv, lba, count, (void *)buf, true); }

static int ahci_blk_flush(BlockDev *d) {
    AhciPort *p = d->priv;
    if (!p->flush_cmd) return 0;
    mutex_lock(&p->lock);
    AtaCmd c = { .cmd = p->flush_cmd, .device = 0x40 };
    int rc = ahci_issue(p, &c, 0, T_FLUSH_MS);
    mutex_unlock(&p->lock);
    return rc;
}

static int port_identify(AhciPort *p, AtaInfo *info) {
    memset(p->ident, 0, 512);
    volatile u32 *prd = (volatile u32 *)(p->ctbl + 0x80);
    prd[0] = lo32(p->ident_phys);
    prd[1] = hi32(p->ident_phys);
    prd[2] = 0;
    prd[3] = 511;
    AtaCmd c = { .cmd = ATA_IDENTIFY, .device = 0 };
    int rc = ahci_issue(p, &c, 1, T_IDENTIFY_MS);
    if (rc) return rc;
    u16 id[256];
    memcpy(id, p->ident, sizeof id);
    ata_parse_identify(id, info);
    return 0;
}

/* ---------------------------------------------------------------- port bring-up */

typedef enum { PORT_EMPTY, PORT_STUCK, PORT_PRESENT } PortProbe;

/*
 * Phase 1, registers only so that empty ports cost nothing: stops whatever the firmware left running, powers the port
 * up if it was off, and reports whether a device is attached.
 */
static PortProbe port_detect(AhciPort *p) {
    if (port_stop_engine(p) || port_stop_fis(p)) return PORT_STUCK;

    u32 sctl = PR(p, PX_SCTL);
    if ((sctl & 0xF0F) != 0x300) PW(p, PX_SCTL, (sctl & 0x0F0) | 0x300);       /* DET=0 (no reset requested), IPM=3 */
    PW(p, PX_SERR, 0xFFFFFFFFu);
    PW(p, PX_IS, 0xFFFFFFFFu);
    PW(p, PX_IE, 0);

    u32 cmd = PR(p, PX_CMD);
    bool powered_before = (cmd & CMD_SUD) != 0;
    if (!powered_before) {
        if (cmd & CMD_CPD) cmd |= CMD_POD;
        PW(p, PX_CMD, cmd | CMD_SUD);                                          /* spin the device up / power the PHY */
    }
    u32 det = PR(p, PX_SSTS) & 0xF;
    if (det == 0 && !powered_before) {
        BLK_POLL((PR(p, PX_SSTS) & 0xF) != 0, 1000);                           /* give a device we just powered time to answer */
        det = PR(p, PX_SSTS) & 0xF;
    }
    return det ? PORT_PRESENT : PORT_EMPTY;
}

/*
 * Phase 2, with the command list and FIS area programmed and FIS reception on (after a reset the device's signature FIS
 * has to land somewhere before the HBA reports the device ready): makes sure the link is established and active, then
 * waits for the device to leave BSY. A link that is down or confused gets one COMRESET.
 */
static bool port_link_ready(AhciPort *p) {
    for (int attempt = 0;; attempt++) {
        u32 ssts = PR(p, PX_SSTS);
        if ((ssts & 0xF) == 3 && ((ssts >> 8) & 0xF) != 1) {                    /* parked in partial/slumber */
            port_wake_link(p);
            ssts = PR(p, PX_SSTS);
        }
        if ((ssts & 0xF) == 3 && ((ssts >> 8) & 0xF) == 1) break;
        if (attempt || port_comreset(p)) return false;
    }
    if (BLK_POLL(port_ready(p), T_READY_MS)) return true;
    klog("Kappa", "ahci%d port %d: device is not ready (TFD %#x); resetting the link", p->hba->index, p->num, PR(p, PX_TFD));
    return port_comreset(p) == 0 && BLK_POLL(port_ready(p), T_READY_MS);
}

static void port_free(AhciPort *p) {
    if (p->bounce) dma_free(p->bounce, AHCI_BOUNCE_BYTES);
    if (p->mem) dma_free(p->mem, p->mem_size);
    kfree(p);
}

/* Idles the port (the HBA must not touch the memory any more) and releases everything. */
static void port_teardown(AhciPort *p) {
    port_stop_engine(p);
    port_stop_fis(p);
    PW(p, PX_CLB, 0); PW(p, PX_CLBU, 0); PW(p, PX_FB, 0); PW(p, PX_FBU, 0);
    port_free(p);
}

static bool port_alloc(AhciPort *p) {
    p->mem_size = 0x1000 + 0x80 + (size_t)AHCI_MAX_PRD * 16;
    p->mem = dma_alloc(p->mem_size, &p->mem_phys);
    if (!p->mem) return false;
    if (!p->hba->s64a && p->mem_phys + p->mem_size > 0x100000000ull) {
        klog("Kappa", "ahci%d port %d: the controller is 32-bit only and its buffers landed above 4 GiB", p->hba->index, p->num);
        return false;
    }
    u8 *m = p->mem;
    p->clb = m;                      p->clb_phys = p->mem_phys;                 /* 1 KiB aligned (page aligned) */
    p->fis = m + 0x400;              p->fis_phys = p->mem_phys + 0x400;         /* 256 B aligned */
    p->ident = m + 0x600;            p->ident_phys = p->mem_phys + 0x600;
    p->ctbl = m + 0x1000;            p->ctbl_phys = p->mem_phys + 0x1000;       /* 128 B aligned */
    return true;
}

static const char *spd_name(u32 ssts) {
    switch ((ssts >> 4) & 0xF) {
    case 1: return "1.5 Gbps";
    case 2: return "3 Gbps";
    case 3: return "6 Gbps";
    default: return "unknown speed";
    }
}

static void port_probe(AhciHba *h, int n) {
    if (PORT_BASE(n) + 0x80 > h->pci->bar[5].size) {
        klog("Kappa", "ahci%d: port %d lies beyond the register window; ignored", h->index, n);
        return;
    }
    AhciPort *p = kzalloc(sizeof *p);
    if (!p) return;
    p->hba = h;
    p->num = n;
    p->regs = h->mmio + PORT_BASE(n);
    mutex_init(&p->lock);

    PortProbe st = port_detect(p);
    if (st == PORT_EMPTY) { kfree(p); return; }
    if (st == PORT_STUCK) {
        klog("Kappa", "ahci%d port %d: the command engine will not stop; port ignored", h->index, n);
        kfree(p);
        return;
    }
    if (!port_alloc(p)) {
        klog("Kappa", "ahci%d port %d: out of DMA memory", h->index, n);
        port_free(p);
        return;
    }
    port_program_lists(p);
    if (!port_link_ready(p)) {
        klog("Kappa", "ahci%d port %d: a device is attached but did not come up (SSTS %#x, TFD %#x)", h->index, n, PR(p, PX_SSTS), PR(p, PX_TFD));
        port_teardown(p);
        return;
    }
    u32 ssts = PR(p, PX_SSTS);
    u32 sig = PR(p, PX_SIG);
    if (sig == SIG_ATAPI) { klog("Kappa", "ahci%d port %d: ATAPI device (CD/DVD) -- not supported, ignored", h->index, n); goto reject; }
    if (sig == SIG_PM) { klog("Kappa", "ahci%d port %d: port multiplier -- not supported, ignored", h->index, n); goto reject; }
    if (sig == SIG_SEMB) { klog("Kappa", "ahci%d port %d: enclosure management bridge -- ignored", h->index, n); goto reject; }
    if (sig != SIG_ATA)
        klog("Kappa", "ahci%d port %d: unusual device signature %#x, trying it as a disk", h->index, n, sig);

    port_start_engine(p);
    AtaInfo info;
    int rc = port_identify(p, &info);
    if (rc && !p->dead) {                           /* once more after a link reset */
        port_recover(p, "identify");
        if (!p->dead) rc = port_identify(p, &info);
    }
    if (rc) {
        klog("Kappa", "ahci%d port %d: IDENTIFY DEVICE failed (%d); ignoring this port", h->index, n, rc);
        goto reject;
    }
    if (info.atapi) { klog("Kappa", "ahci%d port %d: ATAPI device (CD/DVD) -- not supported, ignored", h->index, n); goto reject; }
    if (!info.lba48 || !info.sectors) {
        klog("Kappa", "ahci%d port %d: %s has no 48-bit LBA support (%lu sectors) -- not supported, ignored", h->index, n, info.model,
             (unsigned long)info.sectors);
        goto reject;
    }
    if (info.sector_size != 512 && info.sector_size != 4096) {
        klog("Kappa", "ahci%d port %d: %s uses %u-byte logical sectors -- only 512 and 4096 are supported, ignored", h->index, n, info.model,
             info.sector_size);
        goto reject;
    }
    if (info.locked) klog("Kappa", "ahci%d port %d: the disk is ATA-security locked; reads and writes will fail until it is unlocked", h->index, n);

    strlcpy(p->model, info.model, sizeof p->model);
    strlcpy(p->serial, info.serial, sizeof p->serial);
    strlcpy(p->firmware, info.firmware, sizeof p->firmware);
    p->sector_size = info.sector_size;
    p->sectors = info.sectors;
    p->flush_cmd = info.flush_cmd;
    p->ssd = info.ssd;
    p->locked = info.locked;
    p->phys_shift = info.phys_shift;
    h->port[n] = p;

    klog("Kappa", "ahci%d port %d: %s %s link, \"%s\" serial %s firmware %s", h->index, n, spd_name(ssts), info.ssd ? "SSD" : "disk", p->model,
         p->serial[0] ? p->serial : "-", p->firmware[0] ? p->firmware : "-");
    klog("Kappa", "ahci%d port %d: %lu MiB, %lu sectors of %u bytes%s%s", h->index, n, (unsigned long)(p->sectors * p->sector_size >> 20),
         (unsigned long)p->sectors, p->sector_size, p->phys_shift ? " (512e: larger physical sectors)" : "", p->flush_cmd ? "" : ", no cache flush command");

    BlockDev *bd = kzalloc(sizeof *bd);
    if (!bd) return;
    sd_name(bd->name, sizeof bd->name, g_sd_next++);
    strlcpy(bd->model, p->model, sizeof bd->model);
    bd->driver = "ahci";
    bd->sector_size = p->sector_size;
    bd->sectors = p->sectors;
    bd->read = ahci_blk_read;
    bd->write = ahci_blk_write;
    bd->flush = ahci_blk_flush;
    bd->priv = p;
    p->bd = bd;
    blk_register(bd);
    return;

reject:
    port_teardown(p);
}

/* ---------------------------------------------------------------- controller bring-up */

static void hba_enable_ahci(AhciHba *h) {
    u32 ghc = HR(h, HBA_GHC);
    if (!(ghc & GHC_AE)) {
        HW(h, HBA_GHC, ghc | GHC_AE);
        (void)HR(h, HBA_GHC);
    }
    HW(h, HBA_GHC, (HR(h, HBA_GHC) | GHC_AE) & ~GHC_IE);      /* we poll: no interrupts from the HBA */
    HW(h, HBA_IS, 0xFFFFFFFFu);
}

/* BIOS/OS handoff (AHCI 1.3.1 section 10.6.3): ask the firmware to release the controller if it claims to own it. */
static void hba_handoff(AhciHba *h) {
    if (!(h->cap2 & CAP2_BOH)) return;
    u32 bohc = HR(h, HBA_BOHC);
    if (!(bohc & BOHC_BOS)) return;                         /* the firmware does not hold the semaphore */
    HW(h, HBA_BOHC, bohc | BOHC_OOS);
    if (!BLK_POLL(!(HR(h, HBA_BOHC) & BOHC_BOS), 25)) {
        if (HR(h, HBA_BOHC) & BOHC_BB) BLK_POLL(!(HR(h, HBA_BOHC) & BOHC_BOS), 2000);   /* BIOS busy: allowed up to 2 s */
    }
    if (HR(h, HBA_BOHC) & BOHC_BOS)
        klog("Kappa", "ahci%d: the firmware did not release the controller (BOHC %#x); continuing anyway", h->index, HR(h, HBA_BOHC));
    else
        klog("Kappa", "ahci%d: took the controller over from the firmware (BIOS/OS handoff)", h->index);
}

static int hba_reset(AhciHba *h) {
    u32 saved_pi = h->pi;
    hba_enable_ahci(h);
    HW(h, HBA_GHC, HR(h, HBA_GHC) | GHC_HR);
    if (!BLK_POLL(!(HR(h, HBA_GHC) & GHC_HR), 1000)) return -EIO;
    hba_enable_ahci(h);
    if (HR(h, HBA_PI) == 0 && saved_pi) HW(h, HBA_PI, saved_pi);        /* PI is write-once on some chips and reads 0 after a reset */
    return 0;
}

static void hba_probe(PciDev *pci) {
    AhciHba *h = kzalloc(sizeof *h);
    if (!h) return;
    h->pci = pci;
    h->index = g_nhba;

    pci_enable(pci, PCI_CMD_MEM | PCI_CMD_MASTER);
    h->mmio = pci_map_bar(pci, 5);
    if (!h->mmio) {
        klog("Kappa", "ahci: %02x:%02x.%x %04x:%04x has no memory BAR5; skipped", pci->bus, pci->dev, pci->fn, pci->vendor, pci->device);
        kfree(h);
        return;
    }
    h->cap = HR(h, HBA_CAP);
    h->vs = HR(h, HBA_VS);
    if (h->cap == 0xFFFFFFFFu || h->vs == 0xFFFFFFFFu) {
        klog("Kappa", "ahci: %02x:%02x.%x %04x:%04x does not answer (registers read all ones); skipped", pci->bus, pci->dev, pci->fn, pci->vendor,
             pci->device);
        kfree(h);
        return;
    }
    h->cap2 = h->vs >= 0x00010200 ? HR(h, HBA_CAP2) : 0;
    h->s64a = (h->cap & CAP_S64A) != 0;
    pci->driver = "ahci";
    g_nhba++;
    if (g_hba_tail) g_hba_tail->next = h; else g_hba_head = h;
    g_hba_tail = h;

    hba_handoff(h);
    hba_enable_ahci(h);

    h->pi = HR(h, HBA_PI);
    u32 nports = (h->cap & 0x1F) + 1;
    if (!h->pi) {                                           /* firmware never programmed the port map: assume the first NP ports */
        h->pi = nports >= 32 ? 0xFFFFFFFFu : ((1u << nports) - 1);
        klog("Kappa", "ahci%d: PI register is empty; assuming ports 0-%u are implemented", h->index, nports - 1);
    }
    klog("Kappa", "ahci%d: %04x:%04x at %02x:%02x.%x, AHCI %x.%x.%x, %u of %u ports implemented, %u command slots, %s DMA%s", h->index,
         pci->vendor, pci->device, pci->bus, pci->dev, pci->fn, h->vs >> 16, (h->vs >> 8) & 0xFF, h->vs & 0xFF, (unsigned)__builtin_popcount(h->pi),
         nports, ((h->cap >> 8) & 0x1F) + 1, h->s64a ? "64-bit" : "32-bit", (h->cap & CAP_SNCQ) ? ", NCQ (unused)" : "");

    /* Quiesce every implemented port first; the HBA is only reset if a command engine refuses to stop. */
    bool stuck = g_force_hba_reset;
    for (u32 n = 0; n < 32 && !stuck; n++) {
        if (!(h->pi & (1u << n)) || PORT_BASE(n) + 0x80 > pci->bar[5].size) continue;
        AhciPort tmp = { .hba = h, .num = (int)n, .regs = h->mmio + PORT_BASE(n) };
        if (port_stop_engine(&tmp) || port_stop_fis(&tmp)) {
            klog("Kappa", "ahci%d: port %u's command engine will not stop", h->index, n);
            stuck = true;
        }
    }
    if (stuck) {
        klog("Kappa", "ahci%d: resetting the controller%s", h->index, g_force_hba_reset ? " (requested on the command line)" : "");
        if (hba_reset(h)) klog("Kappa", "ahci%d: controller reset did not complete; trying the ports anyway", h->index);
    }

    for (u32 n = 0; n < 32; n++)
        if (h->pi & (1u << n)) port_probe(h, (int)n);

    int disks = 0;
    for (int n = 0; n < 32; n++) if (h->port[n]) disks++;
    if (!disks) klog("Kappa", "ahci%d: no disks found", h->index);
}

/* ---------------------------------------------------------------- /proc/ahci */

static size_t gen_proc_ahci(char *buf, size_t cap) {
    size_t n = 0;
    for (AhciHba *h = g_hba_head; h && n + 300 < cap; h = h->next) {
        n += (size_t)snprintf(buf + n, cap - n, "ahci%d %02x:%02x.%x %04x:%04x version %x.%x.%x cap %#x pi %#x %s\n", h->index, h->pci->bus, h->pci->dev,
                              h->pci->fn, h->pci->vendor, h->pci->device, h->vs >> 16, (h->vs >> 8) & 0xFF, h->vs & 0xFF, h->cap, h->pi,
                              h->s64a ? "64-bit" : "32-bit");
        for (int i = 0; i < 32 && n + 300 < cap; i++) {
            AhciPort *p = h->port[i];
            if (!p) continue;
            n += (size_t)snprintf(buf + n, cap - n, "  port %d %s \"%s\" serial %s fw %s %lu x %u%s  cmds %u errors %u resets %u%s\n", i, p->bd ? p->bd->name : "-",
                                  p->model, p->serial, p->firmware, (unsigned long)p->sectors, p->sector_size, p->ssd ? " ssd" : "", p->n_cmds,
                                  p->n_errors, p->n_resets, p->dead ? "  DEAD" : "");
        }
    }
    return n;
}

/* ---------------------------------------------------------------- entry point */

static void read_knobs(void) {
    long v;
    if ((v = cmdline_int("ahci.maxsect", 0)) > 0) g_maxsect = (u32)MIN(v, 65535L);
    if ((v = cmdline_int("ahci.prdlen", 0)) >= 2) g_prd_len = (u32)MIN(v, (long)AHCI_PRD_MAX_BYTES) & ~1u;
    if ((v = cmdline_int("ahci.prdmax", 0)) > 0) g_prd_max = (u32)MIN(v, (long)AHCI_MAX_PRD);
    g_force_bounce = cmdline_flag("ahci.bounce");
    g_force_hba_reset = cmdline_flag("ahci.hbareset");
}

void ahci_init(void) {
    read_knobs();
    for (PciDev *d = pci_find_class(0x01, 0x06, 0x01, NULL); d; d = pci_find_class(0x01, 0x06, 0x01, d))
        hba_probe(d);
    if (g_nhba) procfs_register("ahci", gen_proc_ahci);

    /* The most common reason a PC's disks are invisible: Intel RST / RAID mode hides them behind a proprietary interface. */
    for (PciDev *d = pci_find_class(0x01, 0x04, -1, NULL); d; d = pci_find_class(0x01, 0x04, -1, d))
        if (d->vendor == 0x8086)
            klog("Kappa", "ahci: %02x:%02x.%x %04x:%04x: SATA controller is in RAID (Intel RST) mode: switch it to AHCI in the BIOS setup to use its disks",
                 d->bus, d->dev, d->fn, d->vendor, d->device);
    for (PciDev *d = pci_find_class(0x01, 0x01, -1, NULL); d; d = pci_find_class(0x01, 0x01, -1, d))
        if (d->vendor == 0x8086 && d->dev == 31 && (d->fn == 2 || d->fn == 5))
            klog("Kappa", "ahci: %02x:%02x.%x %04x:%04x: Intel SATA controller is in legacy IDE mode: switch it to AHCI in the BIOS setup to use its disks",
                 d->bus, d->dev, d->fn, d->vendor, d->device);

    blk_selftest("ahci");
}
