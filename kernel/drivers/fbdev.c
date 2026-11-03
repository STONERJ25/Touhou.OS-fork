/* /dev/fb0: the linear framebuffer for the window server. mmap it (write-combining), query the geometry, take the screen. */
#include "vfs.h"
#include "kernel.h"
#include "fb.h"
#include "fbcon.h"
#include "initcall.h"

static int g_graphics_owners;

static u64 fb_bytes(void) { return (u64)g_fb.pitch * 4 * g_fb.height; }

static ssize_t fbdev_read(VNode *v, File *f, void *buf, size_t n, u64 off) {
    (void)v; (void)f;
    u64 size = fb_bytes();
    if (off >= size) return 0;
    if (n > size - off) n = (size_t)(size - off);
    memcpy(buf, (u8 *)g_fb.front + off, n);
    return (ssize_t)n;
}

static ssize_t fbdev_write(VNode *v, File *f, const void *buf, size_t n, u64 off) {
    (void)v; (void)f;
    u64 size = fb_bytes();
    if (off >= size) return -ENOSPC;
    if (n > size - off) n = (size_t)(size - off);
    memcpy((u8 *)g_fb.front + off, buf, n);
    return (ssize_t)n;
}

static int fbdev_mmap_phys(VNode *v, u64 off, paddr_t *phys) {
    (void)v;
    if (off >= ALIGN_UP(fb_bytes(), PAGE_SIZE)) return -EINVAL;
    *phys = g_fb.phys + off;
    return 0;
}

static void set_graphics(File *f, bool on) {
    bool was = f->priv != NULL;
    if (on == was) return;
    f->priv = on ? (void *)1 : NULL;
    if (on) {
        if (g_graphics_owners++ == 0) fbcon_set_enabled(false);
    } else if (--g_graphics_owners == 0) {
        fbcon_set_enabled(true);
        fbcon_redraw();
    }
}

static int fbdev_ioctl(VNode *v, File *f, unsigned long req, void *arg) {
    (void)v;
    switch (req) {
    case FBIOGET_INFO: {
        struct gfbinfo *i = arg;
        i->width = g_fb.width;
        i->height = g_fb.height;
        i->pitch = g_fb.pitch * 4;
        i->bpp = 32;
        i->flags = g_fb.swap_rb ? FBINFO_SWAP_RB : 0;
        return 0;
    }
    case FBIO_FLUSH: return 0;
    case FBIO_GRAPHICS: set_graphics(f, *(int *)arg != 0); return 0;
    default: return -ENOTTY;
    }
}

static void fbdev_closed(VNode *v, File *f) {
    (void)v;
    set_graphics(f, false);                 /* a window server that dies must not leave the screen black */
}

static const VNodeOps fbdev_ops = {
    .read = fbdev_read, .write = fbdev_write, .ioctl = fbdev_ioctl, .mmap_phys = fbdev_mmap_phys, .closed = fbdev_closed,
};

static void fbdev_init(void) {
    VNode *node = NULL;
    if (!devfs_root() || !g_fb.ready) return;
    VNode *dir = devfs_root();
    vnode_get(dir);
    int rc = tmpfs_add_special(dir, "fb0", S_IFCHR | 0660, &fbdev_ops, NULL, 0x1D00, &node);
    vnode_put(dir);
    if (rc == 0) {
        node->size = fb_bytes();
        node->gid = GID_SHRINE;
        vnode_put(node);
        klog("Fairies", "/dev/fb0: %ux%u, %s, ready for the window server", g_fb.width, g_fb.height, g_fb.swap_rb ? "RGBX" : "BGRX");
    }
}
INITCALL(fbdev_init);
