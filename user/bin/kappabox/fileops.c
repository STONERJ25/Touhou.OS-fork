/* File and directory utilities: cat mkdir rmdir rm trash mv cp touch ln chmod chown stat readlink basename dirname realpath */
#include "kappabox.h"
#include <libgen.h>
#include <utime.h>

static int cat_fd(int fd, int number, int *lineno, int show_ends) {
    if (!number && !show_ends) return kb_copy_fd(fd, 1);
    char buf[4096];
    ssize_t n;
    int at_start = 1;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (at_start && number) printf("%6d\t", (*lineno)++);
            at_start = 0;
            if (buf[i] == '\n') { if (show_ends) putchar('$'); at_start = 1; }
            putchar(buf[i]);
        }
    }
    return n < 0 ? -1 : 0;
}

APPLET(cat) {
    int number = 0, ends = 0, i = 1, rc = 0, lineno = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'n') number = 1;
            else if (*o == 'E') ends = 1;
            else if (*o == 'u') { }
            else { kb_error("invalid option -- '%c'", *o); return 2; }
        }
    }
    if (i >= argc) return cat_fd(0, number, &lineno, ends) < 0;
    for (; i < argc; i++) {
        int fd = !strcmp(argv[i], "-") ? 0 : open(argv[i], O_RDONLY);
        if (fd < 0) { kb_error("%s: %s", argv[i], strerror(errno)); rc = 1; continue; }
        if (cat_fd(fd, number, &lineno, ends) < 0) { kb_error("%s: %s", argv[i], strerror(errno)); rc = 1; }
        if (fd) close(fd);
    }
    fflush(stdout);
    return rc;
}

static int mkdir_p(const char *path, mode_t mode) {
    char buf[PATH_MAX];
    strlcpy(buf, path, sizeof buf);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(buf, mode) < 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    return mkdir(buf, mode) < 0 && errno != EEXIST ? -1 : 0;
}

APPLET(mkdir) {
    int parents = 0, i = 1, rc = 0;
    mode_t mode = 0755;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-p")) parents = 1;
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) mode = (mode_t)strtol(argv[++i], NULL, 8);
        else { kb_error("invalid option '%s'", argv[i]); return 2; }
    }
    if (i >= argc) { kb_error("missing operand"); return 1; }
    for (; i < argc; i++) {
        int r = parents ? mkdir_p(argv[i], mode) : mkdir(argv[i], mode);
        if (r < 0) { kb_error("cannot create directory '%s': %s", argv[i], strerror(errno)); rc = 1; }
    }
    return rc;
}

APPLET(rmdir) {
    int rc = 0;
    for (int i = 1; i < argc; i++)
        if (rmdir(argv[i]) < 0) { kb_error("failed to remove '%s': %s", argv[i], strerror(errno)); rc = 1; }
    if (argc < 2) { kb_error("missing operand"); return 1; }
    return rc;
}

APPLET(rm) {
    int recursive = 0, force = 0, verbose = 0, i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'r' || *o == 'R') recursive = 1;
            else if (*o == 'f') force = 1;
            else if (*o == 'v') verbose = 1;
            else if (*o == 'i') { }
            else { kb_error("invalid option -- '%c'", *o); return 2; }
        }
    }
    if (i >= argc && !force) { kb_error("missing operand"); return 1; }
    for (; i < argc; i++) {
        struct stat st;
        if (lstat(argv[i], &st) < 0) { if (!force) { kb_error("cannot remove '%s': %s", argv[i], strerror(errno)); rc = 1; } continue; }
        if (S_ISDIR(st.st_mode) && !recursive) { kb_error("cannot remove '%s': Is a directory", argv[i]); rc = 1; continue; }
        if (kb_remove_tree(argv[i], force, verbose) < 0) rc = 1;
    }
    return rc;
}

/* trash: rm's gentler sibling. Files go to the Netherworld where Yuyuko keeps them until they are purged. */
APPLET(trash) {
    int i = 1, rc = 0, quiet = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *o = argv[i] + 1; *o; o++) if (*o == 'q' || *o == 'f') quiet = 1;       /* -r -v accepted and ignored */
    }
    if (i >= argc) { if (!quiet) kb_error("missing operand"); return quiet ? 0 : 1; }
    mkdir("/Netherworld", 01777);
    for (; i < argc; i++) {
        struct stat st;
        if (lstat(argv[i], &st) < 0) { if (!quiet) { kb_error("cannot trash '%s': %s", argv[i], strerror(errno)); rc = 1; } continue; }
        char tmp[PATH_MAX];
        strlcpy(tmp, argv[i], sizeof tmp);
        const char *b = basename(tmp);
        char dst[PATH_MAX];
        snprintf(dst, sizeof dst, "/Netherworld/%s", b);
        for (int k = 1; lstat(dst, &st) == 0 && k < 1000; k++) snprintf(dst, sizeof dst, "/Netherworld/%s.%d", b, k);
        if (rename(argv[i], dst) < 0) {
            kb_error("cannot trash '%s': %s", argv[i], strerror(errno));
            rc = 1;
        } else {
            printf("Yuyuko accepts '%s' into the Netherworld (%s).\n", argv[i], dst);
        }
    }
    return rc;
}

static int copy_tree(const char *src, const char *dst, int recursive, int verbose) {
    struct stat st;
    if (lstat(src, &st) < 0) { kb_error("cannot stat '%s': %s", src, strerror(errno)); return -1; }
    if (S_ISDIR(st.st_mode)) {
        if (!recursive) { kb_error("-r not specified; omitting directory '%s'", src); return -1; }
        if (mkdir(dst, st.st_mode & 0777) < 0 && errno != EEXIST) { kb_error("cannot create directory '%s': %s", dst, strerror(errno)); return -1; }
        DIR *d = opendir(src);
        if (!d) { kb_error("cannot open '%s': %s", src, strerror(errno)); return -1; }
        int rc = 0;
        for (struct dirent *e; (e = readdir(d));) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char *s = kb_join(src, e->d_name), *t = kb_join(dst, e->d_name);
            if (copy_tree(s, t, recursive, verbose) < 0) rc = -1;
            free(s);
            free(t);
        }
        closedir(d);
        return rc;
    }
    if (S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        ssize_t n = readlink(src, target, sizeof target - 1);
        if (n < 0) return -1;
        target[n] = 0;
        unlink(dst);
        return symlink(target, dst);
    }
    if (verbose) printf("'%s' -> '%s'\n", src, dst);
    if (kb_copy_file(src, dst, 1) < 0) { kb_error("cannot copy '%s' to '%s': %s", src, dst, strerror(errno)); return -1; }
    return 0;
}

static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

APPLET(cp) {
    int recursive = 0, verbose = 0, i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'r' || *o == 'R' || *o == 'a') recursive = 1;
            else if (*o == 'v') verbose = 1;
            else if (*o == 'f' || *o == 'p' || *o == 'i') { }
            else { kb_error("invalid option -- '%c'", *o); return 2; }
        }
    }
    if (argc - i < 2) { kb_error("missing file operand"); return 1; }
    const char *dest = argv[argc - 1];
    int dest_is_dir = is_dir(dest);
    if (argc - i > 2 && !dest_is_dir) { kb_error("target '%s' is not a directory", dest); return 1; }
    for (; i < argc - 1; i++) {
        char tmp[PATH_MAX];
        strlcpy(tmp, argv[i], sizeof tmp);
        char *target = dest_is_dir ? kb_join(dest, basename(tmp)) : strdup(dest);
        if (copy_tree(argv[i], target, recursive, verbose) < 0) rc = 1;
        free(target);
    }
    return rc;
}

APPLET(mv) {
    int i = 1, rc = 0;
    while (i < argc && argv[i][0] == '-' && argv[i][1]) i++;
    if (argc - i < 2) { kb_error("missing file operand"); return 1; }
    const char *dest = argv[argc - 1];
    int dest_is_dir = is_dir(dest);
    if (argc - i > 2 && !dest_is_dir) { kb_error("target '%s' is not a directory", dest); return 1; }
    for (; i < argc - 1; i++) {
        char tmp[PATH_MAX];
        strlcpy(tmp, argv[i], sizeof tmp);
        char *target = dest_is_dir ? kb_join(dest, basename(tmp)) : strdup(dest);
        if (rename(argv[i], target) < 0) {
            if (errno == EXDEV) {                               /* different filesystems: copy, then remove */
                if (copy_tree(argv[i], target, 1, 0) == 0) kb_remove_tree(argv[i], 1, 0);
                else rc = 1;
            } else {
                kb_error("cannot move '%s' to '%s': %s", argv[i], target, strerror(errno));
                rc = 1;
            }
        }
        free(target);
    }
    return rc;
}

/* touch -t STAMP: [[CC]YY]MMDDhhmm[.ss] in local time; -d accepts "@SECONDS" and "YYYY-MM-DD[ T]hh:mm[:ss]" (local time) */
static int touch_parse_stamp(const char *s, int iso, time_t *out) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_isdst = -1;
    int n[6] = {0}, k;
    if (iso) {
        if (s[0] == '@') { *out = (time_t)strtoll(s + 1, NULL, 10); return 0; }
        k = sscanf(s, "%d-%d-%d%*[ T]%d:%d:%d", &n[0], &n[1], &n[2], &n[3], &n[4], &n[5]);
        if (k != 3 && k != 5 && k != 6) return -1;
        tm.tm_year = n[0] - 1900; tm.tm_mon = n[1] - 1; tm.tm_mday = n[2];
        tm.tm_hour = k >= 5 ? n[3] : 0; tm.tm_min = k >= 5 ? n[4] : 0; tm.tm_sec = k == 6 ? n[5] : 0;
    } else {
        char digits[16];
        const char *dot = strchr(s, '.');
        size_t len = dot ? (size_t)(dot - s) : strlen(s);
        if ((len != 8 && len != 10 && len != 12) || len >= sizeof digits) return -1;
        memcpy(digits, s, len); digits[len] = 0;
        for (size_t i = 0; i < len; i++) if (!isdigit((unsigned char)digits[i])) return -1;
        int pos = 0, year;
        if (len == 12) { year = (digits[0] - '0') * 1000 + (digits[1] - '0') * 100 + (digits[2] - '0') * 10 + (digits[3] - '0'); pos = 4; }
        else if (len == 10) { year = (digits[0] - '0') * 10 + (digits[1] - '0'); year += year < 69 ? 2000 : 1900; pos = 2; }
        else { time_t now = time(NULL); struct tm lt; localtime_r(&now, &lt); year = lt.tm_year + 1900; }
        #define TWO(p) ((digits[p] - '0') * 10 + (digits[(p) + 1] - '0'))
        tm.tm_year = year - 1900; tm.tm_mon = TWO(pos) - 1; tm.tm_mday = TWO(pos + 2); tm.tm_hour = TWO(pos + 4); tm.tm_min = TWO(pos + 6);
        #undef TWO
        tm.tm_sec = dot ? atoi(dot + 1) : 0;
    }
    time_t t = mktime(&tm);
    if (t == (time_t)-1) return -1;
    *out = t;
    return 0;
}

/* touch [-acm] [-t STAMP | -d DATE | -r REFFILE] FILE...: an existing file gets its times set (to now unless told otherwise), a missing one is created.
 * (make decides what to rebuild by modification times, so "touch" has to move them.) */
APPLET(touch) {
    int rc = 0, no_create = 0, only_a = 0, only_m = 0, i = 1, have_time = 0;
    time_t when = 0, when_a = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        for (const char *o = a + 1; *o; o++) {
            if (*o == 'c') no_create = 1;
            else if (*o == 'a') only_a = 1;
            else if (*o == 'm') only_m = 1;
            else if (*o == 't' || *o == 'd' || *o == 'r') {
                const char *arg = o[1] ? o + 1 : (i + 1 < argc ? argv[++i] : NULL);
                if (!arg) { kb_error("option requires an argument -- '%c'", *o); return 1; }
                if (*o == 'r') {
                    struct stat st;
                    if (stat(arg, &st) < 0) { kb_error("failed to get attributes of '%s': %s", arg, strerror(errno)); return 1; }
                    when_a = (time_t)st.st_atime; when = (time_t)st.st_mtime;
                } else if (touch_parse_stamp(arg, *o == 'd', &when) < 0) { kb_error("invalid date format '%s'", arg); return 1; }
                else when_a = when;
                have_time = 1;
                break;
            } else { kb_error("invalid option -- '%c'", *o); return 1; }
        }
    }
    if (i >= argc) { kb_error("missing file operand"); return 1; }
    for (; i < argc; i++) {
        struct stat st;
        int exists = stat(argv[i], &st) == 0;
        if (!exists) {
            if (no_create) continue;
            int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
            if (fd < 0) { kb_error("cannot touch '%s': %s", argv[i], strerror(errno)); rc = 1; continue; }
            close(fd);
            if (!have_time) continue;                    /* created just now */
            if (stat(argv[i], &st) < 0) continue;
        }
        struct timeval tv[2];
        time_t now = time(NULL);
        time_t a = have_time ? when_a : now, m = have_time ? when : now;
        if (only_a && !only_m) m = (time_t)st.st_mtime;
        if (only_m && !only_a) a = (time_t)st.st_atime;
        tv[0].tv_sec = a; tv[0].tv_usec = 0;
        tv[1].tv_sec = m; tv[1].tv_usec = 0;
        if (utimes(argv[i], tv) < 0) { kb_error("cannot touch '%s': %s", argv[i], strerror(errno)); rc = 1; }
    }
    return rc;
}

APPLET(ln) {
    int sym = 0, force = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *o = argv[i] + 1; *o; o++) { if (*o == 's') sym = 1; else if (*o == 'f') force = 1; }
    if (argc - i < 2) { kb_error("missing file operand"); return 1; }
    const char *target = argv[i], *name = argv[i + 1];
    char *l2 = NULL;
    if (is_dir(name)) { char tmp[PATH_MAX]; strlcpy(tmp, target, sizeof tmp); l2 = kb_join(name, basename(tmp)); name = l2; }
    if (force) unlink(name);
    int rc = sym ? symlink(target, name) : link(target, name);          /* hard links work on tmpfs (/tmp, /); other filesystems refuse */
    if (rc < 0) { kb_error("failed to create %s link '%s': %s", sym ? "symbolic" : "hard", name, strerror(errno)); free(l2); return 1; }
    free(l2);
    return 0;
}

static int parse_mode(const char *s, mode_t old, mode_t *out) {
    if (isdigit((unsigned char)s[0])) { *out = (mode_t)strtol(s, NULL, 8); return 0; }
    mode_t m = old & 07777;
    const char *p = s;
    while (*p) {
        mode_t who = 0;
        while (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') { who |= *p == 'u' ? 0700 : *p == 'g' ? 070 : *p == 'o' ? 07 : 0777; p++; }
        if (!who) who = 0777;
        char op = *p++;
        if (op != '+' && op != '-' && op != '=') return -1;
        mode_t bits = 0;
        while (*p == 'r' || *p == 'w' || *p == 'x' || *p == 's' || *p == 't') {
            mode_t b = *p == 'r' ? 0444 : *p == 'w' ? 0222 : *p == 'x' ? 0111 : 0;
            bits |= b & who;
            p++;
        }
        if (op == '+') m |= bits;
        else if (op == '-') m &= ~bits;
        else m = (m & ~who) | bits;
        if (*p == ',') p++;
    }
    *out = m;
    return 0;
}

APPLET(chmod) {
    int i = 1, rc = 0;
    while (i < argc && argv[i][0] == '-' && argv[i][1] && !isdigit((unsigned char)argv[i][1]) && strcmp(argv[i], "-R")) i++;
    int recursive = i < argc && !strcmp(argv[i], "-R");
    if (recursive) i++;
    if (argc - i < 2) { kb_error("missing operand"); return 1; }
    const char *mode = argv[i++];
    for (; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) { kb_error("cannot access '%s': %s", argv[i], strerror(errno)); rc = 1; continue; }
        mode_t m;
        if (parse_mode(mode, st.st_mode, &m) < 0) { kb_error("invalid mode: '%s'", mode); return 1; }
        if (chmod(argv[i], m) < 0) { kb_error("changing permissions of '%s': %s", argv[i], strerror(errno)); rc = 1; }
    }
    return rc;
}

APPLET(chown) {
    if (argc < 3) { kb_error("missing operand"); return 1; }
    char spec[64];
    strlcpy(spec, argv[1], sizeof spec);
    char *colon = strchr(spec, ':');
    int uid = -1, gid = -1;
    if (colon) { *colon = 0; if (colon[1]) gid = kb_group_id(colon + 1); }
    if (spec[0]) uid = kb_user_id(spec);
    if ((spec[0] && uid < 0) || (colon && colon[1] && gid < 0)) { kb_error("invalid user or group: '%s'", argv[1]); return 1; }
    int rc = 0;
    for (int i = 2; i < argc; i++)
        if (chown(argv[i], (uid_t)uid, (gid_t)gid) < 0) { kb_error("changing ownership of '%s': %s", argv[i], strerror(errno)); rc = 1; }
    return rc;
}

APPLET(stat) {
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (lstat(argv[i], &st) < 0) { kb_error("cannot stat '%s': %s", argv[i], strerror(errno)); rc = 1; continue; }
        char ms[11], when[32];
        kb_mode_string(st.st_mode, ms);
        struct tm tm;
        time_t t = st.st_mtime;
        localtime_r(&t, &tm);
        strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S %z", &tm);
        printf("  File: %s\n  Size: %ld\tBlocks: %lu\tInode: %lu\tLinks: %u\n", argv[i], (long)st.st_size,
               (unsigned long)st.st_blocks, (unsigned long)st.st_ino, st.st_nlink);
        printf("  Mode: (%04o/%s)\tUid: (%u/%s)\tGid: (%u/%s)\n", st.st_mode & 07777, ms, st.st_uid, kb_user_name(st.st_uid),
               st.st_gid, kb_group_name(st.st_gid));
        printf("Modify: %s\n", when);
    }
    return rc;
}

APPLET(readlink) {
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        char buf[PATH_MAX];
        ssize_t n = readlink(argv[i], buf, sizeof buf - 1);
        if (n < 0) { rc = 1; continue; }
        buf[n] = 0;
        puts(buf);
    }
    return rc;
}

APPLET(realpath) {
    for (int i = 1; i < argc; i++) {
        char *p = realpath(argv[i], NULL);
        if (p) { puts(p); free(p); }
    }
    return 0;
}

APPLET(basename) {
    if (argc < 2) { kb_error("missing operand"); return 1; }
    char tmp[PATH_MAX];
    strlcpy(tmp, argv[1], sizeof tmp);
    char *b = basename(tmp);
    if (argc > 2) {
        size_t bl = strlen(b), sl = strlen(argv[2]);
        if (bl > sl && !strcmp(b + bl - sl, argv[2])) b[bl - sl] = 0;
    }
    puts(b);
    return 0;
}

APPLET(dirname) {
    if (argc < 2) { kb_error("missing operand"); return 1; }
    char tmp[PATH_MAX];
    strlcpy(tmp, argv[1], sizeof tmp);
    puts(dirname(tmp));
    return 0;
}
