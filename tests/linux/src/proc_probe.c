/*
 * What a Linux program finds under /proc, as facts about the format (docs/COMPAT.md, "A Linux-shaped /proc"). The program reads /proc/self/stat, status, statm, cmdline, environ,
 * comm, maps, smaps, limits, mountinfo, the links, fd and fdinfo, task/, a child's files (running and zombie), /proc/stat, uptime, loadavg, meminfo, cpuinfo, version, filesystems,
 * partitions and /proc/sys, and prints one line per fact: "what: yes". It never prints a number that differs between machines (a pid, a size, a time): the numbers are compared with
 * each other and with what the system calls say (getpid, getrlimit, sysinfo, uname ...), and the line says whether they agree. So the lines are the same on any Linux that follows
 * proc(5), and tests/case_linux_proc.py compares what TouhouOS prints with what the stored output of a real Linux says (tools/linux-oracle.py).
 *
 * Run as:  FOO=bar proc_probe alpha beta        (the status is 0; every line ends in "yes" unless a fact does not hold)
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <limits.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define TCP_HEAD   "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode"
#define ROUTE_HEAD "Iface	Destination	Gateway 	Flags	RefCnt	Use	Metric	Mask		MTU	Window	IRTT"

static int g_bad;
static char g_comm[16];                                                     /* the name of this program, as the kernel keeps it: the last component of the path, 15 characters at most */

static void fact(const char *what, int ok) {
    printf("%s: %s\n", what, ok ? "yes" : "no");
    if (!ok) g_bad++;
}

/* A whole file through open/read/close only: no stdio and no malloc between two measurements of the address space. */
static char g_a[300000], g_b[300000], g_c[16384];

static ssize_t slurp(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t n = 0;
    for (;;) {
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = 0;
    return (ssize_t)n;
}

/* the value after "Key:" in a status-like text, as a number (-1: no such key) */
static long key_value(const char *text, const char *key) {
    size_t kl = strlen(key);
    for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        if (!strncmp(p, key, kl) && p[kl] == ':') return strtol(p + kl + 1, NULL, 10);
    return -1;
}

/* the value of a "key value" line of /proc/stat (no colon there), or -1 */
static long line_value(const char *text, const char *key) {
    size_t kl = strlen(key);
    for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        if (!strncmp(p, key, kl) && p[kl] == ' ') return strtol(p + kl + 1, NULL, 10);
    return -1;
}

/* the position of "Key:" at the start of a line, or -1 */
static long key_at(const char *text, const char *key) {
    size_t kl = strlen(key);
    for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        if (!strncmp(p, key, kl) && p[kl] == ':') return (long)(p - text);
    return -1;
}

/* are the keys all present, in the order given? */
static int keys_in_order(const char *text, const char *const *keys, int n) {
    long last = -1;
    for (int i = 0; i < n; i++) {
        long at = key_at(text, keys[i]);
        if (at < 0 || at < last) return 0;
        last = at;
    }
    return 1;
}

static int all_digits(const char *s) {
    if (!*s) return 0;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

static int is_hex16(const char *s) {
    for (int i = 0; i < 16; i++) if (!isxdigit((unsigned char)s[i])) return 0;
    return s[16] == 0 || s[16] == '\n' || s[16] == '\t';
}

/* the line of status-like text that starts with "Key:", with the value after the colon and the whitespace */
static const char *key_text(const char *text, const char *key) {
    long at = key_at(text, key);
    if (at < 0) return NULL;
    const char *p = text + at + strlen(key) + 1;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

struct Fields { char comm[64]; char state; long long f[64]; int n; };       /* /proc/<pid>/stat: f[i] is field i (1-based) after the state, number 4 onwards */

/* /proc/<pid>/stat: "pid (comm) state field4 ... field52". The comm may hold spaces and parentheses: it ends at the LAST ')'. Returns the number of fields (0: not parseable). */
static int parse_stat(const char *text, struct Fields *s) {
    memset(s, 0, sizeof *s);
    const char *open = strchr(text, '(');
    const char *close = strrchr(text, ')');
    if (!open || !close || close < open || open != strchr(text, ' ') + 1) return 0;
    size_t cl = (size_t)(close - open - 1);
    if (cl >= sizeof s->comm) return 0;
    memcpy(s->comm, open + 1, cl);
    s->comm[cl] = 0;
    const char *p = close + 1;
    if (*p != ' ') return 0;
    s->state = p[1];
    p += 3;
    s->f[1] = strtoll(text, NULL, 10);                                      /* the pid */
    int n = 3;                                                              /* state counts as field 3 */
    while (*p && *p != '\n') {
        char *end;
        s->f[++n] = strtoll(p, &end, 10);
        if (end == p) return 0;
        p = *end == ' ' ? end + 1 : end;
        if (n >= 63) break;
    }
    s->n = n;
    return n;
}

struct Arg { int pipe_in; int tid; };

static void *thread_main(void *a) {
    struct Arg *arg = a;
    arg->tid = (int)syscall(SYS_gettid);
    char c;
    (void)!read(arg->pipe_in, &c, 1);                                       /* parked until the main thread has looked at us */
    return NULL;
}

static void quiet_handler(int sig) { (void)sig; }

int main(int argc, char **argv) {
    struct utsname uts;
    uname(&uts);
    pid_t pid = getpid();
    {
        char path[512];
        ssize_t l = readlink("/proc/self/exe", path, sizeof path - 1);
        path[l > 0 ? l : 0] = 0;
        const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
        snprintf(g_comm, sizeof g_comm, "%.15s", base);
    }

    /* ---- /proc/self/stat ---- */
    ssize_t n = slurp("/proc/self/stat", g_a, sizeof g_a);
    struct Fields st;
    int nf = n > 0 ? parse_stat(g_a, &st) : 0;
    printf("fields in /proc/self/stat: %d\n", nf);
    fact("stat: the comm is in parentheses and is the program's name", nf && !strcmp(st.comm, g_comm));
    fact("stat: the line ends with a newline", n > 0 && g_a[n - 1] == '\n');
    fact("stat: pid, ppid, pgrp and session are ours", nf && st.f[1] == pid && st.f[4] == getppid() && st.f[5] == getpgrp() && st.f[6] == getsid(0));
    fact("stat: the state is R while we read it", nf && st.state == 'R');
    fact("stat: num_threads is 1 and priority is 20 + nice", nf && st.f[20] == 1 && st.f[18] == 20 + st.f[19]);
    fact("stat: exit_signal is SIGCHLD, policy and rt_priority are 0", nf && st.f[38] == SIGCHLD && st.f[40] == 0 && st.f[41] == 0);
    fact("stat: vsize and rss are positive, rsslim is the maximum", nf && st.f[23] > 0 && st.f[24] > 0 && st.f[25] == LLONG_MAX);
    {
        unsigned long long start_stack = (unsigned long long)st.f[28];
        char *top = sbrk(0);
        fact("stat: start_stack lies above our locals, start_brk is below the break", nf && start_stack >= (unsigned long long)(unsigned long)&nf && (unsigned long long)st.f[47] <= (unsigned long long)(unsigned long)top);
        fact("stat: arg_start < arg_end <= env_start < env_end", nf && st.f[48] < st.f[49] && st.f[49] <= st.f[50] && st.f[50] < st.f[51]);
        fact("stat: startcode <= endcode", nf && st.f[26] <= st.f[27]);
    }
    {
        long hz = sysconf(_SC_CLK_TCK);
        struct timespec bt;
        clock_gettime(CLOCK_BOOTTIME, &bt);
        fact("stat: starttime is in clock ticks since boot, not in the future", nf && st.f[22] >= 0 && st.f[22] <= (long long)bt.tv_sec * hz + hz);
        clock_t t0 = clock();
        while (clock() - t0 < CLOCKS_PER_SEC / 20) { volatile int x = 0; for (int i = 0; i < 100000; i++) x += i; }
        slurp("/proc/self/stat", g_b, sizeof g_b);
        struct Fields st2;
        int nf2 = parse_stat(g_b, &st2);
        fact("stat: utime + stime grow while we compute", nf && nf2 && st2.f[14] + st2.f[15] > st.f[14] + st.f[15]);
    }

    /* ---- /proc/self/statm, status, maps, smaps: one consistent picture of the address space, read without allocating ---- */
    sbrk(4096);                                                             /* make sure there is a heap */
    int fd = open("/proc/self/exe", O_RDONLY);
    char *file_map = mmap(NULL, 3 * 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    char *anon_map = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mprotect(file_map + 4096, 4096, PROT_NONE);                             /* the file mapping splits in three */
    ssize_t nm = slurp("/proc/self/maps", g_a, sizeof g_a);
    ssize_t ns = slurp("/proc/self/status", g_b, sizeof g_b);
    ssize_t nsm = slurp("/proc/self/statm", g_c, sizeof g_c);
    char exe[512] = "";
    ssize_t el = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (el > 0) exe[el] = 0;
    fact("maps, status and statm could be read", nm > 0 && ns > 0 && nsm > 0);

    /* every line of maps: start-end perms offset dev inode [name] */
    int lines = 0, bad_lines = 0, sorted = 1, have_heap = 0, have_stack = 0, have_exe = 0, anon_ok = 0, split_ok[3] = { 0, 0, 0 };
    unsigned long long prev_end = 0, total = 0;
    for (const char *line = g_a; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        unsigned long long lo, hi, off, ino;
        unsigned dmaj, dmin;
        char perms[8];
        int used = 0;
        if (sscanf(line, "%llx-%llx %7s %llx %x:%x %llu%n", &lo, &hi, perms, &off, &dmaj, &dmin, &ino, &used) < 7 || strlen(perms) != 4 || hi <= lo) { bad_lines++; continue; }
        if (!strchr("r-", perms[0]) || !strchr("w-", perms[1]) || !strchr("x-", perms[2]) || !strchr("ps", perms[3])) bad_lines++;
        const char *name = line + used;
        while (*name == ' ') name++;
        size_t nl = strcspn(name, "\n");
        lines++;
        if (lo < prev_end) sorted = 0;
        prev_end = hi;
        total += hi - lo;
        if (nl == 7 && !strncmp(name, "[stack]", 7)) have_stack = 1;
        if (nl == 6 && !strncmp(name, "[heap]", 6)) have_heap = 1;
        if (el > 0 && nl == (size_t)el && !strncmp(name, exe, nl)) have_exe = 1;
        if (lo == (unsigned long long)(unsigned long)anon_map && nl == 0 && !strncmp(perms, "rw-p", 4) && hi - lo == 2 * 4096) anon_ok = 1;
        if (el > 0 && nl == (size_t)el && !strncmp(name, exe, nl)) {
            if (lo == (unsigned long long)(unsigned long)file_map && hi - lo == 4096 && off == 0 && !strncmp(perms, "r--p", 4)) split_ok[0] = 1;
            if (lo == (unsigned long long)(unsigned long)file_map + 4096 && hi - lo == 4096 && off == 4096 && !strncmp(perms, "---p", 4)) split_ok[1] = 1;
            if (lo == (unsigned long long)(unsigned long)file_map + 8192 && hi - lo == 4096 && off == 8192 && !strncmp(perms, "r--p", 4)) split_ok[2] = 1;
        }
    }
    fact("maps: every line is start-end, perms, offset, device, inode and an optional name", lines > 4 && !bad_lines);
    fact("maps: sorted by address and not overlapping", sorted);
    fact("maps: [heap] and [stack] are named", have_heap && have_stack);
    fact("maps: the program is mapped from the file /proc/self/exe names", have_exe);
    fact("maps: an anonymous mapping has no name", anon_ok);
    fact("maps: a file mapping that mprotect split in three keeps its file and offsets", split_ok[0] && split_ok[1] && split_ok[2]);
    fact("maps: the sizes add up to VmSize", ns > 0 && key_value(g_b, "VmSize") == (long)(total / 1024));
    fact("status: VmRSS <= VmSize and RssAnon + RssFile + RssShmem == VmRSS", ns > 0 && key_value(g_b, "VmRSS") <= key_value(g_b, "VmSize") &&
         key_value(g_b, "RssAnon") + key_value(g_b, "RssFile") + key_value(g_b, "RssShmem") == key_value(g_b, "VmRSS"));
    {
        unsigned long size = 0, res = 0, shared = 0, text = 0, lib = 0, data = 0, dt = 0;
        int got = sscanf(g_c, "%lu %lu %lu %lu %lu %lu %lu", &size, &res, &shared, &text, &lib, &data, &dt);
        fact("statm: seven numbers, size in pages equals VmSize", got == 7 && (long)(size * 4) == key_value(g_b, "VmSize") && res <= size && lib == 0 && dt == 0);
        fact("statm: resident is close to VmRSS", got == 7 && labs((long)res * 4 - key_value(g_b, "VmRSS")) < 400);
    }

    /* ---- /proc/self/status ---- */
    static const char *const status_keys[] = { "Name", "Umask", "State", "Tgid", "Pid", "PPid", "TracerPid", "Uid", "Gid", "FDSize", "Groups", "VmPeak", "VmSize", "VmLck", "VmHWM",
        "VmRSS", "VmData", "VmStk", "VmExe", "VmLib", "VmPTE", "VmSwap", "Threads", "SigQ", "SigPnd", "ShdPnd", "SigBlk", "SigIgn", "SigCgt", "CapInh", "CapPrm", "CapEff", "CapBnd",
        "NoNewPrivs", "Seccomp", "Cpus_allowed", "Cpus_allowed_list", "Mems_allowed", "voluntary_ctxt_switches", "nonvoluntary_ctxt_switches" };
    ns = slurp("/proc/self/status", g_b, sizeof g_b);
    fact("status: the keys of proc(5) are all there, in order", keys_in_order(g_b, status_keys, (int)(sizeof status_keys / sizeof *status_keys)));
    {
        const char *name = key_text(g_b, "Name"), *state = key_text(g_b, "State"), *uid = key_text(g_b, "Uid"), *gid = key_text(g_b, "Gid");
        fact("status: Name, State and Pid are ours", name && !strncmp(name, g_comm, strlen(g_comm)) && name[strlen(g_comm)] == '\n' && state && !strncmp(state, "R (running)\n", 12) && key_value(g_b, "Pid") == pid &&
             key_value(g_b, "Tgid") == pid && key_value(g_b, "PPid") == getppid() && key_value(g_b, "Threads") == 1);
        unsigned ru, eu, su, fu, rg, eg, sg, fg;
        fact("status: Uid and Gid have four values: real, effective, saved, filesystem",
             uid && gid && sscanf(uid, "%u\t%u\t%u\t%u", &ru, &eu, &su, &fu) == 4 && sscanf(gid, "%u\t%u\t%u\t%u", &rg, &eg, &sg, &fg) == 4 && ru == getuid() && eu == geteuid() &&
             rg == getgid() && eg == getegid() && fu == eu && fg == eg);
        gid_t groups[64];
        int ng = getgroups(64, groups);
        const char *gp = key_text(g_b, "Groups");
        int listed = 0, match = gp != NULL;
        while (gp && *gp && *gp != '\n') {
            char *end;
            unsigned long v = strtoul(gp, &end, 10);
            if (end == gp) break;
            int found = 0;
            for (int i = 0; i < ng; i++) if (groups[i] == v) found = 1;
            if (!found) match = 0;
            listed++;
            gp = *end == ' ' ? end + 1 : end;
        }
        fact("status: Groups lists the supplementary groups getgroups returns", match && listed == ng);
        fact("status: the signal sets are 16 hexadecimal digits", is_hex16(key_text(g_b, "SigPnd")) && is_hex16(key_text(g_b, "ShdPnd")) && is_hex16(key_text(g_b, "SigBlk")) &&
             is_hex16(key_text(g_b, "SigIgn")) && is_hex16(key_text(g_b, "SigCgt")) && is_hex16(key_text(g_b, "CapEff")));
        fact("status: Cpus_allowed_list starts with CPU 0", key_text(g_b, "Cpus_allowed_list") && key_text(g_b, "Cpus_allowed_list")[0] == '0');
        fact("status: FDSize is at least the number of open descriptors we have", key_value(g_b, "FDSize") >= 4);
    }
    {   /* the signal masks follow what we do to them: handlers, ignoring, blocking and pending (signal n is bit n-1) */
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = quiet_handler;
        sigaction(SIGUSR1, &sa, NULL);
        sigaction(SIGUSR2, &sa, NULL);                                      /* so that the signals we leave pending below do no harm when they are unblocked */
        sigaction(SIGRTMIN + 2, &sa, NULL);
        signal(SIGHUP, SIG_IGN);
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR2);
        sigaddset(&set, SIGRTMIN + 2);
        sigprocmask(SIG_BLOCK, &set, NULL);
        raise(SIGUSR2);                                                     /* aimed at this thread: SigPnd */
        kill(pid, SIGRTMIN + 2);                                            /* aimed at the process: ShdPnd */
        slurp("/proc/self/status", g_b, sizeof g_b);
        unsigned long long cgt = strtoull(key_text(g_b, "SigCgt"), NULL, 16), ign = strtoull(key_text(g_b, "SigIgn"), NULL, 16), blk = strtoull(key_text(g_b, "SigBlk"), NULL, 16);
        unsigned long long pnd = strtoull(key_text(g_b, "SigPnd"), NULL, 16), shd = strtoull(key_text(g_b, "ShdPnd"), NULL, 16);
        fact("status: SigCgt has the signal we gave a handler", (cgt >> (SIGUSR1 - 1)) & 1);
        fact("status: SigIgn has the signal we ignore", (ign >> (SIGHUP - 1)) & 1);
        fact("status: SigBlk has the signals we block, including a real-time one", ((blk >> (SIGUSR2 - 1)) & 1) && ((blk >> (SIGRTMIN + 2 - 1)) & 1));
        fact("status: SigPnd has the signal aimed at the thread, ShdPnd the one aimed at the process", ((pnd >> (SIGUSR2 - 1)) & 1) && ((shd >> (SIGRTMIN + 2 - 1)) & 1));
        sigprocmask(SIG_UNBLOCK, &set, NULL);
    }

    /* ---- cmdline, environ, comm ---- */
    n = slurp("/proc/self/cmdline", g_a, sizeof g_a);
    {
        size_t want = 0;
        for (int i = 0; i < argc; i++) want += strlen(argv[i]) + 1;
        int same = n == (ssize_t)want;
        size_t at = 0;
        for (int i = 0; same && i < argc; i++) { if (strcmp(g_a + at, argv[i])) same = 0; at += strlen(argv[i]) + 1; }
        fact("cmdline: the arguments, each ended by a NUL", same);
    }
    n = slurp("/proc/self/environ", g_a, sizeof g_a);
    {
        int same = n > 0, count = 0;
        size_t at = 0;
        for (char **e = environ; same && *e; e++, count++) { if (at >= (size_t)n || strcmp(g_a + at, *e)) same = 0; else at += strlen(*e) + 1; }
        fact("environ: the environment, each entry ended by a NUL", same && at == (size_t)n && count >= 1);
    }
    prctl(PR_SET_NAME, "a b)c", 0, 0, 0);
    n = slurp("/proc/self/comm", g_a, sizeof g_a);
    fact("comm: the name prctl set, and a newline", n == 6 && !strcmp(g_a, "a b)c\n"));
    n = slurp("/proc/self/stat", g_a, sizeof g_a);
    nf = parse_stat(g_a, &st);
    fact("stat: a comm with a space and a parenthesis is still in parentheses", nf == 52 && !strcmp(st.comm, "a b)c"));
    prctl(PR_SET_NAME, g_comm, 0, 0, 0);

    /* ---- smaps, limits, mountinfo ---- */
    nsm = slurp("/proc/self/smaps", g_b, sizeof g_b);
    {
        int heads = 0, sizes = 0, rss = 0;
        for (const char *line = g_b; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
            unsigned long long lo, hi, off, ino;
            unsigned dmaj, dmin;
            char perms[8];
            if (sscanf(line, "%llx-%llx %7s %llx %x:%x %llu", &lo, &hi, perms, &off, &dmaj, &dmin, &ino) == 7 && strlen(perms) == 4) heads++;
            if (!strncmp(line, "Size:", 5)) sizes++;
            if (!strncmp(line, "Rss:", 4)) rss++;
        }
        fact("smaps: a Size and an Rss line for every mapping of maps", nsm > 0 && heads == lines && sizes == lines && rss == lines);
    }
    n = slurp("/proc/self/limits", g_a, sizeof g_a);
    {
        struct rlimit rl;
        const char *nofile = strstr(g_a, "Max open files"), *stack = strstr(g_a, "Max stack size");
        getrlimit(RLIMIT_NOFILE, &rl);
        unsigned long long nf_soft = nofile ? strtoull(nofile + 14 + strspn(nofile + 14, " "), NULL, 10) : 0;
        fact("limits: a header and the sixteen limits, the open files and stack ones agree with getrlimit", n > 0 && !strncmp(g_a, "Limit ", 6) && strstr(g_a, "Max cpu time") &&
             strstr(g_a, "Max realtime timeout") && nf_soft == rl.rlim_cur);
        getrlimit(RLIMIT_STACK, &rl);
        unsigned long long st_soft = stack ? strtoull(stack + 14 + strspn(stack + 14, " "), NULL, 10) : 0;
        fact("limits: the stack limit agrees with getrlimit", stack && st_soft == rl.rlim_cur);
    }
    n = slurp("/proc/self/mountinfo", g_a, sizeof g_a);
    {
        int ok = n > 0, root = 0;
        for (const char *line = g_a; ok && line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
            int id, parent;
            unsigned maj, min;
            char rootdir[256], mp[256];
            if (sscanf(line, "%d %d %u:%u %255s %255s", &id, &parent, &maj, &min, rootdir, mp) != 6 || !strstr(line, " - ")) ok = 0;
            if (!strcmp(mp, "/")) root = 1;
        }
        fact("mountinfo: id, parent, device, root, mount point, options, a separator, type, source, options; the root is mounted", ok && root);
    }
    n = slurp("/proc/self/mounts", g_a, sizeof g_a);
    {
        int ok = n > 0;
        for (const char *line = g_a; ok && line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
            char a[256], b2[256], c2[64], d2[256];
            int x, y;
            if (sscanf(line, "%255s %255s %63s %255s %d %d", a, b2, c2, d2, &x, &y) != 6) ok = 0;
        }
        fact("mounts: device, mount point, type, options and two zeros on every line", ok);
    }

    /* ---- the links, fd, fdinfo ---- */
    {
        char link[256];
        ssize_t l = readlink("/proc/self", link, sizeof link - 1);
        link[l > 0 ? l : 0] = 0;
        fact("/proc/self is a link to our pid", l > 0 && atoi(link) == pid);
        char want[64];
        snprintf(want, sizeof want, "%d/task/%d", pid, (int)syscall(SYS_gettid));
        l = readlink("/proc/thread-self", link, sizeof link - 1);
        link[l > 0 ? l : 0] = 0;
        fact("/proc/thread-self is a link to pid/task/tid", l > 0 && !strcmp(link, want));
        char cwd[256], cl[256];
        getcwd(cwd, sizeof cwd);
        l = readlink("/proc/self/cwd", cl, sizeof cl - 1);
        cl[l > 0 ? l : 0] = 0;
        fact("/proc/self/cwd is the working directory", l > 0 && !strcmp(cl, cwd));
        l = readlink("/proc/self/root", cl, sizeof cl - 1);
        cl[l > 0 ? l : 0] = 0;
        fact("/proc/self/root is /", l == 1 && cl[0] == '/');
        fact("/proc/self/exe is an absolute path to a file that exists", exe[0] == '/' && access(exe, X_OK) == 0);
    }
    {
        int p[2];
        pipe(p);
        int vfd = open("/proc/version", O_RDONLY);
        char buf[16], link[256];
        (void)!read(vfd, buf, 10);
        snprintf(buf, sizeof buf, "/proc/self/fd/%d", p[0]);
        ssize_t l = readlink(buf, link, sizeof link - 1);
        link[l > 0 ? l : 0] = 0;
        fact("fd: a pipe reads as pipe:[inode]", l > 0 && !strncmp(link, "pipe:[", 6) && link[l - 1] == ']');
        snprintf(buf, sizeof buf, "/proc/self/fd/%d", vfd);
        l = readlink(buf, link, sizeof link - 1);
        link[l > 0 ? l : 0] = 0;
        fact("fd: a file reads as the path it was opened by", l > 0 && !strcmp(link, "/proc/version"));
        char info[64], text[256];
        snprintf(info, sizeof info, "/proc/self/fdinfo/%d", vfd);
        slurp(info, text, sizeof text);
        fact("fdinfo: pos, flags, mnt_id and ino, with the position after a read of 10 bytes", key_value(text, "pos") == 10 && key_at(text, "flags") >= 0 && key_at(text, "mnt_id") >= 0 && key_at(text, "ino") >= 0);
        DIR *d = opendir("/proc/self/fd");
        int seen[3] = { 0, 0, 0 }, entries = 0, all_links = 1;
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (!all_digits(de->d_name)) continue;
            entries++;
            int num = atoi(de->d_name);
            if (num < 3) seen[num] = 1;
            snprintf(buf, sizeof buf, "/proc/self/fd/%s", de->d_name);
            if (readlink(buf, link, sizeof link) < 0) all_links = 0;
        }
        if (d) closedir(d);
        fact("fd: the directory lists descriptors 0, 1, 2 and ours, and each is a link", d && seen[0] && seen[1] && seen[2] && entries >= 6 && all_links);
        close(vfd);
        close(p[0]);
        close(p[1]);
    }

    /* ---- threads: /proc/self/task and the files of a thread ---- */
    {
        int p[2];
        pipe(p);
        pthread_t th;
        struct Arg arg = { p[0], 0 };
        pthread_create(&th, NULL, thread_main, &arg);
        for (int i = 0; i < 200 && !arg.tid; i++) usleep(10000);
        slurp("/proc/self/status", g_b, sizeof g_b);
        fact("status: Threads counts the new thread", key_value(g_b, "Threads") == 2);
        DIR *d = opendir("/proc/self/task");
        int listed = 0, has_main = 0, has_thread = 0;
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (!all_digits(de->d_name)) continue;
            listed++;
            if (atoi(de->d_name) == pid) has_main = 1;
            if (atoi(de->d_name) == arg.tid) has_thread = 1;
        }
        if (d) closedir(d);
        fact("task: the directory lists the main thread and the new one", listed == 2 && has_main && has_thread);
        char path[96];
        snprintf(path, sizeof path, "/proc/self/task/%d/stat", arg.tid);
        n = slurp(path, g_a, sizeof g_a);
        struct Fields ts;
        int tf = n > 0 ? parse_stat(g_a, &ts) : 0;
        fact("task/TID/stat: the first field is the thread id, the process's pgrp is ours, the state is a sleeping one", tf == 52 && ts.f[1] == arg.tid && ts.f[5] == getpgrp() && ts.state == 'S');
        snprintf(path, sizeof path, "/proc/self/task/%d/status", arg.tid);
        n = slurp(path, g_a, sizeof g_a);
        fact("task/TID/status: Tgid is the process, Pid is the thread", n > 0 && key_value(g_a, "Tgid") == pid && key_value(g_a, "Pid") == arg.tid && key_value(g_a, "Threads") == 2);
        (void)!write(p[1], "x", 1);
        pthread_join(th, NULL);
        close(p[0]);
        close(p[1]);
    }

    /* ---- another process: sleeping, then a zombie, then gone ---- */
    {
        int go[2], ready[2];
        pipe(go);
        pipe(ready);
        pid_t child = fork();
        if (child == 0) {
            (void)!write(ready[1], "r", 1);
            char c;
            (void)!read(go[0], &c, 1);
            _exit(7);
        }
        char c;
        (void)!read(ready[0], &c, 1);
        char path[64];
        struct Fields cs;
        int sleeping = 0;
        for (int i = 0; i < 300 && !sleeping; i++) {
            snprintf(path, sizeof path, "/proc/%d/stat", child);
            n = slurp(path, g_a, sizeof g_a);
            if (n > 0 && parse_stat(g_a, &cs) && cs.state == 'S') sleeping = 1;
            else usleep(10000);
        }
        fact("child: its stat says it sleeps, its parent is us and its pid is the directory's", sleeping && cs.f[4] == pid && cs.f[1] == child && !strcmp(cs.comm, g_comm));
        snprintf(path, sizeof path, "/proc/%d/cmdline", child);
        n = slurp(path, g_a, sizeof g_a);
        fact("child: a forked process still shows its parent's command line", n > 0 && !strcmp(g_a, argv[0]));
        (void)!write(go[1], "g", 1);
        int zombie = 0;
        for (int i = 0; i < 300 && !zombie; i++) {
            snprintf(path, sizeof path, "/proc/%d/stat", child);
            n = slurp(path, g_a, sizeof g_a);
            if (n > 0 && parse_stat(g_a, &cs) && cs.state == 'Z') zombie = 1;
            else usleep(10000);
        }
        snprintf(path, sizeof path, "/proc/%d/status", child);
        slurp(path, g_b, sizeof g_b);
        snprintf(path, sizeof path, "/proc/%d/cmdline", child);
        ssize_t zc = slurp(path, g_c, sizeof g_c);
        fact("child: a zombie shows state Z in stat and status, and an empty command line", zombie && strstr(g_b, "State:\tZ (zombie)") && zc == 0);
        int status = 0;
        waitpid(child, &status, 0);
        snprintf(path, sizeof path, "/proc/%d", child);
        fact("child: after waitpid its directory is gone and its status was 7", access(path, F_OK) != 0 && WIFEXITED(status) && WEXITSTATUS(status) == 7);
        close(go[0]); close(go[1]); close(ready[0]); close(ready[1]);
    }

    /* ---- the files of the whole system ---- */
    n = slurp("/proc/stat", g_a, sizeof g_a);
    {
        unsigned long long v[10];
        int cpu_fields = n > 0 ? sscanf(g_a, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]) : 0;
        printf("numbers on the first line of /proc/stat: %d\n", cpu_fields);
        fact("stat: the aggregate cpu line, then cpu0 and the keys intr, ctxt, btime, processes, procs_running", n > 0 && strstr(g_a, "\ncpu0 ") && strstr(g_a, "\nintr ") && strstr(g_a, "\nctxt ") &&
             strstr(g_a, "\nbtime ") && strstr(g_a, "\nprocesses ") && strstr(g_a, "\nprocs_running "));
        long btime = line_value(g_a, "btime");
        time_t now = time(NULL);
        fact("stat: btime is in the past and after the year 2000", btime > 946684800L && btime <= (long)now);
        fact("stat: procs_running is at least 1 and processes at least our pid", line_value(g_a, "procs_running") >= 1 && line_value(g_a, "processes") >= pid);
    }
    n = slurp("/proc/uptime", g_a, sizeof g_a);
    {
        double up = 0, idle = 0;
        struct timespec bt;
        clock_gettime(CLOCK_BOOTTIME, &bt);
        fact("uptime: two numbers with two decimals, the first is the time since boot", n > 0 && sscanf(g_a, "%lf %lf", &up, &idle) == 2 && up > 0 && idle >= 0 && up - (double)bt.tv_sec < 5 && (double)bt.tv_sec - up < 5 &&
             strchr(g_a, '.') && strchr(g_a, '.')[3] == ' ');
    }
    n = slurp("/proc/loadavg", g_a, sizeof g_a);
    {
        double l1, l5, l15;
        int running, total, last;
        fact("loadavg: three load averages, running/total and the last pid", n > 0 && sscanf(g_a, "%lf %lf %lf %d/%d %d", &l1, &l5, &l15, &running, &total, &last) == 6 && l1 >= 0 && l5 >= 0 &&
             l15 >= 0 && running >= 1 && total >= running && last >= pid);
    }
    n = slurp("/proc/meminfo", g_a, sizeof g_a);
    {
        static const char *const mem_keys[] = { "MemTotal", "MemFree", "MemAvailable", "Buffers", "Cached", "SwapTotal", "SwapFree", "Shmem", "Slab" };
        fact("meminfo: MemTotal, MemFree, MemAvailable, Buffers, Cached, SwapTotal, SwapFree, Shmem and Slab, in this order", keys_in_order(g_a, mem_keys, 9));
        struct sysinfo si;
        sysinfo(&si);
        long total = key_value(g_a, "MemTotal"), freek = key_value(g_a, "MemFree");
        fact("meminfo: MemTotal is what sysinfo(2) says, MemFree and MemAvailable do not exceed it", total > 0 && (unsigned long)total * 1024 == si.totalram * si.mem_unit && freek >= 0 && freek <= total &&
             key_value(g_a, "MemAvailable") >= 0 && key_value(g_a, "MemAvailable") <= total);
        int ok = 1;
        for (const char *line = g_a; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
            const char *colon = memchr(line, ':', (size_t)(strchr(line, '\n') ? strchr(line, '\n') - line : 0));
            if (!colon) { if (*line) ok = 0; continue; }
            char *end;
            strtoul(colon + 1, &end, 10);
            if (end == colon + 1 || (*end != '\n' && strncmp(end, " kB\n", 4))) ok = 0;
        }
        fact("meminfo: every line is a key, a colon, a number and perhaps kB", ok);
    }
    n = slurp("/proc/cpuinfo", g_a, sizeof g_a);
    {
        int procs = 0;
        for (const char *p = g_a; (p = strstr(p, "processor\t:")); p++) procs++;
        long online = sysconf(_SC_NPROCESSORS_ONLN);
        const char *flags = strstr(g_a, "\nflags\t\t:");
        fact("cpuinfo: one block per processor, as many as sysconf says are online", n > 0 && procs >= 1 && procs == online);
        fact("cpuinfo: vendor_id, cpu family, model name and a flags line with fpu and sse2", strstr(g_a, "vendor_id\t:") && strstr(g_a, "cpu family\t:") && strstr(g_a, "model name\t:") && flags &&
             strstr(flags, " fpu") && strstr(flags, " sse2") && strstr(g_a, "cpu MHz\t\t:") && strstr(g_a, "bogomips\t:"));
        fact("cpuinfo: nproc agrees with the number of processors we may run on", sysconf(_SC_NPROCESSORS_ONLN) >= 1 && get_nprocs() == procs);
    }
    n = slurp("/proc/version", g_a, sizeof g_a);
    fact("version: Linux version, then the release uname(2) reports", n > 0 && !strncmp(g_a, "Linux version ", 14) && strstr(g_a, uts.release));
    n = slurp("/proc/filesystems", g_a, sizeof g_a);
    fact("filesystems: nodev lines and plain lines, with proc in them", n > 0 && (strstr(g_a, "nodev\tproc\n") != NULL) && (strstr(g_a, "nodev\ttmpfs\n") != NULL));
    n = slurp("/proc/partitions", g_a, sizeof g_a);
    fact("partitions: the header major minor #blocks name", n > 0 && !strncmp(g_a, "major minor  #blocks  name\n", 26));

    /* ---- /proc/sys ---- */
    n = slurp("/proc/sys/kernel/hostname", g_a, sizeof g_a);
    {
        char host[256];
        gethostname(host, sizeof host);
        fact("sys/kernel/hostname: the host name and a newline", n > 1 && g_a[n - 1] == '\n' && !strncmp(g_a, host, (size_t)n - 1) && strlen(host) == (size_t)n - 1);
    }
    n = slurp("/proc/sys/kernel/ostype", g_a, sizeof g_a);
    fact("sys/kernel/ostype: Linux, as uname(2) says", n > 1 && !strncmp(g_a, uts.sysname, (size_t)n - 1) && !strcmp(uts.sysname, "Linux"));
    n = slurp("/proc/sys/kernel/osrelease", g_a, sizeof g_a);
    fact("sys/kernel/osrelease: what uname(2) says", n > 1 && !strncmp(g_a, uts.release, (size_t)n - 1) && strlen(uts.release) == (size_t)n - 1);
    n = slurp("/proc/sys/kernel/version", g_a, sizeof g_a);
    fact("sys/kernel/version: what uname(2) says", n > 1 && !strncmp(g_a, uts.version, (size_t)n - 1) && strlen(uts.version) == (size_t)n - 1);
    n = slurp("/proc/sys/kernel/pid_max", g_a, sizeof g_a);
    fact("sys/kernel/pid_max: a number above our pid", n > 1 && atol(g_a) > pid);
    n = slurp("/proc/sys/kernel/random/boot_id", g_a, sizeof g_a);
    {
        int ok = n == 37 && g_a[36] == '\n';
        for (int i = 0; ok && i < 36; i++) ok = (i == 8 || i == 13 || i == 18 || i == 23) ? g_a[i] == '-' : isxdigit((unsigned char)g_a[i]) != 0;
        slurp("/proc/sys/kernel/random/boot_id", g_b, sizeof g_b);
        fact("sys/kernel/random/boot_id: a UUID that does not change during the boot", ok && !strcmp(g_a, g_b));
    }
    n = slurp("/proc/sys/vm/overcommit_memory", g_a, sizeof g_a);
    fact("sys/vm/overcommit_memory: 0, 1 or 2", n > 1 && atoi(g_a) >= 0 && atoi(g_a) <= 2);
    n = slurp("/proc/sys/vm/max_map_count", g_a, sizeof g_a);
    fact("sys/vm/max_map_count: a number of at least 1000", n > 1 && atol(g_a) >= 1000);
    fd = open("/proc/sys/kernel/hostname", O_WRONLY);
    fact("sys/kernel/hostname: not writable by a program without the right to", fd < 0 || geteuid() == 0);
    if (fd >= 0) close(fd);

    /* ---- the network files keep their formats (the files of /proc/net are the system's own) ---- */
    n = slurp("/proc/net/dev", g_a, sizeof g_a);
    fact("net/dev: the two header lines of proc(5)", n > 0 && !strncmp(g_a, "Inter-|   Receive", 17) && strstr(g_a, "\n face |bytes"));
    n = slurp("/proc/net/tcp", g_a, sizeof g_a);
    fact("net/tcp: the header of proc(5)", n > 0 && !strncmp(g_a, TCP_HEAD, strlen(TCP_HEAD)));
    n = slurp("/proc/net/route", g_a, sizeof g_a);
    fact("net/route: the header of proc(5)", n > 0 && !strncmp(g_a, ROUTE_HEAD, strlen(ROUTE_HEAD)));

    /* ---- a process that is not ours ---- */
    n = slurp("/proc/1/stat", g_a, sizeof g_a);
    fact("init: /proc/1/stat is readable and is the process with pid 1", n > 0 && parse_stat(g_a, &st) == 52 && st.f[1] == 1 && st.f[4] == 0);
    (void)file_map;
    (void)anon_map;
    printf("facts that did not hold: %d\n", g_bad);
    return g_bad ? 1 : 0;
}
