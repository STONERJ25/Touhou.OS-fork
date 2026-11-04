/* Warnings and /proc/kstats (docs/HARDENING.md).
 *
 * A warning is something that must never happen but that the kernel survived: it is logged under the tag WARNING with a
 * backtrace and counted. /proc/kstats lists the counters the hardening tests compare before and after a run: free memory, heap
 * use, and how many of each kind of object are alive, so a leak shows up as a number that does not come back. */
#include "kernel.h"
#include "arch.h"
#include "cpu.h"
#include "mm.h"
#include "process.h"
#include "initcall.h"

#define KWARN_LOUD 20                       /* warnings logged in full; the rest are only counted */

static u64 g_nwarn;

u64 kwarn_count(void) { return g_nwarn; }

void kwarn_at(const char *file, int line, const char *fmt, ...) {
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    const char *slash = strrchr(file, '/');
    u64 n = __atomic_add_fetch(&g_nwarn, 1, __ATOMIC_RELAXED);
    if (n <= KWARN_LOUD) {
        Process *p = current ? current->proc : NULL;
        klog("WARNING", "%s:%d: %s%s%s", slash ? slash + 1 : file, line, msg, p ? " -- during a system call of " : "", p ? p->name : "");
        dump_backtrace((u64)__builtin_return_address(0), (u64)__builtin_frame_address(0));
    } else if (n == KWARN_LOUD + 1) {
        klog("WARNING", "more warnings follow; they are only counted (/proc/kstats)");
    }
}

/* ---------------------------------------------------------------- /proc/kstats */

int pipes_alive(void);
int inet_sock_count(void);
int unix_sock_count(void);
int pty_count(void);
u64 oom_kill_count(void);
u64 watchdog_report_count(void);

static size_t gen_kstats(char *b, size_t cap) {
    u64 tmp = 0;
    tmpfs_stats(&tmp);
    return (size_t)snprintf(b, cap,
        "free_pages %lu\ntotal_pages %lu\nheap_bytes %lu\nslab_pages %lu\ntmpfs_pages %lu\n"
        "tasks %d\nprocesses %d\nuser_threads %d\nkstacks %d\n"
        "files %lu\nvnodes %lu\npipes %d\nunix_sockets %d\ninet_sockets %d\nptys %d\n"
        "warnings %lu\noom_kills %lu\nwatchdog_reports %lu\n",
        (unsigned long)pmm_free_pages_count(), (unsigned long)pmm_total_pages(), (unsigned long)heap_bytes_in_use(),
        (unsigned long)heap_slab_pages(), (unsigned long)tmp,
        task_count(), process_count(), thread_count(), kstack_slots_used(),
        (unsigned long)vfs_file_count(), (unsigned long)vfs_vnode_count(), pipes_alive(), unix_sock_count(), inet_sock_count(), pty_count(),
        (unsigned long)kwarn_count(), (unsigned long)oom_kill_count(), (unsigned long)watchdog_report_count());
}

static void kstats_init(void) { procfs_register("kstats", gen_kstats); }
INITCALL(kstats_init);
