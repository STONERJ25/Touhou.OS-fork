/*
 * Unpacks the initrd the loader handed over into the (tmpfs) root filesystem.
 *
 * The initrd is a ustar archive, either as it is or as one gzip member holding it (docs/UPDATE.md, "The compressed initrd": tools/build.py writes the gzip form
 * unless TOUHOU_INITRD=plain is set; the loader does not care, it loads the file whole and hands over its address and size). Both forms go through the same reader,
 * which takes the archive in pieces of any size and writes each regular file to the file system as its bytes arrive: a plain archive is given to it in one piece,
 * straight from the loader's memory; a compressed one is given to it by the inflate (kernel/lib/inflate.c) a window's worth at a time as it comes out.
 *
 * So the unpacked archive is never held in memory as a whole. It could not be: the page allocator's largest block is 16 MiB (PMM_MAX_ORDER) and the archive is
 * bigger than that already, and a requirement of "one contiguous run for all of it" would only get harder as the image grows. The compressed file is the one
 * large buffer, and main.c gives its pages back to the allocator when this function returns, as it always did for the plain archive.
 *
 * A compressed stream that is damaged does not get as far as the shell: the CRC-32 and length in its trailer are only checked after the last byte, so by then
 * the files are in the file system, but the answer is panic() (a clear line on the screen, the serial port and in the record that survives the restart; on a start
 * under trial the machine restarts by itself 30 seconds later, and after the second such start the loader brings the previous kernel and initrd back, docs/UPDATE.md).
 * A plain archive that is cut short or damaged was never treated like that and still is not (the files that came out are used, one line says what is wrong): only
 * something that is neither a gzip stream nor a tar archive at all is refused.
 */
#include "vfs.h"
#include "kernel.h"
#include "inflate.h"
#include "cpu.h"
#include "irq.h"
#include "mm.h"

#define INITRD_MAX_UNPACKED (256ull << 20)       /* a gzip stream that unpacks to more than this is a bomb (and an image that is bigger needs a better plan than this one) */

static u64 parse_octal(const char *s, size_t n) {
    u64 v = 0;
    for (size_t i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++) v = v * 8 + (u64)(s[i] - '0');
    return v;
}

static void parent_of(const char *path, char *out, size_t n) {
    strlcpy(out, path, n);
    char *slash = strrchr(out, '/');
    if (slash && slash != out) *slash = 0;
    else strlcpy(out, "/", n);
}

/* ------------------------------------------------------------------------------------------------ the tar reader */

enum { TS_HEADER, TS_DATA, TS_PAD, TS_END };

typedef struct {
    int state;
    u8 hdr[512];
    size_t have;                    /* bytes of hdr collected so far (TS_HEADER) */
    u64 remain;                     /* TS_DATA: bytes of the entry's data still to come; TS_PAD: bytes of padding still to skip */
    u64 pad;                        /* the padding that follows the data of this entry (up to the next 512-byte block) */
    File *file;                     /* the regular file being written; NULL while the data of some other kind of entry (or of a file that could not be created) goes by */
    bool wfail;
    char path[320];                 /* of the entry in progress, for messages */
    u64 entries;
    int files, dirs, links;
    u64 bytes;
} Tar;

static void tar_init(Tar *t) {
    memset(t, 0, sizeof *t);
    t->state = TS_HEADER;
}

static void tar_close(Tar *t) {
    if (t->file) file_put(t->file);
    t->file = NULL;
}

/* The header in t->hdr is complete: act on it and decide what the bytes that follow are. */
static void tar_entry(Tar *t) {
    const char *h = (const char *)t->hdr;
    if (h[0] == 0) { t->state = TS_END; return; }               /* end-of-archive marker */
    if (t->entries++ == 0 && memcmp(h + 257, "ustar", 5))       /* the first header says what this is: a stream of anything else is not an archive */
        panic("initrd.img is not a tar archive (the first header has no 'ustar' mark)");

    char name[300];
    char prefix[156];
    memcpy(prefix, h + 345, 155);
    prefix[155] = 0;
    char shortname[101];
    memcpy(shortname, h, 100);
    shortname[100] = 0;
    if (prefix[0]) snprintf(name, sizeof name, "%s/%s", prefix, shortname);
    else strlcpy(name, shortname, sizeof name);

    u64 fsize = parse_octal(h + 124, 12);
    u32 mode = (u32)parse_octal(h + 100, 8);
    char type = h[156];
    t->remain = fsize;
    t->pad = ALIGN_UP(fsize, 512) - fsize;
    tar_close(t);
    t->wfail = false;

    char *n = name;
    while (n[0] == '.' && n[1] == '/') n += 2;
    while (*n == '/') n++;
    size_t len = strlen(n);
    while (len && n[len - 1] == '/') n[--len] = 0;
    if (*n && strcmp(n, ".")) {
        snprintf(t->path, sizeof t->path, "/%s", n);
        if (type == '5') {
            vfs_mkdir_p(t->path, mode & 0777);
            vfs_chmod(t->path, mode & 07777, NULL, NULL);
            t->dirs++;
        } else if (type == '0' || type == 0) {
            char parent[320];
            parent_of(t->path, parent, sizeof parent);
            vfs_mkdir_p(parent, 0755);
            int rc = vfs_open(t->path, O_WRONLY | O_CREAT | O_TRUNC, mode & 07777, NULL, NULL, &t->file);
            if (rc) {
                klog("Keine", "initrd: cannot write %s (%d)", t->path, rc);
                t->file = NULL;
            }
            t->files++;
            t->bytes += fsize;
        } else if (type == '2') {
            char link[101];
            memcpy(link, h + 157, 100);
            link[100] = 0;
            char parent[320];
            parent_of(t->path, parent, sizeof parent);
            vfs_mkdir_p(parent, 0755);
            vfs_symlink(link, t->path, NULL, NULL);
            t->links++;
        }
    }
    if (t->remain) t->state = TS_DATA;
    else {
        tar_close(t);                                           /* (an empty file exists once it has been created) */
        t->state = t->pad ? TS_PAD : TS_HEADER;
        t->remain = t->pad;
    }
}

static void tar_write(Tar *t, const u8 *p, size_t n) {
    while (t->file && n) {
        ssize_t w = vfs_write(t->file, p, n);
        if (w <= 0) {                                           /* no room left (the tmpfs keeps half of RAM for everything else): say so once, drop the rest of this file */
            if (!t->wfail) klog("Keine", "initrd: cannot write %s (%d)", t->path, (int)w);
            t->wfail = true;
            tar_close(t);
            return;
        }
        p += w;
        n -= (size_t)w;
    }
}

/* Takes the next n bytes of the archive. */
static void tar_feed(Tar *t, const u8 *p, size_t n) {
    while (n && t->state != TS_END) {
        if (t->state == TS_HEADER) {
            size_t take = MIN(n, sizeof t->hdr - t->have);
            memcpy(t->hdr + t->have, p, take);
            t->have += take;
            p += take;
            n -= take;
            if (t->have == sizeof t->hdr) {
                t->have = 0;
                tar_entry(t);
            }
        } else {
            size_t take = (size_t)MIN((u64)n, t->remain);
            if (t->state == TS_DATA) tar_write(t, p, take);
            p += take;
            n -= take;
            t->remain -= take;
            if (!t->remain) {
                if (t->state == TS_DATA) {
                    tar_close(t);
                    t->state = t->pad ? TS_PAD : TS_HEADER;
                    t->remain = t->pad;
                } else {
                    t->state = TS_HEADER;
                }
            }
        }
    }
}

/* The input is over: close what is open, and say so if an entry's data did not all come. (An archive whose last padding is missing is fine; it always was.) */
static void tar_finish(Tar *t) {
    if (t->state == TS_DATA) klog("Keine", "initrd: truncated archive (%s is missing %lu bytes)", t->path, (unsigned long)t->remain);
    else if (t->state == TS_HEADER && t->have) klog("Keine", "initrd: truncated archive (a header of %lu bytes at the end)", (unsigned long)t->have);
    tar_close(t);
}

/* ------------------------------------------------------------------------------------------------ the two forms */

typedef struct {
    Tar tar;
    u64 sink_cycles;                /* time spent in the reader and the file system (the rest of the time is the inflate) */
} GzCtx;

static int gz_sink(void *ctx, const u8 *data, size_t n) {
    GzCtx *c = ctx;
    u64 t0 = rdtsc();
    tar_feed(&c->tar, data, n);
    c->sink_cycles += rdtsc() - t0;
    return 0;
}

/* Milliseconds for a number of TSC ticks. The clock the TSC gives is the honest one here: uptime_ms() counts timer interrupts, and a virtual machine on a busy
 * host loses some of them (the same boot said 337 ms by the uptime and 558 ms by the TSC). */
static unsigned long cycles_to_ms(u64 cycles) {
    u64 mhz = timer_tsc_mhz();
    return mhz ? (unsigned long)(cycles / (mhz * 1000)) : 0;
}

void initrd_unpack(const void *img, size_t size) {
    const u8 *b = img;
    const bool gzip = size >= 2 && b[0] == 0x1F && b[1] == 0x8B;
    const u64 t0 = uptime_ms(), c0 = rdtsc();
    Tar plain;
    GzCtx gz;
    GzInfo info = { 0 };
    Tar *tar = &plain;

    if (gzip) {
        size_t wsize = GZ_WORK_128K;                            /* (a smaller work area means more, smaller pieces and a little more copying; one of 64 KiB is plenty if memory is that short) */
        void *work = kmalloc(wsize);
        if (!work) { wsize = 64 * 1024; work = kmalloc(wsize); }
        if (!work) panic("initrd.img is gzip-compressed and there is no memory (%u KiB) for the decompressor", 64u);
        tar_init(&gz.tar);
        gz.sink_cycles = 0;
        int rc = gunzip(b, size, work, wsize, INITRD_MAX_UNPACKED, gz_sink, &gz, &info);
        kfree(work);
        if (rc)
            panic("initrd.img is damaged: %s%s%s (a gzip stream of %lu bytes, unusable at byte %lu, after %lu bytes of output)", gunzip_strerror(rc),
                  info.why ? ": " : "", info.why ? info.why : "", (unsigned long)size, (unsigned long)info.in_pos, (unsigned long)info.out_len);
        tar = &gz.tar;
    } else {
        tar_init(&plain);
        tar_feed(&plain, b, size);
    }
    tar_finish(tar);

    klog("Keine", "initrd unpacked: %d files (%lu KiB), %d directories, %d symlinks", tar->files, (unsigned long)(tar->bytes >> 10), tar->dirs, tar->links);
    const u64 all = rdtsc() - c0;
    const bool tsc = timer_tsc_mhz() != 0;
    const unsigned long ms = tsc ? cycles_to_ms(all) : (unsigned long)(uptime_ms() - t0);
    if (gzip && tsc) {
        klog("Keine", "initrd.img was gzip-compressed: %lu KiB -> %lu KiB (%lu%%), unpacked in %lu ms: the inflate with its CRC-32 %lu ms, the files %lu ms",
             (unsigned long)(size >> 10), (unsigned long)(info.out_len >> 10), (unsigned long)(info.out_len ? (u64)size * 100 / info.out_len : 0), ms,
             cycles_to_ms(all - gz.sink_cycles), cycles_to_ms(gz.sink_cycles));
    } else if (gzip) {
        klog("Keine", "initrd.img was gzip-compressed: %lu KiB -> %lu KiB (%lu%%), unpacked in %lu ms", (unsigned long)(size >> 10), (unsigned long)(info.out_len >> 10),
             (unsigned long)(info.out_len ? (u64)size * 100 / info.out_len : 0), ms);
    } else {
        klog("Keine", "initrd.img is a plain tar archive (%lu KiB), unpacked in %lu ms", (unsigned long)(size >> 10), ms);
    }
}
