/*
 * A Linux-shaped /proc (docs/COMPAT.md, "A Linux-shaped /proc"). The programs of a distribution read /proc: ps, top, free, uptime, pidof, pgrep, vmstat, lscpu, Python's
 * os.cpu_count() and platform, every runtime that asks for its own mappings. They expect the formats of proc(5), and the native /proc (kernel/fs/procfs.c) has formats of its own,
 * which native programs and tests read and which do not change. So the same paths show a different view to a process of the Linux personality, and this file is that view.
 *
 * The mechanism: kernel/fs/procfs.c asks this file first, when it resolves a name in /proc or lists /proc, and only when the process doing it runs under the Linux personality
 * (linux_proc_view). A name this file knows becomes a vnode of its own kind here (a generated file, a directory, a symbolic link); a name it does not know (the native files
 * nobody ports, everything under /proc/net, /proc/watchdog and the other files a driver registers and a root user writes) falls through to the native lookup, byte for byte as before.
 * The decision is made when the name is resolved: the view belongs to the open file, the way an open file keeps its contents on any system, so a descriptor handed from a native
 * process to a Linux one still reads what it was opened as, and a file that one process writes and another reads (/proc/watchdog, /proc/net/tune) is the native vnode with its
 * native write operation whoever opens it, because nothing here claims its name.
 *
 * Written from proc(5) (every file and field below is in that manual page), the manual pages it points to (maps from mmap(2) and proc(5), limits from getrlimit(2), the status
 * fields from proc(5)), and from reading the real files on the development PC's Linux (WSL), which is only an observation of formats. No kernel, procps or C library source
 * was read. Values this kernel does not keep (page faults, I/O counts, cache sizes, the split of memory into active and inactive) are plausible and said so in docs/COMPAT.md;
 * everything it does know (processes, threads, mappings, descriptors, signal masks, credentials, times, memory totals, mounts) is the real value.
 */
#include "linux.h"
#include "kernel.h"
#include "calls.h"
#include "mm.h"
#include "irq.h"
#include "cpu.h"
#include "cpuinfo.h"
#include "blk.h"
#include "ktime.h"
#include "tty.h"
#include "random.h"
#include "initcall.h"

#define LNX_HZ        100                                   /* AT_CLKTCK: the unit of every time in /proc/<pid>/stat and /proc/stat */
#define LNX_RELEASE   "6.1.0-gensokyo"                      /* the same strings as uname(2) of this personality (kernel/linux/syscall.c) */
#define LNX_VERSION   "#1 TouhouOS Linux personality"
#define PROC_NOT_MINE LINUX_PROC_NOT_MINE

typedef size_t (*GlobalGen)(char *buf, size_t cap);

/* ------------------------------------------------------------------ writing into a fixed buffer */

typedef struct Buf { char *b; size_t cap, n; } Buf;         /* b holds cap + 1 bytes: vsnprintf wants room for its NUL */

static void bp(Buf *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bp(Buf *o, const char *fmt, ...) {
    if (o->n >= o->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(o->b + o->n, o->cap + 1 - o->n, fmt, ap);
    va_end(ap);
    if (r > 0) o->n += (size_t)r;
    if (o->n > o->cap) o->n = o->cap;
}

static void bput(Buf *o, const void *p, size_t len) {
    size_t room = o->cap - o->n;
    if (len > room) len = room;
    memcpy(o->b + o->n, p, len);
    o->n += len;
}

static u64 ticks_of_ms(u64 ms) { return ms / (1000 / LNX_HZ); }

bool linux_proc_view(void) { return current && current->proc && current->proc->abi == ABI_LINUX; }

/* ------------------------------------------------------------------ what the scheduler knows */

typedef struct SchedStats { u64 idle_ms; int running, tasks; } SchedStats;

static void sched_cb(Task *t, void *ctx) {
    SchedStats *s = ctx;
    if (!t->proc && !strcmp(t->name, "idle")) { s->idle_ms = t->cpu_ticks; return; }       /* the idle task: every tick nothing else used is charged to it */
    s->tasks++;
    if (t->state == TASK_READY || t->state == TASK_RUNNING) s->running++;
}

static void sched_stats(SchedStats *s) {
    memset(s, 0, sizeof *s);
    task_for_each(sched_cb, s);
}

/* ---- the load average: the number of runnable tasks, averaged over one, five and fifteen minutes. A kernel thread that wakes every five seconds does the sums with the
 * exponential decay e^(-5/60), e^(-5/300) and e^(-5/900) in 11-bit fixed point; it is started by the first read of a load average, so a machine that never asks pays nothing. */
#define FSHIFT   11
#define FIXED_1  (1U << FSHIFT)
static u32 g_load[3];
static bool g_load_started;

static void load_cb(Task *t, void *ctx) {
    if (!t->proc && !strcmp(t->name, "idle")) return;
    if (t != current && (t->state == TASK_READY || t->state == TASK_RUNNING)) (*(int *)ctx)++;
}

static void load_thread(void *arg) {
    static const u32 decay[3] = { 1884, 2014, 2037 };      /* round(2048 * e^(-5/60)), round(2048 * e^(-5/300)), round(2048 * e^(-5/900)) */
    (void)arg;
    for (;;) {
        task_sleep_ms(5000);
        int n = 0;
        task_for_each(load_cb, &n);
        for (int i = 0; i < 3; i++) g_load[i] = (u32)(((u64)g_load[i] * decay[i] + (u64)n * FIXED_1 * (FIXED_1 - decay[i])) >> FSHIFT);
    }
}

static void load_start(void) {
    u64 f = irq_save();
    bool first = !g_load_started;
    g_load_started = true;
    irq_restore(f);
    if (first) task_create_detached("loadavg", load_thread, NULL);
}

/* ------------------------------------------------------------------ the global files */

static size_t gen_uptime(char *b, size_t cap) {
    SchedStats s;
    sched_stats(&s);
    u64 up = uptime_ms();
    return (size_t)snprintf(b, cap, "%lu.%02lu %lu.%02lu\n", (unsigned long)(up / 1000), (unsigned long)(up % 1000 / 10),
                            (unsigned long)(s.idle_ms / 1000), (unsigned long)(s.idle_ms % 1000 / 10));
}

static u32 g_last_pid;                                      /* the highest process id seen so far: pids only grow here, and a reaped process must not make the count fall */

static void maxpid_cb(Process *p, void *ctx) { if (p->pid > *(int *)ctx) *(int *)ctx = p->pid; }

static int last_pid(void) {
    int m = 0;
    process_for_each(maxpid_cb, &m);
    if ((u32)m > g_last_pid) g_last_pid = (u32)m;
    return (int)g_last_pid;
}

static size_t gen_loadavg(char *b, size_t cap) {
    load_start();
    SchedStats s;
    sched_stats(&s);
    u32 v[3];
    for (int i = 0; i < 3; i++) v[i] = g_load[i];
    return (size_t)snprintf(b, cap, "%u.%02u %u.%02u %u.%02u %d/%d %d\n", v[0] >> FSHIFT, ((v[0] & (FIXED_1 - 1)) * 100) >> FSHIFT, v[1] >> FSHIFT,
                            ((v[1] & (FIXED_1 - 1)) * 100) >> FSHIFT, v[2] >> FSHIFT, ((v[2] & (FIXED_1 - 1)) * 100) >> FSHIFT, s.running ? s.running : 1, s.tasks,
                            last_pid());
}

/* The memory totals. Linux's page cache, active and inactive lists and swap do not exist here: Buffers is 0, Cached is what tmpfs holds (it is Shmem too, as on Linux),
 * everything else a program has allocated is AnonPages, and MemAvailable is MemFree (nothing but the free list can be given back). The other lines are plausible. */
static size_t gen_meminfo(char *b, size_t cap) {
    u64 total = pmm_total_pages() * 4, free = pmm_free_pages_count() * 4, tmp_pages;
    tmpfs_stats(&tmp_pages);
    u64 shmem = tmp_pages * 4, slab = heap_bytes_in_use() >> 10, kstack = (u64)task_count() * (KSTACK_SIZE >> 10);
    u64 used = total > free ? total - free : 0;
    u64 anon = used > slab + shmem + kstack ? used - slab - shmem - kstack : 0;
    u64 ptes = anon / 256 + 4;
    Buf o = { b, cap, 0 };
    bp(&o, "MemTotal:       %8lu kB\nMemFree:        %8lu kB\nMemAvailable:   %8lu kB\nBuffers:        %8lu kB\nCached:         %8lu kB\nSwapCached:     %8lu kB\n",
       (unsigned long)total, (unsigned long)free, (unsigned long)free, 0UL, (unsigned long)shmem, 0UL);
    bp(&o, "Active:         %8lu kB\nInactive:       %8lu kB\nActive(anon):   %8lu kB\nInactive(anon): %8lu kB\nActive(file):   %8lu kB\nInactive(file): %8lu kB\nUnevictable:    %8lu kB\nMlocked:        %8lu kB\n",
       (unsigned long)(anon + shmem), 0UL, (unsigned long)anon, 0UL, (unsigned long)shmem, 0UL, 0UL, 0UL);
    bp(&o, "SwapTotal:      %8lu kB\nSwapFree:       %8lu kB\nZswap:          %8lu kB\nZswapped:       %8lu kB\nDirty:          %8lu kB\nWriteback:      %8lu kB\n", 0UL, 0UL, 0UL, 0UL, 0UL, 0UL);
    bp(&o, "AnonPages:      %8lu kB\nMapped:         %8lu kB\nShmem:          %8lu kB\nKReclaimable:   %8lu kB\nSlab:           %8lu kB\nSReclaimable:   %8lu kB\nSUnreclaim:     %8lu kB\n",
       (unsigned long)anon, 0UL, (unsigned long)shmem, 0UL, (unsigned long)slab, 0UL, (unsigned long)slab);
    bp(&o, "KernelStack:    %8lu kB\nPageTables:     %8lu kB\nSecPageTables:  %8lu kB\nNFS_Unstable:   %8lu kB\nBounce:         %8lu kB\nWritebackTmp:   %8lu kB\n",
       (unsigned long)kstack, (unsigned long)ptes, 0UL, 0UL, 0UL, 0UL);
    bp(&o, "CommitLimit:    %8lu kB\nCommitted_AS:   %8lu kB\nVmallocTotal:   34359738367 kB\nVmallocUsed:   %8lu kB\nVmallocChunk:   %8lu kB\nPercpu:         %8lu kB\n",
       (unsigned long)(total / 2), (unsigned long)used, 0UL, 0UL, 0UL);
    bp(&o, "HardwareCorrupted: %5lu kB\nAnonHugePages:  %8lu kB\nShmemHugePages: %8lu kB\nShmemPmdMapped: %8lu kB\nFileHugePages:  %8lu kB\nFilePmdMapped:  %8lu kB\n",
       0UL, 0UL, 0UL, 0UL, 0UL, 0UL);
    bp(&o, "HugePages_Total:   %5lu\nHugePages_Free:    %5lu\nHugePages_Rsvd:    %5lu\nHugePages_Surp:    %5lu\nHugepagesize:   %8lu kB\nHugetlb:        %8lu kB\n",
       0UL, 0UL, 0UL, 0UL, 2048UL, 0UL);
    bp(&o, "DirectMap4k:    %8lu kB\nDirectMap2M:    %8lu kB\nDirectMap1G:    %8lu kB\n", (unsigned long)total, 0UL, 0UL);
    return o.n;
}

/* /proc/version: the same release and version strings as uname(2). */
static size_t gen_version(char *b, size_t cap) {
    return (size_t)snprintf(b, cap, "Linux version " LNX_RELEASE " (gensokyo@touhouos) (clang) " LNX_VERSION "\n");
}

/* ---- /proc/cpuinfo: one processor (the kernel runs one). The flags come from CPUID, named as Linux names them; a bit this table does not know is left out. */
typedef struct { u8 bit; const char *name; } FlagName;
static const FlagName FLAGS_1_EDX[] = {
    { 0, "fpu" }, { 1, "vme" }, { 2, "de" }, { 3, "pse" }, { 4, "tsc" }, { 5, "msr" }, { 6, "pae" }, { 7, "mce" }, { 8, "cx8" }, { 9, "apic" }, { 11, "sep" }, { 12, "mtrr" },
    { 13, "pge" }, { 14, "mca" }, { 15, "cmov" }, { 16, "pat" }, { 17, "pse36" }, { 19, "clflush" }, { 23, "mmx" }, { 24, "fxsr" }, { 25, "sse" }, { 26, "sse2" },
    { 27, "ss" }, { 28, "ht" },
};
static const FlagName FLAGS_1_ECX[] = {
    { 0, "pni" }, { 1, "pclmulqdq" }, { 3, "monitor" }, { 5, "vmx" }, { 6, "smx" }, { 7, "est" }, { 8, "tm2" }, { 9, "ssse3" }, { 12, "fma" }, { 13, "cx16" },
    { 15, "pdcm" }, { 17, "pcid" }, { 19, "sse4_1" }, { 20, "sse4_2" }, { 21, "x2apic" }, { 22, "movbe" }, { 23, "popcnt" }, { 25, "aes" }, { 26, "xsave" },
    { 27, "osxsave" }, { 28, "avx" }, { 29, "f16c" }, { 30, "rdrand" }, { 31, "hypervisor" },
};
static const FlagName FLAGS_7_EBX[] = {
    { 0, "fsgsbase" }, { 3, "bmi1" }, { 4, "hle" }, { 5, "avx2" }, { 7, "smep" }, { 8, "bmi2" }, { 9, "erms" }, { 10, "invpcid" }, { 11, "rtm" }, { 16, "avx512f" },
    { 18, "rdseed" }, { 19, "adx" }, { 20, "smap" }, { 23, "clflushopt" }, { 29, "sha_ni" },
};
static const FlagName FLAGS_80000001_EDX[] = { { 11, "syscall" }, { 20, "nx" }, { 26, "pdpe1gb" }, { 27, "rdtscp" }, { 29, "lm" } };
static const FlagName FLAGS_80000001_ECX[] = { { 0, "lahf_lm" }, { 5, "abm" }, { 8, "3dnowprefetch" } };

static void put_flags(Buf *o, u32 word, const FlagName *t, size_t n) {
    for (size_t i = 0; i < n; i++) if (word & (1U << t[i].bit)) bp(o, " %s", t[i].name);
}

static u64 cache_kb(void) {                                  /* the biggest cache CPUID leaf 4 describes, in kB (0 when the CPU has no such leaf) */
    u64 best = 0;
    if (g_cpu.max_leaf < 4) return 0;
    for (u32 sub = 0; sub < 8; sub++) {
        u32 a, b, c, d;
        cpuid(4, sub, &a, &b, &c, &d);
        if (!(a & 31)) break;
        u64 size = (u64)(((b >> 22) & 0x3ff) + 1) * (((b >> 12) & 0x3ff) + 1) * ((b & 0xfff) + 1) * ((u64)c + 1);
        if (size / 1024 > best) best = size / 1024;
    }
    return best;
}

static size_t gen_cpuinfo(char *b, size_t cap) {
    Buf o = { b, cap, 0 };
    u32 a, bb, c, d, maxext = 0, phys = 36, virt = 48;
    cpuid(0x80000000u, 0, &maxext, &bb, &c, &d);
    if (maxext >= 0x80000008u) {
        cpuid(0x80000008u, 0, &a, &bb, &c, &d);
        phys = a & 0xff;
        virt = (a >> 8) & 0xff;
    }
    u64 mhz = g_cpu.brand_mhz ? g_cpu.brand_mhz : timer_tsc_mhz();
    bp(&o, "processor\t: 0\nvendor_id\t: %s\ncpu family\t: %u\nmodel\t\t: %u\nmodel name\t: %s\nstepping\t: %u\nmicrocode\t: 0x0\ncpu MHz\t\t: %lu.000\n"
           "cache size\t: %lu KB\nphysical id\t: 0\nsiblings\t: 1\ncore id\t\t: 0\ncpu cores\t: 1\napicid\t\t: 0\ninitial apicid\t: 0\nfpu\t\t: yes\nfpu_exception\t: yes\n"
           "cpuid level\t: %u\nwp\t\t: yes\nflags\t\t:",
       g_cpu.vendor, g_cpu.family, g_cpu.model, g_cpu.brand[0] ? g_cpu.brand : "unknown", g_cpu.stepping, (unsigned long)mhz, (unsigned long)cache_kb(), g_cpu.max_leaf);
    put_flags(&o, g_cpu.feat_edx, FLAGS_1_EDX, ARRAY_SIZE(FLAGS_1_EDX));
    if (maxext >= 0x80000001u) {
        cpuid(0x80000001u, 0, &a, &bb, &c, &d);
        put_flags(&o, d, FLAGS_80000001_EDX, ARRAY_SIZE(FLAGS_80000001_EDX));
    }
    put_flags(&o, g_cpu.feat_ecx, FLAGS_1_ECX, ARRAY_SIZE(FLAGS_1_ECX));
    if (maxext >= 0x80000001u) put_flags(&o, c, FLAGS_80000001_ECX, ARRAY_SIZE(FLAGS_80000001_ECX));
    put_flags(&o, g_cpu.feat7_ebx, FLAGS_7_EBX, ARRAY_SIZE(FLAGS_7_EBX));
    bp(&o, "\nbugs\t\t:\nbogomips\t: %lu.00\nclflush size\t: 64\ncache_alignment\t: 64\naddress sizes\t: %u bits physical, %u bits virtual\npower management:\n\n",
       (unsigned long)(mhz * 2), phys, virt);
    return o.n;
}

/* ---- /proc/stat. The CPU time is the ticks the scheduler charged: idle to the idle task, everything else to the tasks that ran. The split of the busy time into user and system
 * comes from the live processes (a reaped process takes its share with it, so the user counter is kept from falling: it is a high-water mark), `nice` and the other columns
 * are 0 because nothing here is accounted that way. */
typedef struct UserSum { u64 user_ms; int running, blocked; } UserSum;

static void usersum_cb(Process *p, void *ctx) {
    UserSum *s = ctx;
    u64 user, total;
    process_cpu_ticks(p, &user, &total);
    s->user_ms += user;
    for (Task *t = p->threads; t; t = t->thread_next) {
        if (t->state == TASK_READY || t->state == TASK_RUNNING) s->running++;
        else if (t->state == TASK_BLOCKED) s->blocked++;
    }
}

static size_t gen_stat(char *b, size_t cap) {
    Buf o = { b, cap, 0 };
    SchedStats ss;
    sched_stats(&ss);
    UserSum us = { 0, 0, 0 };
    process_for_each(usersum_cb, &us);
    u64 up = uptime_ms(), busy = up > ss.idle_ms ? up - ss.idle_ms : 0;
    static u64 g_user_hwm;
    if (us.user_ms > g_user_hwm) g_user_hwm = us.user_ms;
    u64 user = g_user_hwm > busy ? busy : g_user_hwm, sys = busy - user, idle = ss.idle_ms;
    bp(&o, "cpu  %lu 0 %lu %lu 0 0 0 0 0 0\ncpu0 %lu 0 %lu %lu 0 0 0 0 0 0\n", (unsigned long)ticks_of_ms(user), (unsigned long)ticks_of_ms(sys), (unsigned long)ticks_of_ms(idle),
       (unsigned long)ticks_of_ms(user), (unsigned long)ticks_of_ms(sys), (unsigned long)ticks_of_ms(idle));
    u64 total = 0;
    for (int i = 0; i < 16; i++) total += irq_count(i);
    bp(&o, "intr %lu", (unsigned long)total);
    for (int i = 0; i < 16; i++) bp(&o, " %lu", (unsigned long)irq_count(i));
    u64 now = time_now_sec();
    bp(&o, "\nctxt %lu\nbtime %lu\nprocesses %d\nprocs_running %d\nprocs_blocked %d\nsoftirq 0 0 0 0 0 0 0 0 0 0 0\n",
       (unsigned long)(up / SCHED_SLICE + ss.tasks), (unsigned long)(now > up / 1000 ? now - up / 1000 : 0), last_pid(), us.running ? us.running : 1, us.blocked);
    return o.n;
}

/* ---- /proc/vmstat: the counters vmstat(8) reads. The page counts follow the totals above; the event counters (faults, paging) are not kept and read 0. */
static size_t gen_vmstat(char *b, size_t cap) {
    u64 total = pmm_total_pages(), freep = pmm_free_pages_count(), tmp;
    tmpfs_stats(&tmp);
    Buf o = { b, cap, 0 };
    static const char *const zero[] = { "nr_zone_inactive_anon", "nr_zone_active_anon", "nr_zone_inactive_file", "nr_zone_active_file", "nr_zone_unevictable", "nr_zone_write_pending",
        "nr_mlock", "nr_bounce", "nr_zspages", "nr_free_cma", "nr_dirty", "nr_writeback", "nr_writeback_temp", "nr_dirtied", "nr_written", "pgpgin", "pgpgout", "pswpin",
        "pswpout", "pgalloc_normal", "pgfree", "pgfault", "pgmajfault", "pgrefill", "pgsteal_kswapd", "pgsteal_direct", "pgscan_kswapd", "pgscan_direct", "oom_kill" };
    bp(&o, "nr_free_pages %lu\nnr_shmem %lu\nnr_mapped 0\nnr_file_pages %lu\nnr_anon_pages %lu\nnr_slab_unreclaimable %lu\nnr_kernel_stack %lu\n", (unsigned long)freep,
       (unsigned long)tmp, (unsigned long)tmp, (unsigned long)(total > freep + tmp ? total - freep - tmp : 0), (unsigned long)(heap_bytes_in_use() >> 12),
       (unsigned long)task_count() * (KSTACK_SIZE >> 10));
    for (unsigned i = 0; i < ARRAY_SIZE(zero); i++) bp(&o, "%s 0\n", zero[i]);
    return o.n;
}

/* ---- mounts. The machine's mounts as the process sees them: a Linux program in the Linux root sees the tree mounted on /linux as "/", the mounts under it, and the machine's own
 * five directories (/proc /dev /tmp /sys /run, the real /tmp being /BambooForest); a Linux program with no root sees every mount where it is. The type names are Linux's. */
static const char *linux_fstype(const char *native) {
    if (!strcmp(native, "procfs")) return "proc";
    if (!strcmp(native, "devfs")) return "devtmpfs";
    if (!strcmp(native, "fat32") || !strcmp(native, "fat16")) return "vfat";
    return native;
}

static bool shown_mount_path(const char *target, bool in_root, char *out, size_t n) {
    if (!in_root) { strlcpy(out, target, n); return true; }
    if (!strcmp(target, "/linux")) { strlcpy(out, "/", n); return true; }
    if (!strncmp(target, "/linux/", 7)) { strlcpy(out, target + 6, n); return true; }
    if (!strcmp(target, "/BambooForest")) { strlcpy(out, "/tmp", n); return true; }
    if (!strcmp(target, "/proc") || !strcmp(target, "/dev") || !strcmp(target, "/sys") || !strcmp(target, "/run")) { strlcpy(out, target, n); return true; }
    return false;
}

static size_t gen_mounts_view(char *b, size_t cap, bool info) {
    Buf o = { b, cap, 0 };
    bool in_root = current->proc->root != NULL;
    char t[64], y[16], s[64], shown[64];
    int rootid = 0;
    for (int i = 0; vfs_mount_list(i, t, sizeof t, y, sizeof y, s, sizeof s); i++)
        if (shown_mount_path(t, in_root, shown, sizeof shown) && !strcmp(shown, "/")) rootid = i + 1;
    for (int i = 0; vfs_mount_list(i, t, sizeof t, y, sizeof y, s, sizeof s) && o.n + 200 < cap; i++) {
        if (!shown_mount_path(t, in_root, shown, sizeof shown)) continue;
        const char *opts = (vfs_mount_flags(i) & MNT_RDONLY) ? "ro,relatime" : "rw,relatime";
        if (info) bp(&o, "%d %d 0:%d / %s %s - %s %s %s\n", i + 1, i + 1 == rootid ? 0 : rootid ? rootid : 1, i + 1, shown, opts, linux_fstype(y), s[0] ? s : "none", (vfs_mount_flags(i) & MNT_RDONLY) ? "ro" : "rw");
        else bp(&o, "%s %s %s %s 0 0\n", s[0] ? s : "none", shown, linux_fstype(y), opts);
    }
    return o.n;
}

static size_t gen_mounts(char *b, size_t cap) { return gen_mounts_view(b, cap, false); }

static size_t gen_filesystems(char *b, size_t cap) {
    return (size_t)snprintf(b, cap, "nodev\tproc\nnodev\ttmpfs\nnodev\tdevtmpfs\nnodev\tramfs\n\tvfat\n\tntfs\n");
}

/* ---- /proc/partitions: major, minor, size in 1 KiB blocks, name. The majors are the ones Linux gives the driver (sd 8, nvme 259, virtio 254), the minors count 16 to a disk. */
static size_t gen_partitions(char *b, size_t cap) {
    Buf o = { b, cap, 0 };
    bp(&o, "major minor  #blocks  name\n\n");
    for (BlockDev *d = blk_first(); d && o.n + 100 < cap; d = blk_next(d)) {
        const BlockDev *disk = d->parent ? d->parent : d;
        int major = !strcmp(disk->driver ? disk->driver : "", "nvme") ? 259 : !strcmp(disk->driver ? disk->driver : "", "virtio-blk") ? 254 : 8;
        bp(&o, "%4d %7d %10lu %s\n", major, disk->dev_index * 16 + (d->parent ? d->part_index : 0), (unsigned long)(d->sectors * d->sector_size / 1024), d->name);
    }
    return o.n;
}

/* ---- /proc/tty/drivers: what ps(1) reads to turn the tty number of /proc/<pid>/stat into a name (pts/3, tty1). */
static size_t gen_tty_drivers(char *b, size_t cap) {
    return (size_t)snprintf(b, cap, "/dev/tty             /dev/tty        5       0 system:/dev/tty\n/dev/console         /dev/console    5       1 system:console\n"
                                    "/dev/ptmx            /dev/ptmx       5       2 system\n/dev/vc/0            /dev/vc/0       4       0 system:vtmaster\n"
                                    "serial               /dev/ttyS       4 64-95 serial\npty_slave            /dev/pts      136 0-1048575 pty:slave\n"
                                    "pty_master           /dev/ptm      128 0-1048575 pty:master\nunknown              /dev/tty        4 1-63 console\n");
}

/* ---- /proc/sys: the few tunables programs read (read-only here). */
static size_t gen_sys_hostname(char *b, size_t cap) { return (size_t)snprintf(b, cap, "gensokyo\n"); }
static size_t gen_sys_domainname(char *b, size_t cap) { return (size_t)snprintf(b, cap, "(none)\n"); }
static size_t gen_sys_ostype(char *b, size_t cap) { return (size_t)snprintf(b, cap, "Linux\n"); }
static size_t gen_sys_osrelease(char *b, size_t cap) { return (size_t)snprintf(b, cap, LNX_RELEASE "\n"); }
static size_t gen_sys_version(char *b, size_t cap) { return (size_t)snprintf(b, cap, LNX_VERSION "\n"); }
static size_t gen_sys_pid_max(char *b, size_t cap) { return (size_t)snprintf(b, cap, "4194304\n"); }
static size_t gen_sys_threads_max(char *b, size_t cap) { return (size_t)snprintf(b, cap, "%d\n", MAX_PROCS * 8); }
static size_t gen_sys_cap_last_cap(char *b, size_t cap) { return (size_t)snprintf(b, cap, "40\n"); }
static size_t gen_sys_overcommit(char *b, size_t cap) { return (size_t)snprintf(b, cap, "0\n"); }
static size_t gen_sys_overcommit_ratio(char *b, size_t cap) { return (size_t)snprintf(b, cap, "50\n"); }
static size_t gen_sys_max_map_count(char *b, size_t cap) { return (size_t)snprintf(b, cap, "65530\n"); }
static size_t gen_sys_swappiness(char *b, size_t cap) { return (size_t)snprintf(b, cap, "60\n"); }
static size_t gen_sys_file_max(char *b, size_t cap) { return (size_t)snprintf(b, cap, "1048576\n"); }
static size_t gen_sys_nr_open(char *b, size_t cap) { return (size_t)snprintf(b, cap, "1048576\n"); }
static size_t gen_sys_file_nr(char *b, size_t cap) { return (size_t)snprintf(b, cap, "%lu\t0\t1048576\n", (unsigned long)vfs_file_count()); }

static void uuid_text(char *out, size_t n, const u8 r[16]) {
    snprintf(out, n, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n", r[0], r[1], r[2], r[3], r[4], r[5], (r[6] & 0x0f) | 0x40, r[7], (r[8] & 0x3f) | 0x80, r[9],
             r[10], r[11], r[12], r[13], r[14], r[15]);                                  /* version 4, variant 1: 8-4-4-4-12 hexadecimal digits */
}

static size_t gen_sys_boot_id(char *b, size_t cap) {         /* one random version-4 UUID for the whole boot */
    static u8 id[16];
    static bool have;
    if (!have) { random_fill(id, sizeof id); have = true; }
    char t[48];
    uuid_text(t, sizeof t, id);
    return (size_t)snprintf(b, cap, "%s", t);
}

static size_t gen_sys_uuid(char *b, size_t cap) {            /* a new one at every read */
    u8 id[16];
    random_fill(id, sizeof id);
    char t[48];
    uuid_text(t, sizeof t, id);
    return (size_t)snprintf(b, cap, "%s", t);
}

static size_t gen_sys_entropy(char *b, size_t cap) { return (size_t)snprintf(b, cap, "256\n"); }

/* The table of the global files. A name with slashes is a file in a subdirectory (sys/kernel/hostname); the directories exist because a file is in them. */
typedef struct GlobalEntry { const char *name; GlobalGen gen; size_t cap; } GlobalEntry;
static const GlobalEntry g_global[] = {
    { "uptime", gen_uptime, 128 }, { "loadavg", gen_loadavg, 128 }, { "meminfo", gen_meminfo, 4096 }, { "cpuinfo", gen_cpuinfo, 4096 }, { "version", gen_version, 256 },
    { "stat", gen_stat, 2048 }, { "vmstat", gen_vmstat, 4096 }, { "mounts", gen_mounts, 8192 }, { "filesystems", gen_filesystems, 256 }, { "partitions", gen_partitions, 4096 },
    { "tty/drivers", gen_tty_drivers, 1024 },
    { "sys/kernel/hostname", gen_sys_hostname, 64 }, { "sys/kernel/domainname", gen_sys_domainname, 64 }, { "sys/kernel/ostype", gen_sys_ostype, 64 },
    { "sys/kernel/osrelease", gen_sys_osrelease, 64 }, { "sys/kernel/version", gen_sys_version, 128 }, { "sys/kernel/pid_max", gen_sys_pid_max, 32 },
    { "sys/kernel/threads-max", gen_sys_threads_max, 32 }, { "sys/kernel/cap_last_cap", gen_sys_cap_last_cap, 32 },
    { "sys/kernel/random/boot_id", gen_sys_boot_id, 64 }, { "sys/kernel/random/uuid", gen_sys_uuid, 64 }, { "sys/kernel/random/entropy_avail", gen_sys_entropy, 32 },
    { "sys/vm/overcommit_memory", gen_sys_overcommit, 32 }, { "sys/vm/overcommit_ratio", gen_sys_overcommit_ratio, 32 }, { "sys/vm/max_map_count", gen_sys_max_map_count, 32 },
    { "sys/vm/swappiness", gen_sys_swappiness, 32 }, { "sys/fs/file-max", gen_sys_file_max, 32 }, { "sys/fs/nr_open", gen_sys_nr_open, 32 }, { "sys/fs/file-nr", gen_sys_file_nr, 64 },
};
#define NGLOBAL ((int)ARRAY_SIZE(g_global))

/* ------------------------------------------------------------------ one process, as /proc/<pid>/... shows it */

/* A process (or one of its threads) with the scratch memory the generators need. Everything runs with interrupts off, so that the process cannot exit or be reaped under us
 * (one processor: nothing else runs) and its mappings do not change. */
typedef struct Seg { u64 start, end; u32 prot, flags; const VmaFile *file; u64 off; } Seg;
#define MAXSEG 1024                                                         /* mappings listed at most (a Python with its extension modules has a few hundred) */
typedef struct PidCtx { Process *p; Task *t; Buf *o; Seg *segs; int nseg; } PidCtx;
typedef void (*PidGen)(PidCtx *c);

/* ---- what a process looks like from outside ---- */

static char state_letter(const Process *p, const Task *t) {
    if (p->state == PROC_ZOMBIE) return 'Z';
    if (p->stopped) return 'T';
    if (t) return t->state == TASK_READY || t->state == TASK_RUNNING ? 'R' : 'S';
    for (const Task *x = p->threads; x; x = x->thread_next)
        if (x->state == TASK_READY || x->state == TASK_RUNNING) return 'R';
    return p->threads ? 'S' : 'R';
}

static const char *state_word(char c) {
    return c == 'R' ? "running" : c == 'S' ? "sleeping" : c == 'T' ? "stopped" : c == 'Z' ? "zombie" : "dead";
}

static const Tty *ctty_of(const Process *p) { return p->ctty ? p->ctty : tty_console(); }

/* The tty number of proc(5): major 136 and the index for a pseudo-terminal slave, 4:1 (tty1) for the console. */
static int tty_number(const Process *p) {
    const Tty *t = ctty_of(p);
    if (!t) return 0;
    if (!strncmp(t->name, "pts/", 4)) {
        int n = 0;
        for (const char *s = t->name + 4; *s >= '0' && *s <= '9'; s++) n = n * 10 + (*s - '0');
        return (136 << 8) | (n & 0xff) | ((n & ~0xff) << 12);
    }
    return (4 << 8) | 1;
}

static int tty_foreground(const Process *p) {
    const Tty *t = ctty_of(p);
    return t && t->fg_pgid > 0 ? t->fg_pgid : -1;
}

/* The mappings of the process, sorted by address, with the pieces of one mapping that were split (or made one after the other, as the heap grows) joined again, and the stack
 * cut to the part that is in use (the kernel reserves 8 MiB and fills it from the top as the program needs it). */
static bool segs_join(const Seg *a, const Seg *b) {
    if (a->end != b->start || a->prot != b->prot || a->file != b->file) return false;
    if ((a->flags ^ b->flags) & (VMA_STACK | VMA_HEAP | VMA_IO | VMA_FILE | VMA_WC)) return false;
    return !a->file || a->off + (a->end - a->start) == b->off;
}

static int collect_segs(const Process *p, Seg *out, int max) {
    const AddrSpace *as = &p->as;
    int n = 0;
    for (const Vma *v = as->vmas; v && n < max; v = v->next) {
        Seg s = { v->start, v->end, v->prot, v->flags, v->file, v->file_off };
        if (v->flags & VMA_STACK) {
            u64 lo = v->end;
            while (lo > v->start && vmm_translate(as->pml4, lo - PAGE_SIZE, NULL)) lo -= PAGE_SIZE;
            if (lo == v->end) lo = v->end - PAGE_SIZE;
            s.start = lo;
        }
        int i = n++;
        while (i > 0 && out[i - 1].start > s.start) { out[i] = out[i - 1]; i--; }
        out[i] = s;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (m && segs_join(&out[m - 1], &out[i])) out[m - 1].end = out[i].end;
        else out[m++] = out[i];
    }
    return m;
}

static u64 seg_rss(const AddrSpace *as, const Seg *s) {
    u64 n = 0;
    if (s->flags & VMA_IO) return 0;                             /* device and shared-buffer memory is not the process's own */
    for (u64 va = s->start; va < s->end; va += PAGE_SIZE) if (vmm_translate(as->pml4, va, NULL)) n++;
    return n;
}

typedef struct VmInfo { u64 size, rss, rss_file, data, stack, exe, lib; } VmInfo;       /* bytes, except rss and rss_file (pages) */

static void vm_info(const PidCtx *c, VmInfo *vi) {
    memset(vi, 0, sizeof *vi);
    for (int i = 0; i < c->nseg; i++) {
        const Seg *s = &c->segs[i];
        u64 len = s->end - s->start, rss = seg_rss(&c->p->as, s);
        vi->size += len;
        vi->rss += rss;
        if (s->file) vi->rss_file += rss;
        if (s->flags & VMA_STACK) vi->stack += len;
        else if ((s->prot & PROT_WRITE) && !(s->flags & VMA_IO) && !s->file) vi->data += len;
        if (s->file && (s->prot & PROT_EXEC)) {
            if (!strcmp(s->file->path, c->p->exe)) vi->exe += len;
            else vi->lib += len;
        }
    }
}

/* Reads n bytes of the process's memory at va through its page tables (what a reader of /proc/<pid>/cmdline gets); stops at the first page that is not there. */
static size_t read_user(const Process *p, u64 va, void *dst, size_t n) {
    size_t done = 0;
    if (!p->as.pml4) return 0;
    while (done < n) {
        paddr_t pa = vmm_translate(p->as.pml4, va + done, NULL);
        if (!pa) break;
        size_t chunk = MIN(n - done, PAGE_SIZE - ((va + done) & (PAGE_SIZE - 1)));
        memcpy((u8 *)dst + done, phys_to_virt(pa), chunk);
        done += chunk;
    }
    return done;
}

static void sig_sets(const Process *p, u64 *ignored, u64 *caught) {              /* in the kernel's bit order (siginfo.h) */
    *ignored = *caught = 0;
    for (int s = 1; s < KNSIG; s++) {
        if (p->sigact[s].handler == SIG_IGN) *ignored |= sigbit(s);
        else if (p->sigact[s].handler != SIG_DFL) *caught |= sigbit(s);
    }
}

static void cpu_ms(const PidCtx *c, u64 *user, u64 *total) {
    if (c->t) { *user = c->t->utime_ticks; *total = c->t->cpu_ticks; }
    else process_cpu_ticks(c->p, user, total);
}

/* /proc/<pid>/stat: the fields of proc(5) in order (52 of them). Memory the process does not have (a zombie), faults and swap (not kept) and the instruction pointers (Linux
 * hides those from other readers) are 0. startcode..end_data are the bounds of the executable and writable file mappings of the program. */
static void pid_stat(PidCtx *c) {
    Process *p = c->p;
    Buf *o = c->o;
    bool zombie = p->state == PROC_ZOMBIE;
    int tid = c->t ? c->t->utid : p->pid;
    u64 user, total, cuser = p->child_utime, call = p->child_ticks;
    cpu_ms(c, &user, &total);
    VmInfo vi;
    vm_info(c, &vi);
    u64 start_code = 0, end_code = 0, start_data = 0, end_data = 0;
    for (int i = 0; i < c->nseg; i++) {
        const Seg *s = &c->segs[i];
        if (!s->file) continue;
        if ((s->prot & PROT_EXEC) && !start_code) { start_code = s->start; end_code = s->end; }
        else if ((s->prot & PROT_WRITE) && !start_data) { start_data = s->start; end_data = s->end; }
    }
    const Task *lt = c->t ? c->t : p->task;
    u64 blocked = lt ? lt->sig_blocked : 0, pending = p->sig_pending | (lt ? lt->sig_pending : 0), ign, cgt;
    sig_sets(p, &ign, &cgt);
    bp(o, "%d (%.15s) %c %d %d %d %d %d %u %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld %ld %ld %llu %lu %ld %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %d %d %u %u %llu %lu %ld "
           "%lu %lu %lu %lu %lu %lu %lu %d\n",
       tid, p->name, state_letter(p, c->t), p->ppid, p->pgid, p->sid, tty_number(p), tty_foreground(p), zombie ? 4194560u : 4194304u,    /* flags: PF_RANDOMIZE (| PF_EXITING) */
       0UL, 0UL, 0UL, 0UL,                                                                                                                  /* page faults: not kept */
       (unsigned long)ticks_of_ms(user), (unsigned long)ticks_of_ms(total - user), (long)ticks_of_ms(cuser), (long)ticks_of_ms(call - cuser),
       (long)(20 + p->nice), (long)p->nice, (long)(zombie ? 1 : p->nthreads), 0L, (unsigned long long)ticks_of_ms(p->start_ms),
       (unsigned long)vi.size, (long)vi.rss, ~0UL,                                                                                          /* vsize, rss, rsslim */
       (unsigned long)start_code, (unsigned long)end_code, (unsigned long)(zombie ? 0 : p->start_stack), 0UL, 0UL,                          /* startstack, kstkesp, kstkeip */
       (unsigned long)lnx_sigset_to_linux(pending), (unsigned long)lnx_sigset_to_linux(blocked), (unsigned long)lnx_sigset_to_linux(ign), (unsigned long)lnx_sigset_to_linux(cgt),
       0UL, 0UL, 0UL, c->t && c->t->utid != p->pid ? -1 : 17, 0, 0u, 0u, 0ULL, 0UL, 0L,                                                  /* wchan, nswap, cnswap, exit_signal, processor, rt_priority, policy, blkio, guest */
       (unsigned long)start_data, (unsigned long)end_data, (unsigned long)p->as.brk_start, (unsigned long)p->arg_start, (unsigned long)p->arg_end, (unsigned long)p->env_start,
       (unsigned long)p->env_end, zombie ? p->exit_status : 0);
}

/* /proc/<pid>/statm: size, resident, shared (file pages), text, lib (0 since Linux 2.6), data and stack, dirty (0), all in pages. */
static void pid_statm(PidCtx *c) {
    VmInfo vi;
    vm_info(c, &vi);
    bp(c->o, "%lu %lu %lu %lu 0 %lu 0\n", (unsigned long)(vi.size >> 12), (unsigned long)vi.rss, (unsigned long)vi.rss_file, (unsigned long)((vi.exe + vi.lib) >> 12),
       (unsigned long)((vi.data + vi.stack) >> 12));
}

/* /proc/<pid>/status, the keys of a current Linux in its order. Totals this kernel does not track (the peak of the virtual size and of the resident size, huge pages, the
 * speculation mitigations, the memory nodes) are what an idle single-processor machine would say. */
static void pid_status(PidCtx *c) {
    Process *p = c->p;
    Buf *o = c->o;
    char st = state_letter(p, c->t);
    int tid = c->t ? c->t->utid : p->pid;
    VmInfo vi;
    vm_info(c, &vi);
    u64 user, total, ign, cgt;
    cpu_ms(c, &user, &total);
    sig_sets(p, &ign, &cgt);
    const Task *lt = c->t ? c->t : p->task;                              /* the thread this file speaks for: the one asked about, else the process's first */
    u64 blocked = lt ? lt->sig_blocked : 0, pending = lt ? lt->sig_pending : 0;
    bool root = p->cred.uid == 0;
    bp(o, "Name:\t%.15s\nUmask:\t%04o\nState:\t%c (%s)\nTgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t%d\nTracerPid:\t0\nUid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nFDSize:\t%d\nGroups:\t",
       p->name, p->umask, st, state_word(st), p->pid, tid, p->ppid, p->ruid, p->cred.uid, p->cred.uid, p->cred.uid, p->rgid, p->cred.gid, p->cred.gid, p->cred.gid, fd_slots(p));
    for (u32 i = 0; i < p->cred.ngroups; i++) bp(o, "%u ", p->cred.groups[i]);
    bp(o, "\nNStgid:\t%d\nNSpid:\t%d\nNSpgid:\t%d\nNSsid:\t%d\nKthread:\t0\n", p->pid, tid, p->pgid, p->sid);
    u64 rss_anon = (vi.rss - vi.rss_file) * 4, rss_file = vi.rss_file * 4;
    bp(o, "VmPeak:\t%8lu kB\nVmSize:\t%8lu kB\nVmLck:\t%8lu kB\nVmPin:\t%8lu kB\nVmHWM:\t%8lu kB\nVmRSS:\t%8lu kB\nRssAnon:\t%8lu kB\nRssFile:\t%8lu kB\nRssShmem:\t%8lu kB\n",
       (unsigned long)(vi.size >> 10), (unsigned long)(vi.size >> 10), 0UL, 0UL, (unsigned long)(vi.rss * 4), (unsigned long)(vi.rss * 4), (unsigned long)rss_anon,
       (unsigned long)rss_file, 0UL);
    bp(o, "VmData:\t%8lu kB\nVmStk:\t%8lu kB\nVmExe:\t%8lu kB\nVmLib:\t%8lu kB\nVmPTE:\t%8lu kB\nVmSwap:\t%8lu kB\nHugetlbPages:\t%8lu kB\nCoreDumping:\t0\nTHP_enabled:\t1\n",
       (unsigned long)(vi.data >> 10), (unsigned long)(vi.stack >> 10), (unsigned long)(vi.exe >> 10), (unsigned long)(vi.lib >> 10), (unsigned long)(vi.rss / 128 * 4 + 4), 0UL, 0UL);
    bp(o, "Threads:\t%d\nSigQ:\t%d/%d\nSigPnd:\t%016lx\nShdPnd:\t%016lx\nSigBlk:\t%016lx\nSigIgn:\t%016lx\nSigCgt:\t%016lx\n", p->state == PROC_ZOMBIE ? 1 : p->nthreads, p->sigq_count, SIGQ_PER_PROC,
       (unsigned long)lnx_sigset_to_linux(pending), (unsigned long)lnx_sigset_to_linux(p->sig_pending), (unsigned long)lnx_sigset_to_linux(blocked),
       (unsigned long)lnx_sigset_to_linux(ign), (unsigned long)lnx_sigset_to_linux(cgt));
    bp(o, "CapInh:\t%016lx\nCapPrm:\t%016lx\nCapEff:\t%016lx\nCapBnd:\t000001ffffffffff\nCapAmb:\t%016lx\nNoNewPrivs:\t0\nSeccomp:\t0\nSeccomp_filters:\t0\n", 0UL,
       root ? 0x1ffffffffffUL : 0UL, root ? 0x1ffffffffffUL : 0UL, 0UL);
    bp(o, "Speculation_Store_Bypass:\tthread vulnerable\nSpeculationIndirectBranch:\tconditional enabled\nCpus_allowed:\t1\nCpus_allowed_list:\t0\nMems_allowed:\t00000001\n"
          "Mems_allowed_list:\t0\nvoluntary_ctxt_switches:\t%lu\nnonvoluntary_ctxt_switches:\t%lu\n",
       (unsigned long)(total / SCHED_SLICE), (unsigned long)(total / (SCHED_SLICE * 10)));
}

/* /proc/<pid>/cmdline and environ: the strings, NUL-separated, read from the process's own memory where exec put them (so a program that rewrites them shows the new text). */
static void put_user_range(PidCtx *c, u64 start, u64 end) {
    if (!start || end <= start || c->p->state == PROC_ZOMBIE) return;
    u64 len = MIN(end - start, (u64)(c->o->cap - c->o->n));
    size_t got = read_user(c->p, start, c->o->b + c->o->n, (size_t)len);
    c->o->n += got;
}

static void pid_cmdline(PidCtx *c) {
    Process *p = c->p;
    if (p->state == PROC_ZOMBIE) return;
    if (p->arg_start) { put_user_range(c, p->arg_start, p->arg_end); return; }
    for (const char *s = p->cmdline; *s; s++) bput(c->o, *s == ' ' ? "" : s, 1);                 /* a child that has not exec'd since the fork: the text kept for the native /proc */
    if (p->cmdline[0]) bput(c->o, "", 1);
}

static void pid_environ(PidCtx *c) { put_user_range(c, c->p->env_start, c->p->env_end); }
static void pid_comm(PidCtx *c) { bp(c->o, "%.15s\n", c->p->name); }

/* /proc/<pid>/maps and smaps: "start-end perms offset dev inode" and the path, the path starting in column 73 as Linux prints it; an anonymous mapping ends in a space. The heap and
 * the stack are named, a mapping that came from a file shows its path (the one the file was opened by: not resolved through symbolic links), the offset into it, the volume
 * (major 0, minor the mount's number) and the file's inode number. */
static void put_map_head(Buf *o, const Seg *s) {
    size_t start = o->n;
    u64 dev = s->file ? s->file->dev : 0;
    bp(o, "%08lx-%08lx %c%c%c%c %08lx %02x:%02x %lu ", (unsigned long)s->start, (unsigned long)s->end, (s->prot & PROT_READ) ? 'r' : '-', (s->prot & PROT_WRITE) ? 'w' : '-',
       (s->prot & PROT_EXEC) ? 'x' : '-', (s->flags & VMA_IO) ? 's' : 'p', (unsigned long)(s->file ? s->off : 0), (unsigned)((dev >> 8) & 0xff), (unsigned)(dev & 0xff),
       (unsigned long)(s->file ? s->file->ino : 0));
    const char *name = s->file ? s->file->path : (s->flags & VMA_STACK) ? "[stack]" : (s->flags & VMA_HEAP) ? "[heap]" : "";
    if (!name[0]) return;
    while (o->n - start < 73 && o->n < o->cap) bput(o, " ", 1);
    bp(o, "%s", name);
}

static void pid_maps(PidCtx *c) {
    for (int i = 0; i < c->nseg && c->o->n + 200 < c->o->cap; i++) {
        put_map_head(c->o, &c->segs[i]);
        bput(c->o, "\n", 1);
    }
}

static void vmflags(const Seg *s, char *out, size_t n) {
    out[0] = 0;
    if (s->prot & PROT_READ) strlcat(out, "rd ", n);
    if (s->prot & PROT_WRITE) strlcat(out, "wr ", n);
    if (s->prot & PROT_EXEC) strlcat(out, "ex ", n);
    if (s->flags & VMA_IO) strlcat(out, "sh ", n);
    if (s->prot & PROT_READ) strlcat(out, "mr ", n);
    if (s->prot & PROT_WRITE) strlcat(out, "mw ", n);
    if (s->prot & PROT_EXEC) strlcat(out, "me ", n);
    if ((s->prot & PROT_WRITE) && !(s->flags & VMA_IO)) strlcat(out, "ac ", n);
    if (s->flags & VMA_STACK) strlcat(out, "gd ", n);
    strlcat(out, "sd", n);
}

static void pid_smaps(PidCtx *c) {
    for (int i = 0; i < c->nseg && c->o->n + 1100 < c->o->cap; i++) {
        const Seg *s = &c->segs[i];
        u64 size = (s->end - s->start) >> 10, rss = seg_rss(&c->p->as, s) * 4;
        bool dirty = (s->prot & PROT_WRITE) && !(s->flags & VMA_IO);
        char fl[48];
        put_map_head(c->o, s);
        bput(c->o, "\n", 1);
        vmflags(s, fl, sizeof fl);
        bp(c->o, "Size:           %8lu kB\nKernelPageSize: %8d kB\nMMUPageSize:    %8d kB\nRss:            %8lu kB\nPss:            %8lu kB\nPss_Dirty:      %8lu kB\n"
                 "Shared_Clean:   %8lu kB\nShared_Dirty:   %8lu kB\nPrivate_Clean:  %8lu kB\nPrivate_Dirty:  %8lu kB\nReferenced:     %8lu kB\nAnonymous:      %8lu kB\n"
                 "KSM:            %8lu kB\nLazyFree:       %8lu kB\nAnonHugePages:  %8lu kB\nShmemPmdMapped: %8lu kB\nFilePmdMapped:  %8lu kB\nShared_Hugetlb: %8lu kB\n"
                 "Private_Hugetlb:%8lu kB\nSwap:           %8lu kB\nSwapPss:        %8lu kB\nLocked:         %8lu kB\nTHPeligible:    %8d\nVmFlags: %s\n",
           (unsigned long)size, 4, 4, (unsigned long)rss, (unsigned long)rss, dirty ? (unsigned long)rss : 0UL, 0UL, 0UL, dirty ? 0UL : (unsigned long)rss,
           dirty ? (unsigned long)rss : 0UL, (unsigned long)rss, !s->file && dirty ? (unsigned long)rss : 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0, fl);
    }
}

/* /proc/<pid>/limits: the soft and hard limits of the process, the ones getrlimit(2) reports and the kernel enforces (kernel/sys/rlimit.c, docs/HARDENING.md). */
static void limit_text(char *out, size_t n, u64 v) {
    if (v == RLIM_INFINITY) strlcpy(out, "unlimited", n);
    else snprintf(out, n, "%lu", (unsigned long)v);
}

static void pid_limits(PidCtx *c) {
    static const struct { const char *name; int res; const char *unit; } L[] = {
        { "Max cpu time", RLIMIT_CPU, "seconds" }, { "Max file size", RLIMIT_FSIZE, "bytes" }, { "Max data size", RLIMIT_DATA, "bytes" },
        { "Max stack size", RLIMIT_STACK, "bytes" }, { "Max core file size", RLIMIT_CORE, "bytes" }, { "Max resident set", RLIMIT_RSS, "bytes" },
        { "Max processes", RLIMIT_NPROC, "processes" }, { "Max open files", RLIMIT_NOFILE, "files" }, { "Max locked memory", RLIMIT_MEMLOCK, "bytes" },
        { "Max address space", RLIMIT_AS, "bytes" }, { "Max file locks", RLIMIT_LOCKS, "locks" }, { "Max pending signals", RLIMIT_SIGPENDING, "signals" },
        { "Max msgqueue size", RLIMIT_MSGQUEUE, "bytes" }, { "Max nice priority", RLIMIT_NICE, "" }, { "Max realtime priority", RLIMIT_RTPRIO, "" },
        { "Max realtime timeout", RLIMIT_RTTIME, "us" },
    };
    bp(c->o, "%-25s %-20s %-20s %-10s\n", "Limit", "Soft Limit", "Hard Limit", "Units");
    for (unsigned i = 0; i < ARRAY_SIZE(L); i++) {
        char soft[24], hard[24];
        limit_text(soft, sizeof soft, c->p->rlim.cur[L[i].res]);
        limit_text(hard, sizeof hard, c->p->rlim.max[L[i].res]);
        bp(c->o, "%-25s %-20s %-20s %-10s\n", L[i].name, soft, hard, L[i].unit);
    }
}

static void pid_mounts(PidCtx *c) { c->o->n += gen_mounts_view(c->o->b + c->o->n, c->o->cap - c->o->n, false); }
static void pid_mountinfo(PidCtx *c) { c->o->n += gen_mounts_view(c->o->b + c->o->n, c->o->cap - c->o->n, true); }
static void pid_cgroup(PidCtx *c) { bp(c->o, "0::/\n"); }
static void pid_zero(PidCtx *c) { bp(c->o, "0\n"); }
static void pid_unset(PidCtx *c) { bp(c->o, "4294967295\n"); }
static void pid_schedstat(PidCtx *c) {
    u64 user, total;
    cpu_ms(c, &user, &total);
    bp(c->o, "%lu 0 %lu\n", (unsigned long)(total * 1000000), (unsigned long)(total / SCHED_SLICE));
}

/* ---- the table of what a process directory holds */
enum { LK_ROOT, LK_SUBDIR, LK_PID_DIR, LK_TASK_DIR, LK_TID_DIR, LK_FD_DIR, LK_FDINFO_DIR, LK_GLOBAL_FILE, LK_PID_FILE, LK_FDINFO_FILE,
       LK_LINK_SELF, LK_LINK_THREAD_SELF, LK_LINK_EXE, LK_LINK_CWD, LK_LINK_ROOT, LK_LINK_FD, LK_LINK_NET };

typedef struct PidEntry {
    const char *name;
    u8     kind;                  /* LK_PID_FILE, a directory or a link */
    u16    mode;
    PidGen gen;
    size_t cap;                   /* bytes the generator may write */
    bool   segs;                  /* the generator needs the sorted list of mappings */
    bool   owner_only;            /* only the owner of the process and root may read it (maps: Linux asks for ptrace access) */
    bool   thread;                /* also in /proc/<pid>/task/<tid>/ */
} PidEntry;

static const PidEntry g_pid[] = {
    { "stat", LK_PID_FILE, 0444, pid_stat, 2048, true, false, true },
    { "statm", LK_PID_FILE, 0444, pid_statm, 128, true, false, true },
    { "status", LK_PID_FILE, 0444, pid_status, 4096, true, false, true },
    { "cmdline", LK_PID_FILE, 0444, pid_cmdline, 131072, false, false, true },
    { "environ", LK_PID_FILE, 0400, pid_environ, 131072, false, true, true },
    { "comm", LK_PID_FILE, 0444, pid_comm, 64, false, false, true },
    { "maps", LK_PID_FILE, 0444, pid_maps, 131072, true, true, true },
    { "smaps", LK_PID_FILE, 0444, pid_smaps, 524288, true, true, true },
    { "limits", LK_PID_FILE, 0444, pid_limits, 2048, false, false, true },
    { "mounts", LK_PID_FILE, 0444, pid_mounts, 8192, false, false, true },
    { "mountinfo", LK_PID_FILE, 0444, pid_mountinfo, 8192, false, false, true },
    { "cgroup", LK_PID_FILE, 0444, pid_cgroup, 32, false, false, true },
    { "oom_score", LK_PID_FILE, 0444, pid_zero, 32, false, false, true },
    { "oom_score_adj", LK_PID_FILE, 0444, pid_zero, 32, false, false, true },
    { "wchan", LK_PID_FILE, 0444, pid_zero, 32, false, false, true },
    { "loginuid", LK_PID_FILE, 0444, pid_unset, 32, false, false, true },
    { "sessionid", LK_PID_FILE, 0444, pid_unset, 32, false, false, true },
    { "schedstat", LK_PID_FILE, 0444, pid_schedstat, 128, false, false, true },
    { "exe", LK_LINK_EXE, 0777, NULL, 0, false, false, true },
    { "cwd", LK_LINK_CWD, 0777, NULL, 0, false, false, true },
    { "root", LK_LINK_ROOT, 0777, NULL, 0, false, false, true },
    { "fd", LK_FD_DIR, 0500, NULL, 0, false, false, true },
    { "fdinfo", LK_FDINFO_DIR, 0500, NULL, 0, false, false, true },
    { "task", LK_TASK_DIR, 0555, NULL, 0, false, false, false },
    { "net", LK_LINK_NET, 0777, NULL, 0, false, false, false },
};
#define NPID ((int)ARRAY_SIZE(g_pid))

/* ------------------------------------------------------------------ vnodes: the files, directories and links of this view */

typedef struct LpNode {
    u8          kind;             /* LK_* */
    u16         what;             /* index into g_global or g_pid */
    int         pid, tid, fd;
    const char *prefix;           /* LK_SUBDIR: the directory's path in g_global, its first plen bytes ("sys/kernel") */
    u16         plen;
} LpNode;

static const VNodeOps lp_file_ops, lp_dir_ops, lp_link_ops;

static void lp_release(VNode *v) { kfree(v->priv); }

static VNode *lp_make(Mount *m, const VNodeOps *ops, u32 mode, u64 ino, const LpNode *proto) {
    VNode *v = vnode_alloc(ops, m, mode);
    LpNode *ln = kmalloc(sizeof *ln);
    if (!v || !ln) { kfree(ln); if (v) vnode_put(v); return NULL; }
    *ln = *proto;
    v->priv = ln;
    v->ino = ino;
    return v;
}

static void owner_of(VNode *v, int pid) {                  /* a process's directory belongs to the user of the process, as ls -l shows it */
    u64 f = irq_save();
    Process *p = process_by_pid(pid);
    if (p) { v->uid = p->cred.uid; v->gid = p->cred.gid; }
    irq_restore(f);
}

static u64 pid_ino(int pid, int tid, int slot) { return 0x400000 + ((u64)pid << 12) + ((u64)(tid ? tid & 0x3f : 0) << 6) + (u64)slot; }

static ssize_t slice_out(const char *tmp, size_t len, void *buf, size_t n, u64 off) {
    if (off >= len) return 0;
    size_t r = MIN(n, (size_t)(len - off));
    memcpy(buf, tmp + off, r);
    return (ssize_t)r;
}

static int number_name(const char *s, int *out) {
    if (!*s) return 0;
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 99999999) return 0;
        v = v * 10 + (*s - '0');
    }
    *out = v;
    return 1;
}

/* ---- the files ---- */

/* What a descriptor refers to, the way Linux prints it: the path it was opened by, or pipe:[N], socket:[N], anon_inode:[...]. */
static void describe_fd(const File *f, char *out, size_t n) {
    if (f->path) { strlcpy(out, f->path, n); return; }
    unsigned long id = f->vn ? (unsigned long)(((u64)f->vn >> 4) & 0xFFFFFF) : 0;
    if (f->vn && S_ISFIFO(f->vn->mode)) snprintf(out, n, "pipe:[%lu]", id);
    else if (f->vn && (f->vn->mode & S_IFMT) == S_IFSOCK) snprintf(out, n, "socket:[%lu]", id);
    else snprintf(out, n, "anon_inode:[%s]", f->vn && S_ISCHR(f->vn->mode) ? "tty" : "file");
}

static ssize_t fdinfo_read(const LpNode *ln, void *buf, size_t n, u64 off) {
    char tmp[160];
    size_t len = 0;
    u64 fl = irq_save();
    Process *p = process_by_pid(ln->pid);
    File *f = p && ln->fd >= 0 && ln->fd < fd_slots(p) ? p->fds[ln->fd] : NULL;
    if (f) {
        len = (size_t)snprintf(tmp, sizeof tmp, "pos:\t%lu\nflags:\t0%o\nmnt_id:\t%u\nino:\t%lu\n", (unsigned long)f->off, (unsigned)(f->flags | ((p->fd_flags[ln->fd] & FD_CLOEXEC) ? O_CLOEXEC : 0)),
                               f->vn && f->vn->mount ? f->vn->mount->id : 0, (unsigned long)(f->vn ? f->vn->ino : 0));
        int pp = pidfd_pid(f);                                            /* a pidfd says which process it names (pidfd_open(2), proc(5)) */
        if (pp > 0 && len < sizeof tmp) len += (size_t)snprintf(tmp + len, sizeof tmp - len, "Pid:\t%d\n", process_by_pid(pp) ? pp : -1);
    }
    irq_restore(fl);
    return f ? slice_out(tmp, len, buf, n, off) : -ENOENT;
}

static ssize_t lp_file_read(VNode *v, File *file, void *buf, size_t n, u64 off) {
    (void)file;
    const LpNode *ln = v->priv;
    if (ln->kind == LK_FDINFO_FILE) return fdinfo_read(ln, buf, n, off);
    if (ln->kind == LK_GLOBAL_FILE) {
        const GlobalEntry *e = &g_global[ln->what];
        char *tmp = kmalloc(e->cap + 1);
        if (!tmp) return -ENOMEM;
        size_t len = e->gen(tmp, e->cap);
        ssize_t r = slice_out(tmp, MIN(len, e->cap), buf, n, off);          /* snprintf says how much it wanted, not how much it wrote */
        kfree(tmp);
        return r;
    }
    const PidEntry *e = &g_pid[ln->what];
    char *tmp = kmalloc(e->cap + 1);
    Seg *segs = e->segs ? kmalloc(MAXSEG * sizeof(Seg)) : NULL;
    if (!tmp || (e->segs && !segs)) { kfree(tmp); kfree(segs); return -ENOMEM; }
    Buf o = { tmp, e->cap, 0 };
    ssize_t r;
    u64 fl = irq_save();                                    /* nothing else runs: the process cannot exit, be reaped or change its mappings while we look */
    Process *p = process_by_pid(ln->pid);
    Task *t = NULL;
    bool ok = p != NULL;
    if (ok && ln->tid) {
        for (Task *x = p->threads; x; x = x->thread_next) if (x->utid == ln->tid) t = x;
        ok = t || ln->tid == ln->pid;
    }
    if (ok && e->owner_only && current->proc->cred.uid != 0 && current->proc->cred.uid != p->cred.uid) {
        irq_restore(fl);
        r = -EACCES;
    } else {
        if (ok) {
            PidCtx c = { p, t, &o, segs, 0 };
            if (e->segs && p->state != PROC_ZOMBIE) c.nseg = collect_segs(p, segs, MAXSEG);
            e->gen(&c);
        }
        irq_restore(fl);
        r = ok ? slice_out(tmp, o.n, buf, n, off) : -ESRCH;
    }
    kfree(tmp);
    kfree(segs);
    return r;
}

/* ---- the links ---- */

static ssize_t lp_readlink(VNode *v, char *buf, size_t n) {
    const LpNode *ln = v->priv;
    Process *me = current->proc;
    char tmp[260];
    tmp[0] = 0;
    switch (ln->kind) {
    case LK_LINK_SELF: snprintf(tmp, sizeof tmp, "%d", me->pid); break;
    case LK_LINK_THREAD_SELF: snprintf(tmp, sizeof tmp, "%d/task/%d", me->pid, current->utid); break;
    case LK_LINK_NET: strlcpy(tmp, "../net", sizeof tmp); break;
    default: {
        u64 fl = irq_save();
        Process *p = process_by_pid(ln->pid);
        if (p && p->state != PROC_ZOMBIE) {
            if (me->cred.uid != 0 && me->cred.uid != p->cred.uid) {         /* /proc/<pid>/exe, cwd, root and fd are for the owner and root (Linux asks for ptrace access) */
                irq_restore(fl);
                return -EACCES;
            }
            if (ln->kind == LK_LINK_EXE) strlcpy(tmp, p->exe, sizeof tmp);
            else if (ln->kind == LK_LINK_CWD) strlcpy(tmp, p->cwd_path, sizeof tmp);
            else if (ln->kind == LK_LINK_ROOT) strlcpy(tmp, p->root && !me->root ? "/linux" : "/", sizeof tmp);
            else if (ln->kind == LK_LINK_FD) {
                File *f = ln->fd >= 0 && ln->fd < fd_slots(p) ? p->fds[ln->fd] : NULL;
                if (f) describe_fd(f, tmp, sizeof tmp);
            }
        }
        irq_restore(fl);
        if (!tmp[0]) return -ENOENT;
    }
    }
    size_t len = MIN(strlen(tmp), n);
    memcpy(buf, tmp, len);
    return (ssize_t)len;
}

/* ---- the directories ---- */

static VNode *make_pid_dir(Mount *m, int pid, int tid) {
    LpNode n = { .kind = tid ? LK_TID_DIR : LK_PID_DIR, .pid = pid, .tid = tid };
    VNode *v = lp_make(m, &lp_dir_ops, S_IFDIR | 0555, pid_ino(pid, tid, 0), &n);
    if (v) owner_of(v, pid);
    return v;
}

static VNode *make_subdir(Mount *m, const char *prefix, size_t plen) {
    LpNode n = { .kind = LK_SUBDIR, .prefix = prefix, .plen = (u16)plen };
    return lp_make(m, &lp_dir_ops, S_IFDIR | 0555, 0x30000 + (u64)(uintptr_t)prefix % 1024 * 16 + plen, &n);
}

static VNode *make_special_link(Mount *m, int kind, u64 ino) {
    LpNode n = { .kind = (u8)kind };
    return lp_make(m, &lp_link_ops, S_IFLNK | 0777, ino, &n);
}

/* The idx-th distinct first component below the directory dn names in the table of global files; its name, and whether it is a directory (more slashes follow). */
static bool nth_component(const LpNode *dn, u64 idx, const char **name, size_t *len, bool *isdir) {
    u64 k = 0;
    for (int i = 0; i < NGLOBAL; i++) {
        const char *en = g_global[i].name;
        if (dn->plen) {
            if (strncmp(en, dn->prefix, dn->plen) || en[dn->plen] != '/') continue;
            en += dn->plen + 1;
        }
        const char *slash = strchr(en, '/');
        size_t cl = slash ? (size_t)(slash - en) : strlen(en);
        bool seen = false;
        for (int j = 0; j < i && !seen; j++) {
            const char *ej = g_global[j].name;
            if (dn->plen) {
                if (strncmp(ej, dn->prefix, dn->plen) || ej[dn->plen] != '/') continue;
                ej += dn->plen + 1;
            }
            const char *sj = strchr(ej, '/');
            size_t cj = sj ? (size_t)(sj - ej) : strlen(ej);
            seen = cj == cl && !strncmp(ej, en, cl);
        }
        if (seen) continue;
        if (k++ == idx) { *name = en; *len = cl; *isdir = slash != NULL; return true; }
    }
    return false;
}

static u64 component_count(const LpNode *dn) {
    const char *name;
    size_t len;
    bool isdir;
    u64 n = 0;
    while (nth_component(dn, n, &name, &len, &isdir)) n++;
    return n;
}

/* /proc itself and the directories below it that come from the table of global files. At the top, a name this view does not claim is the native /proc's to answer. */
static int global_lookup(VNode *dir, const LpNode *dn, const char *name, VNode **out) {
    Mount *m = dir->mount;
    bool top = dn->plen == 0;
    if (top) {
        if (!strcmp(name, "self")) { VNode *v = make_special_link(m, LK_LINK_SELF, 2); if (!v) return -ENOMEM; *out = v; return 0; }
        if (!strcmp(name, "thread-self")) { VNode *v = make_special_link(m, LK_LINK_THREAD_SELF, 3); if (!v) return -ENOMEM; *out = v; return 0; }
        int pid;
        if (number_name(name, &pid)) {
            if (!process_by_pid(pid)) return -ENOENT;
            VNode *v = make_pid_dir(m, pid, 0);
            if (!v) return -ENOMEM;
            *out = v;
            return 0;
        }
    }
    size_t nl = strlen(name);
    for (int i = 0; i < NGLOBAL; i++) {
        const char *en = g_global[i].name;
        if (!top) {
            if (strncmp(en, dn->prefix, dn->plen) || en[dn->plen] != '/') continue;
            en += dn->plen + 1;
        }
        const char *slash = strchr(en, '/');
        size_t cl = slash ? (size_t)(slash - en) : strlen(en);
        if (cl != nl || strncmp(en, name, nl)) continue;
        VNode *v;
        if (slash) {
            v = make_subdir(m, g_global[i].name, (size_t)(en - g_global[i].name) + cl);
        } else {
            LpNode n = { .kind = LK_GLOBAL_FILE, .what = (u16)i };
            v = lp_make(m, &lp_file_ops, S_IFREG | 0444, 0x30000 + (u64)i, &n);
        }
        if (!v) return -ENOMEM;
        *out = v;
        return 0;
    }
    return top ? PROC_NOT_MINE : -ENOENT;
}

static int pid_dir_lookup(VNode *dir, const LpNode *dn, const char *name, VNode **out) {
    Mount *m = dir->mount;
    for (int i = 0; i < NPID; i++) {
        const PidEntry *e = &g_pid[i];
        if ((dn->tid && !e->thread) || strcmp(e->name, name)) continue;
        bool is_dir = e->kind == LK_FD_DIR || e->kind == LK_FDINFO_DIR || e->kind == LK_TASK_DIR;
        bool is_file = e->kind == LK_PID_FILE;
        LpNode n = { .kind = e->kind, .what = (u16)i, .pid = dn->pid, .tid = dn->tid };
        VNode *v = lp_make(m, is_dir ? &lp_dir_ops : is_file ? &lp_file_ops : &lp_link_ops, (is_dir ? S_IFDIR : is_file ? S_IFREG : S_IFLNK) | e->mode,
                           pid_ino(dn->pid, dn->tid, 1 + i), &n);
        if (!v) return -ENOMEM;
        owner_of(v, dn->pid);
        *out = v;
        return 0;
    }
    return -ENOENT;
}

static int task_dir_lookup(VNode *dir, const LpNode *dn, const char *name, VNode **out) {
    int tid;
    if (!number_name(name, &tid)) return -ENOENT;
    u64 fl = irq_save();
    Process *p = process_by_pid(dn->pid);
    bool found = p && tid == dn->pid;
    if (p) for (Task *x = p->threads; x; x = x->thread_next) if (x->utid == tid) found = true;
    irq_restore(fl);
    if (!found) return -ENOENT;
    VNode *v = make_pid_dir(dir->mount, dn->pid, tid);
    if (!v) return -ENOMEM;
    *out = v;
    return 0;
}

static int fd_dir_lookup_lp(VNode *dir, const LpNode *dn, const char *name, VNode **out) {
    int fd;
    if (!number_name(name, &fd) || fd >= FD_HARD_MAX) return -ENOENT;
    u64 fl = irq_save();
    Process *p = process_by_pid(dn->pid);
    bool open = p && fd < fd_slots(p) && p->fds[fd];
    irq_restore(fl);
    if (!open) return -ENOENT;
    bool info = dn->kind == LK_FDINFO_DIR;
    LpNode n = { .kind = info ? LK_FDINFO_FILE : LK_LINK_FD, .pid = dn->pid, .fd = fd };
    VNode *v = lp_make(dir->mount, info ? &lp_file_ops : &lp_link_ops, info ? S_IFREG | 0400 : S_IFLNK | 0777, pid_ino(dn->pid, 0, 0x100 + (info ? 0x100 : 0) + fd), &n);
    if (!v) return -ENOMEM;
    owner_of(v, dn->pid);
    *out = v;
    return 0;
}

static VNode *parent_dir(VNode *dir, const LpNode *dn) {
    Mount *m = dir->mount;
    switch (dn->kind) {
    case LK_SUBDIR: {
        size_t up = dn->plen;
        while (up > 0 && dn->prefix[up - 1] != '/') up--;
        if (up > 0) return make_subdir(m, dn->prefix, up - 1);
        break;
    }
    case LK_FD_DIR: case LK_FDINFO_DIR: return make_pid_dir(m, dn->pid, 0);
    case LK_TID_DIR: {
        LpNode up = { .kind = LK_TASK_DIR, .pid = dn->pid };
        return lp_make(m, &lp_dir_ops, S_IFDIR | 0555, pid_ino(dn->pid, 0, 0), &up);
    }
    case LK_TASK_DIR: return make_pid_dir(m, dn->pid, 0);
    default: break;
    }
    vnode_get(m->root);
    return m->root;
}

static int lp_dir_lookup(VNode *dir, const char *name, VNode **out) {
    const LpNode *dn = dir->priv;
    if (!strcmp(name, ".")) { vnode_get(dir); *out = dir; return 0; }
    if (!strcmp(name, "..")) {
        VNode *up = parent_dir(dir, dn);
        if (!up) return -ENOMEM;
        *out = up;
        return 0;
    }
    switch (dn->kind) {
    case LK_SUBDIR: return global_lookup(dir, dn, name, out);
    case LK_PID_DIR: case LK_TID_DIR: return pid_dir_lookup(dir, dn, name, out);
    case LK_TASK_DIR: return task_dir_lookup(dir, dn, name, out);
    case LK_FD_DIR: case LK_FDINFO_DIR: return fd_dir_lookup_lp(dir, dn, name, out);
    default: return -ENOENT;
    }
}

static void fill_entry(DirEntry *out, u64 ino, u32 type, const char *name, size_t len) {
    out->ino = ino;
    out->type = type;
    out->next = 0;
    if (len >= sizeof out->name) len = sizeof out->name - 1;
    memcpy(out->name, name, len);
    out->name[len] = 0;
}

typedef struct PidList { int pid; u64 skip; bool found; } PidList;
static void pid_cb(Process *p, void *ctx) {
    PidList *l = ctx;
    if (l->skip) { l->skip--; return; }
    if (!l->found) { l->pid = p->pid; l->found = true; }
}

static int global_readdir(const LpNode *dn, u64 cookie, DirEntry *out) {
    const char *name;
    size_t len;
    bool isdir;
    if (nth_component(dn, cookie, &name, &len, &isdir)) {
        fill_entry(out, 0x30000 + cookie, isdir ? DT_DIR : DT_REG, name, len);
        return 1;
    }
    if (dn->plen) return 0;
    u64 k = cookie - component_count(dn);
    static const struct { const char *name; u32 type; } virt[] = { { "self", DT_LNK }, { "thread-self", DT_LNK }, { "net", DT_DIR } };
    if (k < ARRAY_SIZE(virt)) { fill_entry(out, 2 + k, virt[k].type, virt[k].name, strlen(virt[k].name)); return 1; }
    PidList l = { 0, k - ARRAY_SIZE(virt), false };
    process_for_each(pid_cb, &l);
    if (!l.found) return 0;
    char num[16];
    fill_entry(out, pid_ino(l.pid, 0, 0), DT_DIR, num, (size_t)snprintf(num, sizeof num, "%d", l.pid));
    return 1;
}

static int pid_dir_readdir(const LpNode *dn, u64 cookie, DirEntry *out) {
    u64 k = 0;
    for (int i = 0; i < NPID; i++) {
        const PidEntry *e = &g_pid[i];
        if (dn->tid && !e->thread) continue;
        if (k++ != cookie) continue;
        bool is_dir = e->kind == LK_FD_DIR || e->kind == LK_FDINFO_DIR || e->kind == LK_TASK_DIR;
        fill_entry(out, pid_ino(dn->pid, dn->tid, 1 + i), is_dir ? DT_DIR : e->kind == LK_PID_FILE ? DT_REG : DT_LNK, e->name, strlen(e->name));
        return 1;
    }
    return 0;
}

static int task_dir_readdir(const LpNode *dn, u64 cookie, DirEntry *out) {
    int found = -1;
    u64 fl = irq_save();
    Process *p = process_by_pid(dn->pid);
    if (p) {
        u64 k = 0;
        for (Task *x = p->threads; x && found < 0; x = x->thread_next) if (k++ == cookie) found = x->utid;
        if (!p->threads && cookie == 0) found = p->pid;                    /* a zombie has no threads left: its leader is listed */
    }
    irq_restore(fl);
    if (found < 0) return 0;
    char num[16];
    fill_entry(out, pid_ino(dn->pid, found, 0), DT_DIR, num, (size_t)snprintf(num, sizeof num, "%d", found));
    return 1;
}

static int fd_dir_readdir_lp(const LpNode *dn, u64 cookie, DirEntry *out) {
    int found = -1;
    u64 k = 0, fl = irq_save();
    Process *p = process_by_pid(dn->pid);
    if (p) for (int i = 0; i < fd_slots(p) && found < 0; i++) if (p->fds[i] && k++ == cookie) found = i;
    irq_restore(fl);
    if (found < 0) return 0;
    char num[16];
    fill_entry(out, pid_ino(dn->pid, 0, 0x100 + found), dn->kind == LK_FDINFO_DIR ? DT_REG : DT_LNK, num, (size_t)snprintf(num, sizeof num, "%d", found));
    return 1;
}

static int lp_dir_readdir(VNode *dir, u64 cookie, DirEntry *out) {
    const LpNode *dn = dir->priv;
    switch (dn->kind) {
    case LK_SUBDIR: return global_readdir(dn, cookie, out);
    case LK_PID_DIR: case LK_TID_DIR: return pid_dir_readdir(dn, cookie, out);
    case LK_TASK_DIR: return task_dir_readdir(dn, cookie, out);
    case LK_FD_DIR: case LK_FDINFO_DIR: return fd_dir_readdir_lp(dn, cookie, out);
    default: return 0;
    }
}

static const VNodeOps lp_file_ops = { .read = lp_file_read, .release = lp_release };
static const VNodeOps lp_dir_ops = { .lookup = lp_dir_lookup, .readdir = lp_dir_readdir, .release = lp_release };
static const VNodeOps lp_link_ops = { .readlink = lp_readlink, .release = lp_release };

/* ------------------------------------------------------------------ /sys: which processors exist */

/* The machine has no sysfs: layout.c makes /sys an empty directory, which is also what a Linux program sees there (the five directories of the machine are the root's own). lscpu
 * and the C library's get_nprocs() ask /sys/devices/system/cpu which processors there are; four small read-only files answer: one processor, number 0. */
static ssize_t sysfile_read(VNode *v, File *f, void *buf, size_t n, u64 off) {
    (void)f;
    const char *text = v->priv;
    size_t len = strlen(text);
    return slice_out(text, len, buf, n, off);
}

static const VNodeOps sysfile_ops = { .read = sysfile_read };

static void sysfs_init(void) {
    static const char *const dirpath = "/sys/devices/system/cpu";
    static const struct { const char *name, *text; } files[] = { { "online", "0\n" }, { "possible", "0\n" }, { "present", "0\n" }, { "offline", "\n" } };
    VNode *dir;
    if (vfs_mkdir_p(dirpath, 0555) || vfs_lookup(dirpath, NULL, NULL, LOOKUP_DIR, &dir)) return;
    for (unsigned i = 0; i < ARRAY_SIZE(files); i++) tmpfs_add_special(dir, files[i].name, S_IFREG | 0444, &sysfile_ops, (void *)files[i].text, 0, NULL);
    vnode_put(dir);
}
INITCALL(sysfs_init);

/* ------------------------------------------------------------------ the hooks kernel/fs/procfs.c calls for a Linux reader */

int linux_proc_lookup(VNode *dir, const char *name, VNode **out) {
    LpNode top = { .kind = LK_ROOT };
    return global_lookup(dir, &top, name, out);
}

int linux_proc_readdir(VNode *dir, u64 cookie, DirEntry *out) {
    (void)dir;
    LpNode top = { .kind = LK_ROOT };
    return global_readdir(&top, cookie, out);
}
