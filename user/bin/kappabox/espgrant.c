/*
 * espgrant: write the ESP the system started from, for a moment (docs/INSTALL.md "The ESP write grant", docs/UPDATE.md "Writing the ESP").
 *
 * Under blk.ro the installed ESP (loader, kernel, initrd, cmdline.txt) is read-only; only root, through the kernel's ESP write grant
 * (shared/blkabi.h, BLKESPGRANT), may mount it read-write, and only for as long as this program holds the grant:
 *
 *   espgrant                                   which partition that is, and whether a grant is held
 *   espgrant [-f] [-d DIR] run CMD [ARG]...    mount it read-write on DIR, run CMD with $ESP set to DIR, unmount
 *   espgrant [-f] [-k] [-q] put SRC NAME...    the update sequence that survives a power cut (stage NAME.new, flush, compare, rename)
 *   espgrant [-f] [-q] rm NAME...              delete files (the loader fallback's "this boot worked": rm update.try)
 *
 * The functions in espgrant.h are the part an `update` client reuses.
 */
#include "kappabox.h"
#include "espgrant.h"
#include <stdarg.h>
#include <sys/statfs.h>
#include <blkabi.h>

static void seterr(char *err, size_t en, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void seterr(char *err, size_t en, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, en, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------------------------------------ the grant and the mount */

int esp_find(char *dev, size_t cap, char *why, size_t wn) {
    FILE *f = fopen("/proc/bootdisk", "r");
    if (!f) { snprintf(why, wn, "cannot read /proc/bootdisk: %s", strerror(errno)); return -1; }
    char line[256];
    int rc = -1;
    snprintf(why, wn, "/proc/bootdisk names none (the firmware did not say where the system came from?)");
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "esp ", 4)) continue;
        line[strcspn(line, "\n")] = 0;
        char *v = line + 4;
        if (v[0] == '-') {                                  /* "esp - (the reason)" */
            char *p = strchr(v, '(');
            snprintf(why, wn, "%s", p ? p + 1 : v);
            size_t l = strlen(why);
            if (l && why[l - 1] == ')') why[l - 1] = 0;
        } else {
            size_t k = strcspn(v, " ");
            if (!k || k >= cap) continue;
            memcpy(dev, v, k);
            dev[k] = 0;
            rc = 0;
        }
    }
    fclose(f);
    return rc;
}

static int grant(EspSession *s, char *err, size_t en) {
    struct gblk_unlock u;
    memset(&u, 0, sizeof u);
    strlcpy(u.name, s->dev, sizeof u.name);
    if (ioctl(s->fd, BLKESPGRANT, &u) == 0) return 0;
    int e = errno;
    if (e == EPERM) seterr(err, en, "only root may write the ESP");
    else if (e == EACCES) seterr(err, en, "the kernel refused: /dev/%s is not the ESP this system started from (see dmesg)", s->dev);
    else if (e == EBUSY) seterr(err, en, "/dev/%s is mounted, or another program holds its write grant (umount it first)", s->dev);
    else if (e == EROFS) seterr(err, en, "/dev/%s is on a write-protected medium", s->dev);
    else seterr(err, en, "the ESP write grant for /dev/%s failed: %s", s->dev, strerror(e));
    return -1;
}

static int do_mount(EspSession *s, char *err, size_t en) {
    char opts[96], src[48];
    snprintf(opts, sizeof opts, "rw,espgrant%s", s->force ? ",force" : "");
    time_t now = time(NULL);                                 /* FAT keeps local time, as the mount applet does */
    struct tm tmv;
    localtime_r(&now, &tmv);
    long minutes = tmv.tm_gmtoff / 60;
    if (minutes) {
        char one[32];
        snprintf(one, sizeof one, ",tzoffset=%ld", minutes);
        strlcat(opts, one, sizeof opts);
    }
    snprintf(src, sizeof src, "/dev/%s", s->dev);
    if (gap_mount_opts(src, s->dir, "fat32", 0, opts) < 0) {
        int e = errno;
        if (e == EROFS)
            seterr(err, en, "%s was not cleanly unmounted (a power cut during an earlier update?): check it first, or use -f to "
                   "mount it anyway", src);
        else seterr(err, en, "cannot mount %s on %s: %s", src, s->dir, strerror(e));
        return -1;
    }
    s->mounted = 1;
    return 0;
}

int esp_open(EspSession *s, const char *dir, int force, char *err, size_t en) {
    memset(s, 0, sizeof *s);
    s->fd = -1;
    s->force = force;
    if (geteuid() != 0) { seterr(err, en, "only root may write the ESP"); return -1; }
    char why[200];
    if (esp_find(s->dev, sizeof s->dev, why, sizeof why)) { seterr(err, en, "there is no ESP to write: %s", why); return -1; }
    if (dir) strlcpy(s->dir, dir, sizeof s->dir);
    else snprintf(s->dir, sizeof s->dir, "/BambooForest/espgrant.%d", (int)getpid());
    struct stat st;
    if (stat(s->dir, &st) != 0) {
        if (mkdir(s->dir, 0700) != 0) { seterr(err, en, "cannot make %s: %s", s->dir, strerror(errno)); return -1; }
        s->made_dir = 1;
    }
    char path[48];
    snprintf(path, sizeof path, "/dev/%s", s->dev);
    s->fd = open(path, O_RDWR | O_CLOEXEC);                  /* close-on-exec: a command run by `run` never holds the grant */
    if (s->fd < 0) { seterr(err, en, "cannot open %s: %s", path, strerror(errno)); esp_close(s); return -1; }
    if (grant(s, err, en) || do_mount(s, err, en)) { esp_close(s); return -1; }
    return 0;
}

int esp_remount(EspSession *s, char *err, size_t en) {
    sync();
    if (gap_umount(s->dir) < 0) { seterr(err, en, "cannot unmount %s: %s", s->dir, strerror(errno)); return -1; }
    s->mounted = 0;
    return grant(s, err, en) || do_mount(s, err, en) ? -1 : 0;
}

int esp_close(EspSession *s) {
    int rc = 0;
    if (s->mounted) {
        sync();
        if (gap_umount(s->dir) == 0) s->mounted = 0;
        else rc = -1;                                       /* busy: closing the grant below turns it read-only instead */
    }
    if (s->made_dir && !s->mounted) rmdir(s->dir);
    if (s->fd >= 0) close(s->fd);
    s->fd = -1;
    return rc;
}

/* ------------------------------------------------------------------------------------------------ the update sequence */

void esp_old_name(const char *name, char *out, size_t cap) {
    const char *slash = strrchr(name, '/');
    const char *dot = strrchr(slash ? slash + 1 : name, '.');
    size_t keep = dot ? (size_t)(dot - name) : strlen(name);
    snprintf(out, cap, "%.*s.old", (int)keep, name);
}

static int write_file(const char *path, const void *data, size_t len, char *err, size_t en) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { seterr(err, en, "cannot create %s: %s", path, strerror(errno)); return -1; }
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, (const char *)data + done, len - done > 65536 ? 65536 : len - done);
        if (w <= 0) { seterr(err, en, "writing %s failed: %s", path, w < 0 ? strerror(errno) : "no progress"); close(fd); return -1; }
        done += (size_t)w;
    }
    if (close(fd) != 0) { seterr(err, en, "writing %s failed: %s", path, strerror(errno)); return -1; }
    return 0;
}

/* 0 when the file holds exactly len bytes equal to data. */
static int same_contents(const char *path, const void *data, size_t len, char *err, size_t en) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { seterr(err, en, "cannot read %s back: %s", path, strerror(errno)); return -1; }
    static char buf[65536];
    size_t off = 0;
    int rc = 0;
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0) { seterr(err, en, "cannot read %s back: %s", path, strerror(errno)); rc = -1; break; }
        if (!r) break;
        if (off + (size_t)r > len || memcmp(buf, (const char *)data + off, (size_t)r)) {
            seterr(err, en, "%s read back from the disk differs from what was written (at byte %zu)", path, off);
            rc = -1;
            break;
        }
        off += (size_t)r;
    }
    close(fd);
    if (!rc && off != len) { seterr(err, en, "%s read back from the disk is %zu bytes, not %zu", path, off, len); rc = -1; }
    return rc;
}

static char *read_whole(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    char *d = kb_read_all(fd, len);
    close(fd);
    return d;
}

static char *staged(const EspSession *s, const char *name, const char *suffix) {
    char *p = malloc(strlen(s->dir) + strlen(name) + strlen(suffix) + 2);
    if (!p) kb_die("out of memory");
    sprintf(p, "%s/%s%s", s->dir, name, suffix);
    return p;
}

static void unstage(EspSession *s, const EspFile *files, int n) {
    for (int i = 0; i < n; i++) {
        char *p = staged(s, files[i].name, ".new");
        unlink(p);
        free(p);
    }
    sync();
}

int esp_update(EspSession *s, const EspFile *files, int n, int keep_old, void (*say)(const char *step, const char *name),
               char *err, size_t en) {
    #define SAY(step, name) do { if (say) say(step, name); } while (0)
    int *ord = malloc(sizeof(int) * (size_t)(n ? n : 1)), k = 0;
    if (!ord) kb_die("out of memory");
    for (int pass = 0; pass < 2; pass++)                    /* the loader last: a broken one cannot fall back to anything */
        for (int i = 0; i < n; i++)
            if ((strncasecmp(files[i].name, "EFI/", 4) == 0) == (pass == 1)) ord[k++] = i;

    /* room for the new files (and the copies kept as .old) next to the current ones */
    struct statfs sf;
    if (statfs(s->dir, &sf) == 0 && sf.f_bsize) {
        unsigned long long need = 0, bs = sf.f_bsize, room = (unsigned long long)sf.f_bavail * bs;
        for (int i = 0; i < n; i++) {
            need += (files[i].len + bs - 1) / bs * bs;
            struct stat st;
            char *cur = staged(s, files[i].name, "");
            if (keep_old && stat(cur, &st) == 0) need += ((unsigned long long)st.st_size + bs - 1) / bs * bs;
            free(cur);
        }
        if (need > room) {
            seterr(err, en, "not enough room on the ESP: %llu KiB needed, %llu KiB free (nothing was changed)", need >> 10, room >> 10);
            free(ord);
            return -1;
        }
    }

    /* 1. stage every file as NAME.new and flush */
    for (int j = 0; j < n; j++) {
        const EspFile *f = &files[ord[j]];
        char *p = staged(s, f->name, ".new");
        SAY("staging", f->name);
        unlink(p);
        int rc = write_file(p, f->data, f->len, err, en);
        free(p);
        if (rc) { unstage(s, files, n); free(ord); return -1; }
        SAY("staged", f->name);
    }
    sync();
    SAY("synced", "the staged files");

    /* 2. mount again so the comparison reads the disk, not the cache */
    if (esp_remount(s, err, en)) { free(ord); return -1; }
    for (int j = 0; j < n; j++) {
        const EspFile *f = &files[ord[j]];
        char *p = staged(s, f->name, ".new");
        int rc = same_contents(p, f->data, f->len, err, en);
        free(p);
        if (rc) { unstage(s, files, n); free(ord); return -1; }
        SAY("verified", f->name);
    }

    /* 3. optionally keep the current files under their .old names: a copy through OLD.new, renamed over OLD */
    for (int j = 0; keep_old && j < n; j++) {
        const EspFile *f = &files[ord[j]];
        char *cur = staged(s, f->name, ""), old[160];
        esp_old_name(f->name, old, sizeof old);
        size_t len = 0;
        char *data = read_whole(cur, &len);
        free(cur);
        if (!data) continue;                                /* nothing there yet: nothing to keep */
        char *oldnew = staged(s, old, ".new"), *oldp = staged(s, old, "");
        int rc = write_file(oldnew, data, len, err, en);
        free(data);
        if (!rc) {
            sync();
            if (rename(oldnew, oldp) != 0) { seterr(err, en, "cannot rename %s to %s: %s", oldnew, oldp, strerror(errno)); rc = -1; }
            sync();
        }
        free(oldnew);
        free(oldp);
        if (rc) { unstage(s, files, n); free(ord); return -1; }
        SAY("kept", f->name);
    }

    /* 4. rename each NAME.new over NAME, one at a time, the loader last; each rename is on the disk before the next */
    for (int j = 0; j < n; j++) {
        const EspFile *f = &files[ord[j]];
        char *p = staged(s, f->name, ".new"), *cur = staged(s, f->name, "");
        SAY("replacing", f->name);
        int rc = rename(p, cur);
        if (rc) seterr(err, en, "cannot rename %s to %s: %s", p, cur, strerror(errno));
        sync();
        free(p);
        free(cur);
        if (rc) { free(ord); return -1; }                  /* the files renamed so far are new, the rest old: run it again */
        SAY("replaced", f->name);
    }
    free(ord);
    SAY("done", "");
    return 0;
    #undef SAY
}

/* ------------------------------------------------------------------------------------------------ the applet */

static int g_quiet;

static void say_step(const char *step, const char *name) {
    if (g_quiet) return;
    printf("espgrant: %s %s\n", step, name);
    fflush(stdout);
}

static int status(void) {
    char dev[32], why[200];
    if (esp_find(dev, sizeof dev, why, sizeof why)) {
        printf("no ESP can be written: %s\n", why);
        return 1;
    }
    char state[32] = "";
    FILE *f = fopen("/proc/bootdisk", "r");
    char line[256];
    int grants = 0;
    while (f && fgets(line, sizeof line, f)) {
        char part[32], who[32], how[16], where[128] = "";
        int pid;
        if (!strncmp(line, "esp ", 4)) sscanf(line + 4, "%*s %31s", state);
        else if (sscanf(line, "grant %31s pid %d %31s %15s %127s", part, &pid, who, how, where) >= 4) {
            if (!strcmp(how, "mounted")) printf("grant: %s is held by %s (pid %d), mounted read-write on %s\n", part, who, pid, where);
            else printf("grant: %s is held by %s (pid %d), not mounted yet\n", part, who, pid);
            grants++;
        }
    }
    if (f) fclose(f);
    printf("ESP: /dev/%s, the partition this system started from (%s%s)\n", dev, state,
           !strcmp(state, "read-only") ? " under blk.ro: espgrant can lift that for one mount" : "");
    if (!grants) printf("grant: none held\n");
    return 0;
}

static int usage(void) {
    kb_error("usage: espgrant\n"
             "       espgrant [-f] [-d DIR] run COMMAND [ARG]...\n"
             "       espgrant [-f] [-k] [-q] put SRC NAME [SRC NAME]...\n"
             "       espgrant [-f] [-q] rm NAME...");
    return 2;
}

APPLET(espgrant) {
    int force = 0, keep = 0, i = 1;
    const char *dir = NULL;
    g_quiet = 0;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-f")) force = 1;
        else if (!strcmp(argv[i], "-k")) keep = 1;
        else if (!strcmp(argv[i], "-q")) g_quiet = 1;
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else return usage();
    }
    if (i >= argc) return status();
    const char *cmd = argv[i++];
    char err[400];
    EspSession s;

    if (!strcmp(cmd, "run")) {
        if (i >= argc) return usage();
        if (esp_open(&s, dir, force, err, sizeof err)) { kb_error("%s", err); return 1; }
        if (!g_quiet) printf("espgrant: /dev/%s is mounted read-write on %s until %s ends\n", s.dev, s.dir, argv[i]);
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            char me[16];
            snprintf(me, sizeof me, "%d", (int)getppid());
            setenv("ESP", s.dir, 1);
            setenv("ESPGRANT_PID", me, 1);
            execvp(argv[i], argv + i);
            fprintf(stderr, "espgrant: cannot run %s: %s\n", argv[i], strerror(errno));
            _exit(127);
        }
        int st = 0;
        if (pid > 0) {
            signal(SIGINT, SIG_IGN);                        /* Ctrl-C belongs to the command; the unmount comes after it */
            signal(SIGQUIT, SIG_IGN);
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        } else {
            kb_error("cannot fork: %s", strerror(errno));
        }
        if (esp_close(&s)) kb_error("cannot unmount %s (busy?): the grant is handed back anyway, so it stays mounted read-only; "
                                    "umount it by hand", s.dir);
        else if (!g_quiet) printf("espgrant: %s unmounted; /dev/%s is read-only again\n", s.dir, s.dev);
        if (pid < 0) return 1;
        return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }

    if (!strcmp(cmd, "put")) {
        int n = (argc - i) / 2;
        if (n < 1 || (argc - i) % 2) return usage();
        EspFile *files = calloc((size_t)n, sizeof *files);
        if (!files) kb_die("out of memory");
        for (int k = 0; k < n; k++) {
            const char *src = argv[i + 2 * k], *name = argv[i + 2 * k + 1];
            while (*name == '/') name++;
            if (!*name || strstr(name, "..") || strlen(name) > 120) { kb_error("%s: not a file name on the ESP", argv[i + 2 * k + 1]); return 2; }
            size_t len = 0;
            char *data = read_whole(src, &len);
            if (!data) { kb_error("%s: %s", src, strerror(errno)); return 1; }
            files[k] = (EspFile){ name, data, len };
        }
        if (esp_open(&s, dir, force, err, sizeof err)) { kb_error("%s", err); return 1; }
        say_step("mounted", s.dir);
        int rc = esp_update(&s, files, n, keep, say_step, err, sizeof err);
        if (rc) kb_error("%s", err);
        if (esp_close(&s)) { kb_error("cannot unmount %s: it stays mounted read-only", s.dir); rc = -1; }
        if (!rc && !g_quiet) printf("espgrant: the ESP holds the new files; /dev/%s is read-only again\n", s.dev);
        return rc ? 1 : 0;
    }

    if (!strcmp(cmd, "rm")) {
        if (i >= argc) return usage();
        if (esp_open(&s, dir, force, err, sizeof err)) { kb_error("%s", err); return 1; }
        int bad = 0;
        for (; i < argc; i++) {
            const char *name = argv[i];
            while (*name == '/') name++;
            char *p = staged(&s, name, "");
            if (!*name || strstr(name, "..")) { kb_error("%s: not a file name on the ESP", argv[i]); bad = 1; }
            else if (unlink(p) != 0) { kb_error("%s: %s", name, strerror(errno)); bad = 1; }
            else say_step("removed", name);
            free(p);
        }
        sync();
        if (esp_close(&s)) { kb_error("cannot unmount %s: it stays mounted read-only", s.dir); bad = 1; }
        return bad;
    }
    return usage();
}
