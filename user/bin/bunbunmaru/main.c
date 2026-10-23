/*
 * bunbunmaru: Bunbunmaru News, a gossip paper for the desktop.
 *
 *   bunbunmaru                    the newspaper window
 *   bunbunmaru --date Y-M-D       the issue of another day
 *   bunbunmaru --print            the whole issue as plain text, then exit
 *   bunbunmaru --pool             how many stories, notices and puzzles the paper can draw on
 *   bunbunmaru --trace            log page turns and puzzle moves on stdout
 *
 * Keys: Left/Right or PageUp/PageDown on the page strip flip pages, 1-5 jump to a page, Up/Down/Space scroll.
 */
#include "news.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static Issue g_is;
static Layout g_lay;
static int g_page, g_scroll, g_laid_page = -1, g_laid_w = -1, g_laid_h = -1;
static bool g_bar;
static long g_daynum_open;
static bool g_fixed_date;
static int g_fy, g_fm, g_fd;

static void today(int *y, int *m, int *d) {
    time_t now = time(NULL);
    struct tm t;
    gmtime_r(&now, &t);
    *y = t.tm_year + 1900; *m = t.tm_mon + 1; *d = t.tm_mday;
}

static void open_issue(int y, int m, int d) {
    issue_build(&g_is, y, m, d);
    g_daynum_open = g_is.daynum;
    g_laid_page = -1;
    news_trace("bunbunmaru: issue of %04d-%02d-%02d: %d system stories, %d gossip stories", y, m, d, g_is.nsys, g_is.nfan);
}

static void go_page(int p) {
    if (p < 0) p = 0;
    if (p >= N_PAGES) p = N_PAGES - 1;
    if (p == g_page) return;
    g_page = p;
    g_scroll = 0;
    g_is.puzzle.sel_x = g_is.puzzle.sel_y = -1;
    news_trace("bunbunmaru: page %d %s", p + 1, issue_page_name(p));
}

static void nav_bar(KUI *ui, KRect r) {
    KCanvas *cv = &ui->cv;
    kc_fill(cv, r, kc_shade(ui->theme->window_bg, 94));
    kc_hline(cv, r.x, r.y, r.w, kc_shade(ui->theme->window_bg, 72));
    kc_hline(cv, r.x, r.y + 1, r.w, 0xFFFFFF);
    int x = r.x + 6, y = r.y + 4, h = r.h - 8;
    if (k_button(ui, kr(x, y, 78, h), "< Prev")) go_page(g_page - 1);
    int nx = r.x + r.w - 6 - 78;
    if (k_button(ui, kr(nx, y, 78, h), "Next >")) go_page(g_page + 1);
    int avail = nx - (x + 78) - 16;
    int full = 0;
    for (int i = 0; i < N_PAGES; i++) full += (int)(strlen(issue_page_name(i)) + 3) * 8 + 16;
    bool names = avail >= full + 90;
    int px = x + 78 + 10;
    for (int i = 0; i < N_PAGES; i++) {
        char label[40];
        if (names || i == g_page) snprintf(label, sizeof label, "%d %s", i + 1, issue_page_name(i));
        else snprintf(label, sizeof label, "%d", i + 1);
        int w = (int)strlen(label) * 8 + 16;
        if (px + w > nx - 4) break;
        if (k_button_flat(ui, kr(px, y, w, h), label, i == g_page)) go_page(i);
        px += w + 2;
    }
}

static void keys(KUI *ui, int view_h) {
    Puzzle *pz = &g_is.puzzle;
    for (int i = 0; i < ui->nkeys; i++) {
        KKey *k = &ui->keys[i];
        if (!k->down || !k->code) continue;
        bool used = true;
        if ((k->mods & MOD_CTRL) && (k->code == KEY_Q || k->code == KEY_W)) { g_laid_page = -2; used = false; }
        else if (g_page == 3 && pz->sel_x >= 0 && puzzle_key(pz, k)) { }
        else if (k->mods & (MOD_CTRL | MOD_ALT)) used = false;
        else switch (k->code) {
        case KEY_LEFT: case KEY_PAGEUP: if (k->code == KEY_LEFT || g_scroll == 0) go_page(g_page - 1); else g_scroll -= view_h - 40; break;
        case KEY_RIGHT: case KEY_PAGEDOWN: if (k->code == KEY_RIGHT || g_scroll + view_h >= g_lay.height) go_page(g_page + 1); else g_scroll += view_h - 40; break;
        case KEY_SPACE: if (g_scroll + view_h >= g_lay.height) go_page(g_page + 1); else g_scroll += view_h - 40; break;
        case KEY_HOME: go_page(0); break;
        case KEY_END: go_page(N_PAGES - 1); break;
        case KEY_UP: g_scroll -= 36; break;
        case KEY_DOWN: g_scroll += 36; break;
        case KEY_1: case KEY_2: case KEY_3: case KEY_4: case KEY_5: go_page(k->code - KEY_1); break;
        default: used = false; break;
        }
        if (used) { k->down = 0; k->code = 0; }
    }
}

static void draw(KApp *app, KUI *ui, void *user) {
    (void)user;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    if (!g_fixed_date) {                                                      /* a new day, a new paper */
        int y, m, d;
        today(&y, &m, &d);
        long dn = (long)y * 372 + m * 31 + d;
        (void)dn;
        if (y != g_is.year || m != g_is.month || d != g_is.day) open_issue(y, m, d);
    }
    int nav_h = 32;
    KRect view = kr(0, 0, cv->w, cv->h - nav_h);
    keys(ui, view.h);
    if (g_laid_page == -2) { kapp_quit(app, 0); return; }

    int width = view.w - (g_bar ? 16 : 0);
    if (g_laid_page != g_page || g_laid_w != width || g_laid_h != view.h) {
        pages_build(&g_lay, &g_is, g_page, view.w);
        g_bar = g_lay.height > view.h;
        if (g_bar) pages_build(&g_lay, &g_is, g_page, view.w - 16);
        g_laid_page = g_page;
        g_laid_w = view.w - (g_bar ? 16 : 0);
        g_laid_h = view.h;
    }
    int maxs = g_lay.height > view.h ? g_lay.height - view.h : 0;
    if (k_hover(ui, view) && ui->wheel) g_scroll -= ui->wheel * 54;
    if (g_scroll > maxs) g_scroll = maxs;
    if (g_scroll < 0) g_scroll = 0;

    kc_fill(cv, view, PAGE_BG);
    KRect page = kr(0, 0, view.w - (g_bar ? 16 : 0), view.h);
    KRect old = cv->clip;
    kc_set_clip(cv, kr_intersect(old, page));
    lay_draw(ui, &g_lay, 0, -g_scroll, page);
    cv->clip = old;
    if (g_bar) k_scrollbar(ui, kr(view.w - 16, 0, 16, view.h), &g_scroll, g_lay.height, view.h);
    nav_bar(ui, kr(0, cv->h - nav_h, cv->w, nav_h));
}

static bool on_close(KApp *app, void *user) { (void)app; (void)user; return true; }

int main(int argc, char **argv) {
    bool print = false, pool = false;
    int y, m, d;
    today(&y, &m, &d);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) g_news_trace = true;
        else if (!strcmp(argv[i], "--print")) print = true;
        else if (!strcmp(argv[i], "--pool")) pool = true;
        else if (!strcmp(argv[i], "--date") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31 || y < 1) { fprintf(stderr, "bunbunmaru: not a date: %s\n", argv[i]); return 2; }
            g_fixed_date = true;
            g_fy = y; g_fm = m; g_fd = d;
        } else if (!strcmp(argv[i], "--page") && i + 1 < argc) g_page = atoi(argv[++i]) - 1;
    }
    if (pool) {
        printf("stories %d classifieds %d spellcards %d fortunes %d words %d letters %d\n", g_nfan_news, g_nads, g_ncards, g_nfortunes, g_nwords, g_nletters);
        return 0;
    }
    open_issue(y, m, d);
    if (print) { issue_print(&g_is); return 0; }
    if (g_page < 0 || g_page >= N_PAGES) g_page = 0;
    KApp *app = kapp_new("Bunbunmaru News", 900, 620, FW_RESIZABLE);
    if (!app) return 1;
    kapp_set_draw(app, draw, NULL);
    kapp_set_close_handler(app, on_close, NULL);
    kapp_set_minsize(app, 420, 320);
    kapp_set_tick(app, 30000);                                               /* only to notice midnight */
    news_trace("bunbunmaru: ready");
    int rc = kapp_run(app);
    kapp_destroy(app);
    return rc;
}
