/* The Spells tab: the process table, a filter, End Spell / Exorcise with confirmation and a details pane. */
#include "spell.h"
#include <errno.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_vis_rows;
static char g_last_filter[sizeof A.filter];
static char g_cmd[200], g_cwd[160];
static uint64_t g_detail_ms;
static const char *const k_col_names[COL_COUNT] = { "PID", "Name", "Owner", "State", "CPU", "Memory" };

typedef struct KillReq { int pid; int sig; char name[32]; } KillReq;

/* ---------------------------------------------------------------- refreshing */

static void select_pid(int pid) {
    if (pid != A.selected_pid) trace("spellmgr: selected pid %d", pid);
    A.selected_pid = pid;
    A.selected_gone = false;
    g_detail_ms = 0;
    const Proc *p = pid ? proclist_find(&A.procs, pid) : NULL;
    if (p) { A.selected_copy = *p; trace("spellmgr: selected %s", p->name); }
}

static int row_of_pid(int pid) {
    for (int i = 0; i < A.view.n; i++) if (A.procs.v[A.view.idx[i]].pid == pid) return i;
    return -1;
}

static void load_details(void) {
    g_cmd[0] = g_cwd[0] = 0;
    if (!A.selected_pid || A.selected_gone) return;
    proc_cmdline(A.selected_pid, g_cmd, sizeof g_cmd);
    proc_cwd(A.selected_pid, g_cwd, sizeof g_cwd);
    g_detail_ms = k_now_ms();
}

void spells_refresh(void) {
    ProcList tmp = { 0 };
    proclist_scan(&tmp, &A.procs);                                    /* needs the old list for the CPU deltas */
    proclist_free(&A.prev);
    A.prev = A.procs;
    A.procs = tmp;
    procview_build(&A.view, &A.procs, A.sort_col, A.sort_desc, A.filter);
    if (A.selected_pid) {
        const Proc *p = proclist_find(&A.procs, A.selected_pid);
        if (p) { A.selected_copy = *p; A.selected_gone = false; }
        else if (!A.selected_gone) { A.selected_gone = true; trace("spellmgr: pid %d has ended", A.selected_pid); }
    }
    load_details();
    A.last_scan_ms = k_now_ms();
    snprintf(g_last_filter, sizeof g_last_filter, "%s", A.filter);
}

static void rebuild_view(void) {
    procview_build(&A.view, &A.procs, A.sort_col, A.sort_desc, A.filter);
    snprintf(g_last_filter, sizeof g_last_filter, "%s", A.filter);
}

/* ---------------------------------------------------------------- signals */

static void flash_note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(A.note, sizeof A.note, fmt, ap);
    va_end(ap);
    A.note_until = k_now_ms() + 5000;
}

static void on_kill_answer(bool yes, void *user) {
    KillReq *rq = user;
    if (!yes) trace("spellmgr: signal %d for pid %d cancelled", rq->sig, rq->pid);
    if (yes) {
        if (kill(rq->pid, rq->sig) == 0) {
            trace("spellmgr: sent signal %d to pid %d (%s)", rq->sig, rq->pid, rq->name);
            flash_note("%s sent to '%s' (pid %d).", rq->sig == SIGKILL ? "SIGKILL" : "SIGTERM", rq->name, rq->pid);
            A.force_refresh = true;
        } else {
            int err = errno;
            const char *why = err == EPERM ? "Eiki says you may not touch that spell: it belongs to someone else." :
                              err == ESRCH ? "That spell has already ended." : strerror(err);
            trace("spellmgr: kill %d failed: %s", rq->pid, strerror(err));
            const char *btn[] = { "OK" };
            char msg[200];
            snprintf(msg, sizeof msg, "Could not send the signal to '%s' (pid %d).\n\n%s", rq->name, rq->pid, why);
            k_messagebox(kapp_display(A.app), "Spell Manager", msg, KI_WARNING, btn, 1);
            A.force_refresh = true;
        }
    }
    free(rq);
}

static bool is_core(const Proc *p) {
    static const char *const core[] = { "init", "fairyd", "hakurei-shell", "hakurei-session", "spellmgr" };
    for (unsigned i = 0; i < sizeof core / sizeof core[0]; i++) if (!strcmp(p->name, core[i])) return true;
    return p->pid == 1;
}

static void ask_kill(int sig) {
    if (!A.selected_pid || A.selected_gone) return;
    KillReq *rq = calloc(1, sizeof *rq);
    if (!rq) return;
    rq->pid = A.selected_pid;
    rq->sig = sig;
    snprintf(rq->name, sizeof rq->name, "%s", A.selected_copy.name);
    char msg[320];
    bool core = is_core(&A.selected_copy);
    if (sig == SIGTERM)
        snprintf(msg, sizeof msg, "Ask '%s' (pid %d, owned by %s) to finish up and leave?\n\nThe spell may still tidy up first or refuse.%s",
                 rq->name, rq->pid, A.selected_copy.user, core ? "\n\nWarning: this is part of the desktop itself." : "");
    else
        snprintf(msg, sizeof msg, "Force '%s' (pid %d, owned by %s) out right now?\n\nIt gets no chance to save or tidy up, so unsaved work is lost.%s",
                 rq->name, rq->pid, A.selected_copy.user, core ? "\n\nWarning: this is part of the desktop itself." : "");
    trace("spellmgr: asking to confirm signal %d for pid %d", sig, rq->pid);
    k_confirm_dialog(A.app, sig == SIGTERM ? "End Spell" : "Exorcise", msg, KI_WARNING, sig == SIGTERM ? "End Spell" : "Exorcise", "Cancel", on_kill_answer, rq);
}

/* ---------------------------------------------------------------- keyboard */

static void move_selection(int delta, bool absolute) {
    if (A.view.n == 0) return;
    int row = row_of_pid(A.selected_pid);
    if (absolute) row = delta < 0 ? 0 : A.view.n - 1;
    else row = row < 0 ? (delta > 0 ? 0 : A.view.n - 1) : row + delta;
    if (row < 0) row = 0;
    if (row >= A.view.n) row = A.view.n - 1;
    select_pid(A.procs.v[A.view.idx[row]].pid);
    if (row < A.scroll) A.scroll = row;
    if (g_vis_rows > 0 && row >= A.scroll + g_vis_rows) A.scroll = row - g_vis_rows + 1;
}

void spells_keys(KUI *ui) {
    if (ui->focus != 0) return;                                        /* the filter field has the keyboard */
    for (int i = 0; i < ui->nkeys; i++) {
        KKey *k = &ui->keys[i];
        if (!k->down || !k->code) continue;
        bool ctrl = k->mods & MOD_CTRL, shift = k->mods & MOD_SHIFT;
        bool used = true;
        int page = g_vis_rows > 1 ? g_vis_rows - 1 : 1;
        if (ctrl && k->code == KEY_F) k_focus_textfield(ui, A.filter);
        else if (ctrl) used = false;
        else switch (k->code) {
        case KEY_UP: move_selection(-1, false); break;
        case KEY_DOWN: move_selection(1, false); break;
        case KEY_PAGEUP: move_selection(-page, false); break;
        case KEY_PAGEDOWN: move_selection(page, false); break;
        case KEY_HOME: move_selection(-1, true); break;
        case KEY_END: move_selection(1, true); break;
        case KEY_DELETE: ask_kill(shift ? SIGKILL : SIGTERM); break;
        case KEY_F5: A.force_refresh = true; break;
        case KEY_ESC:
            if (A.filter[0]) { A.filter[0] = 0; rebuild_view(); }
            else select_pid(0);
            break;
        default: used = false; break;
        }
        if (used) { k->down = 0; k->code = 0; }
    }
}

/* ---------------------------------------------------------------- drawing */

static void fmt_cpu(int cpu10, char *out, size_t cap) { snprintf(out, cap, "%d.%d%%", cpu10 / 10, cpu10 % 10); }

static void draw_details(KUI *ui, KRect r) {
    KCanvas *cv = &ui->cv;
    kc_panel(cv, r, KB_ETCHED, ui->theme->window_bg);
    if (!A.selected_pid) {
        kc_text_in(cv, kr(r.x + 12, r.y + 8, r.w - 24, 16), "Select a spell above to see its details.", ui->theme->text_dim, KA_LEFT);
        kc_text_in(cv, kr(r.x + 12, r.y + 28, r.w - 24, 16), "End Spell asks it to leave; Exorcise removes it by force.", ui->theme->text_dim, KA_LEFT);
        return;
    }
    const Proc *p = &A.selected_copy;
    char line[200], a[48], b[48];
    int x = r.x + 12, y = r.y + 6;
    int colw = r.w / 2;
    bool two = r.w >= 560;
    snprintf(line, sizeof line, "%s  (pid %d)", p->name, p->pid);
    kc_text_bold(cv, x, y, line, A.selected_gone ? ui->theme->accent : ui->theme->accent_dark);
    if (A.selected_gone) kc_text_bold(cv, x + (int)strlen(line) * 8 + 16, y, "This spell has ended.", ui->theme->accent);
    y += 20;
    int label_w = 78;
    int rows = 0;
    struct { const char *k; char v[200]; } kv[10];
    snprintf(kv[rows].v, sizeof kv[rows].v, "%s", g_cmd[0] ? g_cmd : p->name); kv[rows].k = "Command"; rows++;
    const Proc *parent = proclist_find(&A.procs, p->ppid);
    snprintf(kv[rows].v, sizeof kv[rows].v, "%s (pid %d)", parent ? parent->name : (p->ppid ? "unknown" : "none"), p->ppid); kv[rows].k = "Parent"; rows++;
    snprintf(kv[rows].v, sizeof kv[rows].v, "%s (uid %u, gid %u)", p->user, p->uid, p->gid); kv[rows].k = "Owner"; rows++;
    {
        uint64_t up_ms = k_now_ms();
        time_t now = time(NULL);
        long age_s = (long)((up_ms > p->start_ms ? up_ms - p->start_ms : 0) / 1000);
        time_t started = now - age_s;
        struct tm t;
        localtime_r(&started, &t);
        fmt_duration((unsigned long long)age_s * 1000, a, sizeof a);
        snprintf(kv[rows].v, sizeof kv[rows].v, "%02d:%02d:%02d %s, %s ago", t.tm_hour, t.tm_min, t.tm_sec, t.tm_zone ? t.tm_zone : "UTC", a);
        kv[rows].k = "Started";
        rows++;
    }
    snprintf(kv[rows].v, sizeof kv[rows].v, "%s", state_word(p->state)); kv[rows].k = "State"; rows++;
    fmt_cpu(p->cpu10, a, sizeof a);
    if (p->ticks < 1000) snprintf(b, sizeof b, "%lu ms", p->ticks);
    else fmt_duration(p->ticks, b, sizeof b);
    snprintf(kv[rows].v, sizeof kv[rows].v, "%s now, %s used so far", a, b); kv[rows].k = "CPU"; rows++;
    fmt_kb(p->rss_kb, a, sizeof a);
    fmt_kb(p->vsz_kb, b, sizeof b);
    snprintf(kv[rows].v, sizeof kv[rows].v, "%s RSS, %s VM", a, b); kv[rows].k = "Memory"; rows++;
    snprintf(kv[rows].v, sizeof kv[rows].v, "%s", g_cwd[0] ? g_cwd : "unknown"); kv[rows].k = "Folder"; rows++;

    int per_col = two ? (rows + 1) / 2 : rows;
    KRect old = cv->clip;
    for (int i = 0; i < rows; i++) {
        int col = two ? i / per_col : 0;
        int row = two ? i % per_col : i;
        int cx = x + col * colw, cy = y + row * 18;
        if (cy + 16 > r.y + r.h - 2) continue;
        kc_set_clip(cv, kr_intersect(old, kr(cx, cy, two ? colw - 14 : r.w - 24, 16)));
        kc_text(cv, cx, cy, kv[i].k, ui->theme->text_dim);
        kc_text_in(cv, kr(cx + label_w, cy, (two ? colw : r.w - 20) - label_w - 16, 16), kv[i].v, ui->theme->text, KA_LEFT);
        cv->clip = old;
    }
    if (parent && !A.selected_gone && r.w >= 360) {
        KRect btn = kr(r.x + r.w - 118, r.y + 4, 110, 20);
        if (k_button(ui, btn, "Go to parent")) select_pid(parent->pid);
    }
}

void spells_tab(KUI *ui, KRect b) {
    KCanvas *cv = &ui->cv;
    KRect old = cv->clip;
    int pad = 8;

    /* toolbar */
    int y = b.y + pad;
    kc_text(cv, b.x + pad + 2, y + 5, "Filter:", ui->theme->text);
    int fw = b.w >= 520 ? 200 : 120;
    k_textfield(ui, kr(b.x + pad + 64, y, fw, 26), A.filter, sizeof A.filter);
    if (strcmp(A.filter, g_last_filter)) { rebuild_view(); A.scroll = 0; }
    int bw = 104;
    bool can = A.selected_pid && !A.selected_gone;
    KRect bk = kr(b.x + b.w - pad - bw, y, bw, 26), be = kr(bk.x - 6 - bw, y, bw, 26);
    if (can) {
        if (k_button(ui, be, "End Spell")) ask_kill(SIGTERM);
        if (k_button(ui, bk, "Exorcise")) ask_kill(SIGKILL);
    } else {
        kc_panel(cv, be, KB_RAISED, kc_shade(ui->theme->button_bg, 96));
        kc_text_in(cv, be, "End Spell", ui->theme->text_dim, KA_CENTER);
        kc_panel(cv, bk, KB_RAISED, kc_shade(ui->theme->button_bg, 96));
        kc_text_in(cv, bk, "Exorcise", ui->theme->text_dim, KA_CENTER);
    }
    if (k_hover(ui, be) && can) k_tooltip_text(ui, "Ask the spell to finish and leave (SIGTERM)");
    if (k_hover(ui, bk) && can) k_tooltip_text(ui, "Remove the spell by force (SIGKILL)");
    y += 34;

    /* layout: table on top, details below */
    int details_h = b.h >= 330 ? 128 : 0;
    KRect table = kr(b.x + pad, y, b.w - pad * 2, b.y + b.h - y - pad - (details_h ? details_h + 6 : 0));
    if (table.h < 60) { details_h = 0; table.h = b.y + b.h - y - pad; }
    if (table.h < 44) return;

    /* which columns fit */
    bool show_user = table.w >= 440, show_state = table.w >= 540;
    int w_pid = 56, w_user = show_user ? 84 : 0, w_state = show_state ? 84 : 0, w_cpu = 64, w_mem = 90;
    int w_name = table.w - 16 - w_pid - w_user - w_state - w_cpu - w_mem;
    if (w_name < 60) { w_name = 60; }
    int xs[COL_COUNT + 1];
    xs[COL_PID] = 0;
    xs[COL_NAME] = w_pid;
    xs[COL_USER] = xs[COL_NAME] + w_name;
    xs[COL_STATE] = xs[COL_USER] + w_user;
    xs[COL_CPU] = xs[COL_STATE] + w_state;
    xs[COL_MEM] = xs[COL_CPU] + w_cpu;
    xs[COL_COUNT] = xs[COL_MEM] + w_mem;

    kc_panel(cv, table, KB_SUNKEN, ui->theme->field_bg);
    KRect in = kr_inset(table, 2);
    const int header_h = 22, row_h = 20;
    KRect head = kr(in.x, in.y, in.w, header_h);
    kc_fill(cv, head, kc_shade(ui->theme->window_bg, 97));
    kc_hline(cv, head.x, head.y + header_h - 1, head.w, kc_shade(ui->theme->window_bg, 70));
    int rows_h = in.h - header_h;
    g_vis_rows = rows_h / row_h;
    bool bar = A.view.n > g_vis_rows;
    int area_w = bar ? in.w - 14 : in.w;
    if (k_hover(ui, table) && ui->wheel) A.scroll -= ui->wheel * 3;
    int maxs = A.view.n > g_vis_rows ? A.view.n - g_vis_rows : 0;
    if (A.scroll > maxs) A.scroll = maxs;
    if (A.scroll < 0) A.scroll = 0;

    /* header cells: click to sort */
    for (int c = 0; c < COL_COUNT; c++) {
        if ((c == COL_USER && !show_user) || (c == COL_STATE && !show_state)) continue;
        KRect cell = kr(in.x + xs[c], in.y, xs[c + 1] - xs[c], header_h);
        if (c == COL_USER && !show_state) cell.w = xs[COL_CPU] - xs[COL_USER];
        bool hover = k_hover(ui, cell);
        if (hover) kc_fill(cv, cell, kc_shade(ui->theme->window_bg, 102));
        bool right = c == COL_CPU || c == COL_MEM || c == COL_PID;
        char label[40];
        snprintf(label, sizeof label, "%s%s", k_col_names[c], A.sort_col == c ? (A.sort_desc ? " \xE2\x96\xBC" : " \xE2\x96\xB2") : "");
        kc_text_in(cv, kr(cell.x + 6, cell.y, cell.w - 12, header_h), label, ui->theme->text, right ? KA_RIGHT : KA_LEFT);
        if (c > 0) kc_vline(cv, cell.x, cell.y + 3, header_h - 6, kc_shade(ui->theme->window_bg, 82));
        if (hover && ui->released[0]) {
            if (A.sort_col == c) A.sort_desc = !A.sort_desc;
            else { A.sort_col = c; A.sort_desc = c == COL_CPU || c == COL_MEM; }
            rebuild_view();
            trace("spellmgr: sorted by %s %s", k_col_names[c], A.sort_desc ? "descending" : "ascending");
        }
    }

    /* rows */
    KRect body = kr(in.x, in.y + header_h, area_w, rows_h);
    kc_set_clip(cv, kr_intersect(old, body));
    if (A.view.n == 0) {
        kc_text_in(cv, kr(body.x + 10, body.y + 8, body.w - 20, 16), A.filter[0] ? "No spell matches the filter." : "No spells are running (that is odd).", ui->theme->text_dim, KA_LEFT);
    }
    for (int i = 0; i <= g_vis_rows; i++) {
        int idx = A.scroll + i;
        if (idx >= A.view.n) break;
        const Proc *p = &A.procs.v[A.view.idx[idx]];
        KRect rr = kr(body.x, body.y + i * row_h, body.w, row_h);
        bool sel = p->pid == A.selected_pid;
        bool hover = k_hover(ui, rr) && k_hover(ui, body);
        uint32_t fg = ui->theme->field_text;
        if (sel) { kc_fill(cv, rr, ui->theme->select_bg); fg = ui->theme->select_text; }
        else if (hover) kc_fill(cv, rr, kc_mix(ui->theme->field_bg, ui->theme->accent_light, 40));
        else if (idx & 1) kc_fill(cv, rr, kc_shade(ui->theme->field_bg, 97));
        if (!sel && p->state == 'Z') fg = ui->theme->text_dim;
        if (hover && ui->pressed[0]) {
            select_pid(p->pid);
            ui->focus = 0;
        }
        char cell[48];
        snprintf(cell, sizeof cell, "%d", p->pid);
        kc_text_in(cv, kr(rr.x + xs[COL_PID] + 4, rr.y, xs[COL_NAME] - xs[COL_PID] - 8, row_h), cell, fg, KA_RIGHT);
        kc_text_in(cv, kr(rr.x + xs[COL_NAME] + 6, rr.y, w_name - 8, row_h), p->name, fg, KA_LEFT);
        if (show_user) kc_text_in(cv, kr(rr.x + xs[COL_USER] + 6, rr.y, w_user - 8, row_h), p->user, fg, KA_LEFT);
        if (show_state) kc_text_in(cv, kr(rr.x + xs[COL_STATE] + 6, rr.y, w_state - 8, row_h), state_word(p->state), fg, KA_LEFT);
        fmt_cpu(p->cpu10, cell, sizeof cell);
        uint32_t cpu_col = fg;
        if (!sel && p->cpu10 >= 500) cpu_col = ui->theme->accent;
        kc_text_in(cv, kr(rr.x + xs[COL_CPU] + 4, rr.y, w_cpu - 8, row_h), cell, cpu_col, KA_RIGHT);
        fmt_kb(p->rss_kb, cell, sizeof cell);
        kc_text_in(cv, kr(rr.x + xs[COL_MEM] + 4, rr.y, w_mem - 8, row_h), cell, fg, KA_RIGHT);
    }
    cv->clip = old;
    if (bar) k_scrollbar(ui, kr(in.x + in.w - 14, in.y + header_h, 14, rows_h), &A.scroll, A.view.n, g_vis_rows);       /* units: rows */

    if (details_h) draw_details(ui, kr(b.x + pad, table.y + table.h + 6, b.w - pad * 2, details_h));
    if (A.note[0] && k_now_ms() < A.note_until) {
        kc_text_in(cv, kr(b.x + pad + 260, b.y + pad + 6, b.w - pad * 2 - 260 - bw * 2 - 20, 16), A.note, ui->theme->accent_dark, KA_LEFT);
        ui->redraw_soon = true;
    }
}
