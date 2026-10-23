/* /proc: synthetic files generated on every read. */
#include "vfs.h"
#include "kernel.h"
#include "mm.h"
#include "irq.h"
#include "cpuinfo.h"
#include "ktime.h"
#include "task.h"

typedef size_t (*GenFn)(char *buf, size_t cap);

typedef struct ProcEntry {
    const char *name;
    GenFn gen;
} ProcEntry;

int vfs_mount_list(int idx, char *target, size_t tn, char *type, size_t yn, char *source, size_t sn);
u64 irq_count(int irq);

static size_t gen_version(char *b, size_t cap) {
    return (size_t)snprintf(b, cap, "%s version %s \"%s\" (clang, x86_64) #1\n", KERNEL_NAME, KERNEL_VERSION, KERNEL_CODENAME);
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

static const ProcEntry g_entries[] = {
    { "version", gen_version }, { "uptime", gen_uptime }, { "meminfo", gen_meminfo }, { "cpuinfo", gen_cpuinfo },
    { "mounts", gen_mounts }, { "interrupts", gen_interrupts }, { "tasks", gen_tasks },
};

static const VNodeOps proc_dir_ops, proc_file_ops;

static ssize_t proc_read(VNode *v, File *f, void *buf, size_t n, u64 off) {
    (void)f;
    const ProcEntry *e = v->priv;
    char *tmp = kmalloc(16384);
    if (!tmp) return -ENOMEM;
    size_t len = e->gen(tmp, 16384);
    if (len > 16384) len = 16384;
    ssize_t r = 0;
    if (off < len) {
        r = (ssize_t)MIN(n, len - off);
        memcpy(buf, tmp + off, (size_t)r);
    }
    kfree(tmp);
    return r;
}

static int proc_lookup(VNode *dir, const char *name, VNode **out) {
    if (!strcmp(name, ".") || !strcmp(name, "..")) { vnode_get(dir); *out = dir; return 0; }
    for (unsigned i = 0; i < ARRAY_SIZE(g_entries); i++) {
        if (strcmp(g_entries[i].name, name)) continue;
        VNode *v = vnode_alloc(&proc_file_ops, dir->mount, S_IFREG | 0444);
        if (!v) return -ENOMEM;
        v->priv = (void *)&g_entries[i];
        v->ino = 100 + i;
        *out = v;
        return 0;
    }
    return -ENOENT;
}

static int proc_readdir(VNode *dir, u64 cookie, DirEntry *out) {
    (void)dir;
    if (cookie >= ARRAY_SIZE(g_entries)) return 0;
    out->ino = 100 + cookie;
    out->type = DT_REG;
    strlcpy(out->name, g_entries[cookie].name, sizeof out->name);
    return 1;
}

static const VNodeOps proc_dir_ops = { .lookup = proc_lookup, .readdir = proc_readdir };
static const VNodeOps proc_file_ops = { .read = proc_read };

static int procfs_mount(const char *source, u32 flags, Mount *m) {
    (void)source; (void)flags;
    VNode *root = vnode_alloc(&proc_dir_ops, m, S_IFDIR | 0555);
    if (!root) return -ENOMEM;
    root->ino = 1;
    m->root = root;
    return 0;
}

const FsType g_procfs_type = { "procfs", procfs_mount, NULL };
