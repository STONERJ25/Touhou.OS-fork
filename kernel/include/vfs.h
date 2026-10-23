/*
 * Virtual file system.
 *
 * Reference rules: every function that returns a VNode* or File* hands the caller one reference
 * (drop it with vnode_put / file_put). A directory entry owns one reference to the vnode it names, so a vnode
 * is freed (ops->release) once it is unlinked and nobody holds it open.
 */
#ifndef K_VFS_H
#define K_VFS_H

#include "types.h"
#include "task.h"
#include "abi.h"
#include "syscall.h"

#define PATH_MAX 1024

typedef struct VNode VNode;
typedef struct VNodeOps VNodeOps;
typedef struct Mount Mount;
typedef struct File File;
typedef struct FsType FsType;

#define NGROUPS_MAX 16

typedef struct Cred {
    u32 uid, gid;
    u32 ngroups;                    /* supplementary groups */
    u32 groups[NGROUPS_MAX];
} Cred;

static inline bool cred_in_group(const Cred *c, u32 gid) {
    if (c->gid == gid) return true;
    for (u32 i = 0; i < c->ngroups; i++) if (c->groups[i] == gid) return true;
    return false;
}

typedef struct DirEntry {
    u64 ino;
    u32 type;               /* DT_* */
    char name[GNAME_MAX + 1];
} DirEntry;

struct VNode {
    u64 ino;
    u32 mode;               /* S_IF* | permission bits */
    u32 uid, gid;
    u32 nlink;
    u64 size;
    u64 atime, mtime, ctime;
    u64 rdev;
    int refs;
    const VNodeOps *ops;
    Mount *mount;
    void *priv;
    VNode *mounted;         /* root of the filesystem mounted on this directory, if any */
};

struct VNodeOps {
    /* data */
    ssize_t (*read)(VNode *v, File *f, void *buf, size_t n, u64 off);
    ssize_t (*write)(VNode *v, File *f, const void *buf, size_t n, u64 off);
    int     (*truncate)(VNode *v, u64 size);
    int     (*ioctl)(VNode *v, File *f, unsigned long req, void *arg);
    int     (*mmap_phys)(VNode *v, u64 off, paddr_t *phys);          /* device memory that user space may map */
    void    (*opened)(VNode *v, File *f);
    void    (*closed)(VNode *v, File *f);
    int     (*poll)(VNode *v, File *f);                              /* POLLIN | POLLOUT readiness */
    /* directories */
    int     (*lookup)(VNode *dir, const char *name, VNode **out);    /* 0 + referenced vnode, or -ENOENT */
    int     (*create)(VNode *dir, const char *name, u32 mode, VNode **out);
    int     (*mkdir)(VNode *dir, const char *name, u32 mode, VNode **out);
    int     (*symlink)(VNode *dir, const char *name, const char *target, VNode **out);
    int     (*unlink)(VNode *dir, const char *name);                 /* files and symlinks */
    int     (*rmdir)(VNode *dir, const char *name);
    int     (*rename)(VNode *olddir, const char *oldname, VNode *newdir, const char *newname);
    int     (*readdir)(VNode *dir, u64 cookie, DirEntry *out);       /* 1 = entry filled, 0 = end; cookie = entry index */
    ssize_t (*readlink)(VNode *v, char *buf, size_t n);
    int     (*chmod)(VNode *v, u32 mode);
    int     (*chown)(VNode *v, u32 uid, u32 gid);
    int     (*sync)(VNode *v);
    void    (*release)(VNode *v);                                    /* last reference dropped; VFS frees the VNode */
};

struct Mount {
    char target[PATH_MAX / 4];
    const FsType *type;
    VNode *root;
    VNode *covered;         /* directory this is mounted on (referenced) */
    void *priv;
    char source[64];
    char opts[160];         /* comma-separated mount options, handed to the filesystem's mount() */
    u32 flags;              /* MNT_* (abi.h) */
    u32 id;
};

struct FsType {
    const char *name;
    int  (*mount)(const char *source, u32 flags, Mount *m);         /* fills m->root, m->priv */
    void (*unmount)(Mount *m);
    int  (*statfs)(Mount *m, struct gstatfs *out);                  /* optional: fills f_type, f_bsize and the block/file counts */
    int  (*sync)(Mount *m);                                         /* optional: write everything back to the device */
    int  (*busy)(Mount *m);                                         /* optional: non-zero when the filesystem cannot be unmounted */
    u32  flags;                                                     /* FS_* */
};
#define FS_FIXED_OWNER 1    /* uid/gid/mode come from the filesystem: the VFS must not stamp the creator on new nodes */

typedef struct FileOps {
    ssize_t (*read)(File *f, void *buf, size_t n);
    ssize_t (*write)(File *f, const void *buf, size_t n);
    int     (*ioctl)(File *f, unsigned long req, void *arg);
    void    (*close)(File *f);
    int     (*poll)(File *f);
} FileOps;

struct File {
    VNode *vn;
    u64 off;
    int flags;
    int refs;
    const FileOps *fops;    /* overrides the vnode ops (pipes, anonymous files) */
    void *priv;
};

/* ---- vnodes and open files ---- */
VNode *vnode_alloc(const VNodeOps *ops, Mount *m, u32 mode);       /* refs = 1, nlink = 1 */
void   vnode_get(VNode *v);
void   vnode_put(VNode *v);
void   vnode_stat(VNode *v, struct gstat *st);
File  *file_new(VNode *v, int flags, const FileOps *fops, void *priv);   /* takes over the caller's vnode reference */
void   file_get(File *f);
void   file_put(File *f);

/* ---- namespace ---- */
#define LOOKUP_NOFOLLOW 1
#define LOOKUP_DIR      2           /* final component must be a directory */

void   vfs_init(void);
int    vfs_register_fs(const FsType *t);
int    vfs_mount(const char *fstype, const char *source, const char *target, u32 flags);
int    vfs_mount_opts(const char *fstype, const char *source, const char *target, u32 flags, const char *opts);
int    vfs_umount(const char *target);
int    vfs_sync_all(void);                                              /* flush every writable filesystem (sync(2), shutdown) */
int    vfs_statfs(const char *path, VNode *cwd, const Cred *c, struct gstatfs *out);
u32    vfs_mount_flags(int idx);                                        /* flags of the idx-th mount (same order as vfs_mount_list) */
VNode *vfs_root(void);

int    vfs_lookup(const char *path, VNode *cwd, const Cred *c, int flags, VNode **out);
int    vfs_lookup_parent(const char *path, VNode *cwd, const Cred *c, VNode **dir, char *name);
int    vfs_access(VNode *v, const Cred *c, int mask);                   /* R_OK | W_OK | X_OK; c == NULL means root */

int    vfs_open(const char *path, int flags, u32 mode, const Cred *c, VNode *cwd, File **out);
ssize_t vfs_read(File *f, void *buf, size_t n);
ssize_t vfs_write(File *f, const void *buf, size_t n);
s64    vfs_lseek(File *f, s64 off, int whence);
int    vfs_ioctl(File *f, unsigned long req, void *arg);
int    vfs_poll(File *f);
int    vfs_readdir(File *f, struct gdirent *out);                       /* 1 entry, 0 end, <0 error */
int    vfs_ftruncate(File *f, u64 size);

int    vfs_stat(const char *path, VNode *cwd, const Cred *c, int flags, struct gstat *st);
int    vfs_mkdir(const char *path, u32 mode, const Cred *c, VNode *cwd);
int    vfs_unlink(const char *path, const Cred *c, VNode *cwd);
int    vfs_rmdir(const char *path, const Cred *c, VNode *cwd);
int    vfs_rename(const char *from, const char *to, const Cred *c, VNode *cwd);
int    vfs_symlink(const char *target, const char *path, const Cred *c, VNode *cwd);
ssize_t vfs_readlink(const char *path, char *buf, size_t n, VNode *cwd, const Cred *c);
int    vfs_truncate(const char *path, u64 size, const Cred *c, VNode *cwd);
int    vfs_chmod(const char *path, u32 mode, const Cred *c, VNode *cwd);
int    vfs_chown(const char *path, u32 uid, u32 gid, const Cred *c, VNode *cwd);

/* Convenience for the kernel itself (root credentials, absolute paths). */
int    vfs_read_file(const char *path, void **data, size_t *size);      /* kmalloc'd; caller kfree()s */
int    vfs_write_file(const char *path, const void *data, size_t size, u32 mode);
int    vfs_mkdir_p(const char *path, u32 mode);

/* ---- pipes ---- */
int    vfs_pipe(File **rd, File **wr);

/* ---- device nodes ---- */
int    devfs_register(const char *path, u32 mode, const VNodeOps *ops, void *priv, u64 rdev);   /* "tty", "input/events" */
int    devfs_unregister(const char *path);
int    devfs_chown(const char *path, u32 uid, u32 gid);
#define GID_SHRINE 100              /* group "shrine": the people allowed to run the desktop (display and input devices) */
VNode *devfs_root(void);

/* ---- concrete filesystems ---- */
extern const FsType g_tmpfs_type;
extern const FsType g_devfs_type;
extern const FsType g_procfs_type;
extern const FsType g_fat32_type;
int    tmpfs_add_special(VNode *dir, const char *name, u32 mode, const VNodeOps *ops, void *priv, u64 rdev, VNode **out);
void   tmpfs_stats(u64 *pages_used);

/* Adds a read-only text file /proc/<name> generated on every read (drivers use this for /proc/usb and the like). */
int    procfs_register(const char *name, size_t (*gen)(char *buf, size_t cap));

void   initrd_unpack(const void *tar, size_t size);
void   layout_create(void);                                  /* mounts "/" and builds the Gensokyo directory tree */
void   devfs_register_standard(void);
int    vfs_mount_list(int idx, char *target, size_t tn, char *type, size_t yn, char *source, size_t sn);

extern const Cred g_root_cred;
u64    time_now_sec(void);
void   rtc_init(void);

#endif
