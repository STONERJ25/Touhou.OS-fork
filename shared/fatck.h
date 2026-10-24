/*
 * fatck -- the consistency checker for FAT12/16/32 volumes, shared by the kernel and by user space.
 *
 *   kernel:  kernel/fs/fat_check.c runs it READ-ONLY on a HUMANVILLAGE volume that was not shut down cleanly, before the volume is
 *            mounted, to decide between "read-write, nothing is out of place" and "read-only, here is what is wrong".
 *   user:    user/bin/fsck/ (fsck, fsck.fat, fsck.vfat) runs it on an unmounted volume and repairs what it finds (fatfix.c).
 *   host:    tools/fatck_host.py builds the very same file into a DLL for the tests, which hold it against tools/mkdisk.py's
 *            independent Python checker (FatReader.check): the two must agree on what counts as damage.
 *
 * The scanner never writes. It walks the directory tree iteratively (explicit stack, bounded depth), claims the clusters of
 * every chain in a bitmap (cross-links and loops fall out of that), looks at the FAT once from end to end for clusters nobody
 * owns, and compares the FAT copies. Memory is a bitmap with one bit per cluster plus a few buffers; time is bounded by one
 * pass over the FAT, one read of every directory and a deadline supplied by the caller.
 *
 * Everything the volume says is checked before use: cluster numbers against the volume, chains against loops, directory
 * sizes against the 2 MiB limit, names against the characters FAT allows. A hostile volume costs time, never safety.
 *
 * Severity of a finding (what a caller may do about it):
 *   FS_ERR   damage: data loss or storage that two things claim. Never write to such a volume without repairing it.
 *   FS_LEAK  harmless leftovers that a power cut can produce on a volume the driver was writing to (lost clusters, a chain longer
 *            than the file size says, bytes after the end-of-directory marker, FAT copies or FSInfo that lag behind). Writing
 *            is safe; a repair tidies them up.
 *   FS_WARN  cosmetic (case flags, padding, dates, a dirty flag).
 *   FS_NOTE  information.
 */
#ifndef SHARED_FATCK_H
#define SHARED_FATCK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

enum { FS_NOTE = 0, FS_WARN = 1, FS_LEAK = 2, FS_ERR = 3 };

/* What a finding is about. Entry-level findings carry the directory and slot; chain-level ones the cluster. */
enum {
    FK_NONE = 0,
    /* the volume */
    FK_BPB,         /* the boot sector cannot be trusted: nothing else is looked at */
    FK_SIZE,        /* the volume claims more bytes than its device holds */
    FK_FATCOPY,     /* a FAT copy differs from the active one: aux = copy, cluster = first difference, count in text */
    FK_MEDIA,       /* FAT[0] media byte differs from the boot sector */
    FK_DIRTY,       /* the volume is flagged dirty (clean-shutdown bit or boot sector flag), or reports disk errors */
    FK_FSINFO,      /* FSInfo sector: signatures, free count or next-free hint wrong */
    FK_BACKUP,      /* backup boot sector differs from the main one */
    /* directories */
    FK_DIRCHAIN,    /* a directory's cluster chain is broken: cluster = where it went wrong, aux = why (FC_*) */
    FK_DIRBIG,      /* a directory with more than 65536 slots */
    FK_DOTS,        /* "." / ".." missing or pointing elsewhere: aux = 1 if missing, 2 for ".", 3 for ".." */
    FK_GARBAGE,     /* non-zero slots after the end-of-directory marker (slot = the first one) */
    FK_DIRSIZE,     /* a directory entry with a non-zero size field */
    FK_LFN,         /* long-name entries that do not belong together: orphan, order, checksum, count */
    FK_LFNFIELD,    /* an LFN entry with a non-zero first-cluster field */
    FK_LFNPAD,      /* LFN padding that is not 0xFFFF, or a non-zero type byte */
    FK_NAME,        /* illegal character, control character or leading space in a short name */
    FK_NAMECASE,    /* lower-case letters in a short name, unknown NT reserved bits */
    FK_ATTR,        /* reserved attribute bits */
    FK_DATE,        /* a date that is not a date */
    FK_DUP,         /* two entries of one directory with the same (long or short) name */
    FK_LABEL,       /* a volume label entry outside the root directory */
    /* files and chains */
    FK_RANGE,       /* first cluster outside the volume */
    FK_NOCLUSTER,   /* a size but no clusters (files) or no first cluster (directories) */
    FK_CHAIN,       /* the chain leaves the volume, runs into a free, bad or reserved cluster (aux = FC_*, cluster = where) */
    FK_CROSS,       /* a cluster is claimed twice: cross-link, or a loop (cluster = it) */
    FK_SHORT,       /* chain shorter than the size needs: aux = clusters in the chain */
    FK_LONG,        /* chain longer than the size needs: aux = clusters in the chain */
    FK_LOST,        /* lost clusters: allocated in the FAT, part of no chain (count in the text, first ones in the result) */
    /* the scan itself */
    FK_INCOMPLETE,  /* the scan stopped before it was done (deadline, memory, depth, I/O): no verdict */
    FK_IO,          /* the device failed a read */
    FK__COUNT
};

/* why a chain is broken (FatCkFinding.aux for FK_DIRCHAIN / FK_CHAIN) */
enum { FC_RANGE = 1, FC_FREE, FC_BAD, FC_RESERVED, FC_LOOP };

typedef struct FatCkFinding {
    uint16_t code;          /* FK_* */
    uint8_t  sev;           /* FS_* */
    uint8_t  fixable;       /* the repair code knows what to do about it */
    uint32_t dir;           /* first cluster of the directory the entry lives in (the root counts as FatCkGeom.root_dir) */
    uint32_t slot;          /* slot of the short entry (or of the offending LFN fragment) inside that directory */
    uint32_t nslots;        /* slots of the whole entry set (LFN fragments + short entry), 1 for fragments */
    uint32_t cluster;       /* the cluster the finding is about, 0 if none */
    uint32_t first;         /* the entry's first cluster */
    uint32_t size;          /* the entry's size field */
    uint32_t aux;           /* code specific */
    uint32_t chain_len;     /* clusters that were found in the entry's chain before the trouble (or in all of it) */
    uint8_t  attr;
} FatCkFinding;

/* The scan's tallies. */
typedef struct FatCkResult {
    bool     opened;        /* the boot sector was usable */
    bool     incomplete;    /* no verdict: the scan was cut short */
    bool     dirty;         /* flagged dirty (clean-shutdown bit clear or boot sector flag set) */
    bool     hard_error;    /* flagged with disk errors */
    int      bits;          /* 12, 16 or 32 */
    uint32_t nclusters;
    uint32_t cluster_bytes;
    uint32_t errors, leaks, warnings, notes;
    uint32_t count[FK__COUNT];
    uint32_t files, dirs;
    uint32_t used, free_clusters, bad_clusters, lost_clusters;
    uint32_t first_lost[8];         /* the first few lost clusters */
    uint32_t fat_diff[4];           /* entries in which FAT copy n differs from the active one */
    uint32_t max_depth;
    uint64_t file_bytes;
} FatCkResult;

/* Read-only facts about the volume's geometry, for callers that repair or print. */
typedef struct FatCkGeom {
    int      bits;
    uint32_t bps, spc, cluster_bytes, reserved, nfats, fat_sectors, root_entries;
    uint32_t nclusters, root_cluster, root_dir, active_fat;
    uint32_t fsinfo_sector, backup_sector;
    uint64_t fat_off[4], fat_bytes, root_off, root_bytes, data_off, vol_bytes;
    uint8_t  media;
    bool     mirrored;
} FatCkGeom;

/* What the host program supplies. All offsets are byte offsets inside the volume; reads and writes are multiples of `align`
 * (the device's sector size) at offsets that are multiples of it. */
typedef struct FatCkIo {
    int   (*read)(void *ctx, uint64_t off, void *buf, size_t n);          /* 0 or -errno */
    int   (*write)(void *ctx, uint64_t off, const void *buf, size_t n);   /* NULL: the session is read-only */
    int   (*flush)(void *ctx);                                            /* make written data durable (may be NULL) */
    void *(*alloc)(void *ctx, size_t n);                                  /* zero-filled, or NULL */
    void  (*release)(void *ctx, void *p, size_t n);
    uint64_t (*now_ms)(void *ctx);                                        /* NULL: no deadline */
    void  (*report)(void *ctx, const FatCkFinding *f, const char *text);
    void  *ctx;
    uint64_t bytes;         /* size of the volume window (the partition) */
    uint32_t align;         /* sector size of the device: 512 or 4096 */
    uint64_t deadline_ms;   /* absolute, in now_ms() terms; 0 = none */
    uint32_t max_windows;   /* FAT cache: windows of 64 KiB (0 = 16) */
    uint32_t arena_bytes;   /* memory for the duplicate-name tables (0 = 1 MiB) */
    uint32_t max_depth;     /* directory nesting limit (0 = 256) */
} FatCkIo;

typedef struct FatCk FatCk;

/* Parses and validates the boot sector. On success *out is a session (fatck_close it); res.opened is true. Otherwise returns
 * -EINVAL-style negative codes and, when `report` is set, a FK_BPB finding explains why. */
int  fatck_open(FatCk **out, const FatCkIo *io);
void fatck_close(FatCk *ck);

/* The cheap part of the verdict: the clean-shutdown bit, the boot sector flag byte and the hard-error bit. A volume that is flagged
 * clean needs no scan (that is what the kernel does). */
int  fatck_quick_state(FatCk *ck, bool *dirty, bool *hard_error);

/* The whole scan. Returns 0 when it ran to the end (look at the result), a negative code when it could not (res.incomplete). */
int  fatck_scan(FatCk *ck);
const FatCkResult *fatck_result(const FatCk *ck);
const FatCkGeom   *fatck_geom(const FatCk *ck);

/* Names for messages. */
const char *fatck_code_name(int code);
const char *fatck_sev_name(int sev);

/* ---- primitives for the repair code (fatfix.c) and for tests ---- */
int  fatck_fat_get(FatCk *ck, uint32_t c, uint32_t *val);            /* the active FAT */
int  fatck_fat_get_copy(FatCk *ck, uint32_t copy, uint32_t c, uint32_t *val);
int  fatck_fat_set(FatCk *ck, uint32_t c, uint32_t val);             /* every copy that is kept up to date; needs a writer */
int  fatck_fat_set_copy(FatCk *ck, uint32_t copy, uint32_t c, uint32_t val);
int  fatck_read(FatCk *ck, uint64_t off, void *buf, size_t n);       /* any offset; through the cache, so it sees pending writes */
int  fatck_write(FatCk *ck, uint64_t off, const void *buf, size_t n);
int  fatck_flush(FatCk *ck);                                         /* write the cache back and tell the device to flush */
int  fatck_dir_slot_off(FatCk *ck, uint32_t dir, uint32_t slot, uint64_t *off);   /* where a directory slot lives on the volume */
int  fatck_dir_slots(FatCk *ck, uint32_t dir, uint32_t *slots);      /* how many slots the directory holds (chain length * slots per cluster) */
uint64_t fatck_cluster_off(const FatCk *ck, uint32_t c);
bool fatck_is_eoc(const FatCk *ck, uint32_t v);
bool fatck_is_bad(const FatCk *ck, uint32_t v);
bool fatck_is_reserved(const FatCk *ck, uint32_t v);
uint32_t fatck_eoc(const FatCk *ck);
uint8_t fatck_lfn_checksum(const uint8_t sfn[11]);

/* The value a power cut leaves in FAT[1] / the boot sector flag, and a way to set them; used when a repair declares the volume clean. */
int  fatck_set_clean(FatCk *ck, bool clean);

#endif
