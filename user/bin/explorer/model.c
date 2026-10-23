/* The folder model: entries read in slices (so a folder with thousands of files never freezes the window), the hidden-file
 * filter, sorting (folders first, whatever the column), and selection helpers. */
#include "explorer.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void model_init(Model *m) {
    memset(m, 0, sizeof *m);
    m->sort_col = COL_NAME;
}

void entry_free(Entry *e) {
    if (!e) return;
    free(e->name);
    free(e->key);
    free(e);
}

Entry *entry_make(const char *dir, const char *name) {
    Entry *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    char folded[EX_NAME];
    fold_name(name, folded, sizeof folded);
    e->name = strdup(name);
    e->key = strdup(folded);
    if (!e->name || !e->key) { entry_free(e); return NULL; }
    e->hidden = name[0] == '.';
    char full[EX_PATH * 2];
    path_join(full, sizeof full, dir, name);
    struct stat st;
    if (lstat(full, &st) != 0) {                                   /* vanished or unreadable: still listed, as something odd */
        e->kind = EK_OTHER;
        e->ftype = FT_GENERIC;
        return e;
    }
    e->mode = st.st_mode;
    e->size = st.st_size;
    e->mtime = st.st_mtime;
    e->uid = st.st_uid;
    e->gid = st.st_gid;
    if (S_ISLNK(st.st_mode)) {
        e->kind = EK_LINK;
        struct stat t;
        if (stat(full, &t) == 0) {
            e->is_dir = S_ISDIR(t.st_mode);
            e->size = t.st_size;
            e->mtime = t.st_mtime;
        } else e->broken = true;
        e->ftype = e->is_dir ? FT_GENERIC : classify(name, st.st_mode & ~0111u, EK_FILE);
    } else if (S_ISDIR(st.st_mode)) {
        e->kind = EK_DIR;
        e->is_dir = true;
    } else {
        e->kind = S_ISREG(st.st_mode) ? EK_FILE : EK_OTHER;
        e->ftype = classify(name, st.st_mode, e->kind);
    }
    return e;
}

void model_clear(Model *m) {
    for (int i = 0; i < m->nall; i++) entry_free(m->all[i]);
    m->nall = m->nshown = m->nhidden = 0;
}

void model_abort(Model *m) {
    if (m->dp) closedir(m->dp);
    m->dp = NULL;
    m->scanning = false;
    model_clear(m);
}

static bool grow(Model *m) {
    if (m->nall < m->cap) return true;
    int ncap = m->cap ? m->cap * 2 : 128;
    Entry **na = realloc(m->all, (size_t)ncap * sizeof *na);
    if (!na) return false;
    m->all = na;
    Entry **ns = realloc(m->shown, (size_t)ncap * sizeof *ns);
    if (!ns) return false;
    m->shown = ns;
    m->cap = ncap;
    return true;
}

bool model_open(Model *m, const char *abs_path) {
    DIR *dp = opendir(abs_path);
    if (!dp) { m->err = errno; return false; }
    if (m->dp) closedir(m->dp);
    model_clear(m);
    m->dp = dp;
    m->scanning = true;
    m->err = 0;
    strlcpy(m->path, abs_path, sizeof m->path);
    struct stat st;
    m->dir_mtime = stat(abs_path, &st) == 0 ? st.st_mtime : 0;
    m->scanned_at = (int64_t)time(NULL);
    return true;
}

bool model_scan_step(Model *m, int budget_ms) {
    if (!m->scanning) return true;
    uint64_t t0 = ex_now_ms();
    int n = 0;
    for (;;) {
        struct dirent *d = readdir(m->dp);
        if (!d) {
            closedir(m->dp);
            m->dp = NULL;
            m->scanning = false;
            model_refilter(m);
            return true;
        }
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..")) continue;
        if (!grow(m)) continue;
        Entry *e = entry_make(m->path, d->d_name);
        if (!e) continue;
        m->all[m->nall++] = e;
        if (e->hidden) m->nhidden++;
        if (!e->hidden || m->show_hidden) m->shown[m->nshown++] = e;        /* shown in arrival order until the scan ends */
        if ((++n & 7) == 0 && ex_now_ms() - t0 >= (uint64_t)budget_ms) return false;
    }
}

/* ---------------------------------------------------------------- sorting */

static int s_col;
static bool s_desc;

static int cmp_entries(const void *pa, const void *pb) {
    const Entry *a = *(Entry *const *)pa, *b = *(Entry *const *)pb;
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    int r = 0;
    switch (s_col) {
    case COL_SIZE:
        if (!a->is_dir) r = a->size < b->size ? -1 : a->size > b->size ? 1 : 0;
        break;
    case COL_MTIME: r = a->mtime < b->mtime ? -1 : a->mtime > b->mtime ? 1 : 0; break;
    case COL_TYPE: {
        int d = (int)a->ftype - (int)b->ftype;
        r = d ? (d < 0 ? -1 : 1) : strcmp(ext_of(a->key), ext_of(b->key));
        break;
    }
    case COL_PERM: r = (int)(a->mode & 07777) - (int)(b->mode & 07777); break;
    case COL_OWNER: r = strcmp(uid_name(a->uid), uid_name(b->uid)); break;
    default: break;
    }
    if (r == 0) r = natcmp(a->key, b->key);
    if (r == 0) r = strcmp(a->name, b->name);
    return s_desc ? -r : r;
}

void model_sort(Model *m) {
    s_col = m->sort_col;
    s_desc = m->sort_desc;
    if (m->nshown > 1) qsort(m->shown, (size_t)m->nshown, sizeof *m->shown, cmp_entries);
}

void model_refilter(Model *m) {
    m->nshown = 0;
    m->nhidden = 0;
    for (int i = 0; i < m->nall; i++) {
        Entry *e = m->all[i];
        if (e->hidden) {
            m->nhidden++;
            if (!m->show_hidden) { e->sel = false; continue; }
        }
        m->shown[m->nshown++] = e;
    }
    model_sort(m);
}

int model_find(const Model *m, const char *name) {
    for (int i = 0; i < m->nshown; i++) if (!strcmp(m->shown[i]->name, name)) return i;
    return -1;
}

int model_prefix(const Model *m, const char *folded, int from, bool include_from) {
    size_t n = strlen(folded);
    if (m->nshown == 0 || n == 0) return -1;
    int start = from < 0 ? 0 : include_from ? from : from + 1;
    for (int k = 0; k < m->nshown; k++) {
        int i = (start + k) % m->nshown;
        if (!strncmp(m->shown[i]->key, folded, n)) return i;
    }
    return -1;
}

/* ---------------------------------------------------------------- selection */

void model_select_none(Model *m) {
    for (int i = 0; i < m->nall; i++) m->all[i]->sel = false;
}

void model_select_all(Model *m) {
    for (int i = 0; i < m->nshown; i++) m->shown[i]->sel = true;
}

void model_select_range(Model *m, int a, int b, bool on, bool clear_others) {
    if (a > b) { int t = a; a = b; b = t; }
    if (clear_others) model_select_none(m);
    if (a < 0) a = 0;
    if (b >= m->nshown) b = m->nshown - 1;
    for (int i = a; i <= b; i++) m->shown[i]->sel = on;
}

int model_sel_count(const Model *m, int64_t *bytes) {
    int n = 0;
    int64_t total = 0;
    for (int i = 0; i < m->nshown; i++) {
        const Entry *e = m->shown[i];
        if (!e->sel) continue;
        n++;
        if (!e->is_dir) total += e->size;
    }
    if (bytes) *bytes = total;
    return n;
}
