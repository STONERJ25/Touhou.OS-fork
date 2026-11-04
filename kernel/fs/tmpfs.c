/* tmpfs: RAM-backed filesystem used for the root, /BambooForest (tmp) and devfs. File data lives in 4 KiB pages.
 *
 * Hardening (docs/HARDENING.md): offsets and sizes are limited (TMP_MAX_SIZE), so no arithmetic on an offset can wrap and the table of
 * page pointers cannot grow beyond a few MiB; every allocation is checked and an operation that cannot get its memory leaves the
 * directory as it was; directory listings use stable cookies (a listing continues correctly while its entries are being removed). */
#include "vfs.h"
#include "kernel.h"
#include "mm.h"
#include "cpu.h"

#define TMP_MAX_PAGES (1UL << 19)                          /* 2 GiB of offsets per file: the pointer table is 4 MiB at most */
#define TMP_MAX_SIZE  ((u64)TMP_MAX_PAGES << PAGE_SHIFT)

typedef struct TmpDirent {
    struct TmpDirent *next;
    VNode *node;
    u64 seq;                            /* creation order within the directory: the readdir cookie, stable when other entries go */
    char name[];
} TmpDirent;

typedef struct TmpNode {
    union {
        struct { u8 **pages; u32 cap; } file;
        struct { TmpDirent *head, *tail; u32 count; VNode *parent; u64 seq; } dir;
        struct { char *target; } link;
    };
} TmpNode;

typedef struct TmpFs {
    Mutex lock;
    u64 next_ino;
} TmpFs;

static u64 g_pages_used;
static const VNodeOps tmp_ops;

static u32 dt_from_mode(u32 mode) {
    switch (mode & S_IFMT) {
    case S_IFDIR: return DT_DIR;
    case S_IFREG: return DT_REG;
    case S_IFLNK: return DT_LNK;
    case S_IFCHR: return DT_CHR;
    case S_IFBLK: return DT_BLK;
    case S_IFIFO: return DT_FIFO;
    default:      return DT_UNKNOWN;
    }
}

static TmpFs *fs_of(VNode *v) { return v->mount->priv; }

static VNode *new_node(Mount *m, u32 mode, const VNodeOps *ops) {
    VNode *v = vnode_alloc(ops, m, mode);
    if (!v) return NULL;
    TmpFs *fs = m->priv;
    v->ino = ++fs->next_ino;
    if (ops == &tmp_ops) {
        TmpNode *t = kzalloc(sizeof *t);
        if (!t) { vnode_put(v); return NULL; }       /* nobody else knows the node: this frees it (and the release hook sees no TmpNode) */
        v->priv = t;
    }
    if (S_ISDIR(mode)) v->nlink = 2;
    return v;
}

/* A directory entry is allocated before the operation that needs it changes anything, so running out of memory leaves no half-made file. */
static TmpDirent *dirent_alloc(const char *name) {
    size_t len = strlen(name);
    TmpDirent *e = kmalloc(sizeof *e + len + 1);
    if (!e) return NULL;
    e->next = NULL;
    e->node = NULL;
    memcpy(e->name, name, len + 1);
    return e;
}

static void dirent_attach(VNode *dir, TmpDirent *e, VNode *node) {
    TmpNode *d = dir->priv;
    e->node = node;                     /* the directory entry owns the node's initial reference */
    e->seq = ++d->dir.seq;
    e->next = NULL;
    if (d->dir.tail) d->dir.tail->next = e;
    else d->dir.head = e;
    d->dir.tail = e;
    d->dir.count++;
    dir->mtime = dir->ctime = time_now_sec();
}

static TmpDirent *find_dirent(VNode *dir, const char *name, TmpDirent **prev_out) {
    TmpNode *d = dir->priv;
    TmpDirent *prev = NULL;
    for (TmpDirent *e = d->dir.head; e; prev = e, e = e->next) {
        if (!strcmp(e->name, name)) {
            if (prev_out) *prev_out = prev;
            return e;
        }
    }
    return NULL;
}

static void unlink_dirent(VNode *dir, TmpDirent *e, TmpDirent *prev) {
    TmpNode *d = dir->priv;
    if (prev) prev->next = e->next;
    else d->dir.head = e->next;
    if (d->dir.tail == e) d->dir.tail = prev;
    d->dir.count--;
    dir->mtime = dir->ctime = time_now_sec();
}

static int tmp_lookup(VNode *dir, const char *name, VNode **out) {
    TmpNode *d = dir->priv;
    if (!strcmp(name, ".")) { vnode_get(dir); *out = dir; return 0; }
    if (!strcmp(name, "..")) {
        VNode *p = d->dir.parent ? d->dir.parent : dir;
        vnode_get(p);
        *out = p;
        return 0;
    }
    TmpFs *fs = fs_of(dir);
    mutex_lock(&fs->lock);
    TmpDirent *e = find_dirent(dir, name, NULL);
    if (e) vnode_get(e->node);
    mutex_unlock(&fs->lock);
    if (!e) return -ENOENT;
    *out = e->node;
    return 0;
}

static int check_new_name(VNode *dir, const char *name) {
    if (!strcmp(name, ".") || !strcmp(name, "..")) return -EEXIST;
    if (find_dirent(dir, name, NULL)) return -EEXIST;
    return 0;
}

static int tmp_create(VNode *dir, const char *name, u32 mode, VNode **out) {
    TmpFs *fs = fs_of(dir);
    mutex_lock(&fs->lock);
    int rc = check_new_name(dir, name);
    if (rc) { mutex_unlock(&fs->lock); return rc; }
    TmpDirent *e = dirent_alloc(name);
    VNode *v = e ? new_node(dir->mount, (mode & S_IFMT) ? mode : (S_IFREG | mode), &tmp_ops) : NULL;
    if (!v) { kfree(e); mutex_unlock(&fs->lock); return -ENOMEM; }
    dirent_attach(dir, e, v);
    vnode_get(v);
    mutex_unlock(&fs->lock);
    *out = v;
    return 0;
}

static int tmp_mkdir(VNode *dir, const char *name, u32 mode, VNode **out) {
    TmpFs *fs = fs_of(dir);
    mutex_lock(&fs->lock);
    int rc = check_new_name(dir, name);
    if (rc) { mutex_unlock(&fs->lock); return rc; }
    TmpDirent *e = dirent_alloc(name);
    VNode *v = e ? new_node(dir->mount, S_IFDIR | (mode & 07777), &tmp_ops) : NULL;
    if (!v) { kfree(e); mutex_unlock(&fs->lock); return -ENOMEM; }
    ((TmpNode *)v->priv)->dir.parent = dir;
    dir->nlink++;
    dirent_attach(dir, e, v);
    vnode_get(v);
    mutex_unlock(&fs->lock);
    *out = v;
    return 0;
}

static int tmp_symlink(VNode *dir, const char *name, const char *target, VNode **out) {
    TmpFs *fs = fs_of(dir);
    mutex_lock(&fs->lock);
    int rc = check_new_name(dir, name);
    if (rc) { mutex_unlock(&fs->lock); return rc; }
    size_t len = strlen(target);
    TmpDirent *e = dirent_alloc(name);
    char *t = e ? kmalloc(len + 1) : NULL;
    VNode *v = t ? new_node(dir->mount, S_IFLNK | 0777, &tmp_ops) : NULL;
    if (!v) { kfree(t); kfree(e); mutex_unlock(&fs->lock); return -ENOMEM; }
    memcpy(t, target, len + 1);
    ((TmpNode *)v->priv)->link.target = t;
    v->size = len;
    dirent_attach(dir, e, v);
    vnode_get(v);
    mutex_unlock(&fs->lock);
    *out = v;
    return 0;
}

/* A hard link: one more directory entry for the same node. Every entry owns one reference to it, nlink counts the entries. */
static int tmp_link(VNode *dir, const char *name, VNode *node) {
    TmpFs *fs = fs_of(dir);
    mutex_lock(&fs->lock);
    int rc = check_new_name(dir, name);
    if (rc) { mutex_unlock(&fs->lock); return rc; }
    if (S_ISDIR(node->mode)) { mutex_unlock(&fs->lock); return -EPERM; }
    if (node->nlink >= 65000) { mutex_unlock(&fs->lock); return -EMLINK; }
    TmpDirent *e = dirent_alloc(name);
    if (!e) { mutex_unlock(&fs->lock); return -ENOMEM; }
    vnode_get(node);
    node->nlink++;
    dirent_attach(dir, e, node);
    mutex_unlock(&fs->lock);
    return 0;
}

static int tmp_unlink(VNode *dir, const char *name) {
    TmpFs *fs = fs_of(dir);
    mutex_lock(&fs->lock);
    TmpDirent *prev, *e = find_dirent(dir, name, &prev);
    if (!e) { mutex_unlock(&fs->lock); return -ENOENT; }
    if (S_ISDIR(e->node->mode)) { mutex_unlock(&fs->lock); return -EISDIR; }
    unlink_dirent(dir, e, prev);
    VNode *v = e->node;
    kfree(e);
    if (v->nlink) v->nlink--;
    mutex_unlock(&fs->lock);
    vnode_put(v);                       /* the directory entry's reference */
    return 0;
}

static int tmp_rmdir(VNode *dir, const char *name) {
    TmpFs *fs = fs_of(dir);
    mutex_lock(&fs->lock);
    TmpDirent *prev, *e = find_dirent(dir, name, &prev);
    if (!e) { mutex_unlock(&fs->lock); return -ENOENT; }
    VNode *v = e->node;
    if (!S_ISDIR(v->mode)) { mutex_unlock(&fs->lock); return -ENOTDIR; }
    if (v->mounted) { mutex_unlock(&fs->lock); return -EBUSY; }
    TmpNode *t = v->priv;
    if (t->dir.count) { mutex_unlock(&fs->lock); return -ENOTEMPTY; }
    unlink_dirent(dir, e, prev);
    kfree(e);
    t->dir.parent = v;
    v->nlink = 0;
    if (dir->nlink > 2) dir->nlink--;
    mutex_unlock(&fs->lock);
    vnode_put(v);
    return 0;
}

static int tmp_rename(VNode *od, const char *on, VNode *nd, const char *nn) {
    TmpFs *fs = fs_of(od);
    mutex_lock(&fs->lock);
    TmpDirent *oprev, *oe = find_dirent(od, on, &oprev);
    if (!oe) { mutex_unlock(&fs->lock); return -ENOENT; }
    if (od == nd && !strcmp(on, nn)) { mutex_unlock(&fs->lock); return 0; }
    if (!strcmp(nn, ".") || !strcmp(nn, "..")) { mutex_unlock(&fs->lock); return -EINVAL; }
    VNode *src = oe->node;

    if (S_ISDIR(src->mode) && od != nd) {                   /* never move a directory below itself */
        for (VNode *p = nd; p; ) {
            if (p == src) { mutex_unlock(&fs->lock); return -EINVAL; }
            VNode *up = ((TmpNode *)p->priv)->dir.parent;
            if (!up || up == p) break;
            p = up;
        }
    }

    TmpDirent *nprev, *ne = find_dirent(nd, nn, &nprev);
    if (ne) {
        VNode *dst = ne->node;
        if (dst == src) { mutex_unlock(&fs->lock); return 0; }
        if (S_ISDIR(dst->mode)) {
            if (!S_ISDIR(src->mode)) { mutex_unlock(&fs->lock); return -EISDIR; }
            if (((TmpNode *)dst->priv)->dir.count) { mutex_unlock(&fs->lock); return -ENOTEMPTY; }
        } else if (S_ISDIR(src->mode)) {
            mutex_unlock(&fs->lock);
            return -ENOTDIR;
        }
    }
    TmpDirent *fresh = dirent_alloc(nn);                    /* the entry for the new name, made before anything is changed */
    if (!fresh) { mutex_unlock(&fs->lock); return -ENOMEM; }

    if (ne) {
        VNode *dst = ne->node;
        if (S_ISDIR(dst->mode)) {
            if (nd->nlink > 2) nd->nlink--;
            ((TmpNode *)dst->priv)->dir.parent = dst;
            dst->nlink = 0;
        } else if (dst->nlink) {
            dst->nlink--;
        }
        unlink_dirent(nd, ne, nprev);
        kfree(ne);
        vnode_put(dst);
        if (find_dirent(od, on, &oprev) != oe) { kfree(fresh); mutex_unlock(&fs->lock); return -EIO; }
    }

    unlink_dirent(od, oe, oprev);
    kfree(oe);
    dirent_attach(nd, fresh, src);      /* the entry keeps the node's reference */
    if (S_ISDIR(src->mode)) {
        ((TmpNode *)src->priv)->dir.parent = nd;
        if (od != nd) {
            if (od->nlink > 2) od->nlink--;
            nd->nlink++;
        }
    }
    mutex_unlock(&fs->lock);
    return 0;
}

static int tmp_readdir(VNode *dir, u64 cookie, DirEntry *out) {
    TmpFs *fs = fs_of(dir);
    mutex_lock(&fs->lock);
    TmpDirent *e = ((TmpNode *)dir->priv)->dir.head;
    while (e && e->seq <= cookie) e = e->next;      /* the list is in creation order: skip what the caller has seen */
    int rc = 0;
    if (e) {
        out->next = e->seq;
        out->ino = e->node->ino;
        out->type = dt_from_mode(e->node->mode);
        strlcpy(out->name, e->name, sizeof out->name);
        rc = 1;
    }
    mutex_unlock(&fs->lock);
    return rc;
}

static ssize_t tmp_read(VNode *v, File *f, void *buf, size_t n, u64 off) {
    (void)f;
    TmpNode *t = v->priv;
    TmpFs *fs = fs_of(v);
    mutex_lock(&fs->lock);
    if (off >= v->size) { mutex_unlock(&fs->lock); return 0; }
    if (n > v->size - off) n = (size_t)(v->size - off);
    size_t done = 0;
    while (done < n) {
        u64 pos = off + done;
        u64 page = pos >> PAGE_SHIFT;
        size_t po = (size_t)(pos & (PAGE_SIZE - 1));
        size_t chunk = MIN(n - done, PAGE_SIZE - po);
        u8 *pg = page < t->file.cap ? t->file.pages[page] : NULL;
        if (pg) memcpy((u8 *)buf + done, pg + po, chunk);
        else memset((u8 *)buf + done, 0, chunk);
        done += chunk;
    }
    mutex_unlock(&fs->lock);
    return (ssize_t)done;
}

/* Grows the pointer table to hold `need` pages (need <= TMP_MAX_PAGES). fs->lock held. */
static int grow_table(TmpNode *t, u64 need) {
    if (need <= t->file.cap) return 0;
    u64 ncap = MAX(need, (u64)t->file.cap * 2);
    if (ncap > TMP_MAX_PAGES) ncap = TMP_MAX_PAGES;
    u8 **np = krealloc(t->file.pages, (size_t)ncap * sizeof(u8 *));
    if (!np) return -ENOMEM;
    memset(np + t->file.cap, 0, (size_t)(ncap - t->file.cap) * sizeof(u8 *));
    t->file.pages = np;
    t->file.cap = (u32)ncap;
    return 0;
}

static ssize_t tmp_write(VNode *v, File *f, const void *buf, size_t n, u64 off) {
    (void)f;
    TmpNode *t = v->priv;
    TmpFs *fs = fs_of(v);
    if (!n) return 0;
    if (off >= TMP_MAX_SIZE) return -EFBIG;
    if (n > TMP_MAX_SIZE - off) n = (size_t)(TMP_MAX_SIZE - off);          /* what fits below the limit is written, as with a full disk */
    mutex_lock(&fs->lock);
    u64 end = off + n;
    int rc = grow_table(t, (end + PAGE_SIZE - 1) >> PAGE_SHIFT);
    if (rc) { mutex_unlock(&fs->lock); return rc; }
    size_t done = 0;
    while (done < n) {
        u64 pos = off + done;
        u64 page = pos >> PAGE_SHIFT;
        size_t po = (size_t)(pos & (PAGE_SIZE - 1));
        size_t chunk = MIN(n - done, PAGE_SIZE - po);
        if (!t->file.pages[page]) {
            if (g_pages_used > pmm_total_pages() / 2) break;        /* keep half of RAM for everything else */
            paddr_t p = pmm_alloc_page_zeroed();
            if (!p) break;
            t->file.pages[page] = phys_to_virt(p);
            g_pages_used++;
        }
        memcpy(t->file.pages[page] + po, (const u8 *)buf + done, chunk);
        done += chunk;
    }
    if (off + done > v->size) v->size = off + done;
    v->mtime = v->ctime = time_now_sec();
    mutex_unlock(&fs->lock);
    return done ? (ssize_t)done : -ENOSPC;
}

static int tmp_truncate(VNode *v, u64 size) {
    TmpNode *t = v->priv;
    TmpFs *fs = fs_of(v);
    if (size > TMP_MAX_SIZE) return -EFBIG;
    mutex_lock(&fs->lock);
    if (size < v->size) {
        u64 keep = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
        for (u64 i = keep; i < t->file.cap; i++) {
            if (t->file.pages[i]) {
                pmm_free_page(virt_to_phys_hhdm(t->file.pages[i]));
                t->file.pages[i] = NULL;
                g_pages_used--;
            }
        }
        if (size & (PAGE_SIZE - 1) && (size >> PAGE_SHIFT) < t->file.cap && t->file.pages[size >> PAGE_SHIFT])
            memset(t->file.pages[size >> PAGE_SHIFT] + (size & (PAGE_SIZE - 1)), 0, PAGE_SIZE - (size & (PAGE_SIZE - 1)));
    }
    v->size = size;
    v->mtime = v->ctime = time_now_sec();
    mutex_unlock(&fs->lock);
    return 0;
}

/* Shared mappings of a file: hand out the file's own pages (allocated on demand, zero-filled). The mapping takes a page
 * reference, so truncating or deleting the file while it is mapped cannot pull the memory out from under the mapper. */
static int tmp_mmap_phys(VNode *v, u64 off, paddr_t *phys) {
    if (!S_ISREG(v->mode)) return -ENODEV;
    TmpNode *t = v->priv;
    TmpFs *fs = fs_of(v);
    int rc = 0;
    mutex_lock(&fs->lock);
    u64 idx = off >> PAGE_SHIFT;
    if (off >= ALIGN_UP(v->size, PAGE_SIZE) || idx >= TMP_MAX_PAGES) rc = -EINVAL;
    else {
        rc = grow_table(t, idx + 1);
        if (!rc && !t->file.pages[idx]) {
            paddr_t p = g_pages_used > pmm_total_pages() / 2 ? 0 : pmm_alloc_page_zeroed();
            if (!p) rc = -ENOMEM;
            else { t->file.pages[idx] = phys_to_virt(p); g_pages_used++; }
        }
        if (!rc) *phys = virt_to_phys_hhdm(t->file.pages[idx]);
    }
    mutex_unlock(&fs->lock);
    return rc;
}

static ssize_t tmp_readlink(VNode *v, char *buf, size_t n) {
    TmpNode *t = v->priv;
    size_t len = MIN(strlen(t->link.target), n);
    memcpy(buf, t->link.target, len);
    return (ssize_t)len;
}

static void tmp_release(VNode *v) {
    TmpNode *t = v->priv;
    if (!t) return;
    if (S_ISREG(v->mode)) {
        for (u32 i = 0; i < t->file.cap; i++)
            if (t->file.pages[i]) { pmm_free_page(virt_to_phys_hhdm(t->file.pages[i])); g_pages_used--; }
        kfree(t->file.pages);
    } else if (S_ISLNK(v->mode)) {
        kfree(t->link.target);
    }
    kfree(t);
}

static const VNodeOps tmp_ops = {
    .read = tmp_read, .write = tmp_write, .truncate = tmp_truncate, .mmap_phys = tmp_mmap_phys,
    .lookup = tmp_lookup, .create = tmp_create, .mkdir = tmp_mkdir, .symlink = tmp_symlink,
    .link = tmp_link, .unlink = tmp_unlink, .rmdir = tmp_rmdir, .rename = tmp_rename, .readdir = tmp_readdir,
    .readlink = tmp_readlink, .release = tmp_release,
};

int tmpfs_add_special(VNode *dir, const char *name, u32 mode, const VNodeOps *ops, void *priv, u64 rdev, VNode **out) {
    TmpFs *fs = fs_of(dir);
    mutex_lock(&fs->lock);
    int rc = check_new_name(dir, name);
    if (rc) { mutex_unlock(&fs->lock); return rc; }
    TmpDirent *e = dirent_alloc(name);
    VNode *v = e ? new_node(dir->mount, mode, ops) : NULL;
    if (!v) { kfree(e); mutex_unlock(&fs->lock); return -ENOMEM; }
    v->priv = priv;
    v->rdev = rdev;
    dirent_attach(dir, e, v);
    if (out) { vnode_get(v); *out = v; }
    mutex_unlock(&fs->lock);
    return 0;
}

static int tmpfs_mount(const char *source, u32 flags, Mount *m) {
    (void)source; (void)flags;
    TmpFs *fs = kzalloc(sizeof *fs);
    if (!fs) return -ENOMEM;
    mutex_init(&fs->lock);
    m->priv = fs;
    VNode *root = new_node(m, S_IFDIR | 0755, &tmp_ops);
    if (!root) { kfree(fs); return -ENOMEM; }
    ((TmpNode *)root->priv)->dir.parent = root;
    m->root = root;
    return 0;
}

static void tmpfs_unmount(Mount *m) { kfree(m->priv); }

void tmpfs_stats(u64 *pages_used) { *pages_used = g_pages_used; }

/* All tmpfs instances draw from one pool (half of RAM, see tmp_write), so every mount reports the shared figures. */
static int tmpfs_statfs(Mount *m, struct gstatfs *out) {
    (void)m;
    u64 total = pmm_total_pages() / 2;
    out->f_type = 0x01021994;
    out->f_bsize = PAGE_SIZE;
    out->f_blocks = total;
    out->f_bfree = out->f_bavail = g_pages_used < total ? total - g_pages_used : 0;
    return 0;
}

const FsType g_tmpfs_type = { "tmpfs", tmpfs_mount, tmpfs_unmount, tmpfs_statfs };
