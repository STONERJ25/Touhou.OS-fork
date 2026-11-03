/*
 * NTFS, read only: the volume, file and directory parser behind kernel/fs/ntfs.c (the VFS glue). Written from the published descriptions of the format (the
 * libfsntfs "New Technologies File System (NTFS) format" document and MS-XCA for LZNT1); docs/NTFS.md lists what is supported and what was checked against what.
 *
 * Two rules shape every function here:
 *   - the volume is not trusted. Every length, offset, count and run is checked before it is used, loops are bounded by counters (a damaged or hostile
 *     image ends in -EIO, never in a hang), nothing recurses on volume data, and memory use has fixed ceilings (NT_MAX_RUNS, NT_MAX_ALIST, ...);
 *   - nothing here writes. There is no write path of any kind, and none will be added: the device is only ever asked to read.
 *
 * The parser knows nothing of the VFS, of locks or of block devices. It reads through NtfsDev (a byte-granular read callback) and allocates through the
 * NT_* macros, so the very same sources are built for the host (tests/host/ntfstool.c, with -DNTFS_HOST) and run under AddressSanitizer in the tests.
 * The caller serialises: one NtfsVol is used by one thread at a time.
 */
#ifndef NTFS_PRIV_H
#define NTFS_PRIV_H

#ifdef NTFS_HOST
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
#define NT_ALLOC(n)     malloc(n)
#define NT_ZALLOC(n)    calloc(1, (n))
#define NT_FREE(p)      free(p)
#define NT_LOG(...)     do { fprintf(stderr, "ntfs: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define NT_DT_DIR 4
#define NT_DT_REG 8
#define NT_DT_LNK 10
#define NT_NAME_MAX 255                          /* longest name the VFS can carry, in bytes of UTF-8 */
#else
#include "kernel.h"
#include "mm.h"
#include "abi.h"
#define NT_ALLOC(n)     kmalloc(n)
#define NT_ZALLOC(n)    kzalloc(n)
#define NT_FREE(p)      kfree(p)
#define NT_LOG(...)     klog("Keine", "ntfs: " __VA_ARGS__)
#define NT_DT_DIR DT_DIR
#define NT_DT_REG DT_REG
#define NT_DT_LNK DT_LNK
#define NT_NAME_MAX GNAME_MAX
#endif

/* ---- ceilings that keep a hostile volume from taking the machine down ---- */
#define NT_MAX_RUNS      (1u << 18)             /* runs in one data stream's map (6 MiB) */
#define NT_MAX_ALIST     (8u << 20)             /* bytes of one $ATTRIBUTE_LIST */
#define NT_MAX_EXTENTS   (1u << 16)             /* attribute list entries walked for one attribute */
#define NT_MAX_DEPTH     24                     /* levels of an index B+tree */
#define NT_MAX_UNIT      (1u << 20)             /* bytes of one compression unit */
#define NT_RECCACHE      32                     /* MFT records kept */
#define NT_IDXCACHE      8                      /* index blocks kept */
#define NT_RA_BYTES      (64u * 1024)           /* per-file read-ahead window */

/* ---- on-disk constants (libfsntfs) ---- */
#define NT_A_STD     0x10u
#define NT_A_LIST    0x20u
#define NT_A_NAME    0x30u
#define NT_A_VOLNAME 0x60u
#define NT_A_VOLINFO 0x70u
#define NT_A_DATA    0x80u
#define NT_A_IROOT   0x90u
#define NT_A_IALLOC  0xA0u
#define NT_A_BITMAP  0xB0u
#define NT_A_REPARSE 0xC0u
#define NT_A_END     0xFFFFFFFFu

#define NT_REC_INUSE 0x0001
#define NT_REC_DIR   0x0002

#define NT_AF_COMPRESSED 0x0001                 /* attribute data flags */
#define NT_AF_ENCRYPTED  0x4000
#define NT_AF_SPARSE     0x8000

#define NT_FA_READONLY   0x00000001u            /* file attribute flags */
#define NT_FA_HIDDEN     0x00000002u
#define NT_FA_SYSTEM     0x00000004u
#define NT_FA_REPARSE    0x00000400u
#define NT_FA_ENCRYPTED  0x00004000u
#define NT_FA_DIRINDEX   0x10000000u            /* $FILE_NAME copy: has an $I30 index (a directory) */

#define NT_NS_POSIX 0
#define NT_NS_WIN32 1
#define NT_NS_DOS   2
#define NT_NS_BOTH  3

#define NT_TAG_SYMLINK  0xA000000Cu
#define NT_TAG_JUNCTION 0xA0000003u

#define NT_REF_MASK 0x0000FFFFFFFFFFFFull

/* little-endian loads from unaligned bytes (the record buffers hold UTF-16 and 64-bit fields at any 2- or 8-byte position) */
static inline u16 nt_le16(const u8 *p) { return (u16)(p[0] | (u16)p[1] << 8); }
static inline u32 nt_le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static inline u64 nt_le64(const u8 *p) { return (u64)nt_le32(p) | (u64)nt_le32(p + 4) << 32; }

/* ---- the device ---- */
typedef struct NtfsDev {
    int (*read)(void *ctx, u64 off, void *buf, size_t len);   /* bytes of the volume; 0 or -errno; never asked for anything past `size` */
    void *ctx;
    u64 size;                                                 /* bytes */
    u32 sector;                                               /* the device's sector size (an alignment hint only) */
} NtfsDev;

/* ---- run lists ---- */
typedef struct NtfsRun { u64 vcn, len; s64 lcn; } NtfsRun;    /* lcn -1: a sparse run (reads as zeros) */
typedef struct NtfsRuns { NtfsRun *r; u32 n, cap; u64 end; } NtfsRuns;

typedef struct NtfsOpts {
    bool show_system;                           /* list and open the metadata files ($MFT ... record 15) */
    bool show_dos;                              /* list the 8.3 twin names as well */
} NtfsOpts;

typedef struct NtfsVol NtfsVol;
typedef struct NtfsFile NtfsFile;

/* One data-like attribute: resident (a copy of the value) or non-resident (sizes and, once asked for, the map). */
typedef struct NtfsStream {
    bool present, resident, runs_loaded;
    u8 *rbuf;                                   /* resident value (a copy) */
    u32 rlen;
    u64 size, init, alloc;
    u16 flags;                                  /* NT_AF_* of the first extent */
    u8 cunit;                                   /* log2 of clusters per compression unit */
    NtfsRuns runs;
} NtfsStream;

struct NtfsFile {
    NtfsVol *vol;
    u64 no;
    u16 seq, nlink, rflags;
    u32 attrs;                                  /* file attribute flags of $STANDARD_INFORMATION */
    u64 ctime, mtime, atime;                    /* seconds since 1970 */
    u32 reparse_tag;
    bool is_dir;
    u8 *rec;                                    /* the base record, fixups applied */
    u8 *alist;                                  /* $ATTRIBUTE_LIST contents, or NULL */
    u32 alist_len;
    NtfsStream data;                            /* the unnamed $DATA */
    /* directories */
    u8 *iroot;                                  /* $INDEX_ROOT ($I30) value (a copy) */
    u32 iroot_len, idx_size;                    /* index block size from the root */
    NtfsStream ialloc;                          /* $INDEX_ALLOCATION ($I30) */
    /* reading */
    u8 *ra;                                     /* read-ahead window (and the last decompressed unit) */
    u64 ra_off;                                 /* file offset of the window, or the number of the decompressed unit it holds */
    u32 ra_len, ra_cap;
    bool ra_unit;                               /* ra holds a decompressed unit, not raw bytes */
};

typedef struct NtfsDirCursor {                  /* an in-order walk of a directory's index without recursion */
    struct { u64 vcn; u32 off; u8 sub_done; } f[NT_MAX_DEPTH];
    int depth;
    bool started;
    u32 visited;
} NtfsDirCursor;

typedef struct NtfsDirEntry {                   /* what the walk returns; name points into a cache and is valid until the next call */
    u64 ref;
    u32 flags;                                  /* file attribute flags from the index copy of $FILE_NAME */
    u32 reparse;                                /* its reparse tag (or EA size) */
    u8 ns;
    u32 nunits;
    const u8 *name;                             /* UTF-16LE, not aligned */
} NtfsDirEntry;

typedef struct NtfsVisible {                    /* a directory entry as the VFS sees it */
    u64 ino;
    u32 type;                                   /* NT_DT_* */
    char name[NT_NAME_MAX + 1];
} NtfsVisible;

/* ---- the volume ---- */
struct NtfsVol {
    NtfsDev dev;
    NtfsOpts opts;
    u32 bps, spc, cluster, cshift;
    u64 nclusters, total_sectors;
    u32 rec_size, idx_size;
    u64 mft_lcn, mftmirr_lcn, nrecords, mft_size;
    NtfsRuns mft;                               /* the map of $MFT's $DATA */
    u16 *upcase;                                /* 65536 entries; builtin when the volume's own table was unusable */
    bool upcase_builtin;
    u8 ver_major, ver_minor;
    u16 vol_flags;
    u64 serial;
    char label[130];                            /* UTF-8 */
    struct { u64 no; u8 *buf; u32 use; } recs[NT_RECCACHE];
    struct { u64 dir, vcn; u8 *buf; u32 use; } idx[NT_IDXCACHE];
    u32 clock;
    bool free_known, warned_recno;
    u64 free_clusters;
    /* counters for the tests */
    u64 n_reads, n_read_bytes, n_rec_hits, n_rec_misses, n_idx_hits, n_idx_misses;
};

/* ---- ntfs_name.c ---- */
int  ntfs_utf8_to_utf16(const char *s, u16 *out, u32 cap);                         /* units, or -1 (invalid UTF-8, too long) */
int  ntfs_utf16_to_utf8(const u8 *le, u32 nunits, char *out, u32 cap);             /* bytes (no NUL), or -1 (too long, or a NUL/slash in it) */
u64  ntfs_time_to_unix(u64 filetime);
void ntfs_upcase_builtin(u16 *table);
int  ntfs_name_cmp(const NtfsVol *v, const u16 *target_up, u32 tn, const u8 *key_le, u32 kn);   /* collation order: upper-cased units, shorter first */
bool ntfs_name_equal_exact(const u16 *target, u32 tn, const u8 *key_le, u32 kn);

/* ---- ntfs_lznt1.c ---- */
int  ntfs_lznt1(const u8 *src, size_t slen, u8 *dst, size_t dcap, size_t *dlen);   /* 0, or -EIO for a damaged stream */

/* ---- ntfs_core.c ---- */
int  ntfs_vol_open(NtfsVol **out, const NtfsDev *dev, const NtfsOpts *opts, char *why, size_t whyn);
void ntfs_vol_close(NtfsVol *v);
int  ntfs_file_open(NtfsVol *v, u64 ref, NtfsFile **out);                          /* by MFT reference (a stale sequence number: -ENOENT) */
void ntfs_file_close(NtfsFile *f);
ssize_t ntfs_read(NtfsFile *f, u64 off, void *buf, size_t n);                      /* the unnamed data stream; -EACCES for EFS, -ENOTSUP when the data lives elsewhere */
int  ntfs_dir_lookup(NtfsFile *dir, const char *name_utf8, u64 *ref);              /* -ENOENT, -EIO, ... */
void ntfs_dir_cursor_reset(NtfsDirCursor *c);
int  ntfs_dir_next(NtfsFile *dir, NtfsDirCursor *c, NtfsDirEntry *e);              /* 1 = an entry, 0 = end, <0 = error */
int  ntfs_dir_next_visible(NtfsFile *dir, NtfsDirCursor *c, NtfsVisible *out, u32 *skipped);
int  ntfs_parent_of(NtfsFile *f, u64 *parent);                                     /* from the first $FILE_NAME */
int  ntfs_readlink(NtfsFile *f, const char *mount_target, char *out, size_t cap);  /* symlinks and junctions; -EINVAL otherwise */
bool ntfs_is_link(const NtfsFile *f);
int  ntfs_free_clusters(NtfsVol *v, u64 *free_out);
bool ntfs_data_elsewhere(u32 reparse_tag);                                         /* WOF, cloud placeholders, dedup ...: the unnamed stream is not the content */
int  ntfs_dump_record(NtfsVol *v, u64 no, char *out, size_t cap);                  /* debugging text for the host tool */

#endif
