/*
 * FAT directories: parsing a directory's slots into an in-memory listing (one FatNode per entry, hashed by long and
 * short name, kept in slot order for readdir), finding room for new entries, writing and deleting entry sets.
 *
 * Invariants kept on disk: slots from FatDir.end_slot onwards are zero (readers stop at the first 0x00 slot), deleted
 * entries are 0xE5, a long-name set always sits in consecutive slots directly before its short entry.
 */
#include "fatfs.h"

/* ---------------------------------------------------------------------------------------------- bitmap helpers */

static inline bool bit_get(const FatDir *d, u32 i) { return (d->used[i >> 5] >> (i & 31)) & 1; }
static inline void bit_set(FatDir *d, u32 i) { d->used[i >> 5] |= 1u << (i & 31); }
static inline void bit_clr(FatDir *d, u32 i) { d->used[i >> 5] &= ~(1u << (i & 31)); }

static int bitmap_resize(FatDir *dd, u32 slots) {
    u32 words = (slots + 31) / 32;
    if (words <= dd->used_words) { dd->slots = slots; return 0; }
    u32 *nb = kzalloc((size_t)words * 4);
    if (!nb) return -ENOMEM;
    if (dd->used) memcpy(nb, dd->used, (size_t)dd->used_words * 4);
    kfree(dd->used);
    dd->used = nb;
    dd->used_words = words;
    dd->slots = slots;
    return 0;
}

/* ---------------------------------------------------------------------------------------------- nodes */

FatNode *fatd_node_new(FatVol *v, FatNode *parent, const char *name) {
    FatNode *n = kzalloc(sizeof *n);
    if (!n) return NULL;
    size_t len = strlen(name);
    n->name = kmalloc(len + 1);
    if (!n->name) { kfree(n); return NULL; }
    memcpy(n->name, name, len + 1);
    n->name_len = (u16)len;
    n->vol = v;
    n->parent = parent;
    n->ino = ++v->next_ino;
    v->nnodes++;
    return n;
}

void fatd_node_free(FatNode *n) {
    if (n->dir) fatd_drop(n);
    kfree(n->ext);
    kfree(n->name);
    n->vol->nnodes--;
    kfree(n);
}

/* ---------------------------------------------------------------------------------------------- hash tables */

static u32 sfn_hash(const u8 sfn[11]) {
    u32 h = 2166136261u;
    for (int i = 0; i < 11; i++) h = (h ^ sfn[i]) * 16777619u;
    return h;
}

static int tables_alloc(FatDir *dd, u32 hsize) {
    FatNode **h = kzalloc((size_t)hsize * sizeof *h), **s = kzalloc((size_t)hsize * sizeof *s);
    if (!h || !s) { kfree(h); kfree(s); return -ENOMEM; }
    kfree(dd->htab);
    kfree(dd->stab);
    dd->htab = h;
    dd->stab = s;
    dd->hsize = hsize;
    return 0;
}

static void tables_insert(FatDir *dd, FatNode *n) {
    u32 h = fatn_hash(n->name) & (dd->hsize - 1);
    n->hnext = dd->htab[h];
    dd->htab[h] = n;
    u32 s = sfn_hash(n->sfn) & (dd->hsize - 1);
    n->snext = dd->stab[s];
    dd->stab[s] = n;
}

static void tables_remove(FatDir *dd, FatNode *n) {
    FatNode **pp = &dd->htab[fatn_hash(n->name) & (dd->hsize - 1)];
    while (*pp && *pp != n) pp = &(*pp)->hnext;
    if (*pp) *pp = n->hnext;
    pp = &dd->stab[sfn_hash(n->sfn) & (dd->hsize - 1)];
    while (*pp && *pp != n) pp = &(*pp)->snext;
    if (*pp) *pp = n->snext;
    n->hnext = n->snext = NULL;
}

static int tables_grow(FatDir *dd) {
    u32 nsize = dd->hsize * 2;
    FatNode **oh = dd->htab, **os = dd->stab;
    u32 osize = dd->hsize;
    dd->htab = dd->stab = NULL;
    int rc = tables_alloc(dd, nsize);
    if (rc) { dd->htab = oh; dd->stab = os; dd->hsize = osize; return rc; }
    kfree(oh);
    kfree(os);
    for (u32 i = 0; i < dd->count; i++) tables_insert(dd, dd->order[i]);
    return 0;
}

/* Adds a child in slot order. */
static int dir_insert(FatDir *dd, FatNode *n) {
    if (dd->count == dd->cap) {
        u32 ncap = dd->cap ? dd->cap * 2 : 16;
        FatNode **no = krealloc(dd->order, (size_t)ncap * sizeof *no);
        if (!no) return -ENOMEM;
        dd->order = no;
        dd->cap = ncap;
    }
    if (dd->count * 2 >= dd->hsize) {
        int rc = tables_grow(dd);
        if (rc) return rc;
    }
    u32 lo = 0, hi = dd->count;                         /* keep `order` sorted by slot */
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (dd->order[mid]->slot < n->slot) lo = mid + 1; else hi = mid;
    }
    memmove(dd->order + lo + 1, dd->order + lo, (size_t)(dd->count - lo) * sizeof *dd->order);
    dd->order[lo] = n;
    dd->count++;
    tables_insert(dd, n);
    return 0;
}

static void dir_unlist(FatDir *dd, FatNode *n) {
    u32 lo = 0, hi = dd->count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (dd->order[mid]->slot < n->slot) lo = mid + 1; else hi = mid;
    }
    while (lo < dd->count && dd->order[lo] != n && dd->order[lo]->slot == n->slot) lo++;
    if (lo < dd->count && dd->order[lo] == n) {
        memmove(dd->order + lo, dd->order + lo + 1, (size_t)(dd->count - lo - 1) * sizeof *dd->order);
        dd->count--;
    }
    tables_remove(dd, n);
}

FatNode *fatd_find(FatNode *d, const char *name) {
    FatDir *dd = d->dir;
    if (!dd) return NULL;
    for (FatNode *n = dd->htab[fatn_hash(name) & (dd->hsize - 1)]; n; n = n->hnext)
        if (fatn_equal(n->name, name)) return n;
    u8 sfn[11];
    if (fatn_name_to_sfn(name, sfn)) {                  /* the 8.3 alias of a long name */
        for (FatNode *n = dd->stab[sfn_hash(sfn) & (dd->hsize - 1)]; n; n = n->snext)
            if (!memcmp(n->sfn, sfn, 11)) return n;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------------------------- slot I/O */

static int dir_write(FatNode *d, u64 off, const void *buf, size_t n) {
    const u8 *p = buf;
    while (n) {
        u64 disk, avail;
        int rc = chain_map(d, off, (u32)n, &disk, &avail);
        if (rc) return rc == -ENXIO ? -EIO : rc;
        size_t take = (size_t)MIN((u64)n, avail);
        rc = fc_write(d->vol, disk, p, take, FPC_META);
        if (rc) return rc;
        p += take;
        off += take;
        n -= take;
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------------- loading */

static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

typedef struct {
    bool active;
    u8 expect, count, csum;
    u32 first_slot;
    u16 units[20 * 13 + 1];
} LfnState;

static void fill_times(FatNode *n, const u8 *e) {
    n->mtime = fat_time_from_dos(rd16(e + 24), rd16(e + 22), 0);
    n->ctime = fat_time_from_dos(rd16(e + 16), rd16(e + 14), e[13]);
    n->atime = fat_time_from_dos(rd16(e + 18), 0, 0);
    if (!n->ctime) n->ctime = n->mtime;
    if (!n->atime) n->atime = n->mtime;
}

static u8 owner_for(FatNode *parent, const char *name, bool is_dir) {
    FatVol *v = parent->vol;
    if (parent->owner) return parent->owner;
    if (!v->userdirs || !(parent->flags & FN_ROOT) || !is_dir) return 0;
    for (int i = 0; i < v->nusers; i++)
        if (fatn_equal(v->users[i].name, name)) return (u8)(i + 1);
    return 0;
}

/* Turns one short entry (plus the long-name fragments collected before it) into a child node. */
static int load_entry(FatNode *d, const u8 *e, u32 slot, LfnState *lfn, FatNode **out) {
    FatVol *v = d->vol;
    char name[FAT_MAX_NAME * 4 + 1];
    bool used_lfn = false;
    u8 sfn[11];
    memcpy(sfn, e, 11);
    if (lfn->active && lfn->expect == 0 && lfn->csum == fatn_checksum(sfn)) {
        int n16 = 0;
        int max = lfn->count * 13;
        while (n16 < max && lfn->units[n16] != 0x0000) n16++;
        /* names that VFS cannot address ('/' inside, "." / "..", too long) fall back to the short name */
        if (n16 > 0 && fatn_utf16_to_utf8(lfn->units, n16, name, sizeof name) > 0 && strlen(name) <= GNAME_MAX &&
            !strchr(name, '/') && strcmp(name, ".") && strcmp(name, "..")) used_lfn = true;
    }
    if (!used_lfn) fatn_sfn_to_utf8(sfn, e[12], name, sizeof name);
    FatNode *n = fatd_node_new(v, d, name);
    if (!n) return -ENOMEM;
    memcpy(n->sfn, sfn, 11);
    n->attr = e[11] & 0x3F;
    n->ntres = e[12];
    if (n->attr & FAT_ATTR_DIR) n->flags |= FN_DIR;
    n->first = rd16(e + 26) | (v->bits == 32 ? (u32)rd16(e + 20) << 16 : 0);
    n->size = rd32(e + 28);
    fill_times(n, e);
    n->slot = used_lfn ? lfn->first_slot : slot;
    n->nslots = (u8)(slot - n->slot + 1);
    n->owner = owner_for(d, name, (n->flags & FN_DIR) != 0);
    if (n->flags & FN_DIR) {
        n->size = 0;
        if (!fat_valid_cluster(v, n->first)) n->flags |= FN_DAMAGED;
    } else if (n->first != 0 && !fat_valid_cluster(v, n->first)) {
        n->flags |= FN_DAMAGED;
        n->size = 0;
    } else if (n->first == 0 && n->size != 0) {
        n->flags |= FN_DAMAGED;
        n->size = 0;
    }
    *out = n;
    return 0;
}

int fatd_load(FatNode *d) {
    FatVol *v = d->vol;
    if (d->dir) { d->lru = ++v->lru_clock; return 0; }
    if (!(d->flags & FN_DIR)) return -ENOTDIR;
    if (d->flags & (FN_DAMAGED | FN_UNLINKED)) return d->flags & FN_UNLINKED ? -ENOENT : -EIO;

    FatDir *dd = kzalloc(sizeof *dd);
    if (!dd) return -ENOMEM;
    int rc = tables_alloc(dd, 16);
    u32 slots = 0;
    if (!rc) {
        if (d->flags & FN_FIXROOT) {
            slots = v->root_entries;
        } else {
            rc = chain_fill(d);
            if (!rc && (u64)d->known * v->cbytes / FAT_SLOT > FAT_MAX_DIR_SLOTS) rc = -EIO;
            slots = rc ? 0 : (u32)((u64)d->known * v->cbytes / FAT_SLOT);
        }
    }
    if (!rc) rc = bitmap_resize(dd, slots);
    u8 *buf = rc ? NULL : kmalloc(FAT_PAGE);
    if (!rc && !buf) rc = -ENOMEM;
    if (rc) { kfree(dd->htab); kfree(dd->stab); kfree(dd->used); kfree(dd); return rc; }

    LfnState lfn;
    lfn.active = false;
    dd->end_slot = slots;
    d->dir = dd;
    bool ended = false, garbage = false;
    u32 slot = 0;
    while (slot < slots && !rc) {
        u64 disk, avail;
        rc = chain_map(d, (u64)slot * FAT_SLOT, FAT_PAGE, &disk, &avail);
        if (rc) { rc = rc == -ENXIO ? -EIO : rc; break; }
        size_t nbytes = (size_t)MIN(MIN(avail, (u64)FAT_PAGE), (u64)(slots - slot) * FAT_SLOT);
        nbytes &= ~(size_t)(FAT_SLOT - 1);
        if (!nbytes) { rc = -EIO; break; }
        rc = fc_read(v, disk, buf, nbytes);
        if (rc) break;
        for (size_t off = 0; off < nbytes; off += FAT_SLOT, slot++) {
            const u8 *e = buf + off;
            if (ended) {
                if (e[0] != 0x00) garbage = true;
                continue;
            }
            if (e[0] == 0x00) { ended = true; dd->end_slot = slot; continue; }
            if (e[0] == 0xE5) { lfn.active = false; continue; }
            bit_set(dd, slot);
            u8 attr = e[11];
            if ((attr & 0x3F) == FAT_ATTR_LFN) {
                u8 seq = e[0];
                if (seq & 0x40) {
                    lfn.count = lfn.expect = seq & 0x3F;
                    lfn.csum = e[13];
                    lfn.first_slot = slot;
                    lfn.active = lfn.count >= 1 && lfn.count <= 20;
                } else if (!lfn.active || (seq & 0x3F) != lfn.expect || e[13] != lfn.csum) {
                    lfn.active = false;
                }
                if (lfn.active) {
                    u16 *u = lfn.units + (lfn.expect - 1) * 13;
                    for (int i = 0; i < 5; i++) u[i] = rd16(e + 1 + 2 * i);
                    for (int i = 0; i < 6; i++) u[5 + i] = rd16(e + 14 + 2 * i);
                    for (int i = 0; i < 2; i++) u[11 + i] = rd16(e + 28 + 2 * i);
                    lfn.expect--;
                }
                continue;
            }
            if (attr & FAT_ATTR_VOLUME) {                           /* the volume label (root directory) */
                if (d->flags & FN_ROOT && !v->label[0]) {
                    memcpy(v->label, e, 11);
                    v->label[11] = 0;
                    for (int i = 10; i >= 0 && v->label[i] == ' '; i--) v->label[i] = 0;
                }
                lfn.active = false;
                continue;
            }
            if (e[0] == '.' && (!memcmp(e, ".          ", 11) || !memcmp(e, "..         ", 11))) { lfn.active = false; continue; }
            FatNode *n;
            rc = load_entry(d, e, slot, &lfn, &n);
            lfn.active = false;
            if (rc) break;
            if (v->nnodes > FAT_NODE_LIMIT * 2) { fatd_node_free(n); rc = -ENOMEM; break; }
            rc = dir_insert(dd, n);
            if (rc) { fatd_node_free(n); break; }
        }
    }
    kfree(buf);
    if (rc) { fatd_drop(d); return rc; }
    dd->garbage = garbage;
    d->lru = ++v->lru_clock;
    return 0;
}

/* Frees the children and the listing structures; the caller guarantees nothing references the children. */
void fatd_drop(FatNode *d) {
    FatDir *dd = d->dir;
    if (!dd) return;
    d->dir = NULL;
    for (u32 i = 0; i < dd->count; i++) {
        FatNode *c = dd->order[i];
        c->parent = NULL;
        fatd_node_free(c);
    }
    kfree(dd->order);
    kfree(dd->htab);
    kfree(dd->stab);
    kfree(dd->used);
    kfree(dd);
}

void fatd_tree_free(FatNode *n) { fatd_drop(n); }

/* ---------------------------------------------------------------------------------------------- cache trimming */

/* A listing can go when none of its children is in use and none has a listing of its own. */
static bool droppable(const FatNode *d) {
    if (!d->dir) return false;
    for (u32 i = 0; i < d->dir->count; i++) {
        const FatNode *c = d->dir->order[i];
        if (c->vn || c->dir) return false;
    }
    return true;
}

static void find_victim(FatNode *d, FatNode **best) {
    if (!d->dir) return;
    if (droppable(d) && (!*best || d->lru < (*best)->lru)) *best = d;
    for (u32 i = 0; i < d->dir->count; i++) find_victim(d->dir->order[i], best);
}

/* Keeps the number of cached nodes bounded by dropping the least recently used idle listings. Called at the start of
 * an operation, before any node pointers are in use. */
void fatd_trim(FatVol *v) {
    for (int guard = 0; guard < 64 && v->nnodes > FAT_NODE_LIMIT; guard++) {
        FatNode *victim = NULL;
        find_victim(v->root, &victim);
        if (!victim) break;
        fatd_drop(victim);
    }
}

/* ---------------------------------------------------------------------------------------------- directory entries */

u32 fatd_dirsize(FatNode *d) {
    FatVol *v = d->vol;
    if (d->flags & FN_FIXROOT) return (u32)v->root_bytes;
    return d->known ? d->known * v->cbytes : v->cbytes;
}

typedef struct { FatDir *dd; const FatNode *skip, *skip2; } TakenCtx;

static bool sfn_taken(void *ctx, const u8 sfn[11]) {
    TakenCtx *t = ctx;
    for (FatNode *n = t->dd->stab[sfn_hash(sfn) & (t->dd->hsize - 1)]; n; n = n->snext)
        if (n != t->skip && n != t->skip2 && !memcmp(n->sfn, sfn, 11)) return true;
    return false;
}

/* Clears whatever follows the end-of-directory marker so that the zero-slots invariant holds again. */
static int clean_tail(FatNode *d) {
    FatDir *dd = d->dir;
    u64 off = (u64)dd->end_slot * FAT_SLOT, end = (u64)dd->slots * FAT_SLOT;
    while (off < end) {
        u64 disk, avail;
        int rc = chain_map(d, off, (u32)MIN(end - off, (u64)0x10000), &disk, &avail);
        if (rc) return rc == -ENXIO ? -EIO : rc;
        size_t take = (size_t)MIN(avail, end - off);
        rc = fc_zero(d->vol, disk, take, FPC_META);
        if (rc) return rc;
        off += take;
    }
    dd->garbage = false;
    return 0;
}

static int dir_grow(FatNode *d) {
    FatVol *v = d->vol;
    FatDir *dd = d->dir;
    if (d->flags & FN_FIXROOT) return -ENOSPC;
    u32 per = v->cbytes / FAT_SLOT;
    if (dd->slots + per > FAT_MAX_DIR_SLOTS) return -ENOSPC;
    int rc = chain_grow(d, 1);
    if (rc) return rc;
    rc = bitmap_resize(dd, dd->slots + per);
    return rc;
}

/* Finds (or makes) room for n consecutive slots. */
static int dir_reserve(FatNode *d, u32 n, u32 *slot_out) {
    FatDir *dd = d->dir;
    if (dd->garbage) {
        int rc = clean_tail(d);
        if (rc) return rc;
    }
    u32 run = 0, start = 0;
    for (u32 i = 0; i < dd->end_slot; i++) {
        if (!bit_get(dd, i)) {
            if (!run) start = i;
            if (++run == n) { *slot_out = start; return 0; }
        } else {
            run = 0;
        }
    }
    u32 tail = dd->end_slot - run;                      /* free slots just below the end join the untouched tail */
    while (tail + n > dd->slots) {
        int rc = dir_grow(d);
        if (rc) return rc;
    }
    *slot_out = tail;
    return 0;
}

typedef struct { u8 sfn[11], ntres; u32 slot; u8 nslots; } Placed;

static void build_short_entry(u8 *e, const u8 sfn[11], u8 attr, u8 ntres, u32 first, u32 size, u64 ct, u64 mt, u64 at, int bits) {
    u16 cd, ctm, md, mtm, ad, atm;
    u8 ctenth, dummy;
    fat_time_to_dos(ct, &cd, &ctm, &ctenth);
    fat_time_to_dos(mt, &md, &mtm, &dummy);
    fat_time_to_dos(at, &ad, &atm, &dummy);
    memset(e, 0, FAT_SLOT);
    memcpy(e, sfn, 11);
    e[11] = attr;
    e[12] = ntres;
    e[13] = ctenth;
    e[14] = (u8)ctm; e[15] = (u8)(ctm >> 8);
    e[16] = (u8)cd; e[17] = (u8)(cd >> 8);
    e[18] = (u8)ad; e[19] = (u8)(ad >> 8);
    if (bits == 32) { e[20] = (u8)(first >> 16); e[21] = (u8)(first >> 24); }
    e[22] = (u8)mtm; e[23] = (u8)(mtm >> 8);
    e[24] = (u8)md; e[25] = (u8)(md >> 8);
    e[26] = (u8)first; e[27] = (u8)(first >> 8);
    e[28] = (u8)size; e[29] = (u8)(size >> 8); e[30] = (u8)(size >> 16); e[31] = (u8)(size >> 24);
}

/* Validates the name, picks the short name, reserves slots and writes the entry set. */
static int dir_place(FatNode *d, const char *name, u8 attr, u32 first, u32 size, u64 ct, u64 mt, u64 at, const FatNode *skip,
                     const FatNode *skip2, Placed *out) {
    FatVol *v = d->vol;
    FatDir *dd = d->dir;
    u16 name16[FAT_MAX_NAME + 1];
    int n16 = fatn_utf8_to_utf16(name, strlen(name), name16, FAT_MAX_NAME);
    if (n16 < 0) return n16 == -ENAMETOOLONG ? -ENAMETOOLONG : -EINVAL;
    if (!fatn_valid16(name16, n16)) return -EINVAL;
    TakenCtx tc = { dd, skip, skip2 };
    bool need_lfn;
    int rc = fatn_make_sfn(name16, n16, out->sfn, &out->ntres, &need_lfn, sfn_taken, &tc);
    if (rc) return rc;
    u32 nslots = need_lfn ? 1 + (u32)(n16 + 12) / 13 : 1;
    u8 raw[21 * FAT_SLOT];
    if (need_lfn) fatn_pack_lfn(name16, n16, fatn_checksum(out->sfn), raw);
    build_short_entry(raw + (nslots - 1) * FAT_SLOT, out->sfn, attr, out->ntres, first, size, ct, mt, at, v->bits);
    u32 slot;
    rc = dir_reserve(d, nslots, &slot);
    if (rc) return rc;
    /* a set that starts below end_slot but reaches past it needs the (still zero) slots after it to stay zero */
    rc = dir_write(d, (u64)slot * FAT_SLOT, raw, (size_t)nslots * FAT_SLOT);
    if (rc) return rc;
    for (u32 i = 0; i < nslots; i++) bit_set(dd, slot + i);
    if (slot + nslots > dd->end_slot) dd->end_slot = slot + nslots;
    out->slot = slot;
    out->nslots = (u8)nslots;
    return 0;
}

/* Creates an entry in directory d (whose listing is loaded). first/size/attr describe what it points at. */
int fatd_create(FatNode *d, const char *name, u8 attr, u32 first, FatNode **out) {
    FatVol *v = d->vol;
    FatDir *dd = d->dir;
    if (!dd) return -EIO;
    if (fatd_find(d, name)) return -EEXIST;
    FatNode *n = fatd_node_new(v, d, name);
    if (!n) return -ENOMEM;
    u64 now = fat_now();
    Placed pl;
    int rc = dir_place(d, name, attr, first, 0, now, now, now, NULL, NULL, &pl);
    if (rc) { fatd_node_free(n); return rc; }
    memcpy(n->sfn, pl.sfn, 11);
    n->ntres = pl.ntres;
    n->attr = attr;
    n->first = first;
    n->ctime = n->mtime = n->atime = now;
    n->slot = pl.slot;
    n->nslots = pl.nslots;
    if (attr & FAT_ATTR_DIR) n->flags |= FN_DIR;
    n->owner = owner_for(d, name, (n->flags & FN_DIR) != 0);
    rc = dir_insert(dd, n);
    if (rc) {
        for (u32 i = 0; i < pl.nslots; i++) { u8 del = 0xE5; dir_write(d, (u64)(pl.slot + i) * FAT_SLOT, &del, 1); bit_clr(dd, pl.slot + i); }
        fatd_node_free(n);
        return rc;
    }
    *out = n;
    return 0;
}

/* Deletes the entry set of c from d: slots are marked 0xE5 and the node leaves the listing (it is not freed). */
void fatd_remove(FatNode *d, FatNode *c) {
    FatDir *dd = d->dir;
    u8 del = 0xE5;
    for (u32 i = 0; i < c->nslots; i++) {
        dir_write(d, (u64)(c->slot + i) * FAT_SLOT, &del, 1);
        bit_clr(dd, c->slot + i);
    }
    dir_unlist(dd, c);
    c->parent = NULL;
}

/* Writes the short entry of n back to its directory (size, first cluster, times, attributes). */
int fatd_commit(FatNode *n) {
    FatNode *p = n->parent;
    if (!p || (n->flags & (FN_ROOT | FN_UNLINKED))) return 0;
    u8 e[FAT_SLOT];
    build_short_entry(e, n->sfn, n->attr, n->ntres, n->first, (n->flags & FN_DIR) ? 0 : n->size, n->ctime, n->mtime, n->atime, n->vol->bits);
    return dir_write(p, (u64)(n->slot + n->nslots - 1) * FAT_SLOT, e, FAT_SLOT);
}

/* Moves node c into directory nd under a new name: new entry set first, then the old one is removed. */
int fatd_attach_renamed(FatNode *nd, FatNode *c, const char *newname, const FatNode *replaced) {
    FatNode *od = c->parent;
    char *nname = kmalloc(strlen(newname) + 1);
    if (!nname) return -ENOMEM;
    strcpy(nname, newname);
    Placed pl;
    int rc = dir_place(nd, newname, c->attr, c->first, (c->flags & FN_DIR) ? 0 : c->size, c->ctime, c->mtime, c->atime, c, replaced, &pl);
    if (rc) { kfree(nname); return rc; }
    fatd_remove(od, c);
    kfree(c->name);
    c->name = nname;
    c->name_len = (u16)strlen(nname);
    memcpy(c->sfn, pl.sfn, 11);
    c->ntres = pl.ntres;
    c->slot = pl.slot;
    c->nslots = pl.nslots;
    c->parent = nd;
    rc = dir_insert(nd->dir, c);
    if (rc) {                                           /* out of memory after the disk is updated: keep c reachable through its parent */
        c->parent = NULL;
        return rc;
    }
    return 0;
}

/* "." and ".." for a fresh directory cluster (already zeroed). */
int fatd_write_dots(FatVol *v, u32 cluster, u32 parent_cluster, u64 now) {
    u8 raw[2 * FAT_SLOT];
    build_short_entry(raw, (const u8 *)".          ", FAT_ATTR_DIR, 0, cluster, 0, now, now, now, v->bits);
    build_short_entry(raw + FAT_SLOT, (const u8 *)"..         ", FAT_ATTR_DIR, 0, parent_cluster, 0, now, now, now, v->bits);
    return fc_write(v, fat_cluster_off(v, cluster), raw, sizeof raw, FPC_META);
}

/* Points a directory's ".." entry at a new parent (after a move). parent_cluster is 0 for the root. */
int fatd_set_dotdot(FatNode *d, u32 parent_cluster) {
    FatVol *v = d->vol;
    if (!d->first) return -EIO;
    u8 e[FAT_SLOT];
    u64 off = fat_cluster_off(v, d->first) + FAT_SLOT;
    int rc = fc_read(v, off, e, sizeof e);
    if (rc) return rc;
    if (memcmp(e, "..         ", 11)) return -EIO;
    if (v->bits == 32) { e[20] = (u8)(parent_cluster >> 16); e[21] = (u8)(parent_cluster >> 24); }
    e[26] = (u8)parent_cluster; e[27] = (u8)(parent_cluster >> 8);
    return fc_write(v, off, e, sizeof e, FPC_META);
}

/* Reads the directory's contents without loading nodes: true if it has no entries besides "." and "..". */
bool fatd_is_empty(FatNode *d) {
    return d->dir && d->dir->count == 0;
}
