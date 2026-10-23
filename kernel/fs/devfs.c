/* /dev: registration of device nodes plus the standard character devices. */
#include "vfs.h"
#include "kernel.h"
#include "cpu.h"
#include "tty.h"
#include "random.h"
#include "ktime.h"

static VNode *g_devfs_root;

VNode *devfs_root(void) { return g_devfs_root; }

/* devfs is tmpfs underneath; remember the root of the first one mounted. */
static int devfs_mount_hook(const char *source, u32 flags, Mount *m) {
    int rc = g_tmpfs_type.mount(source, flags, m);
    if (rc == 0 && !g_devfs_root) g_devfs_root = m->root;
    return rc;
}

const FsType g_devfs_type = { "devfs", devfs_mount_hook, NULL };

int devfs_register(const char *path, u32 mode, const VNodeOps *ops, void *priv, u64 rdev) {
    if (!g_devfs_root) return -ENODEV;
    VNode *dir = g_devfs_root;
    vnode_get(dir);
    char comp[GNAME_MAX + 1];
    const char *p = path;
    for (;;) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        if (n == 0 || n > GNAME_MAX) { vnode_put(dir); return -EINVAL; }
        memcpy(comp, p, n);
        comp[n] = 0;
        if (!slash) break;
        VNode *sub = NULL;
        int rc = dir->ops->lookup(dir, comp, &sub);
        if (rc == -ENOENT) rc = dir->ops->mkdir(dir, comp, S_IFDIR | 0755, &sub);
        vnode_put(dir);
        if (rc) return rc;
        dir = sub;
        p = slash + 1;
    }
    int rc = tmpfs_add_special(dir, comp, mode, ops, priv, rdev, NULL);
    vnode_put(dir);
    return rc;
}

/* ---- null / zero / full ---- */
static ssize_t null_read(VNode *v, File *f, void *b, size_t n, u64 o) { (void)v; (void)f; (void)b; (void)n; (void)o; return 0; }
static ssize_t null_write(VNode *v, File *f, const void *b, size_t n, u64 o) { (void)v; (void)f; (void)b; (void)o; return (ssize_t)n; }
static ssize_t zero_read(VNode *v, File *f, void *b, size_t n, u64 o) { (void)v; (void)f; (void)o; memset(b, 0, n); return (ssize_t)n; }
static ssize_t random_read(VNode *v, File *f, void *b, size_t n, u64 o) { (void)v; (void)f; (void)o; random_fill(b, n); return (ssize_t)n; }
static ssize_t random_write(VNode *v, File *f, const void *b, size_t n, u64 o) {
    (void)v; (void)f; (void)o;
    const u8 *p = b;
    for (size_t i = 0; i < n; i++) random_mix(p[i]);
    return (ssize_t)n;
}
static const VNodeOps null_ops = { .read = null_read, .write = null_write };
static const VNodeOps zero_ops = { .read = zero_read, .write = null_write };
static const VNodeOps random_ops = { .read = random_read, .write = random_write };

/* ---- console tty ---- */
static ssize_t tty_node_read(VNode *v, File *f, void *b, size_t n, u64 o) {
    (void)v; (void)o;
    return tty_read(tty_console(), b, n, f && (f->flags & O_NONBLOCK));
}
static ssize_t tty_node_write(VNode *v, File *f, const void *b, size_t n, u64 o) {
    (void)v; (void)f; (void)o;
    return tty_write(tty_console(), b, n);
}
static int tty_node_ioctl(VNode *v, File *f, unsigned long req, void *arg) {
    (void)v; (void)f;
    return tty_ioctl(tty_console(), req, arg);
}
static const VNodeOps tty_ops = { .read = tty_node_read, .write = tty_node_write, .ioctl = tty_node_ioctl };

/* ---- kernel log ---- */
static ssize_t kmsg_read(VNode *v, File *f, void *b, size_t n, u64 off) {
    (void)v;
    size_t got = klog_read(f ? f->off : off, b, n);
    if (f) f->off += got;
    return (ssize_t)got;
}
static const VNodeOps kmsg_ops = { .read = kmsg_read };

void devfs_register_standard(void) {
    devfs_register("null", S_IFCHR | 0666, &null_ops, NULL, 0x0103);
    devfs_register("zero", S_IFCHR | 0666, &zero_ops, NULL, 0x0105);
    devfs_register("random", S_IFCHR | 0666, &random_ops, NULL, 0x0108);
    devfs_register("urandom", S_IFCHR | 0666, &random_ops, NULL, 0x0109);
    devfs_register("console", S_IFCHR | 0620, &tty_ops, NULL, 0x0501);
    devfs_register("tty", S_IFCHR | 0666, &tty_ops, NULL, 0x0500);
    devfs_register("kmsg", S_IFCHR | 0644, &kmsg_ops, NULL, 0x010B);
}
