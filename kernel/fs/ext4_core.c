/*
 * ext2, ext3 and ext4, read only: the parser (see ext4_priv.h for the rules it keeps, docs/EXT4.md for what it supports). The structures follow the published
 * on-disk layout (the kernel.org "ext4 Disk Layout" document, e2fsprogs' ext4(5) and mke2fs(8)); every offset used is named next to the place it is read.
 *
 * What a reader needs, in the order of this file: the volume (superblock, geometry checks, group descriptors, a cache of metadata blocks), inodes (decoded once
 * into an Ext4File), the map from a file's logical blocks to the volume's (extent trees and ext2/ext3 block maps), reads, directories, symbolic links, statfs.
 * Everything that comes from the disk is checked before it is used: a damaged volume costs an error code, never a hang, an overrun or a panic.
 */
#include "ext4_priv.h"

#define WHY(...) do { if (why && whyn) snprintf(why, whyn, __VA_ARGS__); } while (0)

#ifdef EXT4_HOST
extern void (*ext4_meta_hook)(u64 blk, u32 bsize);      /* the fuzz of tests/host/ext4tool.c learns which blocks are metadata */
#endif

static bool is_pow2(u32 x) { return x && !(x & (x - 1)); }

/* ---------------------------------------------------------------------------------------------- feature names (ext4(5)) */

typedef struct { u32 bit; const char *name; } FeatName;

static const FeatName F_COMPAT[] = {
    { 0x1, "dir_prealloc" }, { 0x2, "imagic_inodes" }, { 0x4, "has_journal" }, { 0x8, "ext_attr" }, { 0x10, "resize_inode" }, { 0x20, "dir_index" },
    { 0x40, "lazy_bg" }, { 0x100, "snapshot_bitmap" }, { 0x200, "sparse_super2" }, { 0x400, "fast_commit" }, { 0x800, "stable_inodes" }, { 0x1000, "orphan_file" },
};
static const FeatName F_INCOMPAT[] = {
    { 0x1, "compression" }, { 0x2, "filetype" }, { 0x4, "needs_recovery" }, { 0x8, "journal_dev" }, { 0x10, "meta_bg" }, { 0x40, "extent" }, { 0x80, "64bit" },
    { 0x100, "mmp" }, { 0x200, "flex_bg" }, { 0x400, "ea_inode" }, { 0x1000, "dirdata" }, { 0x2000, "metadata_csum_seed" }, { 0x4000, "large_dir" },
    { 0x8000, "inline_data" }, { 0x10000, "encrypt" }, { 0x20000, "casefold" },
};
static const FeatName F_RO[] = {
    { 0x1, "sparse_super" }, { 0x2, "large_file" }, { 0x4, "btree_dir" }, { 0x8, "huge_file" }, { 0x10, "uninit_bg" }, { 0x20, "dir_nlink" }, { 0x40, "extra_isize" },
    { 0x80, "has_snapshot" }, { 0x100, "quota" }, { 0x200, "bigalloc" }, { 0x400, "metadata_csum" }, { 0x800, "replica" }, { 0x1000, "read-only" }, { 0x2000, "project" },
    { 0x4000, "shared_blocks" }, { 0x8000, "verity" }, { 0x10000, "orphan_present" },
};

static size_t feat_text(char *buf, size_t cap, size_t n, const FeatName *t, size_t nt, u32 word) {
    for (size_t i = 0; i < nt; i++) {
        if (!(word & t[i].bit)) continue;
        int w = snprintf(buf + n, n < cap ? cap - n : 0, "%s%s", n ? " " : "", t[i].name);
        if (w > 0) n += (size_t)w;
        word &= ~t[i].bit;
    }
    if (word) {                                                 /* bits that have no name here */
        int w = snprintf(buf + n, n < cap ? cap - n : 0, "%s0x%x", n ? " " : "", word);
        if (w > 0) n += (size_t)w;
    }
    return n;
}

size_t ext4_feature_text(const Ext4Vol *v, char *buf, size_t cap) {
    size_t n = 0;
    if (cap) buf[0] = 0;
    n = feat_text(buf, cap, n, F_COMPAT, sizeof F_COMPAT / sizeof *F_COMPAT, v->compat);
    n = feat_text(buf, cap, n, F_INCOMPAT, sizeof F_INCOMPAT / sizeof *F_INCOMPAT, v->incompat);
    n = feat_text(buf, cap, n, F_RO, sizeof F_RO / sizeof *F_RO, v->ro_compat);
    return n;
}

/* ext3 can do what ext2 can plus a journal; everything else (extents, 64bit, flex_bg, huge_file, ...) is ext4. The name is only a label: all three are read by this code. */
static const char *flavour_of(u32 compat, u32 incompat, u32 ro_compat) {
    const u32 ext3_incompat = E4_INCOMPAT_FILETYPE | E4_INCOMPAT_RECOVER | E4_INCOMPAT_META_BG;
    const u32 ext3_ro = 0x1u | 0x2u | 0x4u;                     /* sparse_super, large_file, btree_dir */
    if ((incompat & ~ext3_incompat) || (ro_compat & ~ext3_ro)) return "ext4";
    return (compat & E4_COMPAT_HAS_JOURNAL) ? "ext3" : "ext2";
}

const char *ext4_flavour(const Ext4Vol *v) { return flavour_of(v->compat, v->incompat, v->ro_compat); }

/* ---------------------------------------------------------------------------------------------- the device and the cache */

static int devread(Ext4Vol *v, u64 off, void *buf, size_t len) {
    if (!len) return 0;
    if (off > v->dev.size || len > v->dev.size - off) return -EIO;
    v->n_reads++;
    v->n_read_bytes += len;
    return v->dev.read(v->dev.ctx, off, buf, len);
}

/* A block of metadata. The pointer is valid until the next call of bget(): every user copies what it needs (or finishes with it) before asking for another block.
 * Direct mapped: a block lives in slot (number mod slots), so a conflict costs a read and nothing else. */
static int bget(Ext4Vol *v, u64 blk, const u8 **out) {
    if (blk >= v->blocks) return -EIO;
#ifdef EXT4_HOST
    if (ext4_meta_hook) ext4_meta_hook(blk, v->bsize);
#endif
    Ext4Slot *s = &v->slots[blk & (v->nslots - 1)];
    if (s->buf && s->blk == blk) {
        v->n_hits++;
        *out = s->buf;
        return 0;
    }
    if (!s->buf) {
        s->buf = E4_ALLOC(v->bsize);
        if (!s->buf) return -ENOMEM;
    }
    s->blk = ~0ull;                                             /* nothing valid in the slot while it is being filled */
    int rc = devread(v, blk << v->bshift, s->buf, v->bsize);
    if (rc) return rc;
    s->blk = blk;
    v->n_misses++;
    *out = s->buf;
    return 0;
}

/* ---------------------------------------------------------------------------------------------- the volume */

/* The superblock's own checks. `sb` is the 1024 bytes at byte offset 1024. Returns the filesystem name and label for ext4_probe_super() too. */
int ext4_probe_super(const void *sbv, size_t len, char *fs, size_t fsn, char *label, size_t labeln) {
    const u8 *sb = sbv;
    if (len < 1024 || e4_le16(sb + 0x38) != 0xEF53) return 0;
    u32 rev = e4_le32(sb + 0x4C);
    u32 log_bs = e4_le32(sb + 0x18), bpg = e4_le32(sb + 0x20), ipg = e4_le32(sb + 0x28);
    if (rev > 1 || log_bs > E4_MAX_BLOCK_LOG || !e4_le32(sb + 0x04) || !bpg || !ipg) return 0;       /* two bytes of random data are 0xEF53 now and then */
    u32 compat = rev ? e4_le32(sb + 0x5C) : 0, incompat = rev ? e4_le32(sb + 0x60) : 0, ro = rev ? e4_le32(sb + 0x64) : 0;
    if (incompat & E4_INCOMPAT_JOURNAL_DEV) return 0;           /* an external journal is not a filesystem */
    if (fs && fsn) strlcpy(fs, flavour_of(compat, incompat, ro), fsn);
    if (label && labeln) {
        size_t n = 0;
        for (; n < 16 && sb[0x78 + n] && n + 1 < labeln; n++) label[n] = (sb[0x78 + n] >= 32 && sb[0x78 + n] < 127) ? (char)sb[0x78 + n] : '?';
        label[n] = 0;
    }
    return 1;
}

/* Does group g carry a copy of the superblock and the descriptors? Group 0 always; the others depend on sparse_super, sparse_super2 (two named groups) or all do. */
static bool has_super(const Ext4Vol *v, u32 g) {
    if (g == 0) return true;
    if (v->compat & E4_COMPAT_SPARSE_SUPER2) return g == v->backup_bgs[0] || g == v->backup_bgs[1];
    if (!(v->ro_compat & E4_RO_SPARSE_SUPER)) return true;
    if (g == 1) return true;
    if (!(g & 1)) return false;
    static const u32 bases[3] = { 3, 5, 7 };
    for (int i = 0; i < 3; i++) {
        u64 x = bases[i];
        while (x < g) x *= bases[i];
        if (x == g) return true;
    }
    return false;
}

static int open_checked(Ext4Vol **out, const Ext4Dev *dev, const u8 *sb, char *why, size_t whyn) {
    if (e4_le16(sb + 0x38) != 0xEF53) {
        WHY("not an ext2/3/4 volume: the magic 0xEF53 is not at byte 1080 (found 0x%04x)", e4_le16(sb + 0x38));
        return -EINVAL;
    }
    u32 rev = e4_le32(sb + 0x4C);
    if (rev > 1) { WHY("unknown superblock revision %u", rev); return -EINVAL; }
    u32 log_bs = e4_le32(sb + 0x18);
    if (log_bs > E4_MAX_BLOCK_LOG) { WHY("block size 1024 << %u is not supported (at most 64 KiB)", log_bs); return -EINVAL; }
    u32 bsize = 1024u << log_bs;
    u32 compat = 0, incompat = 0, ro = 0, inode_size = 128, first_ino = 11;
    if (rev == 1) {
        inode_size = e4_le16(sb + 0x58);
        first_ino = e4_le32(sb + 0x54);
        compat = e4_le32(sb + 0x5C);
        incompat = e4_le32(sb + 0x60);
        ro = e4_le32(sb + 0x64);
    }
    if (incompat & ~E4_INCOMPAT_SUPPORTED) {
        char names[160];
        names[0] = 0;
        size_t n = feat_text(names, sizeof names, 0, F_INCOMPAT, sizeof F_INCOMPAT / sizeof *F_INCOMPAT, incompat & ~E4_INCOMPAT_SUPPORTED);
        (void)n;
        WHY("the volume needs incompatible features this driver does not have: %s (a read-only mount can still not guess what they mean)", names);
        return -EINVAL;
    }
    bool is64 = (incompat & E4_INCOMPAT_64BIT) != 0, big = (ro & E4_RO_BIGALLOC) != 0;
    if (inode_size < 128 || inode_size > bsize || !is_pow2(inode_size)) { WHY("inode size %u is impossible with %u-byte blocks", inode_size, bsize); return -EINVAL; }
    if (first_ino < 11) { WHY("first non-reserved inode %u is below 11", first_ino); return -EINVAL; }

    u64 blocks = e4_le32(sb + 0x04) | (is64 ? (u64)e4_le32(sb + 0x150) << 32 : 0);
    u64 r_blocks = e4_le32(sb + 0x08) | (is64 ? (u64)e4_le32(sb + 0x154) << 32 : 0);
    u64 free_blocks = e4_le32(sb + 0x0C) | (is64 ? (u64)e4_le32(sb + 0x158) << 32 : 0);
    u32 ratio = 1;
    if (big) {
        u32 log_cl = e4_le32(sb + 0x1C);
        if (log_cl < log_bs || log_cl - log_bs > 15) { WHY("allocation cluster size 1024 << %u does not fit with %u-byte blocks", log_cl, bsize); return -EINVAL; }
        ratio = 1u << (log_cl - log_bs);
    }
    u32 bpg = e4_le32(sb + 0x20), cpg = e4_le32(sb + 0x24), ipg = e4_le32(sb + 0x28);
    if (big) {
        if (!cpg || cpg > 8 * bsize || (u64)cpg * ratio != bpg) { WHY("clusters per group %u and blocks per group %u disagree", cpg, bpg); return -EINVAL; }
    } else if (!bpg || bpg > 8 * bsize) {
        WHY("blocks per group %u is impossible with %u-byte blocks (one bitmap block holds at most %u)", bpg, bsize, 8 * bsize);
        return -EINVAL;
    }
    if (!ipg || ipg > 8 * bsize) { WHY("inodes per group %u is impossible with %u-byte blocks", ipg, bsize); return -EINVAL; }
    u32 fdb = e4_le32(sb + 0x14);
    if (fdb != ((bsize == 1024 && !big) ? 1u : 0u)) { WHY("first data block %u does not fit with %u-byte blocks", fdb, bsize); return -EINVAL; }
    if (blocks <= fdb) { WHY("the volume has no blocks"); return -EINVAL; }
    u64 ng = (blocks - fdb + bpg - 1) / bpg;
    if (ng > E4_MAX_GROUPS) { WHY("%llu block groups are more than this driver handles (%u)", (unsigned long long)ng, E4_MAX_GROUPS); return -EINVAL; }
    u32 inodes = e4_le32(sb + 0x00);
    if (inodes < first_ino || inodes > ng * ipg || inodes <= (ng - 1) * ipg) {
        WHY("%u inodes do not fit %llu groups of %u", inodes, (unsigned long long)ng, ipg);
        return -EINVAL;
    }
    u32 ds = 32;
    if (is64) {
        ds = e4_le16(sb + 0xFE);
        if (ds < 64 || ds > 1024 || !is_pow2(ds) || ds > bsize) { WHY("group descriptor size %u is not usable", ds); return -EINVAL; }
    }
    if (blocks > dev->size / bsize) {
        WHY("the superblock says %llu blocks of %u bytes, but the device holds only %llu bytes (a volume cut short, or a partition made too small)",
            (unsigned long long)blocks, bsize, (unsigned long long)dev->size);
        return -EINVAL;
    }

    Ext4Vol *v = E4_ZALLOC(sizeof *v);
    if (!v) return -ENOMEM;
    v->dev = *dev;
    v->bsize = bsize;
    v->bshift = 10 + log_bs;
    v->ratio = ratio;
    v->blocks = blocks;
    v->r_blocks = r_blocks;
    v->free_blocks_sb = free_blocks;
    v->inodes = inodes;
    v->first_ino = first_ino;
    v->ipg = ipg;
    v->bpg = bpg;
    v->ngroups = (u32)ng;
    v->first_data_block = fdb;
    v->inode_size = inode_size;
    v->itb = (u32)(((u64)ipg * inode_size + bsize - 1) / bsize);
    v->desc_size = ds;
    v->dpb = bsize / ds;
    v->first_meta_bg = (incompat & E4_INCOMPAT_META_BG) ? e4_le32(sb + 0x104) : 0;
    v->rev = rev;
    v->compat = compat;
    v->incompat = incompat;
    v->ro_compat = ro;
    v->state = e4_le16(sb + 0x3A);
    v->backup_bgs[0] = e4_le32(sb + 0x24C);
    v->backup_bgs[1] = e4_le32(sb + 0x250);
    v->overhead = e4_le32(sb + 0x248);
    v->last_orphan = e4_le32(sb + 0xE8);
    v->free_inodes_sb = e4_le32(sb + 0x10);
    memcpy(v->uuid, sb + 0x68, 16);
    for (u32 i = 0; i < 16 && sb[0x78 + i]; i++) v->label[i] = (sb[0x78 + i] >= 32 && sb[0x78 + i] < 127) ? (char)sb[0x78 + i] : '?';

    u32 n = E4_CACHE_BYTES / bsize;
    if (n < 8) n = 8;
    while (n & (n - 1)) n &= n - 1;                             /* a power of two: the slot is the block number's low bits */
    v->nslots = n;
    v->slots = E4_ZALLOC((size_t)n * sizeof *v->slots);
    if (!v->slots) { E4_FREE(v); return -ENOMEM; }

    Ext4File *root;
    int rc = ext4_file_open(v, E4_ROOT_INO, &root);
    if (rc) {
        WHY("the root directory (inode 2) cannot be read (%d): the group descriptors or the inode table are damaged", rc);
        ext4_vol_close(v);
        return rc < 0 ? rc : -EIO;
    }
    bool ok = (root->mode & E4_S_IFMT) == E4_S_IFDIR && !root->encrypted;
    ext4_file_close(root);
    if (!ok) {
        WHY("inode 2 is not an ordinary directory");
        ext4_vol_close(v);
        return -EIO;
    }
    *out = v;
    return 0;
}

int ext4_vol_open(Ext4Vol **out, const Ext4Dev *dev, char *why, size_t whyn) {
    *out = NULL;
    if (why && whyn) why[0] = 0;
    if (dev->size < 2048) { WHY("the device is smaller than a superblock"); return -EINVAL; }
    u8 *sb = E4_ALLOC(1024);                                    /* heap: the block layer never reads into a stack */
    if (!sb) return -ENOMEM;
    int rc = dev->read(dev->ctx, 1024, sb, 1024);
    if (rc) {
        WHY("cannot read the superblock (%d)", rc);
        E4_FREE(sb);
        return rc;
    }
    rc = open_checked(out, dev, sb, why, whyn);
    E4_FREE(sb);
    return rc;
}

void ext4_vol_close(Ext4Vol *v) {
    if (!v) return;
    if (v->slots) for (u32 i = 0; i < v->nslots; i++) E4_FREE(v->slots[i].buf);
    E4_FREE(v->slots);
    E4_FREE(v);
}

/* ---------------------------------------------------------------------------------------------- group descriptors and inodes */

/* Group g's descriptor: the fields a reader needs. Where it lives: after the superblock (the old layout), or, for the groups from first_meta_bg * dpb on with meta_bg, in
 * the first block of the first group of g's meta group (after that group's superblock copy, if it has one). */
int ext4_group_info(Ext4Vol *v, u32 g, u64 *itable, u32 *free_blocks, u32 *free_inodes) {
    if (g >= v->ngroups) return -EIO;
    u32 meta = g / v->dpb;
    u64 blk;
    if (!(v->incompat & E4_INCOMPAT_META_BG) || meta < v->first_meta_bg) {
        blk = (u64)v->first_data_block + 1 + meta;
    } else {
        u64 first = (u64)meta * v->dpb;                         /* <= g, so inside the volume */
        blk = (u64)v->first_data_block + first * v->bpg + (has_super(v, (u32)first) ? 1 : 0);
    }
    const u8 *p;
    int rc = bget(v, blk, &p);
    if (rc) return rc;
    const u8 *d = p + (u64)(g % v->dpb) * v->desc_size;         /* dpb * desc_size <= block size: inside the block */
    bool wide = v->desc_size >= 64;
    if (itable) *itable = e4_le32(d + 0x08) | (wide ? (u64)e4_le32(d + 0x28) << 32 : 0);
    if (free_blocks) *free_blocks = e4_le16(d + 0x0C) | (wide ? (u32)e4_le16(d + 0x2C) << 16 : 0);
    if (free_inodes) *free_inodes = e4_le16(d + 0x0E) | (wide ? (u32)e4_le16(d + 0x2E) << 16 : 0);
    return 0;
}

/* Where inode `ino` is: a pointer to its inode_size bytes, valid until the next bget(). */
static int inode_ptr(Ext4Vol *v, u32 ino, const u8 **raw) {
    if (ino == 0 || ino > v->inodes) return -EIO;
    u32 g = (ino - 1) / v->ipg, idx = (ino - 1) % v->ipg;
    u64 itable;
    int rc = ext4_group_info(v, g, &itable, NULL, NULL);
    if (rc) return rc;
    if (itable < v->first_data_block || itable > v->blocks || v->itb > v->blocks - itable) return -EIO;     /* the table must lie inside the volume */
    u64 byte = (u64)idx * v->inode_size;
    const u8 *p;
    rc = bget(v, itable + (byte >> v->bshift), &p);
    if (rc) return rc;
    *raw = p + (byte & (v->bsize - 1));                         /* an inode never straddles a block: its size divides the block size */
    return 0;
}

static u32 dt_of_mode(u32 mode) {
    switch (mode & E4_S_IFMT) {
    case E4_S_IFREG: return E4_DT_REG;
    case E4_S_IFDIR: return E4_DT_DIR;
    case E4_S_IFLNK: return E4_DT_LNK;
    case E4_S_IFCHR: return E4_DT_CHR;
    case E4_S_IFBLK: return E4_DT_BLK;
    case E4_S_IFIFO: return E4_DT_FIFO;
    case E4_S_IFSOCK: return E4_DT_SOCK;
    default: return E4_DT_UNKNOWN;
    }
}

/* The in-inode extended attribute system.data (name index 7): the part of an inline file that does not fit in i_block. `raw` is the whole inode. 1 = found (*val points
 * into the inode), 0 = there is none, -EIO = the attribute area contradicts itself. The area starts at 128 + extra_isize with the magic 0xEA020000; entries follow (name length u8,
 * name index u8, value offset u16 counted from the first entry, value inode u32, value size u32, hash u32, name), each padded to 4 bytes, ended by a u32 zero. */
static int find_inline_ea(const u8 *raw, u32 isz, u32 extra, const u8 **val, u32 *len) {
    *val = NULL;
    *len = 0;
    u32 start = 128 + extra;
    if (start + 4 > isz || e4_le32(raw + start) != E4_XATTR_MAGIC) return 0;
    const u8 *base = raw + start + 4;
    u32 room = isz - start - 4;
    u32 o = 0;
    while (o + 16 <= room) {
        const u8 *e = base + o;
        if (e4_le32(e) == 0) break;
        u32 nlen = e[0], idx = e[1], voff = e4_le16(e + 2), vinum = e4_le32(e + 4), vsize = e4_le32(e + 8);
        u32 elen = (16 + nlen + 3) & ~3u;
        if (o + elen > room) return -EIO;
        if (idx == 7 && nlen == 4 && !memcmp(e + 16, "data", 4)) {
            if (vinum != 0 || (u64)voff + vsize > room) return -EIO;
            *val = base + voff;
            *len = vsize;
            return 1;
        }
        o += elen;
    }
    return 0;
}

/* A time: seconds as a signed 32-bit number plus the low two bits of the extra field as bits 32 and 33; the other 30 bits of the extra field are nanoseconds. */
static void decode_time(const u8 *raw, u32 off, u32 extra_isize, u32 extra_off, s64 *sec, u32 *ns) {
    s64 s = (s32)e4_le32(raw + off);
    u32 ns_v = 0;
    if (extra_isize + 128 >= extra_off + 4) {
        u32 x = e4_le32(raw + extra_off);
        s += (s64)(x & 3) << 32;
        ns_v = x >> 2;
        if (ns_v > 999999999u) ns_v = 0;
    }
    *sec = s;
    *ns = ns_v;
}

static int decode_inode(Ext4Vol *v, u32 ino, const u8 *raw, Ext4File *f) {
    u32 isz = v->inode_size;
    u32 mode = e4_le16(raw + 0x00);
    switch (mode & E4_S_IFMT) {
    case E4_S_IFREG: case E4_S_IFDIR: case E4_S_IFLNK: case E4_S_IFCHR: case E4_S_IFBLK: case E4_S_IFIFO: case E4_S_IFSOCK: break;
    default: return -EIO;                                       /* an unused inode (mode 0) or garbage */
    }
    u32 flags = e4_le32(raw + 0x20);
    u32 extra = 0;
    if (isz > 128) {
        extra = e4_le16(raw + 0x80);
        if ((extra & 3) || 128 + extra > isz) return -EIO;
    }
    f->v = v;
    f->ino = ino;
    f->mode = mode;
    f->flags = flags;
    f->uid = e4_le16(raw + 0x02) | (u32)e4_le16(raw + 0x78) << 16;
    f->gid = e4_le16(raw + 0x18) | (u32)e4_le16(raw + 0x7A) << 16;
    f->nlink = e4_le16(raw + 0x1A);
    u64 size = e4_le32(raw + 0x04);
    if ((mode & E4_S_IFMT) == E4_S_IFREG || ((mode & E4_S_IFMT) == E4_S_IFDIR && (v->incompat & E4_INCOMPAT_LARGEDIR))) size |= (u64)e4_le32(raw + 0x6C) << 32;
    if (size > E4_MAX_SIZE) return -EIO;
    f->size = size;
    decode_time(raw, 0x08, extra, 0x8C, &f->atime, &f->atime_ns);
    decode_time(raw, 0x0C, extra, 0x84, &f->ctime, &f->ctime_ns);
    decode_time(raw, 0x10, extra, 0x88, &f->mtime, &f->mtime_ns);
    f->crtime = 0;
    f->crtime_ns = 0;
    if (extra + 128 >= 0x90 + 4) decode_time(raw, 0x90, extra, 0x94, &f->crtime, &f->crtime_ns);
    u64 blocks = e4_le32(raw + 0x1C) | (u64)e4_le16(raw + 0x74) << 32;
    if ((v->ro_compat & E4_RO_HUGE_FILE) && (flags & E4_FL_HUGE_FILE)) blocks *= v->bsize / 512;      /* this inode counts in file system blocks */
    f->blocks512 = blocks;
    f->generation = e4_le32(raw + 0x64);
    memcpy(f->iblock, raw + 0x28, 60);
    f->encrypted = (flags & E4_FL_ENCRYPT) != 0;
    f->extents = (flags & E4_FL_EXTENTS) != 0;
    f->inline_data = (flags & E4_FL_INLINE_DATA) != 0;
    if ((f->extents && !(v->incompat & E4_INCOMPAT_EXTENTS)) || (f->inline_data && !(v->incompat & E4_INCOMPAT_INLINE_DATA)) || (f->extents && f->inline_data)) return -EIO;

    switch (mode & E4_S_IFMT) {
    case E4_S_IFCHR: case E4_S_IFBLK: {                         /* the device number: the old 16-bit encoding in i_block[0], else the new one in i_block[1] */
        u32 d0 = e4_le32(f->iblock), d1 = e4_le32(f->iblock + 4), major, minor;
        if (d0) { major = (d0 >> 8) & 0xff; minor = d0 & 0xff; }
        else { major = (d1 & 0xfff00) >> 8; minor = (d1 & 0xff) | ((d1 >> 12) & 0xfff00); }
        f->rdev = ((u64)(major & 0xfff) << 8) | (minor & 0xff) | ((u64)(major & ~0xfffu) << 32) | ((u64)(minor & ~0xffu) << 12);
        break;
    }
    case E4_S_IFLNK: {
        u64 acl = e4_le32(raw + 0x68) | (u64)e4_le16(raw + 0x76) << 32;
        u64 ea_sectors = acl ? (u64)v->bsize * v->ratio / 512 : 0;
        f->fast_link = !f->inline_data && !f->extents && f->blocks512 == ea_sectors;
        if (f->fast_link && size >= 60) return -EIO;
        break;
    }
    default: break;
    }
    if (f->inline_data) {
        const u8 *val;
        u32 len;
        int rc = find_inline_ea(raw, isz, extra, &val, &len);
        if (rc < 0) return rc;
        if (rc == 1 && len) {
            f->inl = E4_ALLOC(len);
            if (!f->inl) return -ENOMEM;
            memcpy(f->inl, val, len);
            f->inl_len = len;
        }
        if ((mode & E4_S_IFMT) == E4_S_IFREG || (mode & E4_S_IFMT) == E4_S_IFLNK) {
            if (size > 60 + (u64)f->inl_len) return -EIO;       /* more bytes than the inode holds */
        } else if ((mode & E4_S_IFMT) != E4_S_IFDIR) {
            return -EIO;                                        /* no other type keeps inline data */
        }
    }
    if ((mode & E4_S_IFMT) == E4_S_IFDIR && !f->inline_data && (size >> v->bshift) > v->blocks) return -EIO;   /* a directory cannot have more blocks than the volume */
    if (!f->extents && !f->inline_data && !f->fast_link && ((mode & E4_S_IFMT) == E4_S_IFREG || (mode & E4_S_IFMT) == E4_S_IFDIR || (mode & E4_S_IFMT) == E4_S_IFLNK)) {
        u64 p = v->bsize / 4, maxb = 12 + p + p * p + p * p * p;                  /* what a block map can address */
        if (((size + v->bsize - 1) >> v->bshift) > maxb) return -EIO;
    }
    return 0;
}

int ext4_file_open(Ext4Vol *v, u32 ino, Ext4File **out) {
    *out = NULL;
    if (ino < v->first_ino && ino != E4_ROOT_INO) return -EIO;  /* the journal, the resize inode and the other reserved inodes are not reached by a name */
    const u8 *raw;
    int rc = inode_ptr(v, ino, &raw);
    if (rc) return rc;
    Ext4File *f = E4_ZALLOC(sizeof *f);
    if (!f) return -ENOMEM;
    rc = decode_inode(v, ino, raw, f);
    if (rc) {
        ext4_file_close(f);
        return rc;
    }
    *out = f;
    return 0;
}

void ext4_file_close(Ext4File *f) {
    if (!f) return;
    E4_FREE(f->inl);
    E4_FREE(f->ra);
    E4_FREE(f->hidx);
    E4_FREE(f);
}

/* The type of an inode without keeping it (directory entries of volumes without the filetype feature). */
static u32 inode_dt(Ext4Vol *v, u32 ino) {
    if (ino < v->first_ino && ino != E4_ROOT_INO) return E4_DT_UNKNOWN;
    const u8 *raw;
    if (inode_ptr(v, ino, &raw)) return E4_DT_UNKNOWN;
    return dt_of_mode(e4_le16(raw));
}

/* ---------------------------------------------------------------------------------------------- mapping logical blocks */

static bool blk_ok(const Ext4Vol *v, u64 b) { return b >= v->first_data_block && b < v->blocks; }

#define E4_RUN_CAP (1ull << 40)                                 /* a run of holes without an end is cut here (a 64 KiB block times this still fits in 64 bits) */

/* The extent tree. The root is in i_block (60 bytes: a 12-byte header and room for four entries), the other nodes are whole blocks of the volume. Every node has the
 * header magic 0xF30A, entries (u16) at most max (u16), and a depth (u16) that is one less than its parent's: so the walk ends after at most E4_MAX_DEPTH + 1 nodes.
 * Index entries: first logical block (u32), child (u32 low, u16 high); leaf entries: first logical block (u32), length (u16), start (u16 high, u32 low). */
static int map_extents(Ext4File *f, u64 lblk, Ext4Map *m) {
    Ext4Vol *v = f->v;
    const u8 *node = f->iblock;
    u32 nsize = 60;
    int depth = 0;
    u64 limit = ~0ull;                                          /* where the logical blocks of the next subtree begin (nothing: no limit) */
    for (int level = 0;; level++) {
        if (e4_le16(node) != E4_EXT_MAGIC) return -EIO;
        u32 ent = e4_le16(node + 2), max = e4_le16(node + 4), dep = e4_le16(node + 6);
        if (level == 0) {
            if (dep > E4_MAX_DEPTH) return -EIO;
            depth = (int)dep;
        } else if ((int)dep != depth - level) {
            return -EIO;
        }
        if (max > (nsize - 12) / 12 || ent > max) return -EIO;
        const u8 *es = node + 12;
        /* the last entry whose first logical block is <= lblk (binary search; on unsorted data it still ends, and the checks below catch the nonsense) */
        s64 lo = 0, hi = (s64)ent - 1, found = -1;
        while (lo <= hi) {
            s64 mid = (lo + hi) / 2;
            if (e4_le32(es + mid * 12) <= lblk) { found = mid; lo = mid + 1; } else hi = mid - 1;
        }
        if (dep == 0) {                                         /* a leaf */
            if (found >= 0) {
                const u8 *e = es + found * 12;
                u64 first = e4_le32(e);
                u32 len = e4_le16(e + 4);
                bool unwritten = len > 32768;
                u64 n = unwritten ? len - 32768 : len;
                if (n == 0) return -EIO;
                u64 start = e4_le32(e + 8) | (u64)e4_le16(e + 6) << 32;
                if (start >= v->blocks || n > v->blocks - start) return -EIO;         /* the blocks must lie inside the volume */
                if (lblk < first + n) {
                    m->kind = unwritten ? E4_MAP_UNWRITTEN : E4_MAP_MAPPED;
                    m->pblk = start + (lblk - first);
                    m->run = first + n - lblk;
                    return 0;
                }
            }
            u64 end = limit;                                    /* a hole, up to the next extent (or the end of what this leaf covers) */
            if (found + 1 < (s64)ent) { u64 nx = e4_le32(es + (found + 1) * 12); if (nx < end) end = nx; }
            if (end <= lblk) return -EIO;                       /* the entries are not in order */
            m->kind = E4_MAP_HOLE;
            m->pblk = 0;
            m->run = end == ~0ull || end - lblk > E4_RUN_CAP ? E4_RUN_CAP : end - lblk;
            return 0;
        }
        if (ent == 0) return -EIO;                              /* an index node with nothing in it */
        if (found < 0) {                                        /* before the first subtree: a hole up to it */
            u64 end = e4_le32(es);
            if (limit < end) end = limit;
            if (end <= lblk) return -EIO;
            m->kind = E4_MAP_HOLE;
            m->pblk = 0;
            m->run = end - lblk > E4_RUN_CAP ? E4_RUN_CAP : end - lblk;
            return 0;
        }
        if (found + 1 < (s64)ent) { u64 nx = e4_le32(es + (found + 1) * 12); if (nx < limit) limit = nx; }
        const u8 *e = es + found * 12;
        u64 child = e4_le32(e + 4) | (u64)e4_le16(e + 8) << 32;
        int rc = bget(v, child, &node);
        if (rc) return rc;
        nsize = v->bsize;
    }
}

/* The ext2/ext3 block map: i_block[0..11] are the first twelve blocks, [12] is a block of block numbers (single indirect), [13] points to blocks of those (double), [14] to
 * those (triple). Zero is a hole. A run extends over neighbours that continue it, inside one pointer block. */
static int map_walk_ind(Ext4File *f, u64 root, int levels, u64 idx, Ext4Map *m) {
    Ext4Vol *v = f->v;
    u64 p = v->bsize / 4;
    u64 cov = 1;                                                /* data blocks under one pointer of the top level */
    for (int i = 1; i < levels; i++) cov *= p;
    u64 cur = root;
    for (int lv = levels; lv >= 1; lv--) {
        if (cur == 0) {                                         /* the whole subtree is missing: a hole to its end */
            m->kind = E4_MAP_HOLE;
            m->pblk = 0;
            m->run = cov * p - idx;
            return 0;
        }
        if (!blk_ok(v, cur)) return -EIO;
        const u8 *arr;
        int rc = bget(v, cur, &arr);
        if (rc) return rc;
        u64 i = idx / cov, rem = idx % cov;
        u64 b = e4_le32(arr + 4 * i);
        if (lv == 1) {                                          /* the pointers are the data blocks */
            u64 run = 1;
            if (b == 0) {
                while (i + run < p && e4_le32(arr + 4 * (i + run)) == 0) run++;
                m->kind = E4_MAP_HOLE;
                m->pblk = 0;
            } else {
                if (!blk_ok(v, b)) return -EIO;
                while (i + run < p && e4_le32(arr + 4 * (i + run)) == b + run && blk_ok(v, b + run)) run++;
                m->kind = E4_MAP_MAPPED;
                m->pblk = b;
            }
            m->run = run;
            return 0;
        }
        idx = rem;
        cur = b;
        cov /= p;
        if (cur == 0) {                                         /* the child is missing: a hole to the end of its range */
            m->kind = E4_MAP_HOLE;
            m->pblk = 0;
            m->run = cov * p - idx;
            return 0;
        }
    }
    return -EIO;                                                /* not reached */
}

static int map_blockmap(Ext4File *f, u64 lblk, Ext4Map *m) {
    Ext4Vol *v = f->v;
    u64 p = v->bsize / 4;
    if (lblk < 12) {
        u64 b = e4_le32(f->iblock + 4 * lblk), run = 1;
        if (b == 0) {
            while (lblk + run < 12 && e4_le32(f->iblock + 4 * (lblk + run)) == 0) run++;
            m->kind = E4_MAP_HOLE;
            m->pblk = 0;
        } else {
            if (!blk_ok(v, b)) return -EIO;
            while (lblk + run < 12 && e4_le32(f->iblock + 4 * (lblk + run)) == b + run && blk_ok(v, b + run)) run++;
            m->kind = E4_MAP_MAPPED;
            m->pblk = b;
        }
        m->run = run;
        return 0;
    }
    lblk -= 12;
    if (lblk < p) return map_walk_ind(f, e4_le32(f->iblock + 48), 1, lblk, m);
    lblk -= p;
    if (lblk < p * p) return map_walk_ind(f, e4_le32(f->iblock + 52), 2, lblk, m);
    lblk -= p * p;
    if (lblk < p * p * p) return map_walk_ind(f, e4_le32(f->iblock + 56), 3, lblk, m);
    return -EIO;                                                /* beyond what a block map can address */
}

int ext4_map_block(Ext4File *f, u64 lblk, Ext4Map *m) {
    if (f->mc_ok && lblk >= f->mc_l && lblk - f->mc_l < f->mc.run) {
        u64 d = lblk - f->mc_l;
        m->kind = f->mc.kind;
        m->pblk = f->mc.kind == E4_MAP_MAPPED ? f->mc.pblk + d : 0;
        m->run = f->mc.run - d;
        return 0;
    }
    int rc = f->extents ? map_extents(f, lblk, m) : map_blockmap(f, lblk, m);
    if (rc == 0) {
        f->mc_ok = true;
        f->mc_l = lblk;
        f->mc = *m;
    }
    return rc;
}

/* ---------------------------------------------------------------------------------------------- reading */

/* Reads [off, off + n) of a file with blocks: n has been clamped to the file's size by the caller. Holes and unwritten extents read as zeros. */
static ssize_t read_range(Ext4File *f, u64 off, u8 *out, size_t n) {
    Ext4Vol *v = f->v;
    size_t done = 0;
    while (done < n) {
        u64 pos = off + done;
        u64 lblk = pos >> v->bshift;
        u32 within = (u32)(pos & (v->bsize - 1));
        Ext4Map m;
        int rc = ext4_map_block(f, lblk, &m);
        if (rc) return done ? (ssize_t)done : rc;
        u64 avail = m.run * v->bsize - within;                  /* run <= 2^40 and bsize <= 2^16: no overflow */
        size_t take = (n - done) < avail ? n - done : (size_t)avail;
        if (m.kind != E4_MAP_MAPPED) {
            memset(out + done, 0, take);
        } else {
            rc = devread(v, (m.pblk << v->bshift) + within, out + done, take);
            if (rc) return done ? (ssize_t)done : rc;
        }
        done += take;
    }
    return (ssize_t)done;
}

/* Small reads (a program that reads a page at a time, a loader reading headers) go through a window of E4_RA_BYTES that is read in one piece. */
static ssize_t read_windowed(Ext4File *f, u64 off, u8 *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        u64 pos = off + done;
        if (!(f->ra_len && pos >= f->ra_off && pos < f->ra_off + f->ra_len)) {
            u64 ws = pos & ~(u64)(E4_RA_BYTES - 1);
            u64 we = ws + E4_RA_BYTES;
            if (we > f->size) we = f->size;
            if (!f->ra) {
                f->ra = E4_ALLOC(E4_RA_BYTES);
                if (!f->ra) return done ? (ssize_t)done : read_range(f, pos, buf + done, n - done);
            }
            f->ra_len = 0;
            ssize_t r = read_range(f, ws, f->ra, (size_t)(we - ws));
            if (r != (ssize_t)(we - ws)) return done ? (ssize_t)done : (r < 0 ? r : -EIO);
            f->ra_off = ws;
            f->ra_len = (u32)(we - ws);
        }
        u64 in = pos - f->ra_off;
        size_t take = (n - done) < f->ra_len - in ? n - done : (size_t)(f->ra_len - in);
        memcpy(buf + done, f->ra + in, take);
        done += take;
    }
    return (ssize_t)done;
}

/* Bytes of a file whose data is in the inode: i_block, then the value of system.data. */
static ssize_t read_inline(Ext4File *f, u64 off, u8 *out, size_t n) {
    size_t done = 0;
    while (done < n) {
        u64 pos = off + done;
        size_t take;
        if (pos < 60) {
            take = (n - done) < 60 - pos ? n - done : (size_t)(60 - pos);
            memcpy(out + done, f->iblock + pos, take);
        } else {
            u64 q = pos - 60;
            if (q >= f->inl_len) break;
            take = (n - done) < f->inl_len - q ? n - done : (size_t)(f->inl_len - q);
            memcpy(out + done, f->inl + q, take);
        }
        done += take;
    }
    return (ssize_t)done;
}

ssize_t ext4_read(Ext4File *f, u64 off, void *buf, size_t n) {
    u32 type = f->mode & E4_S_IFMT;
    if (type == E4_S_IFDIR) return -EISDIR;
    if (type != E4_S_IFREG && type != E4_S_IFLNK) return -EINVAL;
    if (f->encrypted) return -ENOKEY;
    if (off >= f->size || !n) return 0;
    if (n > f->size - off) n = (size_t)(f->size - off);
    if (f->inline_data) return read_inline(f, off, buf, n);
    if (f->fast_link) {
        size_t take = (n < 60 - off) ? n : (size_t)(60 - off);
        memcpy(buf, f->iblock + off, take);
        return (ssize_t)take;
    }
    if (n < E4_RA_BYTES / 2) return read_windowed(f, off, buf, n);
    return read_range(f, off, buf, n);
}

/* ---------------------------------------------------------------------------------------------- directories */

/* A directory's bytes are read in "areas": its blocks one by one, or, for an inline directory, the 56 bytes of entries after the parent's inode number in i_block and then the
 * value of system.data. A position in the directory (the cookie of readdir) is a byte offset: for blocks the offset in the file, for inline directories the offset in
 * the two areas one after the other. */
typedef struct { const u8 *p; u32 len; u64 base; } DirArea;

/* The area that holds position `pos`. 0 = found, 1 = past the end, <0 = error. A hole in a directory is damage. */
static int dir_area_at(Ext4File *d, u64 pos, DirArea *a) {
    Ext4Vol *v = d->v;
    if (d->inline_data) {
        if (pos < E4_INLINE_AREA) { a->p = d->iblock + 4; a->len = E4_INLINE_AREA; a->base = 0; return 0; }
        if (pos - E4_INLINE_AREA < d->inl_len) { a->p = d->inl; a->len = d->inl_len; a->base = E4_INLINE_AREA; return 0; }
        return 1;
    }
    u64 blk = pos >> v->bshift;
    if (blk >= ((d->size + v->bsize - 1) >> v->bshift)) return 1;
    Ext4Map m;
    int rc = ext4_map_block(d, blk, &m);
    if (rc) return rc;
    if (m.kind != E4_MAP_MAPPED) return -EIO;
    const u8 *p;
    rc = bget(v, m.pblk, &p);
    if (rc) return rc;
    a->p = p;
    a->len = v->bsize;
    a->base = blk << v->bshift;
    return 0;
}

/* The entry at offset o of an area of `len` bytes: inode (u32), record length (u16), name length (u8), type (u8), name. For a block size of 64 KiB the record length has only
 * 16 bits: 0 and 65535 mean the whole block, otherwise bits 16 and 17 sit in its low two bits. Returns the record length or -EIO. An entry with inode 0 is an unused slot (the
 * checksum tail and the index nodes of a hashed directory look like that): only its length is checked. */
static int dirent_at(const Ext4Vol *v, bool wide_rec, const u8 *a, u32 len, u32 o, u32 *ino, u32 *nlen, u32 *ftype) {
    if (len - o < 8) return -EIO;
    u32 rl = e4_le16(a + o + 4);
    if (wide_rec) rl = (rl == 0 || rl == 65535) ? 65536u : ((rl & 65532u) | ((rl & 3u) << 16));
    if (rl < 8 || (rl & 3) || rl > len - o) return -EIO;
    u32 in = e4_le32(a + o), nl = a[o + 6];
    if (in) {
        if (!nl || 8 + nl > rl || in > v->inodes) return -EIO;
    }
    *ino = in;
    *nlen = nl;
    *ftype = a[o + 7];
    return (int)rl;
}

static u32 ftype_to_dt(u32 ft) {
    switch (ft) {
    case 1: return E4_DT_REG;
    case 2: return E4_DT_DIR;
    case 3: return E4_DT_CHR;
    case 4: return E4_DT_BLK;
    case 5: return E4_DT_FIFO;
    case 6: return E4_DT_SOCK;
    case 7: return E4_DT_LNK;
    default: return E4_DT_UNKNOWN;
    }
}

int ext4_dir_next(Ext4File *d, u64 *cookie, Ext4DirEnt *out) {
    Ext4Vol *v = d->v;
    if ((d->mode & E4_S_IFMT) != E4_S_IFDIR) return -ENOTDIR;
    if (d->encrypted) return -ENOKEY;
    bool wide_rec = !d->inline_data && v->bsize == 65536;
    u64 pos = *cookie;
    for (;;) {
        DirArea a;
        int rc = dir_area_at(d, pos, &a);
        if (rc == 1) return 0;
        if (rc < 0) return rc;
        u32 o = (u32)(pos - a.base);
        if (o && !(d->dir_cookie_ok && pos == d->dir_cookie)) {  /* a position nobody returned (a seek): find the first entry at or after it by walking from the start of its area */
            u32 s = 0, ino, nl, ft;
            while (s < o) {
                int rl = dirent_at(v, wide_rec, a.p, a.len, s, &ino, &nl, &ft);
                if (rl < 0) return rl;
                s += (u32)rl;
            }
            o = s;
        }
        while (o < a.len) {
            u32 ino, nl, ft;
            int rl = dirent_at(v, wide_rec, a.p, a.len, o, &ino, &nl, &ft);
            if (rl < 0) return rl;
            u32 next = o + (u32)rl;
            if (ino) {
                const u8 *nm = a.p + o + 8;
                bool dots = (nl == 1 && nm[0] == '.') || (nl == 2 && nm[0] == '.' && nm[1] == '.');
                bool bad = false;
                for (u32 i = 0; i < nl && !bad; i++) bad = nm[i] == '/' || nm[i] == 0;
                if (bad) v->skipped_names++;
                if (!dots && !bad) {
                    out->ino = ino;
                    out->namelen = nl;
                    memcpy(out->name, nm, nl);
                    out->name[nl] = 0;
                    *cookie = a.base + next;
                    d->dir_cookie = *cookie;
                    d->dir_cookie_ok = true;
                    out->type = (v->incompat & E4_INCOMPAT_FILETYPE) ? ftype_to_dt(ft) : E4_DT_UNKNOWN;
                    if (out->type == E4_DT_UNKNOWN) out->type = inode_dt(v, ino);       /* this may replace the block `a` points into: nothing of it is used after */
                    return 1;
                }
            }
            o = next;
        }
        pos = a.base + a.len;
    }
}

static u32 name_hash(const u8 *s, size_t n) {                   /* FNV-1a: the index is ours, not the hash tree of the volume */
    u32 h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ s[i]) * 16777619u;
    return h;
}

/* A name index for a big directory: one pass counts the entries, a second puts each entry's position in an open-addressing table of at least twice as many slots. The
 * hash tree that ext4 keeps in such a directory is ignored (docs/EXT4.md), so without this every lookup would scan the directory: `ls -l` of 20000 files would read the
 * blocks 20000 times. The index only speeds up what the linear scan would find; a directory that cannot be indexed (damaged, too many entries) keeps being scanned. */
static void build_index(Ext4File *d) {
    Ext4Vol *v = d->v;
    d->hdone = true;
    if (d->inline_data || d->size < E4_INDEX_MIN || d->size >= (1ull << 32)) return;
    bool wide_rec = v->bsize == 65536;
    u64 nb = (d->size + v->bsize - 1) >> v->bshift;
    u32 count = 0, slots = 0;
    u32 *t = NULL;
    for (int pass = 0; pass < 2; pass++) {
        for (u64 b = 0; b < nb; b++) {
            DirArea a;
            if (dir_area_at(d, b << v->bshift, &a) != 0) goto fail;
            for (u32 o = 0; o < a.len;) {
                u32 ino, nl, ft;
                int rl = dirent_at(v, wide_rec, a.p, a.len, o, &ino, &nl, &ft);
                if (rl < 0) goto fail;
                if (ino) {
                    if (pass == 0) {
                        if (++count > E4_INDEX_MAX_ENTRIES) goto fail;
                    } else {
                        u32 slot = name_hash(a.p + o + 8, nl) & (slots - 1);
                        while (t[slot]) slot = (slot + 1) & (slots - 1);
                        t[slot] = (u32)(a.base + o) + 1;
                    }
                }
                o += (u32)rl;
            }
        }
        if (pass == 0) {
            slots = 64;
            while (slots < count * 2) slots <<= 1;
            t = E4_ZALLOC((size_t)slots * sizeof *t);
            if (!t) return;
        }
    }
    d->hidx = t;
    d->hmask = slots - 1;
    return;
fail:
    E4_FREE(t);
}

static int index_lookup(Ext4File *d, const char *name, size_t nl, u32 *ino_out) {
    Ext4Vol *v = d->v;
    bool wide_rec = v->bsize == 65536;
    u32 slot = name_hash((const u8 *)name, nl) & d->hmask;
    for (u32 probes = 0; d->hidx[slot] && probes <= d->hmask; probes++, slot = (slot + 1) & d->hmask) {
        u64 pos = d->hidx[slot] - 1;
        DirArea a;
        int rc = dir_area_at(d, pos, &a);
        if (rc < 0) return rc;
        if (rc == 1) return -EIO;
        u32 o = (u32)(pos - a.base), ino, enl, ft;
        int rl = dirent_at(v, wide_rec, a.p, a.len, o, &ino, &enl, &ft);
        if (rl < 0) return rl;
        if (ino && enl == nl && !memcmp(a.p + o + 8, name, nl)) {
            *ino_out = ino;
            return 0;
        }
    }
    return -ENOENT;
}

int ext4_dir_lookup(Ext4File *d, const char *name, u32 *ino_out) {
    Ext4Vol *v = d->v;
    if ((d->mode & E4_S_IFMT) != E4_S_IFDIR) return -ENOTDIR;
    if (d->encrypted) return -ENOKEY;
    size_t nl = strlen(name);
    if (!nl || nl > E4_NAME_MAX) return -ENOENT;
    if (d->inline_data && name[0] == '.' && (nl == 1 || (nl == 2 && name[1] == '.'))) {          /* an inline directory has neither entry: "." is itself, ".." is the number in i_block */
        *ino_out = nl == 1 ? d->ino : e4_le32(d->iblock);
        return 0;
    }
    bool wide_rec = !d->inline_data && v->bsize == 65536;
    if (!d->hdone) build_index(d);
    if (d->hidx) return index_lookup(d, name, nl, ino_out);
    u64 nareas = d->inline_data ? (d->inl_len ? 2 : 1) : ((d->size + v->bsize - 1) >> v->bshift);
    for (u64 k = 0; k < nareas; k++) {
        u64 idx = d->inline_data ? k : (d->dir_hint + k) % nareas;       /* start where the last lookup found its name: a listing followed by a lookup of each name walks the blocks once */
        DirArea a;
        int rc = dir_area_at(d, d->inline_data ? (idx ? E4_INLINE_AREA : 0) : idx << v->bshift, &a);
        if (rc < 0) return rc;
        if (rc == 1) return -ENOENT;
        for (u32 o = 0; o < a.len;) {
            u32 ino, enl, ft;
            int rl = dirent_at(v, wide_rec, a.p, a.len, o, &ino, &enl, &ft);
            if (rl < 0) return rl;
            if (ino && enl == nl && !memcmp(a.p + o + 8, name, nl)) {
                *ino_out = ino;
                if (!d->inline_data) d->dir_hint = idx;
                return 0;
            }
            o += (u32)rl;
        }
    }
    return -ENOENT;
}

/* ---------------------------------------------------------------------------------------------- symbolic links, statfs */

int ext4_readlink(Ext4File *f, char *out, size_t cap) {
    if ((f->mode & E4_S_IFMT) != E4_S_IFLNK) return -EINVAL;
    if (f->encrypted) return -ENOKEY;
    if (f->size == 0 || f->size > 4096) return -EIO;
    u8 *tmp = E4_ALLOC((size_t)f->size);
    if (!tmp) return -ENOMEM;
    ssize_t r = ext4_read(f, 0, tmp, (size_t)f->size);
    int rc;
    if (r < 0) {
        rc = (int)r;
    } else if ((u64)r != f->size || memchr(tmp, 0, (size_t)f->size)) {
        rc = -EIO;                                              /* short, or a NUL inside the target */
    } else {
        memcpy(out, tmp, cap < (size_t)f->size ? cap : (size_t)f->size);
        rc = (int)f->size;
    }
    E4_FREE(tmp);
    return rc;
}

int ext4_statfs(Ext4Vol *v, Ext4Stat *out) {
    u64 fb = 0, fi = 0;
    for (u32 g = 0; g < v->ngroups; g++) {
        u32 b, i;
        int rc = ext4_group_info(v, g, NULL, &b, &i);
        if (rc) return rc;
        fb += b;
        fi += i;
    }
    fb *= v->ratio;                                             /* the descriptors count clusters */
    if (fb > v->blocks) fb = v->blocks;
    out->bsize = v->bsize;
    u64 over = (u64)v->overhead * v->ratio;                     /* blocks that hold the filesystem's own structures, when the superblock says */
    out->blocks = (over && over < v->blocks) ? v->blocks - over : v->blocks - v->first_data_block;
    out->bfree = fb;
    out->bavail = fb > v->r_blocks ? fb - v->r_blocks : 0;
    out->files = v->inodes;
    out->ffree = fi > v->inodes ? v->inodes : fi;
    return 0;
}
