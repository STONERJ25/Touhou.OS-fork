/* System information and control: uname whoami id groups hostname date uptime free env printenv clear reboot poweroff halt
 * shutdown dmesg mount umount sync kill sleep ps top tty which nproc true false yes seq logname getty-less helpers. */
#include "kappabox.h"
#include <sys/utsname.h>
#include <poll.h>

APPLET(true) { (void)argc; (void)argv; return 0; }
APPLET(false) { (void)argc; (void)argv; return 1; }

APPLET(yes) {
    const char *s = argc > 1 ? argv[1] : "y";
    for (;;) {
        if (puts(s) < 0) return 0;
    }
}

APPLET(seq) {
    double first = 1, step = 1, last;
    if (argc == 2) last = atof(argv[1]);
    else if (argc == 3) { first = atof(argv[1]); last = atof(argv[2]); }
    else if (argc == 4) { first = atof(argv[1]); step = atof(argv[2]); last = atof(argv[3]); }
    else { kb_error("usage: seq [FIRST [STEP]] LAST"); return 1; }
    if (step == 0) return 1;
    int integral = first == (long)first && step == (long)step;
    for (double v = first; step > 0 ? v <= last + 1e-9 : v >= last - 1e-9; v += step) {
        if (integral) printf("%ld\n", (long)v);
        else printf("%g\n", v);
    }
    return 0;
}

APPLET(sleep) {
    if (argc < 2) { kb_error("missing operand"); return 1; }
    double total = 0;
    for (int i = 1; i < argc; i++) {
        char *end;
        double v = strtod(argv[i], &end);
        if (end == argv[i]) { kb_error("invalid time interval '%s'", argv[i]); return 1; }
        if (*end == 'm') v *= 60;
        else if (*end == 'h') v *= 3600;
        else if (*end == 'd') v *= 86400;
        total += v;
    }
    struct timespec ts = { (time_t)total, (long)((total - (long)total) * 1e9) };
    nanosleep(&ts, NULL);
    return 0;
}

APPLET(usleep) {
    if (argc < 2) return 1;
    usleep((useconds_t)atol(argv[1]));
    return 0;
}

APPLET(uname) {
    struct utsname u;
    uname(&u);
    int all = 0, s = 0, n = 0, r = 0, v = 0, m = 0, c = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--all")) { all = 1; continue; }
        if (argv[i][0] != '-') continue;
        for (const char *o = argv[i] + 1; *o; o++) {
            switch (*o) {
            case 'a': all = 1; break;
            case 's': s = 1; break;
            case 'n': n = 1; break;
            case 'r': r = 1; break;
            case 'v': v = 1; break;
            case 'm': case 'p': m = 1; break;
            case 'c': c = 1; break;                       /* codename: the Gensokyo extension */
            default: kb_error("invalid option -- '%c'", *o); return 1;
            }
        }
    }
    if (all) s = n = r = v = m = 1;
    if (!(s | n | r | v | m | c)) s = 1;
    int first = 1;
#define PUT(flag, str) if (flag) { printf(first ? "%s" : " %s", str); first = 0; }
    PUT(s, u.sysname) PUT(n, u.nodename) PUT(r, u.release) PUT(v, u.version) PUT(m, u.machine) PUT(c || all, u.codename)
#undef PUT
    putchar('\n');
    return 0;
}

APPLET(hostname) {
    char buf[64];
    if (argc > 1) { kb_error("cannot set hostname"); return 1; }
    gethostname(buf, sizeof buf);
    puts(buf);
    return 0;
}

APPLET(whoami) { (void)argc; (void)argv; puts(kb_user_name(geteuid())); return 0; }
APPLET(logname) { (void)argc; (void)argv; puts(kb_user_name(getuid())); return 0; }

/* primary group first, then every other group listing the user as a member, space separated */
static void print_groups(unsigned uid, unsigned gid, int names) {
    const char *user = kb_user_name(uid);
    FILE *f = fopen("/etc/group", "r");
    unsigned seen[32];
    int nseen = 0;
    seen[nseen++] = gid;
    if (names) printf("%s", kb_group_name(gid)); else printf("%u", gid);
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *save;
        char *name = strtok_r(line, ":", &save);
        char *pw = name ? strtok_r(NULL, ":", &save) : NULL;
        char *id = pw ? strtok_r(NULL, ":", &save) : NULL;
        char *members = id ? strtok_r(NULL, "\n", &save) : NULL;
        if (!members) continue;
        unsigned g = (unsigned)atoi(id);
        int dup = 0;
        for (int i = 0; i < nseen; i++) if (seen[i] == g) dup = 1;
        if (dup) continue;
        char *m, *ms;
        for (m = strtok_r(members, ",", &ms); m; m = strtok_r(NULL, ",", &ms)) {
            if (!strcmp(m, user)) {
                if (nseen < 32) seen[nseen++] = g;
                if (names) printf(" %s", name); else printf(" %u", g);
                break;
            }
        }
    }
    fclose(f);
}

APPLET(id) {
    unsigned uid = geteuid(), gid = getegid();
    int opt_u = 0, opt_g = 0, opt_G = 0, opt_n = 0;
    const char *who = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (const char *o = argv[i] + 1; *o; o++) {
                if (*o == 'u') opt_u = 1;
                else if (*o == 'g') opt_g = 1;
                else if (*o == 'G') opt_G = 1;
                else if (*o == 'n') opt_n = 1;
                else if (*o == 'r') { }
                else { kb_error("invalid option -- '%c'", *o); return 1; }
            }
        } else who = argv[i];
    }
    if (who) {
        int u = kb_user_id(who);
        if (u < 0) { kb_error("'%s': no such user", who); return 1; }
        uid = (unsigned)u;
        gid = uid;
        FILE *f = fopen("/etc/passwd", "r");
        char line[256];
        while (f && fgets(line, sizeof line, f)) {
            char *save, *name = strtok_r(line, ":", &save), *pw = strtok_r(NULL, ":", &save), *id = strtok_r(NULL, ":", &save),
                 *g = strtok_r(NULL, ":", &save);
            (void)pw; (void)id;
            if (name && g && !strcmp(name, who)) gid = (unsigned)atoi(g);
        }
        if (f) fclose(f);
    }
    if (opt_u) { if (opt_n) puts(kb_user_name(uid)); else printf("%u\n", uid); return 0; }
    if (opt_g) { if (opt_n) puts(kb_group_name(gid)); else printf("%u\n", gid); return 0; }
    if (opt_G) { print_groups(uid, gid, opt_n); putchar('\n'); return 0; }
    printf("uid=%u(%s) gid=%u(%s) groups=", uid, kb_user_name(uid), gid, kb_group_name(gid));
    /* groups=1000(reimu),100(shrine) */
    FILE *f = fopen("/etc/group", "r");
    char line[256];
    printf("%u(%s)", gid, kb_group_name(gid));
    while (f && fgets(line, sizeof line, f)) {
        char *save, *name = strtok_r(line, ":", &save), *pw = strtok_r(NULL, ":", &save), *id = strtok_r(NULL, ":", &save),
             *members = strtok_r(NULL, "\n", &save);
        (void)pw;
        if (!name || !id || !members || (unsigned)atoi(id) == gid) continue;
        char *ms, *m;
        for (m = strtok_r(members, ",", &ms); m; m = strtok_r(NULL, ",", &ms))
            if (!strcmp(m, kb_user_name(uid))) { printf(",%u(%s)", (unsigned)atoi(id), name); break; }
    }
    if (f) fclose(f);
    putchar('\n');
    return 0;
}

APPLET(groups) {
    unsigned uid = argc > 1 ? (unsigned)kb_user_id(argv[1]) : getuid();
    unsigned gid = uid;
    if (argc <= 1) gid = getgid();
    print_groups(uid, gid, 1);
    putchar('\n');
    return 0;
}

/* ---- date ---- */

/* "@seconds", "YYYY-MM-DD", "YYYY-MM-DD HH:MM[:SS]" (a 'T' also separates); local time unless `utc`. Returns 0 on success. */
static int parse_when(const char *d, bool utc, time_t *out) {
    if (d[0] == '@') { *out = (time_t)atol(d + 1); return 0; }
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    int y, mo, da, h = 0, mi = 0, s = 0;
    char sep = ' ';
    int got = sscanf(d, "%d-%d-%d%c%d:%d:%d", &y, &mo, &da, &sep, &h, &mi, &s);
    if (got < 3 || (got > 3 && sep != ' ' && sep != 'T')) return -1;
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = da; tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s;
    *out = utc ? timegm(&tm) : mktime(&tm);
    return 0;
}

/* date [-u] [-d STRING] [-s STRING] [+FORMAT] [-R] [-I] [--iso-8601=seconds]; -s sets the system clock (root only). */
APPLET(date) {
    time_t t = time(NULL);
    const char *fmt = "%a %b %e %H:%M:%S %Z %Y", *set = NULL;
    bool utc = false;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-u") || !strcmp(argv[i], "--utc")) utc = true;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '+') fmt = argv[i] + 1;
        else if (!strcmp(argv[i], "-u") || !strcmp(argv[i], "--utc")) { }
        else if ((!strcmp(argv[i], "-d") || !strcmp(argv[i], "-s")) && i + 1 < argc) {
            bool setting = argv[i][1] == 's';
            const char *d = argv[++i];
            if (parse_when(d, utc, &t) < 0) { kb_error("invalid date '%s'", d); return 1; }
            if (setting) set = d;
        } else if (!strcmp(argv[i], "-R")) fmt = "%a, %d %b %Y %H:%M:%S %z";
        else if (!strcmp(argv[i], "-I")) fmt = "%Y-%m-%d";
        else if (!strcmp(argv[i], "--iso-8601=seconds")) fmt = "%Y-%m-%dT%H:%M:%S%z";
    }
    if (set && gap_settime((long)t * 1000, 0) < 0) { kb_error("cannot set the date: %s", strerror(errno)); return 1; }
    struct tm tm;
    if (utc) gmtime_r(&t, &tm);
    else localtime_r(&t, &tm);
    char buf[256];
    strftime(buf, sizeof buf, fmt, &tm);
    puts(buf);
    return 0;
}

/* hwclock [-w | -s] [--localtime]: the battery-backed clock chip. Without options it shows the chip's time.
 *   -s  set the system clock from the chip       -w  write the system clock to the chip
 * The chip normally keeps UTC; PCs that also run Windows keep local time there, which --localtime copes with. */
APPLET(hwclock) {
    bool to_chip = false, from_chip = false, local = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w") || !strcmp(argv[i], "--systohc")) to_chip = true;
        else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--hctosys")) from_chip = true;
        else if (!strcmp(argv[i], "--localtime")) local = true;
        else if (!strcmp(argv[i], "--utc")) local = false;
        else { kb_error("usage: hwclock [-w | -s] [--localtime | --utc]"); return 2; }
    }
    FILE *f = fopen("/proc/rtc", "r");
    unsigned long raw = 0;
    if (!f || fscanf(f, "%lu", &raw) != 1) { kb_error("cannot read the clock chip"); if (f) fclose(f); return 1; }
    fclose(f);
    time_t chip = (time_t)raw;
    struct tm tm;
    if (from_chip) {
        time_t t = chip;
        if (local) { gmtime_r(&chip, &tm); t = mktime(&tm); }
        if (gap_settime((long)t * 1000, 0) < 0) { kb_error("cannot set the system clock: %s", strerror(errno)); return 1; }
        return 0;
    }
    if (to_chip) {
        time_t now = time(NULL), value = now;
        if (local) { localtime_r(&now, &tm); value = now + tm.tm_gmtoff; }
        if (gap_settime((long)now * 1000, (long)value) < 0) { kb_error("cannot write the clock chip: %s", strerror(errno)); return 1; }
        return 0;
    }
    time_t shown = chip;
    if (local) { gmtime_r(&chip, &tm); shown = mktime(&tm); }
    localtime_r(&shown, &tm);
    char buf[128];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S %z", &tm);
    puts(buf);
    return 0;
}

static unsigned long long proc_uptime_ms(void) {
    FILE *f = fopen("/proc/uptime", "r");
    unsigned long s = 0, cs = 0;
    if (f) { if (fscanf(f, "%lu.%lu", &s, &cs) < 1) s = 0; fclose(f); }
    return s * 1000ull + cs * 10;
}

APPLET(uptime) {
    unsigned long long ms = proc_uptime_ms();
    unsigned long secs = (unsigned long)(ms / 1000);
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    unsigned long d = secs / 86400, h = secs % 86400 / 3600, m = secs % 3600 / 60;
    int pretty = argc > 1 && !strcmp(argv[1], "-p");
    if (pretty) {
        printf("up");
        if (d) printf(" %lu day%s,", d, d == 1 ? "" : "s");
        if (h) printf(" %lu hour%s,", h, h == 1 ? "" : "s");
        printf(" %lu minute%s\n", m, m == 1 ? "" : "s");
        return 0;
    }
    char up[64];
    if (d) snprintf(up, sizeof up, "%lu day%s, %lu:%02lu", d, d == 1 ? "" : "s", h, m);
    else if (h) snprintf(up, sizeof up, "%lu:%02lu", h, m);
    else snprintf(up, sizeof up, "%lu min", m);
    double l1 = 0, l5 = 0, l15 = 0;
    FILE *f = fopen("/proc/loadavg", "r");
    if (f) { if (fscanf(f, "%lf %lf %lf", &l1, &l5, &l15) != 3) l1 = l5 = l15 = 0; fclose(f); }
    printf(" %02d:%02d:%02d up %s,  load average: %.2f, %.2f, %.2f\n", tm.tm_hour, tm.tm_min, tm.tm_sec, up, l1, l5, l15);
    return 0;
}

static unsigned long meminfo(const char *key) {
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    unsigned long v = 0;
    size_t kl = strlen(key);
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl) && line[kl] == ':') { v = strtoul(line + kl + 1, NULL, 10); break; }
    if (f) fclose(f);
    return v;
}

APPLET(free) {
    int human = 0, div = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h")) human = 1;
        else if (!strcmp(argv[i], "-m")) div = 1024;
        else if (!strcmp(argv[i], "-g")) div = 1024 * 1024;
        else if (!strcmp(argv[i], "-k")) div = 1;
    }
    unsigned long total = meminfo("MemTotal"), free_ = meminfo("MemFree"), used = total - free_, heap = meminfo("HeapInUse"),
                  tmp = meminfo("Tmpfs");
    char a[16], b[16], c[16];
    printf("%-8s %10s %10s %10s\n", "", "total", "used", "free");
    if (human)
        printf("%-8s %10s %10s %10s\n", "Mem:", kb_format_size(total * 1024ull, a, sizeof a), kb_format_size(used * 1024ull, b, sizeof b),
               kb_format_size(free_ * 1024ull, c, sizeof c));
    else printf("%-8s %10lu %10lu %10lu\n", "Mem:", total / div, used / div, free_ / div);
    if (human) printf("%-8s %10s %10s\n", "Kernel:", "", kb_format_size(heap * 1024ull, a, sizeof a));
    else printf("%-8s %10s %10lu\n", "Kernel:", "", heap / div);
    if (human) printf("%-8s %10s %10s\n", "Tmpfs:", "", kb_format_size(tmp * 1024ull, a, sizeof a));
    else printf("%-8s %10s %10lu\n", "Tmpfs:", "", tmp / div);
    return 0;
}

APPLET(nproc) { (void)argc; (void)argv; printf("%ld\n", sysconf(_SC_NPROCESSORS_ONLN)); return 0; }

APPLET(env) {
    int i = 1;
    char **envp = environ;
    int clear = 0;
    if (i < argc && !strcmp(argv[i], "-i")) { clear = 1; i++; }
    static char *empty[1] = { NULL };
    if (clear) environ = empty;
    for (; i < argc && strchr(argv[i], '='); i++) putenv(argv[i]);
    if (i < argc) {
        execvp(argv[i], &argv[i]);
        kb_error("cannot run '%s': %s", argv[i], strerror(errno));
        return errno == ENOENT ? 127 : 126;
    }
    for (char **e = clear ? environ : envp; e && *e; e++) puts(*e);
    return 0;
}

APPLET(printenv) {
    if (argc < 2) { for (char **e = environ; e && *e; e++) puts(*e); return 0; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        const char *v = getenv(argv[i]);
        if (v) puts(v); else rc = 1;
    }
    return rc;
}

APPLET(clear) { (void)argc; (void)argv; fputs("\033[H\033[2J\033[3J", stdout); fflush(stdout); return 0; }

APPLET(reset) { (void)argc; (void)argv; fputs("\033c\033[0m\033[H\033[2J", stdout); fflush(stdout); return 0; }

APPLET(tty) {
    (void)argc; (void)argv;
    char *n = ttyname(0);
    if (!n) { puts("not a tty"); return 1; }
    puts(n);
    return 0;
}

/* ---- power ---- */

/* Says goodbye, asks the kernel, and reports why not if it refuses (it only lets root and the residents with a seat do this). */
static int power_action(int how, const char *name, const char *farewell) {
    if (gap_reboot(REBOOT_CHECK) < 0) { kb_error("%s: %s", name, strerror(errno)); return 1; }
    sync();
    printf("%s\n", farewell);
    fflush(stdout);
    gap_reboot(how);
    kb_error("%s: %s", name, strerror(errno));
    return 1;
}

APPLET(reboot) { (void)argc; (void)argv; return power_action(REBOOT_RESTART, "reboot", "The Hakurei barrier is resetting..."); }

APPLET(poweroff) {
    (void)argc; (void)argv;
    return power_action(REBOOT_POWEROFF, "poweroff", "The shrine maiden is closing up for the night. Goodbye!");
}

APPLET(halt) { return applet_poweroff(argc, argv); }
APPLET(shutdown) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--reboot")) return applet_reboot(argc, argv);
    return applet_poweroff(argc, argv);
}

APPLET(sync) { (void)argc; (void)argv; sync(); return 0; }

APPLET(dmesg) {
    int fd = open("/dev/kmsg", O_RDONLY);
    if (fd < 0) { kb_error("/dev/kmsg: %s", strerror(errno)); return 1; }
    int clear_after = argc > 1 && !strcmp(argv[1], "-c");
    kb_copy_fd(fd, 1);
    close(fd);
    (void)clear_after;
    return 0;
}

/* logger [-t TAG] MESSAGE...: one line into the kernel log (dmesg, the serial console); TAG replaces the process name as the klog tag. */
APPLET(logger) {
    int i = 1;
    if (argc > 2 && !strcmp(argv[1], "-t")) { gap_setname(argv[2]); i = 3; }
    char line[200] = "";
    for (; i < argc; i++) {
        size_t used = strlen(line);
        snprintf(line + used, sizeof line - used, "%s%s", used ? " " : "", argv[i]);
    }
    if (!line[0]) { kb_error("usage: logger [-t TAG] MESSAGE..."); return 1; }
    gap_log(line);
    return 0;
}

APPLET(mount) {
    if (argc == 1) {
        int fd = open("/proc/mounts", O_RDONLY);
        if (fd < 0) { kb_error("/proc/mounts: %s", strerror(errno)); return 1; }
        char *data = kb_read_all(fd, NULL);
        close(fd);
        for (char *line = strtok(data, "\n"); line; line = strtok(NULL, "\n")) {
            char src[64], tgt[64], type[32];
            if (sscanf(line, "%63s %63s %31s", src, tgt, type) == 3) printf("%s on %s type %s\n", src, tgt, type);
        }
        free(data);
        return 0;
    }
    const char *type = NULL, *opts = NULL;
    unsigned long flags = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) type = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) opts = argv[++i];
        else if (!strcmp(argv[i], "-r")) flags |= MNT_RDONLY;
        else if (!strcmp(argv[i], "-w")) flags &= ~MNT_RDONLY;
        else { kb_error("usage: mount [-t type] [-o options] [-r] source target\n"
                        "  fat32 options: ro,rw,force,sync,uid=N,gid=N,fmask=OCT,dmask=OCT,umask=OCT,userdirs,usermap=name:uid:gid/..."); return 1; }
    }
    if (argc - i < 2) { kb_error("usage: mount [-t type] [-o options] [-r] source target"); return 1; }
    const char *src = argv[i], *target = argv[i + 1];
    if (!type || !strcmp(type, "auto")) type = strncmp(src, "/dev/", 5) ? NULL : "fat32";
    if (!type) { kb_error("cannot guess the file system type of '%s': use -t type", src); return 1; }
    char optbuf[128];
    optbuf[0] = 0;
    if (opts) strlcpy(optbuf, opts, sizeof optbuf);
    for (const char *p = optbuf; *p;) {                         /* "ro" in the option list is also the read-only flag */
        if (!strncmp(p, "ro", 2) && (p[2] == ',' || !p[2])) flags |= MNT_RDONLY;
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
    if (strstr(optbuf, "userdirs") && !strstr(optbuf, "usermap=")) {   /* hand the kernel the users of /etc/passwd */
        FILE *pw = fopen("/etc/passwd", "r");
        char line[160], map[96] = "";
        while (pw && fgets(line, sizeof line, pw)) {
            char name[32];
            unsigned uid, gid;
            if (sscanf(line, "%31[^:]:%*[^:]:%u:%u:", name, &uid, &gid) != 3 || uid < 1000) continue;
            char one[48];
            snprintf(one, sizeof one, "%s%s:%u:%u", map[0] ? "/" : "", name, uid, gid);
            if (strlen(map) + strlen(one) + strlen(optbuf) + 10 > sizeof optbuf) break;
            strcat(map, one);
        }
        if (pw) fclose(pw);
        if (map[0]) { strlcat(optbuf, ",usermap=", sizeof optbuf); strlcat(optbuf, map, sizeof optbuf); }
    }
    if (gap_mount_opts(src, target, type, flags, optbuf[0] ? optbuf : NULL) < 0) {
        kb_error("mounting %s on %s failed: %s", src, target, strerror(errno));
        return 1;
    }
    return 0;
}

APPLET(umount) {
    if (argc < 2) { kb_error("usage: umount target"); return 1; }
    if (gap_umount(argv[1]) < 0) {
        kb_error("%s: %s", argv[1], errno == EBUSY ? "target is busy (an open file or a shell is still inside)" : strerror(errno));
        return 1;
    }
    return 0;
}

/* ---- kill ---- */

static const struct { const char *name; int num; } g_sigs[] = {
    { "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT }, { "ILL", SIGILL }, { "TRAP", SIGTRAP }, { "ABRT", SIGABRT },
    { "BUS", SIGBUS }, { "FPE", SIGFPE }, { "KILL", SIGKILL }, { "USR1", SIGUSR1 }, { "SEGV", SIGSEGV }, { "USR2", SIGUSR2 },
    { "PIPE", SIGPIPE }, { "ALRM", SIGALRM }, { "TERM", SIGTERM }, { "CHLD", SIGCHLD }, { "CONT", SIGCONT }, { "STOP", SIGSTOP },
    { "TSTP", SIGTSTP }, { "TTIN", SIGTTIN }, { "TTOU", SIGTTOU },
};

static int sig_by_name(const char *s) {
    if (!strncmp(s, "SIG", 3)) s += 3;
    if (isdigit((unsigned char)s[0])) return atoi(s);
    for (unsigned i = 0; i < sizeof g_sigs / sizeof g_sigs[0]; i++) if (!strcmp(g_sigs[i].name, s)) return g_sigs[i].num;
    return -1;
}

APPLET(kill) {
    int sig = SIGTERM, i = 1, rc = 0;
    if (argc > 1 && !strcmp(argv[1], "-l")) {
        for (unsigned k = 0; k < sizeof g_sigs / sizeof g_sigs[0]; k++) printf("%2d) SIG%-5s%s", g_sigs[k].num, g_sigs[k].name, k % 5 == 4 ? "\n" : " ");
        putchar('\n');
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "-s")) { sig = sig_by_name(argv[2]); i = 3; }
    else if (argc > 1 && argv[1][0] == '-' && argv[1][1]) { sig = sig_by_name(argv[1] + 1); i = 2; }
    if (sig < 0) { kb_error("invalid signal"); return 1; }
    if (i >= argc) { kb_error("usage: kill [-SIGNAL] pid..."); return 1; }
    for (; i < argc; i++) {
        if (kill((pid_t)atoi(argv[i]), sig) < 0) { kb_error("(%s) - %s", argv[i], strerror(errno)); rc = 1; }
    }
    return rc;
}

/* ---- ps / top: read /proc/<pid>/stat ---- */

typedef struct ProcInfo {
    int pid, ppid, pgid;
    unsigned uid, gid;
    char state[4];
    unsigned long vsz_kb, rss_kb, ticks, start_ms;
    char name[32];
    char cmd[128];
    double cpu;
} ProcInfo;

static int read_proc(int pid, ProcInfo *p, int want_cmd) {
    char path[64], buf[256];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    memset(p, 0, sizeof *p);
    if (sscanf(buf, "%d %d %d %u %u %3s %lu %lu %lu %lu %31s", &p->pid, &p->ppid, &p->pgid, &p->uid, &p->gid, p->state, &p->vsz_kb,
               &p->rss_kb, &p->ticks, &p->start_ms, p->name) < 11)
        return -1;
    strlcpy(p->cmd, p->name, sizeof p->cmd);
    if (want_cmd) {
        snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
        fd = open(path, O_RDONLY);
        if (fd >= 0) {
            n = read(fd, p->cmd, sizeof p->cmd - 1);
            close(fd);
            if (n > 0) { p->cmd[n] = 0; char *nl = strchr(p->cmd, '\n'); if (nl) *nl = 0; }
        }
    }
    return 0;
}

static ProcInfo *list_procs(int *count, int want_cmd) {
    DIR *d = opendir("/proc");
    ProcInfo *v = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        ProcInfo pi;
        if (read_proc(atoi(e->d_name), &pi, want_cmd) < 0) continue;
        if (n == cap) { cap = cap ? cap * 2 : 32; v = realloc(v, (size_t)cap * sizeof *v); }
        v[n++] = pi;
    }
    if (d) closedir(d);
    *count = n;
    return v;
}

static int cmp_pid(const void *a, const void *b) { return ((const ProcInfo *)a)->pid - ((const ProcInfo *)b)->pid; }
static int cmp_cpu(const void *a, const void *b) {
    const ProcInfo *x = a, *y = b;
    if (x->cpu != y->cpu) return x->cpu < y->cpu ? 1 : -1;
    return x->pid - y->pid;
}

static void fmt_time(unsigned long ticks, char *out, size_t n) {
    unsigned long s = ticks / 1000;
    snprintf(out, n, "%lu:%02lu", s / 60, s % 60);
}

APPLET(ps) {
    int full = 0, wide = 0, aux = 0, threads = 0, lng = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-') a++;
        for (; *a; a++) {
            if (*a == 'f') full = 1;
            else if (*a == 'a' || *a == 'x' || *a == 'e' || *a == 'A') { }
            else if (*a == 'u') aux = 1;
            else if (*a == 'w') wide = 1;
            else if (*a == 'T') threads = 1;
            else if (*a == 'l') lng = 1;
        }
    }
    int n;
    ProcInfo *v = list_procs(&n, full || aux);
    qsort(v, (size_t)n, sizeof *v, cmp_pid);
    if (threads) {                                  /* -T: one row per thread (the first thread's TID is the PID) */
        printf("%5s %5s %-8s %s %6s  %s\n", "PID", "TID", "USER", "S", "TIME", "COMMAND");
        for (int i = 0; i < n; i++) {
            char path[64], buf[8192];
            snprintf(path, sizeof path, "/proc/%d/threads", v[i].pid);
            int fd = open(path, O_RDONLY);
            if (fd < 0) continue;
            ssize_t got = read(fd, buf, sizeof buf - 1);
            close(fd);
            if (got <= 0) continue;
            buf[got] = 0;
            for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
                int tid;
                char st[4];
                unsigned long ticks;
                char name[32] = "";
                if (sscanf(line, "%d %3s %lu %31s", &tid, st, &ticks, name) < 3) continue;
                char tm[16];
                fmt_time(ticks, tm, sizeof tm);
                printf("%5d %5d %-8s %s %6s  %s\n", v[i].pid, tid, kb_user_name(v[i].uid), st, tm, name);
            }
        }
        free(v);
        return 0;
    }
    int cols = kb_term_cols();
    if (lng) {                                      /* -l: long format with the nice value (NI) from /proc/<pid>/status */
        printf("%s %-8s %5s %5s %3s %6s  %s\n", "S", "USER", "PID", "PPID", "NI", "TIME", "COMMAND");
        for (int i = 0; i < n; i++) {
            ProcInfo *p = &v[i];
            char path[64], buf[512], tm[16];
            int nice = 0;
            snprintf(path, sizeof path, "/proc/%d/status", p->pid);
            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                ssize_t got = read(fd, buf, sizeof buf - 1);
                close(fd);
                if (got > 0) { buf[got] = 0; char *q = strstr(buf, "Nice:\t"); if (q) nice = atoi(q + 6); }
            }
            fmt_time(p->ticks, tm, sizeof tm);
            printf("%s %-8s %5d %5d %3d %6s  %s\n", p->state, kb_user_name(p->uid), p->pid, p->ppid, nice, tm, p->name);
        }
        free(v);
        return 0;
    }
    if (aux) printf("%-8s %5s %5s %7s %6s  %s %6s  %s\n", "USER", "PID", "PPID", "VSZ", "RSS", "S", "TIME", "COMMAND");
    else if (full) printf("%-8s %5s %5s %s %6s  %s\n", "UID", "PID", "PPID", "S", "TIME", "COMMAND");
    else printf("%5s %-8s %s %6s  %s\n", "PID", "USER", "S", "TIME", "COMMAND");
    for (int i = 0; i < n; i++) {
        ProcInfo *p = &v[i];
        char tm[16];
        fmt_time(p->ticks, tm, sizeof tm);
        const char *cmd = (aux || full) ? p->cmd : p->name;
        int room = wide ? 1000 : cols - (aux ? 50 : 30);
        if (room < 10) room = 10;
        if (aux) printf("%-8s %5d %5d %7lu %6lu  %s %6s  %.*s\n", kb_user_name(p->uid), p->pid, p->ppid, p->vsz_kb, p->rss_kb, p->state, tm, room, cmd);
        else if (full) printf("%-8s %5d %5d %s %6s  %.*s\n", kb_user_name(p->uid), p->pid, p->ppid, p->state, tm, room, cmd);
        else printf("%5d %-8s %s %6s  %.*s\n", p->pid, kb_user_name(p->uid), p->state, tm, room, cmd);
    }
    free(v);
    return 0;
}

APPLET(pidof) {
    int n, found = 0;
    ProcInfo *v = list_procs(&n, 0);
    for (int a = 1; a < argc; a++)
        for (int i = 0; i < n; i++)
            if (!strcmp(v[i].name, argv[a])) { printf(found ? " %d" : "%d", v[i].pid); found = 1; }
    if (found) putchar('\n');
    free(v);
    return found ? 0 : 1;
}

APPLET(pkill) {
    int sig = SIGTERM, i = 1;
    if (i < argc && argv[i][0] == '-') { sig = sig_by_name(argv[i] + 1); i++; }
    if (i >= argc) { kb_error("usage: pkill [-SIGNAL] name"); return 2; }
    int n, hit = 0;
    ProcInfo *v = list_procs(&n, 0);
    for (int k = 0; k < n; k++)
        if (v[k].pid != getpid() && strstr(v[k].name, argv[i])) { kill(v[k].pid, sig); hit++; }
    free(v);
    return hit ? 0 : 1;
}

static struct termios g_top_saved;
static void top_restore(void) { tcsetattr(0, TCSANOW, &g_top_saved); fputs("\033[?25h", stdout); fflush(stdout); }

APPLET(top) {
    int batch = 0, iters = -1, delay_ms = 2000;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b")) batch = 1;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) delay_ms = (int)(atof(argv[++i]) * 1000);
    }
    if (!isatty(0)) batch = 1;
    if (!batch) {
        struct termios raw;
        tcgetattr(0, &g_top_saved);
        raw = g_top_saved;
        raw.c_lflag &= ~(unsigned)(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &raw);
        atexit(top_restore);
        fputs("\033[?25l", stdout);
    }
    unsigned long prev_ticks[1024] = {0};
    int prev_pid[1024] = {0};
    int nprev = 0;
    unsigned long long prev_up = proc_uptime_ms();
    for (int round = 0; iters < 0 || round < iters; round++) {
        if (round) {
            if (batch) usleep((useconds_t)delay_ms * 1000);
            else {
                struct pollfd pf = { 0, POLLIN, 0 };
                if (poll(&pf, 1, delay_ms) > 0) {
                    char c;
                    if (read(0, &c, 1) == 1 && (c == 'q' || c == 'Q' || c == 3)) break;
                }
            }
        }
        int n;
        ProcInfo *v = list_procs(&n, 0);
        unsigned long long up = proc_uptime_ms();
        double dt = (double)(up - prev_up);
        if (dt < 1) dt = 1;
        for (int i = 0; i < n; i++) {
            v[i].cpu = 0;
            for (int k = 0; k < nprev; k++)
                if (prev_pid[k] == v[i].pid) { v[i].cpu = (double)(v[i].ticks - prev_ticks[k]) * 100.0 / dt; break; }
            if (round == 0 && up > 0) v[i].cpu = (double)v[i].ticks * 100.0 / (double)up;
        }
        nprev = n < 1024 ? n : 1024;
        for (int i = 0; i < nprev; i++) { prev_pid[i] = v[i].pid; prev_ticks[i] = v[i].ticks; }
        prev_up = up;
        qsort(v, (size_t)n, sizeof *v, cmp_cpu);

        unsigned long total = meminfo("MemTotal"), freek = meminfo("MemFree");
        char up_s[32];
        fmt_time(up, up_s, sizeof up_s);
        if (!batch) fputs("\033[H\033[2J", stdout);
        printf("\033[1mtop\033[0m - up %s, %d processes   Mem: %luM total, %luM used, %luM free\n", up_s, n, total / 1024, (total - freek) / 1024, freek / 1024);
        printf("\033[7m%5s %-8s %s %7s %6s %5s  %-s\033[0m\n", "PID", "USER", "S", "VSZ", "RSS", "%CPU", "COMMAND");
        int rows = batch ? n : 20;
        for (int i = 0; i < n && i < rows; i++)
            printf("%5d %-8s %s %7lu %6lu %5.1f  %s\n", v[i].pid, kb_user_name(v[i].uid), v[i].state, v[i].vsz_kb, v[i].rss_kb, v[i].cpu, v[i].name);
        if (!batch) printf("\n  (q to quit)");
        fflush(stdout);
        free(v);
    }
    if (!batch) { top_restore(); putchar('\n'); }
    return 0;
}

APPLET(which) {
    int rc = 0;
    const char *path = getenv("PATH");
    if (!path) path = "/bin:/sbin";
    for (int i = 1; i < argc; i++) {
        int found = 0;
        if (strchr(argv[i], '/')) { if (access(argv[i], X_OK) == 0) { puts(argv[i]); found = 1; } }
        else {
            char *copy = strdup(path), *save, *dir;
            for (dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
                char *full = kb_join(dir, argv[i]);
                if (access(full, X_OK) == 0) { puts(full); found = 1; free(full); break; }
                free(full);
            }
            free(copy);
        }
        if (!found) rc = 1;
    }
    return rc;
}
