/*
 * spellmgr: the Spell Manager (task manager). Ctrl+Alt+Del opens it.
 *
 *   spellmgr                 the desktop application (a second start just raises the window that is already open)
 *   spellmgr --new           open another window even if one exists
 *   spellmgr --trace         log what happens (kills, sorting, ended spells) on stdout
 *   spellmgr --dump          print the spell table as text and exit
 *   spellmgr --hardware      print the Hardware page as text and exit
 *   spellmgr --users         print the Users page as text and exit
 *   spellmgr --perf          sample the CPU for a second, print the numbers and exit
 */
#include "spell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

App A;

/* ---------------------------------------------------------------- text modes (for scripts and tests) */

void doc_dump(const Doc *d);

static void dump_doc(void (*refresh)(void), Doc *(*get)(void)) {
    refresh();
    doc_dump(get());
}

static int cli_dump(void) {
    ProcList a = { 0 }, b = { 0 };
    proclist_scan(&a, NULL);
    usleep(300000);
    proclist_scan(&b, &a);
    ProcView v = { 0 };
    procview_build(&v, &b, COL_PID, false, "");
    printf("%5s %-16s %-8s %-9s %5s %8s\n", "PID", "NAME", "OWNER", "STATE", "CPU", "RSS");
    for (int i = 0; i < v.n; i++) {
        const Proc *p = &b.v[v.idx[i]];
        printf("%5d %-16s %-8s %-9s %2d.%d%% %6lukB\n", p->pid, p->name, p->user, state_word(p->state), p->cpu10 / 10, p->cpu10 % 10, p->rss_kb);
    }
    printf("%d spells\n", v.n);
    free(v.idx);
    proclist_free(&a);
    proclist_free(&b);
    return 0;
}

static int cli_perf(void) {
    perf_sample(k_now_ms());
    usleep(1100000);
    perf_sample(k_now_ms());
    printf("cpu %d%% memory %d%% uptime %llu ms\n", perf_cpu_percent(), perf_mem_percent(), perf_uptime_ms());
    return 0;
}

/* ---------------------------------------------------------------- the window */

static bool already_running(void) {
    FairyDisplay *d = fairy_connect("spellmgr");
    if (!d) return false;
    fairy_subscribe_windows(d);
    FairyEvent ev;
    bool found = false;
    uint64_t end = k_now_ms() + 700;
    while (k_now_ms() < end) {
        int r = fairy_next_event(d, &ev, 100);
        if (r < 0) break;
        if (r == 1 && ev.type == FEV_WINDOWS) {
            for (uint32_t i = 0; i < ev.u.windows.count && i < FAIRY_MAX_LIST; i++) {
                const FeWinInfo *w = &ev.u.windows.win[i];
                if (w->pid != getpid() && !strncmp(w->title, "Task Manager", 12)) {
                    fairy_window_activate(d, w->id);
                    found = true;
                    break;
                }
            }
            break;
        }
    }
    if (found) fairy_next_event(d, &ev, 30);                       /* let the request go out before we hang up */
    fairy_disconnect(d);
    return found;
}

static void set_tab(KUI *ui, int tab) {
    if (tab == A.tab || tab < 0 || tab > 3) return;
    A.tab = tab;
    ui->focus = 0;
    static const char *const names[4] = { "spells", "performance", "hardware", "users" };
    trace("spellmgr: tab %s", names[tab]);
    if (tab == 0) A.force_refresh = true;
}

static void draw(KApp *app, KUI *ui, void *user) {
    (void)user;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    uint64_t now = ui->now_ms;

    perf_sample(now);
    if (A.tab == 0 && (A.force_refresh || now - A.last_scan_ms >= 900)) { A.force_refresh = false; spells_refresh(); }

    for (int i = 0; i < ui->nkeys; i++) {
        KKey *k = &ui->keys[i];
        if (!k->down || !k->code) continue;
        if ((k->mods & MOD_CTRL) && k->code >= KEY_1 && k->code <= KEY_4) { set_tab(ui, k->code - KEY_1); k->down = 0; k->code = 0; }
        else if ((k->mods & MOD_CTRL) && (k->code == KEY_Q || k->code == KEY_W)) { kapp_quit(app, 0); return; }
    }
    if (A.tab == 0) spells_keys(ui);

    static const char *const tabs[4] = { "Spells", "Performance", "Hardware", "Users" };
    int sel = A.tab;
    k_tabs(ui, kr(8, 8, cv->w - 16, 26), tabs, 4, &sel);
    set_tab(ui, sel);

    int status_h = 24;
    KRect body = kr(8, 34, cv->w - 16, cv->h - 34 - status_h - 6);
    kc_panel(cv, body, KB_FLAT, ui->theme->window_bg);
    KRect old = cv->clip;
    kc_set_clip(cv, kr_intersect(old, kr_inset(body, 1)));
    KRect inner = kr_inset(body, 2);
    switch (A.tab) {
    case 0: spells_tab(ui, inner); break;
    case 1: perf_tab(ui, inner); break;
    case 2: hardware_tab(ui, inner); break;
    case 3: users_tab(ui, inner); break;
    }
    cv->clip = old;

    /* status bar */
    KRect st = kr(0, cv->h - status_h, cv->w, status_h);
    kc_fill(cv, st, kc_shade(ui->theme->window_bg, 94));
    kc_hline(cv, st.x, st.y, st.w, kc_shade(ui->theme->window_bg, 72));
    kc_hline(cv, st.x, st.y + 1, st.w, 0xFFFFFF);
    char line[160];
    snprintf(line, sizeof line, "%d spells   |   CPU %d%%   |   Memory %d%%", A.procs.n, perf_cpu_percent(), perf_mem_percent());
    kc_text_in(cv, kr(st.x + 8, st.y + 2, st.w - 16, st.h - 2), line, ui->theme->text_dim, KA_LEFT);
    if (ui->tooltip == NULL && k_hover(ui, st)) k_tooltip_text(ui, "The Spell Manager looks at /proc once a second");
}

static bool on_close(KApp *app, void *user) { (void)app; (void)user; return true; }

int main(int argc, char **argv) {
    bool force_new = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) g_trace = true;
        else if (!strcmp(argv[i], "--new")) force_new = true;
        else if (!strcmp(argv[i], "--dump")) return cli_dump();
        else if (!strcmp(argv[i], "--perf")) return cli_perf();
        else if (!strcmp(argv[i], "--hardware")) { dump_doc(hardware_refresh, hardware_doc); return 0; }
        else if (!strcmp(argv[i], "--users")) { dump_doc(users_refresh, users_doc); return 0; }
    }
    if (!force_new && already_running()) return 0;
    KApp *app = kapp_new("Task Manager", 760, 520, FW_RESIZABLE);
    if (!app) return 1;
    A.app = app;
    A.sort_col = COL_PID;
    kapp_set_draw(app, draw, NULL);
    kapp_set_close_handler(app, on_close, NULL);
    kapp_set_minsize(app, 460, 330);
    kapp_set_tick(app, 1000);
    spells_refresh();
    trace("spellmgr: ready with %d spells", A.procs.n);
    int rc = kapp_run(app);
    kapp_destroy(app);
    return rc;
}
