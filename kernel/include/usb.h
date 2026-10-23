/*
 * USB host stack. Layers (all in kernel/drivers/usb/):
 *
 *   xhci.c   host controller driver: rings, slots, contexts, root hub ports      (the only controller we drive)
 *   usb.c    core: port events, enumeration, descriptors, device tree, /proc/usb
 *   hub.c    class driver for USB 2 and USB 3 hubs
 *   hid.c    class driver for HID keyboards and mice -> input_key() / input_mouse()
 *
 * Everything runs in ONE kernel thread ("usbd") that polls at 1 kHz; there are no interrupts and no locks, so a driver
 * never has to think about concurrency. Every wait has a timeout. A misbehaving controller is declared dead (one klog
 * line) and left alone; it never blocks boot and never gets memory handed back that it might still write to.
 */
#ifndef K_USB_H
#define K_USB_H

#include "types.h"
#include "printk.h"
#include "irq.h"
#include "pci.h"

/* ---------------------------------------------------------------- standard USB definitions */

enum { USB_SPEED_NONE = 0, USB_SPEED_LOW, USB_SPEED_FULL, USB_SPEED_HIGH, USB_SPEED_SUPER, USB_SPEED_SUPER_PLUS };

#define USB_DIR_IN        0x80
#define USB_TYPE_CLASS    0x20
#define USB_TYPE_VENDOR   0x40
#define USB_RECIP_DEVICE  0x00
#define USB_RECIP_IFACE   0x01
#define USB_RECIP_EP      0x02
#define USB_RECIP_OTHER   0x03

#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_SET_INTERFACE     0x0B

#define USB_DT_DEVICE     0x01
#define USB_DT_CONFIG     0x02
#define USB_DT_STRING     0x03
#define USB_DT_INTERFACE  0x04
#define USB_DT_ENDPOINT   0x05
#define USB_DT_HID        0x21
#define USB_DT_HID_REPORT 0x22
#define USB_DT_HUB        0x29
#define USB_DT_SS_HUB     0x2A
#define USB_DT_SS_EP_COMP 0x30

#define USB_CLASS_HID     0x03
#define USB_CLASS_HUB     0x09

#define USB_EP_CONTROL 0
#define USB_EP_ISOC    1
#define USB_EP_BULK    2
#define USB_EP_INT     3

typedef struct PACKED UsbSetup {
    u8  bmRequestType;
    u8  bRequest;
    u16 wValue;
    u16 wIndex;
    u16 wLength;
} UsbSetup;

typedef struct PACKED UsbDeviceDesc {
    u8  bLength, bDescriptorType;
    u16 bcdUSB;
    u8  bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    u16 idVendor, idProduct, bcdDevice;
    u8  iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} UsbDeviceDesc;

/* ---------------------------------------------------------------- core objects */

#define USB_MAX_INTERFACES 16
#define USB_MAX_EPS        8        /* per interface */
#define USB_MAX_DEPTH      5        /* hubs between a device and the root (route string has five nibbles) */

struct UsbDevice;
struct UsbEndpoint;
struct UsbHc;
struct UsbHub;

/* Called by the host controller for every completed interrupt-IN transfer; the controller re-arms the endpoint itself.
 * Runs inside the polling thread, so it must not start control transfers (queue them for later instead). */
typedef void (*UsbIntCb)(struct UsbDevice *dev, struct UsbEndpoint *ep, const u8 *data, int len, void *ctx);

typedef struct UsbEndpoint {
    u8  addr;                       /* bEndpointAddress, bit 7 = IN */
    u8  type;                       /* USB_EP_* */
    u16 mps;                        /* wMaxPacketSize bits 10:0 */
    u8  extra_tx;                   /* wMaxPacketSize bits 12:11 (high-bandwidth high-speed endpoints) */
    u8  interval;                   /* bInterval */
    u8  burst;                      /* SuperSpeed companion: bMaxBurst */
    u8  comp_mult;                  /* SuperSpeed companion: bmAttributes mult (isochronous) */
    u16 bytes_per_interval;         /* SuperSpeed companion */
    u8  dci;                        /* device context index (xHCI): 2 * number + direction */
    bool configured;
    UsbIntCb cb;
    void *cb_ctx;
    void *hc;                       /* host controller private state */
} UsbEndpoint;

struct UsbDriver;

typedef struct UsbInterface {
    u8  number, cls, subcls, proto;
    u8  nep;
    UsbEndpoint ep[USB_MAX_EPS];
    const u8 *extra;                /* class specific descriptors that follow the interface descriptor (HID descriptor) */
    u16 extra_len;
    const struct UsbDriver *driver;
    void *drv_data;
} UsbInterface;

typedef struct UsbDevice {
    struct UsbHc *hc;
    struct UsbHub *parent;          /* hub whose port this device sits on (root hub or class hub) */
    u8   port;                      /* 1-based port number on `parent` */
    u8   root_port;                 /* 1-based root hub port at the top of the chain */
    u8   depth;                     /* number of hubs between this device and the root: 0 = directly on a root port */
    u32  route;                     /* xHCI route string */
    u8   speed;                     /* USB_SPEED_* */
    u8   speed_id;                  /* controller specific speed id (PORTSC.PortSpeed / slot context speed) */
    u8   slot;                      /* controller slot id */
    u16  mps0;                      /* control endpoint max packet size in use */
    struct UsbDevice *tt_hub;       /* high-speed hub whose transaction translator serves this low/full-speed device */
    u8   tt_port;
    bool dead;                      /* being torn down: callbacks and transfers are refused */
    bool broken;                    /* a control transfer timed out: no more traffic except teardown */
    bool configured;
    UsbDeviceDesc desc;
    u8   cfg_value;
    u8  *cfg;                       /* raw configuration descriptor (kmalloc) */
    u16  cfg_len;
    int  nintf;
    UsbInterface intf[USB_MAX_INTERFACES];
    char path[24];                  /* "1-3.2": bus, root port, then hub ports */
    char manufacturer[64], product[64], serial[48];
    struct UsbHub *hub;             /* set if this device is a hub */
    void *hcpriv;
    struct UsbDevice *next;         /* list of all devices on the bus, in creation order */
} UsbDevice;

typedef struct UsbDriver {
    const char *name;
    /* Does this driver want the interface? Called before the device is configured; looks at descriptors only. */
    bool (*match)(UsbDevice *dev, UsbInterface *intf);
    /* After SET_CONFIGURATION. Return 0 if the interface is now bound. */
    int  (*probe)(UsbDevice *dev, UsbInterface *intf);
    void (*disconnect)(UsbDevice *dev, UsbInterface *intf);
    /* Short name of what the bound interface is (shown in /proc/usb): "hub", "hid-keyboard", ... */
    const char *(*label)(UsbInterface *intf);
} UsbDriver;

extern const UsbDriver usb_hub_driver;
extern const UsbDriver usb_hid_driver;

/* ---------------------------------------------------------------- hubs (the root hub is one too) */

/* Port status as seen by the enumeration code, whether it came from PORTSC or from a hub class request. */
#define UPC_CONNECT   0x01          /* connect status changed */
#define UPC_ENABLE    0x02
#define UPC_RESET     0x04          /* reset finished */
#define UPC_OVERCUR   0x08
#define UPC_LINK      0x10          /* USB 3 link state changed */
#define UPC_BH_RESET  0x20          /* USB 3 warm reset finished */
#define UPC_CONFIG    0x40

typedef struct UsbPortStatus {
    bool connected, enabled, resetting, powered, overcurrent;
    u8   speed;                     /* USB_SPEED_*, meaningful once enabled */
    u8   speed_id;
    u8   link_state;                /* USB 3 only: PLS value (0 = U0, 5 = RxDetect, 6 = Inactive, 10 = Compliance) */
    bool warm_reset;                /* USB 3: the link is stuck and needs a warm reset */
    u32  change;                    /* UPC_* */
} UsbPortStatus;

typedef struct UsbHubOps {
    int  (*get_status)(struct UsbHub *hub, int port, UsbPortStatus *st);
    void (*clear_change)(struct UsbHub *hub, int port, u32 change);
    /* Blocking: drives reset signalling to completion. On success the port is enabled and *st holds its new state. */
    int  (*reset)(struct UsbHub *hub, int port, bool warm, UsbPortStatus *st);
    void (*service)(struct UsbHub *hub);        /* optional: hub level changes (over-current, local power) */
} UsbHubOps;

typedef struct UsbPort {
    UsbDevice *dev;
    u8 failed;                      /* enumeration gave up on this connection; wait for the next connect change */
} UsbPort;

typedef struct UsbHub {
    const UsbHubOps *ops;
    struct UsbHc *hc;
    UsbDevice *dev;                 /* NULL for the root hub */
    int  nports;
    bool ss;                        /* SuperSpeed hub */
    bool mtt;                       /* high-speed hub with one transaction translator per port */
    bool dead;
    UsbPort *port;                  /* [nports], index = port - 1 */
    u8   pending[32];               /* bitmap, bit n = port n has (maybe) changed */
    volatile bool any_pending;
    struct UsbHub *next;            /* global list of hubs */
    void *priv;
} UsbHub;

void usb_hub_mark_port(UsbHub *hub, int port);       /* a port needs attention (called from status events) */
void usb_hub_register(UsbHub *hub);
void usb_hub_unregister(UsbHub *hub);

/* ---------------------------------------------------------------- host controllers */

typedef struct UsbHcOps {
    int  (*start)(struct UsbHc *hc);                                  /* reset and run; called from the usbd thread */
    void (*poll)(struct UsbHc *hc);                                   /* drain events, run callbacks, flag port changes */
    void (*service)(struct UsbHc *hc);                                /* deferred work that needs commands (endpoint recovery) */
    int  (*alloc_dev)(struct UsbHc *hc, UsbDevice *dev);              /* Enable Slot + default control pipe */
    int  (*address_dev)(struct UsbHc *hc, UsbDevice *dev, bool bsr);  /* Address Device (bsr: block SET_ADDRESS) */
    int  (*set_ep0_size)(struct UsbHc *hc, UsbDevice *dev, u16 mps);  /* Evaluate Context */
    int  (*add_endpoints)(struct UsbHc *hc, UsbDevice *dev, UsbEndpoint **eps, int n);
    int  (*update_hub)(struct UsbHc *hc, UsbDevice *dev, int nports, int ttt, bool mtt);
    void (*free_dev)(struct UsbHc *hc, UsbDevice *dev);               /* Disable Slot, free contexts and rings */
    int  (*control)(struct UsbHc *hc, UsbDevice *dev, const UsbSetup *s, void *buf, u32 timeout_ms);
    int  (*int_start)(struct UsbHc *hc, UsbDevice *dev, UsbEndpoint *ep);   /* poll an interrupt-IN endpoint forever */
} UsbHcOps;

typedef struct UsbHc {
    const UsbHcOps *ops;
    int  busnum;
    PciDev *pci;
    bool dead;                      /* the controller stopped answering: leave it alone */
    bool failed;                    /* start() failed */
    char status[96];                /* why it is dead or failed */
    char desc[160];                 /* one-line description for /proc/usb */
    UsbHub root;
    UsbDevice *devices;             /* every device on this bus */
    u32  nevents;                   /* statistics */
} UsbHc;

UsbHc *xhci_create(PciDev *pci, int busnum);        /* allocates the driver state; no hardware access besides PCI config */

/* ---------------------------------------------------------------- services for class drivers */

extern bool g_usb_debug;
extern bool g_usb_hidreport;        /* usb=hidreport: parse report descriptors even for boot interfaces (testing aid) */
#define usb_dbg(fmt, ...) do { if (g_usb_debug) klog("Kappa", "%lu: " fmt, (unsigned long)uptime_ms(), ##__VA_ARGS__); } while (0)

/* Control transfer on the default pipe. Returns bytes transferred (>= 0) or -errno (-EPIPE on a stall, -ETIMEDOUT, ...). */
int usb_control(UsbDevice *dev, u8 rtype, u8 req, u16 value, u16 index, void *buf, u16 len, u32 timeout_ms);
int usb_get_descriptor(UsbDevice *dev, u8 type, u8 index, u16 lang, void *buf, u16 len);
int usb_int_start(UsbDevice *dev, UsbEndpoint *ep, UsbIntCb cb, void *ctx);
const char *usb_speed_name(int speed);
void usb_msleep(UsbHc *hc, u32 ms);                  /* sleeps while the controller's events keep being processed */
void usb_disconnect(UsbDevice *dev);                 /* tears a device (and everything below it) down */

/* hid.c */
void hid_service(void);             /* top-level housekeeping (keyboard LEDs) */
size_t hid_describe_stats(char *buf, size_t cap);

#endif
