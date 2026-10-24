/*
 * install: puts TouhouOS on a disk of this machine, so it starts without the stick (docs/INSTALL.md).
 *
 * The layout is the one tools/mkdisk.py builds for the stick image: a protective MBR and a GPT, partition 1 a 64 MiB FAT32 EFI System
 * Partition named TOUHOUOS-ESP (EFI/BOOT/BOOTX64.EFI, kernel.elf, initrd.img, cmdline.txt), partition 2 a FAT32 volume named HUMANVILLAGE
 * that takes the rest; 1 MiB alignment, 128 GPT entries, FAT32 with 32 reserved sectors, two FATs, the cluster size by volume size, the
 * same type GUIDs. The files come from the partition the loader was read from (/proc/bootdisk), mounted read-only, or from --from DIR.
 *
 * Safety: root only; refuses the disk the system started from, a disk with a mounted partition, a partition node, and (without --force) a
 * disk that holds any partition or a recognisable partition table or file system; the user types the disk's name to go on (or gives
 * --yes-i-typed=NAME, which must be that exact name); the kernel's blk.ro is lifted for this one disk through this one open file only
 * (BLKWRUNLOCK, shared/blkabi.h) and handed back before the program ends. Nothing is written before all of that passed.
 * A disk that holds another operating system (Windows, Linux, macOS, a RAID member: other_systems()) needs --force *and* a typed sentence
 * that names the systems, the disk's model and its name (ERASE WINDOWS ON <model> <disk>); what would be lost is listed first.
 * Afterwards the disk's cache is flushed (a failure fails the install) and every byte written is read back and compared (read_back()).
 */
#include "kappabox.h"
#include <inttypes.h>
#include <stdarg.h>
#include <blkabi.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t  u8;

#define MIB        (1024ull * 1024)
#define ESP_BYTES  (64 * MIB)
#define GPT_ENTRIES 128
#define GPT_ESIZE   128
#define BLKFLSBUF   0x1261                  /* flush the disk's write cache (as Linux numbers it) */

static const u8 GUID_ESP[16]   = { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b };
static const u8 GUID_BASIC[16] = { 0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44, 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 };

/* ------------------------------------------------------------------------------------------------ small helpers */

static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { for (int i = 0; i < 4; i++) p[i] = (u8)(v >> (8 * i)); }
static void put64(u8 *p, u64 v) { for (int i = 0; i < 8; i++) p[i] = (u8)(v >> (8 * i)); }
static u32 get32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 get64(const u8 *p) { return (u64)get32(p) | (u64)get32(p + 4) << 32; }

static u32 crc32_of(const u8 *p, size_t n) {
    u32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static void guid_text(const u8 g[16], char *out, size_t cap) {
    snprintf(out, cap, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
             g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

static void random_guid(u8 g[16]) {
    if (gap_getrandom(g, 16) != 16) {                       /* no entropy source: the clock and the pid still make it unique enough */
        u64 t = (u64)gap_time_ms() * 6364136223846793005ull + (u64)getpid();
        for (int i = 0; i < 16; i++) { t = t * 6364136223846793005ull + 1442695040888963407ull; g[i] = (u8)(t >> 56); }
    }
    g[7] = (u8)((g[7] & 0x0F) | 0x40);                     /* version 4 (the third field is stored little-endian) */
    g[8] = (u8)((g[8] & 0x3F) | 0x80);                     /* RFC 4122 variant */
}

static int read_at(int fd, u64 off, void *buf, size_t n) {
    if (lseek(fd, (off_t)off, SEEK_SET) < 0) return -1;
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, (u8 *)buf + done, n - done);
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

/*
 * The write log. Every byte install writes to the target is recorded here (a copy of it, a pointer to data that lives until the end, or
 * "zeros"), so that after the flush all of it can be read back and compared: the files, the directories, both FATs, the reserved areas
 * (boot sectors, FSInfo and their backups), both GPT entry arrays and headers, and the protective MBR.
 */
typedef struct { u64 off, len; const u8 *data; int owned; const char *what; } Extent;

static Extent *g_ext;
static int g_next, g_extcap;
static const char *g_what = "the disk";                    /* what the writes that follow are, for the messages */
static u64 g_fail_off = ~0ull;                              /* the first write that failed: where, what, why */
static const char *g_fail_what;
static int g_fail_errno;

static void note_extent(u64 off, const void *data, u64 n, int copy) {
    if (!n) return;
    Extent *last = g_next ? &g_ext[g_next - 1] : NULL;
    if (!data && last && !last->data && last->off + last->len == off && last->what == g_what) { last->len += n; return; }
    if (g_next == g_extcap) {
        g_extcap = g_extcap ? 2 * g_extcap : 64;
        g_ext = realloc(g_ext, (size_t)g_extcap * sizeof *g_ext);
        if (!g_ext) kb_die("out of memory");
    }
    Extent *e = &g_ext[g_next++];
    e->off = off;
    e->len = n;
    e->what = g_what;
    e->owned = data && copy;
    e->data = data;
    if (e->owned) {
        u8 *c = malloc((size_t)n);
        if (!c) kb_die("out of memory");
        memcpy(c, data, (size_t)n);
        e->data = c;
    }
}

static int raw_write_at(int fd, u64 off, const void *buf, size_t n) {
    size_t done = 0;
    int ok = lseek(fd, (off_t)off, SEEK_SET) >= 0;
    while (ok && done < n) {
        ssize_t r = write(fd, (const u8 *)buf + done, n - done);
        if (r <= 0) {
            if (!r) errno = EIO;
            ok = 0;
        } else {
            done += (size_t)r;
        }
    }
    if (ok) return 0;
    if (g_fail_off == ~0ull) { g_fail_off = off + done; g_fail_what = g_what; g_fail_errno = errno; }
    return -1;
}

/* write_at copies what it wrote into the log; write_at_kept only points at it (the caller keeps buf alive until the read-back). */
static int write_at(int fd, u64 off, const void *buf, size_t n) {
    note_extent(off, buf, n, 1);
    return raw_write_at(fd, off, buf, n);
}

static int write_at_kept(int fd, u64 off, const void *buf, size_t n) {
    note_extent(off, buf, n, 0);
    return raw_write_at(fd, off, buf, n);
}

static u8 *g_zero;                                          /* 1 MiB of zeros */

static int zero_at(int fd, u64 off, u64 n) {
    note_extent(off, NULL, n, 0);
    while (n) {
        size_t take = n > MIB ? (size_t)MIB : (size_t)n;
        if (raw_write_at(fd, off, g_zero, take)) return -1;
        off += take;
        n -= take;
    }
    return 0;
}

/*
 * Reads every logged extent back and compares it with what was written. Prints one line per extent that differs or cannot be read and
 * returns how many did; *bytes gets the number of bytes compared.
 */
static int read_back(int fd, unsigned ss, u64 *bytes) {
    u8 *buf = malloc(MIB);
    if (!buf) kb_die("out of memory");
    int bad = 0;
    *bytes = 0;
    for (int i = 0; i < g_next; i++) {
        const Extent *e = &g_ext[i];
        u64 done = 0, diff = ~0ull;
        int unreadable = 0;
        while (done < e->len && diff == ~0ull && !unreadable) {
            size_t take = e->len - done > MIB ? (size_t)MIB : (size_t)(e->len - done);
            if (read_at(fd, e->off + done, buf, take)) { unreadable = 1; break; }
            const u8 *want = e->data ? e->data + done : g_zero;
            if (memcmp(buf, want, take))
                for (size_t k = 0; k < take; k++)
                    if (buf[k] != want[k]) { diff = done + k; break; }
            done += take;
        }
        *bytes += e->len;
        if (!unreadable && diff == ~0ull) continue;
        if (bad++ < 24) {
            if (unreadable)
                fprintf(stderr, "install: READ-BACK FAILED: %s, %llu bytes at byte %llu (sector %llu): could not be read back: %s\n", e->what,
                        (unsigned long long)e->len, (unsigned long long)e->off, (unsigned long long)(e->off / ss), strerror(errno));
            else
                fprintf(stderr, "install: READ-BACK MISMATCH: %s, %llu bytes at byte %llu (sector %llu): differs from what was written, first at "
                        "byte %llu (sector %llu)\n", e->what, (unsigned long long)e->len, (unsigned long long)e->off, (unsigned long long)(e->off / ss),
                        (unsigned long long)(e->off + diff), (unsigned long long)((e->off + diff) / ss));
        }
    }
    free(buf);
    return bad;
}

static char *mib_text(u64 bytes, char *buf, size_t n) {
    if (bytes >= 10ull * 1024 * MIB) snprintf(buf, n, "%llu GiB", (unsigned long long)(bytes >> 30));
    else snprintf(buf, n, "%llu MiB", (unsigned long long)(bytes >> 20));
    return buf;
}

/* ------------------------------------------------------------------------------------------------ what the kernel says */

typedef struct {
    char name[32], parent[32], label[48], driver[24], model[64];
    u64 size, start;
    unsigned ss;
    int ro;
} Row;

static Row g_rows[96];
static int g_nrows;

static void load_rows(void) {
    FILE *f = fopen("/proc/partitions", "r");
    if (!f) return;
    char line[256];
    while (g_nrows < (int)(sizeof g_rows / sizeof g_rows[0]) && fgets(line, sizeof line, f)) {
        Row *r = &g_rows[g_nrows];
        unsigned long long size, start;
        memset(r, 0, sizeof *r);
        if (sscanf(line, "%31s %llu %u %d %31s %llu %47s %23s %63[^\n]", r->name, &size, &r->ss, &r->ro, r->parent, &start, r->label, r->driver,
                   r->model) < 8) continue;
        r->size = size;
        r->start = start;
        g_nrows++;
    }
    fclose(f);
}

static Row *row(const char *name) {
    for (int i = 0; i < g_nrows; i++) if (!strcmp(g_rows[i].name, name)) return &g_rows[i];
    return NULL;
}

/* The mount point of /dev/NAME, or of any partition of disk NAME (whole = 1). */
static int mounted(const char *name, int whole, char *src, size_t sn, char *where, size_t wn) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;
    char line[256], s[96], t[160];
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        if (sscanf(line, "%95s %159s", s, t) != 2 || strncmp(s, "/dev/", 5)) continue;
        Row *r = row(s + 5);
        if (!strcmp(s + 5, name) || (whole && r && !strcmp(r->parent, name))) {
            if (src) strlcpy(src, s, sn);
            if (where) strlcpy(where, t, wn);
            found = 1;
        }
    }
    fclose(f);
    return found;
}

/* Where the system started from: /proc/bootdisk names the partition the loader was read from (number, start, size, the GPT partition GUID
 * or the MBR disk signature) and the registered partitions with that number, start and size; the signature is checked here on the disk. */
typedef struct {
    int info;                   /* the firmware said which partition it was */
    int found;                  /* ... and it is registered here, signature checked */
    char part[32], disk[32];
    char suspects[8][32];       /* disks with a partition of the same number, start and size that could be it too: the signature could not
                                   be read, or matched a second time (two sticks written from one image carry the same GUIDs) */
    int nsuspects;
} Boot;

/* 1: the disk carries the boot partition's signature, 0: it carries another one (a disk with the same layout, such as an older install),
 * -1: cannot tell. */
static int boot_signature_matches(const char *disk, unsigned number, const char *table, const char *sig) {
    Row *d = row(disk);
    if (!d) return -1;
    char path[48];
    snprintf(path, sizeof path, "/dev/%s", disk);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int ok = -1;
    u8 *buf = malloc(4096);
    if (buf && !strcmp(table, "mbr")) {
        if (!read_at(fd, 0, buf, 512)) {
            char txt[16];
            snprintf(txt, sizeof txt, "0x%08" PRIx32, get32(buf + 440));
            ok = !strcasecmp(txt, sig);
        }
    } else if (buf && !strcmp(table, "gpt") && d->ss <= 4096) {
        if (!read_at(fd, d->ss, buf, d->ss) && !memcmp(buf, "EFI PART", 8)) {
            u64 entries = get64(buf + 72);
            u32 esize = get32(buf + 84);
            u8 e[128];
            if (number >= 1 && esize >= 128 && esize <= 1024 && !read_at(fd, entries * d->ss + (u64)(number - 1) * esize, e, sizeof e)) {
                char txt[40];
                guid_text(e + 16, txt, sizeof txt);
                ok = !strcasecmp(txt, sig);
            }
        }
    }
    free(buf);
    close(fd);
    return ok;
}

static void find_boot(Boot *b) {
    memset(b, 0, sizeof *b);
    FILE *f = fopen("/proc/bootdisk", "r");
    if (!f) return;
    char line[160], key[32], v1[64], v2[64], table[16] = "", sig[48] = "";
    unsigned number = 0;
    while (fgets(line, sizeof line, f)) {
        int n = sscanf(line, "%31s %63s %63s", key, v1, v2);
        if (n < 2) continue;
        if (!strcmp(key, "partition")) { number = (unsigned)strtoul(v1, NULL, 10); b->info = 1; }
        else if (!strcmp(key, "table")) strlcpy(table, v1, sizeof table);
        else if (!strcmp(key, "signature")) strlcpy(sig, v1, sizeof sig);
        else if (!strcmp(key, "device") && n == 3) {
            int m = boot_signature_matches(v2, number, table, sig);
            if (m == 1 && !b->found) {
                b->found = 1;
                strlcpy(b->part, v1, sizeof b->part);
                strlcpy(b->disk, v2, sizeof b->disk);
            } else if (m != 0 && b->nsuspects < 8) {
                strlcpy(b->suspects[b->nsuspects++], v2, sizeof b->suspects[0]);
            }
        }
    }
    fclose(f);
}

/* ------------------------------------------------------------------------------------------------ is the disk in use? */

/* Appends to `what` everything recognisable on the disk: partitions the kernel registered, partition tables, file system signatures. */
static int disk_content(int fd, const Row *d, char *what, size_t wn) {
    int found = 0;
    char one[160];
#define SAY(...) do { snprintf(one, sizeof one, __VA_ARGS__); if (what[0]) strlcat(what, "; ", wn); strlcat(what, one, wn); found = 1; } while (0)
    char parts[160] = "";
    for (int i = 0; i < g_nrows; i++)
        if (!strcmp(g_rows[i].parent, d->name)) {
            if (parts[0]) strlcat(parts, " ", sizeof parts);
            strlcat(parts, g_rows[i].name, sizeof parts);
            if (strcmp(g_rows[i].label, "-")) { strlcat(parts, "(", sizeof parts); strlcat(parts, g_rows[i].label, sizeof parts); strlcat(parts, ")", sizeof parts); }
        }
    if (parts[0]) SAY("partitions %s", parts);
    size_t head = 72 * 1024;
    u8 *b = malloc(head);
    if (!b) return found;
    u64 total = d->size / d->ss;
    if (d->size >= head && !read_at(fd, 0, b, head)) {
        int mbr = b[510] == 0x55 && b[511] == 0xAA;
        int any_entry = 0;
        for (int i = 0; i < 4; i++) if (b[446 + 16 * i + 4]) any_entry = 1;
        u16 bps = (u16)(b[11] | b[12] << 8);
        int fat = mbr && (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096) && b[13] && !(b[13] & (b[13] - 1)) && b[16] >= 1 && b[16] <= 4 &&
                  b[21] >= 0xF0;
        if (!memcmp(b + 3, "NTFS    ", 8)) SAY("an NTFS file system (no partition table)");
        else if (!memcmp(b + 3, "EXFAT   ", 8)) SAY("an exFAT file system (no partition table)");
        else if (fat) SAY("a FAT file system (no partition table)");
        else if (mbr && any_entry) SAY("an MBR partition table");
        if (d->ss <= 4096 && !memcmp(b + d->ss, "EFI PART", 8)) SAY("a GPT partition table");
        if (b[1080] == 0x53 && b[1081] == 0xEF) SAY("an ext2/3/4 file system");
        if (!memcmp(b, "LUKS\xba\xbe", 6)) SAY("a LUKS encrypted volume");
        if (!memcmp(b, "XFSB", 4)) SAY("an XFS file system");
        if (!memcmp(b + 512, "LABELONE", 8)) SAY("an LVM physical volume");
        if (!memcmp(b + 4086, "SWAPSPACE2", 10) || !memcmp(b + 4086, "SWAP-SPACE", 10)) SAY("a Linux swap area");
        if (!memcmp(b + 32769, "CD001", 5)) SAY("an ISO 9660 (CD) image");
        if (!memcmp(b + 65600, "_BHRfS_M", 8)) SAY("a btrfs file system");
    }
    if (total > 4 && !read_at(fd, (total - 3) * d->ss, b, 3 * (size_t)d->ss)) {
        for (size_t o = 0; o + 24 <= 3 * (size_t)d->ss; o += 512)
            if (!memcmp(b + o, "Intel Raid ISM Cfg Sig. ", 24)) { SAY("Intel RST (RAID) metadata"); break; }
        if (!memcmp(b + 2 * d->ss, "EFI PART", 8)) SAY("a backup GPT header at the end of the disk");
    }
    free(b);
#undef SAY
    return found;
}

/* ------------------------------------------------------------------------------------------------ other operating systems */

/*
 * A disk that holds another operating system is not erased with --force alone: the user has to type a sentence that names the systems,
 * the disk's model and its name (docs/INSTALL.md, "Other operating systems"). Recognised: Windows (Microsoft reserved, Windows recovery,
 * LDM and Storage Spaces partitions, a basic data partition that does not hold a FAT file system, NTFS, exFAT, ReFS, BitLocker), Linux
 * (its partition types, ext2/3/4, XFS, btrfs, swap, LVM, LUKS, a GRUB BIOS boot partition), macOS (HFS+, APFS) and the members of an
 * Intel RST RAID set. What cannot be read counts as precious too.
 */
enum { OS_WINDOWS = 1, OS_LINUX = 2, OS_MACOS = 4, OS_RAID = 8, OS_UNKNOWN = 16 };
static const char *const k_os_word[] = { "WINDOWS", "LINUX", "MACOS", "RAID", "UNREADABLE DATA" };
#define OS_KINDS 5
#define SIG_BYTES (72 * 1024)                               /* enough for every signature below (btrfs sits at 64 KiB) */
#define MAX_LOST 64

typedef struct {
    char where[16];                                         /* "partition 3", "logical 5", "whole disk", "end of disk" */
    char name[40];                                          /* GPT partition name */
    u64 bytes;
    char type[48];                                          /* partition type in words */
    char fs[64];                                            /* what was found at its start */
    int os;
} Lost;

static const struct { const char *guid, *name; int os; } k_gpt_types[] = {
    { "c12a7328-f81f-11d2-ba4b-00a0c93ec93b", "EFI System Partition", 0 },
    { "e3c9e316-0b5c-4db8-817d-f92df00215ae", "Microsoft reserved", OS_WINDOWS },
    { "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7", "Microsoft basic data", OS_WINDOWS },        /* not when it holds FAT: see classify() */
    { "de94bba4-06d1-4d40-a16a-bfd50179d6ac", "Windows recovery", OS_WINDOWS },
    { "5808c8aa-7e8f-42e0-85d2-e1e90434cfb3", "Windows LDM metadata", OS_WINDOWS },
    { "af9b60a0-1431-4f62-bc68-3311714a69ad", "Windows LDM data", OS_WINDOWS },
    { "e75caf8f-f680-4cee-afa3-b001e56efc2d", "Windows Storage Spaces", OS_WINDOWS },
    { "0fc63daf-8483-4772-8e79-3d69d8477de4", "Linux filesystem", OS_LINUX },
    { "4f68bce3-e8cd-4db1-96e7-fbcaf984b709", "Linux root (x86-64)", OS_LINUX },
    { "44479540-f297-41b2-9af7-d131d5f0458a", "Linux root (x86)", OS_LINUX },
    { "b921b045-1df0-41c3-af44-4c6f280d3fae", "Linux root (ARM64)", OS_LINUX },
    { "8484680c-9521-48c6-9c11-b0720656f69e", "Linux /usr (x86-64)", OS_LINUX },
    { "933ac7e1-2eb4-4f13-b844-0e14e2aef915", "Linux /home", OS_LINUX },
    { "3b8f8425-20e0-4f3b-907f-1a25a76f98e8", "Linux /srv", OS_LINUX },
    { "4d21b016-b534-45c2-a9fb-5c16e091fd2d", "Linux /var", OS_LINUX },
    { "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f", "Linux swap", OS_LINUX },
    { "e6d6d379-f507-44c2-a23c-238f2a3df928", "Linux LVM", OS_LINUX },
    { "a19d880f-05fc-4d3b-a006-743f0f84911e", "Linux RAID", OS_LINUX },
    { "ca7d7ccb-63ed-4c53-861c-1742536059cc", "Linux LUKS", OS_LINUX },
    { "bc13c2ff-59e6-4262-a352-b275fd6f7172", "Linux extended boot", OS_LINUX },
    { "21686148-6449-6e6f-744e-656564454649", "BIOS boot (GRUB)", OS_LINUX },
    { "48465300-0000-11aa-aa11-00306543ecac", "Apple HFS+", OS_MACOS },
    { "7c3457ef-0000-11aa-aa11-00306543ecac", "Apple APFS", OS_MACOS },
    { "426f6f74-0000-11aa-aa11-00306543ecac", "Apple boot", OS_MACOS },
};

static const struct { u8 type; const char *name; int os; } k_mbr_types[] = {
    { 0x01, "FAT12", 0 }, { 0x04, "FAT16", 0 }, { 0x06, "FAT16", 0 }, { 0x0B, "FAT32", 0 }, { 0x0C, "FAT32 (LBA)", 0 }, { 0x0E, "FAT16 (LBA)", 0 },
    { 0x07, "NTFS/exFAT", OS_WINDOWS }, { 0x27, "Windows recovery", OS_WINDOWS }, { 0x42, "Windows dynamic disk", OS_WINDOWS },
    { 0x82, "Linux swap", OS_LINUX }, { 0x83, "Linux", OS_LINUX }, { 0x8E, "Linux LVM", OS_LINUX }, { 0xFD, "Linux RAID", OS_LINUX },
    { 0xAF, "Apple HFS+", OS_MACOS }, { 0xEF, "EFI System Partition", 0 },
};

/* What sits at the start of a volume. Returns a description ("" when nothing is recognised) and ORs the system it belongs to into *os;
 * *fat is set for a FAT file system. */
static const char *fs_signature(const u8 *b, size_t n, int *os, int *fat, char *buf, size_t bn) {
    *fat = 0;
    if (n >= 512) {
        if (!memcmp(b + 3, "-FVE-FS-", 8)) { *os |= OS_WINDOWS; return "BitLocker-encrypted volume"; }
        if (!memcmp(b + 3, "NTFS    ", 8)) { *os |= OS_WINDOWS; return "NTFS"; }
        if (!memcmp(b + 3, "EXFAT   ", 8)) { *os |= OS_WINDOWS; return "exFAT"; }
        if (!memcmp(b + 3, "ReFS", 4)) { *os |= OS_WINDOWS; return "ReFS"; }
        if (!memcmp(b, "LUKS\xba\xbe", 6)) { *os |= OS_LINUX; return "LUKS encrypted volume"; }
        if (!memcmp(b, "XFSB", 4)) { *os |= OS_LINUX; return "XFS"; }
        if (!memcmp(b + 32, "NXSB", 4)) { *os |= OS_MACOS; return "APFS"; }
        u16 bps = (u16)(b[11] | b[12] << 8);
        if (b[510] == 0x55 && b[511] == 0xAA && (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096) && b[13] && !(b[13] & (b[13] - 1)) &&
            b[16] >= 1 && b[16] <= 4 && b[21] >= 0xF0) {
            int f32 = !b[22] && !b[23];
            const u8 *lab = f32 ? b + 71 : b + 43;
            int has_label = (f32 ? b[66] : b[38]) == 0x29;
            char l[12];
            memcpy(l, lab, 11);
            l[11] = 0;
            for (int i = 10; i >= 0 && l[i] == ' '; i--) l[i] = 0;
            for (int i = 0; l[i]; i++) if (l[i] < 32 || l[i] > 126) l[i] = '?';
            *fat = 1;
            snprintf(buf, bn, has_label && l[0] && strcmp(l, "NO NAME") ? "%s \"%s\"" : "%s", f32 ? "FAT32" : "FAT12/16", l);
            return buf;
        }
    }
    if (n >= 1082 && b[1080] == 0x53 && b[1081] == 0xEF) { *os |= OS_LINUX; return "ext2/3/4"; }
    if (n >= 1026 && b[1024] == 'H' && (b[1025] == '+' || b[1025] == 'X')) { *os |= OS_MACOS; return "HFS+"; }
    if (n >= 520 && !memcmp(b + 512, "LABELONE", 8)) { *os |= OS_LINUX; return "LVM physical volume"; }
    if (n >= 4096 && (!memcmp(b + 4086, "SWAPSPACE2", 10) || !memcmp(b + 4086, "SWAP-SPACE", 10))) { *os |= OS_LINUX; return "Linux swap"; }
    if (n >= 65608 && !memcmp(b + 65600, "_BHRfS_M", 8)) { *os |= OS_LINUX; return "btrfs"; }
    return "";
}

/* Sector 0 is the boot sector of a file system that covers the whole disk (its bytes 446-509 are boot code, not partition entries). */
static int volume_boot_sector(const u8 *b, size_t n) {
    int os = 0, fat = 0;
    char buf[64];
    return (b[0] == 0xEB || b[0] == 0xE9) && fs_signature(b, n, &os, &fat, buf, sizeof buf)[0];
}

/* Looks at one partition: its type (from the table) and what it holds. */
static void classify(int fd, unsigned ss, u64 first, u64 count, const char *type, int type_os, int basic, u8 *b, Lost *l) {
    l->bytes = count * ss;
    strlcpy(l->type, type, sizeof l->type);
    size_t n = count * ss < SIG_BYTES ? (size_t)(count * ss) : SIG_BYTES;
    int fs_os = 0, fat = 0;
    char buf[64];
    if (n && !read_at(fd, first * ss, b, n)) {
        strlcpy(l->fs, fs_signature(b, n, &fs_os, &fat, buf, sizeof buf), sizeof l->fs);
    } else {
        strlcpy(l->fs, "could not be read", sizeof l->fs);
        fs_os = OS_UNKNOWN;
    }
    /* A basic data partition is Windows' kind of partition (C:, D:, a recovery image) unless it holds FAT, which sticks, cameras and an
     * earlier TouhouOS install (HUMANVILLAGE) use too. */
    l->os = (basic && fat ? 0 : type_os) | fs_os;
}

static void guid_lower(const u8 g[16], char *out, size_t cap) {
    guid_text(g, out, cap);
    for (char *p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
}

/* Fills out[] with everything on the disk that would be lost, and returns the systems found (OS_* bits). */
static int other_systems(int fd, const Row *d, Lost *out, int max, int *nout) {
    unsigned ss = d->ss;
    u64 total = d->size / ss;
    int os = 0, n = 0;
    u8 *b = malloc(SIG_BYTES), *hdr = malloc(ss), *mbr = malloc(ss);
    if (!b || !hdr || !mbr) kb_die("out of memory");
#define NEXT() (n < max ? &out[n++] : &out[max - 1])
    int gpt = 0;
    if (!read_at(fd, ss, hdr, ss) && !memcmp(hdr, "EFI PART", 8)) gpt = 1;
    else if (total > 2 && !read_at(fd, (total - 1) * ss, hdr, ss) && !memcmp(hdr, "EFI PART", 8)) gpt = 2;   /* primary damaged: the backup */
    if (gpt) {
        u64 elba = get64(hdr + 72);
        u32 nent = get32(hdr + 80), esz = get32(hdr + 84);
        size_t bytes = (size_t)nent * esz;
        u8 *ents = (esz >= 128 && esz <= 1024 && nent && nent <= 1024 && bytes <= 512 * 1024 && elba < total) ? malloc(bytes) : NULL;
        if (!ents || read_at(fd, elba * ss, ents, bytes)) {
            Lost *l = NEXT();
            memset(l, 0, sizeof *l);
            strlcpy(l->where, "GPT", sizeof l->where);
            strlcpy(l->type, gpt == 2 ? "backup GPT" : "GPT", sizeof l->type);
            strlcpy(l->fs, "its partition entries could not be read", sizeof l->fs);
            l->bytes = d->size;
            l->os = OS_UNKNOWN;
        }
        for (u32 i = 0; ents && i < nent; i++) {
            const u8 *e = ents + (size_t)i * esz;
            static const u8 zero16[16];
            if (!memcmp(e, zero16, 16)) continue;
            u64 first = get64(e + 32), last = get64(e + 40);
            Lost *l = NEXT();
            memset(l, 0, sizeof *l);
            snprintf(l->where, sizeof l->where, "partition %u", i + 1);
            size_t k = 0;
            for (; k < 36 && k < sizeof l->name - 1 && (e[56 + 2 * k] || e[57 + 2 * k]); k++) {
                u16 ch = (u16)(e[56 + 2 * k] | e[57 + 2 * k] << 8);
                l->name[k] = ch >= 32 && ch < 127 ? (char)ch : '?';
            }
            l->name[k] = 0;
            char g[40];
            guid_lower(e, g, sizeof g);
            const char *tname = NULL;
            int tos = 0, basic = 0;
            for (size_t t = 0; t < sizeof k_gpt_types / sizeof k_gpt_types[0]; t++)
                if (!strcmp(g, k_gpt_types[t].guid)) { tname = k_gpt_types[t].name; tos = k_gpt_types[t].os; basic = t == 2; }
            char unknown[48];
            if (!tname) { snprintf(unknown, sizeof unknown, "type %s", g); tname = unknown; }
            if (last < first || last >= total) { strlcpy(l->type, tname, sizeof l->type); strlcpy(l->fs, "outside the disk", sizeof l->fs); l->os = tos; }
            else classify(fd, ss, first, last - first + 1, tname, tos, basic, b, l);
            os |= l->os;
        }
        free(ents);
    } else if (!read_at(fd, 0, mbr, ss) && mbr[510] == 0x55 && mbr[511] == 0xAA && !volume_boot_sector(mbr, ss)) {
        /* MBR: the four primary entries, and the chain of logical partitions inside an extended one */
        u64 ext_base = 0, ebr = 0;
        int logical = 5;
        for (int i = 0; i < 4; i++) {
            const u8 *e = mbr + 446 + 16 * i;
            u8 type = e[4];
            u64 start = get32(e + 8), count = get32(e + 12);
            if (!type || !count || type == 0xEE) continue;
            if (type == 0x05 || type == 0x0F || type == 0x85) { if (!ext_base) ext_base = ebr = start; continue; }
            const char *tname = NULL;
            int tos = 0;
            for (size_t t = 0; t < sizeof k_mbr_types / sizeof k_mbr_types[0]; t++)
                if (k_mbr_types[t].type == type) { tname = k_mbr_types[t].name; tos = k_mbr_types[t].os; }
            char unknown[48];
            if (!tname) { snprintf(unknown, sizeof unknown, "MBR type %02x", type); tname = unknown; }
            Lost *l = NEXT();
            memset(l, 0, sizeof *l);
            snprintf(l->where, sizeof l->where, "partition %d", i + 1);
            if (start + count > total) { strlcpy(l->type, tname, sizeof l->type); strlcpy(l->fs, "outside the disk", sizeof l->fs); l->os = tos; }
            else classify(fd, ss, start, count, tname, tos, type == 0x07, b, l);
            os |= l->os;
        }
        for (int hops = 0; ext_base && ebr < total && hops < 64; hops++) {
            if (read_at(fd, ebr * ss, hdr, ss) || hdr[510] != 0x55 || hdr[511] != 0xAA) {
                Lost *l = NEXT();
                memset(l, 0, sizeof *l);
                strlcpy(l->where, "extended", sizeof l->where);
                strlcpy(l->type, "extended partition", sizeof l->type);
                strlcpy(l->fs, "its chain of logical partitions could not be read", sizeof l->fs);
                l->os = OS_UNKNOWN;
                os |= l->os;
                break;
            }
            const u8 *e = hdr + 446;
            u8 type = e[4];
            u64 start = ebr + get32(e + 8), count = get32(e + 12);
            if (type && count) {
                const char *tname = NULL;
                int tos = 0;
                for (size_t t = 0; t < sizeof k_mbr_types / sizeof k_mbr_types[0]; t++)
                    if (k_mbr_types[t].type == type) { tname = k_mbr_types[t].name; tos = k_mbr_types[t].os; }
                char unknown[48];
                if (!tname) { snprintf(unknown, sizeof unknown, "MBR type %02x", type); tname = unknown; }
                Lost *l = NEXT();
                memset(l, 0, sizeof *l);
                snprintf(l->where, sizeof l->where, "logical %d", logical++);
                if (start + count > total) { strlcpy(l->type, tname, sizeof l->type); strlcpy(l->fs, "outside the disk", sizeof l->fs); l->os = tos; }
                else classify(fd, ss, start, count, tname, tos, type == 0x07, b, l);
                os |= l->os;
            }
            u64 next = get32(e + 16 + 8);
            if (!e[16 + 4] || !next) break;
            ebr = ext_base + next;
        }
    }
    if (!n) {                                               /* no partition table: a file system over the whole disk? */
        Lost l;
        memset(&l, 0, sizeof l);
        classify(fd, ss, 0, total, "no partition table", 0, 0, b, &l);
        if (l.fs[0] || l.os) {
            Lost *o = NEXT();
            *o = l;
            strlcpy(o->where, "whole disk", sizeof o->where);
            os |= l.os;
        }
    }
    if (total > 4 && !read_at(fd, (total - 3) * ss, b, 3 * (size_t)ss)) {
        for (size_t o = 0; o + 24 <= 3 * (size_t)ss; o += 512)
            if (!memcmp(b + o, "Intel Raid ISM Cfg Sig. ", 24)) {
                Lost *l = NEXT();
                memset(l, 0, sizeof *l);
                strlcpy(l->where, "end of disk", sizeof l->where);
                strlcpy(l->type, "Intel RST (RAID) metadata", sizeof l->type);
                strlcpy(l->fs, "a member of a RAID set: erasing it breaks the set", sizeof l->fs);
                l->bytes = d->size;
                l->os = OS_RAID;
                os |= OS_RAID;
                break;
            }
    }
#undef NEXT
    free(b); free(hdr); free(mbr);
    *nout = n;
    return os;
}

static void os_words(int os, char *out, size_t n) {
    out[0] = 0;
    for (int i = 0; i < OS_KINDS; i++)
        if (os & (1 << i)) {
            if (out[0]) strlcat(out, " AND ", n);
            strlcat(out, k_os_word[i], n);
        }
}

/* The sentence the user has to type: ERASE WINDOWS ON <model> <disk>, e.g. "ERASE WINDOWS ON SAMSUNG MZVLB256HAHQ-000L7 nvme0n1". */
static void erase_sentence(int os, const Row *d, const char *name, char *out, size_t n) {
    char w[96];
    os_words(os, w, sizeof w);
    snprintf(out, n, "ERASE %s ON %s%s%s", w, strcmp(d->model, "-") ? d->model : "", strcmp(d->model, "-") ? " " : "", name);
}

static void print_losses(const char *name, const Row *d, const Lost *l, int n, int os) {
    char w[96], sz[24];
    os_words(os, w, sizeof w);
    printf("install: %s (%s%s%s) holds %s. Everything on it would be lost:\n", name, mib_text(d->size, sz, sizeof sz),
           strcmp(d->model, "-") ? ", " : "", strcmp(d->model, "-") ? d->model : "", w);
    for (int i = 0; i < n; i++) {
        char q[48], who[96] = "";
        snprintf(q, sizeof q, "\"%s\"", l[i].name);
        if (l[i].os) { os_words(l[i].os, who, sizeof who); }
        printf("install:   %-12s %-30.30s %9s  %s%s%s%s%s\n", l[i].where, l[i].name[0] ? q : "", mib_text(l[i].bytes, sz, sizeof sz), l[i].type,
               l[i].fs[0] ? ", " : "", l[i].fs, who[0] ? "  <- " : "", who);
    }
}

/* ------------------------------------------------------------------------------------------------ layout */

typedef struct { u64 first, count; } Span;

static u64 min_fat32_bytes(unsigned ss) { return ss == 4096 ? 258 * MIB : 34 * MIB; }

/* tools/mkdisk.py build_image(): partitions on 1 MiB boundaries after the GPT, the ESP first, HUMANVILLAGE takes the rest. */
static int compute_layout(u64 total, unsigned ss, Span *esp, Span *hv) {
    u64 align = MIB / ss, arr = (GPT_ENTRIES * GPT_ESIZE + ss - 1) / ss;
    if (total < 2 * arr + 4 * align) return -1;
    u64 first_usable = 2 + arr, last_usable = total - 2 - arr;
    u64 cursor = (first_usable + align - 1) / align * align;
    u64 esp_bytes = ESP_BYTES > min_fat32_bytes(ss) ? ESP_BYTES : min_fat32_bytes(ss);
    esp->first = cursor;
    esp->count = esp_bytes / ss / align * align;
    cursor = (cursor + esp->count + align - 1) / align * align;
    if (cursor > last_usable) return -1;
    hv->first = cursor;
    hv->count = last_usable + 1 - cursor;
    if (hv->count > 0xFFFFFFFFull) hv->count = 0xFFFFFFFFull / align * align;     /* FAT32 counts sectors in 32 bits: the rest stays free */
    if (hv->count * ss < min_fat32_bytes(ss)) return -1;
    return 0;
}

/* ------------------------------------------------------------------------------------------------ FAT32 */

typedef struct {
    u64 disk_off;               /* byte offset of the volume on the disk */
    u64 total;                  /* sectors */
    u32 bps, spc, reserved, nfats, fatsz, nclusters, root_cluster, volid, cluster_bytes;
    u64 data_start;             /* sector of cluster 2 */
    u32 *fat;                   /* entries 0 .. next_free-1 */
    u32 next_free, cap;
    char label[12];
    char what_dir[64], what_fat[64], what_res[96];          /* names of its parts in the read-back messages */
} Fat;

static u32 default_cluster_bytes(u64 size) {                /* mkdisk.default_cluster_bytes() */
    static const struct { u64 limit; u32 cb; } t[] = { { 64 * MIB, 512 }, { 128 * MIB, 1024 }, { 256 * MIB, 2048 }, { 8192 * MIB, 4096 },
                                                         { 16384 * MIB, 8192 }, { 32768 * MIB, 16384 } };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (size <= t[i].limit) return t[i].cb;
    return 32768;
}

static int fat_init(Fat *v, u64 disk_off, u64 sectors, u32 bps, const char *label) {
    memset(v, 0, sizeof *v);
    v->disk_off = disk_off;
    v->total = sectors;
    v->bps = bps;
    v->reserved = 32;
    v->nfats = 2;
    u32 cb = default_cluster_bytes(sectors * bps);
    if (cb < bps) cb = bps;
    v->cluster_bytes = cb;
    v->spc = cb / bps;
    u32 per_sector = bps / 4;
    u64 clusters = 0, fatsz = (sectors / v->spc + 2) / per_sector;       /* a lower bound; the loop below settles it like mkdisk does */
    if (fatsz > 16) fatsz -= 16; else fatsz = 1;
    for (;;) {
        clusters = (sectors - v->reserved - v->nfats * fatsz) / v->spc;
        if (clusters + 2 <= fatsz * per_sector) break;
        fatsz++;
    }
    v->fatsz = (u32)fatsz;
    v->data_start = v->reserved + v->nfats * fatsz;
    v->nclusters = (u32)((sectors - v->data_start) / v->spc);
    if (v->nclusters < 65525 || v->nclusters >= 0x0FFFFFF5) return -1;
    v->cap = 1024;
    v->fat = calloc(v->cap, 4);
    if (!v->fat) return -1;
    v->fat[0] = 0x0FFFFFF8;
    v->fat[1] = 0x0FFFFFFF;
    v->next_free = 2;
    gap_getrandom(&v->volid, sizeof v->volid);
    snprintf(v->label, sizeof v->label, "%-11.11s", label);
    snprintf(v->what_dir, sizeof v->what_dir, "the directories of %s", label);
    snprintf(v->what_fat, sizeof v->what_fat, "the FATs of %s", label);
    snprintf(v->what_res, sizeof v->what_res, "the boot sector, FSInfo and their backups of %s", label);
    return 0;
}

static u32 fat_alloc(Fat *v, u32 n) {
    if (!n) return 0;
    if ((u64)v->next_free + n > (u64)v->nclusters + 2) return 0;
    while (v->next_free + n > v->cap) {
        v->cap *= 2;
        v->fat = realloc(v->fat, (size_t)v->cap * 4);
        if (!v->fat) kb_die("out of memory");
    }
    u32 first = v->next_free;
    for (u32 c = first; c < first + n - 1; c++) v->fat[c] = c + 1;
    v->fat[first + n - 1] = 0x0FFFFFFF;
    v->next_free += n;
    return first;
}

static u64 cluster_off(const Fat *v, u32 c) { return v->disk_off + (v->data_start + (u64)(c - 2) * v->spc) * v->bps; }

static void dos_time(u16 *date, u16 *tm) {
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    int year = t.tm_year + 1900 - 1980;
    if (year < 0) year = 0;
    if (year > 127) year = 127;
    *date = (u16)(year << 9 | (t.tm_mon + 1) << 5 | t.tm_mday);
    *tm = (u16)(t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec / 2);
}

/* One 32-byte short directory entry. name11 is the 8.3 name padded with spaces; ntres 0x08/0x10 marks an all-lower-case base/extension
 * (how mkdisk stores "kernel.elf" without a long name, and what Windows, Linux and the UEFI FAT driver all understand). */
static void dirent83(u8 *e, const char *name11, u8 attr, u8 ntres, u32 first, u32 size) {
    u16 date, tm;
    dos_time(&date, &tm);
    memset(e, 0, 32);
    memcpy(e, name11, 11);
    e[11] = attr;
    e[12] = ntres;
    put16(e + 14, tm);
    put16(e + 16, date);
    put16(e + 18, date);
    put16(e + 20, (u16)(first >> 16));
    put16(e + 22, tm);
    put16(e + 24, date);
    put16(e + 26, (u16)first);
    put32(e + 28, size);
}

/* kept: data stays alive until the read-back (the file contents), so the write log need not copy it */
static int write_cluster_data(int fd, const Fat *v, u32 first, const void *data, size_t n, int kept) {
    if (!n) return 0;
    u64 clusters = (n + v->cluster_bytes - 1) / v->cluster_bytes;
    if ((kept ? write_at_kept : write_at)(fd, cluster_off(v, first), data, n)) return -1;
    u64 pad = clusters * v->cluster_bytes - n;              /* no stale bytes in the tail of the last cluster */
    return pad ? zero_at(fd, cluster_off(v, first) + n, pad) : 0;
}

/* FATs, then the reserved area with the boot sector last: an interrupted run never leaves something that looks like a finished volume. */
static int fat_finish(int fd, Fat *v) {
    size_t used = ((size_t)v->next_free * 4 + v->bps - 1) / v->bps * v->bps;
    u8 *head = calloc(1, used);
    if (!head) return -1;
    for (u32 i = 0; i < v->next_free; i++) put32(head + (size_t)i * 4, v->fat[i]);
    g_what = v->what_fat;
    for (u32 k = 0; k < v->nfats; k++) {
        u64 off = v->disk_off + (u64)(v->reserved + (u64)k * v->fatsz) * v->bps;
        if (write_at(fd, off, head, used) || zero_at(fd, off + used, (u64)v->fatsz * v->bps - used)) { free(head); return -1; }
    }
    free(head);
    g_what = v->what_res;
    size_t rbytes = (size_t)v->reserved * v->bps;
    u8 *r = calloc(1, rbytes);
    if (!r) return -1;
    u8 *b = r;
    b[0] = 0xEB; b[1] = 0x58; b[2] = 0x90;
    memcpy(b + 3, "TOUHOUOS", 8);
    put16(b + 11, (u16)v->bps);
    b[13] = (u8)v->spc;
    put16(b + 14, (u16)v->reserved);
    b[16] = (u8)v->nfats;
    b[21] = 0xF8;                                           /* media; root entries, total16 and fatsz16 stay 0 on FAT32 */
    put16(b + 24, 63);
    put16(b + 26, 255);
    put32(b + 28, (u32)(v->disk_off / v->bps));             /* hidden sectors: the partition's start */
    put32(b + 32, (u32)v->total);
    put32(b + 36, v->fatsz);
    put32(b + 44, v->root_cluster);
    put16(b + 48, 1);                                       /* FSInfo sector */
    put16(b + 50, 6);                                       /* backup boot sector */
    b[64] = 0x80;
    b[66] = 0x29;
    put32(b + 67, v->volid);
    memcpy(b + 71, v->label, 11);
    memcpy(b + 82, "FAT32   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
    u8 *fsi = r + v->bps;
    put32(fsi, 0x41615252);
    put32(fsi + 484, 0x61417272);
    put32(fsi + 488, v->nclusters - (v->next_free - 2));
    put32(fsi + 492, v->next_free);
    put32(fsi + 508, 0xAA550000);
    memcpy(r + 6 * (size_t)v->bps, r, 2 * (size_t)v->bps);    /* backup boot sector and FSInfo at 6 and 7 */
    int rc = write_at(fd, v->disk_off + v->bps, r + v->bps, rbytes - v->bps);
    if (!rc) rc = write_at(fd, v->disk_off, r, v->bps);
    free(r);
    return rc;
}

/* ------------------------------------------------------------------------------------------------ the boot files */

typedef struct { const char *path; const char *name11; u8 ntres; u8 *data; size_t size; u32 first; } SrcFile;

static u8 *slurp(const char *path, size_t *n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > (off_t)(48 * MIB)) { close(fd); errno = EINVAL; return NULL; }
    u8 *buf = malloc((size_t)st.st_size + 1);
    if (!buf) { close(fd); return NULL; }
    size_t done = 0;
    while (done < (size_t)st.st_size) {
        ssize_t r = read(fd, buf + done, (size_t)st.st_size - done);
        if (r <= 0) break;
        done += (size_t)r;
    }
    close(fd);
    if (done != (size_t)st.st_size) { free(buf); errno = EIO; return NULL; }
    buf[done] = 0;
    *n = done;
    return buf;
}

/* cmdline.txt of the new system: BASE without any blk.rw= word, then blk.rw=<the new HUMANVILLAGE partition's GUID>, so blk.ro keeps
 * every other SATA and NVMe disk, and the installed ESP, read-only while the installed homes can be written. */
static int make_cmdline(const char *base, const char *hv_guid, char *out, size_t cap) {
    out[0] = 0;
    char word[256];
    const char *p = base;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        size_t n = 0;
        while (p[n] && !isspace((unsigned char)p[n])) n++;
        if (!n) break;
        if (n < sizeof word) {
            memcpy(word, p, n);
            word[n] = 0;
            if (strncmp(word, "blk.rw=", 7)) {
                if (out[0]) strlcat(out, " ", cap);
                strlcat(out, word, cap);
            }
        }
        p += n;
    }
    if (out[0]) strlcat(out, " ", cap);
    strlcat(out, "blk.rw=", cap);
    if (strlcat(out, hv_guid, cap) >= cap) return -1;
    return strlen(out) < 240 ? 0 : -1;                      /* the loader hands the kernel at most 255 characters */
}

/* ------------------------------------------------------------------------------------------------ the GPT */

static int write_gpt(int fd, u64 total, unsigned ss, const Span *esp, const Span *hv, const u8 disk_guid[16], const u8 esp_guid[16],
                     const u8 hv_guid[16]) {
    size_t abytes = GPT_ENTRIES * GPT_ESIZE;
    u64 arr = (abytes + ss - 1) / ss;
    u8 *ents = calloc(1, (size_t)arr * ss);
    u8 *hdr = calloc(1, ss);
    u8 *mbr = calloc(1, ss);
    if (!ents || !hdr || !mbr) { free(ents); free(hdr); free(mbr); return -1; }
    const struct { const u8 *type, *uniq; const Span *s; const char *name; } part[2] = {
        { GUID_ESP, esp_guid, esp, "TOUHOUOS-ESP" }, { GUID_BASIC, hv_guid, hv, "HUMANVILLAGE" } };
    for (int i = 0; i < 2; i++) {
        u8 *e = ents + i * GPT_ESIZE;
        memcpy(e, part[i].type, 16);
        memcpy(e + 16, part[i].uniq, 16);
        put64(e + 32, part[i].s->first);
        put64(e + 40, part[i].s->first + part[i].s->count - 1);
        for (int k = 0; part[i].name[k]; k++) put16(e + 56 + 2 * k, (u16)part[i].name[k]);
    }
    u32 ecrc = crc32_of(ents, abytes);
    u64 first_usable = 2 + arr, last_usable = total - 2 - arr, backup_entries = total - 1 - arr;
    int rc = 0;
    for (int which = 1; which >= 0 && !rc; which--) {        /* the backup first, the primary last */
        u64 my = which ? total - 1 : 1, alt = which ? 1 : total - 1, elba = which ? backup_entries : 2;
        memset(hdr, 0, ss);
        memcpy(hdr, "EFI PART", 8);
        put32(hdr + 8, 0x00010000);
        put32(hdr + 12, 92);
        put64(hdr + 24, my);
        put64(hdr + 32, alt);
        put64(hdr + 40, first_usable);
        put64(hdr + 48, last_usable);
        memcpy(hdr + 56, disk_guid, 16);
        put64(hdr + 72, elba);
        put32(hdr + 80, GPT_ENTRIES);
        put32(hdr + 84, GPT_ESIZE);
        put32(hdr + 88, ecrc);
        put32(hdr + 16, crc32_of(hdr, 92));
        g_what = which ? "the backup GPT partition entries" : "the primary GPT partition entries";
        rc = write_at(fd, elba * ss, ents, (size_t)arr * ss);
        g_what = which ? "the backup GPT header (last sector)" : "the primary GPT header (sector 1)";
        if (!rc) rc = write_at(fd, my * ss, hdr, ss);
    }
    if (!rc) {                                              /* protective MBR: one 0xEE entry over the whole disk */
        u8 *e = mbr + 446;
        e[1] = 0x00; e[2] = 0x02; e[3] = 0x00;
        e[4] = 0xEE;
        e[5] = 0xFF; e[6] = 0xFF; e[7] = 0xFF;
        put32(e + 8, 1);
        put32(e + 12, total - 1 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)(total - 1));
        mbr[510] = 0x55; mbr[511] = 0xAA;
        g_what = "the protective MBR (sector 0)";
        rc = write_at(fd, 0, mbr, ss);
    }
    free(ents); free(hdr); free(mbr);
    return rc;
}

/* ------------------------------------------------------------------------------------------------ the applet */

static char g_srcmnt[64];                                   /* the boot ESP, mounted read-only by us (umounted at the end) */

static void cleanup_source(void) {
    if (g_srcmnt[0]) {
        gap_umount(g_srcmnt);
        rmdir(g_srcmnt);
        g_srcmnt[0] = 0;
    }
}

static int refuse(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int refuse(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "install: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\ninstall: nothing was written.\n");
    va_end(ap);
    cleanup_source();
    return 1;
}

static void usage(void) {
    fprintf(stderr, "usage: install [--force] [--yes-i-typed=DISK] [--erase-phrase=SENTENCE] [--from DIR] [--cmdline WORDS] [--dry-run] DISK\n"
                    "       install            (lists the disks and what install thinks of each)\n");
}

static void list_disks(const Boot *b) {
    printf("%-10s %9s  %-24s %s\n", "DISK", "SIZE", "MODEL", "STATE");
    for (int i = 0; i < g_nrows; i++) {
        Row *d = &g_rows[i];
        if (strcmp(d->parent, "-")) continue;
        char sz[24], state[200] = "", path[48];
        mib_text(d->size, sz, sizeof sz);
        snprintf(path, sizeof path, "/dev/%s", d->name);
        if (b->found && !strcmp(b->disk, d->name)) strlcpy(state, "the system was started from this disk", sizeof state);
        else if (mounted(d->name, 1, NULL, 0, NULL, 0)) strlcpy(state, "in use (mounted)", sizeof state);
        else {
            int fd = open(path, O_RDONLY);
            char what[400] = "", w[96];
            Lost lost[MAX_LOST];
            int nlost = 0, os = fd >= 0 ? other_systems(fd, d, lost, MAX_LOST, &nlost) : 0;
            os_words(os, w, sizeof w);
            if (fd >= 0 && disk_content(fd, d, what, sizeof what))
                snprintf(state, sizeof state, os ? "holds %s (protected: needs --force and a typed sentence): %.110s" : "holds %s%.150s (needs --force)",
                         os ? w : "", what);
            else if (fd < 0) snprintf(state, sizeof state, "cannot be read: %s", strerror(errno));
            else strlcpy(state, "empty", sizeof state);
            if (fd >= 0) close(fd);
        }
        printf("%-10s %9s  %-24.24s %s%s\n", d->name, sz, strcmp(d->model, "-") ? d->model : "", state, d->ro ? " [read-only: blk.ro]" : "");
    }
    if (!b->info) printf("(the firmware did not say which disk the system was started from)\n");
}

APPLET(install) {
    const char *typed = NULL, *from = NULL, *cmdline = NULL, *hold = NULL, *target = NULL, *erase = NULL;
    int force = 0, dry = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--force")) force = 1;
        else if (!strcmp(a, "--dry-run") || !strcmp(a, "-n")) dry = 1;
        else if (!strncmp(a, "--yes-i-typed=", 14)) typed = a + 14;
        else if (!strncmp(a, "--erase-phrase=", 15)) erase = a + 15;
        else if (!strcmp(a, "--from") && i + 1 < argc) from = argv[++i];
        else if (!strcmp(a, "--cmdline") && i + 1 < argc) cmdline = argv[++i];
        else if (!strncmp(a, "--hold=", 7)) hold = a + 7;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (a[0] == '-') { usage(); return 2; }
        else if (!target) target = a;
        else { usage(); return 2; }
    }
    load_rows();
    Boot boot;
    find_boot(&boot);
    if (!target) { list_disks(&boot); return 0; }
    if (!strncmp(target, "/dev/", 5)) target += 5;

    /* ---- every check comes before anything is written */
    if (geteuid() != 0) return refuse("only root can install a system (it overwrites a whole disk): try 'sudo install %s'", target);
    Row *d = row(target);
    if (!d) return refuse("there is no disk called %s (see 'install' or 'lsblk')", target);
    if (strcmp(d->parent, "-")) return refuse("%s is a partition of %s: name the whole disk, everything on it is replaced", target, d->parent);
    if (d->ss != 512 && d->ss != 4096) return refuse("%s has %u-byte sectors; only 512 and 4096 are supported", target, d->ss);
    if (boot.found && !strcmp(boot.disk, target))
        return refuse("%s is the disk this system was started from (%s holds the running loader): refusing to overwrite it", target, boot.part);
    for (int i = 0; i < boot.nsuspects; i++)
        if (!strcmp(boot.suspects[i], target))
            return refuse("%s has a partition in the very place the system was started from, and it could not be ruled out that it is that disk", target);
    if (!boot.info && force)
        return refuse("the firmware did not say which disk the system was started from, so --force cannot be allowed (it might be that disk)");
    char src[96], where[160];
    if (mounted(target, 1, src, sizeof src, where, sizeof where)) return refuse("%s is mounted on %s: umount it first", src, where);
    char path[48];
    snprintf(path, sizeof path, "/dev/%s", target);
    int rfd = open(path, O_RDONLY);
    if (rfd < 0) return refuse("%s: %s", path, strerror(errno));
    char what[600] = "";
    int busy = disk_content(rfd, d, what, sizeof what);
    /* Another operating system on it (Windows, Linux, ...): --force is not enough, a typed sentence that names the systems, the model and the
     * disk is needed as well (--erase-phrase=, or at the prompt). */
    static Lost lost[MAX_LOST];
    int nlost = 0;
    int os = other_systems(rfd, d, lost, MAX_LOST, &nlost);
    close(rfd);
    char osw[96], sentence[200];
    os_words(os, osw, sizeof osw);
    erase_sentence(os, d, target, sentence, sizeof sentence);
    if (os) {
        print_losses(target, d, lost, nlost, os);
        fflush(stdout);
        if (!force)
            return refuse("%s holds another operating system (%s). install erases it only with --force and this sentence, typed\n"
                          "install: exactly (see 'man install'):\ninstall:   %s", target, osw, sentence);
        if (erase && strcmp(erase, sentence))
            return refuse("--erase-phrase does not match. To erase %s on %s the sentence is exactly:\ninstall:   %s", osw, target, sentence);
        if (!erase && typed)
            return refuse("%s holds %s: --force and --yes-i-typed are not enough. Type the sentence at the prompt, or give it with\n"
                          "install:   --erase-phrase='%s'", target, osw, sentence);
    } else if (erase) {
        return refuse("--erase-phrase is only for a disk that holds another operating system, and %s holds none", target);
    }
    if (busy && !force) return refuse("%s is not empty: it holds %s. Give --force to overwrite it anyway", target, what);

    u64 total = d->size / d->ss;
    Span esp, hv;
    if (compute_layout(total, d->ss, &esp, &hv)) return refuse("%s is too small (%llu MiB) for a 64 MiB boot partition and a FAT32 volume", target,
                                                               (unsigned long long)(d->size >> 20));
    Fat fe, fh;
    if (fat_init(&fe, esp.first * d->ss, esp.count, d->ss, "TOUHOUOS-ESP") || fat_init(&fh, hv.first * d->ss, hv.count, d->ss, "HUMANVILLAGE"))
        return refuse("%s is too small for two FAT32 volumes with %u-byte sectors", target, d->ss);

    /* ---- the files of the running system */
    char dir[160];
    if (from) strlcpy(dir, from, sizeof dir);
    else {
        if (!boot.found) return refuse("cannot tell which partition the system was started from (see /proc/bootdisk): name the boot files with --from DIR");
        if (!mounted(boot.part, 0, NULL, 0, dir, sizeof dir)) {
            snprintf(g_srcmnt, sizeof g_srcmnt, "/BambooForest/install-esp.%d", (int)getpid());
            mkdir(g_srcmnt, 0700);
            snprintf(src, sizeof src, "/dev/%s", boot.part);
            if (gap_mount_opts(src, g_srcmnt, "fat32", MNT_RDONLY, NULL) < 0) {
                int e = errno;
                rmdir(g_srcmnt);
                g_srcmnt[0] = 0;
                return refuse("cannot mount the boot partition /dev/%s read-only: %s", boot.part, strerror(e));
            }
            strlcpy(dir, g_srcmnt, sizeof dir);
        }
    }
    SrcFile files[4] = {
        { "EFI/BOOT/BOOTX64.EFI", "BOOTX64 EFI", 0x00, NULL, 0, 0 },
        { "kernel.elf", "KERNEL  ELF", 0x18, NULL, 0, 0 },
        { "initrd.img", "INITRD  IMG", 0x18, NULL, 0, 0 },
        { "cmdline.txt", "CMDLINE TXT", 0x18, NULL, 0, 0 },
    };
    for (int i = 0; i < 3; i++) {
        char *p = kb_join(dir, files[i].path);
        files[i].data = slurp(p, &files[i].size);
        if (!files[i].data) { int e = errno; free(p); return refuse("%s/%s: %s (the boot files are not all there)", dir, files[i].path, strerror(e)); }
        free(p);
    }
    if (files[0].size < 2 || memcmp(files[0].data, "MZ", 2)) return refuse("%s/%s is not a UEFI program", dir, files[0].path);
    if (files[1].size < 4 || memcmp(files[1].data, "\177ELF", 4)) return refuse("%s/%s is not an ELF file", dir, files[1].path);
    char base[256] = "video=native blk.ro";
    if (cmdline) strlcpy(base, cmdline, sizeof base);
    else {
        char *p = kb_join(dir, "cmdline.txt");
        size_t n = 0;
        u8 *old = slurp(p, &n);
        free(p);
        if (old) {
            for (size_t k = 0; k < n; k++) if (old[k] < 32 || old[k] > 126) old[k] = ' ';
            if (strspn((char *)old, " ") < n) strlcpy(base, (char *)old, sizeof base);
            free(old);
        }
    }
    cleanup_source();                                       /* everything is in memory now */
    u8 disk_guid[16], esp_guid[16], hv_guid[16];
    random_guid(disk_guid);
    random_guid(esp_guid);
    random_guid(hv_guid);
    char dg[40], hg[40], newcmd[300];
    guid_text(disk_guid, dg, sizeof dg);
    guid_text(hv_guid, hg, sizeof hg);
    if (make_cmdline(base, hg, newcmd, sizeof newcmd)) return refuse("the command line is too long for the loader: '%s'", base);
    strlcat(newcmd, "\n", sizeof newcmd);
    files[3].data = (u8 *)strdup(newcmd);
    files[3].size = strlen(newcmd);

    /* ---- the plan */
    char s1[24], s2[24], s3[24];
    printf("install: the shrine is moved onto %s (%s%s%s, %s, %u-byte sectors). EVERYTHING ON IT IS LOST.\n", target, mib_text(d->size, s1, sizeof s1),
           strcmp(d->model, "-") ? ", " : "", strcmp(d->model, "-") ? d->model : "", d->driver, d->ss);
    if (busy) printf("install: --force: it holds %s%s%s\n", os ? osw : "", os ? ": " : "", what);
    printf("install:   partition 1  TOUHOUOS-ESP  %s  EFI System Partition: EFI/BOOT/BOOTX64.EFI, kernel.elf, initrd.img, cmdline.txt\n",
           mib_text(esp.count * d->ss, s2, sizeof s2));
    printf("install:   partition 2  HUMANVILLAGE  %s  FAT32, %u-byte clusters, empty (the home directories are made at the first start)\n",
           mib_text(hv.count * d->ss, s3, sizeof s3), fh.cluster_bytes);
    printf("install:   files from %s; cmdline.txt: %s", from ? from : "the boot partition", newcmd);
    if (dry) {
        if (os) printf("install: --dry-run: going on would need this sentence, typed exactly: %s\n", sentence);
        printf("install: --dry-run: stopping here, nothing was written.\n");
        return 0;
    }

    /* ---- the user types the sentence (another operating system on the disk) or the name */
    if (os) {
        if (typed && strcmp(typed, target)) return refuse("--yes-i-typed=%s does not name the disk %s", typed, target);
        if (!erase) {
            int tty = open("/dev/tty", O_RDWR);
            if (tty < 0) return refuse("no terminal to ask on: give the sentence with --erase-phrase='%s' if you mean it", sentence);
            char ask[512], answer[256];
            int an = snprintf(ask, sizeof ask, "install: this erases %s on %s. To go on, type this sentence exactly (anything else stops):\n"
                                               "install:   %s\n> ", osw, target, sentence);
            if (an < 0 || an >= (int)sizeof ask) an = (int)strlen(ask);
            fflush(stdout);
            write(tty, ask, (size_t)an);
            ssize_t r = read(tty, answer, sizeof answer - 1);
            close(tty);
            if (r < 0) r = 0;
            answer[r] = 0;
            answer[strcspn(answer, "\r\n")] = 0;
            if (strcmp(answer, sentence)) return refuse("you typed '%s', not the sentence '%s'", answer, sentence);
        }
        printf("install: the sentence matches: %s on %s will be erased\n", osw, target);
    } else if (typed) {
        if (strcmp(typed, target)) return refuse("--yes-i-typed=%s does not name the disk %s", typed, target);
    } else {
        int tty = open("/dev/tty", O_RDWR);
        if (tty < 0) return refuse("no terminal to ask on: type the disk's name with --yes-i-typed=%s if you mean it", target);
        char ask[160], answer[64];
        int an = snprintf(ask, sizeof ask, "Type the disk's name (%s) to go on, anything else stops: ", target);
        fflush(stdout);
        write(tty, ask, (size_t)an);
        ssize_t r = read(tty, answer, sizeof answer - 1);
        close(tty);
        if (r < 0) r = 0;
        answer[r] = 0;
        answer[strcspn(answer, "\r\n")] = 0;
        if (strcmp(answer, target)) return refuse("you typed '%s', not '%s'", answer, target);
    }

    /* ---- the one disk is opened, unlocked for this file only, written, read back, locked again */
    int fd = open(path, O_RDWR);
    if (fd < 0) return refuse("%s: %s", path, strerror(errno));
    struct gblk_unlock u;
    memset(&u, 0, sizeof u);
    strlcpy(u.name, target, sizeof u.name);
    int state = ioctl(fd, BLKWRSTATE, &u);
    if (state >= 0 && (state & BLKWR_READONLY) && !(state & BLKWR_POLICY)) { close(fd); return refuse("%s refuses writes (write-protected)", target); }
    if (ioctl(fd, BLKWRUNLOCK, &u) < 0) { int e = errno; close(fd); return refuse("the kernel would not unlock %s for writing: %s", target, strerror(e)); }
    if (hold) {                                             /* test knob: the unlock is held until HOLD exists, so a test can look around */
        printf("install: holding the write unlock of %s until %s exists\n", target, hold);
        fflush(stdout);
        struct stat st;
        for (int t = 0; t < 3000 && stat(hold, &st); t++) usleep(100000);
    }
    g_zero = calloc(1, MIB);
    if (!g_zero) kb_die("out of memory");
    gap_log("install: writing TouhouOS onto the disk named on the command line");
    int rc = 0;
    /* The ESP: directories first (root, EFI, EFI/BOOT), then the files, contiguous, as mkdisk lays them out. */
    u32 croot = fat_alloc(&fe, 1), cefi = fat_alloc(&fe, 1), cboot = fat_alloc(&fe, 1);
    fe.root_cluster = croot;
    u64 room = 0;
    for (int i = 0; i < 4; i++) room += (files[i].size + fe.cluster_bytes - 1) / fe.cluster_bytes;
    if (room + 5 > fe.nclusters) { rc = -1; errno = ENOSPC; }
    for (int i = 0; i < 4 && !rc; i++) {
        files[i].first = fat_alloc(&fe, (u32)((files[i].size + fe.cluster_bytes - 1) / fe.cluster_bytes));
        if (files[i].size && !files[i].first) { rc = -1; errno = ENOSPC; }
    }
    char p1[40], p2[40];                                    /* the names the kernel will give them: sdb1, nvme0n1p1 */
    const char *sep = isdigit((unsigned char)target[strlen(target) - 1]) ? "p" : "";
    snprintf(p1, sizeof p1, "%s%s1", target, sep);
    snprintf(p2, sizeof p2, "%s%s2", target, sep);
    printf("install: writing the EFI System Partition %s\n", p1);
    fflush(stdout);
    u8 *dbuf = calloc(1, fe.cluster_bytes);
    if (!dbuf) kb_die("out of memory");
    g_what = fe.what_dir;
    if (!rc) {
        dirent83(dbuf, "TOUHOUOS-ES", 0x28, 0, 0, 0);
        dirent83(dbuf + 32, "EFI        ", 0x10, 0, cefi, 0);
        for (int i = 1; i < 4; i++) dirent83(dbuf + 32 * (i + 1), files[i].name11, 0x20, files[i].ntres, files[i].first, (u32)files[i].size);
        rc = write_cluster_data(fd, &fe, croot, dbuf, fe.cluster_bytes, 0);
    }
    if (!rc) {
        memset(dbuf, 0, fe.cluster_bytes);
        dirent83(dbuf, ".          ", 0x10, 0, cefi, 0);
        dirent83(dbuf + 32, "..         ", 0x10, 0, 0, 0);
        dirent83(dbuf + 64, "BOOT       ", 0x10, 0, cboot, 0);
        rc = write_cluster_data(fd, &fe, cefi, dbuf, fe.cluster_bytes, 0);
    }
    if (!rc) {
        memset(dbuf, 0, fe.cluster_bytes);
        dirent83(dbuf, ".          ", 0x10, 0, cboot, 0);
        dirent83(dbuf + 32, "..         ", 0x10, 0, cefi, 0);
        dirent83(dbuf + 64, files[0].name11, 0x20, files[0].ntres, files[0].first, (u32)files[0].size);
        rc = write_cluster_data(fd, &fe, cboot, dbuf, fe.cluster_bytes, 0);
    }
    free(dbuf);
    for (int i = 0; i < 4 && !rc; i++) {
        g_what = files[i].path;
        rc = write_cluster_data(fd, &fe, files[i].first, files[i].data, files[i].size, 1);
    }
    if (!rc) rc = fat_finish(fd, &fe);
    /* HUMANVILLAGE: an empty root directory holding the volume label. */
    if (!rc) {
        printf("install: writing the HUMANVILLAGE volume %s\n", p2);
        fflush(stdout);
        fh.root_cluster = fat_alloc(&fh, 1);
        u8 *rb = calloc(1, fh.cluster_bytes);
        if (!rb) kb_die("out of memory");
        dirent83(rb, "HUMANVILLAG", 0x28, 0, 0, 0);
        g_what = fh.what_dir;
        rc = write_cluster_data(fd, &fh, fh.root_cluster, rb, fh.cluster_bytes, 0);
        free(rb);
        if (!rc) rc = fat_finish(fd, &fh);
    }
    if (!rc) {
        printf("install: writing the GPT partition table\n");
        fflush(stdout);
        rc = write_gpt(fd, total, d->ss, &esp, &hv, disk_guid, esp_guid, hv_guid);
    }
    int werr = g_fail_off != ~0ull ? g_fail_errno : errno;
    /* The disk's write cache has to reach the medium before anything is read back; a failing flush means writes may be lost. */
    int flush_err = 0;
    if (!rc && ioctl(fd, BLKFLSBUF, NULL) < 0) flush_err = errno;
    ioctl(fd, BLKWRLOCK, &u);                               /* nothing below writes: hand the unlock back at once */

    /* ---- read back every byte that was written (files, directories, FATs, boot sectors, both GPTs, the MBR) and compare */
    int bad = 0;
    u64 checked = 0;
    if (!rc && !flush_err) {
        printf("install: reading back and comparing everything that was written\n");
        fflush(stdout);
        bad = read_back(fd, d->ss, &checked);
    }
    close(fd);
    sync();
    if (rc) {
        if (g_fail_off != ~0ull)
            fprintf(stderr, "install: writing %s failed at byte %llu (sector %llu, %s): %s.\n", target, (unsigned long long)g_fail_off,
                    (unsigned long long)(g_fail_off / d->ss), g_fail_what, strerror(werr));
        else
            fprintf(stderr, "install: writing %s failed: %s.\n", target, strerror(werr));
        fprintf(stderr, "install: TouhouOS is NOT installed. The partition table is written last, so %s does not look like a finished install,\n"
                        "install: but what was on it before is partly overwritten. Nothing else was touched. dmesg has the disk's own error.\n", target);
        gap_log("install: FAILED: a write to the target disk failed");
        return 1;
    }
    if (flush_err) {
        fprintf(stderr, "install: flushing the write cache of %s failed: %s. The disk may not hold what was written.\n"
                        "install: TouhouOS is NOT reliably installed: do not start from %s. dmesg has the disk's own error.\n", target, strerror(flush_err),
                target);
        gap_log("install: FAILED: the cache flush of the target disk failed");
        return 1;
    }
    if (bad) {
        fprintf(stderr, "install: *** %s DOES NOT READ BACK AS WRITTEN ***\n"
                        "install: %d of %d written areas differ or cannot be read (listed above): the disk, its cable or this system's driver for\n"
                        "install: it lost or changed data. TouhouOS is NOT reliably installed: do not start from %s. Try once more; if it happens\n"
                        "install: again, do not trust this disk with this system.\n", target, bad, g_next, target);
        gap_log("install: FAILED: the target disk does not read back as written");
        return 1;
    }
    printf("install: every byte written was read back and matches (%llu KiB in %d areas).\n", (unsigned long long)((checked + 1023) / 1024), g_next);
    printf("install: TouhouOS is installed on %s.\n", target);
    printf("install: %s TOUHOUOS-ESP (boot files, read-only under blk.ro), %s HUMANVILLAGE (homes, writable: cmdline.txt says blk.rw=%s).\n", p1, p2, hg);
    printf("install: the new partitions show up here after a restart. Take the stick out, restart, and pick the disk in the firmware's\n"
           "install: boot menu (F12 on the Dell) if it does not start by itself.\n");
    char msg[160];
    snprintf(msg, sizeof msg, "install: TouhouOS installed on %s (GPT disk GUID %s)", target, dg);
    gap_log(msg);
    return 0;
}
