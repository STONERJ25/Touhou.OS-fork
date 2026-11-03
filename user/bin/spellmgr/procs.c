/* Reading processes from /proc. Everything here tolerates processes that appear and vanish between two system calls. */
#include "spell.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* "pid ppid pgid uid gid state vsz_kb rss_kb cpu_ms start_ms name..." */
bool proc_read(int pid, Proc *p) {
    char path[48], buf[320];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    ssize_t n = read_file(path, buf, sizeof buf);
    if (n <= 0) return false;
    memset(p, 0, sizeof *p);
    int used = 0;
    char st[4] = "?";
    int got = sscanf(buf, "%d %d %d %u %u %3s %lu %lu %lu %lu %n", &p->pid, &p->ppid, &p->pgid, &p->uid, &p->gid, st, &p->vsz_kb, &p->rss_kb,
                     &p->ticks, &p->start_ms, &used);
    if (got < 10 || used <= 0 || p->pid != pid) return false;
    p->state = st[0];
    const char *name = buf + used;
    size_t len = strcspn(name, "\r\n");
    if (len >= sizeof p->name) len = sizeof p->name - 1;
    memcpy(p->name, name, len);
    p->name[len] = 0;
    if (!p->name[0]) snprintf(p->name, sizeof p->name, "(unnamed)");
    snprintf(p->user, sizeof p->user, "%s", user_name(p->uid));
    return true;
}

bool proc_cmdline(int pid, char *out, size_t cap) {
    char path[48];
    snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    ssize_t n = read_file(path, out, cap);
    if (n <= 0) { if (cap) out[0] = 0; return false; }
    size_t len = strcspn(out, "\r\n");
    out[len] = 0;
    return true;
}

bool proc_cwd(int pid, char *out, size_t cap) {
    char path[48], buf[512];
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    if (read_file(path, buf, sizeof buf) <= 0) { if (cap) out[0] = 0; return false; }
    char *c = strstr(buf, "Cwd:");
    if (!c) { out[0] = 0; return false; }
    c += 4;
    while (*c == ' ' || *c == '\t') c++;
    size_t len = strcspn(c, "\r\n");
    if (len >= cap) len = cap - 1;
    memcpy(out, c, len);
    out[len] = 0;
    return true;
}

void proclist_free(ProcList *l) {
    free(l->v);
    memset(l, 0, sizeof *l);
}

void proclist_copy(ProcList *dst, const ProcList *src) {
    if (dst->cap < src->n) {
        free(dst->v);
        dst->cap = src->n + 16;
        dst->v = malloc((size_t)dst->cap * sizeof *dst->v);
        if (!dst->v) { dst->cap = dst->n = 0; return; }
    }
    if (src->n) memcpy(dst->v, src->v, (size_t)src->n * sizeof *src->v);
    dst->n = src->n;
    dst->at_ms = src->at_ms;
}

const Proc *proclist_find(const ProcList *l, int pid) {
    for (int i = 0; i < l->n; i++) if (l->v[i].pid == pid) return &l->v[i];
    return NULL;
}

void proclist_scan(ProcList *cur, const ProcList *prev) {
    cur->n = 0;
    cur->at_ms = k_now_ms();
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        int pid = atoi(e->d_name);
        if (pid <= 0) continue;
        Proc p;
        if (!proc_read(pid, &p)) continue;                      /* it ended while we were looking */
        if (cur->n == cur->cap) {
            int nc = cur->cap ? cur->cap * 2 : 64;
            Proc *nv = realloc(cur->v, (size_t)nc * sizeof *nv);
            if (!nv) break;
            cur->v = nv;
            cur->cap = nc;
        }
        p.cpu10 = 0;
        const Proc *old = prev ? proclist_find(prev, pid) : NULL;
        if (old && prev->at_ms && cur->at_ms > prev->at_ms && p.ticks >= old->ticks && p.start_ms == old->start_ms) {
            unsigned long dt = (unsigned long)(cur->at_ms - prev->at_ms);
            unsigned long used = p.ticks - old->ticks;
            unsigned long v = used * 1000 / dt;
            p.cpu10 = v > 1000 ? 1000 : (int)v;
        }
        cur->v[cur->n++] = p;
    }
    closedir(d);
}

/* ---------------------------------------------------------------- sorting and filtering */

static int g_sort_col;
static bool g_sort_desc;
static const ProcList *g_sort_list;

static int cmp_idx(const void *a, const void *b) {
    const Proc *x = &g_sort_list->v[*(const int *)a], *y = &g_sort_list->v[*(const int *)b];
    int r = 0;
    switch (g_sort_col) {
    case COL_PID: r = x->pid - y->pid; break;
    case COL_NAME: r = strcasecmp(x->name, y->name); break;
    case COL_USER: r = strcasecmp(x->user, y->user); break;
    case COL_STATE: r = (int)x->state - (int)y->state; break;
    case COL_CPU: r = x->cpu10 - y->cpu10; break;
    case COL_MEM: r = x->rss_kb == y->rss_kb ? 0 : (x->rss_kb < y->rss_kb ? -1 : 1); break;
    }
    if (r == 0) r = x->pid - y->pid;
    return g_sort_desc ? -r : r;
}

static bool contains_nocase(const char *hay, const char *needle) {
    if (!*needle) return true;
    return strcasestr(hay, needle) != NULL;
}

void procview_build(ProcView *v, const ProcList *list, int sort_col, bool descending, const char *filter) {
    if (v->cap < list->n) {
        free(v->idx);
        v->cap = list->n + 32;
        v->idx = malloc((size_t)v->cap * sizeof *v->idx);
        if (!v->idx) { v->cap = v->n = 0; return; }
    }
    v->n = 0;
    char pidtxt[16];
    for (int i = 0; i < list->n; i++) {
        const Proc *p = &list->v[i];
        if (filter && filter[0]) {
            snprintf(pidtxt, sizeof pidtxt, "%d", p->pid);
            if (!contains_nocase(p->name, filter) && !contains_nocase(p->user, filter) && !contains_nocase(pidtxt, filter) &&
                !contains_nocase(state_word(p->state), filter))
                continue;
        }
        v->idx[v->n++] = i;
    }
    g_sort_col = sort_col;
    g_sort_desc = descending;
    g_sort_list = list;
    qsort(v->idx, (size_t)v->n, sizeof *v->idx, cmp_idx);
}
