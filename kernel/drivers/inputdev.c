/* /dev/input/events: keyboard and mouse events for the window server, with an exclusive keyboard grab. */
#include "vfs.h"
#include "kernel.h"
#include "cpu.h"
#include "input.h"
#include "process.h"
#include "initcall.h"

static int g_grab_pid;

static void drop_grab(File *f) {
    if (f->priv) {
        f->priv = NULL;
        if (g_input_grab > 0) g_input_grab--;
        g_grab_pid = 0;
    }
}

static void input_opened(VNode *v, File *f) {
    (void)v;
    u64 fl = irq_save();
    if (g_input_readers == 0) g_input.head = g_input.tail = 0;          /* start from a clean queue */
    g_input_readers++;
    irq_restore(fl);
    f->priv = NULL;
}

static void input_closed(VNode *v, File *f) {
    (void)v;
    u64 fl = irq_save();
    drop_grab(f);
    if (g_input_readers > 0) g_input_readers--;
    irq_restore(fl);
}

static ssize_t input_read(VNode *v, File *f, void *buf, size_t n, u64 off) {
    (void)v; (void)off;
    size_t cap = n / sizeof(struct ginput_event);
    if (!cap) return -EINVAL;
    struct ginput_event *out = buf;
    size_t got = 0;
    for (;;) {
        while (got < cap && input_pop(&out[got])) got++;
        if (got) return (ssize_t)(got * sizeof(struct ginput_event));
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        if (signal_pending()) return -EINTR;
        u64 fl = irq_save();
        if (g_input.head == g_input.tail) {
            current->intr = true;
            wq_block(&g_input.wq);
            current->intr = false;
        }
        irq_restore(fl);
    }
}

static int input_poll_node(VNode *v, File *f) {
    (void)v; (void)f;
    return POLLOUT | (g_input.head != g_input.tail ? POLLIN : 0);
}

static int input_ioctl(VNode *v, File *f, unsigned long req, void *arg) {
    (void)v;
    if (req == INPUT_GRAB) {
        u64 fl = irq_save();
        bool want = *(int *)arg != 0;
        if (want && !f->priv) {
            f->priv = (void *)1;
            g_input_grab++;
            g_grab_pid = current->proc ? current->proc->pid : 0;
        } else if (!want) {
            drop_grab(f);
        }
        irq_restore(fl);
        return 0;
    }
    return -ENOTTY;
}

/* Ctrl+Alt+Backspace (called from the key path): the window server is the only thing between the user and a dead
 * screen, so give them a way out when it hangs. */
void input_kill_grabber(void) {
    Process *p = g_grab_pid ? process_by_pid(g_grab_pid) : NULL;
    if (p) {
        klog("Eiki", "Ctrl+Alt+Backspace: the window server (pid %d) is being sent to the Netherworld", p->pid);
        signal_send(p, SIGKILL);
    }
}

static const VNodeOps input_ops = { .read = input_read, .poll = input_poll_node, .ioctl = input_ioctl, .opened = input_opened, .closed = input_closed };

static void inputdev_init(void) {
    if (devfs_register("input/events", S_IFCHR | 0660, &input_ops, NULL, 0x0D40) == 0)
        klog("Satori", "/dev/input/events: keyboard and mouse events (Ctrl+Alt+Backspace takes the screen back)");
}
INITCALL(inputdev_init);
