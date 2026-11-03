/*
 * NTFS for Gensokyo, read only: the VFS side. The parser (ntfs_core.c) knows the format; this file turns it into vnodes, mounts and unmounts it, and keeps the
 * promises of docs/NTFS.md:
 *
 *   - read only, for ever: the vnode operations of a directory or file are lookup, readdir, read and readlink; every operation that would change something is
 *     answered with -EROFS and nothing here has a path to the device that writes. `-o rw` is refused with a log line rather than quietly ignored;
 *   - nothing mounts by itself: the mount policy (mountpolicy.c) never reads NTFS volumes, and this type is only used when root says `mount -t ntfs`;
 *   - a BitLocker volume with a clear-key protector is unlocked on the way in (bitlocker.c); the decrypted view is a block device (bdeN) for as long as the
 *     mount lasts, and what the parser reads is that view;
 *   - one vnode per file (found again through a small hash by MFT number), so that a mount point on a directory, ".." at the volume's root and the open files
 *     of a process all agree on what is the same file.
 *
 * Locking: one mutex per mount covers every call into the parser (it has caches and cursors). Releasing a vnode is lock-free (the hash is protected by turning
 * interrupts off, as fat32.c does), because vnode_put() can run while the mutex is held.
 */
#include "ntfs_priv.h"
#include "ntfs.h"
#include "bitlocker.h"
#include "vfs.h"
#include "blk.h"
#include "cpu.h"
#include "initcall.h"

#define NT_HASH 64
#define NT_MAX_MOUNTS 8

typedef struct NtfsMnt NtfsMnt;

typedef struct NtfsInode {
    struct NtfsInode *next;                     /* hash chain */
    NtfsMnt *mnt;
    NtfsFile *f;
    VNode *vn;
    NtfsDirCursor cur;                          /* directories: where the last readdir stopped */
    u64 cur_cookie;                             /* how many visible entries the cursor has returned */
    bool cur_valid;
} NtfsInode;

struct NtfsMnt {
    Mutex lock;
    NtfsVol *vol;
    BlockDev *dev;                              /* the device the user named */
    BlockDev *src;                              /* what the parser reads: the same device, or the BitLocker view of it */
    BdeVol *bde;
    Mount *mount;
    NtfsInode *hash[NT_HASH];
    int nlive;                                  /* live vnodes, the root's included */
    u32 uid, gid, fmask, dmask;
    u32 skipped;                                /* names too long for the VFS that were left out (logged once) */
    bool skip_logged;
};

static NtfsMnt *g_mnts[NT_MAX_MOUNTS];

static const VNodeOps ntfs_dir_ops, ntfs_file_ops, ntfs_link_ops;

/* ---------------------------------------------------------------------------------------------- the device */

/* The parser's read callback over a block device: any byte range, through a bounce buffer for the ragged ends. */
int ntfs_blk_io(void *ctx, u64 off, void *buf, size_t len) {
    BlockDev *d = ctx;
    u32 ss = d->sector_size;
    u8 *out = buf;
    if (!len) return 0;
    if (off % ss == 0 && len % ss == 0) {
        while (len) {
            size_t n = len > (1u << 20) ? (1u << 20) : len;
            int rc = blk_read(d, off / ss, (u32)(n / ss), out);
            if (rc) return rc;
            off += n;
            out += n;
            len -= n;
        }
        return 0;
    }
    size_t cap = 65536 + 2 * (size_t)ss;
    u8 *bb = kmalloc(cap);
    if (!bb) return -ENOMEM;
    int rc = 0;
    while (len && !rc) {
        u64 start = off & ~(u64)(ss - 1);
        size_t head = (size_t)(off - start);
        size_t take = len < 65536 ? len : 65536;
        u64 end = (off + take + ss - 1) & ~(u64)(ss - 1);
        rc = blk_read(d, start / ss, (u32)((end - start) / ss), bb);
        if (rc) break;
        memcpy(out, bb + head, take);
        out += take;
        off += take;
        len -= take;
    }
    kfree(bb);
    return rc;
}

/* ---------------------------------------------------------------------------------------------- vnodes */

static u32 node_mode(const NtfsMnt *m, const NtfsFile *f) {
    if (ntfs_is_link(f)) return S_IFLNK | 0777;
    if (f->is_dir) return S_IFDIR | (0777 & ~m->dmask);
    return S_IFREG | (0777 & ~m->fmask);
}

static int iget(NtfsMnt *m, u64 ref, VNode **out) {
    u64 no = ref & NT_REF_MASK;
    u32 seq = (u32)(ref >> 48);
    u32 h = (u32)(no % NT_HASH);
    u64 fl = irq_save();
    for (NtfsInode *i = m->hash[h]; i; i = i->next) {
        if (i->f->no != no || !i->vn || i->vn->refs <= 0) continue;     /* a vnode on its way out is not reused */
        if (seq && seq != i->f->seq) { irq_restore(fl); return -ENOENT; }
        i->vn->refs++;
        irq_restore(fl);
        *out = i->vn;
        return 0;
    }
    irq_restore(fl);
    NtfsFile *f;
    int rc = ntfs_file_open(m->vol, ref, &f);
    if (rc) return rc;
    NtfsInode *ni = kzalloc(sizeof *ni);
    if (!ni) { ntfs_file_close(f); return -ENOMEM; }
    u32 mode = node_mode(m, f);
    VNode *vn = vnode_alloc(S_ISDIR(mode) ? &ntfs_dir_ops : S_ISLNK(mode) ? &ntfs_link_ops : &ntfs_file_ops, m->mount, mode);
    if (!vn) { kfree(ni); ntfs_file_close(f); return -ENOMEM; }
    ni->mnt = m;
    ni->f = f;
    ni->vn = vn;
    vn->priv = ni;
    vn->ino = f->no;
    vn->uid = m->uid;
    vn->gid = m->gid;
    vn->nlink = S_ISDIR(mode) ? 2 : S_ISLNK(mode) ? 1 : f->nlink;
    vn->atime = f->atime;
    vn->mtime = f->mtime;
    vn->ctime = f->ctime;
    if (S_ISLNK(mode)) {
        char *tgt = kmalloc(PATH_MAX);
        int n = tgt ? ntfs_readlink(f, m->mount->target, tgt, PATH_MAX) : -ENOMEM;
        vn->size = n > 0 ? (u64)n : 0;
        kfree(tgt);
    } else if (f->is_dir) vn->size = f->ialloc.present ? f->ialloc.size : 4096;
    else vn->size = f->data.present ? f->data.size : 0;
    fl = irq_save();
    ni->next = m->hash[h];
    m->hash[h] = ni;
    m->nlive++;
    irq_restore(fl);
    *out = vn;
    return 0;
}

/* Last reference dropped. Lock-free: the hash is guarded by interrupts being off, the parser's memory is simply freed. */
static void ntfs_release(VNode *vn) {
    NtfsInode *ni = vn->priv;
    if (!ni) return;                                                    /* the root after the unmount */
    NtfsMnt *m = ni->mnt;
    u32 h = (u32)(ni->f->no % NT_HASH);
    u64 fl = irq_save();
    for (NtfsInode **pp = &m->hash[h]; *pp; pp = &(*pp)->next)
        if (*pp == ni) { *pp = ni->next; break; }
    m->nlive--;
    irq_restore(fl);
    vn->priv = NULL;
    ntfs_file_close(ni->f);
    kfree(ni);
}

/* ---------------------------------------------------------------------------------------------- reading */

static int ntfs_lookup(VNode *dir, const char *name, VNode **out) {
    NtfsInode *di = dir->priv;
    NtfsMnt *m = di->mnt;
    if (!strcmp(name, ".")) { vnode_get(dir); *out = dir; return 0; }
    mutex_lock(&m->lock);
    int rc;
    u64 ref;
    if (!strcmp(name, "..")) {
        rc = ntfs_parent_of(di->f, &ref);
        if (!rc) rc = iget(m, ref, out);
    } else {
        rc = ntfs_dir_lookup(di->f, name, &ref);
        if (!rc) rc = iget(m, ref, out);
    }
    mutex_unlock(&m->lock);
    return rc;
}

static int ntfs_readdir(VNode *dir, u64 cookie, DirEntry *out) {
    NtfsInode *di = dir->priv;
    NtfsMnt *m = di->mnt;
    mutex_lock(&m->lock);
    NtfsVisible vis;
    u32 skipped = 0;
    int rc = 0;
    if (!di->cur_valid || di->cur_cookie != cookie) {                   /* a seek, or the first call: walk again from the start */
        ntfs_dir_cursor_reset(&di->cur);
        di->cur_cookie = 0;
        di->cur_valid = true;
        while (di->cur_cookie < cookie) {
            rc = ntfs_dir_next_visible(di->f, &di->cur, &vis, &skipped);
            if (rc <= 0) break;
            di->cur_cookie++;
        }
        if (rc < 0) di->cur_valid = false;
    }
    if (rc > 0 || (rc == 0 && di->cur_cookie == cookie)) {
        rc = ntfs_dir_next_visible(di->f, &di->cur, &vis, &skipped);
        if (rc > 0) {
            out->ino = vis.ino;
            out->type = vis.type;
            strlcpy(out->name, vis.name, sizeof out->name);
            di->cur_cookie++;
        } else if (rc < 0) {
            di->cur_valid = false;
        }
    }
    if (skipped) {
        m->skipped += skipped;
        if (!m->skip_logged) {
            m->skip_logged = true;
            klog("Keine", "ntfs: %s: a name longer than %d bytes of UTF-8 is not shown (this system cannot carry it)", m->mount->target, NT_NAME_MAX);
        }
    }
    mutex_unlock(&m->lock);
    return rc;
}

static ssize_t ntfs_vread(VNode *vn, File *fl, void *buf, size_t n, u64 off) {
    (void)fl;
    NtfsInode *ni = vn->priv;
    NtfsMnt *m = ni->mnt;
    mutex_lock(&m->lock);
    ssize_t r = ntfs_read(ni->f, off, buf, n);
    mutex_unlock(&m->lock);
    return r;
}

static ssize_t ntfs_vreadlink(VNode *vn, char *buf, size_t n) {
    NtfsInode *ni = vn->priv;
    NtfsMnt *m = ni->mnt;
    char *tgt = kmalloc(PATH_MAX);
    if (!tgt) return -ENOMEM;
    mutex_lock(&m->lock);
    int len = ntfs_readlink(ni->f, m->mount->target, tgt, PATH_MAX);
    mutex_unlock(&m->lock);
    if (len >= 0) {
        size_t c = (size_t)len < n ? (size_t)len : n;
        memcpy(buf, tgt, c);
        len = (int)c;
    }
    kfree(tgt);
    return len;
}

/* ---------------------------------------------------------------------------------------------- refusals: this driver cannot write, and says so */

static ssize_t ntfs_nowrite(VNode *v, File *f, const void *buf, size_t n, u64 off) { (void)v; (void)f; (void)buf; (void)n; (void)off; return -EROFS; }
static int ntfs_notruncate(VNode *v, u64 size) { (void)v; (void)size; return -EROFS; }
static int ntfs_nocreate(VNode *d, const char *n, u32 mode, VNode **out) { (void)d; (void)n; (void)mode; (void)out; return -EROFS; }
static int ntfs_nosymlink(VNode *d, const char *n, const char *t, VNode **out) { (void)d; (void)n; (void)t; (void)out; return -EROFS; }
static int ntfs_nolink(VNode *d, const char *n, VNode *t) { (void)d; (void)n; (void)t; return -EROFS; }
static int ntfs_nounlink(VNode *d, const char *n) { (void)d; (void)n; return -EROFS; }
static int ntfs_norename(VNode *od, const char *on, VNode *nd, const char *nn) { (void)od; (void)on; (void)nd; (void)nn; return -EROFS; }
static int ntfs_nochmod(VNode *v, u32 mode) { (void)v; (void)mode; return -EROFS; }
static int ntfs_nochown(VNode *v, u32 uid, u32 gid) { (void)v; (void)uid; (void)gid; return -EROFS; }
static int ntfs_notimes(VNode *v, u64 a, u64 m) { (void)v; (void)a; (void)m; return -EROFS; }

static const VNodeOps ntfs_dir_ops = {
    .lookup = ntfs_lookup, .readdir = ntfs_readdir, .release = ntfs_release,
    .create = ntfs_nocreate, .mkdir = ntfs_nocreate, .symlink = ntfs_nosymlink, .link = ntfs_nolink, .unlink = ntfs_nounlink, .rmdir = ntfs_nounlink,
    .rename = ntfs_norename, .chmod = ntfs_nochmod, .chown = ntfs_nochown, .settimes = ntfs_notimes,
};

static const VNodeOps ntfs_file_ops = {
    .read = ntfs_vread, .release = ntfs_release,
    .write = ntfs_nowrite, .truncate = ntfs_notruncate, .chmod = ntfs_nochmod, .chown = ntfs_nochown, .settimes = ntfs_notimes,
};

static const VNodeOps ntfs_link_ops = {
    .readlink = ntfs_vreadlink, .release = ntfs_release, .chmod = ntfs_nochmod, .chown = ntfs_nochown, .settimes = ntfs_notimes,
};

/* ---------------------------------------------------------------------------------------------- mount options */

typedef struct { bool ro, rw, show_system, show_dos, nobde; u32 uid, gid, fmask, dmask; } Opts;

static u32 parse_oct(const char *s, const char **end, int base) {
    u32 v = 0;
    while (*s >= '0' && *s < '0' + base) v = v * (u32)base + (u32)(*s++ - '0');
    *end = s;
    return v;
}

static int parse_opts(const char *s, Opts *o) {
    o->fmask = 0333;                                                    /* files 0444 */
    o->dmask = 0222;                                                    /* directories 0555 */
    char tok[80];
    while (*s) {
        size_t n = 0;
        while (s[n] && s[n] != ',') n++;
        if (n >= sizeof tok) return -EINVAL;
        memcpy(tok, s, n);
        tok[n] = 0;
        s += n + (s[n] == ',');
        if (!tok[0]) continue;
        const char *eq = strchr(tok, '='), *end;
        const char *val = eq ? eq + 1 : "";
        size_t klen = eq ? (size_t)(eq - tok) : strlen(tok);
        #define KEY(k) (klen == sizeof(k) - 1 && !memcmp(tok, k, klen))
        if (KEY("ro")) o->ro = true;
        else if (KEY("rw")) o->rw = true;
        else if (KEY("sysfiles")) o->show_system = true;
        else if (KEY("showdos")) o->show_dos = true;
        else if (KEY("nobitlocker")) o->nobde = true;
        else if (KEY("uid") && eq) { o->uid = parse_oct(val, &end, 10); if (*end) return -EINVAL; }
        else if (KEY("gid") && eq) { o->gid = parse_oct(val, &end, 10); if (*end) return -EINVAL; }
        else if (KEY("fmask") && eq) { o->fmask = parse_oct(val, &end, 8) & 0777; if (*end) return -EINVAL; }
        else if (KEY("dmask") && eq) { o->dmask = parse_oct(val, &end, 8) & 0777; if (*end) return -EINVAL; }
        else if (KEY("umask") && eq) { o->fmask = o->dmask = parse_oct(val, &end, 8) & 0777; if (*end) return -EINVAL; }
        else if (KEY("defaults") || KEY("noatime") || KEY("relatime") || KEY("nodev") || KEY("nosuid") || KEY("noexec") || KEY("exec") || KEY("suid") ||
                 KEY("dev") || KEY("auto") || KEY("noauto") || KEY("user") || KEY("async") || KEY("quiet") || KEY("utf8") || KEY("windows_names")) { }
        else {
            klog("Keine", "ntfs: unknown mount option '%s'", tok);
            return -EINVAL;
        }
        #undef KEY
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------------- mount and unmount */

static void root_of_device(const BlockDev *d, const BlockDev **root, u64 *start) {
    u64 s = 0;
    while (d->parent) { s += d->part_start; d = d->parent; }
    *root = d;
    *start = s;
}

/* Two mounts must never cover the same sectors (a partition and its BitLocker view, a disk and one of its partitions). Mounts are serialised by the VFS. */
static bool device_in_use(const BlockDev *dev) {
    const BlockDev *r1;
    u64 s1;
    root_of_device(dev, &r1, &s1);
    for (int i = 0; i < NT_MAX_MOUNTS; i++) {
        NtfsMnt *o = g_mnts[i];
        if (!o) continue;
        const BlockDev *r2;
        u64 s2;
        root_of_device(o->dev, &r2, &s2);
        if (r1 == r2 && s1 < s2 + o->dev->sectors && s2 < s1 + dev->sectors) return true;
    }
    return false;
}

static int ntfs_mount(const char *source, u32 flags, Mount *mt) {
    (void)flags;
    Opts o = { 0 };
    int rc = parse_opts(mt->opts, &o);
    if (rc) return rc;
    if (o.rw) {
        klog("Keine", "ntfs: refusing a read-write mount of %s: this driver only reads, and never will write (drop the 'rw' option)", source);
        return -EROFS;
    }
    BlockDev *dev = blk_find(source);
    if (!dev || dev->removed) return -ENODEV;
    if (device_in_use(dev)) return -EBUSY;
    int slot = -1;
    for (int i = 0; i < NT_MAX_MOUNTS && slot < 0; i++) if (!g_mnts[i]) slot = i;
    if (slot < 0) return -ENOMEM;

    NtfsMnt *m = kzalloc(sizeof *m);
    if (!m) return -ENOMEM;
    mutex_init(&m->lock);
    m->dev = dev;
    m->src = dev;
    m->mount = mt;
    m->uid = o.uid;
    m->gid = o.gid;
    m->fmask = o.fmask;
    m->dmask = o.dmask;

    u8 *s0 = kmalloc(dev->sector_size < 512 ? 512 : dev->sector_size);
    if (!s0) { kfree(m); return -ENOMEM; }
    rc = blk_read(dev, 0, 1, s0);
    if (rc) { klog("Keine", "ntfs: cannot read %s (%d)", dev->name, rc); kfree(s0); kfree(m); return rc; }
    bool locked = bde_is_bitlocker(s0);
    kfree(s0);
    char why[200];
    why[0] = 0;
    if (locked) {
        if (o.nobde) { klog("Keine", "ntfs: %s is a BitLocker volume and 'nobitlocker' was given", dev->name); kfree(m); return -EACCES; }
        rc = bde_attach(dev, &m->bde, &m->src, why, sizeof why);
        if (rc) { klog("Keine", "ntfs: %s: %s", dev->name, why); kfree(m); return rc; }
    }

    NtfsDev nd = { ntfs_blk_io, m->src, m->src->sectors * m->src->sector_size, m->src->sector_size };
    NtfsOpts no = { .show_system = o.show_system, .show_dos = o.show_dos };
    rc = ntfs_vol_open(&m->vol, &nd, &no, why, sizeof why);
    if (rc) {
        klog("Keine", "ntfs: %s%s%s is not a mountable NTFS volume: %s (%d)", dev->name, locked ? " (decrypted as " : "", locked ? m->src->name : "",
             why[0] ? why : "unreadable", rc);
        if (m->bde) bde_detach(m->bde, m->src);
        kfree(m);
        return rc < 0 ? rc : -EIO;
    }
    mt->priv = m;
    mt->flags |= MNT_RDONLY;
    VNode *root;
    mutex_lock(&m->lock);
    rc = iget(m, 5, &root);
    mutex_unlock(&m->lock);
    if (rc || !S_ISDIR(root->mode)) {
        klog("Keine", "ntfs: %s: the root directory cannot be opened (%d)", dev->name, rc);
        if (!rc) vnode_put(root);
        ntfs_vol_close(m->vol);
        if (m->bde) bde_detach(m->bde, m->src);
        kfree(m);
        mt->priv = NULL;
        return rc ? rc : -EIO;
    }
    mt->root = root;
    g_mnts[slot] = m;
    NtfsVol *v = m->vol;
    klog("Keine", "ntfs: %s%s%s mounted on %s, %lu MiB, %u-byte clusters, label '%s', NTFS %u.%u, read-only (this driver cannot write)", dev->name,
         locked ? " via " : "", locked ? m->src->name : "", mt->target, (unsigned long)(v->total_sectors * v->bps >> 20), v->cluster, v->label, v->ver_major, v->ver_minor);
    if (v->vol_flags & 1)
        klog("Keine", "ntfs: %s is marked dirty (Windows was not shut down cleanly, or Fast Startup left it hibernated): the journal is not replayed, so what is shown can "
             "be a little older than what Windows has in memory", dev->name);
    return 0;
}

static void ntfs_unmount(Mount *mt) {
    NtfsMnt *m = mt->priv;
    if (!m) return;
    for (int i = 0; i < NT_MAX_MOUNTS; i++) if (g_mnts[i] == m) g_mnts[i] = NULL;
    mutex_lock(&m->lock);
    if (mt->root) {                                                     /* the mount's own reference is dropped by the VFS afterwards: detach the inode now */
        NtfsInode *ni = mt->root->priv;
        mt->root->priv = NULL;
        if (ni) {
            u32 h = (u32)(ni->f->no % NT_HASH);
            u64 fl = irq_save();
            for (NtfsInode **pp = &m->hash[h]; *pp; pp = &(*pp)->next)
                if (*pp == ni) { *pp = ni->next; break; }
            m->nlive--;
            irq_restore(fl);
            ntfs_file_close(ni->f);
            kfree(ni);
        }
    }
    u64 reads = m->vol->n_reads;
    ntfs_vol_close(m->vol);
    mutex_unlock(&m->lock);
    klog("Keine", "ntfs: %s unmounted (%lu device reads in all)", m->dev->name, (unsigned long)reads);
    if (m->bde) bde_detach(m->bde, m->src);
    kfree(m);
    mt->priv = NULL;
}

static int ntfs_busy(Mount *mt) {
    NtfsMnt *m = mt->priv;
    return m && m->nlive > 1;
}

static int ntfs_statfs(Mount *mt, struct gstatfs *out) {
    NtfsMnt *m = mt->priv;
    mutex_lock(&m->lock);
    NtfsVol *v = m->vol;
    u64 fr = 0;
    int rc = ntfs_free_clusters(v, &fr);
    out->f_type = NTFS_SUPER_MAGIC;
    out->f_bsize = v->cluster;
    out->f_blocks = v->nclusters;
    out->f_bfree = out->f_bavail = rc ? 0 : fr;
    out->f_files = v->nrecords;
    out->f_namelen = 255;
    strlcpy(out->f_fstypename, "ntfs", sizeof out->f_fstypename);
    mutex_unlock(&m->lock);
    return 0;
}

const FsType g_ntfs_type = {
    .name = "ntfs", .mount = ntfs_mount, .unmount = ntfs_unmount, .statfs = ntfs_statfs, .busy = ntfs_busy, .flags = FS_FIXED_OWNER,
};

static void ntfs_register(void) { vfs_register_fs(&g_ntfs_type); }
INITCALL(ntfs_register);
