/* disks: images in memory, never written back to their file (disks.h). */
#include "disks.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

uint8_t disk_slot_daua(int slot) {
    static const uint8_t da[DISK_SLOTS] = { 0x90, 0x91, 0x80, 0x81 };
    return slot >= 0 && slot < DISK_SLOTS ? da[slot] : 0;
}

const char *disk_slot_name(int slot) {
    static const char *const names[DISK_SLOTS] = { "floppy 1", "floppy 2", "hard disk 1", "hard disk 2" };
    return slot >= 0 && slot < DISK_SLOTS ? names[slot] : "?";
}

int disk_slot_is_hd(int slot) { return slot == SLOT_HD1 || slot == SLOT_HD2; }

int disk_slot_for(Pc98DiskFormat format, const DiskSlot slots[DISK_SLOTS]) {
    int first = format == PC98_DISK_HDI ? SLOT_HD1 : SLOT_FD1;
    return !slots[first].loaded ? first : !slots[first + 1].loaded ? first + 1 : first;
}

const char *disk_basename(const char *path) {
    const char *s = strrchr(path, '/');
    return s && s[1] ? s + 1 : path;
}

static int fail(char *err, size_t cap, const char *fmt, ...) {
    if (err && cap) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, cap, fmt, ap);
        va_end(ap);
    }
    return -1;
}

static uint32_t block_sum(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;                           /* FNV-1a */
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

void disk_unload(DiskSlot *s) {
    free(s->data);
    free(s->sums);
    memset(s, 0, sizeof *s);
}

static void take_sums(DiskSlot *s) {
    for (uint32_t b = 0; b < s->nblocks; b++) {
        size_t n = s->size - (size_t)b * DISK_BLOCK;
        s->sums[b] = block_sum(s->data + (size_t)b * DISK_BLOCK, n < DISK_BLOCK ? n : DISK_BLOCK);
    }
}

/* Takes ownership of `data` (malloc'd, `size` bytes): checks the format and fills the slot. */
static int adopt(DiskSlot *s, uint8_t *data, size_t size, const char *name, char *err, size_t errcap) {
    Pc98Disk d;
    int rc = pc98_disk_open(&d, data, size, PC98_DISK_NONE);
    if (rc == PC98_DISK_EFORMAT) {
        free(data);
        return fail(err, errcap, "this is not a PC-98 disk image (D88, FDI, HDI, or a raw floppy dump of 1232, 1200, 720 or 640 KiB)");
    }
    if (rc != PC98_DISK_OK) {
        free(data);
        return fail(err, errcap, "the image is damaged (a header field points outside the file)");
    }
    uint32_t nblocks = (uint32_t)((size + DISK_BLOCK - 1) / DISK_BLOCK);
    uint32_t *sums = malloc((size_t)nblocks * sizeof *sums);
    if (!sums) {
        free(data);
        return fail(err, errcap, "out of memory");
    }
    disk_unload(s);
    s->loaded = 1;
    s->data = data;
    s->size = size;
    s->disk = d;                                        /* (its data pointer is the one just passed, now owned by the slot) */
    s->sums = sums;
    s->nblocks = nblocks;
    snprintf(s->path, sizeof s->path, "%s", name ? name : "");
    take_sums(s);
    return 0;
}

int disk_load_file(DiskSlot *s, const char *path, char *err, size_t errcap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return fail(err, errcap, "%s", strerror(errno));
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        close(fd);
        return fail(err, errcap, "%s", strerror(e));
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return fail(err, errcap, "this is not a file");
    }
    if (st.st_size <= 0) {
        close(fd);
        return fail(err, errcap, "the file is empty");
    }
    if ((uint64_t)st.st_size > DISK_MAX_BYTES) {
        close(fd);
        return fail(err, errcap, "the file is larger than %u MiB, more than any PC-98 disk", DISK_MAX_BYTES >> 20);
    }
    size_t size = (size_t)st.st_size;
    uint8_t *data = malloc(size);
    if (!data) {
        close(fd);
        return fail(err, errcap, "out of memory");
    }
    size_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, data + got, size - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got != size) {
        free(data);
        return fail(err, errcap, "the file could not be read to its end");
    }
    return adopt(s, data, size, path, err, errcap);
}

int disk_load_memory(DiskSlot *s, const uint8_t *bytes, size_t size, const char *name, char *err, size_t errcap) {
    if (!size) return fail(err, errcap, "the image is empty");
    uint8_t *copy = malloc(size);
    if (!copy) return fail(err, errcap, "out of memory");
    memcpy(copy, bytes, size);
    return adopt(s, copy, size, name, err, errcap);
}

uint32_t disk_changed(const DiskSlot *s, uint32_t *first) {
    uint32_t n = 0;
    if (first) *first = 0;
    if (!s->loaded) return 0;
    for (uint32_t b = 0; b < s->nblocks; b++) {
        size_t left = s->size - (size_t)b * DISK_BLOCK;
        if (block_sum(s->data + (size_t)b * DISK_BLOCK, left < DISK_BLOCK ? left : DISK_BLOCK) == s->sums[b]) continue;
        if (!n && first) *first = b;
        n++;
    }
    return n;
}

int disk_save_as(DiskSlot *s, const char *path, char *err, size_t errcap) {
    if (!s->loaded) return fail(err, errcap, "there is no disk in this drive");
    if (!path || !*path) return fail(err, errcap, "no file name");
    /* O_EXCL: an existing file, the original image included, is never touched. */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST) return fail(err, errcap, "a file with that name exists already (a saved image is always a new file: choose another name)");
        return fail(err, errcap, "%s", strerror(errno));
    }
    size_t put = 0;
    while (put < s->size) {
        ssize_t n = write(fd, s->data + put, s->size - put);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        put += (size_t)n;
    }
    int e = errno;
    int closed = close(fd);
    if (put != s->size || closed != 0) {
        unlink(path);                                   /* no half an image is left behind */
        return fail(err, errcap, "the image could not be written completely (%s)", put != s->size ? strerror(e) : "close failed");
    }
    snprintf(s->path, sizeof s->path, "%s", path);
    take_sums(s);                                       /* what is on disk now is the new starting point */
    return 0;
}

/* ---------------------------------------------------------------- the virtual CONFIG.SYS */

/* TH04's CONFIG.SYS loads HIMEM.SYS and EMM386.EXE /UMB (and wants DOS=HIGH and DOS=UMB), and EMM386 needs protected mode and V86 mode, which
 * the emulator does not have. The game itself needs neither extended nor expanded memory (docs/PC98.md, M5.4 and M7: the only contact is
 * one XMS installation check that gets no answer), so the four lines are commented out in the in-memory copy: each text is replaced by `REM`
 * and the rest of the same length, so no sector moves. The file the image came from is never touched, and the image's starting point (what
 * "changed" is measured against) is taken after the edit, so the copy is not "modified" by it. An image without those lines is left alone. */
int disk_virtual_config(DiskSlot *s) {
    static const struct { const char *from, *to; } edits[] = {
        { "DEVICE=HIMEM.SYS", "REM HIMEM.SYS   " },
        { "DEVICE=EMM386.EXE /UMB", "REM EMM386.EXE /UMB   " },
        { "DOS=HIGH", "REM HIGH" },
        { "DOS=UMB", "REM UMB" },
    };
    if (!s->loaded) return 0;
    int n = 0;
    for (unsigned e = 0; e < sizeof edits / sizeof edits[0]; e++) {
        size_t len = strlen(edits[e].from);
        for (size_t i = 0; i + len <= s->size; i++) {
            if (s->data[i] != (uint8_t)edits[e].from[0] || memcmp(s->data + i, edits[e].from, len)) continue;
            memcpy(s->data + i, edits[e].to, len);
            n++;
            i += len - 1;
        }
    }
    if (n) take_sums(s);
    return n;
}
