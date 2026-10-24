/*
 * FAT32 / FAT16 for Gensokyo (Keine keeps the Human Village's records here).
 *
 * This file is the glue to the VFS: mount and unmount, the vnode operations, the dirty-bit protocol, the periodic
 * flusher and statfs. The cache, the FAT, names and directories live in fat_cache.c, fat_chain.c, fat_name.c, fat_dir.c
 * (see fatfs.h for the overview).
 *
 * Safety rules, in short:
 *   - nothing is written until a volume has been validated (BPB geometry, FAT size, root cluster) and mounted read-write
 *     on purpose; a volume whose clean-shutdown flag is clear mounts read-only unless forced;
 *   - while mounted read-write the on-disk "dirty" flags are set before the first modification and cleared again by sync,
 *     unmount and the periodic flush once everything has reached the device;
 *   - cluster numbers read from disk are range checked, chains are walked with bounds, sizes are 32-bit;
 *   - the FAT has no owners or modes: mount options give uid/gid/fmask/dmask, and with "userdirs" a top-level directory
 *     named after a user (and everything below it) belongs to that user with modes 0700/0600.
 */
#include "fatfs.h"
#include "cpu.h"
#include "irq.h"
#include "ktime.h"
#include "bootlog.h"

#define FLUSH_AGE_MS 3000
#define MSDOS_SUPER_MAGIC 0x4d44

static const VNodeOps fat_dir_ops, fat_file_ops;

/* ---------------------------------------------------------------------------------------------- time */

u64 fat_now(void) { return time_now_sec(); }

static s64 days_from_civil(s64 y, unsigned m, unsigned d) {
    y -= m <= 2;
    s64 era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (s64)doe - 719468;
}

void fat_time_to_dos(u64 t, u16 *date, u16 *time, u8 *tenths) {
    int y, mo, d, h, mi, s, wd;
    if (t < 315532800) t = 315532800;                           /* 1980-01-01, the DOS epoch */
    civil_from_epoch(t, &y, &mo, &d, &h, &mi, &s, &wd);
    if (y > 2107) { y = 2107; mo = 12; d = 31; h = 23; mi = 59; s = 58; }
    *date = (u16)(((y - 1980) << 9) | (mo << 5) | d);
    *time = (u16)((h << 11) | (mi << 5) | (s / 2));
    *tenths = (u8)((s & 1) ? 100 : 0);
}

u64 fat_time_from_dos(u16 date, u16 time, u8 tenths) {
    if (!date) return 0;
    int y = (date >> 9) + 1980, mo = (date >> 5) & 15, d = date & 31;
    int h = time >> 11, mi = (time >> 5) & 63, s = (time & 31) * 2;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 59) return 0;
    return (u64)(days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400) + (u64)(h * 3600 + mi * 60 + s) + (tenths >= 100 && tenths <= 199 ? 1 : 0);
}

/* ---------------------------------------------------------------------------------------------- ownership and modes */

static const FatUser *node_user(const FatNode *n) { return n->owner ? &n->vol->users[n->owner - 1] : NULL; }

/* The mode a node would have if its read-only attribute were clear. */
static u32 node_base_perm(const FatNode *n) {
    const FatVol *v = n->vol;
    bool dir = (n->flags & FN_DIR) != 0;
    if (n->owner) return dir ? 0700 : 0600;
    return dir ? (0777 & ~v->dmask) : (0777 & ~v->fmask);
}

u32 fat_node_mode(const FatNode *n) {
    bool dir = (n->flags & FN_DIR) != 0;
    u32 perm = node_base_perm(n);
    if (!dir && (n->attr & FAT_ATTR_RO)) perm &= ~0222u;
    return (dir ? S_IFDIR : S_IFREG) | perm;
}

/* Copies the node's state into its live vnode (the VFS reads these fields without any lock). The stores happen with
 * interrupts off: a vnode whose last reference is being dropped is only freed after fat_release() has detached it. */
void fat_node_publish(FatNode *n) {
    const FatUser *u = node_user(n);
    bool dir = (n->flags & FN_DIR) != 0;
    u32 mode = fat_node_mode(n);
    u32 uid = u ? u->uid : n->vol->uid, gid = u ? u->gid : n->vol->gid;
    u64 size = dir ? fatd_dirsize(n) : n->size;
    u32 nlink = (n->flags & FN_UNLINKED) ? 0 : dir ? 2 : 1;
    u64 f = irq_save();
    VNode *v = n->vn;
    if (v) {
        v->mode = mode;
        v->uid = uid;
        v->gid = gid;
        v->size = size;
        v->atime = n->atime;
        v->mtime = n->mtime;
        v->ctime = n->ctime;
        v->nlink = nlink;
    }
    irq_restore(f);
}

/* A directory moved between levels may change who owns everything below it. */
static void node_set_owner(FatNode *n, u8 owner) {
    n->owner = owner;
    fat_node_publish(n);
    if (n->dir)
        for (u32 i = 0; i < n->dir->count; i++) node_set_owner(n->dir->order[i], owner);
}

/* ---------------------------------------------------------------------------------------------- volume list and locking */

static FatVol *g_vols[FAT_MAX_VOLS];
static Mutex g_vols_lock;
static bool g_inited;
static bool g_flusher_started;

static void fat_global_init(void) {
    if (!g_inited) { mutex_init(&g_vols_lock); g_inited = true; }
}

static int boot_flag_set(FatVol *v, bool dirty);
static int fsinfo_store(FatVol *v);

static void vol_reap(FatVol *v);

static int mark_media_dirty(FatVol *v) {
    int rc = fat_set_clean_bits(v, false);
    if (!rc) rc = boot_flag_set(v, true);
    if (!rc) rc = fc_flush(v);                                   /* the flag must be durable before the first real change */
    if (rc) { v->io_error = rc; return rc; }
    v->media_dirty = true;
    return 0;
}

/* Everything that modifies the volume starts here: refuses on read-only mounts and sets the on-disk dirty flags first. */
static int vol_begin_write(FatVol *v) {
    if (v->ro) return -EROFS;
    if (v->dying) return -ENODEV;
    v->last_op_ms = uptime_ms();
    if (!v->media_dirty && !v->was_dirty) return mark_media_dirty(v);
    return 0;
}

static void vol_enter(FatVol *v) {
    mutex_lock(&v->lock);
    if (v->orphans) vol_reap(v);
    fatd_trim(v);                                                /* no-op unless the node cache is over its limit */
}

static void vol_leave(FatVol *v) {
    if (!v->ro && (v->sync_mode || v->ndirty > v->dirty_limit)) {
        int rc = fc_flush(v);                                    /* write-behind throttle / "sync" mount option */
        if (rc) v->io_error = rc;
    }
    mutex_unlock(&v->lock);
}

/* Boot sector "dirty" flag byte (bit 0): FAT32 at 0x41, FAT16 at 0x25. */
static int boot_flag_set(FatVol *v, bool dirty) {
    u64 off = v->bits == 32 ? 0x41 : 0x25;
    u8 b;
    int rc = fc_read(v, off, &b, 1);
    if (rc) return rc;
    u8 nb = (u8)(dirty ? (b | 1) : (b & ~1));
    if (nb == b) return 0;
    return fc_write(v, off, &nb, 1, FPC_META);
}

static int boot_flag_get(FatVol *v, bool *dirty) {
    u8 b;
    int rc = fc_read(v, v->bits == 32 ? 0x41 : 0x25, &b, 1);
    if (!rc) *dirty = b & 1;
    return rc;
}

/* FSInfo free count and next-free hint (FAT32). Only written when they changed. */
static int fsinfo_store(FatVol *v) {
    if (!v->fsinfo_sector || !v->free_known) return 0;
    u64 off = (u64)v->fsinfo_sector * v->bps + 488;
    u8 cur[8], want[8];
    int rc = fc_read(v, off, cur, sizeof cur);
    if (rc) return rc;
    u32 free_n = v->free_clusters, next = v->alloc_cursor;
    for (int i = 0; i < 4; i++) { want[i] = (u8)(free_n >> (8 * i)); want[4 + i] = (u8)(next >> (8 * i)); }
    if (!memcmp(cur, want, sizeof cur)) return 0;
    return fc_write(v, off, want, sizeof want, FPC_META);
}

/* Precondition: every dirty page has been written. Clears the dirty flags: the flag byte and FSInfo first, FAT[1] last. */
static int mark_media_clean(FatVol *v) {
    int rc = fsinfo_store(v);
    if (!rc) rc = boot_flag_set(v, false);
    if (!rc) rc = fc_flush(v);
    if (!rc) rc = fat_set_clean_bits(v, true);
    if (!rc) rc = fc_flush(v);
    if (rc) { v->io_error = rc; return rc; }
    v->media_dirty = false;
    return 0;
}

/* Makes everything durable; when nothing is pending afterwards (and may_clean says nobody is busy writing) the volume is
 * marked clean again. */
static int vol_sync(FatVol *v, bool may_clean) {
    if (v->ro) return 0;
    if (v->orphans) vol_reap(v);
    int rc = fc_flush(v);
    if (rc) { v->io_error = rc; return rc; }
    if (v->media_dirty && !v->was_dirty) return may_clean ? mark_media_clean(v) : 0;
    if (v->was_dirty) {                                          /* forced mount: stays dirty, but the hints stay truthful */
        rc = fsinfo_store(v);
        if (!rc) rc = fc_flush(v);
        if (rc) v->io_error = rc;
    }
    return rc;
}

/* ---------------------------------------------------------------------------------------------- vnodes */

static VNode *node_vnode(FatNode *n) {
    FatVol *v = n->vol;
    u64 f = irq_save();
    VNode *vn = n->vn;
    if (vn && vn->refs > 0) { vn->refs++; irq_restore(f); return vn; }
    irq_restore(f);
    vn = vnode_alloc((n->flags & FN_DIR) ? &fat_dir_ops : &fat_file_ops, v->mount, 0);
    if (!vn) return NULL;
    vn->priv = n;
    vn->ino = n->ino;
    f = irq_save();
    n->vn = vn;                                                  /* a dying predecessor (refs == 0) simply loses the race */
    v->nlive++;
    irq_restore(f);
    fat_node_publish(n);
    return vn;
}

/* Last reference dropped: lock-free, deferred cleanup of unlinked nodes goes through the orphan list. */
static void fat_release(VNode *vn) {
    FatNode *n = vn->priv;
    if (!n) return;
    FatVol *v = n->vol;
    u64 f = irq_save();
    if (n->vn == vn) n->vn = NULL;
    v->nlive--;
    if ((n->flags & FN_UNLINKED) && !n->vn) {
        n->onext = v->orphans;
        v->orphans = n;
    }
    irq_restore(f);
}

/* Frees nodes that were unlinked while open and have now been released (volume lock held). */
static void vol_reap(FatVol *v) {
    for (;;) {
        u64 f = irq_save();
        FatNode *n = v->orphans;
        if (n) v->orphans = n->onext;
        irq_restore(f);
        if (!n) break;
        if (n->first && !(n->flags & FN_DIR) && !v->ro && vol_begin_write(v) == 0) fat_free_chain(v, n->first, NULL);
        n->first = 0;
        fatd_node_free(n);
    }
}

/* A node that has been taken out of its directory: forget it now, or when the last user lets go. */
static void node_dispose(FatNode *c) {
    u64 f = irq_save();
    c->flags |= FN_UNLINKED;
    VNode *live = c->vn;
    if (live) live->nlink = 0;
    irq_restore(f);
    if (!live) {
        if (c->first && !(c->flags & FN_DIR)) fat_free_chain(c->vol, c->first, NULL);
        c->first = 0;
        fatd_node_free(c);
    }
}

/* ---------------------------------------------------------------------------------------------- namespace operations */

static int fat_lookup(VNode *dir, const char *name, VNode **out) {
    FatNode *d = dir->priv;
    FatVol *v = d->vol;
    if (!strcmp(name, ".")) { vnode_get(dir); *out = dir; return 0; }
    vol_enter(v);
    int rc;
    if (!strcmp(name, "..")) {
        VNode *pv = node_vnode(d->parent ? d->parent : d);
        rc = pv ? 0 : -ENOMEM;
        *out = pv;
    } else {
        rc = fatd_load(d);
        if (!rc) {
            FatNode *c = fatd_find(d, name);
            if (!c) {
                rc = -ENOENT;
            } else {
                *out = node_vnode(c);
                rc = *out ? 0 : -ENOMEM;
            }
        }
    }
    mutex_unlock(&v->lock);
    return rc;
}

static int fat_readdir(VNode *dir, u64 cookie, DirEntry *out) {
    FatNode *d = dir->priv;
    FatVol *v = d->vol;
    vol_enter(v);
    int rc = fatd_load(d);
    if (rc == -ENOENT) rc = 0;                                  /* a removed directory reads as empty */
    int got = 0;
    if (!rc && d->dir && cookie < d->dir->count) {
        FatNode *c = d->dir->order[cookie];
        out->ino = c->ino;
        out->type = (c->flags & FN_DIR) ? DT_DIR : DT_REG;
        strlcpy(out->name, c->name, sizeof out->name);
        got = 1;
    }
    mutex_unlock(&v->lock);
    return rc ? rc : got;
}

static void touch_dir(FatNode *d) {
    d->mtime = d->ctime = fat_now();
    fat_node_publish(d);
}

static int fat_create(VNode *dir, const char *name, u32 mode, VNode **out) {
    (void)mode;
    FatNode *d = dir->priv;
    FatVol *v = d->vol;
    vol_enter(v);
    int rc = v->ro ? -EROFS : fatd_load(d);
    if (!rc && fatd_find(d, name)) rc = -EEXIST;
    if (!rc) rc = vol_begin_write(v);
    if (!rc) {
        FatNode *c;
        rc = fatd_create(d, name, FAT_ATTR_ARCH, 0, &c);
        if (!rc) {
            c->flags |= FN_CHAIN_OK | FN_CHAIN_END;
            touch_dir(d);
            *out = node_vnode(c);
            if (!*out) rc = -ENOMEM;
        }
    }
    vol_leave(v);
    return rc;
}

static int fat_mkdir(VNode *dir, const char *name, u32 mode, VNode **out) {
    (void)mode;
    FatNode *d = dir->priv;
    FatVol *v = d->vol;
    vol_enter(v);
    int rc = v->ro ? -EROFS : fatd_load(d);
    if (!rc && fatd_find(d, name)) rc = -EEXIST;
    if (!rc) rc = vol_begin_write(v);
    if (!rc) {
        u32 first, last;
        rc = fat_alloc_chain_cb(v, 1, 0, FPC_META, NULL, NULL, &first, &last);
        if (!rc) {
            u64 now = fat_now();
            rc = fatd_write_dots(v, first, (d->flags & FN_ROOT) ? 0 : d->first, now);
            FatNode *c = NULL;
            if (!rc) rc = fatd_create(d, name, FAT_ATTR_DIR, first, &c);
            if (rc) {
                fat_free_chain(v, first, NULL);
            } else {
                touch_dir(d);
                *out = node_vnode(c);
                if (!*out) rc = -ENOMEM;
            }
        }
    }
    vol_leave(v);
    return rc;
}

static int fat_unlink(VNode *dir, const char *name) {
    FatNode *d = dir->priv;
    FatVol *v = d->vol;
    vol_enter(v);
    int rc = v->ro ? -EROFS : fatd_load(d);
    FatNode *c = NULL;
    if (!rc) {
        c = fatd_find(d, name);
        if (!c) rc = -ENOENT;
        else if (c->flags & FN_DIR) rc = -EISDIR;
    }
    if (!rc) rc = vol_begin_write(v);
    if (!rc) {
        fatd_remove(d, c);
        node_dispose(c);
        touch_dir(d);
    }
    vol_leave(v);
    return rc;
}

static int fat_rmdir(VNode *dir, const char *name) {
    FatNode *d = dir->priv;
    FatVol *v = d->vol;
    vol_enter(v);
    int rc = v->ro ? -EROFS : fatd_load(d);
    FatNode *c = NULL;
    if (!rc) {
        c = fatd_find(d, name);
        if (!c) rc = -ENOENT;
        else if (!(c->flags & FN_DIR)) rc = -ENOTDIR;
        else if (c->vn && c->vn->mounted) rc = -EBUSY;
    }
    if (!rc) rc = fatd_load(c);
    if (!rc && c->dir->count) rc = -ENOTEMPTY;
    if (!rc) rc = vol_begin_write(v);
    if (!rc) {
        fatd_remove(d, c);
        u32 first = c->first;
        c->first = 0;
        fatd_drop(c);
        chain_reset(c);
        if (first) fat_free_chain(v, first, NULL);
        node_dispose(c);
        touch_dir(d);
    }
    vol_leave(v);
    return rc;
}

static int fat_rename(VNode *olddir, const char *oldname, VNode *newdir, const char *newname) {
    FatNode *od = olddir->priv, *nd = newdir->priv;
    FatVol *v = od->vol;
    if (nd->vol != v) return -EXDEV;
    vol_enter(v);
    int rc = v->ro ? -EROFS : fatd_load(od);
    if (!rc && od != nd) rc = fatd_load(nd);
    FatNode *src = NULL, *dst = NULL;
    if (!rc) {
        src = fatd_find(od, oldname);
        if (!src) rc = -ENOENT;
    }
    if (!rc && (src->flags & FN_DIR)) {                          /* never move a directory below itself */
        for (FatNode *p = nd; p; p = p->parent)
            if (p == src) { rc = -EINVAL; break; }
    }
    if (!rc) {
        dst = fatd_find(nd, newname);
        if (dst == src) {
            if (!strcmp(src->name, newname)) { vol_leave(v); return 0; }
            dst = NULL;                                          /* only the spelling changes */
        }
    }
    if (!rc && dst) {
        if (dst->flags & FN_DIR) {
            if (!(src->flags & FN_DIR)) rc = -EISDIR;
            else if (dst->vn && dst->vn->mounted) rc = -EBUSY;
            else if ((rc = fatd_load(dst)) == 0 && dst->dir->count) rc = -ENOTEMPTY;
        } else if (src->flags & FN_DIR) {
            rc = -ENOTDIR;
        }
    }
    if (!rc) rc = vol_begin_write(v);
    if (!rc) {
        bool was_dir_move = (src->flags & FN_DIR) && od != nd;
        int how = -EAGAIN;
        if (dst && !((src->flags | dst->flags) & FN_DIR))       /* a file over a file: the name stays in place (fat_dir.c) */
            how = fatd_replace_file(nd, src, newname, dst);
        if (how != -EAGAIN) {
            rc = how;
            if (!dst->parent) node_dispose(dst);                 /* its clusters go now that no entry points at them on the device */
            dst = NULL;
        } else {
            rc = fatd_attach_renamed(nd, src, newname, dst);    /* new entry first, then the old ones go */
        }
        if (!rc && dst) {
            fatd_remove(nd, dst);
            if (dst->flags & FN_DIR) {
                u32 first = dst->first;
                dst->first = 0;
                fatd_drop(dst);
                chain_reset(dst);
                if (first) fat_free_chain(v, first, NULL);
            }
            node_dispose(dst);
        }
        if (!rc && was_dir_move) {
            int r2 = fatd_set_dotdot(src, (nd->flags & FN_ROOT) ? 0 : nd->first);
            if (r2) rc = r2;
        }
        if (!rc) {
            u8 owner = nd->owner;
            if (!owner && (nd->flags & FN_ROOT) && v->userdirs && (src->flags & FN_DIR)) {
                for (int i = 0; i < v->nusers; i++)
                    if (fatn_equal(v->users[i].name, newname)) owner = (u8)(i + 1);
            }
            if (owner != src->owner) node_set_owner(src, owner);
            fat_node_publish(src);
            touch_dir(od);
            if (nd != od) touch_dir(nd);
        }
    }
    vol_leave(v);
    return rc;
}

/* ---------------------------------------------------------------------------------------------- file data */

static ssize_t file_read_locked(FatNode *nd, void *buf, size_t n, u64 off) {
    FatVol *v = nd->vol;
    if (nd->flags & FN_DAMAGED) return -EIO;
    if (off >= nd->size) return 0;
    if (n > nd->size - off) n = (size_t)(nd->size - off);
    size_t done = 0;
    while (done < n) {
        u64 disk, avail;
        int rc = chain_map(nd, off + done, (u32)MIN(n - done, (size_t)(1u << 20)), &disk, &avail);
        if (rc) {
            if (rc == -ENXIO) { nd->flags |= FN_DAMAGED; rc = -EIO; }       /* the chain ends before the size says it should */
            return done ? (ssize_t)done : rc;
        }
        size_t take = (size_t)MIN((u64)(n - done), avail);
        rc = fc_read(v, disk, (u8 *)buf + done, take);
        if (rc) return done ? (ssize_t)done : rc;
        done += take;
    }
    return (ssize_t)done;
}

static ssize_t fat_read(VNode *vn, File *f, void *buf, size_t n, u64 off) {
    (void)f;
    FatNode *nd = vn->priv;
    FatVol *v = nd->vol;
    vol_enter(v);
    ssize_t r = file_read_locked(nd, buf, n, off);
    mutex_unlock(&v->lock);
    return r;
}

static ssize_t file_write_locked(FatNode *nd, const void *buf, size_t n, u64 off) {
    FatVol *v = nd->vol;
    if (nd->flags & (FN_DAMAGED | FN_NOWRITE)) return -EIO;
    int rc = vol_begin_write(v);
    if (rc) return rc;
    if (off >= FAT_MAX_FILE) return -EFBIG;
    if (n > FAT_MAX_FILE - off) n = (size_t)(FAT_MAX_FILE - off);
    rc = chain_prepare(nd);
    if (rc) return rc;
    u64 end = off + n;
    u32 need = (u32)((end + v->cbytes - 1) >> v->cshift);
    if (need > nd->known) {
        rc = chain_grow(nd, need - nd->known);
        if (rc == -ENOSPC) {                                     /* take whatever still fits */
            u32 freec = 0;
            if (v->free_known) freec = v->free_clusters;
            else if (fat_count_free(v, &freec) == 0) { v->free_clusters = freec; v->free_known = true; v->free_verified = true; }
            u32 more = freec < need - nd->known ? freec : need - nd->known;
            rc = more ? chain_grow(nd, more) : -ENOSPC;
            if (rc) {
                u64 cap = (u64)nd->known * v->cbytes;
                if (off >= cap) return rc;
                n = (size_t)(cap - off);
                end = off + n;
                rc = 0;
            } else {
                u64 cap = (u64)nd->known * v->cbytes;
                if (end > cap) { n = (size_t)(cap - off); end = off + n; }
            }
        } else if (rc) {
            return rc;
        }
    }
    size_t done = 0;
    while (done < n) {
        u64 disk, avail;
        rc = chain_map(nd, off + done, (u32)MIN(n - done, (size_t)(1u << 20)), &disk, &avail);
        if (rc) break;
        size_t take = (size_t)MIN((u64)(n - done), avail);
        rc = fc_write(v, disk, (const u8 *)buf + done, take, FPC_DATA);
        if (rc) break;
        done += take;
    }
    if (done) {
        if (off + done > nd->size) nd->size = (u32)(off + done);
        nd->mtime = nd->ctime = fat_now();
        nd->attr |= FAT_ATTR_ARCH;
        fatd_commit(nd);
        fat_node_publish(nd);
    }
    if (rc && !done) return rc;
    return (ssize_t)done;
}

static ssize_t fat_write(VNode *vn, File *f, const void *buf, size_t n, u64 off) {
    (void)f;
    if (!n) return 0;
    FatNode *nd = vn->priv;
    FatVol *v = nd->vol;
    vol_enter(v);
    ssize_t r = file_write_locked(nd, buf, n, off);
    vol_leave(v);
    return r;
}

/* Grows (zero-filled) or shrinks a prepared file to exactly `size` bytes. */
static int file_resize_locked(FatNode *nd, u64 size) {
    FatVol *v = nd->vol;
    int rc = 0;
    u32 need = (u32)((size + v->cbytes - 1) >> v->cshift);
    if (size > nd->size) {
        if (need > nd->known) rc = chain_grow(nd, need - nd->known);
    } else {
        if (need < nd->known) rc = chain_shrink(nd, need);
        if (!rc && need && (size & (v->cbytes - 1))) {          /* keep "everything past the size is zero" true */
            u64 d, avail;
            rc = chain_map(nd, size, 1, &d, &avail);
            if (!rc) rc = fc_zero(v, d, (size_t)(v->cbytes - (size & (v->cbytes - 1))), FPC_DATA);
        }
    }
    if (rc) return rc;
    nd->size = (u32)size;
    nd->mtime = nd->ctime = fat_now();
    fatd_commit(nd);
    fat_node_publish(nd);
    return 0;
}

static int fat_truncate(VNode *vn, u64 size) {
    FatNode *nd = vn->priv;
    FatVol *v = nd->vol;
    vol_enter(v);
    int rc = 0;
    if (v->ro) rc = -EROFS;
    else if (size > FAT_MAX_FILE) rc = -EFBIG;
    else if (nd->flags & FN_DAMAGED) rc = -EIO;
    else if (size != nd->size) {
        rc = vol_begin_write(v);
        if (!rc) rc = chain_prepare(nd);
        if (!rc) rc = file_resize_locked(nd, size);
    }
    vol_leave(v);
    return rc;
}

/* ---------------------------------------------------------------------------------------------- attributes */

/* chmod: the FAT stores no modes, only the read-only attribute. A request that equals the current mode succeeds; a request
 * that differs only by the write bits toggles the attribute; anything else would pretend to store what we cannot. */
static int fat_chmod(VNode *vn, u32 mode) {
    FatNode *n = vn->priv;
    FatVol *v = n->vol;
    u32 req = mode & 07777, cur = vn->mode & 07777;
    if (req == cur) return 0;
    if (n->flags & FN_DIR) return -EPERM;
    u32 base = node_base_perm(n);
    bool want_ro;
    if (req == (base & ~0222u)) want_ro = true;
    else if (req == base) want_ro = false;
    else return -EPERM;
    vol_enter(v);
    int rc = v->ro ? -EROFS : vol_begin_write(v);
    if (!rc) {
        if (want_ro) n->attr |= FAT_ATTR_RO; else n->attr &= (u8) ~FAT_ATTR_RO;
        rc = fatd_commit(n);
    }
    vol_leave(v);
    return rc;
}

static int fat_chown(VNode *vn, u32 uid, u32 gid) {
    return (uid == vn->uid && gid == vn->gid) ? 0 : -EPERM;
}

/* utimes(): the node (not the vnode) is the truth for FAT, so the new times go into the node and its directory entry. */
static int fat_settimes(VNode *vn, u64 atime, u64 mtime) {
    FatNode *n = vn->priv;
    FatVol *v = n->vol;
    vol_enter(v);
    int rc = v->ro ? -EROFS : vol_begin_write(v);
    if (!rc) {
        n->atime = atime;
        n->mtime = mtime;
        rc = fatd_commit(n);
        fat_node_publish(n);
    }
    vol_leave(v);
    return rc;
}

static int fat_vsync(VNode *vn) {
    FatNode *n = vn->priv;
    FatVol *v = n->vol;
    mutex_lock(&v->lock);
    int rc = vol_sync(v, true);
    mutex_unlock(&v->lock);
    return rc;
}

static const VNodeOps fat_dir_ops = {
    .lookup = fat_lookup, .create = fat_create, .mkdir = fat_mkdir, .unlink = fat_unlink, .rmdir = fat_rmdir,
    .rename = fat_rename, .readdir = fat_readdir, .chmod = fat_chmod, .chown = fat_chown, .sync = fat_vsync,
    .release = fat_release, .settimes = fat_settimes,
};

static const VNodeOps fat_file_ops = {
    .read = fat_read, .write = fat_write, .truncate = fat_truncate, .chmod = fat_chmod, .chown = fat_chown,
    .sync = fat_vsync, .release = fat_release, .settimes = fat_settimes,
};

/* ---------------------------------------------------------------------------------------------- mount options */

typedef struct {
    bool ro, rw, force, userdirs, sync;
    bool espgrant;                                              /* take the ESP write grant the caller holds (blk_unlock.c) */
    bool have_umask;
    u32 uid, gid, fmask, dmask;
    s32 tz_min;                                                 /* tzoffset=MINUTES: minutes east of UTC that the volume's local clock is ahead */
} Opts;

static u32 parse_num(const char *s, const char **end, int base) {
    u32 v = 0;
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else break;
        if (d >= base) break;
        v = v * (u32)base + (u32)d;
        s++;
    }
    *end = s;
    return v;
}

static const FatUser g_default_users[] = { { "reimu", 1000, 1000 }, { "marisa", 1001, 1001 }, { "sakuya", 1002, 1002 } };

static int parse_opts(FatVol *v, const char *s, Opts *o) {
    o->uid = o->gid = 0;
    o->fmask = 0133;                                            /* files 0644 */
    o->dmask = 0022;                                            /* directories 0755 */
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
        else if (KEY("force")) o->force = true;
        else if (KEY("sync")) o->sync = true;
        else if (KEY("userdirs")) o->userdirs = true;
        else if (KEY("espgrant")) o->espgrant = true;
        else if (KEY("uid") && eq) { o->uid = parse_num(val, &end, 10); if (*end) return -EINVAL; }
        else if (KEY("gid") && eq) { o->gid = parse_num(val, &end, 10); if (*end) return -EINVAL; }
        else if (KEY("fmask") && eq) { o->fmask = parse_num(val, &end, 8) & 0777; if (*end) return -EINVAL; }
        else if (KEY("dmask") && eq) { o->dmask = parse_num(val, &end, 8) & 0777; if (*end) return -EINVAL; }
        else if (KEY("umask") && eq) { o->fmask = o->dmask = parse_num(val, &end, 8) & 0777; if (*end) return -EINVAL; }
        else if (KEY("tzoffset") && eq) {                       /* FAT keeps local time: minutes east of UTC, e.g. -300 for EST, 540 for JST */
            const char *q = val;
            bool neg = false;
            if (*q == '-') { neg = true; q++; }
            else if (*q == '+') q++;
            u32 minutes = parse_num(q, &end, 10);
            if (end == q || *end || minutes > 24 * 60) return -EINVAL;
            o->tz_min = neg ? -(s32)minutes : (s32)minutes;
        }
        else if (KEY("usermap") && eq) {                         /* name:uid:gid/name:uid:gid */
            v->nusers = 0;
            const char *p = val;
            while (*p) {
                if (v->nusers >= FAT_MAX_USERS) return -EINVAL;
                FatUser *u = &v->users[v->nusers];
                size_t k = 0;
                while (p[k] && p[k] != ':' && p[k] != '/') k++;
                if (!k || k >= sizeof u->name || p[k] != ':') return -EINVAL;
                memcpy(u->name, p, k);
                u->name[k] = 0;
                p += k + 1;
                u->uid = parse_num(p, &end, 10);
                if (end == p || *end != ':') return -EINVAL;
                p = end + 1;
                u->gid = parse_num(p, &end, 10);
                if (end == p) return -EINVAL;
                p = end;
                v->nusers++;
                if (*p == '/') p++;
                else if (*p) return -EINVAL;
            }
        }
        else if (KEY("defaults") || KEY("noatime") || KEY("relatime") || KEY("nodev") || KEY("nosuid") || KEY("noexec") ||
                 KEY("exec") || KEY("suid") || KEY("dev") || KEY("auto") || KEY("noauto") || KEY("user") || KEY("async") ||
                 KEY("utf8") || KEY("quiet")) { }
        else {
            klog("Keine", "fat32: unknown mount option '%s'", tok);
            return -EINVAL;
        }
        #undef KEY
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------------- BPB validation */

static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static bool is_pow2(u32 x) { return x && !(x & (x - 1)); }

/* Fills the geometry from the boot sector or returns a reason why this is not a volume we dare to touch. */
static const char *parse_bpb(FatVol *v, const u8 *b, u64 dev_bytes) {
    if (b[510] != 0x55 || b[511] != 0xAA) return "no boot sector signature";
    u32 bps = le16(b + 11), spc = b[13], reserved = le16(b + 14), nfats = b[16], root_entries = le16(b + 17);
    u32 total16 = le16(b + 19), fatsz16 = le16(b + 22), total32 = le32(b + 32), fatsz32 = le32(b + 36);
    if (bps < 512 || bps > 4096 || !is_pow2(bps)) return "unsupported sector size";
    if (bps < v->ds || bps % v->ds) return "volume sector size is smaller than the device's";
    if (!is_pow2(spc) || spc > 128) return "bad cluster size";
    if (nfats < 1 || nfats > 4) return "bad number of FATs";
    if (reserved < 1) return "no reserved sectors";
    u64 total = total16 ? total16 : total32;
    u64 fatsz = fatsz16 ? fatsz16 : fatsz32;
    if (!total || !fatsz) return "empty volume or FAT";
    if (total * bps > dev_bytes) return "the volume is larger than its device";      /* never trust a size we cannot back */
    u64 root_sectors = ((u64)root_entries * 32 + bps - 1) / bps;
    u64 meta = reserved + (u64)nfats * fatsz + root_sectors;
    if (meta >= total) return "metadata larger than the volume";
    u64 nclusters = (total - meta) / spc;
    if (nclusters < 4085) return "FAT12 volumes are not supported";
    int bits = nclusters < 65525 ? 16 : 32;
    if (bits == 32) {
        if (fatsz16 || root_entries || !fatsz32) return "FAT32 geometry is inconsistent";
        if (nclusters > 0x0FFFFFF5u) return "too many clusters";
        if (le16(b + 42)) return "unknown FAT32 version";
    } else {
        if (!root_entries || !fatsz16 || (root_entries * 32) % bps) return "FAT16 geometry is inconsistent";
    }
    if ((nclusters + 2) * (bits / 8) > fatsz * bps) return "the FAT is too small for the cluster count";
    v->bits = bits;
    v->bps = bps;
    v->spc = spc;
    v->cbytes = bps * spc;
    v->cshift = (u32)__builtin_ctz(v->cbytes);
    v->reserved = reserved;
    v->nfats = nfats;
    v->fat_sectors = (u32)fatsz;
    v->root_entries = root_entries;
    v->nclusters = (u32)nclusters;
    v->vol_bytes = total * bps;
    for (u32 i = 0; i < nfats; i++) v->fat_off[i] = ((u64)reserved + (u64)i * fatsz) * bps;
    v->data_off = meta * bps;
    if (bits == 16) {
        v->root_off = ((u64)reserved + (u64)nfats * fatsz) * bps;
        v->root_bytes = (u64)root_entries * 32;
    } else {
        v->root_cluster = le32(b + 44);
        if (!fat_valid_cluster(v, v->root_cluster)) return "root cluster out of range";
        u32 ext = le16(b + 40);
        v->active_fat = (ext & 0x80) ? (ext & 0xF) : 0;          /* mirroring off: one FAT is the live one */
        if (v->active_fat >= nfats) return "bad active FAT";
        u32 fsi = le16(b + 48);
        v->fsinfo_sector = (fsi >= 1 && fsi < reserved) ? fsi : 0;
    }
    strcpy(v->fstype, bits == 32 ? "fat32" : "fat16");
    memcpy(v->label, b + (bits == 32 ? 71 : 43), 11);
    v->label[11] = 0;
    for (int i = 10; i >= 0 && v->label[i] == ' '; i--) v->label[i] = 0;
    if (!strcmp(v->label, "NO NAME")) v->label[0] = 0;
    return NULL;
}

/* ---------------------------------------------------------------------------------------------- mount */

static void root_of_device(const BlockDev *d, const BlockDev **root, u64 *start) {
    u64 s = 0;
    while (d->parent) { s += d->part_start; d = d->parent; }
    *root = d;
    *start = s;
}

/* Two mounts must never cover the same sectors (a disk and one of its partitions, for instance). */
static bool device_in_use(const BlockDev *dev) {
    const BlockDev *r1;
    u64 s1;
    root_of_device(dev, &r1, &s1);
    u64 e1 = s1 + dev->sectors;
    for (int i = 0; i < FAT_MAX_VOLS; i++) {
        FatVol *o = g_vols[i];
        if (!o) continue;
        const BlockDev *r2;
        u64 s2;
        root_of_device(o->dev, &r2, &s2);
        if (r1 == r2 && s1 < s2 + o->dev->sectors && s2 < e1) return true;
    }
    return false;
}

static void start_flusher(void);
static void fat_grant_revoke(void *holder);

static int fat_mount(const char *source, u32 flags, Mount *m) {
    fat_global_init();
    BlockDev *dev = blk_find(source);
    if (!dev) return -ENODEV;
    mutex_lock(&g_vols_lock);
    bool busy = device_in_use(dev);
    int slot = -1;
    for (int i = 0; i < FAT_MAX_VOLS && slot < 0; i++) if (!g_vols[i]) slot = i;
    mutex_unlock(&g_vols_lock);
    if (busy) return -EBUSY;
    if (slot < 0) return -ENOMEM;

    FatVol *v = kzalloc(sizeof *v);
    if (!v) return -ENOMEM;
    mutex_init(&v->lock);
    v->dev = dev;
    v->mount = m;
    v->ds = dev->sector_size;
    int rc = 0;
    if (v->ds < 512 || v->ds > 4096 || !is_pow2(v->ds)) rc = -EINVAL;
    Opts o = { 0 };
    if (!rc) rc = parse_opts(v, m->opts, &o);
    if (rc) { kfree(v); return rc; }
    if (o.userdirs && v->nusers == 0) {
        for (unsigned i = 0; i < ARRAY_SIZE(g_default_users); i++) v->users[v->nusers++] = g_default_users[i];
    }
    v->uid = o.uid; v->gid = o.gid; v->fmask = o.fmask; v->dmask = o.dmask;
    v->userdirs = o.userdirs;
    v->sync_mode = o.sync;
    v->tz_sec = o.tz_min * 60;

    u8 *bs = kmalloc(MAX(v->ds, 512u));
    const char *why = NULL;
    if (!bs) { kfree(v); return -ENOMEM; }
    rc = blk_read(dev, 0, 1, bs);
    if (rc) { klog("Keine", "fat32: cannot read %s (%d)", dev->name, rc); kfree(bs); kfree(v); return rc; }
    why = parse_bpb(v, bs, dev->sectors * dev->sector_size);
    kfree(bs);
    if (why) { klog("Keine", "fat32: %s is not a mountable FAT volume: %s", dev->name, why); kfree(v); return -EINVAL; }

    if (fc_init(v)) { kfree(v); return -ENOMEM; }

    /* FSInfo hints */
    v->free_known = false;
    v->alloc_cursor = 2;
    if (v->fsinfo_sector) {
        u8 fi[512];
        if (fc_read(v, (u64)v->fsinfo_sector * v->bps, fi, sizeof fi) == 0 && le32(fi) == 0x41615252 && le32(fi + 484) == 0x61417272 &&
            le32(fi + 508) == 0xAA550000) {
            u32 free_n = le32(fi + 488), next = le32(fi + 492);
            if (free_n <= v->nclusters) { v->free_clusters = free_n; v->free_known = true; }
            if (fat_valid_cluster(v, next)) v->alloc_cursor = next;
        }
    }

    /* clean-shutdown flags */
    u32 fat1 = 0;
    bool boot_dirty = false;
    rc = fat_read_state_bits(v, &fat1);
    if (!rc) rc = boot_flag_get(v, &boot_dirty);
    if (rc) { klog("Keine", "fat32: cannot read the state flags of %s (%d)", dev->name, rc); fc_destroy(v); kfree(v); return rc; }
    bool fat_clean = v->bits == 32 ? (fat1 & 0x08000000u) != 0 : (fat1 & 0x8000u) != 0;
    bool hard_error = v->bits == 32 ? (fat1 & 0x04000000u) == 0 : (fat1 & 0x4000u) == 0;      /* "disk errors seen" flag */
    bool unclean = !fat_clean || boot_dirty || hard_error;
    const char *why_unclean = hard_error && fat_clean && !boot_dirty ? "the volume reported disk errors on its last use"
                                                                       : "the volume was not cleanly unmounted";
    bool grant = false;
    if (o.espgrant) {                                            /* the ESP write grant: only for a read-write mount by its holder */
        rc = (o.ro || (flags & MNT_RDONLY)) ? -EINVAL : blk_esp_grant_claim(dev);
        if (rc) { fc_destroy(v); kfree(v); return rc; }
        grant = true;
    }
    v->ro = o.ro || (flags & MNT_RDONLY) || (dev->readonly && !grant);
    if (o.ro && o.rw) { klog("Keine", "fat32: conflicting ro and rw options"); fc_destroy(v); kfree(v); return -EINVAL; }
    char note[200] = "";
    if (unclean && !v->ro) {
        if (o.force) {
            v->forced = true;
            v->was_dirty = true;
            snprintf(note, sizeof note, ", FORCED read-write although %s", why_unclean);
        } else {
            v->ro = true;
            snprintf(note, sizeof note, ": %s (mount with -o force to override)", why_unclean);
        }
    } else if (unclean) {
        v->was_dirty = true;
    }
    if (grant && v->ro) {                                        /* asked for read-write on purpose: say no rather than mount it read-only */
        klog("Keine", "%s: %s is not mounted with the ESP write grant: %s (a power cut during an update?); check it first, or add force",
             v->fstype, dev->name, why_unclean);
        fc_destroy(v);
        kfree(v);
        return -EROFS;
    }
    if (v->ro) m->flags |= MNT_RDONLY;

    /* A small FAT is scanned now so that the free count is exact whatever FSInfo says; a big one trusts FSInfo (or is
     * counted when df asks). */
    if ((u64)(v->nclusters + 2) * (v->bits / 8) <= (8u << 20)) {
        u32 counted;
        if (fat_count_free(v, &counted) == 0) {
            if (v->free_known && counted != v->free_clusters)
                klog("Keine", "%s: %s: FSInfo says %u free clusters, the FAT has %u", v->fstype, dev->name, v->free_clusters, counted);
            v->free_clusters = counted;
            v->free_known = true;
            v->free_verified = true;
        }
    }

    /* root directory */
    FatNode *root = fatd_node_new(v, NULL, "/");
    if (!root) { fc_destroy(v); kfree(v); return -ENOMEM; }
    root->flags = FN_DIR | FN_ROOT | (v->bits == 16 ? FN_FIXROOT : 0);
    root->attr = FAT_ATTR_DIR;
    root->first = v->bits == 32 ? v->root_cluster : 0;
    root->owner = 0;
    v->root = root;
    rc = fatd_load(root);
    if (rc) {
        klog("Keine", "fat32: cannot read the root directory of %s (%d)", dev->name, rc);
        fatd_tree_free(root);
        fatd_node_free(root);
        fc_destroy(v);
        kfree(v);
        return rc;
    }
    root->mtime = root->ctime = root->atime = fat_now();
    VNode *rv = node_vnode(root);
    if (!rv) { fatd_tree_free(root); fatd_node_free(root); fc_destroy(v); kfree(v); return -ENOMEM; }
    m->root = rv;
    m->priv = v;
    v->last_op_ms = uptime_ms();
    if (!dev->label[0] && v->label[0]) {                         /* let lsblk and blk_find_label() see the volume label */
        strlcpy(dev->label, v->label, sizeof dev->label);
        v->label_set = true;
    }
    if (grant && blk_esp_grant_attach(dev, v, fat_grant_revoke, m->target)) {   /* the grant's file was closed meanwhile */
        v->ro = true;
        m->flags |= MNT_RDONLY;
        snprintf(note, sizeof note, ": the ESP write grant was gone before the mount could take it");
    }

    mutex_lock(&g_vols_lock);
    g_vols[slot] = v;
    mutex_unlock(&g_vols_lock);
    start_flusher();

    char tznote[40] = "";
    if (v->tz_sec) snprintf(tznote, sizeof tznote, ", times are local (UTC%+d:%02d)", v->tz_sec / 3600, (v->tz_sec < 0 ? -v->tz_sec : v->tz_sec) % 3600 / 60);
    klog("Keine", "%s: %s mounted on %s, %lu MiB, %u-byte clusters, label '%s'%s%s%s%s", v->fstype, dev->name, m->target,
         (unsigned long)(v->vol_bytes >> 20), v->cbytes, v->label[0] ? v->label : "", v->userdirs ? ", home directories per user" : "",
         v->ro ? ", read-only" : "", tznote, note);
    return 0;
}

static void fat_unmount(Mount *m) {
    FatVol *v = m->priv;
    if (!v) return;
    bootlog_unmounted(m->target);
    mutex_lock(&g_vols_lock);
    for (int i = 0; i < FAT_MAX_VOLS; i++) if (g_vols[i] == v) g_vols[i] = NULL;
    mutex_unlock(&g_vols_lock);

    mutex_lock(&v->lock);
    if (v->orphans) vol_reap(v);                                 /* free the clusters of files unlinked while open */
    v->dying = true;
    bool gone = v->dev->removed;
    int rc = gone ? 0 : vol_sync(v, true);
    if (gone) klog("Keine", "%s: %s was dropped without a write-back: its device is gone (the volume stays dirty; run a file system check when it is back)", v->fstype, v->dev->name);
    else if (rc) klog("Keine", "%s: %s: could not write everything back to the device (%d) -- run a file system check", v->fstype, v->dev->name, rc);
    else if (!v->ro) klog("Keine", "%s: %s unmounted cleanly", v->fstype, v->dev->name);
    if (v->label_set) v->dev->label[0] = 0;
    if (m->root) m->root->priv = NULL;                          /* the mount's own reference is dropped by the VFS afterwards */
    v->root->vn = NULL;
    fatd_tree_free(v->root);
    fatd_node_free(v->root);
    while (v->orphans) {
        FatNode *n = v->orphans;
        v->orphans = n->onext;
        fatd_node_free(n);
    }
    fc_destroy(v);
    mutex_unlock(&v->lock);
    blk_esp_grant_detach(v->dev, v);                             /* after the last write-back, outside the volume lock (blk_unlock.c) */
    kfree(v);
    m->priv = NULL;
}

/* The ESP write grant ended while the volume is still mounted (the open file that held it was closed: the process that asked for it
 * ended, or handed it back): everything is written back while that is still allowed, then the volume is read-only until unmounted. */
static void fat_grant_revoke(void *holder) {
    FatVol *v = holder;
    mutex_lock(&v->lock);
    if (!v->ro && !v->dying) {
        int rc = vol_sync(v, true);
        v->ro = true;
        if (v->mount) v->mount->flags |= MNT_RDONLY;
        if (rc) klog("Keine", "%s: %s: writing back before the ESP write grant ended failed (%d) -- run a file system check", v->fstype, v->dev->name, rc);
        klog("Keine", "%s: %s on %s is read-only now: the ESP write grant ended%s", v->fstype, v->dev->name,
             v->mount ? v->mount->target : "?", rc ? "" : " (everything was written back first)");
    }
    mutex_unlock(&v->lock);
}

/* umount asks first: busy while anything but the mount's own root vnode is in use, or while the data cannot be
 * written back (a failed flush must leave the volume mounted rather than lose it). */
static int fat_busy(Mount *m) {
    FatVol *v = m->priv;
    if (!v) return 0;
    if (v->nlive > 1) return 1;
    if (v->ro || v->dev->removed) return 0;                      /* nothing can be written back to a device that is gone: let the unmount through */
    mutex_lock(&v->lock);
    int rc = vol_sync(v, true);
    mutex_unlock(&v->lock);
    if (rc) klog("Keine", "%s: cannot unmount %s: writing the volume back failed (%d)", v->fstype, v->dev->name, rc);
    return rc != 0;
}

static int fat_sync_mount(Mount *m) {
    FatVol *v = m->priv;
    if (!v || v->ro) return 0;
    mutex_lock(&v->lock);
    int rc = vol_sync(v, true);
    mutex_unlock(&v->lock);
    return rc;
}

static int fat_statfs(Mount *m, struct gstatfs *out) {
    FatVol *v = m->priv;
    mutex_lock(&v->lock);
    if (v->orphans) vol_reap(v);                                 /* files unlinked while open and closed since: space is free now */
    if (!v->free_known) {
        u32 n;
        if (fat_count_free(v, &n) == 0) { v->free_clusters = n; v->free_known = true; v->free_verified = true; }
    }
    out->f_type = MSDOS_SUPER_MAGIC;
    out->f_bsize = v->cbytes;
    out->f_blocks = v->nclusters;
    out->f_bfree = out->f_bavail = v->free_known ? v->free_clusters : 0;
    out->f_namelen = FAT_MAX_NAME;
    strlcpy(out->f_fstypename, v->fstype, sizeof out->f_fstypename);
    mutex_unlock(&v->lock);
    return 0;
}

/* ---------------------------------------------------------------------------------------------- flusher */

static void flusher_main(void *arg) {
    (void)arg;
    for (;;) {
        task_sleep_ms(1000);
        mutex_lock(&g_vols_lock);
        for (int i = 0; i < FAT_MAX_VOLS; i++) {
            FatVol *v = g_vols[i];
            if (!v || v->ro || v->dev->removed) continue;        /* a vanished stick cannot be flushed to: no error line every 30 s */
            u64 now = uptime_ms();
            bool quiet = now - v->last_op_ms >= FLUSH_AGE_MS;           /* nobody has written for a while */
            bool aged = v->ndirty && now - v->dirty_since_ms >= FLUSH_AGE_MS;
            bool idle = v->media_dirty && !v->ndirty && quiet;
            if (!aged && !idle) continue;
            mutex_lock(&v->lock);
            if (!v->dying) {
                int rc = vol_sync(v, quiet);
                if (rc && now - v->err_logged_ms >= 30000) {         /* a broken device must not flood the log every second */
                    v->err_logged_ms = now;
                    klog("Keine", "%s: periodic flush of %s failed (%d)", v->fstype, v->dev->name, rc);
                }
            }
            mutex_unlock(&v->lock);
        }
        mutex_unlock(&g_vols_lock);
    }
}

static void start_flusher(void) {
    if (g_flusher_started) return;
    g_flusher_started = true;
    task_create_detached("fat-flush", flusher_main, NULL);
}

const FsType g_fat32_type = {
    .name = "fat32", .mount = fat_mount, .unmount = fat_unmount, .statfs = fat_statfs, .sync = fat_sync_mount,
    .busy = fat_busy, .flags = FS_FIXED_OWNER,
};

/* aliases so that "mount -t vfat" and "-t fat16" work */
const FsType g_vfat_type = {
    .name = "vfat", .mount = fat_mount, .unmount = fat_unmount, .statfs = fat_statfs, .sync = fat_sync_mount,
    .busy = fat_busy, .flags = FS_FIXED_OWNER,
};

const FsType g_fat16_type = {
    .name = "fat16", .mount = fat_mount, .unmount = fat_unmount, .statfs = fat_statfs, .sync = fat_sync_mount,
    .busy = fat_busy, .flags = FS_FIXED_OWNER,
};
