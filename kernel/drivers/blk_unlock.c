/*
 * What the installer needs from the block layer, kept out of blk.c so that file stays small:
 *
 *  - the per-disk write unlock (BLKWRUNLOCK, shared/blkabi.h): root lifts `blk.ro` for one whole disk, for writes through one open file
 *    only, until that file is closed. Partition nodes, other opens, mounted filesystems and every other disk keep getting EROFS.
 *  - `blk.rw=GUID[,GUID]` on the command line: the GPT partitions whose own (unique) partition GUID is listed stay writable although
 *    `blk.ro` is set. Only those partitions: the rest of their disk, the whole-disk node and every other disk stay read-only. The installer
 *    writes the GUID of the installed HUMANVILLAGE partition into the cmdline.txt of the system it installs, so the homes are writable
 *    while the installed ESP (loader, kernel, initrd, cmdline.txt) and a Windows disk next to it stay read-only.
 *  - the ESP write grant (BLKESPGRANT): root lifts `blk.ro` for one read-write mount of the ESP the loader came from, for as long as
 *    that mount and the open file that asked for it both last (an update of the installed system, the loader's "confirm this boot").
 *  - /proc/bootdisk: the partition the loader was read from (BootInfo.boot_part), the block devices that match it, which of them may get
 *    the ESP write grant, and the grants held.
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

static bool esp_grant_drop(BlockDev *d, File *f, const char *why);

void blk_unlock_closed(BlockDev *d, File *f) {
    if (drop(d, f)) klog("Marisa", "blk: %s is read-only again (the file that held the write unlock was closed)", d->name);
    if (d->parent) esp_grant_drop(d, f, "the file that held it was closed");
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

static int esp_grant(BlockDev *d, File *f, const struct gblk_unlock *u);
static int esp_grant_state(BlockDev *d, File *f);

int blk_unlock_ioctl(BlockDev *d, File *f, unsigned long req, void *arg) {
    switch (req) {
    case BLKWRUNLOCK: return unlock(d, f, arg);
    case BLKESPGRANT: return esp_grant(d, f, arg);
    case BLKWRLOCK:
        if (drop(d, f)) klog("Marisa", "blk: %s is read-only again (the write unlock was handed back)", d->name);
        if (d->parent) esp_grant_drop(d, f, "it was handed back");
        return 0;
    case BLKWRSTATE: {
        bool policy = d->parent ? d->parent->ro_policy : d->ro_policy;     /* a partition is read-only by the policy of its disk */
        return (d->readonly ? BLKWR_READONLY : 0) | (policy ? BLKWR_POLICY : 0) | (blk_unlock_allows(d, f) ? BLKWR_UNLOCKED : 0) |
               esp_grant_state(d, f);
    }
    default: return -ENOTTY;
    }
}

/* ---------------------------------------------------------------- the ESP write grant
 *
 * Under blk.ro an installed system's ESP is read-only (blk.rw= names only its HUMANVILLAGE partition), but an update has to replace the
 * kernel and initrd on it, and the loader's fallback wants a "this boot worked" mark written there. The grant is the narrow way:
 *
 *   - asked for with BLKESPGRANT on an open file (O_RDWR) of the partition's own /dev node, by root (uid 0), naming the node;
 *   - only for the partition the loader was read from: number, start and size as the firmware reported them (/proc/bootdisk), the GPT
 *     partition GUID or MBR disk signature read back from the disk, an EFI System Partition type on GPT, and no second registered
 *     partition that matches all of it (a byte copy of the disk could not be told apart);
 *   - it makes nothing writable by itself: one mount of that partition with the option `espgrant`, made by a process that holds the
 *     open file, takes it, and that mounted volume (`holder`) is then the only writer (blk_write_as); writes through the partition node,
 *     the whole-disk node, the other partitions and every other disk keep failing with EROFS;
 *   - it ends at the unmount, or when the open file is closed (also at exit or a crash, when the kernel closes the descriptors), or with
 *     BLKWRLOCK; if the volume is still mounted then, the filesystem's revoke() writes it back and turns it read-only first;
 *   - it lives in this table only: nothing goes to disk, NVRAM or the command line, and a restart forgets it.
 *
 * Without blk.ro (a USB stick, which blk.ro does not cover) the partition is writable anyway; the grant then changes nothing about
 * writability, but the mount still turns read-only when the file is closed, so an updater behaves the same on the stick and on a disk.
 */

#define MAX_GRANTS 2

typedef struct {
    File *f;                            /* the open partition node that asked */
    BlockDev *part;
    int pid;
    char who[32];
    void *holder;                       /* the mounted volume that took it, or NULL */
    void (*revoke)(void *holder);
    char target[64];                    /* where it is mounted */
} EspGrant;

static EspGrant g_grants[MAX_GRANTS];
static Mutex g_grant_lock;              /* taken before a volume's own lock (revoke), never while holding one */

static bool part_matches_boot(const BlockDev *d, const BootPartition *bp) {
    return d->parent && d->part_index == (int)bp->part_number && d->part_start == bp->part_start && d->sectors == bp->part_size;
}

/* 1: the disk carries the boot partition's signature (GPT entry GUID or MBR disk signature), 0: another one, -1: cannot tell. */
static int boot_signature_on(const BlockDev *part, const BootPartition *bp) {
    BlockDev *disk = part->parent;
    u32 ss = disk->sector_size;
    u8 *buf = kmalloc(2 * (size_t)ss);
    if (!buf) return -1;
    int ok = -1;
    if (bp->sig_type == 1 && blk_read(disk, 0, 1, buf) == 0) {
        ok = !memcmp(buf + 440, bp->signature, 4);
    } else if (bp->sig_type == 2 && blk_read(disk, 1, 1, buf) == 0 && !memcmp(buf, "EFI PART", 8)) {
        u64 entries_lba;
        u32 esize;
        memcpy(&entries_lba, buf + 72, 8);
        memcpy(&esize, buf + 84, 4);
        if (part->part_index >= 1 && esize >= 128 && esize <= 1024) {
            u64 off = entries_lba * ss + (u64)(part->part_index - 1) * esize + 16;       /* the entry's unique GUID */
            u64 lba = off / ss;
            u32 n = off % ss + 16 > ss ? 2 : 1;
            if (lba + n <= disk->sectors && blk_read(disk, lba, n, buf) == 0) ok = !memcmp(buf + off % ss, bp->signature, 16);
        }
    }
    kfree(buf);
    return ok;
}

/* NULL when `d` is the ESP the loader came from, else why not (in words, for the log and /proc/bootdisk). */
static const char *esp_ineligible(BlockDev *d) {
    const BootPartition *bp = &g_boot.boot_part;
    if (g_boot.version < 2 || !bp->valid) return "the firmware did not say which partition the loader came from";
    if (!part_matches_boot(d, bp)) return "it is not the partition the loader came from (see /proc/bootdisk)";
    if (bp->table == 2 && memcmp(d->part_type_guid, GPT_GUID_EFI_SYSTEM, 16)) return "it is not an EFI System Partition";
    int s = boot_signature_on(d, bp);
    if (s < 0) return "its partition signature cannot be read";
    if (!s) return "its disk carries another partition signature than the one the loader came from (the same layout, another disk)";
    for (BlockDev *o = blk_first(); o; o = blk_next(o))
        if (o != d && !o->removed && part_matches_boot(o, bp) && boot_signature_on(o, bp) == 1)
            return "another disk carries the very same partition (a byte copy?), so which one the system came from cannot be told";
    return NULL;
}

static bool part_mounted(const BlockDev *d, char *which, size_t wn) {
    char t[64], y[16], s[64];
    for (int i = 0; vfs_mount_list(i, t, sizeof t, y, sizeof y, s, sizeof s); i++) {
        if (strncmp(s, "/dev/", 5)) continue;
        if (blk_find(s) == d) { strlcpy(which, t, wn); return true; }
    }
    return false;
}

static int esp_grant(BlockDev *d, File *f, const struct gblk_unlock *u) {
    Process *p = current->proc;
    if (p->cred.uid != 0) {
        klog("Eiki", "%s (uid %u) asked for write access to the ESP %s and was refused: only root may", p->name, p->cred.uid, d->name);
        return -EPERM;
    }
    if (!d->parent) return -EINVAL;                                  /* the ESP's partition node, never a whole disk */
    if (d->removed) return -ENODEV;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    char name[sizeof u->name];
    memcpy(name, u->name, sizeof name);
    name[sizeof name - 1] = 0;
    if (strcmp(name, d->name)) {
        klog("Marisa", "blk: ESP write grant refused: the caller named '%s' but the file is /dev/%s", name, d->name);
        return -EINVAL;
    }
    const char *why = esp_ineligible(d);
    if (why) {
        klog("Marisa", "blk: ESP write grant for %s refused: %s", d->name, why);
        return -EACCES;
    }
    if (d->readonly && !d->parent->ro_policy) return -EROFS;         /* a write-protected medium: nothing to lift */
    char where[64];
    if (part_mounted(d, where, sizeof where)) {
        klog("Marisa", "blk: ESP write grant for %s refused: it is mounted on %s (unmount it first)", d->name, where);
        return -EBUSY;
    }
    mutex_lock(&g_grant_lock);
    int slot = -1, rc = 0;
    for (int i = 0; i < MAX_GRANTS && !rc; i++) {
        if (g_grants[i].part == d) rc = g_grants[i].f == f ? 1 : -EBUSY;    /* already ours: nothing to do; somebody else's: busy */
        else if (!g_grants[i].part && slot < 0) slot = i;
    }
    if (!rc && slot < 0) rc = -EBUSY;
    if (!rc) {
        EspGrant *g = &g_grants[slot];
        memset(g, 0, sizeof *g);
        g->f = f;
        g->part = d;
        g->pid = p->pid;
        strlcpy(g->who, p->name, sizeof g->who);
    }
    mutex_unlock(&g_grant_lock);
    if (rc < 0) return rc;
    if (!rc)
        klog("Marisa", "blk: %s, the ESP this system started from, is lent to %s (pid %d) for one read-write mount (this time Marisa asked "
             "first)%s; its node, the rest of %s and every other disk stay as they were", d->name, p->name, p->pid,
             d->readonly ? ": blk.ro is lifted for that mount only" : " (it was writable anyway)", d->parent->name);
    return 0;
}

static int esp_grant_state(BlockDev *d, File *f) {
    int bits = 0;
    mutex_lock(&g_grant_lock);
    for (int i = 0; i < MAX_GRANTS; i++)
        if (g_grants[i].part == d && g_grants[i].f == f) bits = BLKWR_ESPGRANT | (g_grants[i].holder ? BLKWR_ESPMOUNTED : 0);
    mutex_unlock(&g_grant_lock);
    return bits;
}

/* The open file went away (or BLKWRLOCK): a volume still mounted with the grant is written back and turned read-only first. */
static bool esp_grant_drop(BlockDev *d, File *f, const char *why) {
    bool found = false;
    mutex_lock(&g_grant_lock);
    for (int i = 0; i < MAX_GRANTS; i++) {
        EspGrant *g = &g_grants[i];
        if (g->part != d || g->f != f) continue;
        found = true;
        if (g->holder) {
            if (g->revoke) g->revoke(g->holder);                     /* still allowed to write while it writes back */
            d->grant_holder = NULL;
            klog("Marisa", "blk: %s is read-only again: the ESP write grant of %s (pid %d) ended because %s; the volume on %s stays mounted "
                 "read-only until it is unmounted", d->name, g->who, g->pid, why, g->target);
        } else {
            klog("Marisa", "blk: the ESP write grant for %s ended unused (%s)", d->name, why);
        }
        memset(g, 0, sizeof *g);
    }
    mutex_unlock(&g_grant_lock);
    return found;
}

/* mount -o espgrant: a grant for this partition, not taken yet, held by an open file of the calling process. */
static EspGrant *claimable(BlockDev *part) {
    Process *p = current->proc;
    for (int i = 0; i < MAX_GRANTS; i++) {
        EspGrant *g = &g_grants[i];
        if (g->part != part || g->holder) continue;
        for (int k = 0; k < MAX_FD; k++)
            if (p->fds[k] == g->f) return g;
    }
    return NULL;
}

int blk_esp_grant_claim(BlockDev *part) {
    Process *p = current->proc;
    mutex_lock(&g_grant_lock);
    bool ok = p->cred.uid == 0 && claimable(part) != NULL;
    mutex_unlock(&g_grant_lock);
    if (!ok) klog("Marisa", "blk: %s asked for a read-write mount of %s with espgrant but holds no ESP write grant for it (BLKESPGRANT first)",
                  p->name, part->name);
    return ok ? 0 : -EPERM;
}

int blk_esp_grant_attach(BlockDev *part, void *holder, void (*revoke)(void *holder), const char *target) {
    mutex_lock(&g_grant_lock);
    EspGrant *g = claimable(part);
    char who[32] = "";
    int pid = 0;
    if (g) {
        g->holder = holder;
        g->revoke = revoke;
        strlcpy(g->target, target, sizeof g->target);
        part->grant_holder = holder;
        strlcpy(who, g->who, sizeof who);
        pid = g->pid;
    }
    mutex_unlock(&g_grant_lock);
    if (!pid) return -EPERM;                                         /* the file was closed in the meantime */
    klog("Marisa", "blk: %s is writable through the volume mounted on %s only, until it is unmounted or %s (pid %d) closes the grant",
         part->name, target, who, pid);
    return 0;
}

/* The volume is being unmounted (after its last write-back): the grant ends with it. */
void blk_esp_grant_detach(BlockDev *part, void *holder) {
    mutex_lock(&g_grant_lock);
    for (int i = 0; i < MAX_GRANTS; i++) {
        EspGrant *g = &g_grants[i];
        if (g->part != part || !g->holder || g->holder != holder) continue;
        part->grant_holder = NULL;
        klog("Marisa", "blk: %s is read-only again: the ESP write grant ended with the unmount of %s", part->name, g->target);
        memset(g, 0, sizeof *g);
    }
    mutex_unlock(&g_grant_lock);
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
    /* which partition the ESP write grant may be given for (BLKESPGRANT), and the grants held */
    BlockDev *esp = NULL;
    const char *why = "no partition with that number, start and size is registered";
    for (BlockDev *d = blk_first(); d && !esp; d = blk_next(d))
        if (part_matches_boot(d, bp)) {
            why = esp_ineligible(d);
            if (!why) esp = d;
        }
    if (esp) n += (size_t)snprintf(b + n, cap - n, "esp %s %s\n", esp->name, esp->readonly ? "read-only" : "writable");
    else n += (size_t)snprintf(b + n, cap - n, "esp - (%s)\n", why);
    mutex_lock(&g_grant_lock);
    for (int i = 0; i < MAX_GRANTS && n + 160 < cap; i++) {
        const EspGrant *g = &g_grants[i];
        if (!g->part) continue;
        if (g->holder) n += (size_t)snprintf(b + n, cap - n, "grant %s pid %d %s mounted %s\n", g->part->name, g->pid, g->who, g->target);
        else n += (size_t)snprintf(b + n, cap - n, "grant %s pid %d %s unused\n", g->part->name, g->pid, g->who);
    }
    mutex_unlock(&g_grant_lock);
    return n;
}

static void bootdisk_init(void) {
    mutex_init(&g_grant_lock);
    procfs_register("bootdisk", gen_bootdisk);
}
INITCALL(bootdisk_init);
