/*
 * What the installer needs from the block layer, kept out of blk.c so that file stays small:
 *
 *  - the per-disk write unlock (BLKWRUNLOCK, shared/blkabi.h): root lifts `blk.ro` for one whole disk, for writes through one open file
 *    only, until that file is closed. Partition nodes, other opens, mounted filesystems and every other disk keep getting EROFS.
 *  - `blk.rw=GUID[,GUID]` on the command line: the GPT partitions whose own (unique) partition GUID is listed stay writable although
 *    `blk.ro` is set. Only those partitions: the rest of their disk, the whole-disk node and every other disk stay read-only. The installer
 *    writes the GUID of the installed HUMANVILLAGE partition into the cmdline.txt of the system it installs, so the homes are writable
 *    while the installed ESP (loader, kernel, initrd, cmdline.txt) and a Windows disk next to it stay read-only.
 *  - /proc/bootdisk: the partition the loader was read from (BootInfo.boot_part) and the block devices that match it.
 */
#include "blk.h"
#include "kernel.h"
#include "cmdline.h"
#include "cpu.h"
#include "process.h"
#include "initcall.h"
#include "blkabi.h"

#define MAX_UNLOCKS 4

static struct { File *f; BlockDev *d; } g_unlocks[MAX_UNLOCKS];

bool blk_unlock_allows(BlockDev *d, File *f) {
    bool yes = false;
    u64 fl = irq_save();
    for (int i = 0; i < MAX_UNLOCKS; i++) if (g_unlocks[i].f == f && g_unlocks[i].d == d) yes = true;
    irq_restore(fl);
    return yes;
}

static bool drop(BlockDev *d, File *f) {
    bool found = false;
    u64 fl = irq_save();
    for (int i = 0; i < MAX_UNLOCKS; i++)
        if (g_unlocks[i].f == f && (!d || g_unlocks[i].d == d)) {
            if (!d) d = g_unlocks[i].d;
            g_unlocks[i].f = NULL;
            g_unlocks[i].d = NULL;
            found = true;
        }
    irq_restore(fl);
    return found;
}

void blk_unlock_closed(BlockDev *d, File *f) {
    if (drop(d, f)) klog("Marisa", "blk: %s is read-only again (the file that held the write unlock was closed)", d->name);
}

/* Is the disk, or one of its partitions, the source of a mount? */
static bool disk_mounted(BlockDev *d, char *which, size_t wn) {
    char t[64], y[16], s[64];
    for (int i = 0; vfs_mount_list(i, t, sizeof t, y, sizeof y, s, sizeof s); i++) {
        if (strncmp(s, "/dev/", 5)) continue;
        BlockDev *m = blk_find(s);
        if (m && (m == d || m->parent == d)) { strlcpy(which, s, wn); return true; }
    }
    return false;
}

static int unlock(BlockDev *d, File *f, const struct gblk_unlock *u) {
    Process *p = current->proc;
    if (p->cred.uid != 0) {
        klog("Eiki", "%s (uid %u) asked to unlock %s for writing and was refused: only root may", p->name, p->cred.uid, d->name);
        return -EPERM;
    }
    if (d->parent) return -EINVAL;                                   /* whole disks only: a partition node never carries the unlock */
    if (d->removed) return -ENODEV;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    char name[sizeof u->name];
    memcpy(name, u->name, sizeof name);
    name[sizeof name - 1] = 0;
    if (strcmp(name, d->name)) {
        klog("Marisa", "blk: write unlock refused: the caller named '%s' but the file is /dev/%s", name, d->name);
        return -EINVAL;
    }
    if (d->readonly && !d->ro_policy) return -EROFS;                 /* the medium itself is write-protected: nothing to lift */
    char which[64];
    if (disk_mounted(d, which, sizeof which)) {
        klog("Marisa", "blk: write unlock of %s refused: %s is mounted", d->name, which);
        return -EBUSY;
    }
    int slot = -1;
    u64 fl = irq_save();
    for (int i = 0; i < MAX_UNLOCKS; i++) {
        if (g_unlocks[i].f == f && g_unlocks[i].d == d) { irq_restore(fl); return 0; }
        if (!g_unlocks[i].f && slot < 0) slot = i;
    }
    if (slot >= 0) { g_unlocks[slot].f = f; g_unlocks[slot].d = d; }
    irq_restore(fl);
    if (slot < 0) return -EBUSY;
    klog("Marisa", "blk: %s unlocked for writing by %s (pid %d), through one open file only; its partitions and every other disk stay as "
         "they were%s", d->name, p->name, p->pid, d->readonly ? " (blk.ro is lifted for this file)" : " (it was writable anyway)");
    return 0;
}

int blk_unlock_ioctl(BlockDev *d, File *f, unsigned long req, void *arg) {
    switch (req) {
    case BLKWRUNLOCK: return unlock(d, f, arg);
    case BLKWRLOCK:
        if (drop(d, f)) klog("Marisa", "blk: %s is read-only again (the write unlock was handed back)", d->name);
        return 0;
    case BLKWRSTATE:
        return (d->readonly ? BLKWR_READONLY : 0) | (d->ro_policy ? BLKWR_POLICY : 0) | (blk_unlock_allows(d, f) ? BLKWR_UNLOCKED : 0);
    default: return -ENOTTY;
    }
}

/* ---------------------------------------------------------------- blk.rw= */

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "01234567-89ab-cdef-0123-456789abcdef" -> the 16 bytes as GPT stores them (first three fields little-endian). */
static bool parse_guid(const char *s, size_t len, u8 out[16]) {
    static const u8 order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    u8 raw[16];
    int n = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '-') continue;
        int hi = hexval(s[i]), lo = i + 1 < len ? hexval(s[i + 1]) : -1;
        if (hi < 0 || lo < 0 || n >= 16) return false;
        raw[n++] = (u8)(hi << 4 | lo);
        i++;
    }
    if (n != 16) return false;
    for (int i = 0; i < 16; i++) out[order[i]] = raw[i];
    return true;
}

static void guid_text(const u8 g[16], char *out, size_t cap) {
    snprintf(out, cap, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
             g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

void blk_rw_check(BlockDev *part, const u8 part_guid[16]) {
    BlockDev *disk = part->parent;
    if (!disk || !part->readonly || !disk->readonly || !disk->ro_policy) return;  /* only blk.ro is lifted, never a write-protected medium */
    char list[160];
    if (!cmdline_value("blk.rw", list, sizeof list)) return;
    for (char *p = list; *p;) {
        char *end = p;
        while (*end && *end != ',') end++;
        u8 g[16];
        if (parse_guid(p, (size_t)(end - p), g) && !memcmp(g, part_guid, 16)) {
            char txt[40];
            guid_text(g, txt, sizeof txt);
            part->readonly = false;
            part->rw_grant = true;
            klog("Marisa", "blk: %s carries partition GUID %s, named by blk.rw=: writable although blk.ro is set (the homes of the disk "
                 "TouhouOS was installed on); the rest of %s and every other SATA and NVMe disk stay read-only", part->name, txt, disk->name);
            return;
        }
        p = *end ? end + 1 : end;
    }
}

/* ---------------------------------------------------------------- /proc/bootdisk */

static size_t gen_bootdisk(char *b, size_t cap) {
    const BootPartition *bp = &g_boot.boot_part;
    if (g_boot.version < 2 || !bp->valid)
        return (size_t)snprintf(b, cap, "unknown (the firmware did not say which partition the loader came from)\n");
    size_t n = 0;
    char sig[40];
    if (bp->sig_type == 2) guid_text(bp->signature, sig, sizeof sig);
    else if (bp->sig_type == 1) snprintf(sig, sizeof sig, "0x%02x%02x%02x%02x", bp->signature[3], bp->signature[2], bp->signature[1], bp->signature[0]);
    else strlcpy(sig, "-", sizeof sig);
    n += (size_t)snprintf(b + n, cap - n, "partition %u\ntable %s\nstart %lu\nsize %lu\nsignature %s\n", bp->part_number,
                          bp->table == 2 ? "gpt" : bp->table == 1 ? "mbr" : "?", (unsigned long)bp->part_start, (unsigned long)bp->part_size, sig);
    int found = 0;
    for (BlockDev *d = blk_first(); d && n + 64 < cap; d = blk_next(d))
        if (d->parent && d->part_index == (int)bp->part_number && d->part_start == bp->part_start && d->sectors == bp->part_size) {
            n += (size_t)snprintf(b + n, cap - n, "device %s %s\n", d->name, d->parent->name);
            found++;
        }
    if (!found) n += (size_t)snprintf(b + n, cap - n, "device - (no partition with that number, start and size is registered)\n");
    return n;
}

static void bootdisk_init(void) { procfs_register("bootdisk", gen_bootdisk); }
INITCALL(bootdisk_init);
