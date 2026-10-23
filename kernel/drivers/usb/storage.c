/*
 * USB mass storage: the bulk-only transport (BBB, interface class 8 / subclass 6 / protocol 0x50) with the SCSI commands a disk needs.
 * Sticks, card readers and USB enclosures show up as sda, sdb, ... in the block layer (driver "usb-storage"), partitions and all.
 *
 * Who runs where (docs/USB.md, "One thread, no interrupts, no locks"): the usbd thread owns the whole USB stack. This driver's
 *   - probe / disconnect and the bring-up state machine (msc_service, one SCSI command per usbd round per LUN) run IN usbd;
 *   - block requests (BlockDev.read/write/flush) arrive on arbitrary threads and are handed to usbd with usb_call(), one SCSI command
 *     of at most 64 KiB per hand-over, so usbd keeps servicing hubs and keyboards between two commands.
 *
 * Bring-up of a LUN is asynchronous on purpose: sticks need seconds to become ready, and while one sleeps through spin-up the keyboard
 * that sits next to it must still enumerate. LUN states: START (INQUIRY) -> WAIT (TEST UNIT READY with REQUEST SENSE, bounded by
 * READY_TIMEOUT_MS) -> ONLINE (READ CAPACITY, MODE SENSE for the write-protect switch, blk_register_sd, the mount policy is told).
 * A removable unit without medium sits in NOMEDIA and is asked again every two seconds; a unit that fails for good ends in FAILED.
 *
 * Everything the device says is validated: the CSW signature, tag and residue, the sense data, the capacity (zero, overflowing,
 * absurd), the block size (512 or 4096 only), the LUN count. A device that answers nonsense gets a bulk-only reset (reset recovery:
 * class request 0xFF, then CLEAR_FEATURE(HALT) on both bulk endpoints) and one more try; a device that times out, vanishes or fails
 * the recovery is given up on: its disks are unregistered and every later request fails with -ENODEV. Nothing in here ever writes
 * on its own initiative: WRITE(10/16) and SYNCHRONIZE CACHE exist only on the path of BlockDev.write / BlockDev.flush.
 *
 * Kernel command line (all optional, for tests and for the cautious):
 *   usbstorage=off          do not bind mass storage devices at all
 *   usbstorage.nosync       no SYNCHRONIZE CACHE after write requests
 *   usbstorage.rw16         use READ CAPACITY(16) and READ/WRITE(16) even for small devices
 *   usbstorage.stalltest    after bring-up send a command for a LUN that does not exist (QEMU stalls it) and check that recovery works
 *   usbstorage.selftest     run the validators against hand-made hostile device answers and log the verdict
 *   usbstorage.timeout=MS   every bulk wait (command, data, status) at most MS milliseconds (tests only; a timeout kills the device)
 *   usbstorage.fault=csw:N  treat the N-th status wrapper as corrupted (exercises reset recovery)
 *   usbstorage.fault=drop:N never send the N-th command wrapper (the device stays silent: exercises the timeout path)
 *   usbstorage.fault=notready:MS  every unit is taken to be "not ready" for its first MS milliseconds (a stick that takes its time)
 */
#include "usb.h"
#include "blk.h"
#include "kernel.h"
#include "cmdline.h"
#include "mm.h"
#include "task.h"
#include "cpu.h"
#include "irq.h"
#include "abi.h"

#define MSC_MAX_LUN      8
#define MSC_CHUNK        65536                  /* data bytes per SCSI command: exactly one bulk transfer */
#define MSC_MAX_BYTES    (1ull << 50)           /* a device that claims more than a pebibyte is lying */
#define READY_TIMEOUT_MS 20000                  /* how long a unit may stay "not ready" before it is given up on */
#define NOMEDIA_POLL_MS  2000

#define T_CTRL           3000                   /* control requests (GET MAX LUN, reset recovery) */

static u32 g_t_cbw = 3000, g_t_quick = 5000, g_t_csw = 5000, g_t_read = 15000, g_t_write = 30000, g_t_sync = 30000;
static bool g_cfg_loaded, g_off, g_nosync, g_rw16, g_stalltest;
static u32 g_fault_csw, g_fault_drop, g_fault_notready, g_csw_count, g_cbw_count;

/* ---------------------------------------------------------------- state */

typedef enum { LS_START, LS_WAIT, LS_NOMEDIA, LS_ONLINE, LS_FAILED, LS_GONE } LunState;
static const char *const g_state_name[] = { "starting", "waiting for the unit to become ready", "no medium", "online", "given up", "unplugged" };

typedef struct Msc Msc;

typedef struct MscLun {
    Msc *msc;
    u8   lun;
    u8   state;
    u8   ptype;                                 /* SCSI peripheral device type */
    bool removable, wp, use16, no_sync, announced;
    u64  next_at, started_at;                   /* uptime_ms() */
    u64  sectors;
    u32  bsz;
    char model[44];                             /* "VENDOR PRODUCT" */
    char why[72];                               /* why it is FAILED / NOMEDIA */
    BlockDev *bd;                               /* registered disk, NULL while offline (the struct itself stays valid) */
    u32  reads, writes, syncs, errors;
    u64  bytes_read, bytes_written;
} MscLun;

struct Msc {
    UsbDevice *dev;                             /* NULL once the stick is unplugged */
    UsbInterface *intf;
    UsbEndpoint *in, *out;
    u8   nlun;
    volatile bool gone;                         /* unplugged */
    volatile bool dead;                         /* still plugged in but given up on (timeout, failed recovery) */
    u32  tag;
    u32  resets;
    u16  vid, pid;
    char path[24];
    bool ever_online;                           /* a BlockDev points into this struct: it must never be freed */
    Msc *next;
    MscLun lun[];
};

static Msc *g_msc;                              /* oldest first; modified only by usbd, read by /proc with interrupts off (one CPU) */
static volatile int g_nbusy;                    /* LUNs in START or WAIT: what usb_wait_settled() waits for */

static void recount(void) {
    int n = 0;
    for (Msc *m = g_msc; m; m = m->next) {
        if (m->gone || m->dead) continue;
        for (int i = 0; i < m->nlun; i++)
            if (m->lun[i].state == LS_START || m->lun[i].state == LS_WAIT) n++;
    }
    g_nbusy = n;
}

bool msc_busy(void) { return g_nbusy > 0; }

/* ---------------------------------------------------------------- byte order helpers */

static inline u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static inline u64 be64(const u8 *p) { return (u64)be32(p) << 32 | be32(p + 4); }
static inline void put_be32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static inline void put_be64(u8 *p, u64 v) { put_be32(p, (u32)(v >> 32)); put_be32(p + 4, (u32)v); }
static inline void put_le32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static inline u32 get_le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

/* ---------------------------------------------------------------- pure validators (also run by usbstorage.selftest) */

typedef struct { u8 key, asc, ascq; bool valid; } Sense;
typedef enum { SA_FATAL, SA_RETRY, SA_NOMEDIA, SA_CHANGED, SA_PROTECT, SA_UNSUPPORTED } SenseAction;

/* Fixed (0x70/0x71) or descriptor (0x72/0x73) format sense data. Anything else, or anything too short, is not believed. */
static void sense_parse(const u8 *b, u32 n, Sense *s) {
    memset(s, 0, sizeof *s);
    if (n < 8) return;
    u8 code = b[0] & 0x7F;
    if (code == 0x70 || code == 0x71) {
        s->key = b[2] & 0x0F;
        if (n >= 14 && b[7] >= 6) { s->asc = b[12]; s->ascq = b[13]; }
        s->valid = true;
    } else if (code == 0x72 || code == 0x73) {
        s->key = b[1] & 0x0F;
        s->asc = b[2];
        s->ascq = b[3];
        s->valid = true;
    }
}

static SenseAction sense_action(const Sense *s) {
    if (!s->valid) return SA_FATAL;
    switch (s->key) {
    case 0x1: return SA_RETRY;                                                  /* recovered error */
    case 0x2:                                                                   /* not ready */
        if (s->asc == 0x3A || s->asc == 0x30) return SA_NOMEDIA;                /* medium not present / incompatible medium */
        return SA_RETRY;                                                        /* becoming ready, initializing, ... */
    case 0x5: return SA_UNSUPPORTED;                                            /* illegal request */
    case 0x6: return s->asc == 0x28 ? SA_CHANGED : SA_RETRY;                    /* unit attention: 28 = the medium may have changed */
    case 0x7: return SA_PROTECT;                                                /* data protect */
    case 0xB: return SA_RETRY;                                                  /* aborted command */
    default: return SA_FATAL;                                                   /* medium error, hardware error, blank check, no sense, ... */
    }
}

/* The numbers READ CAPACITY gave: NULL if they are usable, else why not. */
static const char *cap_check(u64 last_lba, u32 bsz, u64 *sectors) {
    if (bsz == 0) return "block size zero";
    if (bsz & (bsz - 1)) return "block size is not a power of two";
    if (bsz != 512 && bsz != 4096) return "only 512 and 4096 byte blocks are supported";
    if (last_lba == ~0ull) return "capacity overflows";
    u64 n = last_lba + 1;
    if (n > MSC_MAX_BYTES / bsz) return "capacity is absurd";
    *sectors = n;
    return NULL;
}

/* CSW: signature, tag, status, residue. NULL = fine, else what is wrong (the caller then does reset recovery). */
static const char *csw_check(const u8 *csw, u32 tag, u32 request_len) {
    if (get_le32(csw) != 0x53425355u) return "bad status signature";
    if (get_le32(csw + 4) != tag) return "status tag does not match the command";
    if (csw[12] > 2) return "invalid status byte";
    if (csw[12] == 2) return "phase error";
    if (get_le32(csw + 8) > request_len) return "residue larger than the request";
    return NULL;
}

enum { INQ_OK = 0, INQ_ABSENT = 100, INQ_UNSUPPORTED = 101 };

static void clean_str(char *dst, size_t cap, const u8 *src, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) dst[o++] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : (src[i] ? '?' : ' ');
    while (o && dst[o - 1] == ' ') o--;
    dst[o] = 0;
}

/* Reads an INQUIRY answer into the LUN. */
static int inquiry_parse(const u8 *b, u32 got, MscLun *l) {
    if (got < 5) return -EIO;                                                   /* too short to be one */
    u8 qualifier = b[0] >> 5, type = b[0] & 0x1F;
    if (qualifier != 0 || type == 0x1F) return INQ_ABSENT;                      /* "no device at this LUN" */
    l->ptype = type;
    l->removable = (b[1] & 0x80) != 0;
    char vendor[9] = "", product[17] = "";
    if (got >= 36) {
        clean_str(vendor, sizeof vendor, b + 8, 8);
        clean_str(product, sizeof product, b + 16, 16);
    }
    snprintf(l->model, sizeof l->model, "%s%s%s", vendor, vendor[0] && product[0] ? " " : "", product);
    return (type == 0x00 || type == 0x0E || type == 0x07) ? INQ_OK : INQ_UNSUPPORTED;      /* disk, RBC disk, optical memory (512 byte) */
}

/* ---------------------------------------------------------------- one SCSI command over the bulk-only transport */

typedef struct {
    const u8 *cdb;
    u8   cdb_len;
    void *data;
    u32  len;
    bool in;
    u32  t_data;
    u32  got;                                   /* bytes that really moved in the data stage */
    u32  residue;
    u8   csw_status;
} ScsiCmd;

enum { MSC_OK = 0, MSC_CHECK = 1 };             /* msc_run: command passed / failed with CHECK CONDITION; negative = transport trouble */

static bool fatal_rc(int rc) { return rc == -ENODEV || rc == -ETIMEDOUT; }

/* Bulk-only mass storage reset + CLEAR_FEATURE(HALT) on both bulk endpoints (the order the specification asks for). */
static int msc_reset_recovery(Msc *m) {
    UsbDevice *dev = m->dev;
    if (!dev) return -ENODEV;
    m->resets++;
    usb_dbg("usb %s: bulk-only reset recovery #%u", m->path, m->resets);
    int rc = usb_control(dev, USB_TYPE_CLASS | USB_RECIP_IFACE, 0xFF, 0, m->intf->number, NULL, 0, T_CTRL);
    if (rc < 0) return rc;                                                      /* a stick that stalls its own reset is beyond help */
    int r1 = usb_clear_halt(dev, m->in);
    int r2 = usb_clear_halt(dev, m->out);
    if (r1 < 0) return r1;
    return r2 < 0 ? r2 : 0;
}

/* After garbage on the wire: reset recovery, and a verdict for the caller (-EIO = try the command again, otherwise the stick is lost). */
static int msc_garbage(Msc *m, const char *what) {
    usb_dbg("usb %s: %s", m->path, what);
    int rc = msc_reset_recovery(m);
    if (rc) {
        klog("Kappa", "usb-storage %s: %s, and the reset recovery failed too (%d): the stick is given up on", m->path, what, rc);
        return fatal_rc(rc) ? rc : -ENODEV;
    }
    return -EIO;
}

static int msc_run(Msc *m, u8 lun, ScsiCmd *c) {
    UsbDevice *dev = m->dev;
    if (!dev || m->gone || m->dead) return -ENODEV;
    if (!c->cdb_len || c->cdb_len > 16 || c->len > MSC_CHUNK) return -EINVAL;

    u8 cbw[31];
    u32 tag = ++m->tag;
    memset(cbw, 0, sizeof cbw);
    cbw[0] = 'U'; cbw[1] = 'S'; cbw[2] = 'B'; cbw[3] = 'C';
    put_le32(cbw + 4, tag);
    put_le32(cbw + 8, c->len);
    cbw[12] = (c->len && c->in) ? 0x80 : 0;
    cbw[13] = lun;
    cbw[14] = c->cdb_len;
    memcpy(cbw + 15, c->cdb, c->cdb_len);
    c->got = 0;
    c->residue = 0;
    c->csw_status = 0;

    int rc;
    if (g_fault_drop && ++g_cbw_count == g_fault_drop) {
        klog("Kappa", "usb-storage %s: fault injection: command wrapper %u is never sent", m->path, g_cbw_count);
        rc = 31;
    } else {
        rc = usb_bulk(dev, m->out, cbw, sizeof cbw, g_t_cbw);
    }
    if (rc != (int)sizeof cbw) {
        if (rc == -EPIPE) {                                                     /* the device refused the wrapper: clear the stall and reset */
            usb_clear_halt(dev, m->out);
            return msc_garbage(m, "the command wrapper was stalled");
        }
        if (rc >= 0 || !fatal_rc(rc)) return msc_garbage(m, "the command wrapper was not accepted");
        return rc;
    }

    if (c->len) {
        UsbEndpoint *ep = c->in ? m->in : m->out;
        rc = usb_bulk(dev, ep, c->data, c->len, c->t_data);
        if (rc >= 0) {
            if ((u32)rc > c->len) return msc_garbage(m, "the data stage moved more than was asked for");
            c->got = (u32)rc;
        } else if (rc == -EPIPE) {                                              /* stalled data stage: clear it and read the status */
            if (usb_clear_halt(dev, ep) < 0) return msc_garbage(m, "cannot clear a stalled data endpoint");
        } else if (fatal_rc(rc)) {
            return rc;
        } else {
            return msc_garbage(m, "the data stage failed");
        }
    }

    u8 csw[13];
    rc = usb_bulk(dev, m->in, csw, sizeof csw, g_t_csw);
    if (rc == -EPIPE) {                                                         /* a stalled status gets one more try after clearing */
        usb_clear_halt(dev, m->in);
        rc = usb_bulk(dev, m->in, csw, sizeof csw, g_t_csw);
    }
    if (rc != (int)sizeof csw) {
        if (rc < 0 && fatal_rc(rc)) return rc;
        return msc_garbage(m, "the status wrapper is missing");
    }
    if (g_fault_csw && ++g_csw_count == g_fault_csw) {
        klog("Kappa", "usb-storage %s: fault injection: status wrapper %u is corrupted", m->path, g_csw_count);
        csw[0] ^= 0xFF;
    }
    const char *bad = csw_check(csw, tag, c->len);
    if (bad) return msc_garbage(m, bad);
    c->residue = get_le32(csw + 8);
    c->csw_status = csw[12];
    return c->csw_status == 0 ? MSC_OK : MSC_CHECK;
}

/* REQUEST SENSE after a command failed with CHECK CONDITION. */
static int msc_sense(Msc *m, u8 lun, Sense *s) {
    u8 cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    u8 buf[18];
    memset(buf, 0, sizeof buf);
    ScsiCmd c = { .cdb = cdb, .cdb_len = 6, .data = buf, .len = sizeof buf, .in = true, .t_data = g_t_quick };
    int rc = msc_run(m, lun, &c);
    memset(s, 0, sizeof *s);
    if (rc != MSC_OK) return rc ? rc : -EIO;
    sense_parse(buf, c.got, s);
    return 0;
}

/* The stick stopped answering (timeout, unplugged, recovery failed): drop its disks and refuse every later request. */
static void lun_drop(MscLun *l, LunState to, const char *why);

static void msc_died(Msc *m, const char *why) {
    if (m->dead || m->gone) return;
    m->dead = true;
    klog("Kappa", "usb-storage %s: %s -- the stick is given up on", m->path, why);
    for (int i = 0; i < m->nlun; i++) {
        MscLun *l = &m->lun[i];
        if (l->state != LS_GONE) lun_drop(l, LS_FAILED, why);
    }
    recount();
}

/* ---------------------------------------------------------------- block devices */

static int msc_io_one(MscLun *l, BlockDev *bd, u64 lba, u32 count, void *buf, bool write);
static int msc_sync_one(MscLun *l, BlockDev *bd);

typedef struct { MscLun *l; BlockDev *bd; u64 lba; u32 count; void *buf; bool write; } MscIo;

static int io_job(void *arg) {
    MscIo *io = arg;
    return msc_io_one(io->l, io->bd, io->lba, io->count, io->buf, io->write);
}

static int sync_job(void *arg) {
    MscIo *io = arg;
    return msc_sync_one(io->l, io->bd);
}

static int msc_blk_rw(BlockDev *d, u64 lba, u32 count, void *buf, bool write) {
    MscLun *l = d->priv;
    u32 per = MSC_CHUNK / d->sector_size;
    while (count) {
        if (d->removed) return -ENODEV;
        u32 n = count < per ? count : per;
        MscIo io = { l, d, lba, n, buf, write };
        int rc = usb_call(io_job, &io);                                         /* one SCSI command, run by the usbd thread */
        if (rc) return rc;
        lba += n;
        count -= n;
        buf = (u8 *)buf + (size_t)n * d->sector_size;
    }
    return 0;
}

static int msc_blk_read(BlockDev *d, u64 lba, u32 count, void *buf) { return msc_blk_rw(d, lba, count, buf, false); }

static int msc_blk_flush(BlockDev *d) {
    MscLun *l = d->priv;
    if (d->removed) return -ENODEV;
    MscIo io = { l, d, 0, 0, NULL, false };
    return usb_call(sync_job, &io);
}

/* Write-through, like the SATA and NVMe drivers: nothing above the block layer flushes at power-off, so a write that returned has been
 * pushed out of the stick's own cache (SYNCHRONIZE CACHE once per request, however large). */
static int msc_blk_write(BlockDev *d, u64 lba, u32 count, const void *buf) {
    int rc = msc_blk_rw(d, lba, count, (void *)buf, true);
    if (!rc && count && !g_nosync) rc = msc_blk_flush(d);
    return rc;
}

/* A unit that lost its medium, or got a different one, or died: take its disk away. Mounted filesystems see -ENODEV from now on. */
static void lun_drop(MscLun *l, LunState to, const char *why) {
    if (l->bd) {
        BlockDev *bd = l->bd;
        l->bd = NULL;                                                           /* in-flight requests compare against this and give up */
        blk_unregister(bd);
    }
    l->state = (u8)to;
    l->started_at = uptime_ms();
    l->next_at = l->started_at + (to == LS_NOMEDIA ? NOMEDIA_POLL_MS : 0);
    strlcpy(l->why, why ? why : "", sizeof l->why);
}

/* Runs on the usbd thread. Returns 0 or -errno for the block layer. */
static int msc_io_one(MscLun *l, BlockDev *bd, u64 lba, u32 count, void *buf, bool write) {
    Msc *m = l->msc;
    if (m->gone || m->dead || !m->dev || l->state != LS_ONLINE || l->bd != bd) return -ENODEV;
    u32 bytes = count * bd->sector_size;
    for (int attempt = 0; attempt < 3; attempt++) {
        u8 cdb[16];
        memset(cdb, 0, sizeof cdb);
        bool big = l->use16 || lba > 0xFFFFFFFFull || lba + count - 1 > 0xFFFFFFFFull || count > 0xFFFF;
        u8 cdb_len;
        if (big) {                                                              /* READ(16) / WRITE(16) */
            cdb[0] = write ? 0x8A : 0x88;
            put_be64(cdb + 2, lba);
            put_be32(cdb + 10, count);
            cdb_len = 16;
        } else {                                                                /* READ(10) / WRITE(10) */
            cdb[0] = write ? 0x2A : 0x28;
            put_be32(cdb + 2, (u32)lba);
            cdb[7] = (u8)(count >> 8);
            cdb[8] = (u8)count;
            cdb_len = 10;
        }
        ScsiCmd c = { .cdb = cdb, .cdb_len = cdb_len, .data = buf, .len = bytes, .in = !write, .t_data = write ? g_t_write : g_t_read };
        int rc = msc_run(m, l->lun, &c);
        if (rc == MSC_OK) {
            if (c.residue || (!write && c.got != bytes)) {                      /* the status says "good" but the data does not add up */
                l->errors++;
                usb_dbg("usb %s: %s of %u blocks at %lu: residue %u, %u of %u bytes moved", m->path, write ? "write" : "read", count,
                        (unsigned long)lba, c.residue, c.got, bytes);
                return -EIO;
            }
            if (write) { l->writes++; l->bytes_written += bytes; } else { l->reads++; l->bytes_read += bytes; }
            return 0;
        }
        if (rc == MSC_CHECK) {
            Sense s;
            int sr = msc_sense(m, l->lun, &s);
            if (sr < 0) {
                if (fatal_rc(sr)) { msc_died(m, "no answer to REQUEST SENSE"); return -EIO; }
                l->errors++;
                continue;                                                       /* recovery ran; try the command again */
            }
            usb_dbg("usb %s: %s at %lu failed: sense %x/%02x/%02x", m->path, write ? "write" : "read", (unsigned long)lba, s.key, s.asc, s.ascq);
            switch (sense_action(&s)) {
            case SA_RETRY:
                if (attempt < 2) { usb_msleep(m->dev->hc, 100); continue; }
                l->errors++;
                return -EIO;
            case SA_NOMEDIA:
                klog("Kappa", "usb-storage %s lun %u: the medium was taken out of %s", m->path, l->lun, bd->name);
                lun_drop(l, LS_NOMEDIA, "medium removed");
                recount();
                return -ENXIO;
            case SA_CHANGED:
                klog("Kappa", "usb-storage %s lun %u: the medium was changed under %s (unit attention): the disk is dropped and probed again", m->path, l->lun, bd->name);
                lun_drop(l, LS_START, "medium changed");
                recount();
                return -EIO;
            case SA_PROTECT:
                klog("Kappa", "usb-storage %s lun %u: %s is write-protected after all (data protect): switching it to read-only", m->path, l->lun, bd->name);
                bd->readonly = true;
                l->wp = true;
                return -EROFS;
            default:
                l->errors++;
                return -EIO;
            }
        }
        if (fatal_rc(rc)) {                                                     /* timeout or unplugged: nothing more can be done for this stick */
            if (rc == -ETIMEDOUT) msc_died(m, write ? "a write timed out" : "a read timed out");
            return rc == -ETIMEDOUT ? -EIO : -ENODEV;
        }
        if (rc == -ENODEV) return -ENODEV;                                      /* reset recovery failed: msc_garbage already said so */
        l->errors++;                                                            /* -EIO: recovery ran, ask again */
    }
    return -EIO;
}

/* SYNCHRONIZE CACHE(10) for the whole device; a stick that does not know the command is not asked again. */
static int msc_sync_one(MscLun *l, BlockDev *bd) {
    Msc *m = l->msc;
    if (m->gone || m->dead || !m->dev || l->state != LS_ONLINE || l->bd != bd) return -ENODEV;
    if (l->no_sync || bd->readonly) return 0;
    u8 cdb[10] = { 0x35 };
    for (int attempt = 0; attempt < 2; attempt++) {
        ScsiCmd c = { .cdb = cdb, .cdb_len = 10, .t_data = g_t_sync };
        u32 saved = g_t_csw;
        g_t_csw = g_t_sync;                                                     /* the status only comes when the cache is empty */
        int rc = msc_run(m, l->lun, &c);
        g_t_csw = saved;
        if (rc == MSC_OK) { l->syncs++; return 0; }
        if (rc == MSC_CHECK) {
            Sense s;
            if (msc_sense(m, l->lun, &s) == 0) {
                SenseAction a = sense_action(&s);
                if (a == SA_UNSUPPORTED) { l->no_sync = true; usb_dbg("usb %s: SYNCHRONIZE CACHE is not supported, not asked again", m->path); return 0; }
                if (a == SA_RETRY && attempt == 0) continue;
            }
            l->errors++;
            return -EIO;
        }
        if (rc == -ETIMEDOUT) { msc_died(m, "SYNCHRONIZE CACHE timed out"); return -EIO; }
        if (rc == -ENODEV) return -ENODEV;
        l->errors++;
    }
    return -EIO;
}

/* ---------------------------------------------------------------- bring-up of one LUN */

typedef enum { RDY_READY, RDY_WAIT, RDY_NOMEDIA, RDY_DEAD } Ready;

static Ready lun_ready(Msc *m, MscLun *l) {
    if (g_fault_notready && uptime_ms() - l->started_at < g_fault_notready) return RDY_WAIT;        /* test aid: a stick that takes its time */
    u8 cdb[6] = { 0 };
    ScsiCmd c = { .cdb = cdb, .cdb_len = 6, .t_data = g_t_quick };
    int rc = msc_run(m, l->lun, &c);
    if (rc == MSC_OK) return RDY_READY;
    if (rc == MSC_CHECK) {
        Sense s;
        int sr = msc_sense(m, l->lun, &s);
        if (sr < 0) return fatal_rc(sr) || sr == -ENODEV ? RDY_DEAD : RDY_WAIT;
        usb_dbg("usb %s lun %u: not ready: sense %x/%02x/%02x", m->path, l->lun, s.key, s.asc, s.ascq);
        switch (sense_action(&s)) {
        case SA_NOMEDIA: return RDY_NOMEDIA;
        case SA_UNSUPPORTED: return RDY_READY;                                  /* a unit that does not implement TEST UNIT READY: let READ CAPACITY judge */
        case SA_PROTECT: return RDY_READY;
        default: return RDY_WAIT;
        }
    }
    return (fatal_rc(rc) || rc == -ENODEV) ? RDY_DEAD : RDY_WAIT;
}

/* 0 = capacity known; 1 = the unit says it is not ready after all (go back to waiting); negative = give up (l->why says why). */
static int lun_capacity(Msc *m, MscLun *l, u64 *sectors, u32 *bsz) {
    u8 resp[32];
    u64 last = 0;
    u32 bs = 0;
    bool want16 = l->use16 || g_rw16;
    for (int pass = 0; pass < 2; pass++) {
        memset(resp, 0, sizeof resp);
        u8 cdb[16];
        memset(cdb, 0, sizeof cdb);
        ScsiCmd c = { .cdb = cdb, .data = resp, .in = true, .t_data = g_t_quick };
        if (!want16) {
            cdb[0] = 0x25;                                                      /* READ CAPACITY(10) */
            c.cdb_len = 10;
            c.len = 8;
        } else {
            cdb[0] = 0x9E;                                                      /* SERVICE ACTION IN(16): READ CAPACITY(16) */
            cdb[1] = 0x10;
            put_be32(cdb + 10, 32);
            c.cdb_len = 16;
            c.len = 32;
        }
        int rc = msc_run(m, l->lun, &c);
        if (rc == MSC_CHECK) {
            Sense s;
            int sr = msc_sense(m, l->lun, &s);
            if (sr < 0) { snprintf(l->why, sizeof l->why, "REQUEST SENSE failed (%d)", sr); return sr < 0 && (fatal_rc(sr) || sr == -ENODEV) ? -ENODEV : -EIO; }
            SenseAction a = sense_action(&s);
            if (a == SA_NOMEDIA || a == SA_RETRY || a == SA_CHANGED) return 1;
            snprintf(l->why, sizeof l->why, "READ CAPACITY failed (sense %x/%02x/%02x)", s.key, s.asc, s.ascq);
            return -EIO;
        }
        if (rc != MSC_OK) { snprintf(l->why, sizeof l->why, "READ CAPACITY failed (%d)", rc); return rc; }
        if (!want16) {
            if (c.got < 8) { strlcpy(l->why, "READ CAPACITY answered with too few bytes", sizeof l->why); return -EIO; }
            last = be32(resp);
            bs = be32(resp + 4);
            if (last == 0xFFFFFFFFull) { want16 = true; l->use16 = true; continue; }       /* more than 2 TiB (or a lie): ask the 16-byte way */
        } else {
            if (c.got < 12) { strlcpy(l->why, "READ CAPACITY(16) answered with too few bytes", sizeof l->why); return -EIO; }
            last = be64(resp);
            bs = be32(resp + 8);
            l->use16 = true;
        }
        break;
    }
    const char *bad = cap_check(last, bs, sectors);
    if (bad) { strlcpy(l->why, bad, sizeof l->why); return -EINVAL; }
    *bsz = bs;
    return 0;
}

/* MODE SENSE(6): the write-protect bit of the mode parameter header. A stick that does not answer is taken to be writable. */
static bool lun_write_protected(Msc *m, MscLun *l) {
    u8 cdb[6] = { 0x1A, 0, 0x3F, 0, 4, 0 };
    u8 buf[4] = { 0 };
    ScsiCmd c = { .cdb = cdb, .cdb_len = 6, .data = buf, .len = sizeof buf, .in = true, .t_data = g_t_quick };
    int rc = msc_run(m, l->lun, &c);
    if (rc == MSC_CHECK) { Sense s; msc_sense(m, l->lun, &s); return false; }
    if (rc != MSC_OK || c.got < 4) return false;
    return (buf[2] & 0x80) != 0;
}

static void stall_selftest(Msc *m, MscLun *l) {
    u8 cdb[6] = { 0 };
    ScsiCmd c = { .cdb = cdb, .cdb_len = 6, .t_data = g_t_quick };
    u32 before = m->resets;
    int r1 = msc_run(m, 7, &c);                                                 /* a LUN that does not exist: the stick stalls the wrapper */
    ScsiCmd c2 = { .cdb = cdb, .cdb_len = 6, .t_data = g_t_quick };
    int r2 = msc_run(m, l->lun, &c2);
    klog("Kappa", "usb-storage %s: stall test: a command for LUN 7 returned %d (%u reset recoveries), TEST UNIT READY afterwards returned %d", m->path, r1,
         m->resets - before, r2);
}

static const char *size_text(u64 bytes, char *buf, size_t cap) {
    if (bytes >= (1ull << 30)) snprintf(buf, cap, "%lu.%lu GiB", (unsigned long)(bytes >> 30), (unsigned long)(((bytes & ((1ull << 30) - 1)) * 10) >> 30));
    else snprintf(buf, cap, "%lu MiB", (unsigned long)(bytes >> 20));
    return buf;
}

static void lun_register(Msc *m, MscLun *l, u64 sectors, u32 bsz, bool wp) {
    BlockDev *bd = kzalloc(sizeof *bd);
    if (!bd) { lun_drop(l, LS_FAILED, "out of memory"); return; }
    bd->driver = "usb-storage";
    strlcpy(bd->model, l->model, sizeof bd->model);
    bd->sector_size = bsz;
    bd->sectors = sectors;
    bd->readonly = wp;                                                          /* blk.ro is for the internal disks; a stick follows its own switch */
    bd->read = msc_blk_read;
    bd->write = msc_blk_write;
    bd->flush = msc_blk_flush;
    bd->priv = l;
    l->sectors = sectors;
    l->bsz = bsz;
    l->wp = wp;
    l->bd = bd;                                                                 /* set first: the partition scan below reads through it */
    l->state = LS_ONLINE;
    m->ever_online = true;
    blk_register_sd(bd);                                                        /* names it, scans GPT/MBR (reads through msc_blk_read) */
    char sz[24];
    klog("Kappa", "usb-storage %s lun %u: \"%s\", %s in %u-byte blocks, %s -- carried over the river as %s", m->path, l->lun, l->model[0] ? l->model : "(no name)",
         size_text(sectors * bsz, sz, sizeof sz), bsz, wp ? "write-protected, the kappa keep the sluice gate shut (read-only)" : "writable", bd->name);
    if (g_stalltest) stall_selftest(m, l);
    storage_disk_added(bd);                                                     /* the mount policy looks at it on a worker thread */
}

static void lun_step(Msc *m, MscLun *l) {
    u64 now = uptime_ms();
    switch (l->state) {
    case LS_START: {
        u8 cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
        u8 buf[36];
        memset(buf, 0, sizeof buf);
        ScsiCmd c = { .cdb = cdb, .cdb_len = 6, .data = buf, .len = sizeof buf, .in = true, .t_data = g_t_quick };
        int rc = msc_run(m, l->lun, &c);
        if (rc == MSC_OK) rc = inquiry_parse(buf, c.got, l);
        else if (rc == MSC_CHECK) { Sense s; msc_sense(m, l->lun, &s); rc = (s.valid && s.key == 0x6) ? -EAGAIN : INQ_ABSENT; }
        if (rc == INQ_OK) {
            l->state = LS_WAIT;
            l->started_at = now;
            l->next_at = now;
            usb_dbg("usb %s lun %u: INQUIRY: \"%s\" type %02x%s", m->path, l->lun, l->model, l->ptype, l->removable ? ", removable" : "");
        } else if (rc == INQ_ABSENT) {
            lun_drop(l, LS_FAILED, "no device at this LUN");
            usb_dbg("usb %s lun %u: no device", m->path, l->lun);
        } else if (rc == INQ_UNSUPPORTED) {
            klog("Kappa", "usb-storage %s lun %u: \"%s\" is a SCSI device of type %02x (optical drive, tape, scanner...), not a 512 or 4096 byte disk -- ignored", m->path,
                 l->lun, l->model, l->ptype);
            lun_drop(l, LS_FAILED, "not a disk");
        } else if (fatal_rc(rc) || rc == -ENODEV) {
            msc_died(m, "no answer to INQUIRY");
        } else if (now - l->started_at > 3000) {
            klog("Kappa", "usb-storage %s lun %u: INQUIRY keeps failing (%d) -- ignored", m->path, l->lun, rc);
            lun_drop(l, LS_FAILED, "INQUIRY failed");
        } else {
            l->next_at = now + 150;                                             /* garbage or a unit attention: ask again for a few seconds */
        }
        break;
    }
    case LS_WAIT:
    case LS_NOMEDIA: {
        Ready r = lun_ready(m, l);
        if (r == RDY_DEAD) { msc_died(m, "no answer to TEST UNIT READY"); break; }
        if (r == RDY_NOMEDIA) {
            if (l->state != LS_NOMEDIA) {
                klog("Kappa", "usb-storage %s lun %u: \"%s\" has no medium in it -- asked again every %d seconds", m->path, l->lun, l->model, NOMEDIA_POLL_MS / 1000);
                lun_drop(l, LS_NOMEDIA, "no medium");
            }
            l->next_at = now + NOMEDIA_POLL_MS;
            break;
        }
        if (r == RDY_WAIT) {
            if (now - l->started_at > READY_TIMEOUT_MS) {
                klog("Kappa", "usb-storage %s lun %u: \"%s\" did not become ready within %d seconds -- given up on", m->path, l->lun, l->model, READY_TIMEOUT_MS / 1000);
                lun_drop(l, LS_FAILED, "did not become ready");
            } else {
                l->next_at = now + 250;
            }
            break;
        }
        /* ready: capacity, write protection, registration */
        u64 sectors = 0;
        u32 bsz = 0;
        int rc = lun_capacity(m, l, &sectors, &bsz);
        if (rc == 1) { l->state = LS_WAIT; l->next_at = now + 250; break; }
        if (rc < 0) {
            if (rc == -ENODEV || fatal_rc(rc)) { msc_died(m, "no answer to READ CAPACITY"); break; }
            klog("Kappa", "usb-storage %s lun %u: \"%s\": %s -- ignored", m->path, l->lun, l->model, l->why);
            lun_drop(l, LS_FAILED, l->why);
            break;
        }
        bool wp = lun_write_protected(m, l);
        if (m->dead || m->gone) break;
        lun_register(m, l, sectors, bsz, wp);
        break;
    }
    default:
        break;
    }
}

void msc_service(void) {
    if (!g_msc) return;
    u64 now = uptime_ms();
    for (Msc *m = g_msc; m; m = m->next) {
        if (m->gone || m->dead || !m->dev) continue;
        for (int i = 0; i < m->nlun; i++) {
            MscLun *l = &m->lun[i];
            if (l->state != LS_START && l->state != LS_WAIT && l->state != LS_NOMEDIA) continue;
            if (now < l->next_at) continue;
            lun_step(m, l);
            recount();
            return;                                                             /* one command per round: hubs and keyboards come first */
        }
    }
}

/* ---------------------------------------------------------------- the class driver */

static void cfg_load(void);

static bool msc_match(UsbDevice *dev, UsbInterface *intf) {
    if (intf->cls != 8) return false;
    cfg_load();
    if (g_off || usb_bulktest_enabled()) return false;
    if (intf->proto != 0x50 || intf->subcls != 6) {
        klog("Kappa", "usb %s: mass storage interface %u speaks subclass %02x / protocol %02x (UAS, CBI, ATAPI...): only SCSI over the bulk-only transport is supported -- ignored",
             dev->path, intf->number, intf->subcls, intf->proto);
        return false;
    }
    bool in = false, out = false;
    for (int e = 0; e < intf->nep; e++) {
        if (intf->ep[e].type != USB_EP_BULK) continue;
        if (intf->ep[e].addr & USB_DIR_IN) in = true; else out = true;
    }
    if (!in || !out) {
        klog("Kappa", "usb %s: mass storage interface %u lacks a bulk-in or bulk-out endpoint -- ignored", dev->path, intf->number);
        return false;
    }
    return true;
}

static int msc_probe(UsbDevice *dev, UsbInterface *intf) {
    UsbEndpoint *in = NULL, *out = NULL;
    for (int e = 0; e < intf->nep; e++) {
        UsbEndpoint *ep = &intf->ep[e];
        if (ep->type != USB_EP_BULK) continue;
        if ((ep->addr & USB_DIR_IN) && !in) in = ep;
        else if (!(ep->addr & USB_DIR_IN) && !out) out = ep;
    }
    if (!in || !out || !in->configured || !out->configured) return -ENODEV;

    u8 maxlun = 0;
    int rc = usb_control(dev, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_IFACE, 0xFE, 0, intf->number, &maxlun, 1, T_CTRL);
    if (rc != 1) {
        if (dev->broken || rc == -ENODEV || rc == -ETIMEDOUT) return -ENODEV;
        maxlun = 0;                                                             /* a stall means "one LUN" (the specification allows it) */
    }
    unsigned nlun = (unsigned)maxlun + 1;
    if (maxlun > 15) {                                                          /* more than the protocol can address: the stick lies */
        klog("Kappa", "usb-storage %s: GET MAX LUN answered %u (the limit is 15) -- taken to mean one LUN", dev->path, maxlun);
        nlun = 1;
    }
    if (nlun > MSC_MAX_LUN) {
        klog("Kappa", "usb-storage %s: %u LUNs, only the first %d are used", dev->path, nlun, MSC_MAX_LUN);
        nlun = MSC_MAX_LUN;
    }
    Msc *m = kzalloc(sizeof *m + nlun * sizeof(MscLun));
    if (!m) return -ENOMEM;
    m->dev = dev;
    m->intf = intf;
    m->in = in;
    m->out = out;
    m->nlun = (u8)nlun;
    m->vid = dev->desc.idVendor;
    m->pid = dev->desc.idProduct;
    strlcpy(m->path, dev->path, sizeof m->path);
    u64 now = uptime_ms();
    for (unsigned i = 0; i < nlun; i++) {
        m->lun[i].msc = m;
        m->lun[i].lun = (u8)i;
        m->lun[i].state = LS_START;
        m->lun[i].started_at = now;
        m->lun[i].next_at = now + i * 20;
    }
    m->next = NULL;                                                             /* oldest first: sticks are brought up (and named) in the order they were found */
    Msc **tail = &g_msc;
    while (*tail) tail = &(*tail)->next;
    *tail = m;
    intf->drv_data = m;
    recount();
    klog("Kappa", "usb-storage %s: %04x:%04x, bulk-only transport, %u LUN%s -- the kappa are sizing the stick up", dev->path, m->vid, m->pid, nlun, nlun == 1 ? "" : "s");
    return 0;
}

static void msc_disconnect(UsbDevice *dev, UsbInterface *intf) {
    Msc *m = intf->drv_data;
    if (!m) return;
    intf->drv_data = NULL;
    m->gone = true;                                                             /* queued requests see this and fail with -ENODEV */
    m->dev = NULL;
    bool had_disk = false;
    for (int i = 0; i < m->nlun; i++) {
        MscLun *l = &m->lun[i];
        if (l->bd) had_disk = true;
        if (l->state != LS_GONE) lun_drop(l, LS_GONE, "unplugged");
    }
    recount();
    if (had_disk) klog("Kappa", "usb-storage %s: the stick was pulled out -- whatever was not written to it is lost, and its disks are gone from /dev", m->path);
    else usb_dbg("usb %s: mass storage device gone", m->path);
    if (!m->ever_online) {                                                      /* no BlockDev points into it: safe to free */
        for (Msc **pp = &g_msc; *pp; pp = &(*pp)->next)
            if (*pp == m) { *pp = m->next; break; }
        kfree(m);
    }
}

static const char *msc_label(UsbInterface *intf) { return "usb-storage"; }

const UsbDriver usb_storage_driver = { "usb-storage", msc_match, msc_probe, msc_disconnect, msc_label };

/* ---------------------------------------------------------------- /proc/usbstorage */

size_t msc_describe(char *buf, size_t cap) {
    size_t n = 0;
    u64 f = irq_save();                                                         /* one CPU: usbd cannot run while this walks the list */
    if (!g_msc) n += (size_t)snprintf(buf + n, cap - n, "no USB mass storage devices\n");
    for (Msc *m = g_msc; m && n + 300 < cap; m = m->next) {
        n += (size_t)snprintf(buf + n, cap - n, "usb-storage %s: %04x:%04x bulk-only, %u LUN%s, %u reset recover%s%s\n", m->path, m->vid, m->pid, m->nlun,
                              m->nlun == 1 ? "" : "s", m->resets, m->resets == 1 ? "y" : "ies", m->gone ? ", unplugged" : m->dead ? ", given up on" : "");
        for (int i = 0; i < m->nlun && n + 220 < cap; i++) {
            MscLun *l = &m->lun[i];
            n += (size_t)snprintf(buf + n, cap - n, "  lun %u: %s%s%s%s \"%s\"", l->lun, g_state_name[l->state], l->why[0] && l->state != LS_ONLINE ? " (" : "",
                                  l->state != LS_ONLINE ? l->why : "", l->why[0] && l->state != LS_ONLINE ? ")" : "", l->model);
            if (l->bd) n += (size_t)snprintf(buf + n, cap - n, " %s %lu x %u %s", l->bd->name, (unsigned long)l->sectors, l->bsz, l->wp ? "ro" : "rw");
            n += (size_t)snprintf(buf + n, cap - n, "  reads %u writes %u syncs %u errors %u\n", l->reads, l->writes, l->syncs, l->errors);
        }
    }
    irq_restore(f);
    return n < cap ? n : cap;
}

/* ---------------------------------------------------------------- configuration and self test */

#define EXPECT(cond) do { checks++; if (!(cond)) { bad++; klog("Kappa", "usb-storage selftest: %s failed (line %d)", #cond, __LINE__); } } while (0)

static void selftest(void) {
    int checks = 0, bad = 0;
    u64 sectors = 0;
    /* capacity: zero block size, odd sizes, overflow, absurd sizes, the good cases */
    EXPECT(cap_check(1000, 0, &sectors));
    EXPECT(cap_check(1000, 520, &sectors));
    EXPECT(cap_check(1000, 1024, &sectors));
    EXPECT(cap_check(1000, 2048, &sectors));
    EXPECT(cap_check(1000, 3000, &sectors));
    EXPECT(cap_check(~0ull, 512, &sectors));
    EXPECT(cap_check(1ull << 60, 512, &sectors));
    EXPECT(cap_check((MSC_MAX_BYTES / 512), 512, &sectors));
    EXPECT(!cap_check(131071, 512, &sectors) && sectors == 131072);
    EXPECT(!cap_check(255, 4096, &sectors) && sectors == 256);
    EXPECT(!cap_check(0, 512, &sectors) && sectors == 1);
    /* status wrapper: signature, tag, status byte, phase error, residue */
    u8 csw[13] = { 'U', 'S', 'B', 'S', 7, 0, 0, 0, 0, 0, 0, 0, 0 };
    EXPECT(!csw_check(csw, 7, 512));
    EXPECT(csw_check(csw, 8, 512));
    csw[0] = 'X';
    EXPECT(csw_check(csw, 7, 512));
    csw[0] = 'U';
    csw[12] = 1;
    EXPECT(!csw_check(csw, 7, 512));
    csw[12] = 2;
    EXPECT(csw_check(csw, 7, 512));
    csw[12] = 77;
    EXPECT(csw_check(csw, 7, 512));
    csw[12] = 0;
    put_le32(csw + 8, 513);
    EXPECT(csw_check(csw, 7, 512));
    put_le32(csw + 8, 512);
    EXPECT(!csw_check(csw, 7, 512));
    /* sense data: both formats, truncated, nonsense */
    Sense s;
    u8 fixed[18] = { 0x70, 0, 0x02, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0x3A, 0x00 };
    sense_parse(fixed, 18, &s);
    EXPECT(s.valid && s.key == 2 && s.asc == 0x3A && sense_action(&s) == SA_NOMEDIA);
    u8 desc[8] = { 0x72, 0x06, 0x28, 0x00 };
    sense_parse(desc, 8, &s);
    EXPECT(s.valid && s.key == 6 && s.asc == 0x28 && sense_action(&s) == SA_CHANGED);
    sense_parse(fixed, 5, &s);
    EXPECT(!s.valid && sense_action(&s) == SA_FATAL);
    u8 junk[18] = { 0x55, 0xFF, 0xFF, 0xFF };
    sense_parse(junk, 18, &s);
    EXPECT(!s.valid && sense_action(&s) == SA_FATAL);
    u8 wp[18] = { 0x70, 0, 0x07, 0, 0, 0, 0, 10 };
    sense_parse(wp, 18, &s);
    EXPECT(sense_action(&s) == SA_PROTECT);
    u8 becoming[18] = { 0x70, 0, 0x02, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0x04, 0x01 };
    sense_parse(becoming, 18, &s);
    EXPECT(sense_action(&s) == SA_RETRY);
    /* INQUIRY: a disk, a CD-ROM, an absent unit, a runt */
    MscLun l;
    memset(&l, 0, sizeof l);
    u8 inq[36] = { 0x00, 0x80, 0x05, 0x02, 31 };
    memcpy(inq + 8, "TOUHOU  ", 8);
    memcpy(inq + 16, "GAP DISK        ", 16);
    EXPECT(inquiry_parse(inq, 36, &l) == INQ_OK && l.removable && !strcmp(l.model, "TOUHOU GAP DISK"));
    inq[0] = 0x05;
    EXPECT(inquiry_parse(inq, 36, &l) == INQ_UNSUPPORTED);
    inq[0] = 0x20;
    EXPECT(inquiry_parse(inq, 36, &l) == INQ_ABSENT);
    inq[0] = 0x00;
    EXPECT(inquiry_parse(inq, 3, &l) < 0);
    klog("Kappa", "usb-storage selftest: %d checks, %d failed", checks, bad);
}

static void cfg_load(void) {
    if (g_cfg_loaded) return;
    g_cfg_loaded = true;
    char v[24];
    if (cmdline_value("usbstorage", v, sizeof v) && (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "no"))) {
        g_off = true;
        klog("Kappa", "usb-storage: disabled on the kernel command line (usbstorage=off)");
    }
    g_nosync = cmdline_flag("usbstorage.nosync");
    g_rw16 = cmdline_flag("usbstorage.rw16");
    g_stalltest = cmdline_flag("usbstorage.stalltest");
    long t = cmdline_int("usbstorage.timeout", 0);
    if (t >= 100 && t <= 120000) g_t_cbw = g_t_quick = g_t_csw = g_t_read = g_t_write = g_t_sync = (u32)t;
    if (cmdline_value("usbstorage.fault", v, sizeof v)) {
        const char *colon = strchr(v, ':');
        u32 n = 0;
        for (const char *q = colon ? colon + 1 : ""; *q >= '0' && *q <= '9'; q++) n = n * 10 + (u32)(*q - '0');
        if (!strncmp(v, "csw:", 4)) g_fault_csw = n;
        else if (!strncmp(v, "drop:", 5)) g_fault_drop = n;
        else if (!strncmp(v, "notready:", 9)) g_fault_notready = n;
    }
    if (cmdline_flag("usbstorage.selftest")) selftest();
}
