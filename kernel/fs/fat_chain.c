/*
 * FAT table access, the cluster allocator and the per-file extent cache.
 *
 * Every cluster number read from disk is range checked before use and every chain walk is bounded by the number of
 * clusters on the volume, so a corrupt or malicious volume yields -EIO rather than a loop or a stray write.
 */
#include "fatfs.h"

#define EOC32 0x0FFFFFFFu
#define EOC16 0xFFFFu

bool fat_valid_cluster(const FatVol *v, u32 c) { return c >= 2 && c < v->nclusters + 2; }

bool fat_is_eoc(const FatVol *v, u32 val) { return v->bits == 32 ? val >= 0x0FFFFFF8u : val >= 0xFFF8u; }

static bool is_bad(const FatVol *v, u32 val) { return val == (v->bits == 32 ? 0x0FFFFFF7u : 0xFFF7u); }

static u32 eoc_value(const FatVol *v) { return v->bits == 32 ? EOC32 : EOC16; }

u64 fat_cluster_off(const FatVol *v, u32 c) { return v->data_off + (u64)(c - 2) * v->cbytes; }

static u32 esize(const FatVol *v) { return v->bits == 32 ? 4 : 2; }

static u64 entry_off(const FatVol *v, u32 fat, u32 c) { return v->fat_off[fat] + (u64)c * esize(v); }

/* ---------------------------------------------------------------------------------------------- FAT entries */

int fat_get(FatVol *v, u32 c, u32 *val) {
    if (c >= v->nclusters + 2) return -EIO;
    u8 b[4] = { 0 };
    int rc = fc_read_ra(v, entry_off(v, v->active_fat, c), b, esize(v), 4);
    if (rc) return rc;
    *val = v->bits == 32 ? (((u32)b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24) & 0x0FFFFFFFu) : ((u32)b[0] | (u32)b[1] << 8);
    return 0;
}

/* Stores an entry in every FAT copy (the top four bits of a FAT32 entry are reserved and preserved). */
int fat_put(FatVol *v, u32 c, u32 val) {
    if (c < 2 || c >= v->nclusters + 2) return -EIO;
    u8 b[4];
    for (u32 i = 0; i < v->nfats; i++) {
        u64 off = entry_off(v, i, c);
        if (v->bits == 32) {
            int rc = fc_read_ra(v, off, b, 4, 4);
            if (rc) return rc;
            u32 old = (u32)b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24;
            u32 nv = (old & 0xF0000000u) | (val & 0x0FFFFFFFu);
            b[0] = (u8)nv; b[1] = (u8)(nv >> 8); b[2] = (u8)(nv >> 16); b[3] = (u8)(nv >> 24);
            rc = fc_write(v, off, b, 4, FPC_FAT);
            if (rc) return rc;
        } else {
            b[0] = (u8)val; b[1] = (u8)(val >> 8);
            int rc = fc_write(v, off, b, 2, FPC_FAT);
            if (rc) return rc;
        }
    }
    return 0;
}

/* Sets or clears the clean-shutdown bit in FAT[1] of every copy. */
int fat_set_clean_bits(FatVol *v, bool clean) {
    u8 b[4];
    for (u32 i = 0; i < v->nfats; i++) {
        u64 off = entry_off(v, i, 1);
        int rc = fc_read_ra(v, off, b, esize(v), 4);
        if (rc) return rc;
        if (v->bits == 32) {
            u32 x = (u32)b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24;
            x = clean ? (x | 0x08000000u) : (x & ~0x08000000u);
            b[0] = (u8)x; b[1] = (u8)(x >> 8); b[2] = (u8)(x >> 16); b[3] = (u8)(x >> 24);
        } else {
            u32 x = (u32)b[0] | (u32)b[1] << 8;
            x = clean ? (x | 0x8000u) : (x & ~0x8000u);
            b[0] = (u8)x; b[1] = (u8)(x >> 8);
        }
        rc = fc_write(v, off, b, esize(v), FPC_FAT);
        if (rc) return rc;
    }
    return 0;
}

/* Raw FAT[1] of the active copy (clean-shutdown and hard-error bits). */
int fat_read_state_bits(FatVol *v, u32 *fat1) {
    u8 b[4] = { 0 };
    int rc = fc_read_ra(v, entry_off(v, v->active_fat, 1), b, esize(v), 4);
    if (rc) return rc;
    *fat1 = v->bits == 32 ? ((u32)b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24) : ((u32)b[0] | (u32)b[1] << 8);
    return 0;
}

/* ---------------------------------------------------------------------------------------------- scanning */

typedef struct { FatVol *v; u32 c; const u8 *p; size_t left; } FatIter;

static void it_seek(FatIter *it, FatVol *v, u32 c) { it->v = v; it->c = c; it->p = NULL; it->left = 0; }

static int it_val(FatIter *it, u32 *val) {
    FatVol *v = it->v;
    if (!it->left) {
        size_t avail = 0;
        int err = 0;
        it->p = fc_peek(v, entry_off(v, v->active_fat, it->c), &avail, &err);
        if (!it->p) return err ? err : -EIO;
        it->left = avail / esize(v);
        if (!it->left) return -EIO;
    }
    *val = v->bits == 32 ? (((u32)it->p[0] | (u32)it->p[1] << 8 | (u32)it->p[2] << 16 | (u32)it->p[3] << 24) & 0x0FFFFFFFu)
                         : ((u32)it->p[0] | (u32)it->p[1] << 8);
    return 0;
}

static void it_next(FatIter *it) { it->c++; it->p += esize(it->v); it->left--; }

/* Number of free clusters on the volume, by scanning the FAT. */
int fat_count_free(FatVol *v, u32 *out) {
    FatIter it;
    it_seek(&it, v, 2);
    u32 free_n = 0;
    for (u32 c = 2; c < v->nclusters + 2; c++, it_next(&it)) {
        u32 val;
        int rc = it_val(&it, &val);
        if (rc) return rc;
        if (val == 0) free_n++;
    }
    *out = free_n;
    return 0;
}

/*
 * Finds free clusters for the next piece of an allocation: *start .. *start+*len-1, len <= want. Prefers continuing at
 * `prefer` (the cluster after the file's current end); otherwise scans from the allocation cursor for a run that is
 * long enough, settling for the best run seen once the scan budget is used up.
 */
static int find_run(FatVol *v, u32 prefer, u32 want, u32 *start, u32 *len) {
    FatIter it;
    u32 val;
    if (prefer && fat_valid_cluster(v, prefer)) {
        it_seek(&it, v, prefer);
        u32 n = 0;
        while (n < want && prefer + n < v->nclusters + 2) {
            int rc = it_val(&it, &val);
            if (rc) return rc;
            if (val != 0) break;
            n++;
            it_next(&it);
        }
        if (n) { *start = prefer; *len = n; return 0; }
    }
    u32 total = v->nclusters;
    u32 good = want < 64 ? want : 64;
    u32 c = fat_valid_cluster(v, v->alloc_cursor) ? v->alloc_cursor : 2;
    u32 best_start = 0, best_len = 0, run_start = 0, run_len = 0, budget = 1u << 20;
    it_seek(&it, v, c);
    for (u32 i = 0; i < total; i++) {
        int rc = it_val(&it, &val);
        if (rc) return rc;
        if (val == 0) {
            if (!run_len) run_start = c;
            if (++run_len >= want) { best_start = run_start; best_len = want; goto done; }
        } else if (run_len) {
            if (run_len > best_len) { best_start = run_start; best_len = run_len; }
            run_len = 0;
            if (best_len >= good) goto done;
        }
        c++;
        if (c >= v->nclusters + 2) {                    /* a run never spans the wrap-around */
            if (run_len && run_len > best_len) { best_start = run_start; best_len = run_len; }
            run_len = 0;
            c = 2;
            it_seek(&it, v, c);
        } else {
            it_next(&it);
        }
        if (--budget == 0 && (best_len || run_len)) break;
    }
    if (run_len > best_len) { best_start = run_start; best_len = run_len; }
done:
    if (!best_len) return -ENOSPC;
    *start = best_start;
    *len = best_len;
    return 0;
}

/*
 * Allocates `count` clusters as a chain (runs of consecutive clusters where possible) whose last entry is EOC.
 * zero_cls >= 0 zeroes the new clusters in the cache with that flush class; run_cb sees every run as (start, len).
 */
int fat_alloc_chain_cb(FatVol *v, u32 count, u32 prefer, int zero_cls, void (*run_cb)(void *, u32, u32), void *ctx,
                       u32 *first_out, u32 *last_out) {
    u32 first = 0, prev_last = 0, left = count;
    int rc = 0;
    while (left) {
        u32 start, len;
        rc = find_run(v, prefer, left, &start, &len);
        if (rc) break;
        for (u32 i = 0; i < len && !rc; i++)
            rc = fat_put(v, start + i, i + 1 < len ? start + i + 1 : eoc_value(v));
        if (!rc && prev_last) rc = fat_put(v, prev_last, start);
        if (!rc && zero_cls >= 0) rc = fc_zero(v, fat_cluster_off(v, start), (size_t)len * v->cbytes, zero_cls);
        if (rc) {
            for (u32 i = 0; i < len; i++) fat_put(v, start + i, 0);     /* best effort; earlier runs are freed below */
            if (prev_last) fat_put(v, prev_last, eoc_value(v));
            break;
        }
        if (!first) first = start;
        if (run_cb) run_cb(ctx, start, len);
        prev_last = start + len - 1;
        left -= len;
        prefer = prev_last + 1;
        v->alloc_cursor = prev_last + 1 < v->nclusters + 2 ? prev_last + 1 : 2;
        if (v->free_known && v->free_clusters >= len) v->free_clusters -= len;
    }
    if (rc) {
        if (first) fat_free_chain(v, first, NULL);
        return rc;
    }
    *first_out = first;
    *last_out = prev_last;
    return 0;
}

int fat_alloc_chain(FatVol *v, u32 count, u32 prefer, u32 *first, u32 *last) {
    return fat_alloc_chain_cb(v, count, prefer, -1, NULL, NULL, first, last);
}

/* Frees a whole chain starting at `first`. */
int fat_free_chain(FatVol *v, u32 first, u32 *freed) {
    u32 c = first, n = 0;
    int rc = 0;
    for (;;) {
        if (!fat_valid_cluster(v, c) || n > v->nclusters) { rc = -EIO; break; }
        u32 nxt;
        rc = fat_get(v, c, &nxt);
        if (rc) break;
        if (nxt == 0) { rc = -EIO; break; }                 /* already free: the chain was damaged */
        rc = fat_put(v, c, 0);
        if (rc) break;
        n++;
        if (fat_is_eoc(v, nxt)) break;
        if (is_bad(v, nxt)) { rc = -EIO; break; }
        c = nxt;
    }
    if (v->free_known) v->free_clusters += n;
    if (freed) *freed = n;
    return rc;
}

/* ---------------------------------------------------------------------------------------------- extent cache */

void chain_reset(FatNode *n) {
    kfree(n->ext);
    n->ext = NULL;
    n->next_ext = n->ext_cap = n->known = n->tail = 0;
    n->flags &= (u8) ~(FN_CHAIN_END | FN_CHAIN_OK);
}

static int ext_append(FatNode *n, u32 cluster, u32 count) {
    if (n->next_ext) {
        FatExtent *e = &n->ext[n->next_ext - 1];
        if (e->first + e->count == cluster) {
            e->count += count;
            n->known += count;
            n->tail = cluster + count - 1;
            return 0;
        }
    }
    if (n->next_ext == n->ext_cap) {
        u32 ncap = n->ext_cap ? n->ext_cap * 2 : 4;
        FatExtent *ne = krealloc(n->ext, ncap * sizeof *ne);
        if (!ne) return -ENOMEM;
        n->ext = ne;
        n->ext_cap = ncap;
    }
    n->ext[n->next_ext++] = (FatExtent){ cluster, count, n->known };
    n->known += count;
    n->tail = cluster + count - 1;
    return 0;
}

/* Forgets everything past the first `keep` clusters. */
static void ext_truncate(FatNode *n, u32 keep) {
    if (keep >= n->known) return;
    while (n->next_ext && n->ext[n->next_ext - 1].start >= keep) n->next_ext--;
    if (n->next_ext) {
        FatExtent *e = &n->ext[n->next_ext - 1];
        if (e->start + e->count > keep) e->count = keep - e->start;
    }
    n->known = keep;
    n->tail = n->next_ext ? n->ext[n->next_ext - 1].first + n->ext[n->next_ext - 1].count - 1 : 0;
    n->flags &= (u8) ~FN_CHAIN_END;
}

/* Extends the known prefix of the chain until it covers index idx (or the chain ends: -ENXIO). */
static int chain_walk(FatNode *n, u32 idx) {
    FatVol *v = n->vol;
    if (n->known == 0) {
        if (!n->first) return -ENXIO;
        if (!fat_valid_cluster(v, n->first)) return -EIO;
        int rc = ext_append(n, n->first, 1);
        if (rc) return rc;
    }
    while (n->known <= idx) {
        if (n->flags & FN_CHAIN_END) return -ENXIO;
        u32 nxt;
        int rc = fat_get(v, n->tail, &nxt);
        if (rc) return rc;
        if (fat_is_eoc(v, nxt)) { n->flags |= FN_CHAIN_END; return -ENXIO; }
        if (!fat_valid_cluster(v, nxt) || is_bad(v, nxt)) return -EIO;
        if (n->known >= v->nclusters) return -EIO;                  /* longer than the volume: a loop */
        rc = ext_append(n, nxt, 1);
        if (rc) return rc;
    }
    return 0;
}

/* Cluster number of chain index idx, and how many consecutive clusters follow in the same run (at least 1). */
int chain_lookup(FatNode *n, u32 idx, u32 ahead, u32 *cluster, u32 *run) {
    int rc = chain_walk(n, idx);
    if (rc) return rc;
    if (ahead > 1 && n->known <= idx + ahead - 1 && idx + ahead - 1 >= idx) chain_walk(n, idx + ahead - 1);     /* best effort */
    u32 lo = 0, hi = n->next_ext;                                   /* find the extent with start <= idx < start + count */
    while (lo + 1 < hi) {
        u32 mid = (lo + hi) / 2;
        if (n->ext[mid].start <= idx) lo = mid; else hi = mid;
    }
    FatExtent *e = &n->ext[lo];
    *cluster = e->first + (idx - e->start);
    *run = e->start + e->count - idx;
    return 0;
}

/* Maps a byte offset of the file (or directory) to a device offset plus the number of contiguous bytes from there. */
int chain_map(FatNode *n, u64 byte_off, u32 ahead_bytes, u64 *disk_off, u64 *avail) {
    FatVol *v = n->vol;
    if (n->flags & FN_FIXROOT) {
        if (byte_off >= v->root_bytes) return -ENXIO;
        *disk_off = v->root_off + byte_off;
        *avail = v->root_bytes - byte_off;
        return 0;
    }
    u64 idx = byte_off >> v->cshift;
    if (idx >= 0xFFFFFFFFu) return -EFBIG;
    u32 cluster, run;
    int rc = chain_lookup(n, (u32)idx, (ahead_bytes + v->cbytes - 1) >> v->cshift, &cluster, &run);
    if (rc) return rc;
    u64 in = byte_off & (v->cbytes - 1);
    *disk_off = fat_cluster_off(v, cluster) + in;
    *avail = (u64)run * v->cbytes - in;
    return 0;
}

u32 chain_clusters(const FatNode *n) {
    return n->known;                    /* exact once FN_CHAIN_OK is set */
}

/* Walks a directory's chain to its end (directories have no size to check it against). */
int chain_fill(FatNode *n) {
    if (n->flags & FN_CHAIN_OK) return 0;
    if (n->first == 0) return -EIO;
    int rc = chain_walk(n, 0xFFFFFFFFu);
    if (rc != -ENXIO) return rc ? rc : -EIO;
    n->flags |= FN_CHAIN_OK | FN_CHAIN_END;
    return 0;
}

/* Makes the chain safe to modify: walks it to the end, checks it against the size, trims surplus clusters and
 * zeroes the slack after the last byte so that growing the file can never expose stale data. */
int chain_prepare(FatNode *n) {
    FatVol *v = n->vol;
    if (n->flags & FN_CHAIN_OK) return 0;
    if (n->flags & FN_DAMAGED) return -EIO;
    u32 want = (u32)(((u64)n->size + v->cbytes - 1) >> v->cshift);
    if (n->first == 0) {
        if (n->size) { n->flags |= FN_DAMAGED; return -EIO; }
        n->flags |= FN_CHAIN_OK | FN_CHAIN_END;
        return 0;
    }
    int rc = chain_walk(n, 0xFFFFFFFFu);
    if (rc != -ENXIO) return rc ? rc : -EIO;
    if (n->known < want) { n->flags |= FN_DAMAGED; return -EIO; }
    if (n->known > want) {
        if (want == 0) {
            u32 f = n->first;
            n->first = 0;
            chain_reset(n);
            fatd_commit(n);                                     /* the entry must stop pointing at the freed chain */
            fat_free_chain(v, f, NULL);
        } else {
            u32 c, run;
            rc = chain_lookup(n, want - 1, 1, &c, &run);
            if (rc) return rc;
            u32 nxt;
            rc = fat_get(v, c, &nxt);
            if (rc) return rc;
            rc = fat_put(v, c, eoc_value(v));
            if (rc) return rc;
            if (!fat_is_eoc(v, nxt) && fat_valid_cluster(v, nxt)) fat_free_chain(v, nxt, NULL);
            ext_truncate(n, want);
        }
    }
    n->flags |= FN_CHAIN_OK | FN_CHAIN_END;
    if (n->first && (n->size & (v->cbytes - 1))) {
        u64 d, avail;
        rc = chain_map(n, n->size, 1, &d, &avail);
        if (rc) return rc;
        u64 tail_bytes = v->cbytes - (n->size & (v->cbytes - 1));
        rc = fc_zero(v, d, (size_t)tail_bytes, FPC_DATA);
        if (rc) return rc;
    }
    return 0;
}

struct grow_ctx { FatNode *n; int rc; };

static void grow_cb(void *ctx, u32 start, u32 len) {
    struct grow_ctx *g = ctx;
    if (!g->rc) g->rc = ext_append(g->n, start, len);
}

/* Appends `count` zeroed clusters to a chain that has been prepared. */
int chain_grow(FatNode *n, u32 count) {
    FatVol *v = n->vol;
    if (!count) return 0;
    if (n->first && n->known == 0) {                            /* make sure the tail is known */
        int rc = chain_walk(n, 0xFFFFFFFFu);
        if (rc != -ENXIO) return rc ? rc : -EIO;
    }
    if (n->known + (u64)count > v->nclusters) return -ENOSPC;
    u32 saved = n->known, saved_tail = n->tail;
    u32 first = 0, last = 0;
    struct grow_ctx g = { n, 0 };
    int cls = (n->flags & FN_DIR) ? FPC_META : FPC_DATA;
    int rc = fat_alloc_chain_cb(v, count, n->known ? n->tail + 1 : 0, cls, grow_cb, &g, &first, &last);
    if (!rc && g.rc) {                                          /* out of memory for the extent cache */
        fat_free_chain(v, first, NULL);
        rc = g.rc;
    } else if (!rc) {
        if (saved) {
            rc = fat_put(v, saved_tail, first);                 /* link the old tail to the new clusters */
            if (rc) fat_free_chain(v, first, NULL);
        } else {
            n->first = first;
        }
    }
    if (rc) {
        ext_truncate(n, saved);
        n->flags |= FN_CHAIN_END;
        return rc;
    }
    n->tail = last;
    n->flags |= FN_CHAIN_END;
    return 0;
}

/* Keeps the first `keep` clusters; frees the rest. keep == 0 frees everything and clears n->first. */
int chain_shrink(FatNode *n, u32 keep) {
    FatVol *v = n->vol;
    if (keep >= n->known) return 0;
    int rc = 0;
    if (keep == 0) {
        u32 f = n->first;
        n->first = 0;
        chain_reset(n);
        n->flags |= FN_CHAIN_OK | FN_CHAIN_END;
        return f ? fat_free_chain(v, f, NULL) : 0;
    }
    u32 c, run, nxt;
    rc = chain_lookup(n, keep - 1, 1, &c, &run);
    if (rc) return rc;
    rc = fat_get(v, c, &nxt);
    if (rc) return rc;
    rc = fat_put(v, c, eoc_value(v));
    if (rc) return rc;
    ext_truncate(n, keep);
    n->flags |= FN_CHAIN_END;
    if (!fat_is_eoc(v, nxt) && fat_valid_cluster(v, nxt)) return fat_free_chain(v, nxt, NULL);
    return 0;
}
