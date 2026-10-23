/* VFS core: vnode lifetime, path resolution with mounts and symlinks, permission checks, open files. */
#include "vfs.h"
#include "kernel.h"
#include "mm.h"
#include "cpu.h"

const Cred g_root_cred = { 0, 0 };

static VNode *g_root;
static Mutex g_ns;                      /* serialises namespace operations */
static const FsType *g_fstypes[8];
static int g_nfstypes;
static Mount *g_mounts[32];
static int g_nmounts;
static u32 g_next_mount_id = 1;

/* ------------------------------------------------------------------ vnodes */

VNode *vnode_alloc(const VNodeOps *ops, Mount *m, u32 mode) {
    VNode *v = kzalloc(sizeof *v);
    if (!v) return NULL;
    v->ops = ops;
    v->mount = m;
    v->mode = mode;
    v->refs = 1;
    v->nlink = 1;
    v->atime = v->mtime = v->ctime = time_now_sec();
    return v;
}

void vnode_get(VNode *v) {
    u64 f = irq_save();
    v->refs++;
    irq_restore(f);
}

void vnode_put(VNode *v) {
    if (!v) return;
    u64 f = irq_save();
    int r = --v->refs;
    irq_restore(f);
    if (r > 0) return;
    if (r < 0) panic("vnode_put: refcount underflow (ino %lu)", (unsigned long)v->ino);
    if (v->ops && v->ops->release) v->ops->release(v);
    kfree(v);
}

void vnode_stat(VNode *v, struct gstat *st) {
    memset(st, 0, sizeof *st);
    st->st_dev = v->mount ? v->mount->id : 0;
    st->st_ino = v->ino;
    st->st_mode = v->mode;
    st->st_nlink = v->nlink;
    st->st_uid = v->uid;
    st->st_gid = v->gid;
    st->st_rdev = v->rdev;
    st->st_size = (s64)v->size;
    st->st_blksize = 4096;
    st->st_blocks = (v->size + 511) / 512;
    st->st_atime = (s64)v->atime;
    st->st_mtime = (s64)v->mtime;
    st->st_ctime = (s64)v->ctime;
}

int vfs_access(VNode *v, const Cred *c, int mask) {
    if (!c) return 0;
    u32 mode = v->mode;
    if (c->uid == 0) {
        if ((mask & X_OK) && !S_ISDIR(mode) && !(mode & 0111)) return -EACCES;
        return 0;
    }
    u32 bits = (c->uid == v->uid) ? (mode >> 6) : (c->gid == v->gid) ? (mode >> 3) : mode;
    return ((u32)mask & ~bits & 7) ? -EACCES : 0;
}

/* ------------------------------------------------------------------ registry and mounts */

int vfs_register_fs(const FsType *t) {
    if (g_nfstypes >= (int)ARRAY_SIZE(g_fstypes)) return -ENOMEM;
    g_fstypes[g_nfstypes++] = t;
    return 0;
}

VNode *vfs_root(void) { return g_root; }

/* Resolves path to a directory vnode, bypassing permissions (used by mount). */
static int resolve(const char *path, VNode *cwd, const Cred *c, int flags, bool want_parent,
                   VNode **node_out, VNode **parent_out, char *name_out);

int vfs_mount(const char *fstype, const char *source, const char *target, u32 flags) {
    const FsType *t = NULL;
    for (int i = 0; i < g_nfstypes; i++)
        if (!strcmp(g_fstypes[i]->name, fstype)) t = g_fstypes[i];
    if (!t) return -ENODEV;
    if (g_nmounts >= (int)ARRAY_SIZE(g_mounts)) return -ENOMEM;

    VNode *covered = NULL;
    mutex_lock(&g_ns);
    if (g_root) {
        int rc = resolve(target, NULL, NULL, LOOKUP_DIR, false, &covered, NULL, NULL);
        if (rc) { mutex_unlock(&g_ns); return rc; }
        if (covered->mounted) { vnode_put(covered); mutex_unlock(&g_ns); return -EBUSY; }
    } else if (strcmp(target, "/")) {
        mutex_unlock(&g_ns);
        return -ENOENT;
    }

    Mount *m = kzalloc(sizeof *m);
    if (!m) { if (covered) vnode_put(covered); mutex_unlock(&g_ns); return -ENOMEM; }
    strlcpy(m->target, target, sizeof m->target);
    strlcpy(m->source, source ? source : "", sizeof m->source);
    m->type = t;
    m->flags = flags;
    m->id = g_next_mount_id++;
    int rc = t->mount(source, flags, m);
    if (rc) { kfree(m); if (covered) vnode_put(covered); mutex_unlock(&g_ns); return rc; }
    m->root->mount = m;
    if (covered) {
        m->covered = covered;           /* keeps the reference we took */
        covered->mounted = m->root;
    } else {
        g_root = m->root;
    }
    g_mounts[g_nmounts++] = m;
    mutex_unlock(&g_ns);
    return 0;
}

int vfs_umount(const char *target) {
    mutex_lock(&g_ns);
    for (int i = 0; i < g_nmounts; i++) {
        Mount *m = g_mounts[i];
        if (strcmp(m->target, target) || !m->covered) continue;
        if (m->root->refs > 1) { mutex_unlock(&g_ns); return -EBUSY; }
        m->covered->mounted = NULL;
        vnode_put(m->covered);
        if (m->type->unmount) m->type->unmount(m);
        vnode_put(m->root);
        g_mounts[i] = g_mounts[--g_nmounts];
        kfree(m);
        mutex_unlock(&g_ns);
        return 0;
    }
    mutex_unlock(&g_ns);
    return -EINVAL;
}

int vfs_mount_list(int idx, char *target, size_t tn, char *type, size_t yn, char *source, size_t sn) {
    if (idx < 0 || idx >= g_nmounts) return 0;
    strlcpy(target, g_mounts[idx]->target, tn);
    strlcpy(type, g_mounts[idx]->type->name, yn);
    strlcpy(source, g_mounts[idx]->source, sn);
    return 1;
}

/* ------------------------------------------------------------------ path resolution */

static int dir_lookup(VNode *dir, const Cred *c, const char *name, VNode **out) {
    if (!S_ISDIR(dir->mode)) return -ENOTDIR;
    int rc = vfs_access(dir, c, X_OK);
    if (rc) return rc;
    if (!dir->ops->lookup) return -ENOENT;
    return dir->ops->lookup(dir, name, out);
}

static int resolve(const char *path, VNode *cwd, const Cred *c, int flags, bool want_parent,
                   VNode **node_out, VNode **parent_out, char *name_out) {
    if (!path || !*path) return -ENOENT;
    char buf[PATH_MAX];
    if (strlcpy(buf, path, sizeof buf) >= sizeof buf) return -ENAMETOOLONG;
    VNode *cur = (buf[0] == '/' || !cwd) ? g_root : cwd;
    if (!cur) return -ENOENT;
    vnode_get(cur);
    char *p = buf;
    int links = 0;

    for (;;) {
        while (*p == '/') p++;
        if (!*p) {
            if (want_parent) { vnode_put(cur); return -EINVAL; }
            break;
        }
        char *end = p;
        while (*end && *end != '/') end++;
        size_t n = (size_t)(end - p);
        if (n > GNAME_MAX) { vnode_put(cur); return -ENAMETOOLONG; }
        char comp[GNAME_MAX + 1];
        memcpy(comp, p, n);
        comp[n] = 0;
        char *rest = end;
        while (*rest == '/') rest++;
        bool last = *rest == 0;
        bool slash = *end == '/';

        if (want_parent && last) {
            if (!S_ISDIR(cur->mode)) { vnode_put(cur); return -ENOTDIR; }
            strcpy(name_out, comp);
            *parent_out = cur;
            return 0;
        }
        if (!strcmp(comp, ".")) { p = rest; continue; }

        VNode *child = NULL;
        int rc;
        if (!strcmp(comp, "..")) {
            while (cur->mount && cur == cur->mount->root && cur->mount->covered) {
                VNode *cv = cur->mount->covered;
                vnode_get(cv);
                vnode_put(cur);
                cur = cv;
            }
            rc = dir_lookup(cur, c, "..", &child);
        } else {
            rc = dir_lookup(cur, c, comp, &child);
        }
        if (rc) { vnode_put(cur); return rc; }
        while (child->mounted) {
            VNode *m = child->mounted;
            vnode_get(m);
            vnode_put(child);
            child = m;
        }

        if (S_ISLNK(child->mode) && (!last || slash || !(flags & LOOKUP_NOFOLLOW))) {
            if (++links > 8) { vnode_put(child); vnode_put(cur); return -ELOOP; }
            char target[PATH_MAX];
            ssize_t tn = child->ops->readlink ? child->ops->readlink(child, target, sizeof target - 1) : -EINVAL;
            vnode_put(child);
            if (tn < 0) { vnode_put(cur); return (int)tn; }
            target[tn] = 0;
            char nb[PATH_MAX];
            if (strlcpy(nb, target, sizeof nb) >= sizeof nb) { vnode_put(cur); return -ENAMETOOLONG; }
            if (*rest && (strlcat(nb, "/", sizeof nb) >= sizeof nb || strlcat(nb, rest, sizeof nb) >= sizeof nb)) {
                vnode_put(cur);
                return -ENAMETOOLONG;
            }
            if (target[0] == '/') {
                vnode_put(cur);
                cur = g_root;
                vnode_get(cur);
            }
            strcpy(buf, nb);
            p = buf;
            continue;
        }

        vnode_put(cur);
        cur = child;
        p = rest;
        if ((!last || slash) && !S_ISDIR(cur->mode)) { vnode_put(cur); return -ENOTDIR; }
    }

    if ((flags & LOOKUP_DIR) && !S_ISDIR(cur->mode)) { vnode_put(cur); return -ENOTDIR; }
    *node_out = cur;
    return 0;
}

int vfs_lookup(const char *path, VNode *cwd, const Cred *c, int flags, VNode **out) {
    mutex_lock(&g_ns);
    int rc = resolve(path, cwd, c, flags, false, out, NULL, NULL);
    mutex_unlock(&g_ns);
    return rc;
}

int vfs_lookup_parent(const char *path, VNode *cwd, const Cred *c, VNode **dir, char *name) {
    mutex_lock(&g_ns);
    int rc = resolve(path, cwd, c, 0, true, NULL, dir, name);
    mutex_unlock(&g_ns);
    return rc;
}

/* ------------------------------------------------------------------ open files */

File *file_new(VNode *v, int flags, const FileOps *fops, void *priv) {
    File *f = kzalloc(sizeof *f);
    if (!f) return NULL;
    f->vn = v;
    f->flags = flags;
    f->refs = 1;
    f->fops = fops;
    f->priv = priv;
    return f;
}

void file_get(File *f) {
    u64 fl = irq_save();
    f->refs++;
    irq_restore(fl);
}

void file_put(File *f) {
    if (!f) return;
    u64 fl = irq_save();
    int r = --f->refs;
    irq_restore(fl);
    if (r > 0) return;
    if (f->fops && f->fops->close) f->fops->close(f);
    else if (f->vn && f->vn->ops && f->vn->ops->closed) f->vn->ops->closed(f->vn, f);
    if (f->vn) vnode_put(f->vn);
    kfree(f);
}

int vfs_open(const char *path, int flags, u32 mode, const Cred *c, VNode *cwd, File **out) {
    VNode *v = NULL;
    int rc = vfs_lookup(path, cwd, c, (flags & O_NOFOLLOW) ? LOOKUP_NOFOLLOW : 0, &v);

    if (rc == -ENOENT && (flags & O_CREAT)) {
        VNode *dir;
        char name[GNAME_MAX + 1];
        rc = vfs_lookup_parent(path, cwd, c, &dir, name);
        if (rc) return rc;
        rc = vfs_access(dir, c, W_OK | X_OK);
        if (rc == 0 && !dir->ops->create) rc = -EPERM;
        if (rc == 0) {
            mutex_lock(&g_ns);
            rc = dir->ops->create(dir, name, S_IFREG | (mode & 07777), &v);
            mutex_unlock(&g_ns);
        }
        if (rc == 0 && c) { v->uid = c->uid; v->gid = c->gid; }
        vnode_put(dir);
    } else if (rc == 0 && (flags & O_CREAT) && (flags & O_EXCL)) {
        vnode_put(v);
        return -EEXIST;
    }
    if (rc) return rc;

    int want = ((flags & O_ACCMODE) == O_RDONLY) ? R_OK : ((flags & O_ACCMODE) == O_WRONLY) ? W_OK : (R_OK | W_OK);
    if (flags & O_TRUNC) want |= W_OK;
    if (vfs_access(v, c, want)) { vnode_put(v); return -EACCES; }
    if (S_ISDIR(v->mode) && (flags & O_ACCMODE) != O_RDONLY) { vnode_put(v); return -EISDIR; }
    if ((flags & O_DIRECTORY) && !S_ISDIR(v->mode)) { vnode_put(v); return -ENOTDIR; }
    if (S_ISLNK(v->mode)) { vnode_put(v); return -ELOOP; }
    if ((flags & O_TRUNC) && S_ISREG(v->mode) && v->ops->truncate) v->ops->truncate(v, 0);

    File *f = file_new(v, flags, NULL, NULL);
    if (!f) { vnode_put(v); return -ENOMEM; }
    if (v->ops->opened) v->ops->opened(v, f);
    *out = f;
    return 0;
}

ssize_t vfs_read(File *f, void *buf, size_t n) {
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (f->fops && f->fops->read) return f->fops->read(f, buf, n);
    VNode *v = f->vn;
    if (S_ISDIR(v->mode)) return -EISDIR;
    if (!v->ops->read) return -EINVAL;
    ssize_t r = v->ops->read(v, f, buf, n, f->off);
    if (r > 0 && S_ISREG(v->mode)) f->off += (u64)r;
    return r;
}

ssize_t vfs_write(File *f, const void *buf, size_t n) {
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (f->fops && f->fops->write) return f->fops->write(f, buf, n);
    VNode *v = f->vn;
    if (S_ISDIR(v->mode)) return -EISDIR;
    if (!v->ops->write) return -EINVAL;
    if ((f->flags & O_APPEND) && S_ISREG(v->mode)) f->off = v->size;
    ssize_t r = v->ops->write(v, f, buf, n, f->off);
    if (r > 0 && S_ISREG(v->mode)) f->off += (u64)r;
    return r;
}

s64 vfs_lseek(File *f, s64 off, int whence) {
    VNode *v = f->vn;
    if (f->fops || S_ISCHR(v->mode) || S_ISFIFO(v->mode)) return -ESPIPE;
    s64 base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (s64)f->off; break;
    case SEEK_END: base = (s64)v->size; break;
    default: return -EINVAL;
    }
    if (base + off < 0) return -EINVAL;
    f->off = (u64)(base + off);
    return (s64)f->off;
}

int vfs_ioctl(File *f, unsigned long req, void *arg) {
    if (f->fops && f->fops->ioctl) return f->fops->ioctl(f, req, arg);
    if (f->vn->ops->ioctl) return f->vn->ops->ioctl(f->vn, f, req, arg);
    return -ENOTTY;
}

int vfs_poll(File *f) {
    if (f->fops) return f->fops->poll ? f->fops->poll(f) : (POLLIN | POLLOUT);
    if (f->vn && f->vn->ops && f->vn->ops->poll) return f->vn->ops->poll(f->vn, f);
    return POLLIN | POLLOUT;
}

int vfs_ftruncate(File *f, u64 size) {
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (!S_ISREG(f->vn->mode) || !f->vn->ops->truncate) return -EINVAL;
    return f->vn->ops->truncate(f->vn, size);
}

int vfs_readdir(File *f, struct gdirent *out) {
    VNode *v = f->vn;
    if (!S_ISDIR(v->mode)) return -ENOTDIR;
    DirEntry de;
    u64 idx = f->off;
    if (idx == 0) {
        strcpy(de.name, ".");
        de.ino = v->ino;
        de.type = DT_DIR;
    } else if (idx == 1) {
        strcpy(de.name, "..");
        de.ino = v->ino;
        de.type = DT_DIR;
        VNode *p = NULL;
        if (v->ops->lookup && v->ops->lookup(v, "..", &p) == 0) { de.ino = p->ino; vnode_put(p); }
    } else {
        if (!v->ops->readdir) return 0;
        int rc = v->ops->readdir(v, idx - 2, &de);
        if (rc <= 0) return rc;
    }
    memset(out, 0, sizeof *out);
    out->d_ino = de.ino;
    out->d_type = de.type;
    out->d_namlen = (u32)strlen(de.name);
    strlcpy(out->d_name, de.name, sizeof out->d_name);
    f->off++;
    return 1;
}

/* ------------------------------------------------------------------ namespace operations */

int vfs_stat(const char *path, VNode *cwd, const Cred *c, int flags, struct gstat *st) {
    VNode *v;
    int rc = vfs_lookup(path, cwd, c, flags, &v);
    if (rc) return rc;
    vnode_stat(v, st);
    vnode_put(v);
    return 0;
}

static int parent_for_modify(const char *path, VNode *cwd, const Cred *c, VNode **dir, char *name) {
    int rc = vfs_lookup_parent(path, cwd, c, dir, name);
    if (rc) return rc;
    if (!strcmp(name, ".") || !strcmp(name, "..")) { vnode_put(*dir); return -EINVAL; }
    rc = vfs_access(*dir, c, W_OK | X_OK);
    if (rc) vnode_put(*dir);
    return rc;
}

/* Like Linux, report EEXIST for a name that is already there before complaining about write access to its parent. */
static int exists_check(const char *path, VNode *cwd, const Cred *c) {
    VNode *v;
    if (vfs_lookup(path, cwd, c, LOOKUP_NOFOLLOW, &v) != 0) return 0;
    vnode_put(v);
    return -EEXIST;
}

int vfs_mkdir(const char *path, u32 mode, const Cred *c, VNode *cwd) {
    VNode *dir, *nv = NULL;
    char name[GNAME_MAX + 1];
    int rc = exists_check(path, cwd, c);
    if (rc) return rc;
    rc = parent_for_modify(path, cwd, c, &dir, name);
    if (rc) return rc;
    if (!dir->ops->mkdir) { vnode_put(dir); return -EPERM; }
    mutex_lock(&g_ns);
    rc = dir->ops->mkdir(dir, name, S_IFDIR | (mode & 07777), &nv);
    mutex_unlock(&g_ns);
    if (rc == 0) {
        if (c) { nv->uid = c->uid; nv->gid = c->gid; }
        vnode_put(nv);
    }
    vnode_put(dir);
    return rc;
}

int vfs_unlink(const char *path, const Cred *c, VNode *cwd) {
    VNode *dir;
    char name[GNAME_MAX + 1];
    int rc = parent_for_modify(path, cwd, c, &dir, name);
    if (rc) return rc;
    mutex_lock(&g_ns);
    rc = dir->ops->unlink ? dir->ops->unlink(dir, name) : -EPERM;
    mutex_unlock(&g_ns);
    vnode_put(dir);
    return rc;
}

int vfs_rmdir(const char *path, const Cred *c, VNode *cwd) {
    VNode *dir;
    char name[GNAME_MAX + 1];
    int rc = parent_for_modify(path, cwd, c, &dir, name);
    if (rc) return rc;
    mutex_lock(&g_ns);
    rc = dir->ops->rmdir ? dir->ops->rmdir(dir, name) : -EPERM;
    mutex_unlock(&g_ns);
    vnode_put(dir);
    return rc;
}

int vfs_rename(const char *from, const char *to, const Cred *c, VNode *cwd) {
    VNode *od, *nd;
    char on[GNAME_MAX + 1], nn[GNAME_MAX + 1];
    int rc = parent_for_modify(from, cwd, c, &od, on);
    if (rc) return rc;
    rc = parent_for_modify(to, cwd, c, &nd, nn);
    if (rc) { vnode_put(od); return rc; }
    if (od->mount != nd->mount) rc = -EXDEV;
    else {
        mutex_lock(&g_ns);
        rc = od->ops->rename ? od->ops->rename(od, on, nd, nn) : -EPERM;
        mutex_unlock(&g_ns);
    }
    vnode_put(od);
    vnode_put(nd);
    return rc;
}

int vfs_symlink(const char *target, const char *path, const Cred *c, VNode *cwd) {
    VNode *dir, *nv = NULL;
    char name[GNAME_MAX + 1];
    int rc = exists_check(path, cwd, c);
    if (rc) return rc;
    rc = parent_for_modify(path, cwd, c, &dir, name);
    if (rc) return rc;
    mutex_lock(&g_ns);
    rc = dir->ops->symlink ? dir->ops->symlink(dir, name, target, &nv) : -EPERM;
    mutex_unlock(&g_ns);
    if (rc == 0) {
        if (c) { nv->uid = c->uid; nv->gid = c->gid; }
        vnode_put(nv);
    }
    vnode_put(dir);
    return rc;
}

ssize_t vfs_readlink(const char *path, char *buf, size_t n, VNode *cwd, const Cred *c) {
    VNode *v;
    int rc = vfs_lookup(path, cwd, c, LOOKUP_NOFOLLOW, &v);
    if (rc) return rc;
    ssize_t r = (S_ISLNK(v->mode) && v->ops->readlink) ? v->ops->readlink(v, buf, n) : -EINVAL;
    vnode_put(v);
    return r;
}

int vfs_truncate(const char *path, u64 size, const Cred *c, VNode *cwd) {
    VNode *v;
    int rc = vfs_lookup(path, cwd, c, 0, &v);
    if (rc) return rc;
    if (S_ISDIR(v->mode)) rc = -EISDIR;
    else if (vfs_access(v, c, W_OK)) rc = -EACCES;
    else rc = v->ops->truncate ? v->ops->truncate(v, size) : -EINVAL;
    vnode_put(v);
    return rc;
}

int vfs_chmod(const char *path, u32 mode, const Cred *c, VNode *cwd) {
    VNode *v;
    int rc = vfs_lookup(path, cwd, c, 0, &v);
    if (rc) return rc;
    if (c && c->uid != 0 && c->uid != v->uid) rc = -EPERM;
    else {
        v->mode = (v->mode & S_IFMT) | (mode & 07777);
        v->ctime = time_now_sec();
        if (v->ops->chmod) v->ops->chmod(v, v->mode);
    }
    vnode_put(v);
    return rc;
}

int vfs_chown(const char *path, u32 uid, u32 gid, const Cred *c, VNode *cwd) {
    VNode *v;
    int rc = vfs_lookup(path, cwd, c, 0, &v);
    if (rc) return rc;
    if (c && c->uid != 0) rc = -EPERM;
    else {
        if (uid != (u32)-1) v->uid = uid;
        if (gid != (u32)-1) v->gid = gid;
        v->ctime = time_now_sec();
        if (v->ops->chown) v->ops->chown(v, v->uid, v->gid);
    }
    vnode_put(v);
    return rc;
}

/* ------------------------------------------------------------------ kernel conveniences */

int vfs_read_file(const char *path, void **data, size_t *size) {
    File *f;
    int rc = vfs_open(path, O_RDONLY, 0, NULL, NULL, &f);
    if (rc) return rc;
    size_t cap = f->vn->size ? f->vn->size : 4096, len = 0;
    u8 *buf = kmalloc(cap + 1);
    if (!buf) { file_put(f); return -ENOMEM; }
    for (;;) {
        if (len == cap) {
            cap *= 2;
            u8 *nb = krealloc(buf, cap + 1);
            if (!nb) { kfree(buf); file_put(f); return -ENOMEM; }
            buf = nb;
        }
        ssize_t r = vfs_read(f, buf + len, cap - len);
        if (r < 0) { kfree(buf); file_put(f); return (int)r; }
        if (r == 0) break;
        len += (size_t)r;
    }
    buf[len] = 0;
    file_put(f);
    *data = buf;
    *size = len;
    return 0;
}

int vfs_write_file(const char *path, const void *data, size_t size, u32 mode) {
    File *f;
    int rc = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, mode, NULL, NULL, &f);
    if (rc) return rc;
    ssize_t w = size ? vfs_write(f, data, size) : 0;
    file_put(f);
    return w < 0 ? (int)w : 0;
}

int vfs_mkdir_p(const char *path, u32 mode) {
    char buf[PATH_MAX];
    if (strlcpy(buf, path, sizeof buf) >= sizeof buf) return -ENAMETOOLONG;
    for (char *p = buf + 1; ; p++) {
        if (*p == '/' || *p == 0) {
            char save = *p;
            *p = 0;
            int rc = vfs_mkdir(buf, mode, NULL, NULL);
            if (rc && rc != -EEXIST) return rc;
            *p = save;
            if (!save) break;
        }
    }
    return 0;
}

void vfs_init(void) {
    mutex_init(&g_ns);
    vfs_register_fs(&g_tmpfs_type);
    vfs_register_fs(&g_devfs_type);
    vfs_register_fs(&g_procfs_type);
    vfs_register_fs(&g_fat32_type);
}
