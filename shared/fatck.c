/*
 * fatck: see fatck.h. This file is freestanding on purpose (no libc beyond memcpy/memset/memcmp): the kernel, user space and the
 * host test DLL compile the same source, so a verdict means the same thing everywhere.
 *
 * Layout of the work:
 *   window cache     16 KiB windows over the volume with sequential read-ahead; FAT entries, directories and repairs all go through it
 *   fat access       12/16/32-bit entries, per copy
 *   trace_chain      follows one chain and claims its clusters in a bitmap (a claimed cluster met again = cross-link or loop)
 *   scan_*           directory walk (explicit stack), per-entry checks, lost-cluster pass, FAT copies, FSInfo
 */
#include "fatck.h"
#include <stdarg.h>
#ifdef FATCK_HOST                       /* the test DLL has no C library: tools/fatck_host_rt.c supplies these */
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
#else
#include <string.h>
#endif

#define ENOMEM_ 12
#define EIO_    5
#define EINVAL_ 22
#define ENOSPC_ 28
#define ETIME_  62
#define EFBIG_  27

#define WIN_SHIFT   14
#define WIN_SIZE    (1u << WIN_SHIFT)
#define RA_WINDOWS  4
#define MAX_SLOTS   65536u              /* the FAT directory limit: 2 MiB */
#define MAX_MEM     24

/* ---------------------------------------------------------------------------------------------- small helpers */

static inline uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline bool is_pow2(uint32_t x) { return x && !(x & (x - 1)); }
static inline uint64_t min64(uint64_t a, uint64_t b) { return a < b ? a : b; }

/* ---------------------------------------------------------------------------------------------- tiny formatter
 * %s %u %lu (64-bit) %x %c %d %% and a 0-padded width (%02x). Enough for the messages; the same text in every environment. */

typedef struct { char *p; size_t cap, len; } Out;

static void out_ch(Out *o, char c) { if (o->len + 1 < o->cap) o->p[o->len] = c; o->len++; }
static void out_str(Out *o, const char *s) { if (!s) s = "(null)"; while (*s) out_ch(o, *s++); }

static void out_num(Out *o, uint64_t v, unsigned base, unsigned width, char pad, bool neg) {
    char tmp[24];
    unsigned n = 0;
    do { unsigned d = (unsigned)(v % base); tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= base; } while (v && n < sizeof tmp);
    if (neg) tmp[n++] = '-';
    while (n < width && n < sizeof tmp) tmp[n++] = pad;
    while (n) out_ch(o, tmp[--n]);
}

static void ck_vfmt(char *buf, size_t cap, const char *fmt, va_list ap) {
    Out o = { buf, cap, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { out_ch(&o, *fmt); continue; }
        fmt++;
        char pad = ' ';
        unsigned width = 0;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (unsigned)(*fmt++ - '0');
        bool is_long = false;
        if (*fmt == 'l') { is_long = true; fmt++; if (*fmt == 'l') fmt++; }
        switch (*fmt) {
        case 's': out_str(&o, va_arg(ap, const char *)); break;
        case 'u': out_num(&o, is_long ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned), 10, width, pad, false); break;
        case 'x': out_num(&o, is_long ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned), 16, width, pad, false); break;
        case 'd': {
            long long v = is_long ? va_arg(ap, long long) : va_arg(ap, int);
            out_num(&o, v < 0 ? (uint64_t)(-v) : (uint64_t)v, 10, width, pad, v < 0);
            break;
        }
        case 'c': out_ch(&o, (char)va_arg(ap, int)); break;
        case '%': out_ch(&o, '%'); break;
        case 0: fmt--; break;
        default: out_ch(&o, '%'); out_ch(&o, *fmt); break;
        }
    }
    if (cap) buf[o.len < cap ? o.len : cap - 1] = 0;
}

/* ---------------------------------------------------------------------------------------------- the session */

typedef struct Win { uint64_t base; uint8_t *data; uint32_t len, dlo, dhi; uint8_t valid, dirty; } Win;

typedef struct Frame {
    uint32_t first;         /* the directory's first cluster; 0 for a fixed FAT12/16 root */
    uint32_t dotdot;        /* what ".." must hold: 0 for the root and the root's children, else the parent's first cluster */
    uint32_t nslots, slot;
    uint32_t cur, cidx;     /* cluster that holds `slot` and its index in the chain */
    uint32_t path_len;
    uint32_t nent;          /* short entries seen so far (for the "." / ".." check) */
    uint64_t *tab;
    uint32_t tabmask;
    size_t   arena_mark;
    uint32_t garbage_slot;
    uint8_t  ended, is_root, garbage_done, dots_done;
} Frame;

typedef struct Lfn {
    bool active;
    uint8_t expect, count, csum;
    uint32_t slot;
    uint16_t units[20 * 13 + 1];
    bool pad_bad, type_bad;
} Lfn;

struct FatCk {
    FatCkIo io;
    FatCkGeom g;
    FatCkResult res;
    uint32_t maxc;                  /* nclusters + 2 */
    uint32_t align;
    uint8_t *boot;
    /* window cache */
    Win *win;
    uint32_t nwin;
    uint64_t last_idx;
    uint8_t *stage;
    /* memory accounting */
    struct { void *p; size_t n; } mem[MAX_MEM];
    /* scan state */
    uint32_t *owned;
    size_t owned_bytes;
    uint8_t *cbuf;                  /* one directory cluster */
    uint32_t cbuf_cluster;
    uint8_t *rootbuf;               /* the fixed root directory of FAT12/16 */
    Frame *stack;
    uint32_t depth, max_depth;
    uint8_t *arena;
    size_t arena_size, arena_used;
    char path[1024];
    uint32_t path_len;
    Lfn lfn;
    uint64_t ticks;
    int fatal;
    bool scanned;
    bool fat_dirty_state;           /* dirty flags, for severity decisions */
};

static void *xalloc(FatCk *ck, size_t n) {
    if (!ck->io.alloc || !n) return NULL;
    void *p = ck->io.alloc(ck->io.ctx, n);
    if (!p) return NULL;
    for (int i = 0; i < MAX_MEM; i++)
        if (!ck->mem[i].p) { ck->mem[i].p = p; ck->mem[i].n = n; return p; }
    if (ck->io.release) ck->io.release(ck->io.ctx, p, n);
    return NULL;
}

/* ---------------------------------------------------------------------------------------------- findings */

static const struct { uint8_t sev, fixable; const char *name; } KINDS[FK__COUNT] = {
    [FK_NONE]       = { FS_NOTE, 0, "none" },
    [FK_BPB]        = { FS_ERR,  0, "boot-sector" },
    [FK_SIZE]       = { FS_ERR,  0, "volume-size" },
    [FK_FATCOPY]    = { FS_ERR,  1, "fat-copy" },
    [FK_MEDIA]      = { FS_WARN, 1, "media-byte" },
    [FK_DIRTY]      = { FS_WARN, 1, "dirty-flag" },
    [FK_FSINFO]     = { FS_ERR,  1, "fsinfo" },
    [FK_BACKUP]     = { FS_WARN, 1, "backup-boot" },
    [FK_DIRCHAIN]   = { FS_ERR,  1, "directory-chain" },
    [FK_DIRBIG]     = { FS_ERR,  1, "directory-too-big" },
    [FK_DOTS]       = { FS_ERR,  1, "dot-entries" },
    [FK_GARBAGE]    = { FS_LEAK, 1, "bytes-after-end" },
    [FK_DIRSIZE]    = { FS_ERR,  1, "directory-size" },
    [FK_LFN]        = { FS_ERR,  1, "long-name" },
    [FK_LFNFIELD]   = { FS_ERR,  1, "long-name-field" },
    [FK_LFNPAD]     = { FS_WARN, 1, "long-name-padding" },
    [FK_NAME]       = { FS_ERR,  1, "bad-name" },
    [FK_NAMECASE]   = { FS_WARN, 1, "name-case" },
    [FK_ATTR]       = { FS_WARN, 1, "attributes" },
    [FK_DATE]       = { FS_WARN, 1, "date" },
    [FK_DUP]        = { FS_ERR,  1, "duplicate-name" },
    [FK_LABEL]      = { FS_ERR,  1, "stray-label" },
    [FK_RANGE]      = { FS_ERR,  1, "first-cluster" },
    [FK_NOCLUSTER]  = { FS_ERR,  1, "no-cluster" },
    [FK_CHAIN]      = { FS_ERR,  1, "broken-chain" },
    [FK_CROSS]      = { FS_ERR,  1, "cross-link" },
    [FK_SHORT]      = { FS_ERR,  1, "chain-too-short" },
    [FK_LONG]       = { FS_LEAK, 1, "chain-too-long" },
    [FK_LOST]       = { FS_LEAK, 1, "lost-clusters" },
    [FK_INCOMPLETE] = { FS_ERR,  0, "incomplete" },
    [FK_IO]         = { FS_ERR,  0, "i/o-error" },
};

const char *fatck_code_name(int code) { return code >= 0 && code < FK__COUNT ? KINDS[code].name : "?"; }
const char *fatck_sev_name(int sev) {
    switch (sev) { case FS_ERR: return "error"; case FS_LEAK: return "leftover"; case FS_WARN: return "warning"; default: return "note"; }
}

/* Records a finding and hands it to the caller. `sev` < 0 takes the default for the code. */
static void emit(FatCk *ck, int code, int sev, FatCkFinding *f, const char *fmt, ...) {
    char text[300];
    va_list ap;
    va_start(ap, fmt);
    ck_vfmt(text, sizeof text, fmt, ap);
    va_end(ap);
    if (sev < 0) sev = KINDS[code].sev;
    f->code = (uint16_t)code;
    f->sev = (uint8_t)sev;
    f->fixable = KINDS[code].fixable;
    ck->res.count[code]++;
    switch (sev) {
    case FS_ERR:  ck->res.errors++; break;
    case FS_LEAK: ck->res.leaks++; break;
    case FS_WARN: ck->res.warnings++; break;
    default:      ck->res.notes++; break;
    }
    if (ck->io.report) ck->io.report(ck->io.ctx, f, text);
}

/* A volume-level finding with no entry attached. */
static void emit0(FatCk *ck, int code, int sev, uint32_t cluster, uint32_t aux, const char *fmt, ...) {
    FatCkFinding f;
    memset(&f, 0, sizeof f);
    f.cluster = cluster;
    f.aux = aux;
    char text[300];
    va_list ap;
    va_start(ap, fmt);
    ck_vfmt(text, sizeof text, fmt, ap);
    va_end(ap);
    emit(ck, code, sev, &f, "%s", text);
}

/* ---------------------------------------------------------------------------------------------- the window cache */

static inline uint32_t win_slot(const FatCk *ck, uint64_t idx) {
    return (uint32_t)((idx * 0x9E3779B97F4A7C15ull) >> 40) & (ck->nwin - 1);
}

static int win_writeback(FatCk *ck, Win *w) {
    if (!w->valid || !w->dirty) return 0;
    if (!ck->io.write) return -EIO_;
    uint32_t lo = w->dlo & ~(ck->align - 1), hi = (w->dhi + ck->align - 1) & ~(ck->align - 1);
    if (hi > w->len) hi = w->len;
    int rc = ck->io.write(ck->io.ctx, w->base + lo, w->data + lo, hi - lo);
    if (rc) return rc;
    w->dirty = 0;
    return 0;
}

/* Brings the window with index `idx` into the cache; reads ahead when the access pattern is sequential. */
static Win *win_load(FatCk *ck, uint64_t idx) {
    Win *w = &ck->win[win_slot(ck, idx)];
    if (w->valid && w->base == (idx << WIN_SHIFT)) return w;
    uint32_t n = 1;
    if (idx == ck->last_idx + 1) n = RA_WINDOWS;
    uint64_t base = idx << WIN_SHIFT;
    if (base >= ck->g.vol_bytes) return NULL;
    uint64_t span = min64((uint64_t)n << WIN_SHIFT, ck->g.vol_bytes - base);
    uint64_t rd = (span + ck->align - 1) & ~(uint64_t)(ck->align - 1);
    if (base + rd > ck->io.bytes) rd = ck->io.bytes - base;
    if (ck->io.read(ck->io.ctx, base, ck->stage, (size_t)rd)) { ck->fatal = -EIO_; return NULL; }
    uint32_t count = (uint32_t)((span + WIN_SIZE - 1) >> WIN_SHIFT);
    for (uint32_t i = count; i-- > 0;) {                          /* last first: the window that was asked for wins a slot collision */
        uint64_t b = base + ((uint64_t)i << WIN_SHIFT);
        Win *t = &ck->win[win_slot(ck, idx + i)];
        if (t->valid && t->base == b) continue;                 /* never overwrite a copy we may have modified */
        if (win_writeback(ck, t)) { ck->fatal = -EIO_; return NULL; }
        uint32_t len = (uint32_t)min64(WIN_SIZE, span - ((uint64_t)i << WIN_SHIFT));
        memcpy(t->data, ck->stage + ((size_t)i << WIN_SHIFT), len);
        if (len < WIN_SIZE) memset(t->data + len, 0, WIN_SIZE - len);
        t->base = b;
        t->len = len;
        t->valid = 1;
        t->dirty = 0;
        t->dlo = WIN_SIZE;
        t->dhi = 0;
    }
    ck->last_idx = idx + n - 1;
    w = &ck->win[win_slot(ck, idx)];
    return (w->valid && w->base == base) ? w : NULL;
}

/* Reads n bytes at any volume offset through the cache. */
static int cache_read(FatCk *ck, uint64_t off, void *buf, size_t n) {
    uint8_t *dst = buf;
    if (off > ck->g.vol_bytes || n > ck->g.vol_bytes - off) return -EINVAL_;
    while (n) {
        Win *w = win_load(ck, off >> WIN_SHIFT);
        if (!w) return ck->fatal ? ck->fatal : -EIO_;
        uint32_t in = (uint32_t)(off & (WIN_SIZE - 1));
        size_t take = (size_t)min64(n, WIN_SIZE - in);
        memcpy(dst, w->data + in, take);
        dst += take;
        off += take;
        n -= take;
    }
    return 0;
}

static int cache_write(FatCk *ck, uint64_t off, const void *buf, size_t n) {
    const uint8_t *src = buf;
    if (!ck->io.write) return -EIO_;
    if (off > ck->g.vol_bytes || n > ck->g.vol_bytes - off) return -EINVAL_;
    while (n) {
        Win *w = win_load(ck, off >> WIN_SHIFT);
        if (!w) return ck->fatal ? ck->fatal : -EIO_;
        uint32_t in = (uint32_t)(off & (WIN_SIZE - 1));
        size_t take = (size_t)min64(n, WIN_SIZE - in);
        if (in + take > w->len) return -EINVAL_;
        memcpy(w->data + in, src, take);
        if (!w->dirty) { w->dirty = 1; w->dlo = in; w->dhi = in + (uint32_t)take; }
        else { if (in < w->dlo) w->dlo = in; if (in + take > w->dhi) w->dhi = in + (uint32_t)take; }
        src += take;
        off += take;
        n -= take;
    }
    return 0;
}

int fatck_read(FatCk *ck, uint64_t off, void *buf, size_t n) { return cache_read(ck, off, buf, n); }
int fatck_write(FatCk *ck, uint64_t off, const void *buf, size_t n) { return cache_write(ck, off, buf, n); }

int fatck_flush(FatCk *ck) {
    int rc = 0;
    for (uint32_t i = 0; i < ck->nwin; i++) {
        int r = win_writeback(ck, &ck->win[i]);
        if (r && !rc) rc = r;
    }
    if (!rc && ck->io.flush) rc = ck->io.flush(ck->io.ctx);
    return rc;
}

/* ---------------------------------------------------------------------------------------------- the FAT */

bool fatck_is_eoc(const FatCk *ck, uint32_t v) { return ck->g.bits == 32 ? v >= 0x0FFFFFF8u : ck->g.bits == 16 ? v >= 0xFFF8u : v >= 0xFF8u; }
bool fatck_is_bad(const FatCk *ck, uint32_t v) { return v == (ck->g.bits == 32 ? 0x0FFFFFF7u : ck->g.bits == 16 ? 0xFFF7u : 0xFF7u); }
bool fatck_is_reserved(const FatCk *ck, uint32_t v) {
    if (v == 1) return true;
    if (ck->g.bits == 32) return v >= 0x0FFFFFF0u && v < 0x0FFFFFF7u;
    if (ck->g.bits == 16) return v >= 0xFFF0u && v < 0xFFF7u;
    return v >= 0xFF0u && v < 0xFF7u;
}
uint32_t fatck_eoc(const FatCk *ck) { return ck->g.bits == 32 ? 0x0FFFFFFFu : ck->g.bits == 16 ? 0xFFFFu : 0xFFFu; }

static inline bool valid_cluster(const FatCk *ck, uint32_t c) { return c >= 2 && c < ck->maxc; }

/* Byte offset and width of a FAT entry; FAT12 entries are 12 bits at c * 3 / 2. */
static int fat_entry_get(FatCk *ck, uint32_t copy, uint32_t c, uint32_t *val) {
    uint8_t b[4];
    int rc;
    switch (ck->g.bits) {
    case 32:
        rc = cache_read(ck, ck->g.fat_off[copy] + (uint64_t)c * 4, b, 4);
        if (rc) return rc;
        *val = rd32(b) & 0x0FFFFFFFu;
        return 0;
    case 16:
        rc = cache_read(ck, ck->g.fat_off[copy] + (uint64_t)c * 2, b, 2);
        if (rc) return rc;
        *val = rd16(b);
        return 0;
    default:
        rc = cache_read(ck, ck->g.fat_off[copy] + (uint64_t)c + c / 2, b, 2);
        if (rc) return rc;
        *val = (c & 1) ? (rd16(b) >> 4) : (rd16(b) & 0xFFF);
        return 0;
    }
}

static int fat_entry_set(FatCk *ck, uint32_t copy, uint32_t c, uint32_t val) {
    uint8_t b[4];
    int rc;
    switch (ck->g.bits) {
    case 32: {
        uint64_t off = ck->g.fat_off[copy] + (uint64_t)c * 4;
        rc = cache_read(ck, off, b, 4);
        if (rc) return rc;
        wr32(b, (rd32(b) & 0xF0000000u) | (val & 0x0FFFFFFFu));
        return cache_write(ck, off, b, 4);
    }
    case 16:
        wr16(b, val);
        return cache_write(ck, ck->g.fat_off[copy] + (uint64_t)c * 2, b, 2);
    default: {
        uint64_t off = ck->g.fat_off[copy] + (uint64_t)c + c / 2;
        rc = cache_read(ck, off, b, 2);
        if (rc) return rc;
        uint32_t w = rd16(b);
        w = (c & 1) ? ((w & 0x000F) | ((val & 0xFFF) << 4)) : ((w & 0xF000) | (val & 0xFFF));
        wr16(b, w);
        return cache_write(ck, off, b, 2);
    }
    }
}

int fatck_fat_get(FatCk *ck, uint32_t c, uint32_t *val) {
    if (c >= ck->maxc) return -EINVAL_;
    return fat_entry_get(ck, ck->g.active_fat, c, val);
}
int fatck_fat_get_copy(FatCk *ck, uint32_t copy, uint32_t c, uint32_t *val) {
    if (c >= ck->maxc || copy >= ck->g.nfats) return -EINVAL_;
    return fat_entry_get(ck, copy, c, val);
}
int fatck_fat_set_copy(FatCk *ck, uint32_t copy, uint32_t c, uint32_t val) {
    if (c >= ck->maxc || copy >= ck->g.nfats) return -EINVAL_;
    return fat_entry_set(ck, copy, c, val);
}
int fatck_fat_set(FatCk *ck, uint32_t c, uint32_t val) {
    if (c >= ck->maxc) return -EINVAL_;
    for (uint32_t i = 0; i < ck->g.nfats; i++) {
        if (!ck->g.mirrored && i != ck->g.active_fat) continue;
        int rc = fat_entry_set(ck, i, c, val);
        if (rc) return rc;
    }
    return 0;
}

uint64_t fatck_cluster_off(const FatCk *ck, uint32_t c) { return ck->g.data_off + (uint64_t)(c - 2) * ck->g.cluster_bytes; }

uint8_t fatck_lfn_checksum(const uint8_t sfn[11]) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)((((s & 1) << 7) | (s >> 1)) + sfn[i]);
    return s;
}

/* ---------------------------------------------------------------------------------------------- open */

static bool tick(FatCk *ck) {
    if ((++ck->ticks & 0x1FFF) == 0 && ck->io.now_ms && ck->io.deadline_ms && ck->io.now_ms(ck->io.ctx) > ck->io.deadline_ms) {
        ck->fatal = -ETIME_;
        return true;
    }
    return ck->fatal != 0;
}

int fatck_open(FatCk **out, const FatCkIo *io) {
    *out = NULL;
    if (!io || !io->read || !io->alloc || io->bytes < 512 || !is_pow2(io->align ? io->align : 512)) return -EINVAL_;
    FatCk *ck = io->alloc(io->ctx, sizeof *ck);
    if (!ck) return -ENOMEM_;
    memset(ck, 0, sizeof *ck);
    ck->io = *io;
    ck->align = io->align < 512 ? 512 : io->align;
    uint32_t nw = io->max_windows ? io->max_windows : 16;
    while (nw & (nw - 1)) nw &= nw - 1;                          /* round down to a power of two */
    if (nw < 4) nw = 4;
    ck->nwin = nw;
    ck->max_depth = io->max_depth ? io->max_depth : 256;
    ck->boot = xalloc(ck, ck->align > 512 ? ck->align : 512);
    FatCkGeom *g = &ck->g;
    int rc = 0;
    const char *why = NULL;
    if (!ck->boot) { rc = -ENOMEM_; goto fail; }
    if (io->read(io->ctx, 0, ck->boot, ck->align > 512 ? ck->align : 512)) { rc = -EIO_; why = "the boot sector cannot be read"; goto report; }
    const uint8_t *b = ck->boot;
    if (b[510] != 0x55 || b[511] != 0xAA) { why = "no boot sector signature"; goto report; }
    uint32_t bps = rd16(b + 11), spc = b[13], reserved = rd16(b + 14), nfats = b[16], root_entries = rd16(b + 17);
    uint32_t total16 = rd16(b + 19), fatsz16 = rd16(b + 22), total32 = rd32(b + 32), fatsz32 = rd32(b + 36);
    if (bps < 512 || bps > 4096 || !is_pow2(bps)) { why = "unsupported sector size"; goto report; }
    if (bps < ck->align || bps % ck->align) { why = "the volume's sector size is smaller than the device's"; goto report; }
    if (!is_pow2(spc) || spc > 128) { why = "bad cluster size"; goto report; }
    if (nfats < 1 || nfats > 4) { why = "bad number of FATs"; goto report; }
    if (reserved < 1) { why = "no reserved sectors"; goto report; }
    uint64_t total = total16 ? total16 : total32;
    uint64_t fatsz = fatsz16 ? fatsz16 : fatsz32;
    if (!total || !fatsz) { why = "empty volume or FAT"; goto report; }
    if (total * bps > io->bytes) {
        FatCkFinding f;
        memset(&f, 0, sizeof f);
        emit(ck, FK_SIZE, -1, &f, "volume claims %lu bytes but the device holds only %lu", (unsigned long long)(total * bps), (unsigned long long)io->bytes);
        rc = -EINVAL_;
        goto fail;
    }
    uint64_t root_sectors = ((uint64_t)root_entries * 32 + bps - 1) / bps;
    uint64_t meta = reserved + (uint64_t)nfats * fatsz + root_sectors;
    if (meta >= total) { why = "metadata larger than the volume"; goto report; }
    uint64_t nclusters = (total - meta) / spc;
    if (nclusters < 1) { why = "no data clusters"; goto report; }
    int bits = nclusters < 4085 ? 12 : nclusters < 65525 ? 16 : 32;
    if (bits == 32) {
        if (fatsz16 || root_entries || !fatsz32) { why = "FAT32 geometry is inconsistent"; goto report; }
        if (nclusters > 0x0FFFFFF5u) { why = "too many clusters"; goto report; }
        if (rd16(b + 42)) { why = "unknown FAT32 version"; goto report; }
    } else if (!root_entries || !fatsz16 || (root_entries * 32) % bps) {
        why = "FAT12/16 geometry is inconsistent";
        goto report;
    }
    uint64_t need_fat = bits == 32 ? (nclusters + 2) * 4 : bits == 16 ? (nclusters + 2) * 2 : ((nclusters + 2) * 3 + 1) / 2;
    if (need_fat > fatsz * bps) { why = "the FAT is too small for the cluster count"; goto report; }

    g->bits = bits;
    g->bps = bps;
    g->spc = spc;
    g->cluster_bytes = bps * spc;
    g->reserved = reserved;
    g->nfats = nfats;
    g->fat_sectors = (uint32_t)fatsz;
    g->root_entries = root_entries;
    g->nclusters = (uint32_t)nclusters;
    ck->maxc = g->nclusters + 2;
    g->vol_bytes = total * bps;
    g->fat_bytes = fatsz * bps;
    for (uint32_t i = 0; i < nfats; i++) g->fat_off[i] = ((uint64_t)reserved + (uint64_t)i * fatsz) * bps;
    g->data_off = meta * bps;
    g->media = b[21];
    g->mirrored = true;
    if (bits == 32) {
        g->root_cluster = g->root_dir = rd32(b + 44);
        if (g->root_cluster < 2 || g->root_cluster >= ck->maxc) { why = "root cluster out of range"; goto report; }
        uint32_t ext = rd16(b + 40);
        if (ext & 0x80) { g->mirrored = false; g->active_fat = ext & 0xF; }
        if (g->active_fat >= nfats) { why = "bad active FAT"; goto report; }
        uint32_t fsi = rd16(b + 48), bak = rd16(b + 50);
        g->fsinfo_sector = (fsi >= 1 && fsi < reserved) ? fsi : 0;
        g->backup_sector = (bak >= 1 && bak < reserved) ? bak : 0;
    } else {
        g->root_cluster = 0;
        g->root_dir = 0;
        g->root_off = ((uint64_t)reserved + (uint64_t)nfats * fatsz) * bps;
        g->root_bytes = (uint64_t)root_entries * 32;
    }
    ck->stage = xalloc(ck, (size_t)RA_WINDOWS * WIN_SIZE);
    if (!ck->stage) { rc = -ENOMEM_; goto fail; }
    uint8_t *wdata = NULL;
    while (ck->nwin >= 4 && !(wdata = xalloc(ck, (size_t)ck->nwin * WIN_SIZE))) ck->nwin >>= 1;     /* take what memory allows */
    ck->win = wdata ? xalloc(ck, ck->nwin * sizeof(Win)) : NULL;
    if (!ck->win) { rc = -ENOMEM_; goto fail; }
    for (uint32_t i = 0; i < ck->nwin; i++) ck->win[i].data = wdata + (size_t)i * WIN_SIZE;
    ck->last_idx = ~0ull - 1;
    ck->res.opened = true;
    ck->res.bits = bits;
    ck->res.nclusters = g->nclusters;
    ck->res.cluster_bytes = g->cluster_bytes;
    *out = ck;
    return 0;

report:
    if (why) emit0(ck, FK_BPB, -1, 0, 0, "%s", why);
    if (!rc) rc = -EINVAL_;
fail:
    fatck_close(ck);
    return rc;
}

void fatck_close(FatCk *ck) {
    if (!ck) return;
    FatCkIo io = ck->io;
    for (int i = 0; i < MAX_MEM; i++)
        if (ck->mem[i].p && io.release) io.release(io.ctx, ck->mem[i].p, ck->mem[i].n);
    if (io.release) io.release(io.ctx, ck, sizeof *ck);
}

const FatCkResult *fatck_result(const FatCk *ck) { return &ck->res; }
const FatCkGeom *fatck_geom(const FatCk *ck) { return &ck->g; }

/* ---------------------------------------------------------------------------------------------- directory geometry */

int fatck_dir_slots(FatCk *ck, uint32_t dir, uint32_t *slots) {
    if (dir == 0 && ck->g.bits != 32) { *slots = ck->g.root_entries; return 0; }
    uint32_t c = dir, n = 0, v;
    while (valid_cluster(ck, c) && n <= ck->g.nclusters) {
        n++;
        int rc = fat_entry_get(ck, ck->g.active_fat, c, &v);
        if (rc) return rc;
        if (fatck_is_eoc(ck, v)) break;
        c = v;
        if (!valid_cluster(ck, c)) break;
    }
    uint64_t s = (uint64_t)n * (ck->g.cluster_bytes / 32);
    *slots = s > MAX_SLOTS ? MAX_SLOTS : (uint32_t)s;
    return 0;
}

int fatck_dir_slot_off(FatCk *ck, uint32_t dir, uint32_t slot, uint64_t *off) {
    if (dir == 0 && ck->g.bits != 32) {
        if (slot >= ck->g.root_entries) return -EINVAL_;
        *off = ck->g.root_off + (uint64_t)slot * 32;
        return 0;
    }
    uint32_t per = ck->g.cluster_bytes / 32, idx = slot / per, c = dir, v;
    if (!valid_cluster(ck, c)) return -EINVAL_;
    for (uint32_t i = 0; i < idx; i++) {
        int rc = fat_entry_get(ck, ck->g.active_fat, c, &v);
        if (rc) return rc;
        if (!valid_cluster(ck, v)) return -EINVAL_;
        c = v;
    }
    *off = fatck_cluster_off(ck, c) + (uint64_t)(slot % per) * 32;
    return 0;
}

/* ---------------------------------------------------------------------------------------------- bitmap of claimed clusters */

static inline bool owned_get(const FatCk *ck, uint32_t c) { return (ck->owned[c >> 5] >> (c & 31)) & 1; }
static inline void owned_set(FatCk *ck, uint32_t c) { ck->owned[c >> 5] |= 1u << (c & 31); }

typedef struct { uint32_t len; int stop; uint32_t at; } Trace;

/* Follows a chain from `first`, claiming every cluster. stop = 0: ended with an end-of-chain mark; else FC_* and `at` is the cluster
 * that could not be used (it is not claimed, and not counted in len). */
static void trace_chain(FatCk *ck, uint32_t first, Trace *t) {
    t->len = 0;
    t->stop = 0;
    t->at = 0;
    uint32_t c = first;
    for (;;) {
        if (!valid_cluster(ck, c)) { t->stop = FC_RANGE; t->at = c; return; }
        if (owned_get(ck, c)) { t->stop = FC_LOOP; t->at = c; return; }
        owned_set(ck, c);
        t->len++;
        uint32_t nxt;
        if (fat_entry_get(ck, ck->g.active_fat, c, &nxt)) { t->stop = FC_RANGE; t->at = c; ck->fatal = ck->fatal ? ck->fatal : -EIO_; return; }
        if (fatck_is_eoc(ck, nxt)) return;
        if (nxt == 0) { t->stop = FC_FREE; t->at = c; return; }
        if (fatck_is_bad(ck, nxt)) { t->stop = FC_BAD; t->at = c; return; }
        if (fatck_is_reserved(ck, nxt)) { t->stop = FC_RESERVED; t->at = c; return; }
        c = nxt;
        if (tick(ck)) { t->stop = FC_RANGE; t->at = c; return; }
    }
}

static const char *stop_text(int stop) {
    switch (stop) {
    case FC_RANGE:    return "runs outside the volume";
    case FC_FREE:     return "runs into a free cluster";
    case FC_BAD:      return "runs into a bad cluster";
    case FC_RESERVED: return "runs into a reserved FAT value";
    case FC_LOOP:     return "is cross-linked or loops back (the cluster is already part of a chain)";
    default:          return "ends badly";
    }
}

/* ---------------------------------------------------------------------------------------------- names */

static int utf16_to_utf8(const uint16_t *u, int n, char *out, int cap) {
    int o = 0;
    for (int i = 0; i < n; i++) {
        uint32_t cp = u[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x20 || cp == 0x7F) cp = '?';
        if (cp < 0x80) { if (o + 1 >= cap) break; out[o++] = (char)cp; }
        else if (cp < 0x800) { if (o + 2 >= cap) break; out[o++] = (char)(0xC0 | cp >> 6); out[o++] = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { if (o + 3 >= cap) break; out[o++] = (char)(0xE0 | cp >> 12); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
        else { if (o + 4 >= cap) break; out[o++] = (char)(0xF0 | cp >> 18); out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
    }
    out[o] = 0;
    return o;
}

/* "BASE.EXT" of a short entry, with the NT lower-case flags applied; bytes above 0x7F show as '?'. Returns the length. */
static int short_name(const uint8_t sfn[11], uint8_t ntres, char *out, bool want_lower_flags) {
    int o = 0;
    uint8_t base[8];
    memcpy(base, sfn, 8);
    if (base[0] == 0x05) base[0] = 0xE5;
    int bl = 8;
    while (bl > 0 && base[bl - 1] == ' ') bl--;
    int el = 3;
    while (el > 0 && sfn[8 + el - 1] == ' ') el--;
    for (int i = 0; i < bl; i++) {
        uint8_t ch = base[i];
        if (ch < 0x20 || ch >= 0x7F) ch = '?';
        if (want_lower_flags && (ntres & 0x08) && ch >= 'A' && ch <= 'Z') ch = (uint8_t)(ch + 32);
        out[o++] = (char)ch;
    }
    if (el) {
        out[o++] = '.';
        for (int i = 0; i < el; i++) {
            uint8_t ch = sfn[8 + i];
            if (ch < 0x20 || ch >= 0x7F) ch = '?';
            if (want_lower_flags && (ntres & 0x10) && ch >= 'A' && ch <= 'Z') ch = (uint8_t)(ch + 32);
            out[o++] = (char)ch;
        }
    }
    out[o] = 0;
    return o;
}

static uint64_t fnv64(uint64_t h, uint32_t v) {
    h ^= v;
    return h * 0x100000001B3ull;
}

/* ---------------------------------------------------------------------------------------------- the duplicate-name table */

static void *arena_get(FatCk *ck, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!ck->arena || ck->arena_used + n > ck->arena_size) return NULL;
    void *p = ck->arena + ck->arena_used;
    ck->arena_used += n;
    memset(p, 0, n);
    return p;
}

static void frame_table(FatCk *ck, Frame *f) {
    uint32_t want = 64;
    while (want < f->nslots * 3 && want < (1u << 20)) want <<= 1;
    f->tab = arena_get(ck, (size_t)want * 8);
    f->tabmask = f->tab ? want - 1 : 0;
}

/* true when the hash was already there */
static bool tab_seen(Frame *f, uint64_t h) {
    if (!f->tab) return false;
    if (!h) h = 1;
    uint32_t i = (uint32_t)(h ^ (h >> 29)) & f->tabmask;
    for (uint32_t probes = 0; probes <= f->tabmask; probes++) {
        if (!f->tab[i]) { f->tab[i] = h; return false; }
        if (f->tab[i] == h) return true;
        i = (i + 1) & f->tabmask;
    }
    return false;
}

/* ---------------------------------------------------------------------------------------------- the directory walk */

static inline const char *sfn_label(const uint8_t *e, char *buf) { short_name(e, e[12], buf, true); return buf; }

static bool valid_dos_date(uint32_t date, uint32_t tm) {
    if (date == 0) return true;                                         /* "not set" is fine */
    uint32_t year = (date >> 9) + 1980, mon = (date >> 5) & 15, day = date & 31;
    uint32_t hour = tm >> 11, minute = (tm >> 5) & 63, sec = (tm & 31) * 2;
    static const uint8_t dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (mon < 1 || mon > 12 || day < 1) return false;
    uint32_t d = dim[mon - 1];
    if (mon == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) d = 29;
    return day <= d && hour <= 23 && minute <= 59 && sec <= 59;
}

static void lfn_reset(FatCk *ck) { ck->lfn.active = false; ck->lfn.pad_bad = ck->lfn.type_bad = false; }

/* Reports a long-name set that was left without its short entry. */
static void lfn_orphan(FatCk *ck, Frame *f) {
    if (!ck->lfn.active) return;
    FatCkFinding fi;
    memset(&fi, 0, sizeof fi);
    fi.dir = f->first;
    fi.slot = ck->lfn.slot;
    fi.nslots = 1;
    emit(ck, FK_LFN, -1, &fi, "%s: LFN fragment at slot %u is not followed by a matching entry", ck->path_len ? ck->path : "/", ck->lfn.slot);
    lfn_reset(ck);
}

static int frame_push(FatCk *ck, uint32_t first, uint32_t dotdot, uint32_t nslots, uint32_t path_len, bool is_root) {
    if (ck->depth >= ck->max_depth) {
        FatCkFinding fi;
        memset(&fi, 0, sizeof fi);
        fi.dir = first;
        emit(ck, FK_INCOMPLETE, -1, &fi, "directories nested more than %u deep: the scan stops descending here", ck->max_depth);
        ck->res.incomplete = true;
        return -EFBIG_;
    }
    Frame *f = &ck->stack[ck->depth++];
    memset(f, 0, sizeof *f);
    f->first = first;
    f->dotdot = dotdot;
    f->nslots = nslots;
    f->cur = first;
    f->path_len = path_len;
    f->is_root = is_root;
    f->arena_mark = ck->arena_used;
    frame_table(ck, f);
    if (ck->depth > ck->res.max_depth) ck->res.max_depth = ck->depth;
    ck->res.dirs++;
    return 0;
}

/* Reads the cluster (or fixed-root region) that holds `slot` of the frame's directory; returns a pointer to the 32 bytes of the slot. */
static const uint8_t *frame_slot(FatCk *ck, Frame *f, uint32_t slot) {
    if (f->first == 0 && ck->g.bits != 32) return ck->rootbuf + (size_t)slot * 32;
    uint32_t per = ck->g.cluster_bytes / 32, idx = slot / per;
    while (f->cidx < idx) {
        uint32_t v;
        if (fat_entry_get(ck, ck->g.active_fat, f->cur, &v) || !valid_cluster(ck, v)) { ck->fatal = ck->fatal ? ck->fatal : -EIO_; return NULL; }
        f->cur = v;
        f->cidx++;
    }
    if (ck->cbuf_cluster != f->cur) {
        if (cache_read(ck, fatck_cluster_off(ck, f->cur), ck->cbuf, ck->g.cluster_bytes)) { ck->fatal = ck->fatal ? ck->fatal : -EIO_; return NULL; }
        ck->cbuf_cluster = f->cur;
    }
    return ck->cbuf + (size_t)(slot % per) * 32;
}

/* Checks one short entry and, for a directory with a usable chain, pushes it. */
static void check_short_entry(FatCk *ck, Frame *f, const uint8_t *e, uint32_t slot, uint32_t nslots, const uint16_t *lfn_units, int lfn_len) {
    char sname[16], lname[300], esc[320];
    uint8_t attr = e[11];
    short_name(e, e[12], sname, true);
    const char *disp = sname;
    bool has_lfn = lfn_units != NULL;
    if (has_lfn) { utf16_to_utf8(lfn_units, lfn_len, lname, sizeof lname); disp = lname; }
    esc[0] = 0;
    {
        const char *dp = ck->path_len ? ck->path : "";
        uint32_t i = 0;
        for (; dp[i] && i < 200; i++) esc[i] = dp[i];
        esc[i++] = '/';
        for (const char *s = disp; *s && i < sizeof esc - 1; s++) esc[i++] = *s;
        esc[i] = 0;
    }
    uint32_t first = rd16(e + 26) | (ck->g.bits == 32 ? rd16(e + 20) << 16 : 0);
    uint32_t size = rd32(e + 28);
    bool is_dir = (attr & 0x10) != 0;
    bool is_label = (attr & 0x08) != 0 && !has_lfn;
    FatCkFinding fi;
    memset(&fi, 0, sizeof fi);
    fi.dir = f->first;
    fi.slot = slot;
    fi.nslots = nslots;
    fi.first = first;
    fi.size = size;
    fi.attr = attr;

    /* name, attribute and date checks (volume labels have none of them) */
    if (!is_label && !(attr & 0x08)) {
        if (e[0] == 0x20) emit(ck, FK_NAME, -1, &fi, "%s: short name starts with a space", esc);
        for (int i = 0; i < 11; i++) {
            uint8_t ch = e[i];
            if (ch < 0x20 && !(ch == 0x05 && i == 0)) { emit(ck, FK_NAME, -1, &fi, "%s: control character in the short name", esc); break; }
            if (ch == '"' || ch == '*' || ch == '+' || ch == ',' || ch == '/' || ch == ':' || ch == ';' || ch == '<' || ch == '=' ||
                ch == '>' || ch == '?' || ch == '[' || ch == '\\' || ch == ']' || ch == '|' || ch == 0x7F) {
                emit(ck, FK_NAME, -1, &fi, "%s: illegal character '%c' in the short name", esc, ch);
                break;
            }
        }
        for (int i = 0; i < 11; i++)
            if (e[i] >= 'a' && e[i] <= 'z') { emit(ck, FK_NAMECASE, -1, &fi, "%s: lower-case letters in the short name (should use the NT case flags)", esc); break; }
        if (e[12] & ~0x18) emit(ck, FK_NAMECASE, -1, &fi, "%s: unknown NT reserved bits %x", esc, e[12]);
        if (attr & 0xC0) emit(ck, FK_ATTR, -1, &fi, "%s: reserved attribute bits %x", esc, attr);
        if (!valid_dos_date(rd16(e + 16), rd16(e + 14))) emit(ck, FK_DATE, -1, &fi, "%s: invalid creation date", esc);
        if (!valid_dos_date(rd16(e + 24), rd16(e + 22))) emit(ck, FK_DATE, -1, &fi, "%s: invalid modification date", esc);
    }
    if (is_label) {
        if (!f->is_root) emit(ck, FK_LABEL, -1, &fi, "%s: volume label entry outside the root directory", ck->path_len ? ck->path : "/");
        f->nent++;
        return;
    }

    /* "." and ".." of a subdirectory */
    bool dot = !memcmp(e, ".          ", 11), dotdot = !memcmp(e, "..         ", 11);
    if (!f->is_root) {
        if (f->nent == 0 || f->nent == 1) {
            const char *want = f->nent == 0 ? "." : "..";
            bool ok = f->nent == 0 ? dot : dotdot;
            if (!ok) {
                if (!f->dots_done) { f->dots_done = true; emit(ck, FK_DOTS, -1, &fi, "%s: missing '.' / '..' entries", ck->path_len ? ck->path : "/"); }
            } else {
                uint32_t exp = f->nent == 0 ? f->first : f->dotdot;
                if (first != exp) { fi.aux = f->nent == 0 ? 2 : 3; fi.cluster = exp; emit(ck, FK_DOTS, -1, &fi, "%s: '%s' points to cluster %u, expected %u", ck->path_len ? ck->path : "/", want, first, exp); }
                if (!is_dir) { fi.aux = f->nent == 0 ? 2 : 3; emit(ck, FK_DOTS, -1, &fi, "%s: '.'/'..' without the directory attribute", ck->path_len ? ck->path : "/"); }
            }
        }
        f->nent++;
        if (dot || dotdot) return;                                       /* not a child */
    } else {
        f->nent++;
    }

    /* duplicates */
    {
        uint64_t h = 0xcbf29ce484222325ull, hs = 0x84222325cbf29ce4ull;
        if (has_lfn) {
            for (int i = 0; i < lfn_len; i++) { uint32_t u = lfn_units[i]; if (u >= 'A' && u <= 'Z') u += 32; h = fnv64(h, u); }
        } else {
            for (const char *s = sname; *s; s++) { uint32_t u = (uint8_t)*s; if (u >= 'A' && u <= 'Z') u += 32; h = fnv64(h, u); }
        }
        for (int i = 0; i < 11; i++) hs = fnv64(hs, e[i]);
        hs ^= 0xA5A5A5A5A5A5A5A5ull;
        bool dup_name = tab_seen(f, h), dup_sfn = tab_seen(f, hs);
        if (dup_name) emit(ck, FK_DUP, -1, &fi, "%s: duplicate name '%s' (slot %u)", ck->path_len ? ck->path : "/", disp, slot);
        if (dup_sfn) emit(ck, FK_DUP, -1, &fi, "%s: duplicate short name '%s'", ck->path_len ? ck->path : "/", sname);
    }

    Trace t;
    if (is_dir) {
        if (size != 0) emit(ck, FK_DIRSIZE, -1, &fi, "%s: directory with non-zero size %u", esc, size);
        if (first < 2) { emit(ck, FK_NOCLUSTER, -1, &fi, "%s: directory without a first cluster", esc); return; }
        trace_chain(ck, first, &t);
        fi.chain_len = t.len;
        if (t.stop) {
            fi.cluster = t.at;
            fi.aux = (uint32_t)t.stop;
            if (t.len == 0 && t.at == first) emit(ck, t.stop == FC_LOOP ? FK_CROSS : FK_RANGE, -1, &fi, "%s: cluster %u %s", esc, t.at, stop_text(t.stop));
            else emit(ck, t.stop == FC_LOOP ? FK_CROSS : FK_DIRCHAIN, -1, &fi, "%s: directory chain %s (cluster %u, after %u cluster(s))", esc, stop_text(t.stop), t.at, t.len);
        }
        if (t.len == 0) return;
        uint64_t slots = (uint64_t)t.len * (ck->g.cluster_bytes / 32);
        if (slots > MAX_SLOTS) {
            emit(ck, FK_DIRBIG, -1, &fi, "%s: directory holds %u clusters, more than the 65536 slots FAT allows", esc, t.len);
            slots = MAX_SLOTS;
        }
        /* descend: the frame is pushed by the caller loop through ck->pending */
        uint32_t plen = 0;
        {
            const char *dp = ck->path_len ? ck->path : "";
            uint32_t i = 0;
            for (; dp[i] && i < sizeof ck->path - 2; i++) ck->path[i] = dp[i];
            ck->path[i++] = '/';
            for (const char *s = disp; *s && i < sizeof ck->path - 1; s++) ck->path[i++] = *s;
            ck->path[i] = 0;
            plen = i;
        }
        /* the parent's path is restored by the caller when it resumes */
        if (frame_push(ck, first, f->is_root ? 0 : f->first, (uint32_t)slots, plen, false) == 0) {
            ck->path_len = plen;
            ck->lfn.active = false;
        } else {
            ck->path[f->path_len] = 0;
        }
        return;
    }
    ck->res.files++;
    ck->res.file_bytes += size;
    if (first == 0) {
        if (size) emit(ck, FK_NOCLUSTER, -1, &fi, "%s: size %u but no clusters", esc, size);
        return;
    }
    trace_chain(ck, first, &t);
    fi.chain_len = t.len;
    uint64_t need = ((uint64_t)size + ck->g.cluster_bytes - 1) / ck->g.cluster_bytes;
    if (t.stop) {
        fi.cluster = t.at;
        fi.aux = (uint32_t)t.stop;
        if (t.len == 0 && t.at == first && t.stop != FC_LOOP) emit(ck, FK_RANGE, -1, &fi, "%s: cluster %u is out of range", esc, t.at);
        else emit(ck, t.stop == FC_LOOP ? FK_CROSS : FK_CHAIN, -1, &fi, "%s: chain %s (cluster %u, after %u cluster(s))", esc, stop_text(t.stop), t.at, t.len);
        return;
    }
    if (size == 0) { emit(ck, FK_LONG, FS_LEAK, &fi, "%s: empty file owns %u cluster(s)", esc, t.len); return; }
    if (t.len < need) emit(ck, FK_SHORT, -1, &fi, "%s: size %u needs %lu cluster(s) but the chain has %u", esc, size, (unsigned long long)need, t.len);
    else if (t.len > need) emit(ck, FK_LONG, -1, &fi, "%s: size %u needs %lu cluster(s) but the chain has %u", esc, size, (unsigned long long)need, t.len);
}

/* Parses the slots of the top frame until it has to descend or is done. */
static void walk_frame(FatCk *ck, Frame *f) {
    char pathbuf[2];
    (void)pathbuf;
    ck->path_len = f->path_len;
    ck->path[ck->path_len] = 0;
    lfn_reset(ck);
    uint32_t depth_before = ck->depth;
    while (f->slot < f->nslots) {
        if (tick(ck)) return;
        uint32_t slot = f->slot++;
        const uint8_t *e = frame_slot(ck, f, slot);
        if (!e) return;
        if (f->ended) {
            if (!f->garbage_done) {
                for (int i = 0; i < 32; i++)
                    if (e[i]) {
                        FatCkFinding fi;
                        memset(&fi, 0, sizeof fi);
                        fi.dir = f->first;
                        fi.slot = slot;
                        fi.nslots = 1;
                        emit(ck, FK_GARBAGE, -1, &fi, "%s: non-zero slot %u after the end-of-directory marker", ck->path_len ? ck->path : "/", slot);
                        f->garbage_done = true;
                        break;
                    }
            }
            continue;
        }
        uint8_t first = e[0], attr = e[11];
        if (first == 0x00) { f->ended = true; lfn_orphan(ck, f); continue; }
        if (first == 0xE5) { lfn_orphan(ck, f); continue; }
        if ((attr & 0x3F) == 0x0F) {                                     /* a long-name fragment */
            Lfn *l = &ck->lfn;
            FatCkFinding fi;
            memset(&fi, 0, sizeof fi);
            fi.dir = f->first;
            fi.slot = slot;
            fi.nslots = 1;
            const char *dp = ck->path_len ? ck->path : "/";
            if (first & 0x40) {
                lfn_orphan(ck, f);
                uint32_t cnt = first & 0x3F;
                if (cnt == 0 || cnt > 20) {
                    emit(ck, FK_LFN, -1, &fi, "%s: bad LFN sequence number %x at slot %u", dp, first, slot);
                    continue;
                }
                l->active = true;
                l->expect = l->count = (uint8_t)cnt;
                l->csum = e[13];
                l->slot = slot;
                l->pad_bad = l->type_bad = false;
                memset(l->units, 0xFF, sizeof l->units);
            } else if (!l->active || (first & 0x3F) != l->expect || e[13] != l->csum) {
                emit(ck, FK_LFN, -1, &fi, "%s: out-of-order or orphan LFN entry (seq %x) at slot %u", dp, first, slot);
                lfn_reset(ck);
                continue;
            }
            if (e[12] != 0 && !l->type_bad) { l->type_bad = true; emit(ck, FK_LFNPAD, -1, &fi, "%s: LFN entry at slot %u has a non-zero type byte", dp, slot); }
            if (e[26] | e[27]) emit(ck, FK_LFNFIELD, -1, &fi, "%s: LFN entry at slot %u has a non-zero first-cluster field", dp, slot);
            uint16_t *u = l->units + (size_t)(l->expect - 1) * 13;
            for (int i = 0; i < 5; i++) u[i] = (uint16_t)rd16(e + 1 + 2 * i);
            for (int i = 0; i < 6; i++) u[5 + i] = (uint16_t)rd16(e + 14 + 2 * i);
            for (int i = 0; i < 2; i++) u[11 + i] = (uint16_t)rd16(e + 28 + 2 * i);
            l->expect--;
            continue;
        }
        /* a short entry, possibly preceded by long-name fragments */
        const uint16_t *units = NULL;
        int ulen = 0;
        uint32_t nslots = 1;
        if (ck->lfn.active) {
            Lfn *l = &ck->lfn;
            if (l->expect == 0 && l->csum == fatck_lfn_checksum(e)) {
                int n = l->count * 13, term = -1;
                for (int i = 0; i < n; i++) if (l->units[i] == 0) { term = i; break; }
                ulen = term >= 0 ? term : n;
                if (term >= 0) {
                    for (int i = term + 1; i < n; i++)
                        if (l->units[i] != 0xFFFF && !l->pad_bad) {
                            l->pad_bad = true;
                            char sn[16];
                            FatCkFinding fi;
                            memset(&fi, 0, sizeof fi);
                            fi.dir = f->first;
                            fi.slot = slot;
                            fi.nslots = 1;
                            emit(ck, FK_LFNPAD, -1, &fi, "%s: LFN padding of %s is not 0xFFFF", ck->path_len ? ck->path : "/", sfn_label(e, sn));
                        }
                }
                units = l->units;
                nslots = 1 + l->count;
            } else {
                char sn[16];
                FatCkFinding fi;
                memset(&fi, 0, sizeof fi);
                fi.dir = f->first;
                fi.slot = l->slot;
                fi.nslots = slot - l->slot;
                emit(ck, FK_LFN, -1, &fi, "%s: LFN entries before '%s' do not match it (checksum or count)", ck->path_len ? ck->path : "/", sfn_label(e, sn));
            }
            lfn_reset(ck);
        }
        uint32_t d0 = ck->depth;
        check_short_entry(ck, f, e, slot - (nslots - 1), nslots, units, ulen);
        if (ck->depth > d0) return;                                      /* a child frame was pushed: resume this one later */
        if (ck->fatal) return;
    }
    /* end of the directory */
    lfn_orphan(ck, f);
    if (!f->is_root && f->nent < 2 && !f->dots_done) {
        f->dots_done = true;
        FatCkFinding fi;
        memset(&fi, 0, sizeof fi);
        fi.dir = f->first;
        fi.aux = 1;
        emit(ck, FK_DOTS, -1, &fi, "%s: missing '.' / '..' entries", ck->path_len ? ck->path : "/");
    }
    (void)depth_before;
}

/* ---------------------------------------------------------------------------------------------- the whole scan */

/* Reads the clean-shutdown bit and the boot sector flag byte; sets res.dirty / res.hard_error. f0 gets FAT[0]. */
static int read_flags(FatCk *ck, uint32_t *f0_out, bool *clean_out, bool *boot_dirty_out) {
    const FatCkGeom *g = &ck->g;
    uint32_t f1 = 0, f0 = 0;
    bool boot_dirty = (ck->boot[g->bits == 32 ? 65 : 37] & 1) != 0;
    bool clean = true, no_err = true;
    if (g->bits != 12) {
        if (fat_entry_get(ck, g->active_fat, 0, &f0) || fat_entry_get(ck, g->active_fat, 1, &f1)) return ck->fatal ? ck->fatal : -EIO_;
        if (g->bits == 32) { clean = (f1 & 0x08000000u) != 0; no_err = (f1 & 0x04000000u) != 0; }
        else { clean = (f1 & 0x8000u) != 0; no_err = (f1 & 0x4000u) != 0; }
    } else {
        if (fat_entry_get(ck, g->active_fat, 0, &f0)) return ck->fatal ? ck->fatal : -EIO_;
        no_err = (ck->boot[37] & 2) == 0;
    }
    ck->res.dirty = !clean || boot_dirty;
    ck->res.hard_error = !no_err;
    ck->fat_dirty_state = ck->res.dirty;
    *f0_out = f0;
    *clean_out = clean;
    *boot_dirty_out = boot_dirty;
    return 0;
}

int fatck_quick_state(FatCk *ck, bool *dirty, bool *hard_error) {
    uint32_t f0;
    bool clean, boot_dirty;
    int rc = read_flags(ck, &f0, &clean, &boot_dirty);
    if (rc) return rc;
    *dirty = ck->res.dirty;
    *hard_error = ck->res.hard_error;
    return 0;
}

static int check_flags(FatCk *ck) {
    const FatCkGeom *g = &ck->g;
    uint32_t f0 = 0;
    bool clean = true, boot_dirty = false;
    int rc = read_flags(ck, &f0, &clean, &boot_dirty);
    if (rc) return rc;
    bool no_err = !ck->res.hard_error;
    FatCkFinding fi;
    memset(&fi, 0, sizeof fi);
    if (g->bits != 12 && !clean) emit(ck, FK_DIRTY, -1, &fi, "volume is marked dirty (FAT[1] clean-shutdown bit is clear)");
    if (boot_dirty) emit(ck, FK_DIRTY, -1, &fi, "volume is marked dirty (boot sector flag byte, bit 0)");
    if (!no_err) emit(ck, FK_DIRTY, -1, &fi, "volume is flagged with a hard error");
    if ((f0 & 0xFF) != g->media) emit(ck, FK_MEDIA, -1, &fi, "FAT[0] media byte %x differs from the boot sector's %x", f0 & 0xFF, g->media);
    return 0;
}

static int check_fat_copies(FatCk *ck) {
    const FatCkGeom *g = &ck->g;
    if (g->nfats < 2 || !g->mirrored) return 0;
    for (uint32_t i = 0; i < g->nfats; i++) {
        if (i == g->active_fat) continue;
        uint32_t diffs = 0, firstd = 0;
        for (uint32_t c = 2; c < ck->maxc; c++) {
            uint32_t a, b;
            if (fat_entry_get(ck, g->active_fat, c, &a) || fat_entry_get(ck, i, c, &b)) return ck->fatal ? ck->fatal : -EIO_;
            if (a != b) { if (!diffs) firstd = c; diffs++; }
            if (tick(ck)) return ck->fatal;
        }
        ck->res.fat_diff[i] = diffs;
        if (diffs) {
            FatCkFinding fi;
            memset(&fi, 0, sizeof fi);
            fi.cluster = firstd;
            fi.aux = i;
            fi.size = diffs;
            uint32_t a = 0, b = 0;
            fat_entry_get(ck, g->active_fat, firstd, &a);
            fat_entry_get(ck, i, firstd, &b);
            emit(ck, FK_FATCOPY, ck->res.dirty ? FS_LEAK : FS_ERR, &fi, "FAT copy %u differs from copy %u in %u entr%s (first difference at cluster %u: %x vs %x)",
                 i + 1, g->active_fat + 1, diffs, diffs == 1 ? "y" : "ies", firstd, b, a);
        }
    }
    return 0;
}

static void check_fsinfo(FatCk *ck, uint32_t free_clusters) {
    const FatCkGeom *g = &ck->g;
    FatCkFinding fi;
    memset(&fi, 0, sizeof fi);
    if (g->bits != 32) return;
    if (!g->fsinfo_sector) { emit(ck, FK_FSINFO, FS_WARN, &fi, "no FSInfo sector"); return; }
    uint8_t b[512];
    if (cache_read(ck, (uint64_t)g->fsinfo_sector * g->bps, b, sizeof b)) { ck->fatal = ck->fatal ? ck->fatal : -EIO_; return; }
    if (rd32(b) != 0x41615252u || rd32(b + 484) != 0x61417272u || rd32(b + 508) != 0xAA550000u) {
        emit(ck, FK_FSINFO, FS_ERR, &fi, "FSInfo signatures are wrong");
        return;
    }
    uint32_t cnt = rd32(b + 488), nxt = rd32(b + 492);
    if (cnt != 0xFFFFFFFFu && cnt != free_clusters) {
        fi.aux = cnt;
        emit(ck, FK_FSINFO, ck->res.dirty ? FS_LEAK : FS_ERR, &fi, "FSInfo free count %u but the FAT has %u free clusters", cnt, free_clusters);
    }
    if (nxt != 0xFFFFFFFFu && !(nxt >= 2 && nxt < ck->maxc)) emit(ck, FK_FSINFO, FS_ERR, &fi, "FSInfo next-free hint %u is out of range", nxt);
    if (g->backup_sector) {
        uint8_t bk[512];
        if (cache_read(ck, (uint64_t)g->backup_sector * g->bps, bk, sizeof bk) == 0) {
            uint8_t m[90], k[90];
            memcpy(m, ck->boot, 90);
            memcpy(k, bk, 90);
            m[65] = k[65] = 0;                                           /* the dirty flag byte is not mirrored */
            if (memcmp(m, k, 90)) emit(ck, FK_BACKUP, FS_WARN, &fi, "backup boot sector differs from the main boot sector");
        }
    }
}

int fatck_scan(FatCk *ck) {
    if (!ck || !ck->res.opened || ck->scanned) return -EINVAL_;
    ck->scanned = true;
    const FatCkGeom *g = &ck->g;
    size_t words = ((size_t)ck->maxc + 31) / 32;
    ck->owned_bytes = words * 4;
    ck->owned = xalloc(ck, ck->owned_bytes);
    ck->cbuf = xalloc(ck, g->cluster_bytes);
    ck->stack = xalloc(ck, (size_t)ck->max_depth * sizeof(Frame));
    ck->arena_size = ck->io.arena_bytes ? ck->io.arena_bytes : (1u << 20);
    ck->arena = xalloc(ck, ck->arena_size);                              /* may be NULL: duplicate checks are skipped then */
    if (g->bits != 32) ck->rootbuf = xalloc(ck, (size_t)((g->root_bytes + ck->align - 1) & ~(uint64_t)(ck->align - 1)));
    if (!ck->owned || !ck->cbuf || !ck->stack || (g->bits != 32 && !ck->rootbuf)) {
        ck->res.incomplete = true;
        emit0(ck, FK_INCOMPLETE, -1, 0, 0, "not enough memory to check this volume (%u clusters)", g->nclusters);
        return -ENOMEM_;
    }
    ck->cbuf_cluster = 0;
    int rc = check_flags(ck);
    if (!rc) rc = check_fat_copies(ck);
    if (rc) goto stop;

    /* the root directory */
    {
        uint32_t slots;
        if (g->bits == 32) {
            Trace t;
            trace_chain(ck, g->root_cluster, &t);
            if (t.stop) {
                FatCkFinding fi;
                memset(&fi, 0, sizeof fi);
                fi.dir = g->root_cluster;
                fi.cluster = t.at;
                fi.aux = (uint32_t)t.stop;
                fi.chain_len = t.len;
                emit(ck, FK_DIRCHAIN, -1, &fi, "/: the root directory chain %s (cluster %u, after %u cluster(s))", stop_text(t.stop), t.at, t.len);
            }
            uint64_t s = (uint64_t)t.len * (g->cluster_bytes / 32);
            if (s > MAX_SLOTS) {
                emit0(ck, FK_DIRBIG, -1, 0, 0, "/: the root directory holds more than the 65536 slots FAT allows");
                s = MAX_SLOTS;
            }
            slots = (uint32_t)s;
        } else {
            slots = g->root_entries;
            if (cache_read(ck, g->root_off, ck->rootbuf, (size_t)g->root_bytes)) { rc = ck->fatal ? ck->fatal : -EIO_; goto stop; }
        }
        if (slots && frame_push(ck, g->root_dir, 0, slots, 0, true) == 0) {
            ck->res.dirs--;                                              /* the root is not counted as a directory of the tree */
            ck->path_len = 0;
            while (ck->depth && !ck->fatal) {
                Frame *f = &ck->stack[ck->depth - 1];
                if (f->slot < f->nslots) {
                    uint32_t d0 = ck->depth;
                    walk_frame(ck, f);
                    if (ck->depth > d0) continue;                        /* descend */
                    if (ck->fatal) break;
                    if (f->slot < f->nslots) break;                      /* stopped early */
                }
                ck->arena_used = f->arena_mark;                          /* pop */
                ck->depth--;
                if (ck->depth) {
                    Frame *p = &ck->stack[ck->depth - 1];
                    ck->path_len = p->path_len;
                    ck->path[ck->path_len] = 0;
                    lfn_reset(ck);
                }
            }
        }
    }
    if (ck->fatal) { rc = ck->fatal; goto stop; }

    /* the whole FAT once: free, bad and lost clusters */
    {
        uint32_t free_n = 0, bad = 0, lost = 0, used = 0;
        for (uint32_t c = 2; c < ck->maxc; c++) {
            uint32_t v;
            if (fat_entry_get(ck, g->active_fat, c, &v)) { rc = ck->fatal ? ck->fatal : -EIO_; goto stop; }
            if (v == 0) free_n++;
            else if (fatck_is_bad(ck, v)) bad++;
            else if (!owned_get(ck, c)) { if (lost < 8) ck->res.first_lost[lost] = c; lost++; }
            else used++;
            if (tick(ck)) { rc = ck->fatal; goto stop; }
        }
        ck->res.free_clusters = free_n;
        ck->res.bad_clusters = bad;
        ck->res.lost_clusters = lost;
        ck->res.used = used + lost;
        if (lost) {
            FatCkFinding fi;
            memset(&fi, 0, sizeof fi);
            fi.cluster = ck->res.first_lost[0];
            fi.aux = lost;
            char list[120];
            Out o = { list, sizeof list, 0 };
            for (uint32_t i = 0; i < lost && i < 8; i++) { if (i) out_str(&o, ", "); out_num(&o, ck->res.first_lost[i], 10, 0, ' ', false); }
            list[o.len < sizeof list ? o.len : sizeof list - 1] = 0;
            emit(ck, FK_LOST, -1, &fi, "%u lost cluster(s) (allocated in the FAT but part of no file), first: %s", lost, list);
        }
        check_fsinfo(ck, free_n);
    }
    if (ck->fatal) rc = ck->fatal;
stop:
    if (rc) {
        ck->res.incomplete = true;
        if (rc == -ETIME_) emit0(ck, FK_INCOMPLETE, -1, 0, 0, "the check ran out of time before it was done");
        else emit0(ck, FK_IO, -1, 0, 0, "the device failed while the volume was being read (%d)", rc);
        return rc;
    }
    if (ck->res.incomplete) return -EFBIG_;
    return 0;
}

/* ---------------------------------------------------------------------------------------------- flags */

int fatck_set_clean(FatCk *ck, bool clean) {
    const FatCkGeom *g = &ck->g;
    int rc;
    if (g->bits != 12) {
        for (uint32_t i = 0; i < g->nfats; i++) {
            uint32_t v;
            rc = fat_entry_get(ck, i, 1, &v);
            if (rc) return rc;
            if (g->bits == 32) v = clean ? (v | 0x0C000000u) : (v & ~0x08000000u);
            else v = clean ? (v | 0xC000u) : (v & ~0x8000u);
            if (g->bits == 32) {
                uint8_t b[4];
                uint64_t off = g->fat_off[i] + 4;
                rc = cache_read(ck, off, b, 4);
                if (rc) return rc;
                wr32(b, (rd32(b) & 0xF0000000u) | (v & 0x0FFFFFFFu));
                rc = cache_write(ck, off, b, 4);
            } else {
                uint8_t b[2];
                wr16(b, v);
                rc = cache_write(ck, g->fat_off[i] + 2, b, 2);
            }
            if (rc) return rc;
        }
    }
    uint32_t boff = g->bits == 32 ? 65 : 37;
    uint8_t flag = ck->boot[boff];
    flag = clean ? (uint8_t)(flag & ~3) : (uint8_t)(flag | 1);
    rc = cache_write(ck, boff, &flag, 1);
    if (rc) return rc;
    ck->boot[boff] = flag;
    return 0;
}
