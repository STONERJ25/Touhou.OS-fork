/* /proc: synthetic files generated on every read, plus one directory per process (stat, status, cmdline). */
#include "vfs.h"
#include "kernel.h"
#include "mm.h"
#include "irq.h"
#include "cpu.h"
#include "cpuinfo.h"
#include "pci.h"
#include "blk.h"
#include "ktime.h"
#include "task.h"
#include "cmdline.h"
#include "process.h"

typedef size_t (*GenFn)(char *buf, size_t cap);

typedef struct ProcEntry {
    const char *name;
    GenFn gen;
} ProcEntry;

u64 irq_count(int irq);

static size_t gen_version(char *b, size_t cap) {
    return (size_t)snprintf(b, cap, "%s version %s \"%s\" (clang, x86_64) #1\n", KERNEL_NAME, KERNEL_VERSION, KERNEL_CODENAME);
}

static size_t gen_cmdline(char *b, size_t cap) {
    return (size_t)snprintf(b, cap, "%s\n", cmdline_string());
}

static size_t gen_uptime(char *b, size_t cap) {
    u64 ms = uptime_ms();
    return (size_t)snprintf(b, cap, "%lu.%02lu\n", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 10));
}

static size_t gen_meminfo(char *b, size_t cap) {
    u64 total = pmm_total_pages() * 4, free = pmm_free_pages_count() * 4;
    u64 tmp;
    tmpfs_stats(&tmp);
    return (size_t)snprintf(b, cap,
        "MemTotal:     %8lu kB\nMemFree:      %8lu kB\nMemUsed:      %8lu kB\nHeapInUse:    %8lu kB\nTmpfs:        %8lu kB\n",
        (unsigned long)total, (unsigned long)free, (unsigned long)(total - free),
        (unsigned long)(heap_bytes_in_use() >> 10), (unsigned long)(tmp * 4));
}

static size_t gen_cpuinfo(char *b, size_t cap) {
    return (size_t)snprintf(b, cap, "vendor_id : %s\nmodel name: %s\nfamily    : %u\nmodel     : %u\nstepping  : %u\n",
                            g_cpu.vendor, g_cpu.brand, g_cpu.family, g_cpu.model, g_cpu.stepping);
}

static size_t gen_mounts(char *b, size_t cap) {
    size_t n = 0;
    char t[64], y[16], s[64];
    for (int i = 0; vfs_mount_list(i, t, sizeof t, y, sizeof y, s, sizeof s); i++)
        n += (size_t)snprintf(b + n, cap - n, "%s %s %s rw 0 0\n", s, t, y);
    return n;
}

static size_t gen_interrupts(char *b, size_t cap) {
    size_t n = 0;
    for (int i = 0; i < 16; i++) {
        u64 c = irq_count(i);
        if (c) n += (size_t)snprintf(b + n, cap - n, "%2d: %lu\n", i, (unsigned long)c);
    }
    return n;
}

typedef struct { char *b; size_t cap, n; } Sink;

static void task_line(Task *t, void *ctx) {
    Sink *s = ctx;
    static const char *const st[] = { "ready", "running", "blocked", "sleeping", "zombie" };
    if (s->n + 100 < s->cap)
        s->n += (size_t)snprintf(s->b + s->n, s->cap - s->n, "%4d %-9s %-16s ticks=%lu\n", t->tid, st[t->state], t->name,
                                 (unsigned long)t->cpu_ticks);
}

static size_t gen_tasks(char *b, size_t cap) {
    Sink s = { b, cap, 0 };
    task_for_each(task_line, &s);
    return s.n;
}

static size_t gen_loadavg(char *b, size_t cap) {
    return (size_t)snprintf(b, cap, "0.00 0.00 0.00 %d/%d %d\n", 1, task_count(), process_count());
}

static size_t gen_pci(char *b, size_t cap) { return pci_describe(b, cap); }
static size_t gen_partitions(char *b, size_t cap) { return blk_describe(b, cap); }

/* The table grows at run time: drivers add their own files with procfs_register(). */
static ProcEntry g_entries[48] = {
    { "version", gen_version }, { "uptime", gen_uptime }, { "meminfo", gen_meminfo }, { "cpuinfo", gen_cpuinfo },
    { "mounts", gen_mounts }, { "interrupts", gen_interrupts }, { "tasks", gen_tasks }, { "cmdline", gen_cmdline },
    { "loadavg", gen_loadavg }, { "pci", gen_pci }, { "partitions", gen_partitions },
};
static unsigned g_nentries = 11;
#define NSTATIC g_nentries

int procfs_register(const char *name, size_t (*gen)(char *buf, size_t cap)) {
    if (g_nentries >= ARRAY_SIZE(g_entries)) return -ENOSPC;
    g_entries[g_nentries].name = name;
    g_entries[g_nentries].gen = gen;
    g_nentries++;
    return 0;
}

/* ---- per-process files ---- */

typedef struct {
    int pid;
    int which;
} PidFile;

enum { PF_STAT, PF_STATUS, PF_CMDLINE, PF_COUNT };
static const char *const g_pidfile_names[PF_COUNT] = { "stat", "status", "cmdline" };

static u64 resident_pages(Process *p, u64 *total) {
    u64 rss = 0, vm = 0;
    for (Vma *v = p->as.vmas; v; v = v->next) {
        vm += (v->end - v->start) >> PAGE_SHIFT;
        if (v->flags & VMA_STACK) {                         /* the reserved 8 MiB: only look at the part near the top */
            for (u64 va = v->end - USER_STACK_INIT * 4; va < v->end; va += PAGE_SIZE)
                if (vmm_translate(p->as.pml4, va, NULL)) rss++;
            continue;
        }
        for (u64 va = v->start; va < v->end; va += PAGE_SIZE)
            if (vmm_translate(p->as.pml4, va, NULL)) rss++;
    }
    *total = vm;
    return rss;
}

static size_t gen_pidfile(const PidFile *pf, char *b, size_t cap) {
    u64 f = irq_save();
    Process *p = process_by_pid(pf->pid);
    if (!p) { irq_restore(f); return 0; }
    static const char *const st[] = { "R", "Z" };
    const char *state = st[p->state == PROC_ZOMBIE];
    if (p->state == PROC_RUNNING && p->task && p->task->state == TASK_SLEEPING) state = "S";
    else if (p->state == PROC_RUNNING && p->task && p->task->state == TASK_BLOCKED) state = "S";
    u64 ticks = (p->state == PROC_RUNNING && p->task) ? p->task->cpu_ticks : 0;
    u64 vm = 0, rss = 0;
    if (p->state == PROC_RUNNING) rss = resident_pages(p, &vm);
    size_t n = 0;
    switch (pf->which) {
    case PF_STAT:
        n = (size_t)snprintf(b, cap, "%d %d %d %u %u %s %lu %lu %lu %lu %s\n", p->pid, p->ppid, p->pgid, p->cred.uid, p->cred.gid,
                             state, (unsigned long)(vm * 4), (unsigned long)(rss * 4), (unsigned long)ticks,
                             (unsigned long)p->start_ms, p->name);
        break;
    case PF_STATUS:
        n = (size_t)snprintf(b, cap,
            "Name:\t%s\nState:\t%s\nPid:\t%d\nPPid:\t%d\nPGid:\t%d\nUid:\t%u\nGid:\t%u\nVmSize:\t%lu kB\nVmRSS:\t%lu kB\n"
            "CpuTicks:\t%lu\nStartMs:\t%lu\nCwd:\t%s\n",
            p->name, state, p->pid, p->ppid, p->pgid, p->cred.uid, p->cred.gid, (unsigned long)(vm * 4),
            (unsigned long)(rss * 4), (unsigned long)ticks, (unsigned long)p->start_ms, p->cwd_path);
        break;
    case PF_CMDLINE:
        n = (size_t)snprintf(b, cap, "%s\n", p->cmdline[0] ? p->cmdline : p->name);
        break;
    }
    irq_restore(f);
    return n;
}

static const VNodeOps proc_root_ops, proc_file_ops, pid_dir_ops, pid_file_ops;

static ssize_t slice_read(char *tmp, size_t len, void *buf, size_t n, u64 off) {
    if (len > 16384) len = 16384;
    ssize_t r = 0;
    if (off < len) {
        r = (ssize_t)MIN(n, len - off);
        memcpy(buf, tmp + off, (size_t)r);
    }
    return r;
}

static ssize_t proc_read(VNode *v, File *f, void *buf, size_t n, u64 off) {
    (void)f;
    const ProcEntry *e = v->priv;
    char *tmp = kmalloc(16384);
    if (!tmp) return -ENOMEM;
    ssize_t r = slice_read(tmp, e->gen(tmp, 16384), buf, n, off);
    kfree(tmp);
    return r;
}

static ssize_t pidfile_read(VNode *v, File *f, void *buf, size_t n, u64 off) {
    (void)f;
    char *tmp = kmalloc(4096);
    if (!tmp) return -ENOMEM;
    size_t len = gen_pidfile(v->priv, tmp, 4096);
    ssize_t r = len ? slice_read(tmp, len, buf, n, off) : -ESRCH;
    kfree(tmp);
    return r;
}

static void pidfile_release(VNode *v) { kfree(v->priv); }

static VNode *make_pid_dir(Mount *m, int pid) {
    VNode *d = vnode_alloc(&pid_dir_ops, m, S_IFDIR | 0555);
    if (!d) return NULL;
    d->priv = (void *)(long)pid;
    d->ino = 0x10000 + (u64)pid * 8;
    return d;
}

static int proc_root_lookup(VNode *dir, const char *name, VNode **out) {
    if (!strcmp(name, ".") || !strcmp(name, "..")) { vnode_get(dir); *out = dir; return 0; }
    for (unsigned i = 0; i < NSTATIC; i++) {
        if (strcmp(g_entries[i].name, name)) continue;
        VNode *v = vnode_alloc(&proc_file_ops, dir->mount, S_IFREG | 0444);
        if (!v) return -ENOMEM;
        v->priv = (void *)&g_entries[i];
        v->ino = 100 + i;
        *out = v;
        return 0;
    }
    int pid = -1;
    if (!strcmp(name, "self") && current->proc) pid = current->proc->pid;
    else if (name[0] >= '0' && name[0] <= '9') {
        pid = 0;
        for (const char *p = name; *p; p++) {
            if (*p < '0' || *p > '9') return -ENOENT;
            pid = pid * 10 + (*p - '0');
        }
    }
    if (pid > 0 && process_by_pid(pid)) {
        VNode *d = make_pid_dir(dir->mount, pid);
        if (!d) return -ENOMEM;
        *out = d;
        return 0;
    }
    return -ENOENT;
}

typedef struct { int *pids; u64 skip, found; } PidList;

static void pid_cb(Process *p, void *ctx) {
    PidList *l = ctx;
    if (l->skip) { l->skip--; return; }
    if (!l->found) { *l->pids = p->pid; l->found = 1; }
}

static int proc_root_readdir(VNode *dir, u64 cookie, DirEntry *out) {
    (void)dir;
    if (cookie < NSTATIC) {
        out->ino = 100 + cookie;
        out->type = DT_REG;
        strlcpy(out->name, g_entries[cookie].name, sizeof out->name);
        return 1;
    }
    int pid = 0;
    PidList l = { &pid, cookie - NSTATIC, 0 };
    process_for_each(pid_cb, &l);
    if (!l.found) return 0;
    out->ino = 0x10000 + (u64)pid * 8;
    out->type = DT_DIR;
    snprintf(out->name, sizeof out->name, "%d", pid);
    return 1;
}

static int pid_dir_lookup(VNode *dir, const char *name, VNode **out) {
    if (!strcmp(name, ".") || !strcmp(name, "..")) { vnode_get(dir); *out = dir; return 0; }
    for (int i = 0; i < PF_COUNT; i++) {
        if (strcmp(g_pidfile_names[i], name)) continue;
        VNode *v = vnode_alloc(&pid_file_ops, dir->mount, S_IFREG | 0444);
        PidFile *pf = kmalloc(sizeof *pf);
        if (!v || !pf) { kfree(pf); if (v) vnode_put(v); return -ENOMEM; }
        pf->pid = (int)(long)dir->priv;
        pf->which = i;
        v->priv = pf;
        *out = v;
        return 0;
    }
    return -ENOENT;
}

static int pid_dir_readdir(VNode *dir, u64 cookie, DirEntry *out) {
    (void)dir;
    if (cookie >= PF_COUNT) return 0;
    out->ino = 1;
    out->type = DT_REG;
    strlcpy(out->name, g_pidfile_names[cookie], sizeof out->name);
    return 1;
}

static const VNodeOps proc_root_ops = { .lookup = proc_root_lookup, .readdir = proc_root_readdir };
static const VNodeOps proc_file_ops = { .read = proc_read };
static const VNodeOps pid_dir_ops = { .lookup = pid_dir_lookup, .readdir = pid_dir_readdir };
static const VNodeOps pid_file_ops = { .read = pidfile_read, .release = pidfile_release };

static int procfs_mount(const char *source, u32 flags, Mount *m) {
    (void)source; (void)flags;
    VNode *root = vnode_alloc(&proc_root_ops, m, S_IFDIR | 0555);
    if (!root) return -ENOMEM;
    root->ino = 1;
    m->root = root;
    return 0;
}

const FsType g_procfs_type = { "procfs", procfs_mount, NULL };
