/*
 * ext2, ext3 and ext4 for Gensokyo, read only: the VFS side. The parser (ext4_core.c) knows the format; this file turns it into vnodes, mounts and unmounts it, and keeps
 * the promises of docs/EXT4.md:
 *
 *   - read only, for ever: the vnode operations of a directory or file are lookup, readdir, read and readlink; every operation that would change something is answered
 *     with -EROFS and nothing here has a path to the device that writes. `-o rw` is refused with a log line rather than quietly ignored;
 *   - nothing mounts by itself: the mount policy (mountpolicy.c) never reads these volumes, and the types are only used when root says `mount -t ext4` (or ext2, ext3:
 *     one driver under three names; the name only decides what /proc/mounts and df print);
 *   - owners and permissions are the real ones of each inode (FS_FIXED_OWNER is not set: the VFS checks uid, gid and mode as for any filesystem). `-o nosuid` clears the
 *     set-user-id and set-group-id bits of every file, which is what a stick from somewhere else deserves;
 *   - one vnode per inode (found again through a small hash by inode number), so that hard links, a mount point on a directory, ".." at the volume's root and the open
 *     files of a process all agree on what is the same file.
 *
 * Locking: one mutex per mount covers every call into the parser (it has a cache and per-file windows). Releasing a vnode is lock-free (the hash is protected by
 * turning interrupts off, as ntfs.c and fat32.c do), because vnode_put() can run while the mutex is held.
 */
#include "ext4_priv.h"
#include "ext4.h"
#include "vfs.h"
#include "blk.h"
#include "cpu.h"
#include "initcall.h"

#define E4_HASH 64
#define E4_MAX_MOUNTS 8

typedef struct Ext4Mnt Ext4Mnt;

typedef struct Ext4Inode {
    struct Ext4Inode *next;                     /* hash chain */
    Ext4Mnt *mnt;
    Ext4File *f;
    VNode *vn;
} Ext4Inode;

struct Ext4Mnt {
    Mutex lock;
    Ext4Vol *vol;
    BlockDev *dev;
    Mount *mount;
    Ext4Inode *hash[E4_HASH];
    int nlive;                                  /* live vnodes, the root's included */
    bool nosuid;
    u32 skipped_logged;                         /* names left out so far that the log has been told about */
};

static Ext4Mnt *g_mnts[E4_MAX_MOUNTS];

static const VNodeOps ext4_dir_ops, ext4_file_ops, ext4_link_ops, ext4_special_ops;

/* ---------------------------------------------------------------------------------------------- the device */

/* The parser's read callback over a block device: any byte range, through a bounce buffer for the ragged ends. Reads only. */
static int ext4_blk_io(void *ctx, u64 off, void *buf, size_t len) {
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

static u64 clamp_time(s64 t) { return t < 0 ? 0 : (u64)t; }       /* before 1970 the VFS has nothing to say: the epoch */

static int iget(Ext4Mnt *m, u32 ino, VNode **out) {
    u32 h = ino % E4_HASH;
    u64 fl = irq_save();
    for (Ext4Inode *i = m->hash[h]; i; i = i->next) {
        if (i->f->ino != ino || !i->vn || i->vn->refs <= 0) continue;       /* a vnode on its way out is not reused */
        i->vn->refs++;
        irq_restore(fl);
        *out = i->vn;
        return 0;
    }
    irq_restore(fl);
    Ext4File *f;
    int rc = ext4_file_open(m->vol, ino, &f);
    if (rc) return rc;
    Ext4Inode *ni = kzalloc(sizeof *ni);
    if (!ni) { ext4_file_close(f); return -ENOMEM; }
    u32 mode = f->mode & 0177777;
    if (m->nosuid && S_ISREG(mode)) mode &= ~(u32)(S_ISUID | S_ISGID);
    const VNodeOps *ops = S_ISDIR(mode) ? &ext4_dir_ops : S_ISREG(mode) ? &ext4_file_ops : S_ISLNK(mode) ? &ext4_link_ops : &ext4_special_ops;
    VNode *vn = vnode_alloc(ops, m->mount, mode);
    if (!vn) { kfree(ni); ext4_file_close(f); return -ENOMEM; }
    ni->mnt = m;
    ni->f = f;
    ni->vn = vn;
    vn->priv = ni;
    vn->ino = ino;
    vn->uid = f->uid;
    vn->gid = f->gid;
    vn->nlink = f->nlink ? f->nlink : 1;
    vn->size = f->size;
    vn->atime = clamp_time(f->atime);
    vn->mtime = clamp_time(f->mtime);
    vn->ctime = clamp_time(f->ctime);
    vn->rdev = f->rdev;
    fl = irq_save();
    ni->next = m->hash[h];
    m->hash[h] = ni;
    m->nlive++;
    irq_restore(fl);
    *out = vn;
    return 0;
}

/* Last reference dropped. Lock-free: the hash is guarded by interrupts being off, the parser's memory is simply freed. */
static void ext4_release(VNode *vn) {
    Ext4Inode *ni = vn->priv;
    if (!ni) return;                                                    /* the root after the unmount */
    Ext4Mnt *m = ni->mnt;
    u32 h = ni->f->ino % E4_HASH;
    u64 fl = irq_save();
    for (Ext4Inode **pp = &m->hash[h]; *pp; pp = &(*pp)->next)
        if (*pp == ni) { *pp = ni->next; break; }
    m->nlive--;
    irq_restore(fl);
    vn->priv = NULL;
    ext4_file_close(ni->f);
    kfree(ni);
}

/* ---------------------------------------------------------------------------------------------- reading */

static void note_skipped(Ext4Mnt *m) {
    if (m->vol->skipped_names > m->skipped_logged) {
        m->skipped_logged = m->vol->skipped_names;
        klog("Keine", "ext4: %s: a directory entry whose name contains a slash or a NUL is not shown (this system cannot carry such a name)", m->mount->target);
    }
}

static int ext4_lookup(VNode *dir, const char *name, VNode **out) {
    Ext4Inode *di = dir->priv;
    Ext4Mnt *m = di->mnt;
    if (!strcmp(name, ".")) { vnode_get(dir); *out = dir; return 0; }
    mutex_lock(&m->lock);
    u32 ino;
    int rc = ext4_dir_lookup(di->f, name, &ino);
    if (!rc) rc = iget(m, ino, out);
    mutex_unlock(&m->lock);
    return rc;
}

static int ext4_readdir(VNode *dir, u64 cookie, DirEntry *out) {
    Ext4Inode *di = dir->priv;
    Ext4Mnt *m = di->mnt;
    Ext4DirEnt e;
    u64 pos = cookie;                                                   /* the cookie is a byte position in the directory: it stays valid whatever happens in between */
    mutex_lock(&m->lock);
    int rc = ext4_dir_next(di->f, &pos, &e);
    if (rc > 0) {
        out->ino = e.ino;
        out->type = e.type;
        strlcpy(out->name, e.name, sizeof out->name);
        out->next = pos;
    }
    note_skipped(m);
    mutex_unlock(&m->lock);
    return rc;
}

static ssize_t ext4_vread(VNode *vn, File *fl, void *buf, size_t n, u64 off) {
    (void)fl;
    Ext4Inode *ni = vn->priv;
    Ext4Mnt *m = ni->mnt;
    mutex_lock(&m->lock);
    ssize_t r = ext4_read(ni->f, off, buf, n);
    mutex_unlock(&m->lock);
    return r;
}

static ssize_t ext4_vreadlink(VNode *vn, char *buf, size_t n) {
    Ext4Inode *ni = vn->priv;
    Ext4Mnt *m = ni->mnt;
    mutex_lock(&m->lock);
    int len = ext4_readlink(ni->f, buf, n);
    mutex_unlock(&m->lock);
    if (len < 0) return len;
    return (size_t)len < n ? len : (ssize_t)n;
}

/* A device node, a fifo or a socket on a disk is only a name here: there is no driver behind a Linux device number, and no peer for a socket. */
static ssize_t ext4_nodev_read(VNode *v, File *f, void *buf, size_t n, u64 off) { (void)v; (void)f; (void)buf; (void)n; (void)off; return -ENXIO; }

/* ---------------------------------------------------------------------------------------------- refusals: this driver cannot write, and says so */

static ssize_t ext4_nowrite(VNode *v, File *f, const void *buf, size_t n, u64 off) { (void)v; (void)f; (void)buf; (void)n; (void)off; return -EROFS; }
static int ext4_notruncate(VNode *v, u64 size) { (void)v; (void)size; return -EROFS; }
static int ext4_nocreate(VNode *d, const char *n, u32 mode, VNode **out) { (void)d; (void)n; (void)mode; (void)out; return -EROFS; }
static int ext4_nosymlink(VNode *d, const char *n, const char *t, VNode **out) { (void)d; (void)n; (void)t; (void)out; return -EROFS; }
static int ext4_nolink(VNode *d, const char *n, VNode *t) { (void)d; (void)n; (void)t; return -EROFS; }
static int ext4_nounlink(VNode *d, const char *n) { (void)d; (void)n; return -EROFS; }
static int ext4_norename(VNode *od, const char *on, VNode *nd, const char *nn) { (void)od; (void)on; (void)nd; (void)nn; return -EROFS; }
static int ext4_nochmod(VNode *v, u32 mode) { (void)v; (void)mode; return -EROFS; }
static int ext4_nochown(VNode *v, u32 uid, u32 gid) { (void)v; (void)uid; (void)gid; return -EROFS; }
static int ext4_notimes(VNode *v, u64 a, u64 m) { (void)v; (void)a; (void)m; return -EROFS; }

static const VNodeOps ext4_dir_ops = {
    .lookup = ext4_lookup, .readdir = ext4_readdir, .release = ext4_release,
    .create = ext4_nocreate, .mkdir = ext4_nocreate, .symlink = ext4_nosymlink, .link = ext4_nolink, .unlink = ext4_nounlink, .rmdir = ext4_nounlink,
    .rename = ext4_norename, .chmod = ext4_nochmod, .chown = ext4_nochown, .settimes = ext4_notimes,
};

static const VNodeOps ext4_file_ops = {
    .read = ext4_vread, .release = ext4_release,
    .write = ext4_nowrite, .truncate = ext4_notruncate, .chmod = ext4_nochmod, .chown = ext4_nochown, .settimes = ext4_notimes,
};

static const VNodeOps ext4_link_ops = {
    .readlink = ext4_vreadlink, .release = ext4_release, .chmod = ext4_nochmod, .chown = ext4_nochown, .settimes = ext4_notimes,
};

static const VNodeOps ext4_special_ops = {
    .read = ext4_nodev_read, .write = ext4_nowrite, .release = ext4_release, .chmod = ext4_nochmod, .chown = ext4_nochown, .settimes = ext4_notimes,
};

/* ---------------------------------------------------------------------------------------------- mount options */

typedef struct { bool rw, nosuid; } Opts;

static int parse_opts(const char *s, Opts *o) {
    char tok[80];
    while (*s) {
        size_t n = 0;
        while (s[n] && s[n] != ',') n++;
        if (n >= sizeof tok) return -EINVAL;
        memcpy(tok, s, n);
        tok[n] = 0;
        s += n + (s[n] == ',');
        if (!tok[0]) continue;
        const char *eq = strchr(tok, '=');
        size_t klen = eq ? (size_t)(eq - tok) : strlen(tok);
        #define KEY(k) (klen == sizeof(k) - 1 && !memcmp(tok, k, klen))
        if (KEY("ro")) { }
        else if (KEY("rw")) o->rw = true;
        else if (KEY("nosuid")) o->nosuid = true;
        else if (KEY("defaults") || KEY("noatime") || KEY("relatime") || KEY("strictatime") || KEY("nodev") || KEY("exec") || KEY("noexec") || KEY("suid") || KEY("dev") ||
                 KEY("auto") || KEY("noauto") || KEY("user") || KEY("nouser") || KEY("async") || KEY("sync") || KEY("quiet") || KEY("utf8") || KEY("noload") || KEY("norecovery") ||
                 KEY("errors") || KEY("data") || KEY("barrier") || KEY("nobarrier") || KEY("discard") || KEY("nodiscard") || KEY("user_xattr") || KEY("nouser_xattr") ||
                 KEY("acl") || KEY("noacl") || KEY("commit") || KEY("delalloc") || KEY("nodelalloc") || KEY("lazytime") || KEY("nolazytime") || KEY("i_version") ||
                 KEY("init_itable") || KEY("noinit_itable") || KEY("inode_readahead_blks") || KEY("journal_checksum") || KEY("nojournal_checksum") || KEY("grpid") ||
                 KEY("nogrpid") || KEY("bsddf") || KEY("minixdf") || KEY("nodiscard")) { }
        else {
            klog("Keine", "ext4: unknown mount option '%s'", tok);
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

/* Two mounts must never cover the same sectors (a disk and one of its partitions). Mounts are serialised by the VFS. */
static bool device_in_use(const BlockDev *dev) {
    const BlockDev *r1;
    u64 s1;
    root_of_device(dev, &r1, &s1);
    for (int i = 0; i < E4_MAX_MOUNTS; i++) {
        Ext4Mnt *o = g_mnts[i];
        if (!o) continue;
        const BlockDev *r2;
        u64 s2;
        root_of_device(o->dev, &r2, &s2);
        if (r1 == r2 && s1 < s2 + o->dev->sectors && s2 < s1 + dev->sectors) return true;
    }
    return false;
}

static int ext4_mount(const char *source, u32 flags, Mount *mt) {
    (void)flags;
    Opts o = { 0 };
    int rc = parse_opts(mt->opts, &o);
    if (rc) return rc;
    if (o.rw) {
        klog("Keine", "ext4: refusing a read-write mount of %s: this driver only reads, and never will write (drop the 'rw' option)", source);
        return -EROFS;
    }
    BlockDev *dev = blk_find(source);
    if (!dev || dev->removed) return -ENODEV;
    if (device_in_use(dev)) return -EBUSY;
    int slot = -1;
    for (int i = 0; i < E4_MAX_MOUNTS && slot < 0; i++) if (!g_mnts[i]) slot = i;
    if (slot < 0) return -ENOMEM;

    Ext4Mnt *m = kzalloc(sizeof *m);
    if (!m) return -ENOMEM;
    mutex_init(&m->lock);
    m->dev = dev;
    m->mount = mt;
    m->nosuid = o.nosuid;

    Ext4Dev ed = { ext4_blk_io, dev, dev->sectors * dev->sector_size, dev->sector_size };
    char why[256];
    why[0] = 0;
    rc = ext4_vol_open(&m->vol, &ed, why, sizeof why);
    if (rc) {
        klog("Keine", "ext4: %s is not a mountable ext2/3/4 volume: %s (%d)", dev->name, why[0] ? why : "unreadable", rc);
        kfree(m);
        return rc < 0 ? rc : -EIO;
    }
    mt->priv = m;
    mt->flags |= MNT_RDONLY;
    VNode *root;
    mutex_lock(&m->lock);
    rc = iget(m, E4_ROOT_INO, &root);
    mutex_unlock(&m->lock);
    if (rc || !S_ISDIR(root->mode)) {
        klog("Keine", "ext4: %s: the root directory cannot be opened (%d)", dev->name, rc);
        if (!rc) vnode_put(root);
        ext4_vol_close(m->vol);
        kfree(m);
        mt->priv = NULL;
        return rc ? rc : -EIO;
    }
    mt->root = root;
    g_mnts[slot] = m;
    Ext4Vol *v = m->vol;
    char feat[200];
    ext4_feature_text(v, feat, sizeof feat);
    klog("Keine", "%s: %s mounted on %s, %lu MiB, %u-byte blocks, label '%s', %s (%s), read-only (this driver cannot write)", mt->type->name, dev->name, mt->target,
         (unsigned long)(v->blocks * v->bsize >> 20), v->bsize, v->label, ext4_flavour(v), feat);
    if (v->incompat & E4_INCOMPAT_RECOVER)
        klog("Keine", "ext4: %s needs journal recovery (the machine that used it last did not shut it down cleanly) and the journal is not replayed here: what is shown is the "
             "disk as it is, so a file that was being written can be short or missing and a directory can lag behind its files. Mount it where it can be written, or run e2fsck on it",
             dev->name);
    if (v->state & 2)
        klog("Keine", "ext4: %s is marked as having errors (the file system noticed damage while it was in use): what is shown may be damaged", dev->name);
    if (v->last_orphan)
        klog("Keine", "ext4: %s has an orphan list (files that were being deleted or truncated when it was last in use): they are shown as they are on the disk", dev->name);
    return 0;
}

static void ext4_unmount(Mount *mt) {
    Ext4Mnt *m = mt->priv;
    if (!m) return;
    for (int i = 0; i < E4_MAX_MOUNTS; i++) if (g_mnts[i] == m) g_mnts[i] = NULL;
    mutex_lock(&m->lock);
    if (mt->root) {                                                     /* the mount's own reference is dropped by the VFS afterwards: detach the inode now */
        Ext4Inode *ni = mt->root->priv;
        mt->root->priv = NULL;
        if (ni) {
            u32 h = ni->f->ino % E4_HASH;
            u64 fl = irq_save();
            for (Ext4Inode **pp = &m->hash[h]; *pp; pp = &(*pp)->next)
                if (*pp == ni) { *pp = ni->next; break; }
            m->nlive--;
            irq_restore(fl);
            ext4_file_close(ni->f);
            kfree(ni);
        }
    }
    u64 reads = m->vol->n_reads, hits = m->vol->n_hits;
    ext4_vol_close(m->vol);
    mutex_unlock(&m->lock);
    klog("Keine", "%s: %s unmounted (%lu device reads, %lu metadata cache hits)", mt->type->name, m->dev->name, (unsigned long)reads, (unsigned long)hits);
    kfree(m);
    mt->priv = NULL;
}

static int ext4_busy(Mount *mt) {
    Ext4Mnt *m = mt->priv;
    return m && m->nlive > 1;
}

static int ext4_statfs_op(Mount *mt, struct gstatfs *out) {
    Ext4Mnt *m = mt->priv;
    Ext4Stat st;
    mutex_lock(&m->lock);
    int rc = ext4_statfs(m->vol, &st);
    mutex_unlock(&m->lock);
    if (rc) return rc;
    out->f_type = EXT4_SUPER_MAGIC;
    out->f_bsize = st.bsize;
    out->f_blocks = st.blocks;
    out->f_bfree = st.bfree;
    out->f_bavail = st.bavail;
    out->f_files = st.files;
    out->f_ffree = st.ffree;
    out->f_namelen = E4_NAME_MAX;
    strlcpy(out->f_fstypename, mt->type->name, sizeof out->f_fstypename);
    return 0;
}

const FsType g_ext4_type = { .name = "ext4", .mount = ext4_mount, .unmount = ext4_unmount, .statfs = ext4_statfs_op, .busy = ext4_busy };
const FsType g_ext3_type = { .name = "ext3", .mount = ext4_mount, .unmount = ext4_unmount, .statfs = ext4_statfs_op, .busy = ext4_busy };
const FsType g_ext2_type = { .name = "ext2", .mount = ext4_mount, .unmount = ext4_unmount, .statfs = ext4_statfs_op, .busy = ext4_busy };

static void ext4_register(void) {
    vfs_register_fs(&g_ext2_type);
    vfs_register_fs(&g_ext3_type);
    vfs_register_fs(&g_ext4_type);
}
INITCALL(ext4_register);
