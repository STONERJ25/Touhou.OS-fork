/*
 * fsck, fsck.fat, fsck.vfat -- Keine goes through the ledger of an unmounted FAT12/16/32 volume and mends it.
 *
 * The scanner and the repair engine are shared/fatck.c and shared/fatfix.c, the very code the kernel runs read-only on a volume that was
 * not shut down cleanly (see engine.c). This file is the command line: the device, the questions, the report.
 *
 *   fsck [-n | -a | -p | -y] [-F] [-v] [-q] DEVICE
 *
 * Exit status as fsck(8): 0 nothing wrong, 1 problems found and corrected, 4 problems left, 8 operational error (cannot open it, it is
 * mounted, it is not FAT), 16 usage.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include "fatck.h"
#include "fatfix.h"

#define BLKFLSBUF 0x1261

typedef struct {
    int fd;
    bool print;                         /* print findings as the scan reports them */
    bool verbose, quiet;
    bool yes_all;                       /* interactive: "a" was answered */
    int printed[4];
} Ctx;

static Ctx g;

static void usage(void) {
    fprintf(stderr,
            "usage: fsck [-n | -a | -p | -y] [-F] [-v] [-q] DEVICE\n"
            "  Goes through the ledger of an UNMOUNTED FAT12/16/32 volume (a partition such as /dev/sdb2, or an image file).\n"
            "  -n  look only: report what is wrong, change nothing\n"
            "  -a  mend what loses nothing (-p is the same): FAT copies, FSInfo, sizes against chains, stray long-name entries, dot entries,\n"
            "      cross-linked files (the shared clusters are copied), lost chains (saved as FOUND.000/FILEnnnn.CHK); then clear the dirty flag\n"
            "  -y  say yes to everything, also what costs something: cut broken chains, remove entries that point nowhere or repeat a name\n"
            "  -F  free lost chains instead of saving them as files\n"
            "  -v  also show warnings and notes     -q  only the summary\n"
            "  without -n/-a/-y it asks before each repair (when it can ask; otherwise it behaves like -n)\n"
            "  exit status: 0 clean, 1 problems corrected, 4 problems left, 8 cannot do it, 16 usage\n");
}

/* ---------------------------------------------------------------------------------------------- the device */

static int io_read(void *ctx, uint64_t off, void *buf, size_t n) {
    int fd = ((Ctx *)ctx)->fd;
    if (lseek(fd, (off_t)off, SEEK_SET) < 0) return -(errno ? errno : 5);
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, (char *)buf + done, n - done);
        if (r < 0) { if (errno == EINTR) continue; return -(errno ? errno : 5); }
        if (r == 0) return -5;
        done += (size_t)r;
    }
    return 0;
}

static int io_write(void *ctx, uint64_t off, const void *buf, size_t n) {
    int fd = ((Ctx *)ctx)->fd;
    if (lseek(fd, (off_t)off, SEEK_SET) < 0) return -(errno ? errno : 5);
    size_t done = 0;
    while (done < n) {
        ssize_t r = write(fd, (const char *)buf + done, n - done);
        if (r < 0) { if (errno == EINTR) continue; return -(errno ? errno : 5); }
        if (r == 0) return -5;
        done += (size_t)r;
    }
    return 0;
}

static int io_flush(void *ctx) {
    ioctl(((Ctx *)ctx)->fd, BLKFLSBUF, 0);                               /* a regular file has no such request: fine */
    return 0;
}

static void *io_alloc(void *ctx, size_t n) { (void)ctx; return calloc(1, n); }
static void io_release(void *ctx, void *p, size_t n) { (void)ctx; (void)n; free(p); }

static void io_report(void *ctx, const FatCkFinding *f, const char *text) {
    Ctx *c = ctx;
    c->printed[f->sev & 3]++;
    if (!c->print || c->quiet) return;
    if (f->sev == FS_NOTE || (f->sev == FS_WARN && !c->verbose && f->code != FK_DIRTY)) return;
    printf("%-8s %s\n", fatck_sev_name(f->sev), text);
}

/* ---------------------------------------------------------------------------------------------- questions and actions */

static bool ask(void *ctx, int level, const char *question) {
    Ctx *c = ctx;
    if (c->yes_all) { printf("  %s: yes\n", question); return true; }
    printf("%s\n  %s? [y/N/a(ll)/q] ", question, level == FIX_ALL ? "repair it (it costs something)" : "repair it");
    fflush(stdout);
    char line[16];
    if (!fgets(line, sizeof line, stdin)) { printf("\n"); exit(32); }
    switch (line[0]) {
    case 'y': case 'Y': return true;
    case 'a': case 'A': c->yes_all = true; return true;
    case 'q': case 'Q': printf("stopped; nothing more is changed\n"); exit(32);
    default: return false;
    }
}

static void say(void *ctx, const char *line) {
    (void)ctx;
    if (!g.quiet) printf("  fixed: %s\n", line);
}

/* ---------------------------------------------------------------------------------------------- "is it mounted?" */

static const char *dev_name(const char *path) { return !strncmp(path, "/dev/", 5) ? path + 5 : path; }

/* Is the device, or a partition of it, mounted? (/proc/mounts lists what the kernel mounted from.) */
static bool is_mounted(const char *path, char *where, size_t cap) {
    if (strchr(dev_name(path), '/')) return false;                       /* an image file: nothing to compare */
    const char *want = dev_name(path);
    size_t wl = strlen(want);
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return false;
    char line[512];
    bool mounted = false;
    while (fgets(line, sizeof line, f)) {
        char *src = line, *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        char *target = sp + 1, *sp2 = strchr(target, ' ');
        if (sp2) *sp2 = 0;
        if (strncmp(src, "/dev/", 5)) continue;
        const char *name = src + 5;
        bool same = !strcmp(name, want);
        bool part = !strncmp(name, want, wl) && ((name[wl] >= '0' && name[wl] <= '9') || (name[wl] == 'p' && name[wl + 1] >= '0' && name[wl + 1] <= '9'));
        if (same || part) {
            snprintf(where, cap, "%s on %s", src, target);
            mounted = true;
            break;
        }
    }
    fclose(f);
    return mounted;
}

/* ---------------------------------------------------------------------------------------------- main */

int main(int argc, char **argv) {
    int mode = -1;                                                       /* -1: interactive */
    bool free_lost = false;
    int c;
    while ((c = getopt(argc, argv, "naypfFvqh")) != -1) {
        switch (c) {
        case 'n': mode = FIX_NONE; break;
        case 'a': case 'p': mode = FIX_SAFE; break;
        case 'y': mode = FIX_ALL; break;
        case 'f': break;                                                 /* (accepted: this fsck always looks) */
        case 'F': free_lost = true; break;
        case 'v': g.verbose = true; break;
        case 'q': g.quiet = true; break;
        default: usage(); return 16;
        }
    }
    if (optind + 1 != argc) { usage(); return 16; }
    const char *dev = argv[optind];
    char path[160];
    if (strchr(dev, '/')) snprintf(path, sizeof path, "%s", dev);
    else snprintf(path, sizeof path, "/dev/%s", dev);
    bool interactive = false;
    if (mode < 0) {
        if (isatty(0) && isatty(1)) interactive = true;
        else { mode = FIX_NONE; fprintf(stderr, "fsck: no terminal to ask on: behaving like -n (use -a or -y to repair)\n"); }
    }

    char where[160];
    if (is_mounted(path, where, sizeof where)) {
        fprintf(stderr, "fsck: %s is mounted (%s). Unmount it first (`umount` the mount point); a volume is only checked while nothing else uses it.\n", path, where);
        return 8;
    }
    bool write = mode != FIX_NONE;
    int fd = open(path, write || interactive ? O_RDWR : O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "fsck: cannot open %s: %s%s\n", path, strerror(errno), errno == EACCES || errno == EPERM ? " (only root may read or mend a disk: try `sudo fsck ...`)" : "");
        return 8;
    }
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 512) { fprintf(stderr, "fsck: %s is empty or its size is unknown\n", path); return 8; }
    g.fd = fd;

    FatCkIo io;
    memset(&io, 0, sizeof io);
    io.read = io_read;
    io.write = write || interactive ? io_write : NULL;
    io.flush = io_flush;
    io.alloc = io_alloc;
    io.release = io_release;
    io.report = io_report;
    io.ctx = &g;
    io.bytes = (uint64_t)size;
    io.align = 512;
    io.max_windows = 1024;
    io.arena_bytes = 4u << 20;
    io.keep = 4096;
    g.print = true;
    FatCk *ck;
    int rc = fatck_open(&ck, &io);
    if (rc) {
        fprintf(stderr, "fsck: %s is not a FAT volume that can be checked (%s)\n", path, rc == -22 ? "see above" : strerror(-rc));
        return 8;
    }
    const FatCkGeom *geom = fatck_geom(ck);
    printf("fsck: Keine goes through the ledger of %s (FAT%d, %u clusters of %u bytes)\n", path, geom->bits, geom->nclusters, geom->cluster_bytes);

    rc = fatck_scan(ck);
    const FatCkResult *r = fatck_result(ck);
    if (rc) {
        fprintf(stderr, "fsck: the check of %s could not be finished (%s)\n", path, rc == -12 ? "not enough memory" : rc == -5 ? "the device failed" : "see above");
        return 8;
    }
    uint32_t found_err = r->errors, found_leak = r->leaks, found_warn = r->warnings;
    bool was_dirty = r->dirty || r->hard_error;
    printf("%s: %u files in %u directories, %u of %u clusters free\n", path, r->files, r->dirs, r->free_clusters, r->nclusters);
    if (!found_err && !found_leak) {
        if (was_dirty && mode == FIX_NONE) printf("%s: nothing is wrong, but the volume is flagged dirty (a power cut, or an eject that did not happen): fsck -a clears the flag\n", path);
        else if (!was_dirty) printf("%s: the ledger is in order\n", path);
    } else {
        printf("%s: %u error(s), %u leftover(s) of a power cut, %u warning(s)\n", path, found_err, found_leak, found_warn);
    }

    FatFixStats st;
    memset(&st, 0, sizeof st);
    int exit_code;
    if (mode == FIX_NONE && !interactive) {
        exit_code = (found_err || found_leak) ? 4 : 0;
        if (exit_code) printf("%s: nothing was changed. Run `fsck -a %s` for the repairs that lose nothing, `fsck -y %s` for all of them\n", path, dev, dev);
    } else if (!found_err && !found_leak && !was_dirty) {
        exit_code = 0;
    } else {
        g.print = false;
        FatFixOpts o;
        memset(&o, 0, sizeof o);
        o.mode = interactive ? FIX_NONE : mode;
        o.free_lost = free_lost;
        o.ask = interactive ? ask : NULL;
        o.say = say;
        o.ctx = &g;
        o.unix_time = (uint64_t)time(NULL);
        rc = fatfix_run(ck, &o, &st);
        r = fatck_result(ck);
        if (rc) {
            fprintf(stderr, "fsck: the repair of %s stopped: %s. Whatever was written is consistent (the repairs are done one at a time); run fsck again\n", path, rc == -5 ? "the device failed" : "out of memory or time");
            return 8;
        }
        uint32_t left = r->errors + r->leaks;
        printf("%s: %u repair(s) made in %u pass(es)", path, st.repaired, st.passes);
        if (st.lost_chains) printf(", %u lost chain(s) of %u cluster(s) %s", st.lost_chains, st.lost_clusters, free_lost ? "freed" : "saved in FOUND.000");
        if (st.copied_clusters) printf(", %u cluster(s) copied to part cross-linked files", st.copied_clusters);
        printf("\n");
        if (left) {
            printf("%s: %u error(s) and %u leftover(s) remain", path, r->errors, r->leaks);
            if (st.declined) printf(" (%u repair(s) were not allowed: %s)", st.declined, mode == FIX_SAFE ? "-y allows them" : "you said no");
            printf("\n");
            const FatCkFinding *list;
            uint32_t n = fatck_findings(ck, &list);
            for (uint32_t i = 0; i < n && !g.quiet; i++)
                if (list[i].sev >= FS_LEAK) printf("%-8s %s\n", fatck_sev_name(list[i].sev), fatck_finding_text(ck, i));
            exit_code = 4;
        } else {
            printf("%s: the ledger is in order%s\n", path, st.flags_cleared ? " and the volume is marked clean" : "");
            exit_code = st.wrote ? 1 : 0;
        }
    }
    fatck_close(ck);
    close(fd);
    return exit_code;
}
