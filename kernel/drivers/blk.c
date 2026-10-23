/* Block device registry: /dev nodes, partition scanning (GPT and MBR), byte-granular access. */
#include "blk.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"

const u8 GPT_GUID_EFI_SYSTEM[16]   = { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b };
const u8 GPT_GUID_MS_BASIC_DATA[16] = { 0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44, 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 };
const u8 GPT_GUID_LINUX_FS[16]     = { 0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47, 0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 };

static BlockDev *g_head, *g_tail;
static int g_next_index;
static Mutex g_lock;
static bool g_lock_ready;

static void lock_init_once(void) {
    if (!g_lock_ready) { mutex_init(&g_lock); g_lock_ready = true; }
}

BlockDev *blk_first(void) { return g_head; }
BlockDev *blk_next(BlockDev *d) { return d->next; }

BlockDev *blk_find(const char *name) {
    if (!strncmp(name, "/dev/", 5)) name += 5;
    for (BlockDev *d = g_head; d; d = d->next) if (!strcmp(d->name, name)) return d;
    return NULL;
}

BlockDev *blk_find_label(const char *label) {
    for (BlockDev *d = g_head; d; d = d->next) if (d->label[0] && !strcasecmp(d->label, label)) return d;
    return NULL;
}

/* ---------------------------------------------------------------- checked access */

int blk_read(BlockDev *d, u64 lba, u32 count, void *buf) {
    if (!count) return 0;
    if (d->removed) return -ENODEV;
    if (lba >= d->sectors || lba + count > d->sectors) return -EINVAL;
    return d->read ? d->read(d, lba, count, buf) : -ENODEV;
}

int blk_write(BlockDev *d, u64 lba, u32 count, const void *buf) {
    if (!count) return 0;
    if (d->removed) return -ENODEV;
    if (d->readonly) return -EROFS;
    if (lba >= d->sectors || lba + count > d->sectors) return -EINVAL;
    return d->write ? d->write(d, lba, count, buf) : -ENODEV;
}

int blk_flush(BlockDev *d) {
    if (d->removed) return -ENODEV;
    return d->flush ? d->flush(d) : 0;
}

/* Partitions are thin wrappers that offset into their parent. */
static int part_read(BlockDev *d, u64 lba, u32 count, void *buf) { return blk_read(d->parent, d->part_start + lba, count, buf); }
static int part_write(BlockDev *d, u64 lba, u32 count, const void *buf) { return blk_write(d->parent, d->part_start + lba, count, buf); }
static int part_flush(BlockDev *d) { return blk_flush(d->parent); }

/* ---------------------------------------------------------------- /dev nodes */

#define IO_CHUNK (64 * 1024)

static ssize_t blkdev_io(BlockDev *d, void *buf, size_t n, u64 off, bool write) {
    u64 cap = d->sectors * d->sector_size;
    if (off >= cap) return write ? -ENOSPC : 0;
    if (off + n > cap) n = (size_t)(cap - off);
    if (write && d->readonly) return -EROFS;
    u32 ss = d->sector_size;
    u8 *bounce = kmalloc(IO_CHUNK + ss);
    if (!bounce) return -ENOMEM;
    size_t done = 0;
    int rc = 0;
    while (done < n && !rc) {
        u64 pos = off + done;
        u64 lba = pos / ss;
        u32 head = (u32)(pos % ss);
        size_t take = MIN(n - done, (size_t)IO_CHUNK - head);
        u32 count = (u32)((head + take + ss - 1) / ss);
        if (!write) {
            rc = blk_read(d, lba, count, bounce);
            if (!rc) memcpy((u8 *)buf + done, bounce + head, take);
        } else {
            bool partial = head || ((head + take) % ss);
            if (partial) rc = blk_read(d, lba, count, bounce);              /* read-modify-write at the edges */
            if (!rc) {
                memcpy(bounce + head, (const u8 *)buf + done, take);
                rc = blk_write(d, lba, count, bounce);
            }
        }
        if (!rc) done += take;
    }
    kfree(bounce);
    return done ? (ssize_t)done : rc;
}

static ssize_t blk_node_read(VNode *v, File *f, void *buf, size_t n, u64 off) { (void)f; return blkdev_io(v->priv, buf, n, off, false); }
static ssize_t blk_node_write(VNode *v, File *f, const void *buf, size_t n, u64 off) { (void)f; return blkdev_io(v->priv, (void *)buf, n, off, true); }

#define BLKGETSIZE64 0x80081272
#define BLKSSZGET    0x1268
#define BLKFLSBUF    0x1261

static int blk_node_ioctl(VNode *v, File *f, unsigned long req, void *arg) {
    (void)f;
    BlockDev *d = v->priv;
    switch (req) {
    case BLKGETSIZE64: *(u64 *)arg = d->sectors * d->sector_size; return 0;
    case BLKSSZGET: *(int *)arg = (int)d->sector_size; return 0;
    case BLKFLSBUF: return blk_flush(d);
    default: return -ENOTTY;
    }
}

static const VNodeOps blk_node_ops = { .read = blk_node_read, .write = blk_node_write, .ioctl = blk_node_ioctl };

static void make_node(BlockDev *d) {
    char path[32];
    snprintf(path, sizeof path, "%s", d->name);
    VNode *v = NULL;
    u64 rdev = (u64)(8 + (d->parent ? 0 : 0)) << 8 | (u64)d->dev_index;
    if (!devfs_root()) return;
    VNode *dir = devfs_root();
    vnode_get(dir);
    int rc = tmpfs_add_special(dir, path, S_IFBLK | 0600, &blk_node_ops, d, rdev, &v);
    vnode_put(dir);
    if (rc == 0) {
        v->size = d->sectors * d->sector_size;
        vnode_put(v);
    }
}

/* ---------------------------------------------------------------- partitions */

typedef struct PACKED { u8 sig[8]; u32 rev, hdr_size, hdr_crc, reserved; u64 my_lba, alt_lba, first_usable, last_usable; u8 guid[16];
                        u64 entries_lba; u32 nentries, entry_size, entries_crc; } GptHeader;
typedef struct PACKED { u8 type[16], guid[16]; u64 first, last, attrs; u16 name[36]; } GptEntry;
typedef struct PACKED { u8 status, chs1[3], type, chs2[3]; u32 lba, count; } MbrEntry;

static BlockDev *add_partition(BlockDev *disk, int index, u64 first, u64 count, const u8 *type_guid, u8 mbr_type, const char *label) {
    BlockDev *p = kzalloc(sizeof *p);
    if (!p) return NULL;
    size_t nl = strlen(disk->name);
    bool digit_end = nl && disk->name[nl - 1] >= '0' && disk->name[nl - 1] <= '9';
    snprintf(p->name, sizeof p->name, "%s%s%d", disk->name, digit_end ? "p" : "", index);
    p->driver = disk->driver;
    p->sector_size = disk->sector_size;
    p->sectors = count;
    p->readonly = disk->readonly;
    p->read = part_read;
    p->write = part_write;
    p->flush = part_flush;
    p->parent = disk;
    p->part_start = first;
    p->part_index = index;
    p->part_type_mbr = mbr_type;
    if (type_guid) memcpy(p->part_type_guid, type_guid, 16);
    strlcpy(p->label, label, sizeof p->label);
    p->dev_index = g_next_index++;
    g_tail->next = p;
    g_tail = p;
    make_node(p);
    klog("Marisa", "blk: %s  %lu MiB  start %lu%s%s", p->name, (unsigned long)(count * p->sector_size >> 20), (unsigned long)first,
         label[0] ? "  label " : "", label);
    return p;
}

static bool scan_gpt(BlockDev *d) {
    u8 *buf = kmalloc(d->sector_size);
    if (!buf) return false;
    bool found = false;
    if (blk_read(d, 1, 1, buf) == 0 && !memcmp(buf, "EFI PART", 8)) {
        GptHeader h;
        memcpy(&h, buf, sizeof h);
        if (h.entry_size >= sizeof(GptEntry) && h.entry_size <= 1024 && h.nentries > 0 && h.nentries <= 256) {
            size_t bytes = (size_t)h.nentries * h.entry_size;
            u32 sectors = (u32)((bytes + d->sector_size - 1) / d->sector_size);
            u8 *ents = kmalloc((size_t)sectors * d->sector_size);
            if (ents && blk_read(d, h.entries_lba, sectors, ents) == 0) {
                found = true;
                for (u32 i = 0; i < h.nentries; i++) {
                    GptEntry e;
                    memcpy(&e, ents + (size_t)i * h.entry_size, sizeof e);
                    bool empty = true;
                    for (int k = 0; k < 16; k++) if (e.type[k]) empty = false;
                    if (empty || e.last < e.first || e.last >= d->sectors) continue;
                    char label[40];
                    size_t n = 0;
                    for (; n < 36 && e.name[n] && n < sizeof label - 1; n++) label[n] = e.name[n] < 128 ? (char)e.name[n] : '?';
                    label[n] = 0;
                    add_partition(d, (int)i + 1, e.first, e.last - e.first + 1, e.type, 0, label);
                }
            }
            kfree(ents);
        }
    }
    kfree(buf);
    return found;
}

static void scan_mbr(BlockDev *d) {
    u8 *buf = kmalloc(d->sector_size);
    if (!buf) return;
    if (blk_read(d, 0, 1, buf) == 0 && buf[510] == 0x55 && buf[511] == 0xAA) {
        for (int i = 0; i < 4; i++) {
            MbrEntry e;
            memcpy(&e, buf + 446 + i * 16, sizeof e);
            if (!e.type || e.type == 0x05 || e.type == 0x0F || e.type == 0xEE || !e.count) continue;
            if ((u64)e.lba + e.count > d->sectors) continue;
            add_partition(d, i + 1, e.lba, e.count, NULL, e.type, "");
        }
    }
    kfree(buf);
}

static int register_locked(BlockDev *d) {
    d->dev_index = g_next_index++;
    d->removed = false;
    d->next = NULL;
    if (g_tail) g_tail->next = d; else g_head = d;
    g_tail = d;
    make_node(d);
    klog("Marisa", "blk: %s  %s  %lu MiB, %u-byte sectors%s%s%s", d->name, d->driver, (unsigned long)(d->sectors * d->sector_size >> 20), d->sector_size,
         d->readonly ? ", read-only" : "", d->model[0] ? "  " : "", d->model);
    if (!d->parent && !scan_gpt(d)) scan_mbr(d);
    return 0;
}

int blk_register(BlockDev *d) {
    lock_init_once();
    mutex_lock(&g_lock);
    int rc = register_locked(d);
    mutex_unlock(&g_lock);
    return rc;
}

/* "sda" .. "sdz", "sdaa" .. : the Linux scheme for the n-th SCSI-style disk. */
static void sd_name_of(char *out, size_t cap, int index) {
    char letters[8];
    int n = 0;
    for (int i = index; n < 6; i = i / 26 - 1) {
        letters[n++] = (char)('a' + i % 26);
        if (i < 26) break;
    }
    size_t o = 0;
    if (cap < 4) return;
    out[o++] = 's'; out[o++] = 'd';
    while (n && o + 1 < cap) out[o++] = letters[--n];
    out[o] = 0;
}

/* The first name of sda, sdb, ... that no registered disk uses. SATA ports (boot time) and USB sticks (whenever they show up) draw from
 * the same pool, so a stick plugged in after boot never collides with a disk and a removed stick gives its letter back. */
int blk_register_sd(BlockDev *d) {
    lock_init_once();
    mutex_lock(&g_lock);
    for (int i = 0; i < 18278; i++) {                      /* sda .. zzz */
        sd_name_of(d->name, sizeof d->name, i);
        if (!blk_find(d->name)) break;
    }
    int rc = register_locked(d);
    mutex_unlock(&g_lock);
    return rc;
}

void blk_unregister(BlockDev *d) {
    if (!d || d->removed) return;
    lock_init_once();
    mutex_lock(&g_lock);
    d->removed = true;
    char names[80] = "";
    BlockDev **pp = &g_head;
    BlockDev *last = NULL;
    while (*pp) {
        BlockDev *x = *pp;
        if (x == d || x->parent == d) {
            *pp = x->next;                                 /* x->next stays as it was, so a reader standing on x walks on safely */
            x->removed = true;
            devfs_unregister(x->name);
            if (x != d && strlen(names) + strlen(x->name) + 2 < sizeof names) {
                if (names[0]) strlcat(names, ", ", sizeof names);
                strlcat(names, x->name, sizeof names);
            }
            continue;
        }
        last = x;
        pp = &x->next;
    }
    g_tail = last;
    mutex_unlock(&g_lock);
    klog("Marisa", "blk: %s removed%s%s%s", d->name, names[0] ? " together with " : "", names, names[0] ? " (the device is gone; requests now fail with ENODEV)" : "");
}

size_t blk_describe(char *buf, size_t cap) {
    size_t n = 0;
    for (BlockDev *d = g_head; d && n + 200 < cap; d = d->next)
        n += (size_t)snprintf(buf + n, cap - n, "%s %lu %u %d %s %lu %s %s %s\n", d->name, (unsigned long)(d->sectors * d->sector_size), d->sector_size,
                              d->readonly, d->parent ? d->parent->name : "-", (unsigned long)d->part_start, d->label[0] ? d->label : "-",
                              d->driver ? d->driver : "-", d->model[0] ? d->model : "-");
    return n;
}
