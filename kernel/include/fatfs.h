/*
 * FAT16/FAT32 (Keine's ledger of the Human Village). Internal header shared by kernel/fs/fat*.c; nothing outside the
 * filesystem includes this.
 *
 * Layers, bottom to top:
 *   fat_cache.c   write-back page cache over the block device (4 KiB pages, flush classes for write ordering)
 *   fat_chain.c   FAT table access, cluster allocator, per-file extent cache
 *   fat_name.c    UTF-8/UTF-16, case folding, 8.3 short names, LFN checksums (pure functions)
 *   fat_dir.c     directory listings: parsing, hashing, slot allocation, entry creation/removal
 *   fat32.c       VFS operations, mount/unmount, flusher thread, statfs
 *
 * Locking: one mutex per volume (FatVol.lock) protects everything below it. VNode release is lock-free (it only
 * touches state under irq_save) and defers cleanup of unlinked files to the next locked operation.
 */
#ifndef K_FATFS_H
#define K_FATFS_H

#include "vfs.h"
#include "blk.h"
#include "task.h"
#include "mm.h"
#include "kernel.h"

#define FAT_ATTR_RO     0x01
#define FAT_ATTR_HIDDEN 0x02
#define FAT_ATTR_SYSTEM 0x04
#define FAT_ATTR_VOLUME 0x08
#define FAT_ATTR_DIR    0x10
#define FAT_ATTR_ARCH   0x20
#define FAT_ATTR_LFN    0x0F

#define FAT_PAGE        4096
#define FAT_SLOT        32
#define FAT_MAX_NAME    255
#define FAT_MAX_DIR_SLOTS 65536         /* 2 MiB: the FAT directory size limit */
#define FAT_MAX_FILE    0xFFFFFFFFu     /* sizes are 32-bit */

/* flush classes: data first, then the FAT, then directory entries and other metadata */
enum { FPC_DATA = 0, FPC_FAT = 1, FPC_META = 2, FPC_COUNT = 3 };

typedef struct FatVol FatVol;
typedef struct FatNode FatNode;
typedef struct FatDir FatDir;
typedef struct FatPage FatPage;

/* ---- cache ---- */
struct FatPage {
    u64 key;                    /* volume byte offset >> 12 */
    u8 *data;                   /* FAT_PAGE bytes */
    FatPage *hnext;             /* hash chain */
    FatPage *lru_prev, *lru_next;
    u8 dirty, cls;
};

/* ---- cluster chains ---- */
typedef struct FatExtent { u32 first, count, start; } FatExtent;     /* start = chain index of the first cluster */

/* ---- nodes: one per directory entry that has been looked at (the persistent inode); VNodes are transient views ---- */
#define FN_DIR        0x01
#define FN_ROOT       0x02
#define FN_FIXROOT    0x04      /* FAT16 root directory: a fixed region, not a cluster chain */
#define FN_UNLINKED   0x08
#define FN_DAMAGED    0x10      /* on-disk values are inconsistent: reads fail with EIO, no writes */
#define FN_CHAIN_OK   0x20      /* chain verified against the size and fully known */
#define FN_CHAIN_END  0x40      /* the extent cache reaches the end of the chain (EOC seen) */
#define FN_NOWRITE    0x80      /* chain disagrees with the size: readable, but never written to */
#define FAT_MAX_EXTENTS (1u << 20)

struct FatNode {
    FatVol *vol;
    FatNode *parent;            /* NULL for the root and for detached (unlinked) nodes */
    FatNode *hnext, *snext;     /* parent's name / short-name hash chains */
    FatNode *onext;             /* orphan list */
    VNode *vn;                  /* live vnode or NULL */
    char *name;                 /* UTF-8, case preserved */
    u16 name_len;
    u8 sfn[11];
    u8 attr, ntres, flags, owner;
    u32 first;                  /* first cluster, 0 = none */
    u32 size;                   /* file size in bytes */
    u64 ctime, mtime, atime;    /* unix seconds */
    u32 slot;                   /* first slot of the entry set in the parent directory */
    u8 nslots;                  /* LFN slots + the short entry */
    u64 ino;
    /* extent cache of the cluster chain */
    FatExtent *ext;
    u32 next_ext, ext_cap;      /* used / allocated extents */
    u32 known;                  /* clusters covered by ext[] */
    u32 tail;                   /* last cluster covered by ext[] */
    u64 lru;
    FatDir *dir;                /* directory listing, when loaded */
};

struct FatDir {
    FatNode **order;            /* children in slot order */
    u32 count, cap;
    FatNode **htab, **stab;     /* name and short-name hash tables */
    u32 hsize;                  /* power of two */
    u32 slots;                  /* capacity in slots */
    u32 end_slot;               /* slots from here on have never been used (0x00) */
    u32 *used;                  /* bitmap, one bit per slot: occupied by anything (entries, LFN fragments, dots, label) */
    u32 used_words;
    bool garbage;               /* non-zero slots were found after the end marker; cleared before the first new entry */
};

typedef struct FatUser { char name[24]; u32 uid, gid; } FatUser;

#define FAT_MAX_USERS 8
#define FAT_MAX_VOLS  8

struct FatVol {
    BlockDev *dev;
    Mount *mount;
    Mutex lock;
    FatVol *next;
    u32 ds;                     /* device sector size */
    /* geometry */
    int bits;                   /* 16 or 32 */
    u32 bps, spc, cbytes, cshift;
    u32 reserved, nfats, fat_sectors;
    u32 root_entries;
    u64 fat_off[4];
    u32 active_fat;
    u64 root_off, root_bytes;   /* FAT16 fixed root */
    u64 data_off;               /* byte offset of cluster 2 */
    u32 nclusters;              /* data clusters; valid numbers are 2 .. nclusters+1 */
    u32 root_cluster;
    u64 vol_bytes;
    u32 fsinfo_sector;
    /* allocation state */
    u32 free_clusters;
    bool free_known, free_verified;      /* free_verified: free_clusters came from a scan of the FAT, not from FSInfo */
    u32 alloc_cursor;
    /* state */
    bool ro, forced, was_dirty, media_dirty, sync_mode, dying, label_set;
    int io_error;
    char label[12];
    u64 last_op_ms, err_logged_ms;
    /* options */
    u32 uid, gid, fmask, dmask;
    bool userdirs;
    FatUser users[FAT_MAX_USERS];
    int nusers;
    /* cache */
    FatPage **htab;
    u32 hmask;
    FatPage *lru_head, *lru_tail;
    u32 npages, max_pages, ndirty, dirty_limit;
    u64 dirty_since_ms;
    u8 *bounce;                 /* contiguous scratch for multi-page device I/O */
    /* nodes */
    FatNode *root;
    u32 nnodes, nlive;
    u64 next_ino;
    FatNode *orphans;
    u64 lru_clock;
    char fstype[8];
};

/* ---- fat_cache.c ---- */
int  fc_init(FatVol *v);
void fc_destroy(FatVol *v);
int  fc_read(FatVol *v, u64 off, void *buf, size_t n);
int  fc_write(FatVol *v, u64 off, const void *buf, size_t n, int cls);
int  fc_zero(FatVol *v, u64 off, size_t n, int cls);
int  fc_flush(FatVol *v);
int  fc_read_ra(FatVol *v, u64 off, void *buf, size_t n, u32 ra);    /* ra: pages to read ahead on a miss */
const u8 *fc_peek(FatVol *v, u64 off, size_t *avail, int *err);      /* pointer into a cached page, valid until the next cache call */

/* ---- fat_chain.c ---- */
bool fat_valid_cluster(const FatVol *v, u32 c);
bool fat_is_eoc(const FatVol *v, u32 val);
int  fat_get(FatVol *v, u32 c, u32 *val);
int  fat_put(FatVol *v, u32 c, u32 val);
int  fat_alloc_chain(FatVol *v, u32 count, u32 prefer, u32 *first, u32 *last);
int  fat_alloc_chain_cb(FatVol *v, u32 count, u32 prefer, int zero_cls, void (*run_cb)(void *, u32, u32), void *ctx,
                        u32 *first_out, u32 *last_out);
int  fat_free_chain(FatVol *v, u32 first, u32 *freed);
int  fat_count_free(FatVol *v, u32 *out);
u64  fat_cluster_off(const FatVol *v, u32 c);
int  fat_set_clean_bits(FatVol *v, bool clean);
int  fat_read_state_bits(FatVol *v, u32 *fat1);
void chain_reset(FatNode *n);
int  chain_lookup(FatNode *n, u32 idx, u32 ahead, u32 *cluster, u32 *run);
int  chain_map(FatNode *n, u64 byte_off, u32 ahead_bytes, u64 *disk_off, u64 *avail);
int  chain_prepare(FatNode *n);
int  chain_fill(FatNode *n);
int  chain_grow(FatNode *n, u32 count);
int  chain_shrink(FatNode *n, u32 keep);

/* ---- fat_name.c ---- */
u32  fatn_fold(u32 cp);
int  fatn_utf8_to_utf16(const char *s, size_t len, u16 *out, int cap);
int  fatn_utf16_to_utf8(const u16 *in, int n, char *out, int cap);
u32  fatn_hash(const char *utf8);
bool fatn_equal(const char *a, const char *b);
u8   fatn_checksum(const u8 sfn[11]);
bool fatn_valid16(const u16 *name, int n);
int  fatn_make_sfn(const u16 *name, int n, u8 sfn[11], u8 *ntres, bool *need_lfn,
                   bool (*taken)(void *ctx, const u8 sfn[11]), void *ctx);
void fatn_sfn_to_utf8(const u8 sfn[11], u8 ntres, char *out, size_t cap);
bool fatn_name_to_sfn(const char *utf8, u8 sfn[11]);
u32  fatn_pack_lfn(const u16 *name, int n, u8 checksum, u8 *out);   /* writes the LFN slots, returns their count */

/* ---- fat_dir.c ---- */
FatNode *fatd_node_new(FatVol *v, FatNode *parent, const char *name);
void fatd_node_free(FatNode *n);
int  fatd_load(FatNode *d);
void fatd_drop(FatNode *d);
FatNode *fatd_find(FatNode *d, const char *name);
int  fatd_create(FatNode *d, const char *name, u8 attr, u32 first, FatNode **out);
void fatd_remove(FatNode *d, FatNode *c);
int  fatd_commit(FatNode *n);
int  fatd_attach_renamed(FatNode *nd, FatNode *c, const char *newname, const FatNode *replaced);
#define FAT_NODE_LIMIT 200000      /* cached nodes before idle directory listings are dropped */
void fatd_trim(FatVol *v);
void fatd_tree_free(FatNode *n);
u32  fatd_dirsize(FatNode *d);
int  fatd_write_dots(FatVol *v, u32 cluster, u32 parent_cluster, u64 now);
int  fatd_set_dotdot(FatNode *d, u32 parent_cluster);

/* shared helpers implemented in fat32.c */
u32  fat_node_mode(const FatNode *n);
void fat_node_publish(FatNode *n);
u64  fat_time_from_dos(u16 date, u16 time, u8 tenths);
void fat_time_to_dos(u64 t, u16 *date, u16 *time, u8 *tenths);
u64  fat_now(void);

#endif
