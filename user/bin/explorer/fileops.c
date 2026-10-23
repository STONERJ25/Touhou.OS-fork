/* The job engine: copy, move, send to the Netherworld, delete, restore and measure, all as sliced jobs.
 *
 * A Job holds its list of top-level items and an explicit stack of folders being walked, so the work can stop after any step and
 * resume on the next call of job_run(): one step is one directory entry or one 64 KiB chunk of a file. The window calls jobs_step()
 * every frame with a few milliseconds of budget, so it stays responsive however big the job is. Errors do not stop a job: they
 * are collected and reported in one list at the end. A name that is already taken raises a question (the job waits until
 * job_resolve() answers it), unless a policy was chosen for all of them. Nothing here needs a window.
 *
 * Moving tries rename() first; across volumes (EXDEV) or when two folders have to be merged it copies and removes each source
 * item right after its copy succeeded, so a failure leaves the rest where it was. */
#include "explorer.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#define CHUNK (64 * 1024)
#define MAX_ERRS 200

typedef struct Frame {
    char *src, *dst;                    /* folder being walked; its copy (NULL for delete and measure) */
    char **names;
    int n, i;
    uint32_t mode, uid, gid;
    int64_t atime, mtime;
    bool created;                       /* we made dst: set its mode when done */
    bool keep;                          /* something inside was not handled: do not remove src */
} Frame;

struct Job {
    OpKind kind;
    char dest[EX_PATH];
    char **src;
    int nsrc, srccap, isrc;
    Frame *stack;
    int sp, scap;
    /* the top-level item in work */
    bool top_active;
    char top_src[EX_PATH], top_dst[EX_PATH];
    int top_err_mark;
    bool move_copy;                     /* this item is moved by copy-then-remove */
    /* the file being copied */
    int rfd, wfd;
    char *cur_src, *cur_dst;
    int64_t cur_size, cur_done;
    uint32_t cur_mode, cur_uid, cur_gid;
    int64_t cur_atime, cur_mtime;
    bool cur_top;
    char *buf;
    /* progress */
    int items_done, nok, nskipped, restored_home;
    int64_t bytes_done, files, dirs, size_total;
    char current[EX_NAME];
    int64_t trash_room;
    /* conflicts */
    ConflictPolicy policy, once;
    bool waiting;
    char c_src[EX_PATH], c_dst[EX_PATH];
    struct stat c_sst, c_dst_st;
    /* results */
    JobErr *errs;
    int nstored, nerr;
    bool cancel, cancelled, finished;
    uint64_t started_ms;
    uint32_t serial;
    Job *next;
};

typedef enum { RS_GO, RS_SKIP, RS_BLOCK, RS_FAIL } Resolve;

/* ---------------------------------------------------------------- creation, errors */

Job *job_new(OpKind kind, const char *dest_dir) {
    Job *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->kind = kind;
    j->rfd = j->wfd = -1;
    if (dest_dir) strlcpy(j->dest, dest_dir, sizeof j->dest);
    return j;
}

void job_add(Job *j, const char *abs_path) {
    if (j->nsrc == j->srccap) {
        int nc = j->srccap ? j->srccap * 2 : 8;
        char **ns = realloc(j->src, (size_t)nc * sizeof *ns);
        if (!ns) return;
        j->src = ns;
        j->srccap = nc;
    }
    char *d = strdup(abs_path);
    if (d) j->src[j->nsrc++] = d;
}

void job_set_policy(Job *j, ConflictPolicy p) { j->policy = p; }
const char *job_dest(const Job *j) { return j->dest; }
int job_nsrc(const Job *j) { return j->nsrc; }
const char *job_src(const Job *j, int i) { return i >= 0 && i < j->nsrc ? j->src[i] : ""; }
int job_ok_count(const Job *j) { return j->nok; }
uint32_t job_serial(const Job *j) { return j->serial; }
int job_restored_home(const Job *j) { return j->restored_home; }
bool job_cancelled(const Job *j) { return j->cancelled; }

static void err_msg(Job *j, const char *path, const char *msg) {
    j->nerr++;
    if (j->nstored >= MAX_ERRS) return;
    JobErr *ne = realloc(j->errs, (size_t)(j->nstored + 1) * sizeof *ne);
    if (!ne) return;
    j->errs = ne;
    JobErr *e = &j->errs[j->nstored++];
    e->path = strdup(path);
    strlcpy(e->msg, msg, sizeof e->msg);
}

static void err_add(Job *j, const char *path, const char *what, int err) {
    char msg[160];
    snprintf(msg, sizeof msg, "%s: %s", what, strerror(err));
    err_msg(j, path, msg);
    ex_trace("job error %s: %s", path, msg);
}

const JobErr *job_errors(const Job *j, int *n) {
    if (n) *n = j->nstored;
    return j->errs;
}

/* ---------------------------------------------------------------- frames */

static Frame *top_frame(Job *j) { return j->sp > 0 ? &j->stack[j->sp - 1] : NULL; }
static void mark_keep(Job *j) { Frame *f = top_frame(j); if (f) f->keep = true; }

static bool load_names(Job *j, const char *dir, char ***out, int *count) {
    DIR *dp = opendir(dir);
    if (!dp) { err_add(j, dir, "Cannot read the folder", errno); return false; }
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *d;
    while ((d = readdir(dp))) {
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..")) continue;
        if (n == cap) {
            int nc = cap ? cap * 2 : 32;
            char **nn = realloc(names, (size_t)nc * sizeof *nn);
            if (!nn) break;
            names = nn;
            cap = nc;
        }
        names[n] = strdup(d->d_name);
        if (names[n]) n++;
    }
    closedir(dp);
    *out = names;
    *count = n;
    return true;
}

static bool push_frame(Job *j, const char *src, const char *dst, const struct stat *st, bool created) {
    char **names;
    int n;
    if (!load_names(j, src, &names, &n)) return false;
    if (j->sp == j->scap) {
        int nc = j->scap ? j->scap * 2 : 8;
        Frame *ns = realloc(j->stack, (size_t)nc * sizeof *ns);
        if (!ns) { for (int i = 0; i < n; i++) free(names[i]); free(names); return false; }
        j->stack = ns;
        j->scap = nc;
    }
    Frame *f = &j->stack[j->sp++];
    memset(f, 0, sizeof *f);
    f->src = strdup(src);
    f->dst = dst ? strdup(dst) : NULL;
    f->names = names;
    f->n = n;
    if (st) { f->mode = st->st_mode; f->uid = st->st_uid; f->gid = st->st_gid; f->atime = st->st_atime; f->mtime = st->st_mtime; }
    f->created = created;
    return true;
}

static void free_frame(Frame *f) {
    for (int i = 0; i < f->n; i++) free(f->names[i]);
    free(f->names);
    free(f->src);
    free(f->dst);
}

/* ---------------------------------------------------------------- conflicts */

static Resolve resolve_dst(Job *j, const char *src, const struct stat *sst, char *final, size_t cap, bool *merge) {
    *merge = false;
    struct stat dt;
    if (lstat(final, &dt) != 0) return RS_GO;
    bool sdir = S_ISDIR(sst->st_mode), ddir = S_ISDIR(dt.st_mode);
    if (sdir && ddir) { *merge = true; return RS_GO; }
    if (sst->st_dev == dt.st_dev && sst->st_ino == dt.st_ino) { j->nskipped++; return RS_SKIP; }       /* the very same file */
    ConflictPolicy p = j->once != CF_ASK ? j->once : j->policy;
    if (p == CF_ASK) {
        strlcpy(j->c_src, src, sizeof j->c_src);
        strlcpy(j->c_dst, final, sizeof j->c_dst);
        j->c_sst = *sst;
        j->c_dst_st = dt;
        j->waiting = true;
        ex_trace("job conflict %s -> %s", src, final);
        return RS_BLOCK;
    }
    j->once = CF_ASK;
    switch (p) {
    case CF_SKIP:
        j->nskipped++;
        return RS_SKIP;
    case CF_KEEP: {
        char parent[EX_PATH], nn[EX_NAME];
        path_parent(final, parent, sizeof parent);
        unique_name(parent, path_base(final), nn, sizeof nn, "num");
        path_join(final, cap, parent, nn);
        return RS_GO;
    }
    default:
        if (ddir) { err_msg(j, final, "Cannot replace a folder with a file"); return RS_FAIL; }
        if (sdir) { err_msg(j, final, "Cannot replace a file with a folder"); return RS_FAIL; }
        if (unlink(final) != 0) { err_add(j, final, "Cannot replace it", errno); return RS_FAIL; }
        return RS_GO;
    }
}

void job_resolve(Job *j, ConflictPolicy choice, bool apply_to_all) {
    if (!j->waiting) return;
    if (apply_to_all) j->policy = choice;
    else j->once = choice;
    j->waiting = false;
}

/* ---------------------------------------------------------------- copying */

static void close_copy(Job *j, bool remove_partial) {
    if (j->rfd >= 0) close(j->rfd);
    if (j->wfd >= 0) close(j->wfd);
    j->rfd = j->wfd = -1;
    if (remove_partial && j->cur_dst) unlink(j->cur_dst);
    free(j->cur_src);
    free(j->cur_dst);
    j->cur_src = j->cur_dst = NULL;
}

static void fail_copy(Job *j, const char *what, int err) {
    char path[EX_PATH];
    strlcpy(path, j->cur_src ? j->cur_src : "?", sizeof path);
    close_copy(j, true);
    err_add(j, path, what, err);
    mark_keep(j);
}

static void finish_copy(Job *j) {
    char *src = j->cur_src, *dst = j->cur_dst;
    uint32_t mode = j->cur_mode, uid = j->cur_uid, gid = j->cur_gid;
    if (close(j->wfd) != 0) {
        j->wfd = -1;
        fail_copy(j, "Cannot finish writing", errno);
        return;
    }
    j->wfd = -1;
    close(j->rfd);
    j->rfd = -1;
    chmod(dst, mode & 07777);
    if (geteuid() == 0) chown(dst, uid, gid);
    struct utimbuf ub = { (time_t)j->cur_atime, (time_t)j->cur_mtime };
    utime(dst, &ub);                                                    /* the copy keeps the modification time (best effort) */
    j->files++;
    if (j->move_copy && unlink(src) != 0) { err_add(j, src, "Copied, but the original could not be removed", errno); mark_keep(j); }
    j->cur_src = j->cur_dst = NULL;
    free(src);
    free(dst);
}

static void copy_chunk(Job *j) {
    if (!j->buf) { j->buf = malloc(CHUNK); if (!j->buf) { fail_copy(j, "Out of memory", ENOMEM); return; } }
    ssize_t n = read(j->rfd, j->buf, CHUNK);
    if (n < 0) {
        if (errno == EINTR) return;
        fail_copy(j, "Cannot read", errno);
        return;
    }
    if (n == 0) { finish_copy(j); return; }
    ssize_t off = 0;
    while (off < n) {
        ssize_t w = write(j->wfd, j->buf + off, (size_t)(n - off));
        if (w < 0) {
            if (errno == EINTR) continue;
            fail_copy(j, "Cannot write", errno);
            return;
        }
        off += w;
    }
    j->cur_done += n;
    j->bytes_done += n;
}

static void begin_file_copy(Job *j, const char *src, const char *final, const struct stat *st, bool top) {
    if (j->kind == OP_TRASH) {                                          /* the Netherworld lives in memory: do not swallow a big file */
        if (!j->trash_room) {
            struct statfs sf;
            j->trash_room = statfs(NETHERWORLD, &sf) == 0 ? (int64_t)(sf.f_bavail * sf.f_bsize / 2) : 64 * 1024 * 1024;
        }
        if (j->bytes_done + st->st_size > j->trash_room) {
            err_msg(j, src, "Too big for the Netherworld (it lives in memory). Delete it permanently instead");
            mark_keep(j);
            return;
        }
    }
    int rfd = open(src, O_RDONLY);
    if (rfd < 0) { err_add(j, src, "Cannot open", errno); mark_keep(j); return; }
    int wfd = open(final, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (wfd < 0) { err_add(j, final, "Cannot create", errno); close(rfd); mark_keep(j); return; }
    j->rfd = rfd;
    j->wfd = wfd;
    j->cur_src = strdup(src);
    j->cur_dst = strdup(final);
    j->cur_size = st->st_size;
    j->cur_done = 0;
    j->cur_mode = st->st_mode;
    j->cur_uid = st->st_uid;
    j->cur_gid = st->st_gid;
    j->cur_atime = st->st_atime;
    j->cur_mtime = st->st_mtime;
    j->cur_top = top;
}

/* Creates `final` as a copy of src (final is free, or an existing folder when merge). Files start a chunked copy, folders push a
 * frame. When the job moves by copying, finished sources are removed along the way. */
static void copy_into(Job *j, const char *src, const char *final, bool merge, const struct stat *st, bool top) {
    if (S_ISDIR(st->st_mode)) {
        bool created = false;
        if (!merge) {
            if (mkdir(final, 0700 | (st->st_mode & 0077)) != 0) { err_add(j, final, "Cannot create the folder", errno); mark_keep(j); return; }
            created = true;
        }
        j->dirs++;
        if (!push_frame(j, src, final, st, created)) mark_keep(j);
        return;
    }
    if (S_ISLNK(st->st_mode)) {
        char target[EX_PATH];
        ssize_t n = readlink(src, target, sizeof target - 1);
        if (n < 0) { err_add(j, src, "Cannot read the link", errno); mark_keep(j); return; }
        target[n] = 0;
        if (symlink(target, final) != 0) { err_add(j, final, "Cannot create the link", errno); mark_keep(j); return; }
        j->files++;
        if (j->move_copy && unlink(src) != 0) { err_add(j, src, "Copied, but the original could not be removed", errno); mark_keep(j); }
        return;
    }
    if (!S_ISREG(st->st_mode)) {
        err_msg(j, src, "Not copied: devices, pipes and sockets stay where they are");
        mark_keep(j);
        return;
    }
    begin_file_copy(j, src, final, st, top);
}

/* Move: rename when possible, otherwise copy and remove. */
static void move_into(Job *j, const char *src, const char *final, bool merge, const struct stat *st, bool top) {
    if (!merge) {
        if (rename(src, final) == 0) { j->files++; return; }
        if (errno != EXDEV) { err_add(j, src, "Cannot move", errno); return; }
    }
    j->move_copy = true;
    copy_into(j, src, final, merge, st, top);
}

/* ---------------------------------------------------------------- walking: delete and measure */

static void delete_into(Job *j, const char *src, const struct stat *st) {
    if (S_ISDIR(st->st_mode)) {
        j->dirs++;
        if (!push_frame(j, src, NULL, st, false)) mark_keep(j);
        return;
    }
    if (unlink(src) != 0) { err_add(j, src, "Cannot delete", errno); mark_keep(j); return; }
    j->files++;
}

static void measure_into(Job *j, const char *src, const struct stat *st) {
    if (S_ISDIR(st->st_mode)) {
        j->dirs++;
        if (!push_frame(j, src, NULL, st, false)) mark_keep(j);
        return;
    }
    j->files++;
    if (S_ISREG(st->st_mode)) j->size_total += st->st_size;
}

/* ---------------------------------------------------------------- one frame step */

static void frame_end(Job *j) {
    Frame f = j->stack[--j->sp];
    switch (j->kind) {
    case OP_DELETE:
        if (!f.keep) {
            if (rmdir(f.src) != 0) { err_add(j, f.src, "Cannot remove the folder", errno); mark_keep(j); }
        } else mark_keep(j);
        break;
    case OP_SIZE:
        break;
    default:
        if (f.created) {
            chmod(f.dst, f.mode & 07777);
            if (geteuid() == 0) chown(f.dst, f.uid, f.gid);
            struct utimbuf ub = { (time_t)f.atime, (time_t)f.mtime };
            utime(f.dst, &ub);
        }
        if (j->move_copy) {
            if (!f.keep) {
                if (rmdir(f.src) != 0) { err_add(j, f.src, "Copied, but the original folder could not be removed", errno); mark_keep(j); }
            } else mark_keep(j);
        }
        break;
    }
    free_frame(&f);
}

static void frame_step(Job *j) {
    Frame *f = &j->stack[j->sp - 1];
    if (f->i >= f->n) { frame_end(j); return; }
    char src[EX_PATH], dst[EX_PATH];
    path_join(src, sizeof src, f->src, f->names[f->i]);
    struct stat st;
    if (lstat(src, &st) != 0) {
        if (errno != ENOENT) err_add(j, src, "Cannot look at", errno);
        f->i++;
        return;
    }
    switch (j->kind) {
    case OP_DELETE:
        f->i++;
        delete_into(j, src, &st);
        break;
    case OP_SIZE:
        f->i++;
        measure_into(j, src, &st);
        break;
    default: {
        path_join(dst, sizeof dst, f->dst, f->names[f->i]);
        bool merge;
        Resolve r = resolve_dst(j, src, &st, dst, sizeof dst, &merge);
        if (r == RS_BLOCK) return;                                  /* asked: this entry is tried again after the answer */
        f->i++;
        if (r == RS_GO) copy_into(j, src, dst, merge, &st, false);
        else f->keep = true;                                         /* skipped or failed: the source stays */
        break;
    }
    }
}

/* ---------------------------------------------------------------- top-level items */

static bool is_protected(const char *src, const struct stat *st) {
    if (!strcmp(src, "/") || !strcmp(src, NETHERWORLD)) return true;
    char parent[EX_PATH];
    path_parent(src, parent, sizeof parent);
    struct stat ps;
    if (S_ISDIR(st->st_mode) && stat(parent, &ps) == 0 && ps.st_dev != st->st_dev) return true;      /* a mount point */
    return false;
}

static bool ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode);
    char parent[EX_PATH];
    path_parent(path, parent, sizeof parent);
    if (strcmp(parent, path) && !ensure_dir(parent)) return false;
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

/* Is `dst_dir` the folder `src` or somewhere inside it? (comparing real paths) */
static bool into_itself(const char *src, const char *dst_dir) {
    char a[EX_PATH], b[EX_PATH];
    if (!realpath(src, a) || !realpath(dst_dir, b)) return path_inside(dst_dir, src);
    return path_inside(b, a);
}

/* Starts the next top-level item. Returns false when it has to wait for an answer (nothing was consumed). */
static bool begin_top(Job *j) {
    const char *src = j->src[j->isrc];
    struct stat st;
    j->top_err_mark = j->nerr;
    j->top_src[0] = j->top_dst[0] = 0;
    j->move_copy = false;
    j->cur_top = false;
    strlcpy(j->current, path_base(src), sizeof j->current);
    strlcpy(j->top_src, src, sizeof j->top_src);
    if (lstat(src, &st) != 0) {
        err_add(j, src, "Cannot find it", errno);
        j->isrc++;
        j->top_active = true;
        return true;
    }
    if ((j->kind == OP_TRASH || j->kind == OP_DELETE || j->kind == OP_MOVE) && is_protected(src, &st)) {
        err_msg(j, src, "Refused: this holds the system together (a mounted volume, the root or the Netherworld itself)");
        j->isrc++;
        j->top_active = true;
        return true;
    }
    char final[EX_PATH];
    bool merge = false;
    const char *name = path_base(src);
    switch (j->kind) {
    case OP_SIZE:
        j->isrc++;
        j->top_active = true;
        measure_into(j, src, &st);
        return true;
    case OP_DELETE:
        j->isrc++;
        j->top_active = true;
        delete_into(j, src, &st);
        return true;
    case OP_COPY:
    case OP_MOVE: {
        char sparent[EX_PATH];
        path_parent(src, sparent, sizeof sparent);
        bool same_dir = !strcmp(sparent, j->dest);
        if (S_ISDIR(st.st_mode) && !same_dir && into_itself(src, j->dest)) {
            err_msg(j, src, "A folder cannot be copied or moved into itself");
            j->isrc++;
            j->top_active = true;
            return true;
        }
        if (j->kind == OP_MOVE && same_dir) {                          /* already there */
            j->isrc++;
            j->top_active = true;
            return true;
        }
        if (j->kind == OP_COPY && same_dir) {
            char nn[EX_NAME];
            unique_name(j->dest, name, nn, sizeof nn, "copy");
            path_join(final, sizeof final, j->dest, nn);
        } else {
            path_join(final, sizeof final, j->dest, name);
            Resolve r = resolve_dst(j, src, &st, final, sizeof final, &merge);
            if (r == RS_BLOCK) return false;
            if (r != RS_GO) { j->isrc++; j->top_active = true; return true; }
        }
        j->isrc++;
        j->top_active = true;
        strlcpy(j->top_dst, final, sizeof j->top_dst);
        if (j->kind == OP_COPY) copy_into(j, src, final, merge, &st, true);
        else move_into(j, src, final, merge, &st, true);
        return true;
    }
    case OP_TRASH: {
        j->isrc++;
        j->top_active = true;
        if (!trash_ensure()) { err_msg(j, src, "The Netherworld cannot be reached"); return true; }
        trash_unique(name, final, sizeof final);
        strlcpy(j->top_dst, final, sizeof j->top_dst);
        trash_write_info(path_base(final), src);
        move_into(j, src, final, false, &st, true);
        return true;
    }
    case OP_RESTORE: {
        char orig[EX_PATH];
        if (!trash_read_info(name, orig, sizeof orig, NULL)) {
            const char *home = getenv("HOME");
            path_join(orig, sizeof orig, home && *home ? home : "/", name);
            j->restored_home++;
        }
        char parent[EX_PATH];
        path_parent(orig, parent, sizeof parent);
        if (!ensure_dir(parent)) {
            err_add(j, parent, "Cannot recreate the original folder", errno);
            j->isrc++;
            j->top_active = true;
            return true;
        }
        strlcpy(final, orig, sizeof final);
        Resolve r = resolve_dst(j, src, &st, final, sizeof final, &merge);
        if (r == RS_BLOCK) return false;
        j->isrc++;
        j->top_active = true;
        if (r != RS_GO) return true;
        strlcpy(j->top_dst, final, sizeof j->top_dst);
        move_into(j, src, final, merge, &st, true);
        return true;
    }
    }
    return true;
}

static void top_done(Job *j) {
    j->items_done++;
    bool ok = j->nerr == j->top_err_mark;
    if (ok) j->nok++;
    char parent[EX_PATH];
    path_parent(j->top_src, parent, sizeof parent);
    switch (j->kind) {
    case OP_DELETE:
        if (ok && !strcmp(parent, NETHERWORLD)) trash_remove_info(path_base(j->top_src));
        break;
    case OP_TRASH:
        if (j->top_dst[0] && !path_exists(j->top_dst)) trash_remove_info(path_base(j->top_dst));
        break;
    case OP_RESTORE:
        if (ok) trash_remove_info(path_base(j->top_src));
        break;
    default:
        break;
    }
    j->top_active = false;
    j->move_copy = false;
}

/* ---------------------------------------------------------------- running */

static void release(Job *j) {
    while (j->sp > 0) free_frame(&j->stack[--j->sp]);
    free(j->stack);
    j->stack = NULL;
    j->scap = 0;
    free(j->buf);
    j->buf = NULL;
}

static void abort_job(Job *j) {
    if (j->wfd >= 0 || j->rfd >= 0) close_copy(j, true);
    release(j);
    j->waiting = false;
    j->cancelled = true;
    j->finished = true;
}

static void step(Job *j) {
    if (j->wfd >= 0) { copy_chunk(j); return; }
    if (j->sp > 0) { frame_step(j); return; }
    if (j->top_active) { top_done(j); return; }
    if (j->isrc >= j->nsrc) {
        release(j);
        j->finished = true;
        return;
    }
    begin_top(j);
}

bool job_run(Job *j, int budget_ms) {
    if (j->finished) return true;
    uint64_t t0 = ex_now_ms();
    if (!j->started_ms) j->started_ms = t0;
    do {
        if (j->cancel) { abort_job(j); break; }
        if (j->waiting) break;
        step(j);
    } while (!j->finished && ex_now_ms() - t0 < (uint64_t)budget_ms);
    return j->finished;
}

void job_cancel(Job *j) {
    j->cancel = true;
    j->waiting = false;
}

void job_info(const Job *j, JobInfo *o) {
    memset(o, 0, sizeof *o);
    o->kind = j->kind;
    static const char *const titles[] = { "Copying", "Moving", "Sending to the Netherworld", "Deleting", "Restoring", "Measuring" };
    strlcpy(o->title, titles[j->kind], sizeof o->title);
    strlcpy(o->current, j->current, sizeof o->current);
    o->nsrc = j->nsrc;
    o->items_done = j->items_done;
    o->nerr = j->nerr;
    o->nskipped = j->nskipped;
    o->bytes_done = j->bytes_done;
    o->cur_size = j->cur_top ? j->cur_size : 0;
    o->cur_done = j->cur_top ? j->cur_done : 0;
    o->files = j->files;
    o->dirs = j->dirs;
    o->size_total = j->size_total;
    o->waiting = j->waiting;
    o->cancelling = j->cancel;
    o->finished = j->finished;
    o->elapsed_ms = j->started_ms ? ex_now_ms() - j->started_ms : 0;
    if (j->waiting) {
        strlcpy(o->c_src, j->c_src, sizeof o->c_src);
        strlcpy(o->c_dst, j->c_dst, sizeof o->c_dst);
        o->c_src_dir = S_ISDIR(j->c_sst.st_mode);
        o->c_dst_dir = S_ISDIR(j->c_dst_st.st_mode);
        o->c_src_size = j->c_sst.st_size;
        o->c_dst_size = j->c_dst_st.st_size;
        o->c_src_mtime = j->c_sst.st_mtime;
        o->c_dst_mtime = j->c_dst_st.st_mtime;
    }
}

void job_free(Job *j) {
    if (!j) return;
    if (j->wfd >= 0 || j->rfd >= 0) close_copy(j, true);
    release(j);
    for (int i = 0; i < j->nsrc; i++) free(j->src[i]);
    free(j->src);
    for (int i = 0; i < j->nstored; i++) free(j->errs[i].path);
    free(j->errs);
    free(j);
}

/* ---------------------------------------------------------------- the queue */

static Job *g_head, *g_tail;
static int g_njobs;

void jobs_submit(Job *j) {
    static uint32_t serial;
    j->serial = ++serial;
    j->next = NULL;
    if (g_tail) g_tail->next = j;
    else g_head = j;
    g_tail = j;
    g_njobs++;
    ex_trace("job queued kind=%d items=%d", (int)j->kind, j->nsrc);
}

bool jobs_busy(void) { return g_head != NULL; }
Job *jobs_front(void) { return g_head; }
int jobs_count(void) { return g_njobs; }

void jobs_step(int budget_ms) {
    Job *j = g_head;
    if (!j) return;
    if (!job_run(j, budget_ms)) return;
    g_head = j->next;
    if (!g_head) g_tail = NULL;
    g_njobs--;
    ex_trace("job done kind=%d ok=%d of %d errors=%d skipped=%d%s", (int)j->kind, j->nok, j->nsrc, j->nerr, j->nskipped,
             j->cancelled ? " cancelled" : "");
    ex_job_done(j);
    job_free(j);
}
