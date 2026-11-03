/*
 * NTFS, read only: boot sector, MFT records and their fixups, attributes (also across $ATTRIBUTE_LIST and extension records), run lists, resident and
 * non-resident data, LZNT1 compression units, sparse runs, directory indexes (B+trees in $INDEX_ROOT / $INDEX_ALLOCATION), reparse points, free space.
 * See ntfs_priv.h for the two rules (the volume is not trusted; nothing writes) and docs/NTFS.md for the on-disk structures as implemented.
 *
 * How the pieces fit:
 *   - a *record* is one MFT entry, read through the map of $MFT's own data (v->mft). Records are validated (signature, update sequence array, sizes) when they
 *     are read; a small cache keeps the recent ones. A file's *base record* is copied into its NtfsFile; extension records are only looked at, through the cache;
 *   - an *attribute* is found by (type, name) in the base record or, when the file has an $ATTRIBUTE_LIST, wherever the list says (file_extents()). A stream
 *     (NtfsStream) is the summary of one data-like attribute: the first extent's header (sizes, flags) and, when the content is non-resident and someone
 *     reads it, the whole run list gathered from all extents;
 *   - reads walk the run list (sparse runs read as zeros; bytes past the initialised size read as zeros), or decompress whole compression units;
 *   - directories are walked in order with an explicit stack (NtfsDirCursor) and searched by descending the tree; both are bounded by NT_MAX_DEPTH and by a
 *     count of the blocks the index can contain, so a loop in the tree ends in -EIO.
 */
#include "ntfs_priv.h"

#define NT_MIN(a, b) ((a) < (b) ? (a) : (b))

/* ===================================================================================================== run lists */

static void runs_free(NtfsRuns *rl) {
    if (rl->r) NT_FREE(rl->r);
    rl->r = NULL;
    rl->n = rl->cap = 0;
    rl->end = 0;
}

static int runs_push(NtfsRuns *rl, u64 vcn, u64 len, s64 lcn) {
    if (rl->n) {                                                   /* neighbouring runs that continue each other are one run */
        NtfsRun *p = &rl->r[rl->n - 1];
        if (p->vcn + p->len == vcn && ((p->lcn < 0 && lcn < 0) || (p->lcn >= 0 && lcn >= 0 && (u64)p->lcn + p->len == (u64)lcn))) {
            p->len += len;
            rl->end = vcn + len;
            return 0;
        }
    }
    if (rl->n == rl->cap) {
        u32 nc = rl->cap ? rl->cap * 2 : 16;
        if (nc > NT_MAX_RUNS) nc = NT_MAX_RUNS;
        if (rl->n >= nc) return -EIO;                              /* a map this fragmented is not believed */
        NtfsRun *nr = NT_ALLOC((size_t)nc * sizeof *nr);
        if (!nr) return -ENOMEM;
        if (rl->n) memcpy(nr, rl->r, (size_t)rl->n * sizeof *nr);
        if (rl->r) NT_FREE(rl->r);
        rl->r = nr;
        rl->cap = nc;
    }
    rl->r[rl->n].vcn = vcn;
    rl->r[rl->n].len = len;
    rl->r[rl->n].lcn = lcn;
    rl->n++;
    rl->end = vcn + len;
    return 0;
}

/* Appends the runs of one extent's mapping pairs. A run is a header byte (low nibble: bytes of the length, high nibble: bytes of the LCN difference), the
 * length, and the LCN difference as a signed number relative to the previous run's LCN (for the first run: to 0); no LCN bytes means a sparse run. A zero
 * header byte ends the list. `start_vcn` must be where the map built so far ends. */
static int runs_decode(const NtfsVol *v, NtfsRuns *rl, const u8 *mp, u32 mplen, u64 start_vcn) {
    u64 max_vcn = (1ull << 62) >> v->cshift;
    if (start_vcn != rl->end) return -EIO;                         /* a gap or an overlap between extents */
    u64 vcn = start_vcn;
    s64 lcn = 0;
    u32 i = 0;
    while (i < mplen) {
        u8 h = mp[i++];
        if (h == 0) break;
        u32 ls = h & 15, os = h >> 4;
        if (ls == 0 || ls > 8 || os > 8 || (u64)i + ls + os > mplen) return -EIO;
        u64 len = 0;
        for (u32 k = 0; k < ls; k++) len |= (u64)mp[i + k] << (8 * k);
        i += ls;
        if (len == 0 || len > max_vcn || vcn > max_vcn - len) return -EIO;
        if (os) {
            s64 delta = 0;
            for (u32 k = 0; k < os; k++) delta |= (s64)((u64)mp[i + k] << (8 * k));
            if (os < 8 && (mp[i + os - 1] & 0x80)) delta |= (s64)(~0ull << (8 * os));        /* sign extension */
            i += os;
            if (__builtin_add_overflow(lcn, delta, &lcn)) return -EIO;
            if (lcn < 0 || (u64)lcn >= v->nclusters || len > v->nclusters - (u64)lcn) return -EIO;      /* a real run must lie inside the volume */
            int rc = runs_push(rl, vcn, len, lcn);
            if (rc) return rc;
        } else {
            int rc = runs_push(rl, vcn, len, -1);
            if (rc) return rc;
        }
        vcn += len;
    }
    return 0;
}

/* The run holding `vcn`, or NULL. *hint remembers the last hit so that sequential reads cost no search. */
static const NtfsRun *runs_find(const NtfsRuns *rl, u64 vcn, u32 *hint) {
    if (!rl->n || vcn >= rl->end) return NULL;
    u32 h = hint ? *hint : 0;
    if (h < rl->n) {
        const NtfsRun *r = &rl->r[h];
        if (vcn >= r->vcn && vcn - r->vcn < r->len) return r;
        if (h + 1 < rl->n) {
            r = &rl->r[h + 1];
            if (vcn >= r->vcn && vcn - r->vcn < r->len) { if (hint) *hint = h + 1; return r; }
        }
    }
    u32 lo = 0, hi = rl->n;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        const NtfsRun *r = &rl->r[mid];
        if (vcn < r->vcn) hi = mid;
        else if (vcn - r->vcn >= r->len) lo = mid + 1;
        else { if (hint) *hint = mid; return r; }
    }
    return NULL;
}

/* ===================================================================================================== device access */

static int vol_read(NtfsVol *v, u64 off, void *buf, size_t len) {
    if (off > v->dev.size || len > v->dev.size - off) return -EIO;
    v->n_reads++;
    v->n_read_bytes += len;
    return v->dev.read(v->dev.ctx, off, buf, len);
}

/* Bytes [off, off+len) of whatever the run list maps (clusters on the volume, zeros for sparse runs). -EIO when the map has a hole. */
static int runs_read(NtfsVol *v, const NtfsRuns *rl, u32 *hint, u64 off, void *buf, size_t len) {
    u8 *out = buf;
    while (len) {
        const NtfsRun *r = runs_find(rl, off >> v->cshift, hint);
        if (!r) return -EIO;
        u64 in = off - (r->vcn << v->cshift);
        u64 run_bytes = r->len > ((1ull << 62) >> v->cshift) ? (1ull << 62) : r->len << v->cshift;
        size_t chunk = (size_t)NT_MIN((u64)len, run_bytes - in);
        if (chunk > (1u << 20)) chunk = 1u << 20;
        if (r->lcn < 0) {
            memset(out, 0, chunk);
        } else {
            int rc = vol_read(v, ((u64)r->lcn << v->cshift) + in, out, chunk);
            if (rc) return rc;
        }
        out += chunk;
        off += chunk;
        len -= chunk;
    }
    return 0;
}

/* ===================================================================================================== records */

/* Update sequence array (fixups): the last two bytes of every 512-byte stride of a record or index block were replaced by the sequence number kept at
 * usa_off; the originals follow it in the array. A stride whose last two bytes differ from the sequence number was only partly written: damaged. */
static int fixups_apply(u8 *buf, u32 len, u32 usa_off, u32 usa_cnt) {
    if (usa_cnt < 2 || (u64)(usa_cnt - 1) * 512 != len) return -EIO;
    if ((usa_off & 1) || usa_off < 8 || (u64)usa_off + 2ull * usa_cnt > len) return -EIO;
    u32 usn = nt_le16(buf + usa_off);
    for (u32 k = 1; k < usa_cnt; k++) {
        u32 end = k * 512;
        if (nt_le16(buf + end - 2) != usn) return -EIO;
        buf[end - 2] = buf[usa_off + 2 * k];
        buf[end - 1] = buf[usa_off + 2 * k + 1];
    }
    return 0;
}

static int rec_check(NtfsVol *v, u8 *buf, u64 no, const char **why) {
    *why = NULL;
    if (memcmp(buf, "FILE", 4)) { *why = memcmp(buf, "BAAD", 4) ? "no FILE signature" : "record marked BAAD"; return -EIO; }
    u32 usa_off = nt_le16(buf + 4), usa_cnt = nt_le16(buf + 6);
    if (fixups_apply(buf, v->rec_size, usa_off, usa_cnt)) { *why = "damaged update sequence array"; return -EIO; }
    u32 first = nt_le16(buf + 20), used = nt_le32(buf + 24);
    if (used > v->rec_size || first < usa_off + 2 * usa_cnt || (first & 7) || first + 8 > used) { *why = "bad record header"; return -EIO; }
    if ((nt_le16(buf + 22) & NT_REC_INUSE) && usa_off >= 0x30 && nt_le32(buf + 0x2C) != (u32)no && !v->warned_recno) {
        v->warned_recno = true;                                     /* worth a line, not worth refusing: the content is bounds-checked like any other */
        NT_LOG("MFT record %lu says it is record %u (this is reported once)", (unsigned long)no, (unsigned)nt_le32(buf + 0x2C));
    }
    return 0;
}

/* Reads and validates record `no` into buf (rec_size bytes). */
static int rec_load(NtfsVol *v, u64 no, u8 *buf) {
    if (no >= v->nrecords) return -EIO;
    u64 off = no * v->rec_size;
    u32 hint = 0;
    int rc = runs_read(v, &v->mft, &hint, off, buf, v->rec_size);
    if (rc) return rc;
    const char *why;
    rc = rec_check(v, buf, no, &why);
    if (rc) NT_LOG("MFT record %lu: %s", (unsigned long)no, why);
    return rc;
}

/* A cached, validated record. The pointer stays valid until the next rec_get(). */
static int rec_get(NtfsVol *v, u64 no, const u8 **out) {
    int victim = -1;
    for (int i = 0; i < NT_RECCACHE; i++) {
        if (v->recs[i].buf && v->recs[i].no == no) {
            v->recs[i].use = ++v->clock;
            v->n_rec_hits++;
            *out = v->recs[i].buf;
            return 0;
        }
        if (victim < 0 || !v->recs[i].buf || (v->recs[victim].buf && v->recs[i].use < v->recs[victim].use)) victim = i;
    }
    v->n_rec_misses++;
    if (!v->recs[victim].buf) {
        v->recs[victim].buf = NT_ALLOC(v->rec_size);
        if (!v->recs[victim].buf) return -ENOMEM;
    }
    v->recs[victim].no = ~0ull;                                    /* not valid until it has been read */
    int rc = rec_load(v, no, v->recs[victim].buf);
    if (rc) return rc;
    v->recs[victim].no = no;
    v->recs[victim].use = ++v->clock;
    *out = v->recs[victim].buf;
    return 0;
}

/* ===================================================================================================== attributes */

typedef struct NtfsAttr {
    const u8 *p;                                /* the attribute header inside the record */
    u32 len, type;
    u16 flags, inst;
    u8 nonres, namelen;
    const u8 *name;                             /* UTF-16LE bytes */
    const u8 *val;                              /* resident */
    u32 vlen;
    u64 lvcn, alloc, size, init;                /* non-resident */
    const u8 *mp;
    u32 mplen;
    u8 cunit;
} NtfsAttr;

/* Parses the attribute at `off` of a record whose used part is `used`. 1 = parsed, 0 = the end marker, <0 = damaged. */
static int attr_parse(const u8 *rec, u32 used, u32 off, NtfsAttr *a) {
    if (off > used || used - off < 8) return -EIO;
    a->type = nt_le32(rec + off);
    if (a->type == NT_A_END) return 0;
    if (used - off < 16) return -EIO;
    a->p = rec + off;
    a->len = nt_le32(rec + off + 4);
    if (a->len < 16 || (a->len & 7) || a->len > used - off) return -EIO;
    a->nonres = rec[off + 8];
    a->namelen = rec[off + 9];
    u32 nameoff = nt_le16(rec + off + 10);
    a->flags = nt_le16(rec + off + 12);
    a->inst = nt_le16(rec + off + 14);
    if (a->nonres > 1) return -EIO;
    a->name = rec + off + nameoff;
    if (a->namelen && (nameoff < 16 || nameoff + 2u * a->namelen > a->len)) return -EIO;
    if (!a->nonres) {
        if (a->len < 24) return -EIO;
        a->vlen = nt_le32(rec + off + 16);
        u32 voff = nt_le16(rec + off + 20);
        if (voff < 16 || voff > a->len || a->vlen > a->len - voff) return -EIO;
        a->val = rec + off + voff;
    } else {
        if (a->len < 0x40) return -EIO;
        a->lvcn = nt_le64(rec + off + 16);
        u32 mpoff = nt_le16(rec + off + 32);
        a->cunit = rec[off + 34];
        a->alloc = nt_le64(rec + off + 40);
        a->size = nt_le64(rec + off + 48);
        a->init = nt_le64(rec + off + 56);
        if (mpoff < 0x40 || mpoff > a->len || a->cunit > 16) return -EIO;
        a->mp = rec + off + mpoff;
        a->mplen = a->len - mpoff;
    }
    return 1;
}

static bool attr_is(const NtfsAttr *a, u32 type, const u8 *name, u32 nlen) {
    return a->type == type && a->namelen == nlen && (!nlen || !memcmp(a->name, name, 2u * nlen));
}

static int file_extents_scan(const u8 *rec, u32 type, const u8 *name, u32 nlen, int (*cb)(void *, const NtfsAttr *), void *ctx) {
    u32 used = nt_le32(rec + 24), off = nt_le16(rec + 20);
    NtfsAttr a;
    for (;;) {
        int rc = attr_parse(rec, used, off, &a);
        if (rc < 0) return rc;
        if (rc == 0) return 0;
        if (attr_is(&a, type, name, nlen)) {
            rc = cb(ctx, &a);
            if (rc) return rc;
        }
        off += a.len;
    }
}

/* Calls cb for every extent of attribute (type, name) of the file in order of the list (which is the order of their VCNs): the attributes of the base
 * record when the file has no $ATTRIBUTE_LIST, else the ones the list names, wherever they are. cb returns 0 to go on, >0 to stop (not an error), <0 to fail.
 * The attribute passed to cb points into a record that stays valid only until cb returns. */
static int file_extents(NtfsFile *f, u32 type, const u8 *name, u32 nlen, int (*cb)(void *, const NtfsAttr *), void *ctx) {
    NtfsVol *v = f->vol;
    int rc;
    if (!f->alist) {
        rc = file_extents_scan(f->rec, type, name, nlen, cb, ctx);
        return rc > 0 ? 0 : rc;
    }
    u32 pos = 0, count = 0;
    while (f->alist_len - pos >= 26) {
        const u8 *e = f->alist + pos;
        u32 elen = nt_le16(e + 4), nl = e[6], noff = e[7];
        if (elen < 26 || elen > f->alist_len - pos || noff + 2u * nl > elen) return -EIO;
        pos += elen;
        if (nt_le32(e) != type || nl != nlen || (nlen && memcmp(e + noff, name, 2u * nlen))) continue;
        if (++count > NT_MAX_EXTENTS) return -EIO;
        u64 eno = nt_le64(e + 16) & NT_REF_MASK, evcn = nt_le64(e + 8);
        u32 inst = nt_le16(e + 24);
        const u8 *rec = f->rec;
        if (eno != f->no) {
            rc = rec_get(v, eno, &rec);
            if (rc) return rc;
            if (!(nt_le16(rec + 22) & NT_REC_INUSE) || (nt_le64(rec + 32) & NT_REF_MASK) != f->no) return -EIO;     /* not an extension record of this file */
        }
        u32 used = nt_le32(rec + 24), off = nt_le16(rec + 20);
        NtfsAttr a, found;
        bool have = false;
        for (;;) {
            rc = attr_parse(rec, used, off, &a);
            if (rc < 0) return rc;
            if (rc == 0) break;
            if (attr_is(&a, type, name, nlen)) {
                if (a.inst == inst) { found = a; have = true; break; }
                if (!have && (!a.nonres || a.lvcn == evcn)) { found = a; have = true; }       /* tolerated: instance numbers that disagree */
            }
            off += a.len;
        }
        if (!have) return -EIO;                                    /* the list names an attribute that is not there */
        rc = cb(ctx, &found);
        if (rc) return rc > 0 ? 0 : rc;
    }
    return 0;
}

/* ---- streams ---- */

static void stream_free(NtfsStream *s) {
    if (s->rbuf) NT_FREE(s->rbuf);
    runs_free(&s->runs);
    memset(s, 0, sizeof *s);
}

typedef struct { NtfsVol *v; NtfsStream *s; } OpenCtx;

static int cb_open(void *ctx, const NtfsAttr *a) {
    OpenCtx *c = ctx;
    NtfsStream *s = c->s;
    if (a->nonres && a->lvcn != 0) return 0;                       /* a later extent: the first one carries the sizes */
    s->present = true;
    s->flags = a->flags;
    if (!a->nonres) {
        s->resident = true;
        s->rbuf = NT_ALLOC((size_t)a->vlen + 1);
        if (!s->rbuf) return -ENOMEM;
        memcpy(s->rbuf, a->val, a->vlen);
        s->rlen = a->vlen;
        s->size = s->init = s->alloc = a->vlen;
        return 1;
    }
    if (a->size > (1ull << 62) || a->alloc > (1ull << 62)) return -EIO;
    s->size = a->size;
    s->init = NT_MIN(a->init, a->size);
    s->alloc = a->alloc;
    s->cunit = a->cunit;
    return 1;
}

/* Summarises attribute (type, name): resident value copied, or the sizes of a non-resident one. Not found: s->present stays false (and 0 is returned). */
static int stream_open(NtfsFile *f, u32 type, const u8 *name, u32 nlen, NtfsStream *s) {
    OpenCtx c = { f->vol, s };
    memset(s, 0, sizeof *s);
    int rc = file_extents(f, type, name, nlen, cb_open, &c);
    if (rc) stream_free(s);
    return rc;
}

typedef struct { NtfsVol *v; NtfsRuns *rl; bool first; } RunsCtx;

static int cb_runs(void *ctx, const NtfsAttr *a) {
    RunsCtx *c = ctx;
    if (!a->nonres) return -EIO;
    if (c->first && a->lvcn != 0) return -EIO;                     /* the first extent must start at VCN 0 */
    c->first = false;
    return runs_decode(c->v, c->rl, a->mp, a->mplen, a->lvcn);
}

/* Loads the whole map of a non-resident stream (all extents). */
static int stream_runs(NtfsFile *f, NtfsStream *s, u32 type, const u8 *name, u32 nlen) {
    if (s->runs_loaded || s->resident) return 0;
    RunsCtx c = { f->vol, &s->runs, true };
    int rc = file_extents(f, type, name, nlen, cb_runs, &c);
    if (rc) { runs_free(&s->runs); return rc; }
    s->runs_loaded = true;
    return 0;
}

/* ===================================================================================================== reading file data */

static int comp_unit(NtfsFile *f, NtfsStream *s, u64 unit, u8 *dst) {
    NtfsVol *v = f->vol;
    u32 ucl = 1u << s->cunit;
    u32 ubytes = v->cluster << s->cunit;
    u64 vcn0 = unit << s->cunit;
    /* Which clusters of the unit are real? Walk the runs that overlap it. */
    u32 real = 0, pos = 0;
    bool gap = false;
    struct { s64 lcn; u32 len; } part[16];
    u32 np = 0, hint = 0;
    while (pos < ucl) {
        const NtfsRun *r = runs_find(&s->runs, vcn0 + pos, &hint);
        u32 n, in;
        s64 lcn;
        if (!r) {                                                  /* beyond the map: the rest of the unit is a hole */
            n = ucl - pos;
            lcn = -1;
        } else {
            in = (u32)NT_MIN(vcn0 + pos - r->vcn, (u64)0xFFFFFFFFu);
            n = (u32)NT_MIN(r->len - in, (u64)(ucl - pos));
            lcn = r->lcn < 0 ? -1 : r->lcn + (s64)in;
        }
        if (lcn >= 0) {
            if (gap) return -EIO;                                  /* real clusters after a hole inside a unit: not a layout NTFS writes */
            if (np >= 16) return -EIO;
            part[np].lcn = lcn;
            part[np].len = n;
            np++;
            real += n;
        } else {
            gap = true;
        }
        pos += n;
    }
    if (real == 0) { memset(dst, 0, ubytes); return 0; }
    if (real == ucl) {                                             /* stored without compression */
        u32 at = 0;
        for (u32 i = 0; i < np; i++) {
            int rc = vol_read(v, (u64)part[i].lcn << v->cshift, dst + (size_t)at * v->cluster, (size_t)part[i].len * v->cluster);
            if (rc) return rc;
            at += part[i].len;
        }
        return 0;
    }
    /* compressed: `real` clusters of LZNT1 data followed by a hole */
    size_t cbytes = (size_t)real * v->cluster;
    u8 *tmp = NT_ALLOC(cbytes);
    if (!tmp) return -ENOMEM;
    u32 at = 0;
    int rc = 0;
    for (u32 i = 0; i < np && !rc; i++) {
        rc = vol_read(v, (u64)part[i].lcn << v->cshift, tmp + (size_t)at * v->cluster, (size_t)part[i].len * v->cluster);
        at += part[i].len;
    }
    size_t dlen = 0;
    if (!rc) rc = ntfs_lznt1(tmp, cbytes, dst, ubytes, &dlen);
    NT_FREE(tmp);
    if (rc) { NT_LOG("file %lu: damaged compressed data in unit %lu", (unsigned long)f->no, (unsigned long)unit); return -EIO; }
    memset(dst + dlen, 0, ubytes - dlen);
    return 0;
}

/* Makes sure the per-file window can hold `need` bytes. Its contents are lost when it has to grow. */
static int ra_ensure(NtfsFile *f, u32 need) {
    if (need < NT_RA_BYTES) need = NT_RA_BYTES;
    if (f->ra && f->ra_cap >= need) return 0;
    u8 *nb = NT_ALLOC(need);
    if (!nb) return -ENOMEM;
    if (f->ra) NT_FREE(f->ra);
    f->ra = nb;
    f->ra_cap = need;
    f->ra_len = 0;
    f->ra_unit = false;
    return 0;
}

/* Reads `n` bytes at `off` of a stream that are known to lie below its initialised size. */
static int stream_read_valid(NtfsFile *f, NtfsStream *s, u32 type, const u8 *name, u32 nlen, u64 off, u8 *out, size_t n) {
    NtfsVol *v = f->vol;
    int rc = stream_runs(f, s, type, name, nlen);
    if (rc) return rc;
    if (!(s->flags & NT_AF_COMPRESSED)) {
        u32 hint = 0;
        return runs_read(v, &s->runs, &hint, off, out, n);
    }
    if (s->cunit == 0 || ((u64)v->cluster << s->cunit) > NT_MAX_UNIT) return -ENOTSUP;     /* a unit this size is not something NTFS writes */
    u32 ubytes = v->cluster << s->cunit;
    rc = ra_ensure(f, ubytes);                                     /* the window doubles as the cache of the last decompressed unit */
    if (rc) return rc;
    u32 ushift = v->cshift + s->cunit;
    while (n) {
        u64 unit = off >> ushift;
        u32 in = (u32)(off & (ubytes - 1));
        size_t take = NT_MIN(n, (size_t)(ubytes - in));
        if (!f->ra_unit || f->ra_off != unit) {
            f->ra_unit = false;
            rc = stream_runs(f, s, type, name, nlen);
            if (!rc) rc = comp_unit(f, s, unit, f->ra);
            if (rc) return rc;
            f->ra_unit = true;
            f->ra_off = unit;
            f->ra_len = ubytes;
        }
        memcpy(out, f->ra + in, take);
        out += take;
        off += take;
        n -= take;
    }
    return 0;
}

/* Reads from a stream with the rules of the format: the end of the data is `size`, bytes from `init` on read as zeros. */
static ssize_t stream_read(NtfsFile *f, NtfsStream *s, u32 type, const u8 *name, u32 nlen, u64 off, void *buf, size_t n) {
    if (!s->present || off >= s->size) return 0;
    if (n > s->size - off) n = (size_t)(s->size - off);
    if (s->resident) {
        if (off >= s->rlen) { memset(buf, 0, n); return (ssize_t)n; }
        size_t have = NT_MIN(n, (size_t)(s->rlen - off));
        memcpy(buf, s->rbuf + off, have);
        memset((u8 *)buf + have, 0, n - have);
        return (ssize_t)n;
    }
    size_t valid = off < s->init ? (size_t)NT_MIN((u64)n, s->init - off) : 0;
    if (valid) {
        int rc = stream_read_valid(f, s, type, name, nlen, off, buf, valid);
        if (rc) return rc;
    }
    memset((u8 *)buf + valid, 0, n - valid);
    return (ssize_t)n;
}

static const u8 NAME_I30[8] = { '$', 0, 'I', 0, '3', 0, '0', 0 };

bool ntfs_data_elsewhere(u32 tag) {
    switch (tag) {
    case 0x80000017: case 0x80000018: case 0x80000007: case 0x80000013: case 0xC0000004: case 0xC0000007: return true;   /* WOF, WCI, SIS, dedup, HSM */
    default: return (tag & 0xFFFF0FFFu) == 0x9000001Au;                                                                   /* cloud files placeholders */
    }
}

ssize_t ntfs_read(NtfsFile *f, u64 off, void *buf, size_t n) {
    if (f->is_dir) return -EISDIR;
    if (f->reparse_tag && ntfs_data_elsewhere(f->reparse_tag)) return -ENOTSUP;
    NtfsStream *s = &f->data;
    if (!s->present) return 0;
    if (s->flags & NT_AF_ENCRYPTED) return -EACCES;
    if (off >= s->size || !n) return 0;
    if (n > s->size - off) n = (size_t)(s->size - off);
    bool small = !s->resident && !(s->flags & NT_AF_COMPRESSED) && n < NT_RA_BYTES / 2;
    if (!small) return stream_read(f, s, NT_A_DATA, NULL, 0, off, buf, n);
    /* a small read of a plain file: serve it from the read-ahead window, filling that with one big read */
    if (f->ra && !f->ra_unit && off >= f->ra_off && off + n <= f->ra_off + f->ra_len) {
        memcpy(buf, f->ra + (off - f->ra_off), n);
        return (ssize_t)n;
    }
    if (ra_ensure(f, NT_RA_BYTES)) return stream_read(f, s, NT_A_DATA, NULL, 0, off, buf, n);
    f->ra_unit = false;
    u64 start = off & ~(u64)(f->vol->cluster - 1);
    size_t want = (size_t)NT_MIN((u64)NT_RA_BYTES, s->size - start);
    ssize_t got = stream_read(f, s, NT_A_DATA, NULL, 0, start, f->ra, want);
    f->ra_len = 0;
    if (got < 0) return got;
    f->ra_off = start;
    f->ra_len = (u32)got;
    if (off + n > start + (u64)got) return stream_read(f, s, NT_A_DATA, NULL, 0, off, buf, n);
    memcpy(buf, f->ra + (off - start), n);
    return (ssize_t)n;
}

/* ===================================================================================================== files */

void ntfs_file_close(NtfsFile *f) {
    if (!f) return;
    stream_free(&f->data);
    stream_free(&f->ialloc);
    if (f->rec) NT_FREE(f->rec);
    if (f->alist) NT_FREE(f->alist);
    if (f->iroot) NT_FREE(f->iroot);
    if (f->ra) NT_FREE(f->ra);
    NT_FREE(f);
}

/* Reads the $ATTRIBUTE_LIST of the base record into f->alist. It is a single attribute of the base record (resident, or non-resident with one extent). */
static int load_alist(NtfsFile *f) {
    NtfsVol *v = f->vol;
    u32 used = nt_le32(f->rec + 24), off = nt_le16(f->rec + 20);
    NtfsAttr a;
    for (;;) {
        int rc = attr_parse(f->rec, used, off, &a);
        if (rc < 0) return rc;
        if (rc == 0) return 0;
        if (a.type == NT_A_LIST && a.namelen == 0) break;
        off += a.len;
    }
    if (!a.nonres) {
        f->alist = NT_ALLOC((size_t)a.vlen + 8);
        if (!f->alist) return -ENOMEM;
        memcpy(f->alist, a.val, a.vlen);
        f->alist_len = a.vlen;
        return 0;
    }
    if (a.lvcn != 0 || a.size > NT_MAX_ALIST || a.size < 26) return -EIO;
    NtfsRuns rl;
    memset(&rl, 0, sizeof rl);
    int rc = runs_decode(v, &rl, a.mp, a.mplen, 0);
    if (!rc) {
        f->alist = NT_ALLOC((size_t)a.size + 8);
        if (!f->alist) rc = -ENOMEM;
    }
    if (!rc) {
        u32 hint = 0;
        u64 init = NT_MIN(a.init, a.size);
        rc = runs_read(v, &rl, &hint, 0, f->alist, (size_t)init);
        if (!rc && init < a.size) memset(f->alist + init, 0, (size_t)(a.size - init));
        f->alist_len = (u32)a.size;
    }
    runs_free(&rl);
    if (rc && f->alist) { NT_FREE(f->alist); f->alist = NULL; f->alist_len = 0; }
    return rc;
}

int ntfs_file_open(NtfsVol *v, u64 ref, NtfsFile **out) {
    u64 no = ref & NT_REF_MASK;
    u32 seq = (u32)(ref >> 48);
    NtfsFile *f = NT_ZALLOC(sizeof *f);
    if (!f) return -ENOMEM;
    f->vol = v;
    f->no = no;
    f->rec = NT_ALLOC(v->rec_size);
    if (!f->rec) { ntfs_file_close(f); return -ENOMEM; }
    const u8 *cached;
    int rc = rec_get(v, no, &cached);                              /* through the cache: a file looked at twice is read from the disk once */
    if (rc) goto fail;
    memcpy(f->rec, cached, v->rec_size);
    f->rflags = nt_le16(f->rec + 22);
    if (!(f->rflags & NT_REC_INUSE)) { rc = -ENOENT; goto fail; }
    if (nt_le64(f->rec + 32) != 0) { rc = -ENOENT; goto fail; }    /* an extension record is not a file */
    f->seq = nt_le16(f->rec + 16);
    if (seq && seq != f->seq) { rc = -ENOENT; goto fail; }         /* the entry that led here is out of date */
    f->nlink = nt_le16(f->rec + 18);
    if (f->nlink == 0) f->nlink = 1;
    f->is_dir = (f->rflags & NT_REC_DIR) != 0;
    rc = load_alist(f);
    if (rc) goto fail;
    NtfsStream std;
    rc = stream_open(f, NT_A_STD, NULL, 0, &std);
    if (rc) goto fail;
    if (std.present && std.resident && std.rlen >= 48) {
        f->ctime = ntfs_time_to_unix(nt_le64(std.rbuf + 16));
        f->mtime = ntfs_time_to_unix(nt_le64(std.rbuf + 8));
        f->atime = ntfs_time_to_unix(nt_le64(std.rbuf + 24));
        f->attrs = nt_le32(std.rbuf + 32);
    }
    bool have_std = std.present;
    stream_free(&std);
    if (!have_std && no >= 16) { NT_LOG("file %lu has no $STANDARD_INFORMATION", (unsigned long)no); rc = -EIO; goto fail; }
    if (f->attrs & NT_FA_REPARSE) {
        NtfsStream rp;
        rc = stream_open(f, NT_A_REPARSE, NULL, 0, &rp);
        if (rc) goto fail;
        if (rp.present && rp.resident && rp.rlen >= 8) f->reparse_tag = nt_le32(rp.rbuf);
        stream_free(&rp);
    }
    if (f->is_dir) {
        NtfsStream ir;
        rc = stream_open(f, NT_A_IROOT, NAME_I30, 4, &ir);
        if (rc) goto fail;
        if (!ir.present || !ir.resident || ir.rlen < 32 || nt_le32(ir.rbuf) != NT_A_NAME || nt_le32(ir.rbuf + 4) != 1) {
            stream_free(&ir);
            NT_LOG("directory %lu has no usable $INDEX_ROOT", (unsigned long)no);
            rc = -EIO;
            goto fail;
        }
        f->iroot = ir.rbuf;
        f->iroot_len = ir.rlen;
        ir.rbuf = NULL;
        stream_free(&ir);
        f->idx_size = nt_le32(f->iroot + 8);
        if (f->idx_size != v->idx_size) {                          /* every directory of a volume uses the volume's index block size */
            NT_LOG("directory %lu: index block size %u differs from the volume's %u", (unsigned long)no, f->idx_size, v->idx_size);
            rc = -EIO;
            goto fail;
        }
        rc = stream_open(f, NT_A_IALLOC, NAME_I30, 4, &f->ialloc);
        if (rc) goto fail;
        if (f->ialloc.present && f->ialloc.resident) { rc = -EIO; goto fail; }
    } else {
        rc = stream_open(f, NT_A_DATA, NULL, 0, &f->data);
        if (rc) goto fail;
        if (f->data.present && f->data.size > (u64)0x7FFFFFFFFFFFFFFF) { rc = -EIO; goto fail; }
    }
    *out = f;
    return 0;
fail:
    ntfs_file_close(f);
    return rc;
}

/* ===================================================================================================== directories */

typedef struct IdxEntry {
    u32 pos, len;
    u64 ref;
    u16 flags;
    u32 klen;
    const u8 *key;
    u64 sub;
} IdxEntry;

#define IE_SUB  1
#define IE_LAST 2

/* The entry at `pos` of a node whose entries end at `end`. Entries that carry a name must hold a whole $FILE_NAME. */
static int entry_at(const u8 *buf, u32 end, u32 pos, IdxEntry *e) {
    if (pos > end || end - pos < 16) return -EIO;
    u32 elen = nt_le16(buf + pos + 8);
    e->klen = nt_le16(buf + pos + 10);
    e->flags = nt_le16(buf + pos + 12);
    if (elen < 16 || (elen & 7) || elen > end - pos) return -EIO;
    u32 tail = (e->flags & IE_SUB) ? 8 : 0;
    if (e->klen > elen - 16 || elen - 16 - e->klen < tail) return -EIO;
    if (!(e->flags & IE_LAST)) {
        if (e->klen < 66 || 66u + 2u * buf[pos + 16 + 64] > e->klen) return -EIO;
    }
    e->pos = pos;
    e->len = elen;
    e->ref = nt_le64(buf + pos);
    e->key = buf + pos + 16;
    e->sub = tail ? nt_le64(buf + pos + elen - 8) : 0;
    return 0;
}

/* Checks an index node header at `hdr` (the root's value starts the header at 16, an INDX block at 0x18) and returns where its entries start and end. */
static int node_bounds(u32 hdr, u32 limit, const u8 *buf, u32 *first, u32 *end) {
    if (hdr + 16 > limit) return -EIO;
    u32 f0 = nt_le32(buf + hdr), total = nt_le32(buf + hdr + 4);
    if (f0 < 16 || (f0 & 7) || f0 > total || total > limit - hdr) return -EIO;
    *first = hdr + f0;
    *end = hdr + total;
    return 0;
}

/* An INDX block, validated and cached. The pointer is valid until the next idx_get(). */
static int idx_get(NtfsFile *d, u64 vcn, const u8 **out) {
    NtfsVol *v = d->vol;
    int victim = -1;
    for (int i = 0; i < NT_IDXCACHE; i++) {
        if (v->idx[i].buf && v->idx[i].dir == d->no && v->idx[i].vcn == vcn) {
            v->idx[i].use = ++v->clock;
            v->n_idx_hits++;
            *out = v->idx[i].buf;
            return 0;
        }
        if (victim < 0 || !v->idx[i].buf || (v->idx[victim].buf && v->idx[i].use < v->idx[victim].use)) victim = i;
    }
    v->n_idx_misses++;
    if (!d->ialloc.present) return -EIO;
    u32 shift = d->idx_size >= v->cluster ? v->cshift : 9;         /* a sub-node number counts clusters, or 512-byte units when blocks are smaller */
    if (vcn > ((1ull << 62) >> shift)) return -EIO;
    u64 off = vcn << shift;
    if (off > d->ialloc.size || d->idx_size > d->ialloc.size - off) return -EIO;
    if (!v->idx[victim].buf) {
        v->idx[victim].buf = NT_ALLOC(v->idx_size);                /* d->idx_size == v->idx_size, checked when the directory was opened */
        if (!v->idx[victim].buf) return -ENOMEM;
    }
    u8 *b = v->idx[victim].buf;
    v->idx[victim].dir = ~0ull;
    ssize_t got = stream_read(d, &d->ialloc, NT_A_IALLOC, NAME_I30, 4, off, b, d->idx_size);
    if (got < 0) return (int)got;
    if ((u32)got != d->idx_size) return -EIO;
    if (memcmp(b, "INDX", 4) || fixups_apply(b, d->idx_size, nt_le16(b + 4), nt_le16(b + 6)) || nt_le64(b + 16) != vcn) {
        NT_LOG("directory %lu: bad index block at VCN %lu", (unsigned long)d->no, (unsigned long)vcn);
        return -EIO;
    }
    u32 first, end;
    if (node_bounds(0x18, d->idx_size, b, &first, &end)) { NT_LOG("directory %lu: bad index node at VCN %lu", (unsigned long)d->no, (unsigned long)vcn); return -EIO; }
    v->idx[victim].dir = d->no;
    v->idx[victim].vcn = vcn;
    v->idx[victim].use = ++v->clock;
    *out = b;
    return 0;
}

int ntfs_dir_lookup(NtfsFile *d, const char *name, u64 *ref) {
    NtfsVol *v = d->vol;
    if (!d->is_dir) return -ENOTDIR;
    u16 tgt[256], up[256];
    int tn = ntfs_utf8_to_utf16(name, tgt, 256);
    if (tn < 0) return -ENOENT;
    if (tn == 0) return -ENOENT;
    for (int i = 0; i < tn; i++) up[i] = v->upcase[tgt[i]];
    const u8 *buf = d->iroot;
    u32 limit = d->iroot_len, hdr = 16;
    for (int depth = 0; depth <= NT_MAX_DEPTH; depth++) {
        u32 pos, end;
        if (node_bounds(hdr, limit, buf, &pos, &end)) return -EIO;
        bool descend = false;
        u64 next = 0, found = 0;
        bool have = false;
        while (pos < end) {
            IdxEntry e;
            int rc = entry_at(buf, end, pos, &e);
            if (rc) return rc;
            if (e.flags & IE_LAST) {
                if (e.flags & IE_SUB) { descend = true; next = e.sub; }
                break;
            }
            u32 kn = e.key[64];
            int cmp = ntfs_name_cmp(v, up, (u32)tn, e.key + 66, kn);
            if (cmp == 0) {
                if (ntfs_name_equal_exact(tgt, (u32)tn, e.key + 66, kn)) { *ref = e.ref; goto done; }
                if (!have) { have = true; found = e.ref; }         /* equal ignoring case: keep looking for an exact match among the neighbours */
            } else if (cmp < 0) {
                if (have) break;
                if (e.flags & IE_SUB) { descend = true; next = e.sub; }
                break;
            }
            pos += e.len;
        }
        if (have) { *ref = found; goto done; }
        if (!descend) return -ENOENT;
        const u8 *blk;
        int rc = idx_get(d, next, &blk);
        if (rc) return rc;
        buf = blk;
        limit = d->idx_size;
        hdr = 0x18;
    }
    NT_LOG("directory %lu: index tree is deeper than %d levels", (unsigned long)d->no, NT_MAX_DEPTH);
    return -EIO;
done:
    if (!v->opts.show_system && (*ref & NT_REF_MASK) < 16 && (*ref & NT_REF_MASK) != 5) return -ENOENT;      /* the metadata files stay hidden */
    return 0;
}

void ntfs_dir_cursor_reset(NtfsDirCursor *c) {
    memset(c, 0, sizeof *c);
}

#define CUR_FIRST 0xFFFFFFFFu            /* a frame that has not looked at its node yet */
#define ROOT_VCN  (~0ull)

int ntfs_dir_next(NtfsFile *d, NtfsDirCursor *c, NtfsDirEntry *out) {
    NtfsVol *v = d->vol;
    (void)v;
    if (!d->is_dir) return -ENOTDIR;
    u64 max_nodes = d->ialloc.present ? d->ialloc.size / d->idx_size + 1 : 0;
    if (!c->started) {
        c->started = true;
        c->depth = 1;
        c->f[0].vcn = ROOT_VCN;
        c->f[0].off = CUR_FIRST;
        c->f[0].sub_done = 0;
        c->visited = 0;
    }
    while (c->depth > 0) {
        typeof(c->f[0]) *fr = &c->f[c->depth - 1];
        const u8 *buf;
        u32 limit, hdr;
        if (fr->vcn == ROOT_VCN) { buf = d->iroot; limit = d->iroot_len; hdr = 16; }
        else {
            int rc = idx_get(d, fr->vcn, &buf);
            if (rc) return rc;
            limit = d->idx_size;
            hdr = 0x18;
        }
        u32 first, end;
        if (node_bounds(hdr, limit, buf, &first, &end)) return -EIO;
        if (fr->off == CUR_FIRST) fr->off = first;
        IdxEntry e;
        int rc = entry_at(buf, end, fr->off, &e);
        if (rc) return rc;
        if (!fr->sub_done && (e.flags & IE_SUB)) {
            fr->sub_done = 1;
            if (c->depth >= NT_MAX_DEPTH || ++c->visited > max_nodes) { NT_LOG("directory %lu: the index loops or is too deep", (unsigned long)d->no); return -EIO; }
            fr = &c->f[c->depth++];
            fr->vcn = e.sub;
            fr->off = CUR_FIRST;
            fr->sub_done = 0;
            continue;
        }
        if (e.flags & IE_LAST) { c->depth--; continue; }
        fr->off += e.len;
        fr->sub_done = 0;
        out->ref = e.ref;
        out->flags = nt_le32(e.key + 56);
        out->reparse = nt_le32(e.key + 60);
        out->ns = e.key[65];
        out->nunits = e.key[64];
        out->name = e.key + 66;
        return 1;
    }
    return 0;
}

int ntfs_dir_next_visible(NtfsFile *dir, NtfsDirCursor *c, NtfsVisible *out, u32 *skipped) {
    NtfsVol *v = dir->vol;
    for (;;) {
        NtfsDirEntry e;
        int rc = ntfs_dir_next(dir, c, &e);
        if (rc <= 0) return rc;
        u64 no = e.ref & NT_REF_MASK;
        if (e.ns == NT_NS_DOS && !v->opts.show_dos) continue;       /* the long name is listed once */
        if (!v->opts.show_system && no < 16) continue;               /* $MFT ... and the root's own "." entry */
        int n = ntfs_utf16_to_utf8(e.name, e.nunits, out->name, NT_NAME_MAX);
        if (n < 0) { if (skipped) (*skipped)++; continue; }
        out->name[n] = 0;
        if (n == 0 || (n == 1 && out->name[0] == '.') || (n == 2 && out->name[0] == '.' && out->name[1] == '.')) continue;
        out->ino = no;
        if ((e.flags & NT_FA_REPARSE) && (e.reparse == NT_TAG_SYMLINK || e.reparse == NT_TAG_JUNCTION)) out->type = NT_DT_LNK;
        else out->type = (e.flags & NT_FA_DIRINDEX) ? NT_DT_DIR : NT_DT_REG;
        return 1;
    }
}

/* The parent directory according to the file's first $FILE_NAME. */
typedef struct { u64 parent; bool got; } ParentCtx;

static int cb_parent(void *ctx, const NtfsAttr *a) {
    ParentCtx *p = ctx;
    if (a->nonres || a->vlen < 66) return 0;
    p->parent = nt_le64(a->val) & NT_REF_MASK;
    p->got = true;
    return 1;
}

int ntfs_parent_of(NtfsFile *f, u64 *parent) {
    if (f->no == 5) { *parent = 5; return 0; }
    ParentCtx p = { 0, false };
    int rc = file_extents(f, NT_A_NAME, NULL, 0, cb_parent, &p);
    if (rc) return rc;
    if (!p.got) return -EIO;
    *parent = p.parent;
    return 0;
}

/* ===================================================================================================== reparse points */

bool ntfs_is_link(const NtfsFile *f) {
    return (f->attrs & NT_FA_REPARSE) && (f->reparse_tag == NT_TAG_SYMLINK || f->reparse_tag == NT_TAG_JUNCTION);
}

/* Turns the target of a symbolic link or junction into a path this system can follow. An absolute target (\??\C:\Users, \??\Volume{...}\x, \x) is taken to
 * be on this very volume and becomes <mount point>/Users; a relative one keeps its shape with slashes. Targets on other volumes or on the network cannot be
 * followed and give -EINVAL. */
int ntfs_readlink(NtfsFile *f, const char *mount_target, char *out, size_t cap) {
    if (!ntfs_is_link(f) || cap < 2) return -EINVAL;
    NtfsStream rp;
    int rc = stream_open(f, NT_A_REPARSE, NULL, 0, &rp);
    if (rc) return rc;
    if (!rp.present || !rp.resident || rp.rlen < 8) { stream_free(&rp); return -EIO; }
    u32 dlen = nt_le16(rp.rbuf + 4);
    bool is_symlink = f->reparse_tag == NT_TAG_SYMLINK, relative = false;
    u32 hdr = is_symlink ? 12 : 8;
    if (dlen + 8 > rp.rlen || dlen < hdr) { stream_free(&rp); return -EIO; }
    const u8 *d = rp.rbuf + 8;
    u32 so = nt_le16(d), sl = nt_le16(d + 2), po = nt_le16(d + 4), pl = nt_le16(d + 6);
    if (is_symlink) relative = (nt_le32(d + 8) & 1) != 0;
    const u8 *names = d + hdr;
    u32 nlen = dlen - hdr;
    if (sl == 0 && pl != 0) { so = po; sl = pl; }                  /* no substitute name: the print name will do */
    if ((sl & 1) || so > nlen || sl > nlen - so || sl == 0 || sl / 2 > 1024) { stream_free(&rp); return -EIO; }
    char *text = NT_ALLOC(sl * 2 + 8);
    if (!text) { stream_free(&rp); return -ENOMEM; }
    int n = ntfs_utf16_to_utf8(names + so, sl / 2, text, sl * 2 + 4);
    stream_free(&rp);
    if (n <= 0) { NT_FREE(text); return -EIO; }
    text[n] = 0;
    const char *p = text;
    bool absolute = false;
    if (!strncmp(p, "\\??\\", 4)) {
        p += 4;
        absolute = true;
        if (!strncmp(p, "UNC\\", 4)) { NT_FREE(text); return -EINVAL; }
        if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') p += 2;
        else if (!strncmp(p, "Volume{", 7)) { const char *q = strchr(p, '\\'); p = q ? q : p + strlen(p); }
        else { NT_FREE(text); return -EINVAL; }
    } else if (!relative) {
        absolute = true;
        if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') p += 2;
    }
    while (*p == '\\') p++;
    size_t o = 0;
    if (absolute) {
        size_t ml = strlen(mount_target);
        while (ml > 1 && mount_target[ml - 1] == '/') ml--;
        if (ml + 2 > cap) { NT_FREE(text); return -ENAMETOOLONG; }
        if (!(ml == 1 && mount_target[0] == '/')) { memcpy(out, mount_target, ml); o = ml; }
        out[o++] = '/';
    }
    for (; *p; p++) {
        if (o + 1 >= cap) { NT_FREE(text); return -ENAMETOOLONG; }
        out[o++] = *p == '\\' ? '/' : *p;
    }
    if (o > 1 && out[o - 1] == '/' && absolute) o--;              /* "<mount>/" for the root of the volume: drop the slash after a real directory */
    out[o] = 0;
    NT_FREE(text);
    return (int)o;
}

/* ===================================================================================================== volume */

static void vol_set_why(char *why, size_t whyn, const char *s) {
    if (why && whyn) { size_t n = strlen(s); if (n >= whyn) n = whyn - 1; memcpy(why, s, n); why[n] = 0; }
}

static int geometry_from_boot(NtfsVol *v, const u8 *b, char *why, size_t whyn) {
    if (memcmp(b + 3, "NTFS    ", 8)) { vol_set_why(why, whyn, "no NTFS signature in the boot sector"); return -EINVAL; }
    if (b[510] != 0x55 || b[511] != 0xAA) { vol_set_why(why, whyn, "no boot sector signature"); return -EINVAL; }
    u32 bps = nt_le16(b + 11), spc_raw = b[13];
    if (bps < 512 || bps > 4096 || (bps & (bps - 1))) { vol_set_why(why, whyn, "unsupported sector size"); return -EINVAL; }
    if (v->dev.sector > bps) { vol_set_why(why, whyn, "the volume's sectors are smaller than the device's"); return -EINVAL; }
    u32 spc;
    if (spc_raw == 0) spc = 0;
    else if (spc_raw <= 128) spc = (spc_raw & (spc_raw - 1)) ? 0 : spc_raw;
    else if (spc_raw >= 0xF4) spc = 1u << (256 - spc_raw);         /* 244..255: 2 to the (256-n) sectors */
    else spc = 0;
    if (!spc || (u64)bps * spc > (2u << 20)) { vol_set_why(why, whyn, "unsupported cluster size"); return -EINVAL; }
    if (nt_le16(b + 14) || b[16] || nt_le16(b + 17) || nt_le16(b + 19) || nt_le16(b + 22) || nt_le32(b + 32)) {
        vol_set_why(why, whyn, "the BIOS parameter block holds FAT fields: not an NTFS volume");
        return -EINVAL;
    }
    u64 total = nt_le64(b + 40);
    if (total < 16 || total > v->dev.size / bps) { vol_set_why(why, whyn, "the volume is larger than its device (or empty)"); return -EINVAL; }
    v->bps = bps;
    v->spc = spc;
    v->cluster = bps * spc;
    v->cshift = (u32)__builtin_ctz(v->cluster);
    v->total_sectors = total;
    v->nclusters = total / spc;
    v->mft_lcn = nt_le64(b + 48);
    v->mftmirr_lcn = nt_le64(b + 56);
    if (v->mft_lcn >= v->nclusters || v->nclusters > (1ull << 62) >> v->cshift) { vol_set_why(why, whyn, "the $MFT lies outside the volume"); return -EINVAL; }
    for (int k = 0; k < 2; k++) {
        s8 sv = (s8)b[k ? 68 : 64];
        u32 size;
        if (sv > 0) size = (u32)sv * v->cluster;
        else if (sv >= -16 && sv <= -9) size = 1u << -sv;
        else { vol_set_why(why, whyn, k ? "bad index block size" : "bad MFT record size"); return -EINVAL; }
        if (size < 512 || size > 65536 || (size & (size - 1))) { vol_set_why(why, whyn, k ? "bad index block size" : "bad MFT record size"); return -EINVAL; }
        if (k) v->idx_size = size; else v->rec_size = size;
    }
    v->serial = nt_le64(b + 72);
    return 0;
}

typedef struct { NtfsVol *v; u64 size; bool got; } MftCtx;

static int cb_mft_first(void *ctx, const NtfsAttr *a) {
    MftCtx *c = ctx;
    if (!a->nonres || a->lvcn != 0) return 0;
    if (a->size < (u64)16 * c->v->rec_size || a->size > (1ull << 40)) return -EIO;
    c->size = a->size;
    c->got = true;
    int rc = runs_decode(c->v, &c->v->mft, a->mp, a->mplen, 0);
    return rc ? rc : 1;
}

/* Record 0 is at the cluster the boot sector names; its $DATA run list is the beginning of the map of the whole $MFT. If $MFT is fragmented so much that
 * record 0 has an $ATTRIBUTE_LIST, the other extents of its $DATA are in extension records, which are reached through the part of the map known so far. */
static int load_mft(NtfsVol *v, char *why, size_t whyn) {
    u8 *rec = NT_ALLOC(v->rec_size);
    if (!rec) return -ENOMEM;
    int rc = vol_read(v, v->mft_lcn << v->cshift, rec, v->rec_size);
    const char *w = NULL;
    if (!rc) rc = rec_check(v, rec, 0, &w);
    if (rc) { vol_set_why(why, whyn, w ? w : "cannot read the $MFT record"); NT_FREE(rec); return rc < 0 ? rc : -EIO; }
    NtfsFile tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.vol = v;
    tmp.no = 0;
    tmp.rec = rec;
    MftCtx c = { v, 0, false };
    rc = load_alist(&tmp);
    if (!rc && !tmp.alist) {
        rc = file_extents_scan(rec, NT_A_DATA, NULL, 0, cb_mft_first, &c);
        if (rc > 0) rc = 0;
    } else if (!rc) {
        /* first extent from the base record, then the others through the list in order */
        rc = file_extents_scan(rec, NT_A_DATA, NULL, 0, cb_mft_first, &c);
        if (rc > 0) rc = 0;
        if (!rc && c.got) {
            v->mft_size = c.size;
            v->nrecords = c.size / v->rec_size;
            u32 pos = 0, count = 0;
            while (!rc && tmp.alist_len - pos >= 26) {
                const u8 *e = tmp.alist + pos;
                u32 elen = nt_le16(e + 4), nl = e[6];
                if (elen < 26 || elen > tmp.alist_len - pos) { rc = -EIO; break; }
                pos += elen;
                if (nt_le32(e) != NT_A_DATA || nl != 0 || nt_le64(e + 8) == 0) continue;
                if (++count > NT_MAX_EXTENTS) { rc = -EIO; break; }
                u64 eno = nt_le64(e + 16) & NT_REF_MASK;
                u32 inst = nt_le16(e + 24);
                if (eno == 0) { rc = -EIO; break; }
                u8 *erec = NT_ALLOC(v->rec_size);
                if (!erec) { rc = -ENOMEM; break; }
                rc = rec_load(v, eno, erec);
                if (!rc && (!(nt_le16(erec + 22) & NT_REC_INUSE) || (nt_le64(erec + 32) & NT_REF_MASK) != 0)) rc = -EIO;
                if (!rc) {
                    u32 used = nt_le32(erec + 24), off = nt_le16(erec + 20);
                    NtfsAttr a;
                    bool done = false;
                    for (;;) {
                        int pr = attr_parse(erec, used, off, &a);
                        if (pr < 0) { rc = pr; break; }
                        if (pr == 0) break;
                        if (attr_is(&a, NT_A_DATA, NULL, 0) && a.inst == inst && a.nonres) {
                            rc = runs_decode(v, &v->mft, a.mp, a.mplen, a.lvcn);
                            done = true;
                            break;
                        }
                        off += a.len;
                    }
                    if (!rc && !done) rc = -EIO;
                }
                NT_FREE(erec);
            }
        }
    }
    if (tmp.alist) NT_FREE(tmp.alist);
    NT_FREE(rec);
    if (rc) { vol_set_why(why, whyn, "the $MFT's map is damaged"); return rc < 0 ? rc : -EIO; }
    if (!c.got) { vol_set_why(why, whyn, "$MFT has no non-resident $DATA"); return -EINVAL; }
    v->mft_size = c.size;
    v->nrecords = c.size / v->rec_size;
    if ((v->mft.end << v->cshift) < (u64)16 * v->rec_size) { vol_set_why(why, whyn, "the $MFT is too short"); return -EINVAL; }
    return 0;
}

static void load_upcase(NtfsVol *v) {
    v->upcase = NT_ALLOC(65536 * sizeof(u16));
    if (!v->upcase) return;
    ntfs_upcase_builtin(v->upcase);
    v->upcase_builtin = true;
    NtfsFile *f;
    if (ntfs_file_open(v, 10, &f)) { NT_LOG("$UpCase cannot be opened: names are compared with a built-in table"); return; }
    if (f->data.present && !f->data.resident && f->data.size == 131072) {
        u8 *raw = NT_ALLOC(131072);
        if (raw) {
            if (ntfs_read(f, 0, raw, 131072) == 131072) {
                for (u32 i = 0; i < 65536; i++) v->upcase[i] = nt_le16(raw + 2 * i);
                v->upcase_builtin = false;
            }
            NT_FREE(raw);
        }
    }
    if (v->upcase_builtin) NT_LOG("$UpCase is not 128 KiB: names are compared with a built-in table");
    ntfs_file_close(f);
}

static void load_volume_info(NtfsVol *v) {
    NtfsFile *f;
    if (ntfs_file_open(v, 3, &f)) return;
    NtfsStream s;
    if (!stream_open(f, NT_A_VOLNAME, NULL, 0, &s) && s.present && s.resident) {
        int n = ntfs_utf16_to_utf8(s.rbuf, s.rlen / 2 > 128 ? 128 : s.rlen / 2, v->label, (u32)sizeof v->label - 1);
        v->label[n > 0 ? n : 0] = 0;
    }
    stream_free(&s);
    if (!stream_open(f, NT_A_VOLINFO, NULL, 0, &s) && s.present && s.resident && s.rlen >= 12) {
        v->ver_major = s.rbuf[8];
        v->ver_minor = s.rbuf[9];
        v->vol_flags = nt_le16(s.rbuf + 10);
    }
    stream_free(&s);
    ntfs_file_close(f);
}

int ntfs_vol_open(NtfsVol **out, const NtfsDev *dev, const NtfsOpts *opts, char *why, size_t whyn) {
    if (why && whyn) why[0] = 0;
    if (dev->size < 8192) { vol_set_why(why, whyn, "the device is too small"); return -EINVAL; }
    NtfsVol *v = NT_ZALLOC(sizeof *v);
    if (!v) return -ENOMEM;
    v->dev = *dev;
    if (opts) v->opts = *opts;
    u8 *b = NT_ALLOC(512);
    if (!b) { NT_FREE(v); return -ENOMEM; }
    int rc = vol_read(v, 0, b, 512);
    if (!rc) rc = geometry_from_boot(v, b, why, whyn);
    NT_FREE(b);
    if (rc) { NT_FREE(v); return rc < 0 ? rc : -EIO; }
    rc = load_mft(v, why, whyn);
    if (rc) { ntfs_vol_close(v); return rc; }
    load_upcase(v);
    if (!v->upcase) { ntfs_vol_close(v); return -ENOMEM; }
    load_volume_info(v);
    NtfsFile *root;
    rc = ntfs_file_open(v, 5, &root);                              /* the root must be a directory with a usable index */
    if (rc || !root->is_dir) {
        if (!rc) ntfs_file_close(root);
        vol_set_why(why, whyn, "the root directory is damaged");
        ntfs_vol_close(v);
        return rc ? rc : -EIO;
    }
    ntfs_file_close(root);
    *out = v;
    return 0;
}

void ntfs_vol_close(NtfsVol *v) {
    if (!v) return;
    runs_free(&v->mft);
    if (v->upcase) NT_FREE(v->upcase);
    for (int i = 0; i < NT_RECCACHE; i++) if (v->recs[i].buf) NT_FREE(v->recs[i].buf);
    for (int i = 0; i < NT_IDXCACHE; i++) if (v->idx[i].buf) NT_FREE(v->idx[i].buf);
    NT_FREE(v);
}

/* Free clusters: the number of clear bits of $Bitmap below the cluster count. */
int ntfs_free_clusters(NtfsVol *v, u64 *free_out) {
    if (v->free_known) { *free_out = v->free_clusters; return 0; }
    NtfsFile *f;
    int rc = ntfs_file_open(v, 6, &f);
    if (rc) return rc;
    u64 used = 0, total = v->nclusters;
    u8 *chunk = NT_ALLOC(65536);
    if (!chunk) { ntfs_file_close(f); return -ENOMEM; }
    u64 nbytes = (total + 7) / 8;
    for (u64 off = 0; off < nbytes && !rc;) {
        size_t want = (size_t)NT_MIN((u64)65536, nbytes - off);
        ssize_t got = ntfs_read(f, off, chunk, want);
        if (got <= 0) { rc = got < 0 ? (int)got : -EIO; break; }
        for (ssize_t i = 0; i < got; i++) {
            u64 base = (off + (u64)i) * 8;
            u8 byte = chunk[i];
            if (base + 8 > total) byte &= (u8)((1u << (total - base)) - 1);
            used += (u64)__builtin_popcount(byte);
        }
        off += (u64)got;
    }
    NT_FREE(chunk);
    ntfs_file_close(f);
    if (rc) return rc;
    v->free_clusters = total - used;
    v->free_known = true;
    *free_out = v->free_clusters;
    return 0;
}

/* ===================================================================================================== debugging text */

int ntfs_dump_record(NtfsVol *v, u64 no, char *out, size_t cap) {
    u8 *rec = NT_ALLOC(v->rec_size);
    if (!rec) return -ENOMEM;
    int rc = rec_load(v, no, rec);
    if (rc) { NT_FREE(rec); return rc; }
    size_t o = 0;
#define EMIT(...) do { if (o < cap) o += (size_t)snprintf(out + o, cap - o, __VA_ARGS__); } while (0)
    EMIT("record %lu: seq %u links %u flags %#x used %u of %u base %#lx\n", (unsigned long)no, nt_le16(rec + 16), nt_le16(rec + 18), nt_le16(rec + 22),
         nt_le32(rec + 24), nt_le32(rec + 28), (unsigned long)nt_le64(rec + 32));
    u32 used = nt_le32(rec + 24), off = nt_le16(rec + 20);
    NtfsAttr a;
    for (;;) {
        int pr = attr_parse(rec, used, off, &a);
        if (pr < 0) { EMIT("  damaged attribute at %u\n", off); break; }
        if (pr == 0) break;
        EMIT("  type %#x id %u flags %#x %s", a.type, a.inst, a.flags, a.nonres ? "non-resident" : "resident");
        if (a.namelen) { EMIT(" name '"); for (u32 i = 0; i < a.namelen; i++) EMIT("%c", nt_le16(a.name + 2 * i) < 128 ? (char)nt_le16(a.name + 2 * i) : '?'); EMIT("'"); }
        if (a.nonres) EMIT(" vcn %lu size %lu init %lu alloc %lu cunit %u\n", (unsigned long)a.lvcn, (unsigned long)a.size, (unsigned long)a.init, (unsigned long)a.alloc, a.cunit);
        else EMIT(" %u bytes\n", a.vlen);
        off += a.len;
    }
#undef EMIT
    NT_FREE(rec);
    return (int)(o < cap ? o : cap);
}
