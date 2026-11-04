/*
 * fatfix: see fatfix.h. Compiled only where FATCK_REPAIR is defined (the fsck program and the host test DLL); in the kernel build this file
 * is an empty translation unit.
 */
typedef int fatfix_translation_unit_is_not_empty;

#ifdef FATCK_REPAIR

#include "fatfix.h"
#include <stdarg.h>
#ifdef FATCK_HOST
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
#else
#include <string.h>
#endif

#define EIO_    5
#define ENOENT_ 2
#define ENOSPC_ 28
#define EINVAL_ 22
#define ESTALE_ 116
#define ENOMEM_ 12

static inline uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

typedef struct Fx {
    FatCk *ck;
    const FatFixOpts *o;
    FatFixStats *st;
    const FatCkGeom *g;
    uint32_t maxc;
    uint32_t cursor;                /* where the next free-cluster search starts */
    uint32_t found_dir;             /* FOUND.000 */
    uint32_t chk_next;              /* number of the next FILEnnnn.CHK */
    uint32_t *seen;                 /* while lost chains are recovered: clusters already dealt with (and clusters the recovery allocated) */
    uint32_t *prot;                 /* clusters that belong to chains another entry has run into and that are not separated yet: not to be freed */
    bool     prot_failed;           /* ... and there was no memory to find them: free nothing this pass */
    bool     fsinfo_refused;        /* the FSInfo sector is not ours to rewrite (said once) */
    uint32_t ndecl;                 /* repairs that were declined (by the mode or by the answer): not asked or reported again in the next pass */
    uint32_t decl[64];
    uint8_t *buf;                   /* one cluster of scratch */
    struct { uint32_t dir, idx, cluster; bool valid; } sc;     /* directory slot -> cluster cache for sequential access */
} Fx;

#define SAYK(fx, kind, ...) do { char _b[320]; fatck_fmt(_b, sizeof _b, __VA_ARGS__); if ((fx)->o->say) (fx)->o->say((fx)->o->ctx, (kind), _b); } while (0)
#define SAY(fx, ...) SAYK(fx, FSAY_FIXED, __VA_ARGS__)

static inline bool valid_cluster(const Fx *fx, uint32_t c) { return c >= 2 && c < fx->maxc; }
static inline bool bit_get(const uint32_t *b, uint32_t i) { return (b[i >> 5] >> (i & 31)) & 1; }
static inline void bit_set(uint32_t *b, uint32_t i) { b[i >> 5] |= 1u << (i & 31); }

static uint32_t text_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h ? h : 1;
}

/* Is a repair of this level allowed? (-n: never; -a: safe ones; -y: all; interactive: whatever the answer is). The next pass finds a problem
 * that was left alone again: it is not reported or counted twice, and an answer of "no" is not asked for again. */
static bool allow(Fx *fx, int level, const char *what) {
    if (fx->o->mode == FIX_NONE && !fx->o->ask) return false;
    uint32_t h = text_hash(what) + (uint32_t)level * 0x9E3779B1u;
    bool known = false;
    for (uint32_t i = 0; i < fx->ndecl; i++)
        if (fx->decl[i] == h) known = true;
    if (known && fx->o->ask) return false;
    bool yes = fx->o->ask ? fx->o->ask(fx->o->ctx, level, what) : fx->o->mode >= level;
    if (!yes && !known) {
        if (fx->ndecl < sizeof fx->decl / sizeof fx->decl[0]) fx->decl[fx->ndecl++] = h;
        fx->st->declined++;
        SAYK(fx, FSAY_LEFT, "left alone (%s): %s", fx->o->ask ? "declined" : "needs -y", what);
    }
    return yes;
}

/* ---------------------------------------------------------------------------------------------- the FAT and directory slots */

static int fget(Fx *fx, uint32_t c, uint32_t *v) { return fatck_fat_get(fx->ck, c, v); }
static int fset(Fx *fx, uint32_t c, uint32_t v) { fx->st->wrote = true; return fatck_fat_set(fx->ck, c, v); }

static int slot_off(Fx *fx, uint32_t dir, uint32_t slot, uint64_t *off) {
    const FatCkGeom *g = fx->g;
    if (dir == 0 && g->bits != 32) {
        if (slot >= g->root_entries) return -EINVAL_;
        *off = g->root_off + (uint64_t)slot * 32;
        return 0;
    }
    uint32_t per = g->cluster_bytes / 32, idx = slot / per, c = dir, i = 0, v;
    if (fx->sc.valid && fx->sc.dir == dir && idx >= fx->sc.idx) { c = fx->sc.cluster; i = fx->sc.idx; }
    if (!valid_cluster(fx, c)) return -EINVAL_;
    for (; i < idx; i++) {
        if (fget(fx, c, &v)) return -EIO_;
        if (!valid_cluster(fx, v) || fatck_is_eoc(fx->ck, v)) return -EINVAL_;
        c = v;
    }
    fx->sc.valid = true;
    fx->sc.dir = dir;
    fx->sc.idx = idx;
    fx->sc.cluster = c;
    *off = fatck_cluster_off(fx->ck, c) + (uint64_t)(slot % per) * 32;
    return 0;
}

static int slot_rd(Fx *fx, uint32_t dir, uint32_t slot, uint8_t e[32]) {
    uint64_t off;
    int rc = slot_off(fx, dir, slot, &off);
    return rc ? rc : fatck_read(fx->ck, off, e, 32);
}

static int slot_wr(Fx *fx, uint32_t dir, uint32_t slot, const uint8_t e[32]) {
    uint64_t off;
    int rc = slot_off(fx, dir, slot, &off);
    if (rc) return rc;
    fx->st->wrote = true;
    return fatck_write(fx->ck, off, e, 32);
}

static uint32_t ent_first(const Fx *fx, const uint8_t *e) { return rd16(e + 26) | (fx->g->bits == 32 ? rd16(e + 20) << 16 : 0); }
static void ent_set_first(const Fx *fx, uint8_t *e, uint32_t v) { wr16(e + 26, v); if (fx->g->bits == 32) wr16(e + 20, v >> 16); }

/* The short entry a finding is about, if it still is what the scan saw. */
static int load_entry(Fx *fx, const FatCkFinding *f, uint8_t e[32], uint32_t *slot) {
    if (f->nslots == 0) return -ENOENT_;
    uint32_t sl = f->slot + f->nslots - 1;
    int rc = slot_rd(fx, f->dir, sl, e);
    if (rc) return rc;
    if (e[0] == 0x00 || e[0] == 0xE5 || (e[11] & 0x3F) == 0x0F) return -ENOENT_;
    if (ent_first(fx, e) != f->first || rd32(e + 28) != f->size) return -ESTALE_;
    *slot = sl;
    return 0;
}

static int delete_set(Fx *fx, uint32_t dir, uint32_t slot, uint32_t nslots) {
    uint8_t e[32];
    for (uint32_t i = 0; i < nslots; i++) {
        int rc = slot_rd(fx, dir, slot + i, e);
        if (rc) return rc;
        if (e[0] == 0x00 || e[0] == 0xE5) continue;
        e[0] = 0xE5;
        rc = slot_wr(fx, dir, slot + i, e);
        if (rc) return rc;
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------------- chains and free clusters */

/* the n-th cluster (0-based) of the chain that starts at `first`; 0 if the chain is shorter or goes wrong */
static uint32_t chain_nth(Fx *fx, uint32_t first, uint32_t n) {
    uint32_t c = first, v;
    for (uint32_t i = 0; i < n; i++) {
        if (!valid_cluster(fx, c) || fget(fx, c, &v)) return 0;
        c = v;
    }
    return valid_cluster(fx, c) ? c : 0;
}

/* Marks the chains that start at the cluster of every cross-link finding (a chain that another entry has run into). Until the cross-link is
 * separated, whatever the first owner trims off its own chain may still be what the second entry reads: freeing it would hand out the
 * clusters of a file that is still there. The cross-link repairs run before the trimming ones, so this is only the safety net for a pass in
 * which they could not do their work (no room to copy, directories that need -y). */
static void protect_chains(Fx *fx, const FatCkFinding *list, uint32_t n) {
    fx->prot_failed = false;
    bool any = false;
    for (uint32_t i = 0; i < n; i++)
        if (list[i].code == FK_CROSS && valid_cluster(fx, list[i].cluster)) { any = true; break; }
    if (!any) { if (fx->prot) memset(fx->prot, 0, (((size_t)fx->maxc + 31) / 32) * 4); return; }
    size_t words = ((size_t)fx->maxc + 31) / 32;
    if (!fx->prot) fx->prot = fatck_scratch(fx->ck, words * 4);
    else memset(fx->prot, 0, words * 4);
    if (!fx->prot) { fx->prot_failed = true; return; }
    for (uint32_t i = 0; i < n; i++) {
        if (list[i].code != FK_CROSS) continue;
        uint32_t c = list[i].cluster, v;
        for (uint32_t k = 0; valid_cluster(fx, c) && !bit_get(fx->prot, c) && k <= fx->g->nclusters; k++) {
            bit_set(fx->prot, c);
            if (fget(fx, c, &v) || v == 0 || fatck_is_eoc(fx->ck, v) || fatck_is_bad(fx->ck, v) || fatck_is_reserved(fx->ck, v)) break;
            c = v;
        }
    }
}

/* frees the chain from c on (c included), up to a cluster that another entry still reaches (see protect_chains) */
static uint32_t free_from(Fx *fx, uint32_t c) {
    uint32_t n = 0, v;
    if (fx->prot_failed) return 0;
    while (valid_cluster(fx, c) && n <= fx->g->nclusters) {
        if (fx->prot && bit_get(fx->prot, c)) break;
        if (fget(fx, c, &v) || v == 0) break;
        fset(fx, c, 0);
        n++;
        if (fatck_is_eoc(fx->ck, v) || fatck_is_bad(fx->ck, v) || fatck_is_reserved(fx->ck, v) || !valid_cluster(fx, v)) break;
        c = v;
    }
    fx->st->freed_clusters += n;
    return n;
}

static int alloc_cluster(Fx *fx, uint32_t *out) {
    uint32_t c = valid_cluster(fx, fx->cursor) ? fx->cursor : 2;
    for (uint32_t n = 0; n < fx->g->nclusters; n++) {
        uint32_t v;
        if (fget(fx, c, &v)) return -EIO_;
        if (v == 0 && !fatck_cluster_owned(fx->ck, c)) {                 /* (a chain that still ends in a "free" cluster owns it: not ours to hand out) */
            int rc = fset(fx, c, fatck_eoc(fx->ck));
            if (rc) return rc;
            fx->cursor = c + 1;
            if (fx->seen) bit_set(fx->seen, c);
            *out = c;
            return 0;
        }
        if (++c >= fx->maxc) c = 2;
    }
    return -ENOSPC_;
}

static int zero_cluster(Fx *fx, uint32_t c) {
    if (!fx->buf) {
        fx->buf = fatck_scratch(fx->ck, fx->g->cluster_bytes);
        if (!fx->buf) return -ENOMEM_;
    }
    memset(fx->buf, 0, fx->g->cluster_bytes);
    fx->st->wrote = true;
    return fatck_write(fx->ck, fatck_cluster_off(fx->ck, c), fx->buf, fx->g->cluster_bytes);
}

/* ---------------------------------------------------------------------------------------------- time stamps */

static void unix_to_dos(uint64_t t, uint32_t *date, uint32_t *tm) {
    if (t < 315532800) t = 315532800;                                    /* 1980-01-01 */
    uint64_t days = t / 86400, rem = t % 86400;
    int64_t z = (int64_t)days + 719468;
    int64_t era = z / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    if (y > 2107) { y = 2107; m = 12; d = 31; }
    *date = (uint32_t)(((y - 1980) << 9) | (m << 5) | d);
    *tm = (uint32_t)(((rem / 3600) << 11) | (((rem / 60) % 60) << 5) | ((rem % 60) / 2));
}

static void make_entry(Fx *fx, uint8_t e[32], const char sfn[11], uint8_t attr, uint32_t first, uint32_t size) {
    uint32_t date, tm;
    unix_to_dos(fx->o->unix_time, &date, &tm);
    memset(e, 0, 32);
    memcpy(e, sfn, 11);
    e[11] = attr;
    wr16(e + 14, tm); wr16(e + 16, date); wr16(e + 18, date); wr16(e + 22, tm); wr16(e + 24, date);
    ent_set_first(fx, e, first);
    wr32(e + 28, size);
}

/* Puts an entry into the first free slot of a directory, growing it (FAT32 root and subdirectories) when it is full. */
static int dir_add(Fx *fx, uint32_t dir, const uint8_t ent[32], uint32_t *slot_out) {
    uint32_t nslots;
    int rc = fatck_dir_slots(fx->ck, dir, &nslots);
    if (rc) return rc;
    uint8_t e[32];
    for (uint32_t s = 0; s < nslots; s++) {
        rc = slot_rd(fx, dir, s, e);
        if (rc) return rc;
        if (e[0] == 0x00 || e[0] == 0xE5) {
            rc = slot_wr(fx, dir, s, ent);
            if (!rc && slot_out) *slot_out = s;
            return rc;
        }
    }
    if (dir == 0 && fx->g->bits != 32) return -ENOSPC_;                  /* the fixed root of FAT12/16 cannot grow */
    uint32_t per = fx->g->cluster_bytes / 32;
    if (nslots + per > 65536) return -ENOSPC_;
    uint32_t c = dir, v;
    for (uint32_t n = 0; n <= fx->g->nclusters; n++) {                   /* the last cluster of the directory */
        if (fget(fx, c, &v)) return -EIO_;
        if (!valid_cluster(fx, v) || fatck_is_eoc(fx->ck, v)) break;
        c = v;
    }
    uint32_t nc;
    rc = alloc_cluster(fx, &nc);
    if (rc) return rc;
    rc = zero_cluster(fx, nc);
    if (!rc) rc = fatck_flush(fx->ck);                                   /* the empty cluster is on the disk before the directory is linked to it */
    if (!rc) rc = fset(fx, c, nc);
    if (rc) { fset(fx, nc, 0); return rc; }
    fx->sc.valid = false;
    rc = slot_wr(fx, dir, nslots, ent);
    if (!rc && slot_out) *slot_out = nslots;
    return rc;
}

/* ---------------------------------------------------------------------------------------------- FOUND.000 and lost chains */

static int ensure_found(Fx *fx) {
    if (fx->found_dir) return 0;
    uint32_t root = fx->g->root_dir, nslots;
    int rc = fatck_dir_slots(fx->ck, root, &nslots);
    if (rc) return rc;
    uint8_t e[32];
    for (uint32_t s = 0; s < nslots && !fx->found_dir; s++) {
        if (slot_rd(fx, root, s, e)) return -EIO_;
        if (e[0] == 0x00) break;
        if (e[0] != 0xE5 && (e[11] & 0x3F) != 0x0F && (e[11] & 0x10) && !memcmp(e, "FOUND   000", 11) && valid_cluster(fx, ent_first(fx, e)))
            fx->found_dir = ent_first(fx, e);
    }
    if (!fx->found_dir) {
        uint32_t c;
        rc = alloc_cluster(fx, &c);
        if (rc) return rc;
        rc = zero_cluster(fx, c);
        if (rc) { fset(fx, c, 0); return rc; }
        uint8_t dots[64];
        make_entry(fx, dots, ".          ", 0x10, c, 0);
        make_entry(fx, dots + 32, "..         ", 0x10, 0, 0);
        rc = fatck_write(fx->ck, fatck_cluster_off(fx->ck, c), dots, sizeof dots);
        if (!rc) rc = fatck_flush(fx->ck);                               /* its "." and ".." are on the disk before the root names it */
        if (!rc) {
            make_entry(fx, e, "FOUND   000", 0x10, c, 0);
            rc = dir_add(fx, root, e, NULL);
        }
        if (rc) { fset(fx, c, 0); return rc; }
        fx->found_dir = c;
    }
    fx->chk_next = 0;
    rc = fatck_dir_slots(fx->ck, fx->found_dir, &nslots);
    for (uint32_t s = 0; !rc && s < nslots; s++) {                       /* continue the numbering of an earlier run */
        if (slot_rd(fx, fx->found_dir, s, e)) break;
        if (e[0] == 0x00) break;
        if (e[0] != 0xE5 && !memcmp(e, "FILE", 4) && !memcmp(e + 8, "CHK", 3)) {
            uint32_t n = 0;
            bool ok = true;
            for (int i = 4; i < 8; i++) { if (e[i] < '0' || e[i] > '9') ok = false; n = n * 10 + (uint32_t)(e[i] - '0'); }
            if (ok && n + 1 > fx->chk_next) fx->chk_next = n + 1;
        }
    }
    return 0;
}

static int save_chain(Fx *fx, uint32_t head, uint32_t n) {
    if (fx->o->free_lost) {
        free_from(fx, head);
        fx->st->lost_chains++;
        fx->st->lost_clusters += n;
        return 0;
    }
    int rc = ensure_found(fx);
    if (rc) return rc;
    if (fx->chk_next > 9999) return -ENOSPC_;                            /* FILEnnnn.CHK has four digits: no 10001st name */
    char name[11], shown[16];
    uint32_t k = fx->chk_next++;
    memcpy(name, "FILE0000CHK", 11);
    for (int i = 7; i >= 4; i--) { name[i] = (char)('0' + k % 10); k /= 10; }
    memcpy(shown, name, 8);
    memcpy(shown + 8, ".CHK", 5);
    uint8_t e[32];
    uint64_t size = (uint64_t)n * fx->g->cluster_bytes;
    make_entry(fx, e, name, 0x20, head, size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)size);
    rc = dir_add(fx, fx->found_dir, e, NULL);
    if (rc) return rc;
    fx->st->lost_chains++;
    fx->st->lost_clusters += n;
    SAY(fx, "saved a lost chain of %u cluster(s) as FOUND.000/%s", n, shown);
    return 0;
}

/* Gives every lost chain a file (or frees it). Returns the number of chains handled. */
static int recover_lost(Fx *fx, uint32_t *handled) {
    FatCk *ck = fx->ck;
    *handled = 0;
    if (!allow(fx, FIX_SAFE, fx->o->free_lost ? "free the lost clusters" : "save the lost clusters as files in FOUND.000")) return 0;
    size_t words = ((size_t)fx->maxc + 31) / 32;
    uint32_t *pointed = fatck_scratch(ck, words * 4);
    fx->seen = fatck_scratch(ck, words * 4);
    if (!pointed || !fx->seen) { fatck_scratch_free(ck, pointed); fatck_scratch_free(ck, fx->seen); fx->seen = NULL; return -ENOMEM_; }
    int rc = 0;
    if (!fx->o->free_lost) {                                             /* make FOUND.000 first: the clusters it takes must not look lost */
        rc = ensure_found(fx);
        if (rc) {
            SAYK(fx, FSAY_FAILED, "cannot make FOUND.000 for the lost chains (%s); use -F to free them", rc == -ENOSPC_ ? "no room" : "error");
            fx->st->failed++;
            if (rc == -ENOSPC_) rc = 0;                                  /* not fatal: they stay lost, and the rest of the run goes on */
            goto done;
        }
        for (uint32_t c = fx->found_dir, n = 0; valid_cluster(fx, c) && n <= fx->g->nclusters; n++) {
            bit_set(fx->seen, c);
            uint32_t v;
            if (fget(fx, c, &v) || fatck_is_eoc(ck, v)) break;
            c = v;
        }
    }
    for (uint32_t c = 2; c < fx->maxc; c++) {                            /* which lost clusters are pointed at by another lost cluster */
        uint32_t v;
        if (fget(fx, c, &v) || v == 0 || fatck_is_bad(ck, v) || fatck_cluster_owned(ck, c) || bit_get(fx->seen, c)) continue;
        if (valid_cluster(fx, v) && v != c && !fatck_cluster_owned(ck, v) && !bit_get(fx->seen, v)) bit_set(pointed, v);
    }
    for (int round = 0; round < 2; round++) {                            /* round 0: chains with a head; round 1: what is left (loops) */
        for (uint32_t c = 2; c < fx->maxc; c++) {
            uint32_t v;
            if (fget(fx, c, &v) || v == 0 || fatck_is_bad(ck, v) || fatck_cluster_owned(ck, c) || bit_get(fx->seen, c)) continue;
            if (round == 0 && bit_get(pointed, c)) continue;
            uint32_t n = 0, cur = c, prev = 0;
            for (;;) {                                                   /* walk the chain, claiming it */
                bit_set(fx->seen, cur);
                n++;
                uint32_t nx;
                if (fget(fx, cur, &nx)) { rc = -EIO_; goto done; }
                bool end = fatck_is_eoc(ck, nx);
                if (!end && (!valid_cluster(fx, nx) || nx == 0 || fatck_is_bad(ck, nx) || fatck_is_reserved(ck, nx) || fatck_cluster_owned(ck, nx) || bit_get(fx->seen, nx))) {
                    fset(fx, cur, fatck_eoc(ck));                        /* broken, or runs into something else: end it here */
                    end = true;
                }
                if (end) break;
                prev = cur;
                cur = nx;
            }
            (void)prev;
            if (save_chain(fx, c, n)) {
                SAYK(fx, FSAY_FAILED, "cannot save the lost chain at cluster %u (%u clusters): the directory FOUND.000 is full (10000 files at most) or the volume is; -F frees the rest", c, n);
                fx->st->failed++;
                goto done;
            }
            (*handled)++;
        }
    }
done:
    fatck_scratch_free(ck, pointed);
    fatck_scratch_free(ck, fx->seen);
    fx->seen = NULL;
    return rc;
}

/* ---------------------------------------------------------------------------------------------- names */

static bool sfn_illegal(uint8_t ch, int pos) {
    if (ch < 0x20 && !(pos == 0 && ch == 0x05)) return true;
    return ch == '"' || ch == '*' || ch == '+' || ch == ',' || ch == '/' || ch == ':' || ch == ';' || ch == '<' || ch == '=' || ch == '>' ||
           ch == '?' || ch == '[' || ch == '\\' || ch == ']' || ch == '|' || ch == 0x7F;
}

/* does another live short entry of the directory carry exactly this 8.3 name? */
static bool sfn_taken(Fx *fx, uint32_t dir, const uint8_t sfn[11], uint32_t except) {
    uint32_t nslots;
    if (fatck_dir_slots(fx->ck, dir, &nslots)) return true;
    uint8_t e[32];
    for (uint32_t s = 0; s < nslots; s++) {
        if (slot_rd(fx, dir, s, e)) return true;
        if (e[0] == 0x00) break;
        if (s == except || e[0] == 0xE5 || (e[11] & 0x3F) == 0x0F || (e[11] & 0x08)) continue;
        if (!memcmp(e, sfn, 11)) return true;
    }
    return false;
}

static int rewrite_sfn(Fx *fx, const FatCkFinding *f, uint8_t e[32], uint32_t ss, bool force_unique) {
    uint8_t sfn[11];
    memcpy(sfn, e, 11);
    for (int i = 0; i < 11; i++) if (sfn_illegal(sfn[i], i)) sfn[i] = '_';
    if (sfn[0] == 0x20) sfn[0] = '_';
    if (force_unique || sfn_taken(fx, f->dir, sfn, ss)) {
        uint8_t base[8];
        memcpy(base, sfn, 8);
        int bl = 8;
        while (bl > 1 && base[bl - 1] == ' ') bl--;
        bool found = false;
        for (uint32_t n = 1; n < 1000000 && !found; n++) {
            char tail[8];
            int tl = 0;
            tail[tl++] = '~';
            char digits[8];
            int nd = 0;
            for (uint32_t k = n; k; k /= 10) digits[nd++] = (char)('0' + k % 10);
            while (nd) tail[tl++] = digits[--nd];
            int keep = bl + tl > 8 ? 8 - tl : bl;
            memset(sfn, ' ', 8);
            memcpy(sfn, base, (size_t)keep);
            memcpy(sfn + keep, tail, (size_t)tl);
            if (!sfn_taken(fx, f->dir, sfn, ss)) found = true;
        }
        if (!found) return -ENOSPC_;
    }
    memcpy(e, sfn, 11);
    int rc = slot_wr(fx, f->dir, ss, e);
    if (rc) return rc;
    uint8_t csum = fatck_lfn_checksum(sfn);                              /* the long name has to follow the new alias */
    uint8_t frag[32];
    for (uint32_t s = f->slot; s < ss; s++) {
        if (slot_rd(fx, f->dir, s, frag)) return -EIO_;
        if ((frag[11] & 0x3F) != 0x0F) continue;
        frag[13] = csum;
        rc = slot_wr(fx, f->dir, s, frag);
        if (rc) return rc;
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------------- chain surgery on an entry */

static uint32_t size_for(Fx *fx, uint32_t size, uint32_t clusters) {
    uint64_t cap = (uint64_t)clusters * fx->g->cluster_bytes;
    return (uint64_t)size > cap ? (cap > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)cap) : size;
}

/* Copies the tail that a cross-linked file shares with another chain into fresh clusters. */
static int dup_tail(Fx *fx, const FatCkFinding *f, uint8_t e[32], uint32_t ss) {
    uint32_t cb = fx->g->cluster_bytes, prefix = f->chain_len, at = f->cluster, v;
    uint64_t need = ((uint64_t)f->size + cb - 1) / cb;
    if (need <= prefix) return 1;                                        /* the file needs nothing from the shared part */
    uint32_t m = 0;
    for (uint32_t c = at; valid_cluster(fx, c) && m <= fx->g->nclusters;) {
        m++;
        if (fget(fx, c, &v) || v == 0 || fatck_is_eoc(fx->ck, v) || fatck_is_bad(fx->ck, v) || !valid_cluster(fx, v)) break;
        c = v;
    }
    uint32_t k = (uint64_t)m < need - prefix ? m : (uint32_t)(need - prefix);
    if (!k) return 1;
    if (!fx->buf) { fx->buf = fatck_scratch(fx->ck, cb); if (!fx->buf) return -ENOMEM_; }
    uint32_t first_new = 0, prev = 0, src = at;
    for (uint32_t i = 0; i < k; i++) {
        uint32_t c;
        int rc = alloc_cluster(fx, &c);
        if (rc) { if (first_new) free_from(fx, first_new); return rc; }
        if (prev) fset(fx, prev, c); else first_new = c;
        prev = c;
        if (fatck_read(fx->ck, fatck_cluster_off(fx->ck, src), fx->buf, cb) || fatck_write(fx->ck, fatck_cluster_off(fx->ck, c), fx->buf, cb)) {
            free_from(fx, first_new);
            return -EIO_;
        }
        fx->st->wrote = true;
        if (fget(fx, src, &v)) return -EIO_;
        src = v;
    }
    /* The copy is on the disk before anything points at it: the cache writes its windows in no particular order, and a power cut between the link and
     * the data would leave the file reading what the new clusters held before (the interrupted-repair test, tests/case_fsck.py, caught exactly that). */
    if (fatck_flush(fx->ck)) return -EIO_;
    if (prefix == 0) ent_set_first(fx, e, first_new);
    else if (fset(fx, f->last, first_new)) return -EIO_;
    if (need > (uint64_t)prefix + k) wr32(e + 28, size_for(fx, f->size, prefix + k));
    fx->st->copied_clusters += k;
    return slot_wr(fx, f->dir, ss, e) ? -EIO_ : 0;
}

/* Cuts a broken chain at the break (files: the size follows; directories: what lay behind the cut becomes lost clusters). */
static int cut_chain(Fx *fx, const FatCkFinding *f, uint8_t e[32], uint32_t ss) {
    uint32_t len = f->chain_len, first = f->first, keep_last = f->last, newlen = len;
    bool is_dir = (f->attr & 0x10) != 0;
    if (f->aux == FC_BAD || f->aux == FC_RESERVED) {                     /* the last cluster is itself unusable: stop before it */
        newlen = len ? len - 1 : 0;
        keep_last = newlen ? chain_nth(fx, first, newlen - 1) : 0;
    }
    if (f->code == FK_DIRBIG) {
        newlen = 65536 / (fx->g->cluster_bytes / 32);
        keep_last = chain_nth(fx, first, newlen - 1);
    }
    if (newlen == 0 || !keep_last) {
        if (is_dir) return delete_set(fx, f->dir, f->slot, f->nslots) ? -EIO_ : 0;      /* a directory with no cluster of its own cannot stay */
        ent_set_first(fx, e, 0);
        wr32(e + 28, 0);
        return slot_wr(fx, f->dir, ss, e) ? -EIO_ : 0;
    }
    if (fset(fx, keep_last, fatck_eoc(fx->ck))) return -EIO_;
    fx->sc.valid = false;
    if (!is_dir && size_for(fx, f->size, newlen) != f->size) {
        wr32(e + 28, size_for(fx, f->size, newlen));
        return slot_wr(fx, f->dir, ss, e) ? -EIO_ : 0;
    }
    return 0;
}

/* Does another live entry of the directory carry exactly this entry's name, attributes, first cluster and size? */
static bool exact_duplicate(Fx *fx, uint32_t dir, uint32_t self, const uint8_t ent[32]) {
    uint32_t nslots;
    if (fatck_dir_slots(fx->ck, dir, &nslots)) return false;
    uint8_t e[32];
    for (uint32_t s = 0; s < nslots; s++) {
        if (slot_rd(fx, dir, s, e) || e[0] == 0x00) break;
        if (s == self || e[0] == 0xE5 || (e[11] & 0x3F) == 0x0F) continue;
        if (!memcmp(e, ent, 11) && e[11] == ent[11] && ent_first(fx, e) == ent_first(fx, ent) && rd32(e + 28) == rd32(ent + 28)) return true;
    }
    return false;
}

/* ---------------------------------------------------------------------------------------------- "." and ".." */

enum { SL_FREE, SL_DOT, SL_NEARDOT, SL_OTHER };

/* What sits in slot 0 ("." wanted) or slot 1 (".." wanted) of a subdirectory: nothing (free or the end marker), the right entry, an entry that
 * is evidently that one with a damaged name (starts like it, is a directory), or something else that has no business there. */
static int dot_slot_kind(const uint8_t e[32], bool dotdot) {
    if (e[0] == 0x00 || e[0] == 0xE5) return SL_FREE;
    if ((e[11] & 0x3F) == 0x0F) return SL_OTHER;                         /* a long-name fragment */
    if (!memcmp(e, dotdot ? "..         " : ".          ", 11)) return SL_DOT;
    if ((e[11] & 0x10) && e[0] == '.' && (!dotdot || e[1] == '.')) return SL_NEARDOT;
    return SL_OTHER;
}

/* How many slots the entry set that starts at `slot` spans: long-name fragments up to and including their short entry (1 for a lone entry;
 * fragments that run into free space without a short entry count only themselves, and *has_short says they have none). */
static uint32_t set_extent(Fx *fx, uint32_t dir, uint32_t slot, bool *has_short) {
    uint32_t nslots, n = 0;
    uint8_t e[32];
    *has_short = false;
    if (fatck_dir_slots(fx->ck, dir, &nslots)) return 1;
    for (uint32_t s = slot; s < nslots; s++) {
        if (slot_rd(fx, dir, s, e) || e[0] == 0x00 || e[0] == 0xE5) break;
        n++;
        if ((e[11] & 0x3F) != 0x0F) { *has_short = true; break; }
    }
    return n ? n : 1;
}

/* Puts "." and ".." back at the start of a subdirectory. Slots that are free get the entry; a damaged-looking one is rewritten; an entry that
 * belongs there is kept. An entry that does not belong there (a file's entry, a long-name fragment) is removed -- that costs a name, so it
 * takes -y; its clusters turn into a lost chain and are saved. */
static int restore_dots(Fx *fx, const FatCkFinding *f, const char *text) {
    uint32_t self = f->dir, parent = f->cluster;
    uint8_t e[2][32];
    int kind[2];
    for (int i = 0; i < 2; i++) {
        if (slot_rd(fx, self, (uint32_t)i, e[i])) return 0;
        kind[i] = dot_slot_kind(e[i], i == 1);
    }
    if (kind[0] == SL_DOT && kind[1] == SL_DOT) return 0;               /* (the pointers are the business of the findings for them) */
    int level = FIX_SAFE;                                                /* long-name fragments with no entry behind them cost nothing */
    for (int i = 0; i < 2; i++) {
        bool has_short;
        if (kind[i] == SL_OTHER && (set_extent(fx, self, (uint32_t)i, &has_short), has_short)) level = FIX_ALL;
    }
    if (!allow(fx, level, text)) return 0;
    for (int i = 0; i < 2; i++) {
        if (kind[i] != SL_OTHER) continue;
        bool has_short;
        uint32_t n = set_extent(fx, self, (uint32_t)i, &has_short);
        if (delete_set(fx, self, (uint32_t)i, n)) return -1;
        SAY(fx, "removed %u entr%s that stood where '.' and '..' belong", n, n == 1 ? "y" : "ies");
        kind[i] = SL_FREE;
        if (i == 0) {                                                    /* the set may have covered slot 1 as well */
            if (slot_rd(fx, self, 1, e[1])) return -1;
            kind[1] = dot_slot_kind(e[1], true);
        }
    }
    for (int i = 0; i < 2; i++) {
        if (kind[i] == SL_DOT) continue;
        make_entry(fx, e[i], i ? "..         " : ".          ", 0x10, i ? parent : self, 0);
        if (slot_wr(fx, self, (uint32_t)i, e[i])) return -1;
    }
    SAY(fx, "wrote '.' and '..' where they were missing");
    return 1;
}

/* ---------------------------------------------------------------------------------------------- one repair per finding */

/* Returns 1 when something was repaired, 0 when nothing was done (not allowed, stale, not possible), -1 when it failed. */
static int apply(Fx *fx, const FatCkFinding *f, const char *text) {
    uint8_t e[32];
    uint32_t ss = 0;
    int rc;
    const FatCkGeom *g = fx->g;
    switch (f->code) {
    case FK_FATCOPY: {
        if (!allow(fx, FIX_SAFE, text)) return 0;
        uint32_t copy = f->aux;
        if (copy >= g->nfats || copy == g->active_fat) return 0;
        uint8_t *buf = fatck_scratch(fx->ck, 32768);
        if (!buf) return -1;
        rc = 0;
        for (uint64_t off = 0; off < g->fat_bytes && !rc; off += 32768) {
            size_t n = (size_t)(g->fat_bytes - off < 32768 ? g->fat_bytes - off : 32768);
            rc = fatck_read(fx->ck, g->fat_off[g->active_fat] + off, buf, n);
            if (!rc) rc = fatck_write(fx->ck, g->fat_off[copy] + off, buf, n);
        }
        fatck_scratch_free(fx->ck, buf);
        fx->st->wrote = true;
        if (rc) return -1;
        SAY(fx, "copied FAT %u over FAT %u", g->active_fat + 1, copy + 1);
        return 1;
    }
    case FK_MEDIA: {
        if (!allow(fx, FIX_SAFE, text)) return 0;
        uint32_t v;
        if (fget(fx, 0, &v)) return -1;
        if (fset(fx, 0, (v & ~0xFFu) | g->media)) return -1;
        SAY(fx, "FAT[0] now carries the boot sector's media byte %x", g->media);
        return 1;
    }
    case FK_BACKUP: {
        if (!allow(fx, FIX_SAFE, text) || !g->backup_sector) return 0;
        uint8_t *buf = fatck_scratch(fx->ck, g->bps);
        if (!buf) return -1;
        rc = fatck_read(fx->ck, 0, buf, g->bps);
        if (!rc) { buf[65] = 0; rc = fatck_write(fx->ck, (uint64_t)g->backup_sector * g->bps, buf, g->bps); }
        fatck_scratch_free(fx->ck, buf);
        fx->st->wrote = true;
        if (rc) return -1;
        SAY(fx, "the backup boot sector is a copy of the boot sector again");
        return 1;
    }
    case FK_GARBAGE: {
        /* Where the end marker is NOW, not where the scan saw it: restoring "." and ".." (or any earlier repair) may have moved it, and what
         * the scan called garbage is then a live entry. */
        uint32_t nslots, endm = 0;
        if (fatck_dir_slots(fx->ck, f->dir, &nslots)) return -1;
        for (; endm < nslots; endm++) {
            if (slot_rd(fx, f->dir, endm, e)) return -1;
            if (e[0] == 0x00) break;
        }
        static const uint8_t zero[32];
        bool any = false;
        for (uint32_t s = endm + 1; s < nslots && !any; s++) {
            if (slot_rd(fx, f->dir, s, e)) return -1;
            if (memcmp(e, zero, 32)) any = true;
        }
        if (!any || !allow(fx, FIX_SAFE, text)) return 0;
        uint32_t cleared = 0;
        for (uint32_t s = endm + 1; s < nslots; s++) {
            if (slot_rd(fx, f->dir, s, e)) return -1;
            if (!memcmp(e, zero, 32)) continue;
            if (slot_wr(fx, f->dir, s, zero)) return -1;
            cleared++;
        }
        SAY(fx, "cleared %u leftover slot(s) after the end of a directory", cleared);
        return 1;
    }
    case FK_DIRSIZE:
        if (load_entry(fx, f, e, &ss) || !allow(fx, FIX_SAFE, text)) return 0;
        wr32(e + 28, 0);
        if (slot_wr(fx, f->dir, ss, e)) return -1;
        SAY(fx, "a directory no longer claims a size");
        return 1;
    case FK_LFN: {
        if (!allow(fx, FIX_SAFE, text)) return 0;
        for (uint32_t i = 0; i < f->nslots; i++) {
            if (slot_rd(fx, f->dir, f->slot + i, e)) return -1;
            if ((e[11] & 0x3F) != 0x0F || e[0] == 0xE5 || e[0] == 0x00) continue;
            e[0] = 0xE5;
            if (slot_wr(fx, f->dir, f->slot + i, e)) return -1;
        }
        SAY(fx, "removed %u orphaned long-name entr%s", f->nslots, f->nslots == 1 ? "y" : "ies");
        return 1;
    }
    case FK_LFNFIELD:
        if (slot_rd(fx, f->dir, f->slot, e) || (e[11] & 0x3F) != 0x0F || !allow(fx, FIX_SAFE, text)) return 0;
        e[26] = e[27] = 0;
        if (slot_wr(fx, f->dir, f->slot, e)) return -1;
        SAY(fx, "cleared the cluster field of a long-name entry");
        return 1;
    case FK_LFNPAD:
        if (f->aux != 1) return 0;
        if (slot_rd(fx, f->dir, f->slot, e) || (e[11] & 0x3F) != 0x0F || !allow(fx, FIX_ALL, text)) return 0;
        e[12] = 0;
        if (slot_wr(fx, f->dir, f->slot, e)) return -1;
        SAY(fx, "cleared the type byte of a long-name entry");
        return 1;
    case FK_DOTS: {
        uint32_t self = f->dir;
        if (f->aux == 1) return restore_dots(fx, f, text);               /* "." and ".." are missing (or one of them) */
        if (slot_rd(fx, self, f->slot, e)) return 0;
        bool is_dot = f->aux == 2;
        if (memcmp(e, is_dot ? ".          " : "..         ", 11) || !allow(fx, FIX_SAFE, text)) return 0;
        e[11] |= 0x10;
        ent_set_first(fx, e, f->cluster);
        if (slot_wr(fx, self, f->slot, e)) return -1;
        SAY(fx, "'%s' now points to cluster %u", is_dot ? "." : "..", f->cluster);
        return 1;
    }
    case FK_NOCLUSTER:
        if (load_entry(fx, f, e, &ss)) return 0;
        if (f->attr & 0x10) {
            if (!allow(fx, FIX_ALL, text)) return 0;
            if (delete_set(fx, f->dir, f->slot, f->nslots)) return -1;
            SAY(fx, "removed a directory entry that has no directory behind it");
            return 1;
        }
        if (!allow(fx, FIX_SAFE, text)) return 0;
        wr32(e + 28, 0);
        if (slot_wr(fx, f->dir, ss, e)) return -1;
        SAY(fx, "a file without clusters is empty now");
        return 1;
    case FK_RANGE:
        if (load_entry(fx, f, e, &ss) || !allow(fx, FIX_ALL, text)) return 0;
        if (f->attr & 0x10) {
            if (delete_set(fx, f->dir, f->slot, f->nslots)) return -1;
        } else {
            ent_set_first(fx, e, 0);
            wr32(e + 28, 0);
            if (slot_wr(fx, f->dir, ss, e)) return -1;
        }
        SAY(fx, "an entry pointed outside the volume: %s", (f->attr & 0x10) ? "removed" : "the file is empty now");
        return 1;
    case FK_CROSS: {
        /* A chain that runs into clusters another chain already claimed (or round into its own). Whoever the scan met second is the one that
         * gets flagged, whichever of the two is really wrong, so the repair must not throw either away: a file gets a private copy of what
         * it shares (nothing is lost, the other file keeps the originals), and only what cannot be copied is cut. */
        if (load_entry(fx, f, e, &ss)) return 0;
        bool is_dir = (f->attr & 0x10) != 0;
        const char *done = "cut a broken chain at its break";
        if (exact_duplicate(fx, f->dir, ss, e)) {                        /* the same entry twice: it shares the clusters with its twin, nothing more */
            if (!allow(fx, FIX_SAFE, text)) return 0;
            if (delete_set(fx, f->dir, f->slot, f->nslots)) return -1;
            SAY(fx, "removed an entry that was an exact copy of another one");
            return 1;
        }
        if (!is_dir) {
            uint64_t need = ((uint64_t)f->size + g->cluster_bytes - 1) / g->cluster_bytes;
            if (need <= f->chain_len) {
                /* the file has everything it needs in its own clusters: the link into the other chain is surplus */
                if (!allow(fx, FIX_SAFE, text)) return 0;
                done = f->chain_len ? "the end of a file's chain no longer runs into another chain" : "an empty file no longer points into another file's clusters";
            } else {
                bool own = false;                                        /* does the chain close round on itself? */
                uint32_t c = f->first, v;
                for (uint32_t i = 0; i < f->chain_len && valid_cluster(fx, c); i++) {
                    if (c == f->cluster) { own = true; break; }
                    if (fget(fx, c, &v)) break;
                    c = v;
                }
                if (!own) {
                    if (!allow(fx, FIX_SAFE, text)) return 0;
                    rc = dup_tail(fx, f, e, ss);
                    if (rc == 0) { SAY(fx, "a cross-linked file got its own copy of the shared clusters"); return 1; }
                    if (rc < 0) {
                        SAYK(fx, FSAY_FAILED, "%s the shared clusters (the file is cut instead if -y says so)", rc == -ENOSPC_ ? "no room to copy" : "could not copy");
                        fx->st->failed++;
                    }
                }
                if (!allow(fx, FIX_ALL, text)) return 0;
                done = f->chain_len ? "cut a cross-linked chain where it runs into another" : "a file that only pointed into another file's clusters is empty now";
            }
        } else if (!allow(fx, FIX_ALL, text)) {
            return 0;
        } else {
            done = f->chain_len ? "cut a directory's chain where it runs into another" : "removed a directory entry that only pointed into another chain";
        }
        rc = cut_chain(fx, f, e, ss);
        if (rc) return -1;
        SAY(fx, "%s", done);
        return 1;
    }
    case FK_CHAIN:
    case FK_DIRCHAIN:
    case FK_DIRBIG: {
        bool root_chain = f->nslots == 0 && f->code == FK_DIRCHAIN;
        if (root_chain) {                                                /* the root directory's own chain */
            if (!allow(fx, FIX_ALL, text)) return 0;
            if (f->last && fset(fx, f->last, fatck_eoc(fx->ck))) return -1;
            SAY(fx, "cut the root directory's chain at its break");
            return 1;
        }
        if (load_entry(fx, f, e, &ss)) return 0;
        /* A chain that breaks behind everything its file's size needs is cut without loss: the file is whole, and the clusters beyond the break were
         * surplus (what a trim that a power failure cut short leaves, with its frees written before its end mark). Only a break inside the file costs. */
        int level = FIX_ALL;
        if (f->code == FK_CHAIN && !(f->attr & 0x10)) {
            uint64_t need = ((uint64_t)f->size + g->cluster_bytes - 1) / g->cluster_bytes;
            uint32_t usable = (f->aux == FC_BAD || f->aux == FC_RESERVED) ? (f->chain_len ? f->chain_len - 1 : 0) : f->chain_len;
            if (need <= usable) level = FIX_SAFE;
        }
        if (!allow(fx, level, text)) return 0;
        rc = cut_chain(fx, f, e, ss);
        if (rc) return -1;
        SAY(fx, "cut a broken chain at its break");
        return 1;
    }
    case FK_SHORT:
        if (load_entry(fx, f, e, &ss) || !allow(fx, FIX_SAFE, text)) return 0;
        wr32(e + 28, size_for(fx, f->size, f->chain_len));
        if (slot_wr(fx, f->dir, ss, e)) return -1;
        SAY(fx, "a file no longer claims more bytes than its clusters hold");
        return 1;
    case FK_LONG: {
        if (load_entry(fx, f, e, &ss) || !allow(fx, FIX_SAFE, text)) return 0;
        uint32_t cb = g->cluster_bytes;
        uint64_t need = ((uint64_t)f->size + cb - 1) / cb;
        if (need == 0) {
            ent_set_first(fx, e, 0);
            if (slot_wr(fx, f->dir, ss, e)) return -1;
            free_from(fx, f->first);
        } else {
            uint32_t keep = chain_nth(fx, f->first, (uint32_t)need - 1), v;
            if (!keep || fget(fx, keep, &v)) return -1;
            if (fset(fx, keep, fatck_eoc(fx->ck))) return -1;
            free_from(fx, v);
        }
        SAY(fx, "gave back the clusters behind the end of a file");
        return 1;
    }
    case FK_LABEL:
        if (!allow(fx, FIX_ALL, text)) return 0;
        if (delete_set(fx, f->dir, f->slot, f->nslots)) return -1;
        SAY(fx, "removed a stray volume label entry");
        return 1;
    case FK_NAME:
        if (load_entry(fx, f, e, &ss) || !allow(fx, FIX_ALL, text)) return 0;
        rc = rewrite_sfn(fx, f, e, ss, false);
        if (rc) return -1;
        SAY(fx, "mended an unusable 8.3 name");
        return 1;
    case FK_DUP:
        if (load_entry(fx, f, e, &ss)) return 0;
        if (f->aux == 1 && exact_duplicate(fx, f->dir, ss, e)) {         /* the same entry twice: removing one loses nothing */
            if (!allow(fx, FIX_SAFE, text)) return 0;
            if (delete_set(fx, f->dir, f->slot, f->nslots)) return -1;
            SAY(fx, "removed an entry that was an exact copy of another one");
            return 1;
        }
        if (!allow(fx, FIX_ALL, text)) return 0;
        if (f->aux == 2) {
            rc = rewrite_sfn(fx, f, e, ss, true);
            if (rc) return -1;
            SAY(fx, "gave a duplicate 8.3 name its own alias");
        } else {
            if (delete_set(fx, f->dir, f->slot, f->nslots)) return -1;
            SAY(fx, "removed an entry whose name was taken (its clusters are saved as a lost chain if nothing else holds them)");
        }
        return 1;
    default:
        return 0;
    }
}

/* ---------------------------------------------------------------------------------------------- FSInfo and the flags */

/* May an FSInfo structure be written into this sector? Yes when it is one already (or has lost only some of its signatures) or holds nothing at
 * all. A boot sector whose FSInfo pointer was damaged can name any reserved sector, the backup boot sector or a boot loader's second stage
 * among them, and those are not ours to overwrite. */
static bool fsinfo_sector_usable(const uint8_t b[512]) {
    if (rd32(b) == 0x41615252u || rd32(b + 484) == 0x61417272u) return true;          /* (not the 0xAA55 at the end: every boot sector has that) */
    for (int i = 0; i < 510; i++)
        if (b[i]) return false;
    return true;                                                                        /* empty, apart from a boot signature */
}

static int refresh_fsinfo(Fx *fx, const FatCkResult *r) {
    const FatCkGeom *g = fx->g;
    if (g->bits != 32 || !g->fsinfo_sector || fx->fsinfo_refused) return 0;
    uint8_t b[512];
    uint64_t off = (uint64_t)g->fsinfo_sector * g->bps;
    if (fatck_read(fx->ck, off, b, sizeof b)) return -EIO_;
    if (!fsinfo_sector_usable(b)) {
        SAYK(fx, FSAY_FAILED, "cannot rewrite the FSInfo sector: the boot sector names sector %u for it, and that holds something else (the boot sector itself is not repaired here)", g->fsinfo_sector);
        fx->fsinfo_refused = true;
        fx->st->failed++;
        return 0;
    }
    uint8_t want[512];
    memcpy(want, b, sizeof want);
    uint32_t next = valid_cluster(fx, fx->cursor) ? fx->cursor : 0xFFFFFFFFu;
    wr32(want, 0x41615252u);
    wr32(want + 484, 0x61417272u);
    wr32(want + 488, r->free_clusters);
    if (rd32(b + 492) == 0xFFFFFFFFu || !valid_cluster(fx, rd32(b + 492))) wr32(want + 492, next);
    wr32(want + 508, 0xAA550000u);
    if (!memcmp(b, want, sizeof b)) return 0;
    if (!allow(fx, FIX_SAFE, "rewrite the FSInfo sector (signatures, free count)")) return 0;
    fx->st->wrote = true;
    if (fatck_write(fx->ck, off, want, sizeof want)) return -EIO_;
    SAY(fx, "the FSInfo sector now says %u free clusters", r->free_clusters);
    return 1;
}

/* ---------------------------------------------------------------------------------------------- the passes */

/* The order a pass works in. A repair can make the findings that come after it stale, so the stages that change structure are followed by a
 * new scan before anything else is touched:
 *   0 the FAT copies             (the other repairs read the active FAT; this only makes the copies agree with it)
 *   1 cross-links                (give the second claimant its own copy of what is shared, before anything is written into either of them: a
 *                                 "directory" whose clusters are a file's data would otherwise get its dots and its garbage mended
 *                                 inside the file, and the first owner's surplus is only given back when nobody else reads it)
 *   2 "." and ".."               (decide where a directory's live entries start; the findings about its other slots depend on it)
 *   3 repeated names             (an entry that repeats another goes before the findings about what it names are dealt with)
 *   4 everything else, in the order the scan found it */
static int stage_of(int code) {
    switch (code) {
    case FK_FATCOPY: return 0;
    case FK_CROSS:   return 1;
    case FK_DOTS:    return 2;
    case FK_DUP:     return 3;
    default:         return 4;
    }
}

int fatfix_run(FatCk *ck, const FatFixOpts *o, FatFixStats *st) {
    memset(st, 0, sizeof *st);
    Fx fx;
    memset(&fx, 0, sizeof fx);
    fx.ck = ck;
    fx.o = o;
    fx.st = st;
    fx.g = fatck_geom(ck);
    fx.maxc = fx.g->nclusters + 2;
    fx.cursor = 2;
    uint32_t passes = o->max_passes ? o->max_passes : 16;
    bool can_write = o->mode != FIX_NONE || o->ask;
    int rc = 0;
    bool repaired_last = false;
    for (uint32_t pass = 1;; pass++) {
        st->passes = pass;
        rc = fatck_scan(ck);
        if (rc) return rc;
        repaired_last = false;
        if (!can_write) break;
        const FatCkResult *res = fatck_result(ck);
        const FatCkFinding *list;
        uint32_t n = fatck_findings(ck, &list);
        uint32_t did = 0;
        for (int stage = 0; stage < 5; stage++) {
            uint32_t did_stage = 0;
            if (stage == 4) protect_chains(&fx, list, n);
            for (uint32_t i = 0; i < n; i++) {
                const FatCkFinding *f = &list[i];
                if (!f->fixable || f->code == FK_LOST || f->code == FK_FSINFO || f->code == FK_DIRTY) continue;
                if (stage_of(f->code) != stage) continue;
                int r = apply(&fx, f, fatck_finding_text(ck, i));
                if (r > 0) { did_stage++; st->repaired++; }
                else if (r < 0) st->failed++;
            }
            did += did_stage;
            if (stage >= 1 && stage <= 3 && did_stage) break;            /* the rest of the findings may be stale now: scan again first */
        }
        if (!did && res->lost_clusters) {
            uint32_t handled = 0;
            rc = recover_lost(&fx, &handled);
            if (handled) { did++; st->repaired += handled; }
            if (rc) { if (st->wrote) fatck_flush(ck); return rc; }
        }
        if (!did && !res->incomplete) {                                  /* nothing else changed this pass, so the free count is the final one */
            int w = refresh_fsinfo(&fx, res);
            if (w < 0) { if (st->wrote) fatck_flush(ck); return w; }
            if (w > 0) { did++; st->repaired++; }
        }
        if (st->wrote && fatck_flush(ck)) return -EIO_;
        if (!did) break;
        repaired_last = true;
        if (pass >= passes) break;
    }
    if (repaired_last) {
        rc = fatck_scan(ck);
        if (rc) return rc;
    }
    if (can_write) {
        const FatCkResult *res = fatck_result(ck);
        if (res->errors == 0 && res->leaks == 0 && (res->dirty || res->hard_error) && allow(&fx, FIX_SAFE, "clear the dirty flag")) {
            if (fatck_set_clean(ck, true) == 0 && fatck_flush(ck) == 0) {
                st->flags_cleared = true;
                st->wrote = true;
                SAY(&fx, "the volume is marked clean");
            }
        }
    }
    const FatCkResult *res = fatck_result(ck);
    st->clean_now = res->errors == 0 && res->leaks == 0;
    if (fx.buf) fatck_scratch_free(ck, fx.buf);
    if (fx.prot) fatck_scratch_free(ck, fx.prot);
    return 0;
}

#endif
