/*
 * USB core: the usbd thread, port events, enumeration (descriptors, addressing, configuration, driver binding), device
 * teardown, /proc/usb. Host controller specifics live in xhci.c; class specifics in hub.c and hid.c.
 *
 * The enumeration sequence is the one Linux uses ("new scheme"): reset the port, Address Device with BSR (no SET_ADDRESS yet),
 * read the first 8 bytes of the device descriptor to learn bMaxPacketSize0, fix the control endpoint size if the guess was
 * wrong, Address Device again (now the controller sends SET_ADDRESS), wait 10 ms, read everything else. A device that refuses
 * that is retried with the old order (SET_ADDRESS first), and the whole thing is attempted four times with a fresh reset.
 */
#include "usb.h"
#include "kernel.h"
#include "cmdline.h"
#include "task.h"
#include "mm.h"
#include "cpu.h"
#include "irq.h"
#include "vfs.h"
#include "blk.h"
#include "abi.h"

#define CTRL_TIMEOUT      2500
#define SET_TIMEOUT       3000
#define PORT_INIT_TRIES   4
#define MAX_BUSES         4
#define PROC_SIZE         12288

bool g_usb_debug;
bool g_usb_hidreport;
bool g_usb_stalltest;
static bool g_usb_bulktest;
static UsbHc *g_hcs[MAX_BUSES];
static int g_nhc;
static UsbHub *g_hubs;
static const char *g_step = "?";

extern const UsbDriver usb_hub_driver;
extern const UsbDriver usb_hid_driver;
static const UsbDriver *const g_drivers[] = { &usb_hub_driver, &usb_hid_driver, &usb_storage_driver };

bool usb_bulktest_enabled(void) { return g_usb_bulktest; }

/* Boot rendezvous state (see usb_wait_settled). Written by the usbd thread, read by whoever waits. */
static volatile bool g_usbd_started;        /* every controller's start() has returned (or failed) */
static volatile bool g_usb_expect;          /* something was plugged in when the controllers came up */
static volatile int  g_usb_busy;            /* usbd is handling port changes or jobs right now */
static volatile u64  g_usb_last_active;     /* uptime_ms() of the last round in which usbd had anything to do */
#define SETTLE_QUIET_MS 150                /* how long the bus has to stay idle before it counts as settled */

/* ---------------------------------------------------------------- small helpers */

const char *usb_speed_name(int speed) {
    switch (speed) {
    case USB_SPEED_LOW: return "low-speed";
    case USB_SPEED_FULL: return "full-speed";
    case USB_SPEED_HIGH: return "high-speed";
    case USB_SPEED_SUPER: return "super-speed";
    case USB_SPEED_SUPER_PLUS: return "super-speed+";
    default: return "unknown-speed";
    }
}

void usb_msleep(UsbHc *hc, u32 ms) {
    for (u32 i = 0; i < ms && !hc->dead; i++) {
        hc->ops->poll(hc);
        task_sleep_ms(1);
    }
}

void usb_hub_register(UsbHub *hub) {
    hub->next = g_hubs;
    g_hubs = hub;
}

void usb_hub_unregister(UsbHub *hub) {
    for (UsbHub **pp = &g_hubs; *pp; pp = &(*pp)->next)
        if (*pp == hub) { *pp = hub->next; break; }
    hub->next = NULL;
}

void usb_hub_mark_port(UsbHub *hub, int port) {
    if (port < 0 || port > hub->nports || port > 255) return;
    hub->pending[port >> 3] |= (u8)(1u << (port & 7));
    hub->any_pending = true;
}

/* ---------------------------------------------------------------- control transfers */

int usb_control(UsbDevice *dev, u8 rtype, u8 req, u16 value, u16 index, void *buf, u16 len, u32 timeout_ms) {
    if (dev->dead || dev->broken || dev->hc->dead || !dev->hcpriv) return -ENODEV;
    UsbSetup s = { rtype, req, value, index, len };
    int rc = dev->hc->ops->control(dev->hc, dev, &s, buf, timeout_ms);
    if (rc == -ETIMEDOUT) dev->broken = true;           /* the endpoint was recovered, but this device does not answer */
    return rc;
}

int usb_get_descriptor(UsbDevice *dev, u8 type, u8 index, u16 lang, void *buf, u16 len) {
    return usb_control(dev, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, (u16)((type << 8) | index), lang, buf, len, CTRL_TIMEOUT);
}

int usb_int_start(UsbDevice *dev, UsbEndpoint *ep, UsbIntCb cb, void *ctx) {
    ep->cb = cb;
    ep->cb_ctx = ctx;
    return dev->hc->ops->int_start(dev->hc, dev, ep);
}

int usb_add_endpoints(UsbDevice *dev, UsbEndpoint **eps, int n) {
    if (dev->dead || dev->broken || dev->hc->dead || !dev->hcpriv) return -ENODEV;
    return dev->hc->ops->add_endpoints(dev->hc, dev, eps, n);
}

int usb_bulk(UsbDevice *dev, UsbEndpoint *ep, void *buf, u32 len, u32 timeout_ms) {
    if (dev->dead || dev->broken || dev->hc->dead || !dev->hcpriv) return -ENODEV;
    int rc = dev->hc->ops->bulk(dev->hc, dev, ep, buf, len, timeout_ms);
    if (rc == -ETIMEDOUT) dev->broken = true;
    return rc;
}

/* CLEAR_FEATURE(ENDPOINT_HALT) on the device; the controller side was reset when the stall was reported. */
int usb_clear_halt(UsbDevice *dev, UsbEndpoint *ep) {
    return usb_control(dev, USB_RECIP_EP, USB_REQ_CLEAR_FEATURE, 0, ep->addr, NULL, 0, CTRL_TIMEOUT);
}

/* usb=bulktest: SCSI INQUIRY, READ CAPACITY and a 64 KiB READ(10) over the bulk-only transport of a mass storage device. Reads only;
 * the point is to prove the bulk path (rings, 64 KiB TDs, residuals) on real controllers. The kernel does not register a disk. */
static int msc_command(UsbDevice *dev, UsbEndpoint *out, UsbEndpoint *in, const u8 *cdb, int cdb_len, void *data, u32 dlen) {
    static u32 tag = 0x5000;
    u8 cbw[31] = { 'U', 'S', 'B', 'C' };
    u32 t = ++tag;
    memcpy(cbw + 4, &t, 4);
    memcpy(cbw + 8, &dlen, 4);
    cbw[12] = 0x80;                                     /* data in */
    cbw[14] = (u8)cdb_len;
    memcpy(cbw + 15, cdb, (size_t)cdb_len);
    int rc = usb_bulk(dev, out, cbw, sizeof cbw, 3000);
    if (rc != (int)sizeof cbw) return rc < 0 ? rc : -EIO;
    rc = usb_bulk(dev, in, data, dlen, 5000);
    if (rc < 0) {
        if (rc != -EPIPE) return rc;
        usb_clear_halt(dev, in);
    }
    int got = rc;
    u8 csw[13];
    rc = usb_bulk(dev, in, csw, sizeof csw, 3000);
    if (rc == -EPIPE) { usb_clear_halt(dev, in); rc = usb_bulk(dev, in, csw, sizeof csw, 3000); }
    if (rc != (int)sizeof csw) return rc < 0 ? rc : -EIO;
    if (memcmp(csw, "USBS", 4) || memcmp(csw + 4, &t, 4) || csw[12] != 0) return -EIO;
    return got;
}

static void bulk_selftest(UsbDevice *dev) {
    UsbInterface *it = NULL;
    for (int i = 0; i < dev->nintf; i++)
        if (dev->intf[i].cls == 8 && dev->intf[i].subcls == 6 && dev->intf[i].proto == 0x50) it = &dev->intf[i];
    if (!it) return;
    UsbEndpoint *in = NULL, *out = NULL, *eps[2];
    for (int e = 0; e < it->nep; e++) {
        if (it->ep[e].type != USB_EP_BULK) continue;
        if (it->ep[e].addr & USB_DIR_IN) in = &it->ep[e]; else out = &it->ep[e];
    }
    if (!in || !out) return;
    eps[0] = out;
    eps[1] = in;
    int rc = usb_add_endpoints(dev, eps, 2);
    if (!rc) rc = usb_control(dev, 0, USB_REQ_SET_CONFIGURATION, dev->cfg_value, 0, NULL, 0, SET_TIMEOUT);
    if (rc < 0) { klog("Kappa", "usb %s: bulk test: cannot configure the endpoints (%d)", dev->path, rc); return; }

    u8 inq[6] = { 0x12, 0, 0, 0, 36, 0 }, cap[10] = { 0x25 }, rd[10] = { 0x28, 0, 0, 0, 0, 1, 0, 0, 128, 0 };   /* READ(10): LBA 1, 128 blocks */
    u8 buf[36];
    rc = msc_command(dev, out, in, inq, 6, buf, sizeof buf);
    if (rc < 36) { klog("Kappa", "usb %s: bulk test: INQUIRY failed (%d)", dev->path, rc); return; }
    klog("Kappa", "usb %s: bulk test: INQUIRY -> '%.8s' '%.16s'", dev->path, (const char *)buf + 8, (const char *)buf + 16);
    u8 capbuf[8];
    rc = msc_command(dev, out, in, cap, 10, capbuf, sizeof capbuf);
    if (rc < 8) { klog("Kappa", "usb %s: bulk test: READ CAPACITY failed (%d)", dev->path, rc); return; }
    u32 last = (u32)capbuf[0] << 24 | (u32)capbuf[1] << 16 | (u32)capbuf[2] << 8 | capbuf[3];
    u32 bsz = (u32)capbuf[4] << 24 | (u32)capbuf[5] << 16 | (u32)capbuf[6] << 8 | capbuf[7];
    klog("Kappa", "usb %s: bulk test: last block %u, block size %u", dev->path, last, bsz);
    if (bsz != 512 || last < 130) return;
    u8 *big = kmalloc(65536);
    if (!big) return;
    rc = msc_command(dev, out, in, rd, 10, big, 65536);
    if (rc == 65536) {
        u32 sum = 0;
        for (u32 i = 0; i < 65536; i++) sum += big[i];
        klog("Kappa", "usb %s: bulk test: READ(10) of 64 KiB ok, byte sum %u", dev->path, sum);
    } else {
        klog("Kappa", "usb %s: bulk test: READ(10) failed (%d)", dev->path, rc);
    }
    kfree(big);
}

/* The self test is started from a separate kernel thread and handed to the usbd thread with usb_call(), the way a block device
 * driver would do its I/O. */
static int bulk_job(void *dev) { bulk_selftest(dev); return 0; }
static void bulk_thread(void *dev) { usb_call(bulk_job, dev); }

/* UTF-16LE string descriptor -> printable ASCII. */
static int fetch_string(UsbDevice *dev, u8 idx, u16 lang, char *out, size_t cap) {
    u8 buf[128];
    out[0] = 0;
    if (!idx) return 0;
    int rc = usb_get_descriptor(dev, USB_DT_STRING, idx, lang, buf, sizeof buf - 2);
    if (rc < 2 || buf[1] != USB_DT_STRING) return rc < 0 ? rc : -EIO;
    int n = buf[0] < rc ? buf[0] : rc;
    size_t o = 0;
    for (int i = 2; i + 1 < n && o + 1 < cap; i += 2) {
        u16 c = (u16)(buf[i] | (buf[i + 1] << 8));
        out[o++] = (c >= 32 && c < 127) ? (char)c : (c ? '?' : ' ');
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
    return (int)o;
}

static void fetch_strings(UsbDevice *dev) {
    u8 buf[8];
    u16 lang = 0x0409;
    if (!dev->desc.iManufacturer && !dev->desc.iProduct && !dev->desc.iSerialNumber) return;
    int rc = usb_get_descriptor(dev, USB_DT_STRING, 0, 0, buf, 4);
    if (rc >= 4 && buf[1] == USB_DT_STRING) lang = (u16)(buf[2] | (buf[3] << 8));
    else if (rc < 0) return;                                       /* no string support (or a dead device): do not push our luck */
    fetch_string(dev, dev->desc.iManufacturer, lang, dev->manufacturer, sizeof dev->manufacturer);
    fetch_string(dev, dev->desc.iProduct, lang, dev->product, sizeof dev->product);
    fetch_string(dev, dev->desc.iSerialNumber, lang, dev->serial, sizeof dev->serial);
}

/* ---------------------------------------------------------------- /proc/usb */

static char g_proc[PROC_SIZE];
static size_t g_proc_len;

static size_t gen_usb(char *buf, size_t cap) {
    u64 f = irq_save();
    size_t n = g_proc_len < cap ? g_proc_len : cap;
    memcpy(buf, g_proc, n);
    irq_restore(f);
    return n;
}

typedef struct { char *b; size_t cap, n; } Sink;

static void put(Sink *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(Sink *s, const char *fmt, ...) {
    if (s->n >= s->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(s->b + s->n, s->cap - s->n, fmt, ap);
    va_end(ap);
    if (w > 0) s->n += (size_t)w < s->cap - s->n ? (size_t)w : s->cap - s->n - 1;
}

static void dump_drivers(UsbDevice *dev, char *out, size_t cap) {
    out[0] = 0;
    for (int i = 0; i < dev->nintf; i++) {
        UsbInterface *it = &dev->intf[i];
        if (!it->driver) continue;
        const char *name = it->driver->label ? it->driver->label(it) : it->driver->name;
        if (strstr(out, name)) continue;
        if (out[0]) strlcat(out, ",", cap);
        strlcat(out, name, cap);
    }
    if (!out[0]) strlcpy(out, "none", cap);
}

static void dump_device(Sink *s, UsbDevice *dev) {
    char drv[64], name[140];
    dump_drivers(dev, drv, sizeof drv);
    snprintf(name, sizeof name, "%s%s%s", dev->manufacturer, dev->manufacturer[0] && dev->product[0] ? " " : "", dev->product);
    if (!name[0]) strlcpy(name, "(unnamed device)", sizeof name);
    const char *pp = strchr(dev->path, '-');
    put(s, "Bus %d Port %s: ID %04x:%04x %s, %s, class %02x, driver %s\n", dev->hc->busnum, pp ? pp + 1 : dev->path, dev->desc.idVendor,
        dev->desc.idProduct, name, usb_speed_name(dev->speed), dev->desc.bDeviceClass, drv);
    for (int i = 0; i < dev->nintf; i++) {
        UsbInterface *it = &dev->intf[i];
        const char *lab = it->driver ? (it->driver->label ? it->driver->label(it) : it->driver->name) : "none";
        put(s, "    If %u: class %02x/%02x/%02x, %u endpoint%s, driver %s\n", it->number, it->cls, it->subcls, it->proto, it->nep,
            it->nep == 1 ? "" : "s", lab);
    }
    if (dev->hub)
        for (int p = 0; p < dev->hub->nports; p++)
            if (dev->hub->port[p].dev) dump_device(s, dev->hub->port[p].dev);
}

static void proc_update(void) {
    char *tmp = kmalloc(PROC_SIZE);
    if (!tmp) return;
    Sink s = { tmp, PROC_SIZE, 0 };
    for (int i = 0; i < g_nhc; i++) {
        UsbHc *hc = g_hcs[i];
        put(&s, "Bus %d: %s%s%s\n", hc->busnum, hc->desc[0] ? hc->desc : "xHCI controller",
            hc->failed ? " -- failed: " : hc->dead ? " -- dead: " : "", hc->failed || hc->dead ? hc->status : "");
        if (hc->failed || !hc->root.port) continue;
        for (int p = 0; p < hc->root.nports; p++)
            if (hc->root.port[p].dev) dump_device(&s, hc->root.port[p].dev);
    }
    u64 f = irq_save();
    memcpy(g_proc, tmp, s.n);
    g_proc_len = s.n;
    irq_restore(f);
    kfree(tmp);
}

static size_t gen_usbstat(char *buf, size_t cap) { return hid_describe_stats(buf, cap); }

/* ---------------------------------------------------------------- descriptors */

static u16 default_mps0(u8 speed) {
    switch (speed) {
    case USB_SPEED_LOW: return 8;
    case USB_SPEED_FULL: case USB_SPEED_HIGH: return 64;           /* Linux guesses 64 for full speed too and corrects it later */
    default: return 512;
    }
}

static u16 mps0_from_descriptor(u8 speed, u8 b) {
    if (speed >= USB_SPEED_SUPER) return b == 9 ? 512 : 0;
    if (speed == USB_SPEED_LOW) return b == 8 ? 8 : 0;
    if (speed == USB_SPEED_HIGH) return b == 64 ? 64 : 0;
    return (b == 8 || b == 16 || b == 32 || b == 64) ? b : 0;
}

static void parse_config(UsbDevice *dev) {
    const u8 *c = dev->cfg;
    u16 total = dev->cfg_len;
    UsbInterface *cur = NULL;
    UsbEndpoint *last_ep = NULL;
    dev->nintf = 0;
    for (u32 off = c[0]; off + 2 <= total;) {
        u8 len = c[off], type = c[off + 1];
        if (len < 2 || off + len > total) break;
        switch (type) {
        case USB_DT_INTERFACE:
            last_ep = NULL;
            cur = NULL;
            if (len >= 9 && c[off + 3] == 0 && dev->nintf < USB_MAX_INTERFACES) {          /* alternate setting 0 only */
                cur = &dev->intf[dev->nintf++];
                memset(cur, 0, sizeof *cur);
                cur->number = c[off + 2];
                cur->cls = c[off + 5];
                cur->subcls = c[off + 6];
                cur->proto = c[off + 7];
            }
            break;
        case USB_DT_ENDPOINT:
            last_ep = NULL;
            if (cur && len >= 7 && cur->nep < USB_MAX_EPS) {
                UsbEndpoint *e = &cur->ep[cur->nep];
                u16 raw = (u16)(c[off + 4] | (c[off + 5] << 8));
                e->addr = c[off + 2];
                e->type = c[off + 3] & 3;
                e->mps = raw & 0x7FF;
                e->extra_tx = (raw >> 11) & 3;
                e->interval = c[off + 6];
                e->dci = (u8)((e->addr & 0xF) * 2 + ((e->addr & USB_DIR_IN) ? 1 : 0));
                if ((e->addr & 0xF) != 0) { cur->nep++; last_ep = e; }
            }
            break;
        case USB_DT_SS_EP_COMP:
            if (last_ep && len >= 6) {
                last_ep->burst = c[off + 2];
                last_ep->comp_mult = c[off + 3] & 3;
                last_ep->bytes_per_interval = (u16)(c[off + 4] | (c[off + 5] << 8));
            }
            break;
        default:
            if (cur && cur->nep == 0) {                                                      /* class descriptors before the first endpoint */
                if (!cur->extra) cur->extra = &c[off];
                cur->extra_len = (u16)(cur->extra_len + len);
            }
            break;
        }
        off += len;
    }
}

/* ---------------------------------------------------------------- device lifecycle */

static void link_device(UsbDevice *dev) {
    dev->next = dev->hc->devices;
    dev->hc->devices = dev;
}

static void unlink_device(UsbDevice *dev) {
    for (UsbDevice **pp = &dev->hc->devices; *pp; pp = &(*pp)->next)
        if (*pp == dev) { *pp = dev->next; break; }
}

void usb_disconnect(UsbDevice *dev) {
    if (!dev) return;
    UsbHc *hc = dev->hc;
    dev->dead = true;
    for (int i = 0; i < dev->nintf; i++) {                  /* hubs take their children down here, before the hub's own slot goes */
        UsbInterface *it = &dev->intf[i];
        if (it->driver && it->driver->disconnect) it->driver->disconnect(dev, it);
        it->driver = NULL;
    }
    hc->ops->free_dev(hc, dev);
    if (dev->parent && dev->parent->port && dev->port >= 1 && dev->port <= dev->parent->nports)
        dev->parent->port[dev->port - 1].dev = NULL;
    unlink_device(dev);
    klog("Kappa", "usb %s: device disconnected", dev->path);
    kfree(dev->cfg);
    kfree(dev);
    proc_update();
}

static UsbDevice *dev_create(UsbHub *hub, int port, const UsbPortStatus *st) {
    UsbDevice *dev = kzalloc(sizeof *dev);
    if (!dev) return NULL;
    dev->hc = hub->hc;
    dev->parent = hub;
    dev->port = (u8)port;
    dev->speed = st->speed;
    dev->speed_id = st->speed_id;
    if (hub->dev) {
        UsbDevice *h = hub->dev;
        dev->depth = (u8)(h->depth + 1);
        dev->root_port = h->root_port;
        dev->route = h->route | ((u32)(port > 15 ? 15 : port) << (4 * h->depth));
        if (h->tt_hub) { dev->tt_hub = h->tt_hub; dev->tt_port = h->tt_port; }              /* inherit the translator above us */
        else if (dev->speed != USB_SPEED_HIGH && h->speed == USB_SPEED_HIGH) { dev->tt_hub = h; dev->tt_port = (u8)port; }
        snprintf(dev->path, sizeof dev->path, "%s.%d", h->path, port);
    } else {
        dev->root_port = (u8)port;
        snprintf(dev->path, sizeof dev->path, "%d-%d", hub->hc->busnum, port);
    }
    return dev;
}

/* Address Device (+ descriptor reads) with Linux's sequence; see the file comment. Returns 0 or -errno; g_step says where it died. */
static int dev_address(UsbDevice *dev, int attempt) {
    UsbHc *hc = dev->hc;
    u8 raw[8];
    int rc;
    bool bsr = attempt < 2;                                 /* tries 0 and 1: new scheme; later tries: old scheme */
    dev->mps0 = default_mps0(dev->speed);

    g_step = bsr ? "address device (BSR)" : "address device";
    rc = hc->ops->address_dev(hc, dev, bsr);
    if (rc) return rc;
    if (!bsr) usb_msleep(hc, 10);

    g_step = "device descriptor (8 bytes)";
    rc = -EIO;
    for (int i = 0; i < 3 && rc < 8; i++) {                 /* devices often NAK or stall the first requests after a reset */
        if (i) usb_msleep(hc, 50);
        rc = usb_get_descriptor(dev, USB_DT_DEVICE, 0, 0, raw, 8);
        if (rc == -ENODEV || dev->broken) break;
    }
    if (rc < 8) return rc < 0 ? rc : -EIO;
    u16 mps = mps0_from_descriptor(dev->speed, raw[7]);
    if (!mps) { klog("Kappa", "usb %s: bad bMaxPacketSize0 %u for a %s device", dev->path, raw[7], usb_speed_name(dev->speed)); return -EIO; }
    if (mps != dev->mps0) {
        usb_dbg("usb %s: control endpoint packet size is %u, not the %u we assumed: updating the endpoint context", dev->path, mps, dev->mps0);
        g_step = "control endpoint size";
        rc = hc->ops->set_ep0_size(hc, dev, mps);
        if (rc) return rc;
    }
    if (bsr) {
        g_step = "address device";
        rc = hc->ops->address_dev(hc, dev, false);
        if (rc) return rc;
        usb_msleep(hc, 10);                                 /* SET_ADDRESS recovery time */
    }

    g_step = "device descriptor";
    rc = -EIO;
    for (int i = 0; i < 3 && rc < 18; i++) {
        if (i) usb_msleep(hc, 50);
        rc = usb_get_descriptor(dev, USB_DT_DEVICE, 0, 0, &dev->desc, 18);
        if (rc == -ENODEV || dev->broken) break;
    }
    if (rc < 18 || dev->desc.bDescriptorType != USB_DT_DEVICE || dev->desc.bNumConfigurations == 0) return rc < 0 ? rc : -EIO;
    return 0;
}

static int dev_read_config(UsbDevice *dev) {
    u8 hdr[9];
    int rc = -EIO;
    g_step = "configuration descriptor";
    for (int i = 0; i < 3 && rc < 9; i++) {
        if (i) usb_msleep(dev->hc, 50);
        rc = usb_get_descriptor(dev, USB_DT_CONFIG, 0, 0, hdr, 9);
        if (rc == -ENODEV || dev->broken) break;
    }
    if (rc < 9 || hdr[1] != USB_DT_CONFIG) return rc < 0 ? rc : -EIO;
    u16 total = (u16)(hdr[2] | (hdr[3] << 8));
    if (total < 9) return -EIO;
    if (total > 4096) total = 4096;
    dev->cfg = kmalloc(total);
    if (!dev->cfg) return -ENOMEM;
    rc = usb_get_descriptor(dev, USB_DT_CONFIG, 0, 0, dev->cfg, total);
    if (rc < 9) return rc < 0 ? rc : -EIO;
    dev->cfg_len = (u16)rc;
    dev->cfg_value = dev->cfg[5];
    parse_config(dev);
    return 0;
}

/* Binds drivers, configures endpoints, SET_CONFIGURATION, probes. */
static int dev_configure(UsbDevice *dev) {
    UsbHc *hc = dev->hc;
    UsbEndpoint *eps[30];
    int neps = 0, nbound = 0;
    for (int i = 0; i < dev->nintf; i++) {
        UsbInterface *it = &dev->intf[i];
        for (unsigned d = 0; d < ARRAY_SIZE(g_drivers); d++) {
            if (!g_drivers[d]->match(dev, it)) continue;
            it->driver = g_drivers[d];
            nbound++;
            for (int e = 0; e < it->nep; e++) {             /* what a class driver can use: interrupt-IN (hub, HID) and bulk (storage) */
                bool usable = (it->ep[e].type == USB_EP_INT && (it->ep[e].addr & USB_DIR_IN)) || it->ep[e].type == USB_EP_BULK;
                if (usable && neps < 30) eps[neps++] = &it->ep[e];
            }
            break;
        }
    }
    if (!nbound) return 0;                                  /* nothing we drive: leave the device unconfigured (100 mA) */

    if (neps) {
        g_step = "configure endpoints";
        int rc = hc->ops->add_endpoints(hc, dev, eps, neps);
        if (rc) { for (int i = 0; i < dev->nintf; i++) dev->intf[i].driver = NULL; return rc; }
    }
    g_step = "set configuration";
    int rc = usb_control(dev, 0, USB_REQ_SET_CONFIGURATION, dev->cfg_value, 0, NULL, 0, SET_TIMEOUT);
    if (rc < 0) { for (int i = 0; i < dev->nintf; i++) dev->intf[i].driver = NULL; return rc; }
    dev->configured = true;

    for (int i = 0; i < dev->nintf; i++) {
        UsbInterface *it = &dev->intf[i];
        if (!it->driver) continue;
        g_step = it->driver->name;
        int prc = it->driver->probe(dev, it);
        if (prc) {
            usb_dbg("usb %s: driver %s declined interface %u (%d)", dev->path, it->driver->name, it->number, prc);
            it->driver = NULL;
        }
    }
    return 0;
}

static UsbDevice *usb_setup_device(UsbHub *hub, int port, const UsbPortStatus *st, int attempt) {
    UsbHc *hc = hub->hc;
    UsbDevice *dev = dev_create(hub, port, st);
    if (!dev) return NULL;
    link_device(dev);
    g_step = "enable slot";
    int rc = hc->ops->alloc_dev(hc, dev);
    if (rc) {
        klog("Kappa", "usb %s: no device slot (%d)", dev->path, rc);
        unlink_device(dev);
        kfree(dev);
        return NULL;
    }
    usb_dbg("usb %s: slot %u, route %#x, enumerating", dev->path, dev->slot, dev->route);
    rc = dev_address(dev, attempt);
    usb_dbg("usb %s: addressed: %d", dev->path, rc);
    if (!rc) rc = dev_read_config(dev);
    usb_dbg("usb %s: configuration read: %d", dev->path, rc);
    if (rc) {
        klog("Kappa", "usb %s: setup failed at '%s' (%d), attempt %d of %d", dev->path, g_step, rc, attempt + 1, PORT_INIT_TRIES);
        dev->dead = true;
        hc->ops->free_dev(hc, dev);
        unlink_device(dev);
        kfree(dev->cfg);
        kfree(dev);
        return NULL;
    }
    hub->port[port - 1].dev = dev;                          /* from here on usb_disconnect() can undo everything */
    proc_update();
    rc = dev_configure(dev);
    usb_dbg("usb %s: configured: %d", dev->path, rc);
    if (rc) {
        klog("Kappa", "usb %s: could not configure %04x:%04x at '%s' (%d)", dev->path, dev->desc.idVendor, dev->desc.idProduct, g_step, rc);
        /* A device we cannot configure is not worth another round of resets: take it down and let the caller retry. */
        usb_disconnect(dev);
        return NULL;
    }
    fetch_strings(dev);
    if (g_usb_stalltest) {
        /* A vendor request that no device implements: the device answers with a STALL, which halts the control endpoint. The next
         * request only works if our recovery (Reset Endpoint + Set TR Dequeue Pointer) put the pipe back in business. */
        u8 junk[8];
        UsbDeviceDesc again;
        int r1 = usb_control(dev, USB_DIR_IN | USB_TYPE_VENDOR, 0x77, 0, 0, junk, sizeof junk, CTRL_TIMEOUT);
        int r2 = usb_get_descriptor(dev, USB_DT_DEVICE, 0, 0, &again, 18);
        klog("Kappa", "usb %s: stall test: bogus request returned %d, device descriptor afterwards returned %d", dev->path, r1, r2);
    }
    klog("Kappa", "usb %s: new %s device %04x:%04x \"%s%s%s\"", dev->path, usb_speed_name(dev->speed), dev->desc.idVendor, dev->desc.idProduct,
         dev->manufacturer, dev->manufacturer[0] && dev->product[0] ? " " : "", dev->product);
    proc_update();
    if (g_usb_bulktest) task_create_detached("usbtest", bulk_thread, dev);
    return dev;
}

/* ---------------------------------------------------------------- port events */

static int port_debounce(UsbHub *hub, int port, UsbPortStatus *st) {
    u32 stable = 0;
    for (u32 total = 0; total < 2000; total += 25) {                    /* Linux: HUB_DEBOUNCE_TIMEOUT 2000, STEP 25, STABLE 100 */
        if (hub->ops->get_status(hub, port, st)) return -ENODEV;
        if (st->change & UPC_CONNECT) { hub->ops->clear_change(hub, port, UPC_CONNECT); stable = 0; }
        else if (st->connected || st->warm_reset) stable += 25;
        else return -ENODEV;
        if (stable >= 100) return 0;
        usb_msleep(hub->hc, 25);
    }
    return -ETIMEDOUT;
}

static void port_connect(UsbHub *hub, int port, UsbPortStatus *st0) {
    UsbPort *pt = &hub->port[port - 1];
    UsbPortStatus st = *st0;
    UsbHc *hc = hub->hc;
    if (hub->dev && hub->dev->depth >= USB_MAX_DEPTH) {
        klog("Kappa", "usb %s.%d: too many hubs in a row; ignoring this port", hub->dev->path, port);
        pt->failed = true;
        return;
    }
    if (!st.warm_reset && port_debounce(hub, port, &st)) return;       /* it went away again (or never settled) */
    bool warm = st.warm_reset;
    for (int attempt = 0; attempt < PORT_INIT_TRIES; attempt++) {
        if (hub->dead || hc->dead) return;
        if (warm) {                                                    /* a USB 3 link that will not train: a few warm resets, then leave it */
            if (pt->warm_tries >= 3) {
                if (pt->warm_tries == 3) {
                    pt->warm_tries++;
                    klog("Kappa", "usb: port %d: link does not come up after 3 warm resets; ignoring the port until something changes", port);
                }
                return;
            }
            pt->warm_tries++;
        }
        usb_dbg("usb: port %d reset (attempt %d%s)", port, attempt + 1, warm ? ", warm" : "");
        int rc = hub->ops->reset(hub, port, warm, &st);
        warm = false;
        usb_dbg("usb: port %d reset done: %d, speed %s", port, rc, usb_speed_name(st.speed));
        if (rc == -ENODEV) return;
        if (rc) {
            usb_dbg("usb: port %d reset failed (%d), attempt %d", port, rc, attempt + 1);
            usb_msleep(hc, 200);
            if (!hub->ops->get_status(hub, port, &st) && st.warm_reset) warm = true;
            continue;
        }
        UsbDevice *dev = usb_setup_device(hub, port, &st, attempt);
        if (dev) return;
        usb_msleep(hc, 200);
        if (hub->dead || hc->dead) return;
        if (hub->ops->get_status(hub, port, &st) || !st.connected) return;
        if (st.warm_reset) warm = true;
    }
    pt->failed = true;
    char where[32];
    if (hub->dev) snprintf(where, sizeof where, "%s.%d", hub->dev->path, port);
    else snprintf(where, sizeof where, "%d-%d", hc->busnum, port);
    klog("Kappa", "usb %s: giving up on the device on this port after %d attempts", where, PORT_INIT_TRIES);
}

static void port_event(UsbHub *hub, int port) {
    UsbPort *pt = &hub->port[port - 1];
    UsbPortStatus st;
    if (hub->ops->get_status(hub, port, &st)) {
        if (pt->dev && hub->hc->dead) usb_disconnect(pt->dev);
        return;
    }
    usb_dbg("usb: bus %d%s%s port %d event: connected %d enabled %d reset %d link %u change %#x speed %s", hub->hc->busnum, hub->dev ? " hub " : " root",
            hub->dev ? hub->dev->path : "", port, st.connected, st.enabled, st.resetting, st.link_state, st.change, usb_speed_name(st.speed));
    if (st.change) hub->ops->clear_change(hub, port, st.change);
    if (st.connected && st.enabled && st.link_state == 0) pt->warm_tries = 0;      /* the link is healthy again */
    if (st.overcurrent) klog("Kappa", "usb: over-current condition on port %d", port);
    if (!st.connected || (st.change & UPC_CONNECT)) pt->failed = false;
    if (pt->dev && (!st.connected || (st.change & UPC_CONNECT) || !st.enabled)) usb_disconnect(pt->dev);
    if (!pt->dev && !pt->failed && (st.connected || st.warm_reset)) port_connect(hub, port, &st);
}

static void hub_service(UsbHub *hub) {
    hub->any_pending = false;
    if (hub->ops->service) hub->ops->service(hub);
    for (int p = 1; p <= hub->nports; p++) {
        if (!(hub->pending[p >> 3] & (1u << (p & 7)))) continue;
        hub->pending[p >> 3] &= (u8)~(1u << (p & 7));
        port_event(hub, p);
        if (hub->dead || hub->hc->dead) return;
    }
}

static bool hub_alive(UsbHub *h) {
    for (UsbHub *x = g_hubs; x; x = x->next) if (x == h) return true;
    return false;
}

static void service_hubs(void) {
    UsbHub *todo[24];
    int n = 0;
    for (UsbHub *h = g_hubs; h && n < 24; h = h->next)
        if (h->any_pending && !h->dead && !h->hc->dead) todo[n++] = h;
    for (int i = 0; i < n; i++) {
        if (!hub_alive(todo[i]) || todo[i]->dead || todo[i]->hc->dead) continue;       /* disappeared while we were busy */
        hub_service(todo[i]);
    }
}

/* ---------------------------------------------------------------- the usbd thread */

static void hc_teardown(UsbHc *hc) {
    if (!hc->root.port) return;
    for (int p = 0; p < hc->root.nports; p++)
        if (hc->root.port[p].dev) usb_disconnect(hc->root.port[p].dev);
}

/* ---------------------------------------------------------------- calls from other threads */

typedef struct UsbJob {
    int (*fn)(void *);
    void *arg;
    int result;
    Semaphore done;
    struct UsbJob *next;
} UsbJob;

static UsbJob *g_job_head, *g_job_tail;
static Task *g_usbd_task;
static volatile bool g_usbd_up;

/* Everything in the USB stack belongs to the usbd thread. Other threads (a block device's read callback, a syscall) get their USB
 * work done by handing it over: fn(arg) runs in the usbd thread between two polling rounds, the caller sleeps until it is finished
 * and receives fn's return value. From inside the usbd thread (a probe function, a callback's follow-up) it is a plain call. */
int usb_call(int (*fn)(void *arg), void *arg) {
    if (current == g_usbd_task) return fn(arg);
    if (!g_usbd_up) return -ENODEV;
    UsbJob job = { fn, arg, -ENODEV, { 0 }, NULL };
    sem_init(&job.done, 0);
    u64 f = irq_save();
    if (g_job_tail) g_job_tail->next = &job; else g_job_head = &job;
    g_job_tail = &job;
    irq_restore(f);
    sem_wait(&job.done);                                /* usbd's sem_post is its last access to the job */
    return job.result;
}

static void run_jobs(bool refuse) {
    for (;;) {
        u64 f = irq_save();
        UsbJob *j = g_job_head;
        if (j) { g_job_head = j->next; if (!g_job_head) g_job_tail = NULL; }
        irq_restore(f);
        if (!j) return;
        j->result = refuse ? -ENODEV : j->fn(j->arg);
        sem_post(&j->done);
    }
}

static bool hubs_have_pending(void) {
    for (UsbHub *h = g_hubs; h; h = h->next)
        if (h->any_pending && !h->dead && !h->hc->dead) return true;
    return false;
}

/* ---------------------------------------------------------------- boot rendezvous */

/* Nothing left to do on the bus? Reads only variables that usbd publishes (never the hub or device lists, which usbd changes under
 * its feet). A bus with nothing plugged in at start-up counts as settled the moment the controllers are up. */
static bool usb_settled_now(void) {
    if (!g_usbd_started) return false;                      /* controllers still resetting */
    if (!g_usbd_up || !g_usb_expect) return true;           /* no working controller, or an empty bus */
    if (g_usb_busy || g_job_head) return false;             /* a port change, an enumeration or a block request is in progress */
    if (msc_busy() || storage_automount_busy()) return false;   /* a stick is still getting ready / its HUMANVILLAGE volume is being mounted */
    return uptime_ms() - g_usb_last_active >= SETTLE_QUIET_MS;  /* and nothing new arrived for a moment (hub ports report late) */
}

bool usb_wait_settled(u32 max_ms) {
    if (!g_nhc) return true;                                /* usb=off, or no xHCI controller at all */
    u64 t0 = uptime_ms();
    while (!usb_settled_now()) {
        if (uptime_ms() - t0 >= max_ms) {
            klog("Kappa", "usb: still busy after %u ms (a stick that is slow to get ready?): boot goes on, the bus is watched in the background", max_ms);
            return false;
        }
        task_sleep_ms(5);
    }
    u64 waited = uptime_ms() - t0;
    if (g_usb_expect && g_usbd_up) klog("Kappa", "usb: the bus settled after %lu ms", (unsigned long)waited);
    return true;
}

static void usbd_main(void *arg) {
    (void)arg;
    int alive = 0;
    g_usbd_task = current;
    for (int i = 0; i < g_nhc; i++) {
        UsbHc *hc = g_hcs[i];
        int rc = hc->ops->start(hc);
        if (rc) {
            hc->failed = true;
            if (!hc->dead) hc->dead = true;
            klog("Kappa", "xhci %02x:%02x.%x: start-up failed: %s", hc->pci->bus, hc->pci->dev, hc->pci->fn, hc->status[0] ? hc->status : "unknown error");
            hc->pci->driver = "xhci (failed)";
            continue;
        }
        usb_hub_register(&hc->root);
        alive++;
    }
    proc_update();
    for (int i = 0; i < g_nhc; i++)
        if (!g_hcs[i]->failed && g_hcs[i]->root.any_pending) g_usb_expect = true;     /* start() queued every port that had something attached */
    g_usb_last_active = uptime_ms();
    g_usbd_started = true;
    if (!alive) return;                                     /* nothing to poll */

    g_usbd_up = true;
    bool hinted = false;
    for (;;) {
        for (int i = 0; i < g_nhc; i++) {
            UsbHc *hc = g_hcs[i];
            if (hc->failed) continue;
            if (hc->dead) { hc_teardown(hc); continue; }
            hc->ops->poll(hc);
            if (hc->dead) continue;
            hc->ops->service(hc);
        }
        bool work = hubs_have_pending() || g_job_head != NULL;
        if (work) g_usb_busy = 1;                           /* set before the flags that show the work are cleared, so a waiter never sees a gap */
        service_hubs();
        hid_service();
        msc_service();
        run_jobs(false);
        if (work || msc_busy()) g_usb_last_active = uptime_ms();
        g_usb_busy = 0;
        if (!hinted && uptime_ms() > 8000) {
            /* One line for whoever is staring at a machine whose keyboard does not work: what the controllers see on their ports. */
            hinted = true;
            if (!hid_keyboard_count()) {
                for (int i = 0; i < g_nhc; i++) {
                    UsbHc *hc = g_hcs[i];
                    char ports[400];
                    ports[0] = 0;
                    if (!hc->failed && hc->ops->port_summary) hc->ops->port_summary(hc, ports, sizeof ports);
                    klog("Satori", "no USB keyboard is bound 8 s after boot; xhci %02x:%02x.%x%s%s", hc->pci->bus, hc->pci->dev, hc->pci->fn,
                         hc->failed ? " failed to start: " : hc->dead ? " is dead: " : " ports (number:PORTSC): ",
                         hc->failed || hc->dead ? hc->status : ports);
                }
            }
        }
        task_sleep_ms(1);
    }
}

void usb_init(void) {
    char opt[16];
    if (cmdline_value("usb", opt, sizeof opt)) {
        if (!strcmp(opt, "off") || !strcmp(opt, "0") || !strcmp(opt, "no")) {
            klog("Kappa", "usb: disabled on the kernel command line (usb=off)");
            return;
        }
        if (!strcmp(opt, "debug")) g_usb_debug = true;
        if (!strcmp(opt, "hidreport")) { g_usb_debug = true; g_usb_hidreport = true; }
        if (!strcmp(opt, "stalltest")) { g_usb_debug = true; g_usb_stalltest = true; }
        if (!strcmp(opt, "bulktest")) g_usb_bulktest = true;
        if (!strcmp(opt, "parsertest")) hid_parser_selftest();
    }
    for (PciDev *d = pci_find_class(0x0C, 0x03, 0x30, NULL); d && g_nhc < MAX_BUSES; d = pci_find_class(0x0C, 0x03, 0x30, d)) {
        UsbHc *hc = xhci_create(d, g_nhc + 1);
        if (!hc) { klog("Kappa", "usb: out of memory"); break; }
        g_hcs[g_nhc++] = hc;
    }
    if (!g_nhc) {
        for (PciDev *d = pci_first(); d; d = pci_next(d))
            if (d->class_code == 0x0C && d->subclass == 0x03) {
                klog("Kappa", "usb: %02x:%02x.%x is a %s controller; only xHCI is supported", d->bus, d->dev, d->fn,
                     d->progif == 0x20 ? "EHCI" : d->progif == 0x10 ? "OHCI" : d->progif == 0 ? "UHCI" : "legacy USB");
            }
        return;
    }
    procfs_register("usb", gen_usb);
    procfs_register("usbstat", gen_usbstat);
    procfs_register("usbstorage", msc_describe);
    snprintf(g_proc, sizeof g_proc, "USB starting...\n");
    g_proc_len = strlen(g_proc);
    if (!task_create_detached("usbd", usbd_main, NULL)) klog("Kappa", "usb: cannot start the usbd thread");
}
