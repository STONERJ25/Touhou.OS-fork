/*
 * Mount policy for persistent storage (Keine decides what the Human Village may keep on a real disk).
 *
 * The rules, which docs/STORAGE.md repeats:
 *   1. Exactly one thing is ever mounted automatically: the volume labelled HUMANVILLAGE, read-write, on /HumanVillage.
 *      The label is a GPT partition name ("HUMANVILLAGE") or a FAT volume label ("HUMANVILLAG": FAT labels are 11
 *      characters long, so the 12-letter name is stored truncated).
 *   2. No other disk or partition is mounted, written, formatted or probed with writes - ever. To find a FAT label the
 *      boot sector (and the first root-directory cluster) of FAT-typed partitions is READ; nothing else is touched. Other
 *      operating systems' partitions on the owner's disks stay byte-for-byte as they were.
 *   3. When nothing matches, boot continues with the in-RAM tmpfs home directories exactly as before.
 *   4. A volume that was not shut down cleanly is mounted read-only (see fat32.c); `nohumanvillage` on the kernel command
 *      line skips the whole thing.
 *
 * The three resident homes (reimu, marisa, sakuya) are created on the volume when they are missing. With the "userdirs"
 * mount option the FAT driver gives each of them to its user with mode 0700, files 0600.
 *
 * Disks that appear after boot (USB sticks: the bus is enumerated by a thread of its own, seconds after the boot-time call) reach
 * the policy through storage_disk_added(). Which volume wins when several carry the label: the first one found. The boot-time call
 * sees the internal disks (virtio, SATA, NVMe), so a HUMANVILLAGE volume on one of them is mounted before any stick is looked at,
 * and a stick's volume is then left alone with a line in the log; with no internal one, the first stick's volume wins and later
 * sticks are ignored the same way.
 *
 * FAT stores local time. The boot-time mount passes the offset of the standard time of the `tz=` command line word as
 * `tzoffset=MINUTES` (the kernel has no time zone of its own); without tz= the volume is mounted with UTC, as before.
 */
#include "vfs.h"
#include "blk.h"
#include "kernel.h"
#include "mm.h"
#include "cmdline.h"
#include "bootlog.h"

extern const FsType g_vfat_type, g_fat16_type;

static const char *const HOMES[] = { "reimu", "marisa", "sakuya" };

static bool label_is_humanvillage(const char *s) {
    return !strcasecmp(s, "HUMANVILLAGE") || !strcasecmp(s, "HUMANVILLAG");
}

static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static void trim_label(const u8 *raw, char *out) {
    memcpy(out, raw, 11);
    out[11] = 0;
    for (int i = 10; i >= 0 && (out[i] == ' ' || out[i] == 0); i--) out[i] = 0;
}

/* Reads the FAT volume label of a device (read-only). Returns false if the device does not look like FAT16/FAT32. */
static bool probe_fat_label(BlockDev *d, char *label) {
    label[0] = 0;
    u32 ss = d->sector_size;
    if (ss < 512 || ss > 4096 || d->sectors < 64) return false;
    u8 *bs = kmalloc(ss);
    if (!bs) return false;
    bool ok = false;
    if (blk_read(d, 0, 1, bs) == 0 && bs[510] == 0x55 && bs[511] == 0xAA) {
        u32 bps = rd16(bs + 11), spc = bs[13], reserved = rd16(bs + 14), nfats = bs[16], root_entries = rd16(bs + 17);
        u32 fatsz16 = rd16(bs + 22), fatsz32 = rd32(bs + 36);
        bool pow2 = bps && !(bps & (bps - 1)) && spc && !(spc & (spc - 1));
        bool plausible = pow2 && bps >= ss && bps <= 4096 && bps % ss == 0 && nfats >= 1 && nfats <= 4 && reserved >= 1 && bs[21] >= 0xF0;
        bool fat32 = fatsz16 == 0 && root_entries == 0 && fatsz32 != 0;
        bool fat16 = fatsz16 != 0 && root_entries != 0;
        if (plausible && (fat32 || fat16)) {
            ok = true;
            trim_label(bs + (fat32 ? 71 : 43), label);
            if (!strcmp(label, "NO NAME")) label[0] = 0;
            if (!label_is_humanvillage(label)) {
                /* the authoritative label is an entry in the root directory: look through its first cluster/sectors */
                u64 dir_lba_bytes;
                u32 dir_bytes;
                if (fat32) {
                    u64 root = rd32(bs + 44);
                    u64 data = (u64)reserved + (u64)nfats * fatsz32;
                    dir_lba_bytes = (data + (root >= 2 ? (root - 2) * spc : 0)) * bps;
                    dir_bytes = bps * spc;
                } else {
                    dir_lba_bytes = ((u64)reserved + (u64)nfats * fatsz16) * bps;
                    dir_bytes = root_entries * 32;
                }
                if (dir_bytes > 32768) dir_bytes = 32768;
                dir_bytes = (dir_bytes / ss) * ss;
                u8 *dir = dir_bytes ? kmalloc(dir_bytes) : NULL;
                if (dir && dir_lba_bytes % ss == 0 && (dir_lba_bytes + dir_bytes) / ss <= d->sectors &&
                    blk_read(d, dir_lba_bytes / ss, dir_bytes / ss, dir) == 0) {
                    for (u32 off = 0; off + 32 <= dir_bytes; off += 32) {
                        if (dir[off] == 0x00) break;
                        if (dir[off] != 0xE5 && (dir[off + 11] & 0x3F) == 0x08) { trim_label(dir + off, label); break; }
                    }
                }
                kfree(dir);
            }
        }
    }
    kfree(bs);
    return ok;
}

static bool fat_typed(const BlockDev *d) {
    switch (d->part_type_mbr) {
    case 0x01: case 0x04: case 0x06: case 0x0B: case 0x0C: case 0x0E: return true;
    default: break;
    }
    return !memcmp(d->part_type_guid, GPT_GUID_MS_BASIC_DATA, 16);
}

static bool has_children(const BlockDev *disk) {
    for (BlockDev *d = blk_first(); d; d = blk_next(d)) if (d->parent == disk) return true;
    return false;
}

static const BlockDev *whole_disk(const BlockDev *d) {
    while (d->parent) d = d->parent;
    return d;
}

/* Finds the volume that is meant to be the Human Village; NULL if there is none. With `only`, just that disk (and its partitions) is
 * searched: used to say that a stick also holds one when /HumanVillage is taken already. */
static BlockDev *find_humanvillage(const BlockDev *only, const char **how) {
    BlockDev *found = NULL;
    for (BlockDev *d = blk_first(); d; d = blk_next(d)) {          /* 1. GPT partition name: no I/O needed */
        if (only && whole_disk(d) != only) continue;
        if (!d->label[0] || !label_is_humanvillage(d->label)) continue;
        if (!found) { found = d; *how = "partition label"; }
        else klog("Keine", "ignoring a second HUMANVILLAGE volume on %s", d->name);
    }
    if (found) return found;
    for (BlockDev *d = blk_first(); d; d = blk_next(d)) {          /* 2. FAT volume label: reads one boot sector per candidate */
        if (only && whole_disk(d) != only) continue;
        bool candidate = d->parent ? fat_typed(d) : !has_children(d);
        if (!candidate) continue;
        char label[16];
        if (!probe_fat_label(d, label) || !label_is_humanvillage(label)) continue;
        if (!d->label[0]) strlcpy(d->label, label, sizeof d->label);       /* lsblk and blk_find_label() know it from now on */
        if (!found) { found = d; *how = "FAT volume label"; }
        else klog("Keine", "ignoring a second HUMANVILLAGE volume on %s", d->name);
    }
    return found;
}

static void ensure_homes(void) {
    for (unsigned i = 0; i < ARRAY_SIZE(HOMES); i++) {
        char path[64];
        snprintf(path, sizeof path, "/HumanVillage/%s", HOMES[i]);
        struct gstat st;
        if (vfs_stat(path, NULL, NULL, 0, &st) == 0) continue;
        int rc = vfs_mkdir(path, 0700, NULL, NULL);
        if (rc) klog("Keine", "cannot create %s (%d)%s", path, rc, rc == -EROFS ? ": the volume is read-only" : "");
        else klog("Keine", "created %s", path);
    }
}

/* The standard-time offset of a POSIX TZ string in minutes EAST of UTC, which is what FAT's local-time fields need:
 * "EST5EDT,M3.2.0,M11.1.0" -> -300, "JST-9" -> 540, "<+0530>-5:30" -> 330. Zero when there is no usable offset (the libc does the same:
 * a zone it cannot parse is UTC). Only the standard offset is looked at, not the summer rules. */
int storage_tz_offset_minutes(const char *tz) {
    const char *s = tz;
    if (*s == '<') {
        while (*s && *s != '>') s++;
        if (*s == '>') s++;
    } else {
        while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) s++;
    }
    if (s == tz) return 0;
    int sign = 1;
    if (*s == '+') s++;
    else if (*s == '-') { sign = -1; s++; }
    if (*s < '0' || *s > '9') return 0;
    int h = 0, m = 0;
    while (*s >= '0' && *s <= '9') { h = h * 10 + (*s - '0'); if (h > 99) return 0; s++; }
    if (*s == ':') {
        s++;
        while (*s >= '0' && *s <= '9') { m = m * 10 + (*s - '0'); if (m > 99) return 0; s++; }
    }
    if (h > 24 || m > 59) return 0;
    return -sign * (h * 60 + m);                                   /* POSIX counts the hours WEST of Greenwich */
}

/* Option string for the HUMANVILLAGE mount: userdirs, plus tzoffset= when tz= names a zone that is not UTC. */
static void mount_options(char *out, size_t cap) {
    strlcpy(out, "userdirs", cap);
    char tz[64];
    if (!cmdline_value("tz", tz, sizeof tz)) return;
    int minutes = storage_tz_offset_minutes(tz);
    if (minutes) {
        char more[24];
        snprintf(more, sizeof more, ",tzoffset=%d", minutes);
        strlcat(out, more, cap);
    }
}

static Mutex g_policy_lock;                                        /* the policy runs one pass at a time (boot call, hot-plug workers) */
static bool g_policy_ready;
static BlockDev *g_hv_dev;                                         /* what /HumanVillage was mounted from (may have been unplugged since) */
static volatile int g_workers;                                     /* automount worker threads that exist (queued or running) */

static void policy_init_once(void) {
    if (!g_policy_ready) { mutex_init(&g_policy_lock); g_policy_ready = true; }
}

static bool humanvillage_mounted(char *source, size_t cap) {
    char t[64], y[16], s[64];
    for (int i = 0; vfs_mount_list(i, t, sizeof t, y, sizeof y, s, sizeof s); i++) {
        if (strcmp(t, "/HumanVillage")) continue;
        strlcpy(source, s, cap);
        return true;
    }
    return false;
}

/* One pass of the policy. `added` is the disk that triggered it (a stick that appeared after boot), NULL for the boot-time call. */
static void run_policy(BlockDev *added) {
    char holder[64];
    if (humanvillage_mounted(holder, sizeof holder)) {
        const char *how = "";
        BlockDev *other = added ? find_humanvillage(added, &how) : NULL;
        if (other && g_hv_dev && g_hv_dev->removed)
            klog("Keine", "%s holds a HUMANVILLAGE volume (%s), but /HumanVillage is still mounted on %s, whose stick was pulled out: umount it (or reboot) and the "
                 "Human Village can move to the stick that is here now", other->name, how, holder);
        else if (other)
            klog("Keine", "%s holds a HUMANVILLAGE volume too (%s), but /HumanVillage already lives on %s: the first one found wins, this one is left alone",
                 other->name, how, holder);
        return;
    }
    if (cmdline_flag("nohumanvillage")) {
        klog("Keine", "nohumanvillage: persistent storage is left alone, homes stay in RAM");
        return;
    }
    const char *how = "";
    BlockDev *hv = find_humanvillage(NULL, &how);
    if (!hv) {
        if (!added) klog("Keine", "no HUMANVILLAGE volume found: homes stay in RAM (tmpfs)");
        else klog("Keine", "%s has no HUMANVILLAGE volume: nothing to mount from it (the policy never mounts anything else)", added->name);
        return;
    }
    char source[32], opts[64];
    snprintf(source, sizeof source, "/dev/%s", hv->name);
    mount_options(opts, sizeof opts);
    int rc = vfs_mount_opts("fat32", source, "/HumanVillage", 0, opts);
    if (rc) {
        klog("Keine", "HUMANVILLAGE on %s (%s) cannot be mounted (%d): homes stay in RAM (tmpfs)", hv->name, how, rc);
        return;
    }
    g_hv_dev = hv;
    bool stick = hv->driver && !strcmp(hv->driver, "usb-storage");
    if (stick) klog("Keine", "the Human Village keeps its records on the stick now (%s, found by %s): the homes persist from here on", hv->name, how);
    ensure_homes();
    bool rw = true;
    char t[64], y[16], s[64];
    for (int i = 0; vfs_mount_list(i, t, sizeof t, y, sizeof y, s, sizeof s); i++)
        if (!strcmp(t, "/HumanVillage")) rw = !(vfs_mount_flags(i) & MNT_RDONLY);
    bootlog_volume_mounted("/HumanVillage", rw);
}

/* "storage.selftest" on the kernel command line: the TZ parser against zones of every shape (one log line with the verdict). */
static void tz_selftest(void) {
    static const struct { const char *tz; int minutes; } T[] = {
        { "EST5EDT,M3.2.0,M11.1.0", -300 }, { "JST-9", 540 }, { "UTC", 0 }, { "", 0 }, { "<+0530>-5:30", 330 }, { "PST8PDT", -480 },
        { "AEST-10AEDT,M10.1.0,M4.1.0/3", 600 }, { "nonsense", 0 }, { "XYZ25", 0 }, { "<-03>3", -180 }, { "CET-1CEST,M3.5.0,M10.5.0/3", 60 },
        { "NST3:30NDT", -210 }, { "GMT0", 0 }, { "ABC+4", -240 }, { "ABC-", 0 }, { "5", 0 },
    };
    int bad = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(T); i++) {
        int got = storage_tz_offset_minutes(T[i].tz);
        if (got != T[i].minutes) { bad++; klog("Keine", "storage selftest: tz '%s' gave %d minutes, expected %d", T[i].tz, got, T[i].minutes); }
    }
    klog("Keine", "storage selftest: %u time zone checks, %d failed", (unsigned)ARRAY_SIZE(T), bad);
}

/* Called by kmain once the block devices exist. A driver that finds disks later (USB, hot-plug) goes through storage_disk_added(). */
void storage_mount_boot_volumes(void) {
    static bool registered;
    policy_init_once();
    if (cmdline_flag("storage.selftest")) tz_selftest();
    if (!registered) {
        registered = true;
        vfs_register_fs(&g_vfat_type);
        vfs_register_fs(&g_fat16_type);
    }
    mutex_lock(&g_policy_lock);
    run_policy(NULL);
    mutex_unlock(&g_policy_lock);
}

static void automount_main(void *arg) {
    BlockDev *disk = arg;
    mutex_lock(&g_policy_lock);
    if (!disk->removed) run_policy(disk);
    mutex_unlock(&g_policy_lock);
    __atomic_fetch_sub(&g_workers, 1, __ATOMIC_SEQ_CST);
}

void storage_disk_added(BlockDev *disk) {
    policy_init_once();
    __atomic_fetch_add(&g_workers, 1, __ATOMIC_SEQ_CST);
    if (!task_create_detached("hv-automount", automount_main, disk)) {
        __atomic_fetch_sub(&g_workers, 1, __ATOMIC_SEQ_CST);
        klog("Keine", "cannot start the automount thread for %s: mount the HUMANVILLAGE volume by hand if it has one", disk->name);
    }
}

bool storage_automount_busy(void) { return g_workers > 0; }
