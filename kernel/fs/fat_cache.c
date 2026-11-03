/*
 * Write-back page cache for one FAT volume. Pages are 4 KiB slices of the volume (addressed by volume byte offset, so a
 * partition that does not start on a 4 KiB boundary needs no special care). A dirty page remembers its flush class:
 * file data goes to the device first, then the FAT, then directory entries and the boot/FSInfo sectors, with a device
 * cache flush between the classes, so a power cut can leave leaked clusters but not a directory entry that points at
 * data that was never written.
 *
 * Everything here runs with the volume mutex held.
 */
#include "fatfs.h"
#include "irq.h"

#define RA_MAX      32          /* pages per device request (the bounce buffer holds 128 KiB) */
#define RA_DEFAULT  16
#define HASH_BUCKETS 8192

enum { PG_LOAD, PG_ZERO, PG_BLANK };    /* fetch from the device / start zeroed / contents irrelevant (will be overwritten) */

static inline u32 hash_of(const FatVol *v, u64 key) { return (u32)((key * 0x9E3779B97F4A7C15ull) >> 40) & v->hmask; }

int fc_init(FatVol *v) {
    v->hmask = HASH_BUCKETS - 1;
    v->htab = kzalloc(HASH_BUCKETS * sizeof(FatPage *));
    v->bounce = kmalloc((size_t)RA_MAX * FAT_PAGE);
    if (!v->htab || !v->bounce) { kfree(v->htab); kfree(v->bounce); v->htab = NULL; v->bounce = NULL; return -ENOMEM; }
    u64 pages = pmm_total_pages() / 8;
    if (pages > 16384) pages = 16384;
    if (pages < 256) pages = 256;
    v->max_pages = (u32)pages;
    v->dirty_limit = v->max_pages / 4;
    return 0;
}

static FatPage *pg_lookup(FatVol *v, u64 key) {
    for (FatPage *p = v->htab[hash_of(v, key)]; p; p = p->hnext)
        if (p->key == key) return p;
    return NULL;
}

static void lru_unlink(FatVol *v, FatPage *p) {
    if (p->lru_prev) p->lru_prev->lru_next = p->lru_next; else v->lru_head = p->lru_next;
    if (p->lru_next) p->lru_next->lru_prev = p->lru_prev; else v->lru_tail = p->lru_prev;
    p->lru_prev = p->lru_next = NULL;
}

static void lru_push_front(FatVol *v, FatPage *p) {
    p->lru_prev = NULL;
    p->lru_next = v->lru_head;
    if (v->lru_head) v->lru_head->lru_prev = p; else v->lru_tail = p;
    v->lru_head = p;
}

static void pg_free(FatVol *v, FatPage *p) {
    FatPage **pp = &v->htab[hash_of(v, p->key)];
    while (*pp && *pp != p) pp = &(*pp)->hnext;
    if (*pp) *pp = p->hnext;
    lru_unlink(v, p);
    if (p->dirty) v->ndirty--;
    v->npages--;
    kfree(p->data);
    kfree(p);
}

static int flush_all(FatVol *v) { return fc_flush(v); }

/* Drops a batch of clean pages from the cold end; flushes first if everything left is dirty. */
static void evict_some(FatVol *v) {
    u32 want = v->max_pages / 16 + 1, done = 0;
    for (int pass = 0; pass < 2 && !done; pass++) {
        for (FatPage *p = v->lru_tail; p && done < want;) {
            FatPage *prev = p->lru_prev;
            if (!p->dirty) { pg_free(v, p); done++; }
            p = prev;
        }
        if (!done && pass == 0) flush_all(v);
    }
}

static FatPage *pg_alloc(FatVol *v, u64 key) {
    if (v->npages >= v->max_pages) evict_some(v);
    if (v->npages >= v->max_pages * 2) return NULL;             /* the device keeps failing and the cache is all dirty: stop growing */
    FatPage *p = kzalloc(sizeof *p);
    if (p) p->data = kmalloc(FAT_PAGE);
    if (!p || !p->data) {
        kfree(p ? p->data : NULL);
        kfree(p);
        evict_some(v);                                  /* memory pressure: give some back and try once more */
        p = kzalloc(sizeof *p);
        if (!p) return NULL;
        p->data = kmalloc(FAT_PAGE);
        if (!p->data) { kfree(p); return NULL; }
    }
    p->key = key;
    u32 h = hash_of(v, key);
    p->hnext = v->htab[h];
    v->htab[h] = p;
    lru_push_front(v, p);
    v->npages++;
    return p;
}

static void pg_touch(FatVol *v, FatPage *p) {
    if (v->lru_head != p) { lru_unlink(v, p); lru_push_front(v, p); }
}

static void pg_dirty(FatVol *v, FatPage *p, int cls) {
    if (!p->dirty) {
        p->dirty = 1;
        if (v->ndirty++ == 0) v->dirty_since_ms = uptime_ms();
    }
    p->cls = (u8)cls;
}

/* ---------------------------------------------------------------------------------------------- device access */

static int dev_io(FatVol *v, bool write, u64 off, void *buf, size_t n) {
    if (off % v->ds || n % v->ds || !n) return -EINVAL;
    /* blk_write_as: a volume that holds the ESP write grant (fat32.c, -o espgrant) may write its partition although blk.ro is set */
    int rc = write ? blk_write_as(v->dev, v, off / v->ds, (u32)(n / v->ds), buf) : blk_read(v->dev, off / v->ds, (u32)(n / v->ds), buf);
    if (rc) {
        v->io_error = rc;
        if (rc == -ENODEV && v->dev->removed && !v->vanished) {
            v->vanished = true;
            klog("Keine", "%s: %s vanished while it was mounted on %s (a stick pulled out?) -- whatever was not written yet is lost; umount it, and plug the stick in again (or reboot)",
                 v->fstype, v->dev->name, v->mount ? v->mount->target : "?");
        }
    }
    return rc;
}

/* Reads up to `maxpages` consecutive missing pages starting at `key` with one request. */
static int load_run(FatVol *v, u64 key, u32 maxpages) {
    u64 total_pages = (v->vol_bytes + FAT_PAGE - 1) / FAT_PAGE;
    if (key >= total_pages) return -EIO;
    u32 n = 1;
    while (n < maxpages && n < RA_MAX && key + n < total_pages && !pg_lookup(v, key + n)) n++;
    FatPage *pages[RA_MAX];
    for (u32 i = 0; i < n; i++) {                       /* allocate first: allocation may flush, which uses the bounce buffer */
        pages[i] = pg_alloc(v, key + i);
        if (!pages[i]) {
            for (u32 k = 0; k < i; k++) pg_free(v, pages[k]);
            return -ENOMEM;
        }
    }
    u64 off = key * FAT_PAGE;
    size_t bytes = (size_t)MIN((u64)n * FAT_PAGE, v->vol_bytes - off);
    int rc = dev_io(v, false, off, v->bounce, bytes);
    if (rc) {
        for (u32 i = 0; i < n; i++) pg_free(v, pages[i]);
        return rc;
    }
    for (u32 i = 0; i < n; i++) {
        size_t have = bytes > (size_t)i * FAT_PAGE ? bytes - (size_t)i * FAT_PAGE : 0;
        if (have > FAT_PAGE) have = FAT_PAGE;
        memcpy(pages[i]->data, v->bounce + (size_t)i * FAT_PAGE, have);
        if (have < FAT_PAGE) memset(pages[i]->data + have, 0, FAT_PAGE - have);
    }
    return 0;
}

static int fc_page(FatVol *v, u64 key, int mode, u32 ra, FatPage **out) {
    FatPage *p = pg_lookup(v, key);
    if (p) {
        pg_touch(v, p);
        if (mode == PG_ZERO) memset(p->data, 0, FAT_PAGE);
        *out = p;
        return 0;
    }
    if (mode == PG_LOAD) {
        int rc = load_run(v, key, ra);
        if (rc) return rc;
        p = pg_lookup(v, key);
        if (!p) return -EIO;
    } else {
        if (key * FAT_PAGE >= v->vol_bytes) return -EIO;
        p = pg_alloc(v, key);
        if (!p) return -ENOMEM;
        if (mode == PG_ZERO) memset(p->data, 0, FAT_PAGE);
    }
    *out = p;
    return 0;
}

static int check_range(FatVol *v, u64 off, size_t n) {
    if (off > v->vol_bytes || n > v->vol_bytes - off) return -EIO;
    return 0;
}

int fc_read_ra(FatVol *v, u64 off, void *buf, size_t n, u32 ra) {
    int rc = check_range(v, off, n);
    if (rc) return rc;
    u8 *dst = buf;
    while (n) {
        size_t po = (size_t)(off & (FAT_PAGE - 1)), chunk = MIN(n, FAT_PAGE - po);
        FatPage *p;
        rc = fc_page(v, off / FAT_PAGE, PG_LOAD, ra, &p);
        if (rc) return rc;
        memcpy(dst, p->data + po, chunk);
        dst += chunk;
        off += chunk;
        n -= chunk;
    }
    return 0;
}

int fc_read(FatVol *v, u64 off, void *buf, size_t n) { return fc_read_ra(v, off, buf, n, RA_DEFAULT); }

/* Direct pointer into the cached page holding `off`; *avail bytes (up to the page end) may be read from it. The pointer is
 * only valid until the next cache call, so scanning loops use it for a short burst and then ask again. */
const u8 *fc_peek(FatVol *v, u64 off, size_t *avail, int *err) {
    if (off >= v->vol_bytes) { *err = -EIO; return NULL; }
    FatPage *p;
    int rc = fc_page(v, off / FAT_PAGE, PG_LOAD, RA_DEFAULT, &p);        /* scans walk forward through the table */
    if (rc) { *err = rc; return NULL; }
    size_t po = (size_t)(off & (FAT_PAGE - 1));
    *avail = (size_t)MIN((u64)(FAT_PAGE - po), v->vol_bytes - off);
    return p->data + po;
}

int fc_write(FatVol *v, u64 off, const void *buf, size_t n, int cls) {
    int rc = check_range(v, off, n);
    if (rc) return rc;
    const u8 *src = buf;
    while (n) {
        size_t po = (size_t)(off & (FAT_PAGE - 1)), chunk = MIN(n, FAT_PAGE - po);
        FatPage *p;
        rc = fc_page(v, off / FAT_PAGE, chunk == FAT_PAGE ? PG_BLANK : PG_LOAD, 4, &p);
        if (rc) return rc;
        memcpy(p->data + po, src, chunk);
        pg_dirty(v, p, cls);
        src += chunk;
        off += chunk;
        n -= chunk;
    }
    return 0;
}

int fc_zero(FatVol *v, u64 off, size_t n, int cls) {
    int rc = check_range(v, off, n);
    if (rc) return rc;
    while (n) {
        size_t po = (size_t)(off & (FAT_PAGE - 1)), chunk = MIN(n, FAT_PAGE - po);
        FatPage *p;
        rc = fc_page(v, off / FAT_PAGE, chunk == FAT_PAGE ? PG_ZERO : PG_LOAD, 4, &p);
        if (rc) return rc;
        if (chunk != FAT_PAGE) memset(p->data + po, 0, chunk);
        pg_dirty(v, p, cls);
        off += chunk;
        n -= chunk;
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------------- flushing */

static inline u64 skey(const FatPage *p) { return ((u64)p->cls << 56) | p->key; }

static void sift_down(FatPage **a, u32 start, u32 end) {
    u32 root = start;
    while (root * 2 + 1 <= end) {
        u32 child = root * 2 + 1, sw = root;
        if (skey(a[sw]) < skey(a[child])) sw = child;
        if (child + 1 <= end && skey(a[sw]) < skey(a[child + 1])) sw = child + 1;
        if (sw == root) return;
        FatPage *t = a[root]; a[root] = a[sw]; a[sw] = t;
        root = sw;
    }
}

static void heapsort_pages(FatPage **a, u32 n) {
    if (n < 2) return;
    for (u32 start = (n - 2) / 2 + 1; start-- > 0;) sift_down(a, start, n - 1);
    for (u32 end = n - 1; end > 0; end--) {
        FatPage *t = a[0]; a[0] = a[end]; a[end] = t;
        sift_down(a, 0, end - 1);
    }
}

static int write_run(FatVol *v, FatPage **run, u32 n) {
    u64 off = run[0]->key * FAT_PAGE;
    size_t bytes = (size_t)MIN((u64)n * FAT_PAGE, v->vol_bytes - off);
    for (u32 i = 0; i < n; i++) memcpy(v->bounce + (size_t)i * FAT_PAGE, run[i]->data, FAT_PAGE);
    int rc = dev_io(v, true, off, v->bounce, bytes);
    if (rc) return rc;
    for (u32 i = 0; i < n; i++) {
        run[i]->dirty = 0;
        v->ndirty--;
    }
    return 0;
}

/* When memory is too tight to sort the dirty pages: write them one by one, class by class (still correctly ordered). */
static int flush_unsorted(FatVol *v) {
    int rc = 0;
    for (int cls = 0; cls < FPC_COUNT && !rc; cls++) {
        bool wrote = false;
        for (FatPage *p = v->lru_head; p && !rc; p = p->lru_next) {
            if (!p->dirty || p->cls != cls) continue;
            FatPage *one[1] = { p };
            rc = write_run(v, one, 1);
            wrote = true;
        }
        if (!rc && wrote) rc = blk_flush(v->dev);
    }
    if (!rc && !v->ndirty) v->dirty_since_ms = 0;
    return rc;
}

/* Writes every dirty page (data, then FAT, then metadata, with a device flush between the classes). */
int fc_flush(FatVol *v) {
    if (!v->ndirty) return 0;
    if (v->ro) return -EROFS;
    u32 n = v->ndirty, k = 0;
    FatPage **arr = kmalloc((size_t)n * sizeof *arr);
    if (!arr) return flush_unsorted(v);
    for (FatPage *p = v->lru_head; p && k < n; p = p->lru_next)
        if (p->dirty) arr[k++] = p;
    heapsort_pages(arr, k);
    int rc = 0;
    u32 i = 0;
    while (i < k && !rc) {
        u8 cls = arr[i]->cls;
        while (i < k && arr[i]->cls == cls && !rc) {
            u32 j = i;
            while (j + 1 < k && arr[j + 1]->cls == cls && arr[j + 1]->key == arr[j]->key + 1 && j + 1 - i < RA_MAX) j++;
            rc = write_run(v, arr + i, j - i + 1);
            i = j + 1;
        }
        if (!rc) rc = blk_flush(v->dev);
    }
    kfree(arr);
    if (!rc && !v->ndirty) v->dirty_since_ms = 0;
    return rc;
}

void fc_destroy(FatVol *v) {
    while (v->lru_head) {
        FatPage *p = v->lru_head;
        p->dirty = 0;                                   /* the caller flushed what it wanted to keep */
        pg_free(v, p);
    }
    v->ndirty = 0;
    kfree(v->htab);
    kfree(v->bounce);
    v->htab = NULL;
    v->bounce = NULL;
}
