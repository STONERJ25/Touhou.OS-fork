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

typedef ssize_t (*WriteFn)(const char *buf, size_t n);

typedef struct ProcEntry {
    const char *name;           /* "uptime", or "net/dev" for a file inside a subdirectory */
    GenFn gen;
    WriteFn wr;                 /* NULL = read-only */
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
        n += (size_t)snprintf(b + n, cap - n, "%s %s %s %s 0 0\n", s, t, y, (vfs_mount_flags(i) & MNT_RDONLY) ? "ro" : "rw");
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
    if (s->n + 120 < s->cap) {
        s->n += (size_t)snprintf(s->b + s->n, s->cap - s->n, "%4d %-9s %-16s ticks=%lu", t->tid, st[t->state], t->name,
                                 (unsigned long)t->cpu_ticks);
        if (t->tgid) s->n += (size_t)snprintf(s->b + s->n, s->cap - s->n, " pid=%d tid=%d", t->tgid, t->utid);   /* user threads */
        s->b[s->n++] = '\n';
    }
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
static ProcEntry g_entries[64] = {
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
    g_entries[g_nentries].wr = NULL;
    g_nentries++;
    return 0;
}

/* Like procfs_register, and root may also write the file (mode 0644): wr gets the bytes of each write(). A name with a
 * slash ("net/dev") lives in a subdirectory of /proc that appears on its own. */
int procfs_register_rw(const char *name, size_t (*gen)(char *buf, size_t cap), ssize_t (*wr)(const char *buf, size_t n)) {
    int rc = procfs_register(name, gen);
    if (!rc) g_entries[g_nentries - 1].wr = wr;
    return rc;
}

/* Entries whose name contains a slash are hidden from the root listing; the first of each prefix shows up as a directory. */
static bool entry_first_of_dir(unsigned i) {
    const char *name = g_entries[i].name;
    const char *sl = strchr(name, '/');
    if (!sl) return false;
    size_t plen = (size_t)(sl - name);
    for (unsigned j = 0; j < i; j++)
        if (!strncmp(g_entries[j].name, name, plen) && g_entries[j].name[plen] == '/') return false;
    return true;
}

static unsigned root_visible_count(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < g_nentries; i++) if (!strchr(g_entries[i].name, '/') || entry_first_of_dir(i)) n++;
    return n;
}

/* ---- per-process files ---- */

typedef struct {
    int pid;
    int which;
} PidFile;

enum { PF_STAT, PF_STATUS, PF_CMDLINE, PF_THREADS, PF_COUNT };
static const char *const g_pidfile_names[PF_COUNT] = { "stat", "status", "cmdline", "threads" };

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
    if (p->state == PROC_RUNNING && p->stopped) state = "T";                 /* stopped by SIGSTOP / SIGTSTP / SIGTTIN / SIGTTOU (job control) */
    u64 ticks = 0;                                      /* CPU time of the whole process: every thread counts */
    if (p->state == PROC_RUNNING) for (Task *t = p->threads; t; t = t->thread_next) ticks += t->cpu_ticks;
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
            "CpuTicks:\t%lu\nStartMs:\t%lu\nCwd:\t%s\nThreads:\t%d\nNice:\t%d\n",
            p->name, state, p->pid, p->ppid, p->pgid, p->cred.uid, p->cred.gid, (unsigned long)(vm * 4),
            (unsigned long)(rss * 4), (unsigned long)ticks, (unsigned long)p->start_ms, p->cwd_path, p->nthreads, p->nice);
        break;
    case PF_CMDLINE:
        n = (size_t)snprintf(b, cap, "%s\n", p->cmdline[0] ? p->cmdline : p->name);
        break;
    case PF_THREADS:                                    /* one line per thread: tid, state (R ready/running, S asleep), ticks, name */
        for (Task *t = p->threads; t && n + 80 < cap; t = t->thread_next)
            n += (size_t)snprintf(b + n, cap - n, "%d %s %lu %s\n", t->utid,
                                  p->stopped ? "T" : (t->state == TASK_SLEEPING || t->state == TASK_BLOCKED) ? "S" : "R",
                                  (unsigned long)t->cpu_ticks, t->name);
        if (!n) n = (size_t)snprintf(b, cap, "%d Z 0 %s\n", p->pid, p->name);     /* a zombie has no threads left; the process itself is listed */
        break;
    }
    irq_restore(f);
    return n;
}

static const VNodeOps proc_root_ops, proc_file_ops, pid_dir_ops, pid_file_ops, proc_sub_ops;

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

static ssize_t proc_write(VNode *v, File *f, const void *buf, size_t n, u64 off) {
    (void)f; (void)off;
    const ProcEntry *e = v->priv;
    return e->wr ? e->wr(buf, n) : -EACCES;
}

static ssize_t pidfile_read(VNode *v, File *f, void *buf, size_t n, u64 off) {
    (void)f;
    char *tmp = kmalloc(16384);
    if (!tmp) return -ENOMEM;
    size_t len = gen_pidfile(v->priv, tmp, 16384);
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
        VNode *v = vnode_alloc(&proc_file_ops, dir->mount, S_IFREG | (g_entries[i].wr ? 0644 : 0444));
        if (!v) return -ENOMEM;
        v->priv = (void *)&g_entries[i];
        v->ino = 100 + i;
        *out = v;
        return 0;
    }
    for (unsigned i = 0; i < NSTATIC; i++) {                    /* a subdirectory such as /proc/net */
        const char *sl = strchr(g_entries[i].name, '/');
        size_t nl = strlen(name);
        if (!sl || (size_t)(sl - g_entries[i].name) != nl || strncmp(g_entries[i].name, name, nl)) continue;
        VNode *d = vnode_alloc(&proc_sub_ops, dir->mount, S_IFDIR | 0555);
        if (!d) return -ENOMEM;
        d->priv = (void *)g_entries[i].name;
        d->ino = 0x8000 + i;
        *out = d;
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
    unsigned nvis = root_visible_count();
    if (cookie < nvis) {
        u64 k = 0;
        for (unsigned i = 0; i < g_nentries; i++) {
            const char *sl = strchr(g_entries[i].name, '/');
            if (sl && !entry_first_of_dir(i)) continue;
            if (k++ != cookie) continue;
            out->ino = 100 + i;
            out->type = sl ? DT_DIR : DT_REG;
            size_t nl = sl ? (size_t)(sl - g_entries[i].name) : strlen(g_entries[i].name);
            if (nl >= sizeof out->name) nl = sizeof out->name - 1;
            memcpy(out->name, g_entries[i].name, nl);
            out->name[nl] = 0;
            return 1;
        }
        return 0;
    }
    int pid = 0;
    PidList l = { &pid, cookie - nvis, 0 };
    process_for_each(pid_cb, &l);
    if (!l.found) return 0;
    out->ino = 0x10000 + (u64)pid * 8;
    out->type = DT_DIR;
    snprintf(out->name, sizeof out->name, "%d", pid);
    return 1;
}

/* ---- symlinks into a process: /proc/<pid>/cwd, exe and fd/N (docs/JOBCONTROL.md) ---- */

typedef struct { int pid; int kind; int fd; } ProcLink;
enum { PL_CWD, PL_EXE, PL_FD };

static const VNodeOps proclink_ops, pid_fd_ops;

/* What a descriptor refers to, the way Linux prints it: the path it was opened by, or pipe:[N], socket:[N], anon_inode:[...]. */
static void describe_file(const File *f, char *out, size_t n) {
    if (f->path) { strlcpy(out, f->path, n); return; }
    unsigned long id = f->vn ? (unsigned long)(((u64)f->vn >> 4) & 0xFFFFFF) : 0;       /* both ends of a pipe share the vnode */
    if (f->vn && S_ISFIFO(f->vn->mode)) snprintf(out, n, "pipe:[%lu]", id);
    else if (f->vn && (f->vn->mode & S_IFMT) == S_IFSOCK) snprintf(out, n, "socket:[%lu]", id);
    else snprintf(out, n, "anon_inode:[%s]", f->vn && S_ISCHR(f->vn->mode) ? "tty" : "file");
}

static ssize_t proclink_readlink(VNode *v, char *buf, size_t n) {
    const ProcLink *pl = v->priv;
    char tmp[256];
    tmp[0] = 0;
    u64 f = irq_save();                                         /* the process cannot exit while we copy */
    Process *p = process_by_pid(pl->pid);
    if (p && p->state != PROC_ZOMBIE) {
        switch (pl->kind) {
        case PL_CWD: strlcpy(tmp, p->cwd_path, sizeof tmp); break;
        case PL_EXE: strlcpy(tmp, p->exe, sizeof tmp); break;
        case PL_FD: {
            File *fl = (pl->fd >= 0 && pl->fd < MAX_FD) ? p->fds[pl->fd] : NULL;
            if (fl) describe_file(fl, tmp, sizeof tmp);
            break;
        }
        }
    }
    irq_restore(f);
    if (!tmp[0]) return -ENOENT;
    size_t len = strlen(tmp);
    if (len > n) len = n;
    memcpy(buf, tmp, len);
    return (ssize_t)len;
}

static void proclink_release(VNode *v) { kfree(v->priv); }

static VNode *make_proclink(Mount *m, int pid, int kind, int fd) {
    VNode *v = vnode_alloc(&proclink_ops, m, S_IFLNK | 0777);
    ProcLink *pl = kmalloc(sizeof *pl);
    if (!v || !pl) { kfree(pl); if (v) vnode_put(v); return NULL; }
    pl->pid = pid;
    pl->kind = kind;
    pl->fd = fd;
    v->priv = pl;
    v->ino = 0x10000 + (u64)pid * 8 + 2 + (u64)(kind == PL_FD ? 0x100 + (u64)fd : (u64)kind);
    return v;
}

/* /proc/<pid>/fd: one symlink per open descriptor; only the owner of the process (and root) may look. */
static int fd_dir_lookup(VNode *dir, const char *name, VNode **out) {
    if (!strcmp(name, ".") || !strcmp(name, "..")) { vnode_get(dir); *out = dir; return 0; }
    if (!name[0] || strlen(name) > 3) return -ENOENT;
    int fd = 0;
    for (const char *s = name; *s; s++) {
        if (*s < '0' || *s > '9') return -ENOENT;
        fd = fd * 10 + (*s - '0');
    }
    int pid = (int)(long)dir->priv;
    u64 f = irq_save();
    Process *p = process_by_pid(pid);
    bool open = p && fd < MAX_FD && p->fds[fd];
    irq_restore(f);
    if (!open) return -ENOENT;
    VNode *v = make_proclink(dir->mount, pid, PL_FD, fd);
    if (!v) return -ENOMEM;
    *out = v;
    return 0;
}

static int fd_dir_readdir(VNode *dir, u64 cookie, DirEntry *out) {
    int pid = (int)(long)dir->priv, found = -1;
    u64 k = 0;
    u64 f = irq_save();
    Process *p = process_by_pid(pid);
    if (p) for (int i = 0; i < MAX_FD && found < 0; i++) if (p->fds[i] && k++ == cookie) found = i;
    irq_restore(f);
    if (found < 0) return 0;
    out->ino = 0x10000 + (u64)pid * 8 + 0x102 + (u64)found;
    out->type = DT_LNK;
    snprintf(out->name, sizeof out->name, "%d", found);
    return 1;
}

static int pid_dir_lookup(VNode *dir, const char *name, VNode **out) {
    if (!strcmp(name, ".") || !strcmp(name, "..")) { vnode_get(dir); *out = dir; return 0; }
    int dpid = (int)(long)dir->priv;
    if (!strcmp(name, "cwd") || !strcmp(name, "exe")) {
        VNode *v = make_proclink(dir->mount, dpid, name[0] == 'c' ? PL_CWD : PL_EXE, 0);
        if (!v) return -ENOMEM;
        *out = v;
        return 0;
    }
    if (!strcmp(name, "fd")) {
        VNode *d = vnode_alloc(&pid_fd_ops, dir->mount, S_IFDIR | 0500);
        if (!d) return -ENOMEM;
        u64 f = irq_save();
        Process *p = process_by_pid(dpid);
        if (p) { d->uid = p->cred.uid; d->gid = p->cred.gid; }
        irq_restore(f);
        d->priv = (void *)(long)dpid;
        d->ino = 0x10000 + (u64)dpid * 8 + 1;
        *out = d;
        return 0;
    }
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
    if (cookie >= PF_COUNT) {                           /* after the files: cwd, exe (links) and fd (a directory) */
        static const char *const extra[] = { "cwd", "exe", "fd" };
        u64 e = cookie - PF_COUNT;
        if (e >= ARRAY_SIZE(extra)) return 0;
        out->ino = 1;
        out->type = e == 2 ? DT_DIR : DT_LNK;
        strlcpy(out->name, extra[e], sizeof out->name);
        return 1;
    }
    out->ino = 1;
    out->type = DT_REG;
    strlcpy(out->name, g_pidfile_names[cookie], sizeof out->name);
    return 1;
}

/* A subdirectory of /proc made of the registered entries named "<dir>/<file>". */
static int proc_sub_lookup(VNode *dir, const char *name, VNode **out) {
    if (!strcmp(name, ".") || !strcmp(name, "..")) { vnode_get(dir); *out = dir; return 0; }
    const char *full = dir->priv;
    size_t plen = (size_t)(strchr(full, '/') - full);
    for (unsigned i = 0; i < NSTATIC; i++) {
        const char *en = g_entries[i].name;
        if (strncmp(en, full, plen) || en[plen] != '/' || strcmp(en + plen + 1, name)) continue;
        VNode *v = vnode_alloc(&proc_file_ops, dir->mount, S_IFREG | (g_entries[i].wr ? 0644 : 0444));
        if (!v) return -ENOMEM;
        v->priv = (void *)&g_entries[i];
        v->ino = 100 + i;
        *out = v;
        return 0;
    }
    return -ENOENT;
}

static int proc_sub_readdir(VNode *dir, u64 cookie, DirEntry *out) {
    const char *full = dir->priv;
    size_t plen = (size_t)(strchr(full, '/') - full);
    u64 k = 0;
    for (unsigned i = 0; i < NSTATIC; i++) {
        const char *en = g_entries[i].name;
        if (strncmp(en, full, plen) || en[plen] != '/') continue;
        if (k++ != cookie) continue;
        out->ino = 100 + i;
        out->type = DT_REG;
        strlcpy(out->name, en + plen + 1, sizeof out->name);
        return 1;
    }
    return 0;
}

static const VNodeOps proc_root_ops = { .lookup = proc_root_lookup, .readdir = proc_root_readdir };
static const VNodeOps proc_sub_ops = { .lookup = proc_sub_lookup, .readdir = proc_sub_readdir };
static const VNodeOps proc_file_ops = { .read = proc_read, .write = proc_write };
static const VNodeOps pid_dir_ops = { .lookup = pid_dir_lookup, .readdir = pid_dir_readdir };
static const VNodeOps pid_file_ops = { .read = pidfile_read, .release = pidfile_release };
static const VNodeOps proclink_ops = { .readlink = proclink_readlink, .release = proclink_release };
static const VNodeOps pid_fd_ops = { .lookup = fd_dir_lookup, .readdir = fd_dir_readdir };

static int procfs_mount(const char *source, u32 flags, Mount *m) {
    (void)source; (void)flags;
    VNode *root = vnode_alloc(&proc_root_ops, m, S_IFDIR | 0555);
    if (!root) return -ENOMEM;
    root->ino = 1;
    m->root = root;
    return 0;
}

static int procfs_statfs(Mount *m, struct gstatfs *out) {
    (void)m;
    out->f_type = 0x9fa0;                       /* PROC_SUPER_MAGIC; nothing here occupies storage */
    return 0;
}

const FsType g_procfs_type = { "procfs", procfs_mount, NULL, procfs_statfs };
