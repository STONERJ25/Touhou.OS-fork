/*
 * USB hub class driver for USB 2.0 hubs (single or multiple transaction translators) and USB 3.x hubs.
 *
 * A hub is just another UsbHub for the core: get_status/clear_change/reset are class requests on the hub's control pipe, and the
 * status-change interrupt endpoint marks the ports that need a look. The hub's own bits (hub descriptor, slot context "hub"
 * fields, SET_HUB_DEPTH, port power) are set up here, in the order Linux's hub_configure() uses.
 */
#include "usb.h"
#include "kernel.h"
#include "task.h"
#include "mm.h"
#include "abi.h"

#define HUB_REQ_SET_DEPTH   12

#define F_C_HUB_LOCAL_POWER 0
#define F_C_HUB_OVERCUR     1
#define F_PORT_RESET        4
#define F_PORT_POWER        8
#define F_C_PORT_CONNECTION 16
#define F_C_PORT_ENABLE     17
#define F_C_PORT_OVERCUR    19
#define F_C_PORT_RESET      20
#define F_C_PORT_LINK       25
#define F_C_PORT_CONFIG     26
#define F_BH_PORT_RESET     28
#define F_C_BH_PORT_RESET   29

#define RT_HUB_IN     (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE)    /* 0xA0 */
#define RT_HUB_OUT    (USB_TYPE_CLASS | USB_RECIP_DEVICE)                 /* 0x20 */
#define RT_PORT_IN    (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER)     /* 0xA3 */
#define RT_PORT_OUT   (USB_TYPE_CLASS | USB_RECIP_OTHER)                  /* 0x23 */

#define HUB_TIMEOUT   1500

typedef struct HubInfo {
    UsbHub hub;
    UsbDevice *dev;
    u16 chars;
    u8  pwr2good;
    bool change_pending;                /* the hub itself (not a port) reported a status change */
} HubInfo;

static inline HubInfo *HI(UsbHub *h) { return container_of(h, HubInfo, hub); }

static int port_raw(HubInfo *hi, int port, u16 *status, u16 *change) {
    u8 b[4];
    int rc = usb_control(hi->dev, RT_PORT_IN, USB_REQ_GET_STATUS, 0, (u16)port, b, 4, HUB_TIMEOUT);
    if (rc < 4) return rc < 0 ? rc : -EIO;
    *status = (u16)(b[0] | (b[1] << 8));
    *change = (u16)(b[2] | (b[3] << 8));
    return 0;
}

static int port_feature(HubInfo *hi, int port, bool set, u16 feature) {
    return usb_control(hi->dev, RT_PORT_OUT, set ? USB_REQ_SET_FEATURE : USB_REQ_CLEAR_FEATURE, feature, (u16)port, NULL, 0, HUB_TIMEOUT);
}

static void convert_status(UsbHub *hub, u16 s, u16 c, UsbPortStatus *st) {
    memset(st, 0, sizeof *st);
    st->connected = s & 0x0001;
    st->enabled = s & 0x0002;
    st->resetting = s & 0x0010;
    st->overcurrent = s & 0x0008;
    if (hub->ss) {                                      /* USB 3 hub port status */
        st->powered = s & 0x0200;
        st->link_state = (u8)((s >> 5) & 0xF);
        st->speed = ((s >> 10) & 7) ? USB_SPEED_SUPER_PLUS : USB_SPEED_SUPER;
        st->warm_reset = st->link_state == 6 || st->link_state == 10;       /* SS.Inactive or compliance mode */
        if (c & 0x0001) st->change |= UPC_CONNECT;
        if (c & 0x0008) st->change |= UPC_OVERCUR;
        if (c & 0x0010) st->change |= UPC_RESET;
        if (c & 0x0020) st->change |= UPC_BH_RESET;
        if (c & 0x0040) st->change |= UPC_LINK;
        if (c & 0x0080) st->change |= UPC_CONFIG;
    } else {
        st->powered = s & 0x0100;
        st->speed = (s & 0x0200) ? USB_SPEED_LOW : (s & 0x0400) ? USB_SPEED_HIGH : USB_SPEED_FULL;
        if (c & 0x0001) st->change |= UPC_CONNECT;
        if (c & 0x0002) st->change |= UPC_ENABLE;
        if (c & 0x0008) st->change |= UPC_OVERCUR;
        if (c & 0x0010) st->change |= UPC_RESET;
    }
}

static int hub_get_status(UsbHub *hub, int port, UsbPortStatus *st) {
    u16 s, c;
    int rc = port_raw(HI(hub), port, &s, &c);
    if (rc) return rc;
    convert_status(hub, s, c, st);
    return 0;
}

static void hub_clear_change(UsbHub *hub, int port, u32 change) {
    HubInfo *hi = HI(hub);
    if (change & UPC_CONNECT) port_feature(hi, port, false, F_C_PORT_CONNECTION);
    if ((change & UPC_ENABLE) && !hub->ss) port_feature(hi, port, false, F_C_PORT_ENABLE);
    if (change & UPC_OVERCUR) port_feature(hi, port, false, F_C_PORT_OVERCUR);
    if (change & UPC_RESET) port_feature(hi, port, false, F_C_PORT_RESET);
    if ((change & UPC_BH_RESET) && hub->ss) port_feature(hi, port, false, F_C_BH_PORT_RESET);
    if ((change & UPC_LINK) && hub->ss) port_feature(hi, port, false, F_C_PORT_LINK);
    if ((change & UPC_CONFIG) && hub->ss) port_feature(hi, port, false, F_C_PORT_CONFIG);
}

static int hub_reset(UsbHub *hub, int port, bool warm, UsbPortStatus *out) {
    HubInfo *hi = HI(hub);
    bool bh = warm && hub->ss;
    int rc = port_feature(hi, port, true, bh ? F_BH_PORT_RESET : F_PORT_RESET);
    if (rc < 0) return rc;
    u32 limit = bh ? 4000 : 1500;
    u16 s = 0, c = 0;
    bool done = false;
    for (u32 t = 0; t < limit; t += 20) {
        usb_msleep(hub->hc, 20);
        if (hub->dead || hub->hc->dead) return -ENODEV;
        rc = port_raw(hi, port, &s, &c);
        if (rc) return rc;
        if (!(s & 1) && !bh) return -ENODEV;                            /* unplugged while resetting */
        bool changed = bh ? (c & 0x0020) : (c & 0x0010);
        if (changed && !(s & 0x0010)) { done = true; break; }
    }
    if (!done) { usb_dbg("usb: hub port %d reset timed out (status %04x change %04x)", port, s, c); return -ETIMEDOUT; }
    port_feature(hi, port, false, F_C_PORT_RESET);
    if (hub->ss) port_feature(hi, port, false, F_C_BH_PORT_RESET);
    port_feature(hi, port, false, F_C_PORT_CONNECTION);
    usb_msleep(hub->hc, 50);                                            /* TRSTRCY with some margin */
    rc = port_raw(hi, port, &s, &c);
    if (rc) return rc;
    convert_status(hub, s, c, out);
    if (!out->connected) return -ENODEV;
    if (!out->enabled && !(hub->ss && out->link_state == 0)) { usb_dbg("usb: hub port %d not enabled after reset (status %04x)", port, s); return -EIO; }
    out->enabled = true;
    return 0;
}

/* Hub level change (local power, over-current): acknowledge so the status endpoint stops reporting it. */
static void hub_service_hub(UsbHub *hub) {
    HubInfo *hi = HI(hub);
    if (!hi->change_pending) return;
    hi->change_pending = false;
    u8 b[4];
    if (usb_control(hi->dev, RT_HUB_IN, USB_REQ_GET_STATUS, 0, 0, b, 4, HUB_TIMEOUT) < 4) return;
    u16 change = (u16)(b[2] | (b[3] << 8));
    if (change & 1) usb_control(hi->dev, RT_HUB_OUT, USB_REQ_CLEAR_FEATURE, F_C_HUB_LOCAL_POWER, 0, NULL, 0, HUB_TIMEOUT);
    if (change & 2) usb_control(hi->dev, RT_HUB_OUT, USB_REQ_CLEAR_FEATURE, F_C_HUB_OVERCUR, 0, NULL, 0, HUB_TIMEOUT);
}

static const UsbHubOps hub_ops = { hub_get_status, hub_clear_change, hub_reset, hub_service_hub };

/* The status change endpoint: one bit per port (bit 0 is the hub itself). */
static void hub_irq(UsbDevice *dev, UsbEndpoint *ep, const u8 *data, int len, void *ctx) {
    HubInfo *hi = ctx;
    for (int p = 0; p <= hi->hub.nports && (p >> 3) < len; p++) {
        if (!(data[p >> 3] & (1u << (p & 7)))) continue;
        if (p == 0) { hi->change_pending = true; hi->hub.any_pending = true; }
        else usb_hub_mark_port(&hi->hub, p);
    }
}

static bool hub_match(UsbDevice *dev, UsbInterface *intf) {
    if (intf->cls != USB_CLASS_HUB) return false;
    for (int i = 0; i < intf->nep; i++)
        if (intf->ep[i].type == USB_EP_INT && (intf->ep[i].addr & USB_DIR_IN)) return true;
    return false;
}

static int hub_probe(UsbDevice *dev, UsbInterface *intf) {
    UsbEndpoint *ep = NULL;
    for (int i = 0; i < intf->nep && !ep; i++)
        if (intf->ep[i].type == USB_EP_INT && (intf->ep[i].addr & USB_DIR_IN)) ep = &intf->ep[i];
    if (!ep || dev->hub) return -ENODEV;                                /* one hub function per device */

    bool ss = dev->speed >= USB_SPEED_SUPER;
    u8 d[16];
    int rc = -EIO;
    for (int i = 0; i < 3 && rc < 7; i++) {
        if (i) usb_msleep(dev->hc, 50);
        rc = usb_control(dev, RT_HUB_IN, USB_REQ_GET_DESCRIPTOR, (u16)((ss ? USB_DT_SS_HUB : USB_DT_HUB) << 8), 0, d, sizeof d, HUB_TIMEOUT);
        if (rc == -ENODEV || dev->broken) break;
    }
    if (rc < 7 || d[1] != (ss ? USB_DT_SS_HUB : USB_DT_HUB) || d[2] == 0) {
        klog("Kappa", "usb %s: unreadable hub descriptor (%d)", dev->path, rc);
        return rc < 0 ? rc : -EIO;
    }
    HubInfo *hi = kzalloc(sizeof *hi);
    if (!hi) return -ENOMEM;
    hi->dev = dev;
    hi->chars = (u16)(d[3] | (d[4] << 8));
    hi->pwr2good = d[5];
    hi->hub.ops = &hub_ops;
    hi->hub.hc = dev->hc;
    hi->hub.dev = dev;
    hi->hub.nports = d[2];
    hi->hub.ss = ss;
    hi->hub.port = kzalloc(hi->hub.nports * sizeof(UsbPort));
    if (!hi->hub.port) { kfree(hi); return -ENOMEM; }

    /* A high-speed hub whose device descriptor says "multiple TTs" (bDeviceProtocol 2) has a transaction translator per port,
     * enabled by selecting alternate setting 1 of its interface (the interface itself reports protocol 1 until then). Same test as
     * Linux's hub_probe(). */
    if (dev->speed == USB_SPEED_HIGH && dev->desc.bDeviceProtocol == 2 &&
        usb_control(dev, USB_RECIP_IFACE, USB_REQ_SET_INTERFACE, 1, intf->number, NULL, 0, HUB_TIMEOUT) >= 0)
        hi->hub.mtt = true;
    if (ss) usb_control(dev, RT_HUB_OUT, HUB_REQ_SET_DEPTH, dev->depth, 0, NULL, 0, HUB_TIMEOUT);       /* failure is not fatal */

    rc = dev->hc->ops->update_hub(dev->hc, dev, hi->hub.nports, (hi->chars >> 5) & 3, hi->hub.mtt);
    if (rc) {
        klog("Kappa", "usb %s: controller refused the hub setup (%d)", dev->path, rc);
        kfree(hi->hub.port);
        kfree(hi);
        return rc;
    }

    dev->hub = &hi->hub;
    intf->drv_data = hi;
    usb_hub_register(&hi->hub);
    for (int p = 1; p <= hi->hub.nports; p++) port_feature(hi, p, true, F_PORT_POWER);
    u32 delay = hi->pwr2good * 2u;
    usb_msleep(dev->hc, delay < 100 ? 100 : delay);                     /* power-on-to-power-good, with a floor */

    rc = usb_int_start(dev, ep, hub_irq, hi);
    if (rc) {
        klog("Kappa", "usb %s: cannot poll the hub status endpoint (%d)", dev->path, rc);
        usb_hub_unregister(&hi->hub);
        dev->hub = NULL;
        intf->drv_data = NULL;
        kfree(hi->hub.port);
        kfree(hi);
        return rc;
    }
    for (int p = 1; p <= hi->hub.nports; p++) usb_hub_mark_port(&hi->hub, p);       /* devices may already be plugged in */
    klog("Kappa", "usb %s: %s hub with %d port%s%s", dev->path, ss ? "SuperSpeed" : (dev->speed == USB_SPEED_HIGH ? "high-speed" : "full-speed"),
         hi->hub.nports, hi->hub.nports == 1 ? "" : "s", hi->hub.mtt ? ", one translator per port" : "");
    return 0;
}

static void hub_disconnect(UsbDevice *dev, UsbInterface *intf) {
    HubInfo *hi = intf->drv_data;
    if (!hi) return;
    hi->hub.dead = true;
    for (int p = 0; p < hi->hub.nports; p++)
        if (hi->hub.port[p].dev) usb_disconnect(hi->hub.port[p].dev);
    usb_hub_unregister(&hi->hub);
    dev->hub = NULL;
    intf->drv_data = NULL;
    kfree(hi->hub.port);
    kfree(hi);
}

static const char *hub_label(UsbInterface *intf) { return "hub"; }

const UsbDriver usb_hub_driver = { "hub", hub_match, hub_probe, hub_disconnect, hub_label };
