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
 */
#include "vfs.h"
#include "blk.h"
#include "kernel.h"
#include "mm.h"
#include "cmdline.h"

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

/* Finds the volume that is meant to be the Human Village; NULL if there is none. */
static BlockDev *find_humanvillage(const char **how) {
    BlockDev *found = NULL;
    for (BlockDev *d = blk_first(); d; d = blk_next(d)) {          /* 1. GPT partition name: no I/O needed */
        if (!d->label[0] || !label_is_humanvillage(d->label)) continue;
        if (!found) { found = d; *how = "partition label"; }
        else klog("Keine", "ignoring a second HUMANVILLAGE volume on %s", d->name);
    }
    if (found) return found;
    for (BlockDev *d = blk_first(); d; d = blk_next(d)) {          /* 2. FAT volume label: reads one boot sector per candidate */
        bool candidate = d->parent ? fat_typed(d) : !has_children(d);
        if (!candidate) continue;
        char label[16];
        if (!probe_fat_label(d, label) || !label_is_humanvillage(label)) continue;
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

/* Called by kmain once the block devices exist. */
void storage_mount_boot_volumes(void) {
    vfs_register_fs(&g_vfat_type);
    vfs_register_fs(&g_fat16_type);
    if (cmdline_flag("nohumanvillage")) {
        klog("Keine", "nohumanvillage: persistent storage is left alone, homes stay in RAM");
        return;
    }
    const char *how = "";
    BlockDev *hv = find_humanvillage(&how);
    if (!hv) {
        klog("Keine", "no HUMANVILLAGE volume found: homes stay in RAM (tmpfs)");
        return;
    }
    char source[32];
    snprintf(source, sizeof source, "/dev/%s", hv->name);
    int rc = vfs_mount_opts("fat32", source, "/HumanVillage", 0, "userdirs");
    if (rc) {
        klog("Keine", "HUMANVILLAGE on %s (%s) cannot be mounted (%d): homes stay in RAM (tmpfs)", hv->name, how, rc);
        return;
    }
    ensure_homes();
}
