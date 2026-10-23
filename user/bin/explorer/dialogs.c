/* Dialogs of the Explorer. Every one is a window of the main KApp (kwin_new) that calls back when the user decides, so the file
 * window never freezes. While a modal one is open the file window ignores input (X.modal). Closed dialogs are freed a frame later,
 * because the window server may still deliver one more event to a window that was just closed.
 *
 * The progress and conflict windows follow the front job of the queue: they look it up through jobs_front() and its serial number
 * on every frame, never through a pointer they kept, because the job is freed the moment it finishes. */
#include "explorer.h"
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statfs.h>
#include <unistd.h>

typedef struct Dlg {
    struct Dlg *next;
    KWin *win;
    bool closed, modal;
    uint32_t closed_iter;
    void (*cancel)(struct Dlg *);       /* the window server asked the window to close */
    void (*destroy)(struct Dlg *);      /* frees what the dialog owns, just before the dialog itself is freed */
} Dlg;

static Dlg *g_dlgs;

void dlg_modal_enter(void) { X.modal++; }
void dlg_modal_leave(void) { if (X.modal > 0) X.modal--; }

static void dlg_close(Dlg *d) {
    if (d->closed) return;
    d->closed = true;
    d->closed_iter = X.iter;
    kwin_close(d->win);
    if (d->modal) dlg_modal_leave();
}

static void dlg_closed_by_server(KWin *w, void *user) {
    (void)w;
    Dlg *d = user;
    if (d->closed) return;
    if (d->cancel) d->cancel(d);
    else dlg_close(d);
}

void dlg_gc(void) {
    for (Dlg **pp = &g_dlgs; *pp;) {
        Dlg *d = *pp;
        if (d->closed && X.iter > d->closed_iter + 1) {
            *pp = d->next;
            if (d->destroy) d->destroy(d);
            free(d);
        }
        else pp = &d->next;
    }
}

void dlg_close_all(void) {
    for (Dlg *d = g_dlgs; d; d = d->next) if (!d->closed) dlg_close(d);
}

/* Registers a dialog that was calloc'ed by the caller (its first member is the Dlg). Returns false (and frees it) on failure. */
static bool dlg_open(Dlg *d, const char *title, int w, int h, KDrawFn draw, bool modal) {
    d->modal = modal;
    d->win = kwin_new(X.app, -1, -1, w, h, title, FW_DIALOG | FW_CENTER | FW_TOP, draw, d);
    if (!d->win) { free(d); return false; }
    kwin_set_close_handler(d->win, dlg_closed_by_server, d);
    d->next = g_dlgs;
    g_dlgs = d;
    if (modal) dlg_modal_enter();
    ex_trace("dialog %s", title);
    return true;
}

/* word-wrapped text */
static int wrap_lines(const char *msg, int maxcols, int *starts, int *lens, int cap) {
    int n = 0;
    const char *p = msg;
    if (maxcols < 8) maxcols = 8;
    while (*p && n < cap) {
        int cols = 0, last_space = -1;
        const char *q = p;
        while (*q && *q != '\n' && cols < maxcols) { if (*q == ' ') last_space = (int)(q - p); q++; cols++; }
        size_t len = (size_t)(q - p);
        if (*q && *q != '\n' && last_space > 0) len = (size_t)last_space;
        if (starts) starts[n] = (int)(p - msg);
        if (lens) lens[n] = (int)len;
        n++;
        p += len;
        if (*p == ' ' || *p == '\n') p++;
    }
    return n ? n : 1;
}

static int draw_wrapped(KCanvas *cv, int x, int y, const char *msg, int maxcols, uint32_t color, int ymax) {
    int st[40], ln[40];
    int n = wrap_lines(msg, maxcols, st, ln, 40);
    for (int i = 0; i < n && y < ymax; i++, y += 18) kc_text_n(cv, x, y, msg + st[i], (size_t)ln[i], color);
    return y;
}

static void fmt_bytes_exact(char *out, size_t cap, int64_t n) {
    char big[40], digits[32];
    fmt_size(big, sizeof big, n);
    snprintf(digits, sizeof digits, "%lld", (long long)n);
    char grouped[48];
    int len = (int)strlen(digits), o = 0;
    for (int i = 0; i < len; i++) {
        if (i && (len - i) % 3 == 0) grouped[o++] = ',';
        grouped[o++] = digits[i];
    }
    grouped[o] = 0;
    if (n < 1024) snprintf(out, cap, "%s", big);
    else snprintf(out, cap, "%s (%s bytes)", big, grouped);
}

/* ---------------------------------------------------------------- message / choice */

typedef struct {
    Dlg b;
    ChoiceCb cb;
    void *user;
    char msg[900];
    KIcon icon;
    char labels[4][40];
    int n, cancel_idx;
    bool done;
} Choice;

#define CH_W 480
#define CH_TX 66

static void choice_finish(Choice *c, int which) {
    if (c->done) return;
    c->done = true;
    ChoiceCb cb = c->cb;
    void *user = c->user;
    dlg_close(&c->b);
    if (cb) cb(which, user);
}

static void choice_cancel(Dlg *d) { Choice *c = (Choice *)d; choice_finish(c, c->cancel_idx); }

static void choice_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    Choice *c = user;
    if (c->b.closed) return;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    k_icon(cv, c->icon, 18, 18, 32);
    draw_wrapped(cv, CH_TX, 18, c->msg, (cv->w - CH_TX - 16) / 8, ui->theme->text, cv->h - 50);
    int total = 0, bw[4];
    for (int i = 0; i < c->n; i++) { bw[i] = kc_text_width(c->labels[i]) + 28; if (bw[i] < 84) bw[i] = 84; total += bw[i] + (i ? 8 : 0); }
    int x = cv->w - 16 - total, by = cv->h - 40;
    for (int i = 0; i < c->n; i++) {
        KRect br = kr(x, by, bw[i], 26);
        if (k_button(ui, br, c->labels[i])) { choice_finish(c, i); return; }
        if (i == 0) kc_outline(cv, kr_inset(br, 3), ui->theme->accent);
        x += bw[i] + 8;
    }
    if (k_key_pressed(ui, KEY_ESC)) { choice_finish(c, c->cancel_idx); return; }
    if (k_key_pressed(ui, KEY_ENTER) || k_key_pressed(ui, KEY_KPENTER)) choice_finish(c, 0);
}

void dlg_choice(const char *title, const char *msg, KIcon icon, const char *const *labels, int n, int cancel, ChoiceCb cb, void *user) {
    Choice *c = calloc(1, sizeof *c);
    if (!c) { if (cb) cb(cancel, user); return; }
    c->cb = cb;
    c->user = user;
    c->icon = icon;
    c->n = n > 4 ? 4 : n;
    c->cancel_idx = cancel;
    c->b.cancel = choice_cancel;
    snprintf(c->msg, sizeof c->msg, "%s", msg);
    for (int i = 0; i < c->n; i++) snprintf(c->labels[i], sizeof c->labels[i], "%s", labels[i]);
    int lines = wrap_lines(c->msg, (CH_W - CH_TX - 16) / 8, NULL, NULL, 40);
    int h = 92 + (lines < 2 ? 2 : lines) * 18;
    if (!dlg_open(&c->b, title, CH_W, h, choice_draw, true)) { if (cb) cb(cancel, user); }
}

void dlg_message(const char *title, const char *msg, KIcon icon) {
    static const char *const ok[] = { "OK" };
    dlg_choice(title, msg, icon, ok, 1, 0, NULL, NULL);
}

/* ---------------------------------------------------------------- the list of errors at the end of a job */

typedef struct {
    Dlg b;
    JobErr *errs;
    int n, skipped, scroll;
    char intro[300];
} ErrDlg;

static void errs_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    ErrDlg *d = user;
    if (d->b.closed) return;
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    k_clear(ui);
    k_icon(cv, KI_WARNING, 16, 14, 32);
    int y = draw_wrapped(cv, 60, 14, d->intro, (cv->w - 76) / 8, th->text, 80);
    if (y < 62) y = 62;
    KRect lr = kr(12, y, cv->w - 24, cv->h - y - 52);
    kc_panel(cv, lr, KB_SUNKEN, th->field_bg);
    KRect in = kr_inset(lr, 2);
    const int rh = 38;
    int content = d->n * rh, view = in.h;
    bool bar = content > view;
    KRect body = bar ? kr(in.x, in.y, in.w - L_SB_W, in.h) : in;
    if (kr_contains(lr, ui->mx, ui->my) && ui->wheel) d->scroll -= ui->wheel * rh;
    int maxs = content > view ? content - view : 0;
    if (d->scroll > maxs) d->scroll = maxs;
    if (d->scroll < 0) d->scroll = 0;
    if (bar) k_scrollbar(ui, kr(in.x + in.w - L_SB_W, in.y, L_SB_W, in.h), &d->scroll, content, view);
    KRect old = cv->clip;
    kc_set_clip(cv, body);
    for (int i = d->scroll / rh; i < d->n; i++) {
        int ry = body.y + i * rh - d->scroll;
        if (ry >= body.y + body.h) break;
        if (i & 1) kc_fill(cv, kr(body.x, ry, body.w, rh), kc_shade(th->field_bg, 96));
        char p[EX_PATH];
        strlcpy(p, d->errs[i].path ? d->errs[i].path : "?", sizeof p);
        int maxc = (body.w - 12) / 8;
        int len = (int)kc_utf8_len(p);
        if (len > maxc) {                                          /* keep the end of a long path */
            size_t off = kc_utf8_offset(p, (size_t)(len - maxc + 3));
            char tmp[EX_PATH];
            snprintf(tmp, sizeof tmp, "...%s", p + off);
            strlcpy(p, tmp, sizeof p);
        }
        kc_text(cv, body.x + 6, ry + 3, p, th->text);
        kc_text_in(cv, kr(body.x + 6, ry + 19, body.w - 12, 16), d->errs[i].msg, th->accent_dark, KA_LEFT);
    }
    cv->clip = old;
    if (k_button(ui, kr(cv->w - 100, cv->h - 40, 84, 26), "OK") || k_key_pressed(ui, KEY_ESC) || k_key_pressed(ui, KEY_ENTER)) dlg_close(&d->b);
}

static void errs_destroy(Dlg *dd) {
    ErrDlg *d = (ErrDlg *)dd;
    for (int i = 0; i < d->n; i++) free(d->errs[i].path);
    free(d->errs);
}

void dlg_errors(const char *title, const char *intro, const JobErr *errs, int n, int nskipped) {
    ErrDlg *d = calloc(1, sizeof *d);
    if (!d) return;
    d->errs = calloc((size_t)(n ? n : 1), sizeof *d->errs);
    if (!d->errs) { free(d); return; }
    d->b.destroy = errs_destroy;
    for (int i = 0; i < n; i++) {
        d->errs[i].path = strdup(errs[i].path ? errs[i].path : "?");
        memcpy(d->errs[i].msg, errs[i].msg, sizeof d->errs[i].msg);
    }
    d->n = n;
    d->skipped = nskipped;
    snprintf(d->intro, sizeof d->intro, "%s", intro);
    JobErr *list = d->errs;
    if (!dlg_open(&d->b, title, 580, 380, errs_draw, true)) {              /* dlg_open freed d, but the list was ours */
        for (int i = 0; i < n; i++) free(list[i].path);
        free(list);
    }
}

/* ---------------------------------------------------------------- progress */

typedef struct {
    Dlg b;
    uint32_t serial;
} ProgDlg;

typedef struct {
    Dlg b;
    uint32_t serial;
    bool all, rep_ok;
} ConfDlg;

static struct { uint32_t serial; ProgDlg *prog; ConfDlg *conf; } JU;

static void prog_cancel(Dlg *d) {
    ProgDlg *p = (ProgDlg *)d;
    Job *j = jobs_front();
    if (j && job_serial(j) == p->serial) job_cancel(j);
    dlg_close(d);
    JU.prog = NULL;
}

static void prog_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    ProgDlg *d = user;
    if (d->b.closed) return;
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    k_clear(ui);
    Job *j = jobs_front();
    if (!j || job_serial(j) != d->serial) { dlg_close(&d->b); JU.prog = NULL; return; }
    JobInfo ji;
    job_info(j, &ji);
    k_icon(cv, ji.kind == OP_TRASH || ji.kind == OP_DELETE ? KI_TRASH : KI_FOLDER, 16, 16, 32);
    char line[300];
    snprintf(line, sizeof line, "%s %d item%s", ji.title, ji.nsrc, ji.nsrc == 1 ? "" : "s");
    kc_text_bold(cv, 60, 14, line, th->accent_dark);
    if (ji.kind == OP_COPY || ji.kind == OP_MOVE) {
        snprintf(line, sizeof line, "to %s", job_dest(j));
        kc_text_in(cv, kr(60, 32, cv->w - 76, 18), line, th->text_dim, KA_LEFT);
    }
    snprintf(line, sizeof line, "Now: %s", ji.current);
    kc_text_in(cv, kr(60, 52, cv->w - 76, 18), line, th->text, KA_LEFT);
    int pct = ji.nsrc ? (int)(((int64_t)ji.items_done * 100 + (ji.cur_size > 0 ? ji.cur_done * 100 / ji.cur_size : 0)) / ji.nsrc) : 0;
    k_progress(ui, kr(16, 82, cv->w - 32, 20), pct);
    char sz[40];
    fmt_size(sz, sizeof sz, ji.bytes_done);
    snprintf(line, sizeof line, "%d of %d done, %lld files, %s copied", ji.items_done, ji.nsrc, (long long)ji.files, sz);
    if (ji.kind == OP_DELETE || ji.kind == OP_TRASH) snprintf(line, sizeof line, "%d of %d done, %lld files", ji.items_done, ji.nsrc, (long long)ji.files);
    kc_text_in(cv, kr(16, 106, cv->w - 32, 18), line, th->text_dim, KA_LEFT);
    if (k_button(ui, kr(cv->w - 100, cv->h - 38, 84, 26), ji.cancelling ? "Stopping" : "Cancel") || k_key_pressed(ui, KEY_ESC)) {
        job_cancel(j);
        ex_trace("job cancel requested");
    }
    ui->redraw_soon = true;
}

static void open_progress(uint32_t serial) {
    ProgDlg *d = calloc(1, sizeof *d);
    if (!d) return;
    d->serial = serial;
    d->b.cancel = prog_cancel;
    if (dlg_open(&d->b, "File operation", 460, 176, prog_draw, false)) JU.prog = d;
}

/* ---------------------------------------------------------------- conflicts */

static void conf_answer(ConfDlg *d, ConflictPolicy p) {
    Job *j = jobs_front();
    if (j && job_serial(j) == d->serial) job_resolve(j, p, d->all);
    ex_trace("conflict answer %d all=%d", (int)p, d->all);
    dlg_close(&d->b);
    JU.conf = NULL;
}

static void conf_cancel(Dlg *dd) {
    ConfDlg *d = (ConfDlg *)dd;
    Job *j = jobs_front();
    if (j && job_serial(j) == d->serial) job_cancel(j);
    ex_trace("conflict cancelled");
    dlg_close(dd);
    JU.conf = NULL;
}

static void conf_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    ConfDlg *d = user;
    if (d->b.closed) return;
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    k_clear(ui);
    Job *j = jobs_front();
    if (!j || job_serial(j) != d->serial) { dlg_close(&d->b); JU.conf = NULL; return; }
    JobInfo ji;
    job_info(j, &ji);
    if (!ji.waiting) return;
    k_icon(cv, KI_QUESTION, 16, 16, 32);
    char msg[EX_PATH + 160];
    snprintf(msg, sizeof msg, "\"%s\" already exists here. What should happen to the one being %s?", path_base(ji.c_dst),
             ji.kind == OP_MOVE || ji.kind == OP_TRASH || ji.kind == OP_RESTORE ? "moved" : "copied");
    int y = draw_wrapped(cv, 62, 14, msg, (cv->w - 78) / 8, th->text, 80);
    char sz[40], tm[40], line[300];
    int ly = y + 6;
    fmt_size(sz, sizeof sz, ji.c_dst_size);
    fmt_time(tm, sizeof tm, ji.c_dst_mtime);
    snprintf(line, sizeof line, "In the way:  %s, %s, changed %s", ji.c_dst_dir ? "a folder" : "a file", ji.c_dst_dir ? "-" : sz, tm);
    kc_text_in(cv, kr(62, ly, cv->w - 78, 18), line, th->text_dim, KA_LEFT);
    fmt_size(sz, sizeof sz, ji.c_src_size);
    fmt_time(tm, sizeof tm, ji.c_src_mtime);
    snprintf(line, sizeof line, "Arriving:   %s, %s, changed %s", ji.c_src_dir ? "a folder" : "a file", ji.c_src_dir ? "-" : sz, tm);
    kc_text_in(cv, kr(62, ly + 20, cv->w - 78, 18), line, th->text_dim, KA_LEFT);
    k_checkbox(ui, 62, cv->h - 78, "Do this for all the other conflicts", &d->all);
    bool can_replace = ji.c_src_dir == ji.c_dst_dir;
    int bw = 92, by = cv->h - 40, x = cv->w - 16 - (4 * bw + 3 * 8);
    if (k_button(ui, kr(x, by, bw, 26), "Keep both")) { conf_answer(d, CF_KEEP); return; }
    kc_outline(cv, kr_inset(kr(x, by, bw, 26), 3), th->accent);
    x += bw + 8;
    if (can_replace) { if (k_button(ui, kr(x, by, bw, 26), "Replace")) { conf_answer(d, CF_REPLACE); return; } }
    else k_button(ui, kr(x, by, bw, 26), "(can't)");
    x += bw + 8;
    if (k_button(ui, kr(x, by, bw, 26), "Skip")) { conf_answer(d, CF_SKIP); return; }
    x += bw + 8;
    if (k_button(ui, kr(x, by, bw, 26), "Cancel") || k_key_pressed(ui, KEY_ESC)) { conf_cancel(&d->b); return; }
    if (k_key_pressed(ui, KEY_ENTER)) conf_answer(d, CF_KEEP);
}

static void open_conflict(uint32_t serial) {
    ConfDlg *d = calloc(1, sizeof *d);
    if (!d) return;
    d->serial = serial;
    d->b.cancel = conf_cancel;
    if (dlg_open(&d->b, "Name already taken", 520, 230, conf_draw, true)) JU.conf = d;
}

void dlg_jobs_update(void) {
    Job *j = jobs_front();
    if (!j) {
        if (JU.prog) { dlg_close(&JU.prog->b); JU.prog = NULL; }
        if (JU.conf) { dlg_close(&JU.conf->b); JU.conf = NULL; }
        JU.serial = 0;
        return;
    }
    uint32_t s = job_serial(j);
    if (s != JU.serial) {
        if (JU.prog) { dlg_close(&JU.prog->b); JU.prog = NULL; }
        if (JU.conf) { dlg_close(&JU.conf->b); JU.conf = NULL; }
        JU.serial = s;
    }
    JobInfo ji;
    job_info(j, &ji);
    if (ji.waiting && !JU.conf) open_conflict(s);
    if (!ji.waiting && JU.conf) { dlg_close(&JU.conf->b); JU.conf = NULL; }
    if (!JU.prog && !ji.waiting && ji.elapsed_ms > 400 && ji.kind != OP_SIZE) open_progress(s);
}

/* ---------------------------------------------------------------- properties */

#define PR_MAX 64

typedef struct {
    Dlg b;
    char *paths[PR_MAX];
    int n;
    char name[EX_NAME];
    char type[80], where[EX_PATH], link[EX_PATH], orig[EX_PATH], volume[200];
    int64_t size, mtime, atime, ctime;
    uint32_t mode0, mode;
    bool perm[9], perm0[9];
    char owner[40], group[40], owner0[40], group0[40];
    bool is_dir, can_chown, multi, from_trash;
    KIcon icon;
    Job *measure;
    int64_t files, dirs, total;
    bool measured, sized;
    char err[160];
} PropDlg;

static void props_free_job(PropDlg *d) {
    if (d->measure) { job_free(d->measure); d->measure = NULL; }
}

static void props_close(PropDlg *d) {
    props_free_job(d);
    for (int i = 0; i < d->n; i++) { free(d->paths[i]); d->paths[i] = NULL; }
    dlg_close(&d->b);
}

static void props_cancel(Dlg *dd) { props_close((PropDlg *)dd); }

static bool props_apply(PropDlg *d) {
    uint32_t bits = 0;
    for (int i = 0; i < 9; i++) if (d->perm[i]) bits |= 0400u >> i;
    bool mode_changed = false;
    for (int i = 0; i < 9; i++) if (d->perm[i] != d->perm0[i]) mode_changed = true;
    bool own_changed = strcmp(d->owner, d->owner0) || strcmp(d->group, d->group0);
    char errs[300];
    errs[0] = 0;
    int failed = 0;
    uint32_t uid = (uint32_t)-1, gid = (uint32_t)-1;
    if (own_changed && d->can_chown) {
        struct passwd *pw = getpwnam(d->owner);
        struct group *gr = getgrnam(d->group);
        if (strcmp(d->owner, d->owner0)) {
            if (pw) uid = pw->pw_uid;
            else if (d->owner[0] >= '0' && d->owner[0] <= '9') uid = (uint32_t)atoi(d->owner);
            else { snprintf(d->err, sizeof d->err, "There is no user called \"%s\".", d->owner); return false; }
        }
        if (strcmp(d->group, d->group0)) {
            if (gr) gid = gr->gr_gid;
            else if (d->group[0] >= '0' && d->group[0] <= '9') gid = (uint32_t)atoi(d->group);
            else { snprintf(d->err, sizeof d->err, "There is no group called \"%s\".", d->group); return false; }
        }
    }
    for (int i = 0; i < d->n; i++) {
        if (mode_changed) {
            struct stat st;
            if (stat(d->paths[i], &st) == 0) {
                uint32_t nm = (st.st_mode & 07000) | bits;
                if (d->n > 1) {                                         /* several items: only change the bits that were touched */
                    nm = st.st_mode & 07777;
                    for (int b = 0; b < 9; b++) if (d->perm[b] != d->perm0[b]) { if (d->perm[b]) nm |= 0400u >> b; else nm &= ~(0400u >> b); }
                }
                if (chmod(d->paths[i], nm) != 0) { failed++; snprintf(errs, sizeof errs, "%s: %s", path_base(d->paths[i]), strerror(errno)); }
            }
        }
        if (own_changed && d->can_chown && (uid != (uint32_t)-1 || gid != (uint32_t)-1)) {
            if (chown(d->paths[i], uid, gid) != 0) { failed++; snprintf(errs, sizeof errs, "%s: %s", path_base(d->paths[i]), strerror(errno)); }
        }
    }
    if (failed) { snprintf(d->err, sizeof d->err, "Could not change %d item%s. %s", failed, failed == 1 ? "" : "s", errs); return false; }
    d->err[0] = 0;
    memcpy(d->perm0, d->perm, sizeof d->perm0);
    strlcpy(d->owner0, d->owner, sizeof d->owner0);
    strlcpy(d->group0, d->group, sizeof d->group0);
    ex_trace("properties applied mode=%o", bits);
    ex_refresh(NULL);
    return true;
}

static void prop_row(KUI *ui, int y, const char *label, const char *value) {
    KCanvas *cv = &ui->cv;
    kc_text(cv, 16, y, label, ui->theme->text_dim);
    kc_text_in(cv, kr(112, y - 2, cv->w - 128, 20), value, ui->theme->text, KA_LEFT);
}

static void props_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    PropDlg *d = user;
    if (d->b.closed) return;
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    k_clear(ui);
    if (d->measure && !d->measured) {
        if (job_run(d->measure, 12)) {
            JobInfo ji;
            job_info(d->measure, &ji);
            d->files = ji.files;
            d->dirs = ji.dirs;
            d->total = ji.size_total;
            d->measured = true;
            props_free_job(d);
        }
        ui->redraw_soon = true;
        kwin_redraw(d->b.win);
    }
    k_icon(cv, d->icon, 16, 14, 32);
    kc_text_in(cv, kr(58, 14, cv->w - 74, 20), d->name, th->accent_dark, KA_LEFT);
    kc_text_in(cv, kr(58, 32, cv->w - 74, 16), d->type, th->text_dim, KA_LEFT);
    k_separator(ui, kr(12, 56, cv->w - 24, 2));
    int y = 66;
    prop_row(ui, y, "Location:", d->where);
    y += 22;
    char buf[300];
    if (d->is_dir || d->multi) {
        if (!d->measured) {
            JobInfo mi;
            memset(&mi, 0, sizeof mi);
            if (d->measure) job_info(d->measure, &mi);
            snprintf(buf, sizeof buf, "Measuring... %lld files so far", (long long)mi.files);
        } else {
            char sz[60];
            fmt_bytes_exact(sz, sizeof sz, d->total);
            snprintf(buf, sizeof buf, "%s in %lld file%s, %lld folder%s", sz, (long long)d->files, d->files == 1 ? "" : "s", (long long)d->dirs,
                     d->dirs == 1 ? "" : "s");
        }
    } else fmt_bytes_exact(buf, sizeof buf, d->size);
    prop_row(ui, y, "Size:", buf);
    y += 22;
    if (!d->multi) {
        fmt_time_long(buf, sizeof buf, d->mtime);
        prop_row(ui, y, "Modified:", buf);
        y += 22;
        fmt_time_long(buf, sizeof buf, d->atime);
        prop_row(ui, y, "Accessed:", buf);
        y += 22;
        fmt_time_long(buf, sizeof buf, d->ctime);
        prop_row(ui, y, "Changed:", buf);
        y += 22;
    }
    if (d->link[0]) { prop_row(ui, y, "Link to:", d->link); y += 22; }
    if (d->orig[0]) { prop_row(ui, y, "Came from:", d->orig); y += 22; }
    if (d->volume[0]) { prop_row(ui, y, "Volume:", d->volume); y += 22; }
    y += 4;
    k_separator(ui, kr(12, y, cv->w - 24, 2));
    y += 10;
    kc_text_bold(cv, 16, y, "Permissions", th->accent_dark);
    kc_text(cv, 150, y, "Read", th->text);
    kc_text(cv, 214, y, "Write", th->text);
    kc_text(cv, 278, y, "Execute", th->text);
    y += 22;
    static const char *const who[3] = { "Owner", "Group", "Others" };
    for (int r = 0; r < 3; r++, y += 22) {
        kc_text(cv, 32, y, who[r], th->text);
        for (int c = 0; c < 3; c++) k_checkbox(ui, 150 + c * 64, y, "", &d->perm[r * 3 + c]);
    }
    uint32_t bits = 0;
    for (int i = 0; i < 9; i++) if (d->perm[i]) bits |= 0400u >> i;
    char m[16];
    fmt_mode(m, bits);
    snprintf(buf, sizeof buf, "Mode %04o  (%s)", bits | (d->mode & 07000), m + 1);
    kc_text(cv, 32, y, buf, th->text_dim);
    y += 24;
    kc_text(cv, 16, y + 4, "Owner:", th->text);
    if (d->can_chown) k_textfield(ui, kr(70, y, 130, 24), d->owner, sizeof d->owner);
    else kc_text(cv, 74, y + 4, d->owner, th->text);
    kc_text(cv, 214, y + 4, "Group:", th->text);
    if (d->can_chown) k_textfield(ui, kr(270, y, 130, 24), d->group, sizeof d->group);
    else kc_text(cv, 274, y + 4, d->group, th->text);
    y += 30;
    if (!d->can_chown) kc_text_in(cv, kr(16, y - 4, cv->w - 32, 16), "Only the superuser can give a file to someone else.", th->text_dim, KA_LEFT);
    if (d->err[0]) kc_text_in(cv, kr(16, cv->h - 62, cv->w - 32, 18), d->err, th->accent, KA_LEFT);
    int bw = 84, by = cv->h - 38;
    if (k_button(ui, kr(cv->w - 3 * bw - 28, by, bw, 26), "OK") || k_key_pressed(ui, KEY_ENTER)) { if (props_apply(d)) props_close(d); return; }
    if (k_button(ui, kr(cv->w - 2 * bw - 20, by, bw, 26), "Cancel") || k_key_pressed(ui, KEY_ESC)) { props_close(d); return; }
    if (k_button(ui, kr(cv->w - bw - 12, by, bw, 26), "Apply")) props_apply(d);
}

void dlg_properties(char **paths, int n) {
    if (n <= 0) return;
    PropDlg *d = calloc(1, sizeof *d);
    if (!d) return;
    d->b.cancel = props_cancel;
    d->n = n > PR_MAX ? PR_MAX : n;
    d->multi = n > 1;
    d->can_chown = geteuid() == 0;
    for (int i = 0; i < d->n; i++) d->paths[i] = strdup(paths[i]);
    struct stat ls, st;
    memset(&ls, 0, sizeof ls);
    memset(&st, 0, sizeof st);
    const char *p0 = d->paths[0];
    if (lstat(p0, &ls) != 0) { snprintf(d->name, sizeof d->name, "%s", path_base(p0)); }
    if (stat(p0, &st) != 0) st = ls;
    d->mode0 = d->mode = st.st_mode;
    for (int i = 0; i < 9; i++) d->perm[i] = d->perm0[i] = (st.st_mode & (0400u >> i)) != 0;
    d->size = st.st_size;
    d->mtime = st.st_mtime;
    d->atime = st.st_atime;
    d->ctime = st.st_ctime;
    d->is_dir = S_ISDIR(st.st_mode);
    snprintf(d->owner, sizeof d->owner, "%s", uid_name(st.st_uid));
    snprintf(d->group, sizeof d->group, "%s", gid_name(st.st_gid));
    strlcpy(d->owner0, d->owner, sizeof d->owner0);
    strlcpy(d->group0, d->group, sizeof d->group0);
    path_parent(p0, d->where, sizeof d->where);
    if (d->multi) {
        snprintf(d->name, sizeof d->name, "%d items", n);
        snprintf(d->type, sizeof d->type, "Several things selected together");
        d->icon = KI_FOLDER;
        d->is_dir = true;
    } else {
        Entry *e = entry_make(d->where, path_base(p0));
        if (e) {
            char tb[24];
            snprintf(d->name, sizeof d->name, "%s", e->name);
            snprintf(d->type, sizeof d->type, "%s", type_name(e, tb, sizeof tb));
            d->icon = icon_for(e);
            entry_free(e);
        } else { snprintf(d->name, sizeof d->name, "%s", path_base(p0)); d->icon = KI_FILE; }
        if (S_ISLNK(ls.st_mode)) {
            ssize_t k = readlink(p0, d->link, sizeof d->link - 1);
            if (k >= 0) d->link[k] = 0; else d->link[0] = 0;
        }
        if (!strcmp(d->where, NETHERWORLD)) {
            int64_t when;
            if (trash_read_info(path_base(p0), d->orig, sizeof d->orig, &when)) { }
            else snprintf(d->orig, sizeof d->orig, "unknown (it was put here without a note)");
        }
        struct statfs sf;
        if (statfs(p0, &sf) == 0 && sf.f_blocks) {
            char fr[40];
            fmt_size(fr, sizeof fr, (int64_t)(sf.f_bavail * sf.f_bsize));
            snprintf(d->volume, sizeof d->volume, "%s at %s, %s free", sf.f_fstypename, sf.f_mntonname, fr);
        }
    }
    if (d->is_dir || d->multi) {
        d->measure = job_new(OP_SIZE, NULL);
        if (d->measure) for (int i = 0; i < d->n; i++) job_add(d->measure, d->paths[i]);
    }
    int h = 420 + (d->multi ? -66 : 0) + (d->link[0] ? 22 : 0) + (d->orig[0] ? 22 : 0) + (d->volume[0] ? 22 : 0);
    if (!dlg_open(&d->b, d->multi ? "Properties" : "Properties", 440, h, props_draw, true)) { }
}

/* ---------------------------------------------------------------- help and about */

typedef struct { const char *keys, *what; } Row;
static const Row k_left[] = {
    { "FILES", NULL },
    { "Enter", "Open (folders: go in)" }, { "Backspace", "Up one folder" }, { "Alt+Left / Right", "Back / forward" },
    { "F5", "Read the folder again" }, { "Ctrl+L / F6", "Type an address" }, { "F2", "Rename" },
    { "Delete", "Send to the Netherworld" }, { "Shift+Delete", "Delete for good" }, { "Alt+Enter", "Properties" },
    { "Ctrl+C / X / V", "Copy, cut, paste" }, { "Ctrl+Shift+N", "New folder" }, { "Ctrl+Shift+F", "New file" },
    { "Ctrl+N", "New window" },
    { "SELECTING", NULL },
    { "Click, Ctrl+click", "Select, add or remove" }, { "Shift+click", "Select a range" }, { "Drag on empty space", "Rubber band" },
    { "Ctrl+A", "Select everything" }, { "Type a few letters", "Jump to a name" },
};
static const Row k_right[] = {
    { "VIEW", NULL },
    { "Ctrl+1 / Ctrl+2", "List / icons" }, { "Ctrl+H", "Show hidden files" }, { "Click a heading", "Sort by it, again to reverse" },
    { "THE NETHERWORLD", NULL },
    { "Delete", "Sends things there (a note says where from)" }, { "Restore", "Puts them back where they came from" },
    { "Empty", "Yuyuko eats everything for good" },
    { "DRAGGING", NULL },
    { "Onto a folder", "Move (copy to another volume)" }, { "With Ctrl", "Copy" }, { "With Shift", "Move" },
    { "Onto the Netherworld", "Send it there" },
    { "MENUS", NULL },
    { "Alt+F E V G H, F10", "Open a menu" }, { "Arrows, Enter, Esc", "Use it" }, { "Right click", "Context menu" }, { "F1", "This list" },
};

static void help_column(KUI *ui, int x, int y, const Row *rows, int n, int keyw, int linew) {
    KCanvas *cv = &ui->cv;
    for (int i = 0; i < n; i++) {
        if (!rows[i].what) {
            if (i) y += 6;
            kc_text_bold(cv, x, y, rows[i].keys, ui->theme->accent_dark);
            kc_hline(cv, x, y + 17, linew, kc_shade(ui->theme->window_bg, 80));
            y += 22;
            continue;
        }
        kc_text(cv, x, y, rows[i].keys, ui->theme->accent_dark);
        kc_text(cv, x + keyw, y, rows[i].what, ui->theme->text);
        y += 18;
    }
}

static Dlg *g_help, *g_about;

static void help_cancel(Dlg *d) { dlg_close(d); g_help = NULL; }
static void about_cancel(Dlg *d) { dlg_close(d); g_about = NULL; }

static void help_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    Dlg *d = user;
    if (d->closed) return;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    help_column(ui, 14, 12, k_left, (int)(sizeof k_left / sizeof k_left[0]), 150, 300);
    help_column(ui, 344, 12, k_right, (int)(sizeof k_right / sizeof k_right[0]), 150, 342);
    if (k_button(ui, kr(cv->w - 100, cv->h - 40, 84, 26), "Close") || k_key_pressed(ui, KEY_ESC) || k_key_pressed(ui, KEY_ENTER)) help_cancel(d);
}

void dlg_help(void) {
    if (g_help && !g_help->closed) return;
    Dlg *d = calloc(1, sizeof *d);
    if (!d) return;
    d->cancel = help_cancel;
    g_help = dlg_open(d, "Explorer keyboard shortcuts", 704, 470, help_draw, true) ? d : NULL;
}

static void about_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    Dlg *d = user;
    if (d->closed) return;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    kc_gradient_v(cv, kr(0, 0, cv->w, 96), 0xB5122B, 0x5E0A1C);
    k_icon(cv, KI_FOLDER, 22, 16, 64);
    kc_text_scaled(cv, 104, 24, "Gensokyo Explorer", 0xFFF6E0, 2);
    kc_text(cv, 104, 62, "Version " EX_VERSION " - the file manager of HakureiDE", 0xF2D6D6);
    static const char *const lines[] = {
        "Files are kept in folders. Folders are kept in other",
        "folders. Anything you delete goes to the Netherworld,",
        "where Yuyuko holds it until you restore it or tell her",
        "to eat it for good. Every move you make is a job that",
        "runs in small slices, so the window never stops.",
    };
    for (unsigned i = 0; i < sizeof lines / sizeof lines[0]; i++) kc_text(cv, 24, 112 + (int)i * 18, lines[i], ui->theme->text);
    kc_text(cv, 24, 222, "Part of TouhouOS. An unofficial fan work: Touhou Project", ui->theme->text_dim);
    kc_text(cv, 24, 240, "belongs to ZUN / Team Shanghai Alice.", ui->theme->text_dim);
    if (k_button(ui, kr(cv->w - 100, cv->h - 40, 84, 26), "OK") || k_key_pressed(ui, KEY_ESC) || k_key_pressed(ui, KEY_ENTER)) about_cancel(d);
}

void dlg_about(void) {
    if (g_about && !g_about->closed) return;
    Dlg *d = calloc(1, sizeof *d);
    if (!d) return;
    d->cancel = about_cancel;
    g_about = dlg_open(d, "About Gensokyo Explorer", 520, 300, about_draw, true) ? d : NULL;
}
