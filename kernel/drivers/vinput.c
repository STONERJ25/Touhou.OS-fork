/*
 * /dev/vinput: a virtual input device for root (the VNC server, vncd). Records written here (struct ginput_event of shared/abi.h,
 * 16 bytes each; the time field is ignored and stamped by the kernel) join the event queue of /dev/input/events exactly as if a
 * keyboard or a mouse had made them, so the window server cannot tell them from hardware and needs no second input path.
 *
 * Accepted: EV_KEY with a key code (1..255, value 0 up, 1 down, 2 repeat) or BTN_LEFT/RIGHT/MIDDLE (0 or 1); EV_REL with REL_X,
 * REL_Y (at most 4096 per record) or REL_WHEEL (at most 16 clicks); EV_ABS with ABS_X, ABS_Y (0..65535: a position on the screen in
 * physical pixels, which the window server clamps). A write must be whole records, and one bad record refuses the whole write
 * (-EINVAL) before any of it is queued. Only root may write (the node is 0600 root, and the write checks the caller again).
 *
 * The events go straight into the window server's queue: they do not pass the console's key handling (so they do not type into the
 * text console, and Ctrl+Alt+Backspace sent this way does not shoot the window server; the remote viewer has the close button).
 */
#include "vfs.h"
#include "kernel.h"
#include "cpu.h"
#include "input.h"
#include "process.h"
#include "initcall.h"

#define VABS_X 0
#define VABS_Y 1

static bool valid(const struct ginput_event *e) {
    switch (e->type) {
    case EV_KEY:
        if (e->code == BTN_LEFT || e->code == BTN_RIGHT || e->code == BTN_MIDDLE) return e->value == 0 || e->value == 1;
        return e->code >= 1 && e->code <= 255 && e->value >= 0 && e->value <= 2;
    case EV_REL:
        if (e->code == REL_X || e->code == REL_Y) return e->value >= -4096 && e->value <= 4096;
        if (e->code == REL_WHEEL) return e->value >= -16 && e->value <= 16 && e->value != 0;
        return false;
    case EV_ABS:
        return (e->code == VABS_X || e->code == VABS_Y) && e->value >= 0 && e->value <= 65535;
    default:
        return false;
    }
}

static ssize_t vinput_write(VNode *v, File *f, const void *buf, size_t n, u64 off) {
    (void)v; (void)f; (void)off;
    if (!current->proc || current->proc->cred.uid != 0) return -EPERM;
    if (n == 0) return 0;
    if (n % sizeof(struct ginput_event)) return -EINVAL;
    const struct ginput_event *e = buf;
    size_t count = n / sizeof *e;
    for (size_t i = 0; i < count; i++) if (!valid(&e[i])) return -EINVAL;
    for (size_t i = 0; i < count; i++) {
        u64 fl = irq_save();                                    /* the keyboard and mouse interrupts write the same queue */
        input_emit(e[i].type, e[i].code, e[i].value);
        irq_restore(fl);
    }
    return (ssize_t)n;
}

static ssize_t vinput_read(VNode *v, File *f, void *buf, size_t n, u64 off) {
    (void)v; (void)f; (void)buf; (void)n; (void)off;
    return -EINVAL;                                             /* write only: the events are read from /dev/input/events */
}

static int vinput_poll(VNode *v, File *f) { (void)v; (void)f; return POLLOUT; }

static const VNodeOps vinput_ops = { .read = vinput_read, .write = vinput_write, .poll = vinput_poll };

static void vinput_init(void) {
    if (devfs_register("vinput", S_IFCHR | 0600, &vinput_ops, NULL, 0x0D41) == 0)
        klog("Satori", "/dev/vinput: keys and pointer from root's programs join the events (the VNC server's way in)");
}
INITCALL(vinput_init);
