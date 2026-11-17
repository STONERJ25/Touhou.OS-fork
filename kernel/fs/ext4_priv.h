/*
 * ext2, ext3 and ext4, read only: the volume, inode, extent, block map and directory parser behind kernel/fs/ext4.c (the VFS glue). Written from the published
 * description of the on-disk layout (the kernel.org "ext4 Disk Layout" document and the e2fsprogs manual pages ext4(5) and mke2fs(8)), checked against the
 * observed behaviour of mke2fs, dumpe2fs, debugfs and e2fsck; no source of Linux or e2fsprogs was read. docs/EXT4.md lists what is supported and what was checked against what.
 *
 * Two rules shape every function here:
 *   - the volume is not trusted. Every length, block number, extent, count and inode number is checked before it is used, loops are bounded by counters or by a
 *     strictly decreasing quantity (a damaged or hostile image ends in -EIO, never in a hang, an overrun or a panic), nothing recurses on volume data, and memory has
 *     fixed ceilings (E4_CACHE_BYTES, E4_RA_BYTES, one inode's inline data);
 *   - nothing here writes. There is no write path of any kind, and none will be added: the device is only ever asked to read.
 *
 * The parser knows nothing of the VFS, of locks or of block devices. It reads through Ext4Dev (a byte-granular read callback) and allocates through the E4_* macros, so the
 * very same source is built for the host (tests/host/ext4tool.c, with -DEXT4_HOST) and run under AddressSanitizer in the tests. The caller serialises: one Ext4Vol (and the
 * Ext4Files made from it) is used by one thread at a time. Buffers handed to the read callback are always heap memory, never the stack (the block layer's rule).
 */
#ifndef EXT4_PRIV_H
#define EXT4_PRIV_H

#ifdef EXT4_HOST
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#define E4_ALLOC(n)     malloc(n)
#define E4_ZALLOC(n)    calloc(1, (n))
#define E4_FREE(p)      free(p)
#define E4_LOG(...)     do { fprintf(stderr, "ext4: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#else
#include "kernel.h"
#include "mm.h"
#include "abi.h"
#define E4_ALLOC(n)     kmalloc(n)
#define E4_ZALLOC(n)    kzalloc(n)
#define E4_FREE(p)      kfree(p)
#define E4_LOG(...)     klog("Keine", "ext4: " __VA_ARGS__)
#endif

#ifndef ENOKEY
#define ENOKEY 126                              /* "required key not available": the number Linux uses; an encrypted inode answers it */
#endif

/* ---- ceilings that keep a hostile volume from taking the machine down ---- */
#define E4_MAX_BLOCK_LOG 6                      /* block sizes 1 KiB .. 64 KiB */
#define E4_MAX_GROUPS    (1u << 22)             /* block groups (a 512 TiB volume of 4 KiB blocks) */
#define E4_MAX_DEPTH     5                      /* levels of an extent tree below the root */
#define E4_NAME_MAX      255
#define E4_MAX_SIZE      (1ull << 48)           /* no file of this format is larger: 2^32 blocks of 64 KiB */
#define E4_CACHE_BYTES   (1u << 20)             /* metadata blocks kept per volume (direct mapped) */
#define E4_RA_BYTES      (64u * 1024)           /* read-ahead window of an open file, used for small reads */
#define E4_INDEX_MIN     (128u * 1024)          /* a directory this big gets a name index for its lookups (hashed directories are read in order, so without one a lookup scans) */
#define E4_INDEX_MAX_ENTRIES (1u << 20)         /* ... unless it has more entries than this (the table would take 8 MiB) */
#define E4_INLINE_AREA   56                    /* bytes of directory entries in i_block of an inline directory (after the parent's inode number) */

/* ---- file types, the numbers of the C library's dirent.h and stat.h ---- */
#define E4_DT_UNKNOWN 0
#define E4_DT_FIFO    1
#define E4_DT_CHR     2
#define E4_DT_DIR     4
#define E4_DT_BLK     6
#define E4_DT_REG     8
#define E4_DT_LNK     10
#define E4_DT_SOCK    12

#define E4_S_IFMT   0170000u
#define E4_S_IFSOCK 0140000u
#define E4_S_IFLNK  0120000u
#define E4_S_IFREG  0100000u
#define E4_S_IFBLK  0060000u
#define E4_S_IFDIR  0040000u
#define E4_S_IFCHR  0020000u
#define E4_S_IFIFO  0010000u

/* ---- feature words (superblock 0x5C, 0x60, 0x64) ---- */
#define E4_COMPAT_HAS_JOURNAL     0x0004u
#define E4_COMPAT_SPARSE_SUPER2   0x0200u

#define E4_INCOMPAT_COMPRESSION   0x0001u       /* refused */
#define E4_INCOMPAT_FILETYPE      0x0002u
#define E4_INCOMPAT_RECOVER       0x0004u       /* the journal holds transactions that were never written to their place */
#define E4_INCOMPAT_JOURNAL_DEV   0x0008u       /* an external journal, not a filesystem: refused */
#define E4_INCOMPAT_META_BG       0x0010u
#define E4_INCOMPAT_EXTENTS       0x0040u
#define E4_INCOMPAT_64BIT         0x0080u
#define E4_INCOMPAT_MMP           0x0100u
#define E4_INCOMPAT_FLEX_BG       0x0200u
#define E4_INCOMPAT_EA_INODE      0x0400u
#define E4_INCOMPAT_DIRDATA       0x1000u       /* refused */
#define E4_INCOMPAT_CSUM_SEED     0x2000u
#define E4_INCOMPAT_LARGEDIR      0x4000u
#define E4_INCOMPAT_INLINE_DATA   0x8000u
#define E4_INCOMPAT_ENCRYPT       0x10000u
#define E4_INCOMPAT_CASEFOLD      0x20000u
#define E4_INCOMPAT_SUPPORTED     (E4_INCOMPAT_FILETYPE | E4_INCOMPAT_RECOVER | E4_INCOMPAT_META_BG | E4_INCOMPAT_EXTENTS | E4_INCOMPAT_64BIT | E4_INCOMPAT_MMP | \
                                   E4_INCOMPAT_FLEX_BG | E4_INCOMPAT_EA_INODE | E4_INCOMPAT_CSUM_SEED | E4_INCOMPAT_LARGEDIR | E4_INCOMPAT_INLINE_DATA | \
                                   E4_INCOMPAT_ENCRYPT | E4_INCOMPAT_CASEFOLD)

#define E4_RO_SPARSE_SUPER        0x0001u
#define E4_RO_HUGE_FILE           0x0008u
#define E4_RO_BIGALLOC            0x0200u

/* ---- inode flags (inode offset 0x20) ---- */
#define E4_FL_ENCRYPT     0x00000800u
#define E4_FL_HUGE_FILE   0x00040000u
#define E4_FL_EXTENTS     0x00080000u
#define E4_FL_INLINE_DATA 0x10000000u

#define E4_ROOT_INO 2u
#define E4_EXT_MAGIC 0xF30Au
#define E4_XATTR_MAGIC 0xEA020000u

/* little-endian loads from unaligned bytes */
static inline u16 e4_le16(const u8 *p) { return (u16)(p[0] | (u16)p[1] << 8); }
static inline u32 e4_le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static inline u64 e4_le64(const u8 *p) { return (u64)e4_le32(p) | (u64)e4_le32(p + 4) << 32; }

/* ---- the device ---- */
typedef struct Ext4Dev {
    int (*read)(void *ctx, u64 off, void *buf, size_t len);   /* bytes of the volume; 0 or -errno; never asked for anything past `size` */
    void *ctx;
    u64 size;                                                 /* bytes */
    u32 sector;                                               /* the device's sector size (an alignment hint only) */
} Ext4Dev;

typedef struct Ext4Vol Ext4Vol;
typedef struct Ext4File Ext4File;

/* ---- mapping a file's logical blocks to the volume's ---- */
enum { E4_MAP_HOLE = 0, E4_MAP_MAPPED = 1, E4_MAP_UNWRITTEN = 2 };
typedef struct Ext4Map {
    int kind;                                   /* E4_MAP_* */
    u64 pblk;                                   /* the volume block of the asked logical block (mapped only) */
    u64 run;                                    /* how many blocks from there on are the same kind (and, if mapped, contiguous on the volume): at least 1 */
} Ext4Map;

/* ---- an open inode ---- */
struct Ext4File {
    Ext4Vol *v;
    u32 ino;
    u32 mode;                                   /* type and permission bits as stored (no VFS bits mixed in) */
    u32 uid, gid, nlink;
    u64 size;                                   /* bytes (regular files: the 64-bit size; others: see docs/EXT4.md) */
    s64 atime, ctime, mtime, crtime;            /* seconds since 1970, with the epoch bits of the extra field */
    u32 atime_ns, ctime_ns, mtime_ns, crtime_ns;
    u32 flags;                                  /* i_flags */
    u64 blocks512;                              /* blocks held, in units of 512 bytes (the huge_file unit is converted) */
    u32 generation;
    u64 rdev;                                   /* device nodes: the number as the C library's makedev() writes it */
    u8 iblock[60];                              /* i_block as stored: extent root, block map, fast link target, inline data's first 60 bytes */
    bool inline_data, extents, encrypted, fast_link;
    u8 *inl;                                    /* inline data beyond i_block: the value of system.data (a copy), or NULL */
    u32 inl_len;
    /* caches */
    bool mc_ok;                                 /* the last mapping found: blocks [mc_l, mc_l + mc.run) */
    u64 mc_l;
    Ext4Map mc;
    u64 dir_hint;                               /* directories: the block where the last lookup found its name (the next lookup starts there) */
    u64 dir_cookie;                             /* directories: the position the last ext4_dir_next() returned (known to be the start of an entry) */
    bool dir_cookie_ok;
    u32 *hidx;                                  /* directories of 128 KiB and more: a name index built at the first lookup (open addressing; a slot holds the position of an entry plus one) */
    u32 hmask;                                  /* slots - 1 */
    bool hdone;                                 /* the index was built, or building it was given up (the lookup then reads the blocks in order) */
    u8 *ra;                                     /* read-ahead window */
    u64 ra_off;
    u32 ra_len;
};

typedef struct Ext4DirEnt {                     /* what a directory walk returns */
    u32 ino;
    u32 type;                                   /* E4_DT_*; unknown types are looked up in the inode */
    u32 namelen;
    char name[E4_NAME_MAX + 1];
} Ext4DirEnt;

typedef struct Ext4Stat {
    u64 bsize, blocks, bfree, bavail, files, ffree;
} Ext4Stat;

typedef struct Ext4Slot { u64 blk; u8 *buf; } Ext4Slot;

/* ---- the volume ---- */
struct Ext4Vol {
    Ext4Dev dev;
    u32 bsize, bshift;                          /* block size and its log2 */
    u32 ratio;                                  /* blocks per allocation cluster (bigalloc), else 1 */
    u64 blocks, r_blocks;                       /* blocks count and reserved blocks, in blocks */
    u32 inodes, first_ino, ipg, bpg, ngroups;
    u32 first_data_block;
    u32 inode_size, itb;                        /* itb: blocks of one group's inode table */
    u32 desc_size, dpb;                         /* group descriptor size, descriptors per block */
    u32 first_meta_bg;
    u32 rev, compat, incompat, ro_compat;
    u32 state;                                  /* s_state */
    u32 backup_bgs[2];
    u32 overhead;                               /* s_overhead_clusters (0: not recorded) */
    u32 last_orphan;
    u32 free_inodes_sb;
    u32 skipped_names;                          /* directory entries left out because their names contain a slash or a NUL */
    u64 free_blocks_sb;
    u8 uuid[16];
    char label[17];
    /* the cache of metadata blocks */
    u32 nslots;
    Ext4Slot *slots;
    /* counters for the tests */
    u64 n_reads, n_read_bytes, n_hits, n_misses;
};

/* ---- ext4_core.c ---- */
int  ext4_vol_open(Ext4Vol **out, const Ext4Dev *dev, char *why, size_t whyn);
void ext4_vol_close(Ext4Vol *v);
int  ext4_file_open(Ext4Vol *v, u32 ino, Ext4File **out);                        /* -EIO: not an inode of this volume (or damaged); -ENOMEM */
void ext4_file_close(Ext4File *f);
ssize_t ext4_read(Ext4File *f, u64 off, void *buf, size_t n);                    /* regular files; -ENOKEY (encrypted), -EISDIR, -EIO; 0 at the end of the file */
int  ext4_dir_lookup(Ext4File *dir, const char *name, u32 *ino);                 /* -ENOENT, -ENOKEY, -EIO; "." and ".." work too */
int  ext4_dir_next(Ext4File *dir, u64 *cookie, Ext4DirEnt *out);                 /* 1 = an entry, 0 = end, <0 = error; cookie 0 starts; "." and ".." are not returned */
int  ext4_readlink(Ext4File *f, char *out, size_t cap);                          /* the target's length; writes at most cap bytes, no NUL; -EINVAL, -ENOKEY, -EIO */
int  ext4_statfs(Ext4Vol *v, Ext4Stat *out);                                     /* sums the group descriptors */
int  ext4_group_info(Ext4Vol *v, u32 group, u64 *inode_table, u32 *free_blocks, u32 *free_inodes);
int  ext4_map_block(Ext4File *f, u64 lblk, Ext4Map *m);                          /* for the tests */
const char *ext4_flavour(const Ext4Vol *v);                                      /* "ext2", "ext3" or "ext4", by the features in use */
size_t ext4_feature_text(const Ext4Vol *v, char *buf, size_t cap);               /* "has_journal extent 64bit ..." */
int  ext4_probe_super(const void *sb, size_t len, char *fs, size_t fsn, char *label, size_t labeln);   /* is this 1024-byte superblock an ext2/3/4 one: its name and label (lsblk) */

#endif
