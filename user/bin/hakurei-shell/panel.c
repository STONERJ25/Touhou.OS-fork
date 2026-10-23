/* The panel: start button, quick launch, task list, system tray, clock; plus the start menu and calendar popups. */
#include "shell.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <gensokyo.h>

#define MENU_W 292
#define BANNER_W 32
#define ITEM_H 30

static const char *const k_months[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };

static bool app_present(const AppEntry *a) {
    char path[96];
    char cmd[64];
    strncpy(cmd, a->command, sizeof cmd - 1);
    cmd[sizeof cmd - 1] = 0;
    char *sp = strchr(cmd, ' ');
    if (sp) *sp = 0;
    snprintf(path, sizeof path, "/bin/%s", cmd);
    return access(path, X_OK) == 0;
}

/* ---------------------------------------------------------------- system sampling for the tray */

void sample_system(void) {
    uint64_t now = k_now_ms();
    if (now - G.last_sample_ms < 1000) return;
    int fd = open("/proc/tasks", O_RDONLY);
    if (fd >= 0) {
        char buf[4096];
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n > 0) {
            buf[n] = 0;
            unsigned long idle = 0;
            for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
                char *t = strstr(line, "ticks=");
                if (t && strstr(line, "idle")) idle = strtoul(t + 6, NULL, 10);
            }
            if (G.last_sample_ms) {
                unsigned long dw = (unsigned long)(now - G.last_sample_ms);
                unsigned long di = idle - G.last_idle;
                int pct = dw ? 100 - (int)(di * 100 / dw) : 0;
                G.cpu_percent = pct < 0 ? 0 : pct > 100 ? 100 : pct;
            }
            G.last_idle = idle;
        }
    }
    fd = open("/proc/meminfo", O_RDONLY);
    if (fd >= 0) {
        char buf[512];
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n > 0) {
            buf[n] = 0;
            unsigned long total = 0, used = 0;
            char *p = strstr(buf, "MemTotal:");
            if (p) total = strtoul(p + 9, NULL, 10);
            p = strstr(buf, "MemUsed:");
            if (p) used = strtoul(p + 8, NULL, 10);
            G.mem_percent = total ? (int)(used * 100 / total) : 0;
        }
    }
    G.last_sample_ms = now;
}

/* ---------------------------------------------------------------- start menu */

typedef struct { const char *header; int app; bool power; } MenuRow;

static int menu_rows(MenuRow *rows) {
    int n = 0;
    static const char *const sections[] = { "Programs", "Games", "System" };
    for (int s = 0; s < 3; s++) {
        bool any = false;
        for (int i = 0; i < g_napps; i++) {
            if (strcmp(g_apps[i].section, sections[s]) || !app_present(&g_apps[i])) continue;
            if (!any) { rows[n++] = (MenuRow){ sections[s], -1, false }; any = true; }
            rows[n++] = (MenuRow){ NULL, i, false };
        }
    }
    rows[n++] = (MenuRow){ "Session", -1, false };
    rows[n++] = (MenuRow){ NULL, -2, true };           /* shut down */
    return n;
}

static void menu_draw(KApp *app, KUI *ui, void *user) {
    (void)app; (void)user;
    KCanvas *cv = &ui->cv;
    kc_panel(cv, kr(0, 0, cv->w, cv->h), KB_RAISED, ui->theme->window_bg);
    kc_gradient_v(cv, kr(3, 3, BANNER_W, cv->h - 6), 0xB5122B, 0x3A0612);
    const char *brand = "TouhouOS";
    for (int i = 0; brand[i]; i++) {
        char one[2] = { brand[i], 0 };
        kc_text_bold(cv, 3 + (BANNER_W - 8) / 2, cv->h - 24 - (int)(strlen(brand) - 1 - (size_t)i) * 17, one, 0xFFF6E0);
    }
    k_icon(cv, KI_YINYANG, 3 + (BANNER_W - 24) / 2, 8, 24);

    MenuRow rows[32];
    int n = menu_rows(rows);
    int y = 4;
    for (int i = 0; i < n; i++) {
        if (rows[i].header) {
            kc_text_bold(cv, 3 + BANNER_W + 10, y + 5, rows[i].header, ui->theme->accent_dark);
            kc_hline(cv, 3 + BANNER_W + 8, y + 22, cv->w - BANNER_W - 18, kc_shade(ui->theme->window_bg, 80));
            y += 26;
            continue;
        }
        KRect r = kr(3 + BANNER_W + 2, y, cv->w - BANNER_W - 8, ITEM_H);
        bool hover = k_hover(ui, r);
        if (hover) kc_fill(cv, r, ui->theme->select_bg);
        uint32_t fg = hover ? ui->theme->select_text : ui->theme->text;
        const char *label;
        KIcon icon;
        if (rows[i].power) { label = "Shut down..."; icon = KI_POWER; }
        else { label = g_apps[rows[i].app].name; icon = g_apps[rows[i].app].icon; }
        k_icon(cv, icon, r.x + 6, r.y + 3, 24);
        kc_text(cv, r.x + 38, r.y + 7, label, fg);
        if (hover && ui->released[0]) {
            if (rows[i].power) shutdown_open();
            else launch_app(rows[i].app);
            menu_close();
            return;
        }
        if (hover) k_tooltip_text(ui, rows[i].power ? "Power off or restart" : g_apps[rows[i].app].tooltip);
        y += ITEM_H;
    }
}

static void menu_closed(KWin *w, void *user) { (void)w; (void)user; G.menu = NULL; }

void menu_close(void) {
    if (G.menu) { kwin_close(G.menu); G.menu = NULL; }
}

void menu_open(void) {
    if (G.menu) { menu_close(); return; }
    MenuRow rows[32];
    int n = menu_rows(rows);
    int h = 8;
    for (int i = 0; i < n; i++) h += rows[i].header ? 26 : ITEM_H;
    G.menu = kwin_new(G.app, 2, G.screen_h - PANEL_H - h, MENU_W, h, "Start", FW_POPUP | FW_NO_DECOR, menu_draw, NULL);
    if (G.menu) kwin_set_close_handler(G.menu, menu_closed, NULL);
}

/* ---------------------------------------------------------------- calendar */

static void calendar_draw(KApp *app, KUI *ui, void *user) {
    (void)app; (void)user;
    KCanvas *cv = &ui->cv;
    kc_panel(cv, kr(0, 0, cv->w, cv->h), KB_RAISED, ui->theme->window_bg);
    time_t now = time(NULL);
    struct tm t;
    gmtime_r(&now, &t);
    int month0 = t.tm_year * 12 + t.tm_mon + G.cal_month_offset;
    int year = month0 / 12, month = month0 % 12;
    char title[48];
    snprintf(title, sizeof title, "%s %d", k_months[month], year + 1900);
    if (k_icon_button(ui, kr(8, 8, 26, 24), KI_BACK, false)) G.cal_month_offset--;
    if (k_icon_button(ui, kr(cv->w - 34, 8, 26, 24), KI_FORWARD, false)) G.cal_month_offset++;
    kc_text_in(cv, kr(34, 8, cv->w - 68, 24), title, ui->theme->accent_dark, KA_CENTER);
    static const char *const dow[] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
    for (int i = 0; i < 7; i++) kc_text(cv, 14 + i * 32 + 8, 40, dow[i], i == 0 ? ui->theme->accent : ui->theme->text_dim);
    kc_hline(cv, 10, 58, cv->w - 20, kc_shade(ui->theme->window_bg, 78));
    struct tm first = { 0 };
    first.tm_year = year; first.tm_mon = month; first.tm_mday = 1; first.tm_hour = 12;
    time_t ft = mktime(&first);
    struct tm fl;
    gmtime_r(&ft, &fl);
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int y1900 = year + 1900;
    int dim = mdays[month] + (month == 1 && ((y1900 % 4 == 0 && y1900 % 100 != 0) || y1900 % 400 == 0));
    for (int d = 1; d <= dim; d++) {
        int idx = fl.tm_wday + d - 1;
        KRect r = kr(10 + (idx % 7) * 32, 62 + (idx / 7) * 26, 30, 24);
        bool today = G.cal_month_offset == 0 && d == t.tm_mday;
        if (today) kc_panel(cv, r, KB_SUNKEN, ui->theme->accent);
        char num[4];
        snprintf(num, sizeof num, "%d", d);
        kc_text_in(cv, r, num, today ? ui->theme->text_on_accent : (idx % 7 == 0 ? ui->theme->accent_dark : ui->theme->text), KA_CENTER);
    }
}

static void calendar_closed(KWin *w, void *user) { (void)w; (void)user; G.calendar = NULL; }

void calendar_toggle(void) {
    if (G.calendar) { kwin_close(G.calendar); G.calendar = NULL; return; }
    G.cal_month_offset = 0;
    G.calendar = kwin_new(G.app, G.screen_w - 238, G.screen_h - PANEL_H - 238, 236, 236, "Calendar", FW_POPUP | FW_NO_DECOR, calendar_draw, NULL);
    if (G.calendar) kwin_set_close_handler(G.calendar, calendar_closed, NULL);
}

/* ---------------------------------------------------------------- the panel itself */

static void draw_meter(KCanvas *cv, int x, int y, int w, int h, int pct, uint32_t col, uint32_t bg) {
    kc_fill(cv, kr(x, y, w, h), bg);
    int fill = (h - 2) * pct / 100;
    kc_fill(cv, kr(x + 1, y + h - 1 - fill, w - 2, fill), col);
    kc_outline(cv, kr(x, y, w, h), 0x000000);
}

void panel_draw(KApp *app, KUI *ui, void *user) {
    (void)app; (void)user;
    KCanvas *cv = &ui->cv;
    KRect all = kr(0, 0, cv->w, cv->h);
    kc_gradient_v(cv, all, 0x1C2552, 0x0B1030);
    kc_hline(cv, 0, 0, cv->w, KC_GOLD);
    kc_hline(cv, 0, 1, cv->w, kc_shade(KC_GOLD, 50));
    sample_system();

    /* start button */
    KRect sb = kr(4, 4, 124, PANEL_H - 8);
    bool open = G.menu != NULL;
    bool hover = k_hover(ui, sb);
    kc_panel(cv, sb, open ? KB_SUNKEN : KB_RAISED, open ? 0x8E0B21 : hover ? 0xD6203F : 0xB5122B);
    k_icon(cv, KI_YINYANG, sb.x + 8 + (open ? 1 : 0), sb.y + 4 + (open ? 1 : 0), 20);
    kc_text_bold(cv, sb.x + 34 + (open ? 1 : 0), sb.y + 6 + (open ? 1 : 0), "Gensokyo", 0xFFF6E0);
    if (k_clicked(ui, sb)) menu_open();

    /* quick launch */
    int x = sb.x + sb.w + 8;
    static const struct { KIcon icon; const char *cmd, *tip; } quick[] = {
        { KI_TERMINAL, "gapterm", "Terminal" }, { KI_FOLDER, "explorer", "Explorer" }, { KI_NOTEBOOK, "notebook", "Notebook" },
    };
    for (unsigned i = 0; i < sizeof quick / sizeof quick[0]; i++) {
        KRect r = kr(x, 4, PANEL_H - 6, PANEL_H - 8);
        bool h = k_hover(ui, r);
        if (h) kc_panel(cv, r, ui->mouse_inside && (ui->buttons & 1) ? KB_SUNKEN : KB_RAISED, 0x2A356E);
        k_icon(cv, quick[i].icon, r.x + (r.w - 20) / 2, r.y + (r.h - 20) / 2, 20);
        if (h) k_tooltip_text(ui, quick[i].tip);
        if (k_clicked(ui, r)) launch(quick[i].cmd);
        x += r.w + 2;
    }
    x += 6;
    kc_vline(cv, x, 6, PANEL_H - 12, 0x3B4780);
    x += 8;

    /* tray, right to left */
    int tx = cv->w - 6;
    KRect show = kr(tx - 10, 4, 10, PANEL_H - 8);
    kc_panel(cv, show, KB_RAISED, 0x2A356E);
    if (k_hover(ui, show)) k_tooltip_text(ui, "Show the desktop");
    if (k_clicked(ui, show)) {
        bool any_open = false;
        for (uint32_t i = 0; i < G.windows.count; i++) if (G.windows.win[i].state != FS_MINIMIZED) any_open = true;
        for (uint32_t i = 0; i < G.windows.count; i++) {
            if (any_open) fairy_window_set_state_id(kapp_display(G.app), G.windows.win[i].id, FS_MINIMIZED);
            else fairy_window_activate(kapp_display(G.app), G.windows.win[i].id);
        }
    }
    tx -= 14;
    time_t now = time(NULL);
    struct tm t;
    gmtime_r(&now, &t);
    char clock[16];
    snprintf(clock, sizeof clock, "%02d:%02d", t.tm_hour, t.tm_min);
    KRect cr = kr(tx - 62, 4, 62, PANEL_H - 8);
    bool ch = k_hover(ui, cr);
    kc_panel(cv, cr, G.calendar ? KB_SUNKEN : (ch ? KB_RAISED : KB_FLAT), G.calendar ? 0x1A2244 : 0x141B42);
    kc_text_bold(cv, cr.x + 8, cr.y + 6, clock, 0xF4EFD8);
    if (ch) {
        char tip[48];
        snprintf(tip, sizeof tip, "%s %d, %d", k_months[t.tm_mon], t.tm_mday, t.tm_year + 1900);
        k_tooltip_text(ui, tip);
    }
    if (k_clicked(ui, cr)) calendar_toggle();
    tx = cr.x - 8;
    KRect meters = kr(tx - 46, 6, 46, PANEL_H - 12);
    draw_meter(cv, meters.x, meters.y, 20, meters.h, G.cpu_percent, 0xE8B83C, 0x0B1030);
    draw_meter(cv, meters.x + 24, meters.y, 20, meters.h, G.mem_percent, 0x5BC0E0, 0x0B1030);
    if (k_hover(ui, meters)) {
        char tip[64];
        snprintf(tip, sizeof tip, "CPU %d%%   Memory %d%%", G.cpu_percent, G.mem_percent);
        k_tooltip_text(ui, tip);
    }
    tx = meters.x - 8;

    /* task list */
    int avail = tx - x;
    int n = (int)G.windows.count;
    if (n > 0 && avail > 40) {
        int bw = avail / n - 3;
        if (bw > 190) bw = 190;
        if (bw < 40) bw = 40;
        for (int i = 0; i < n; i++) {
            FeWinInfo *w = &G.windows.win[i];
            KRect r = kr(x + i * (bw + 3), 4, bw, PANEL_H - 8);
            if (r.x + r.w > tx) break;
            bool focused = w->focused != 0, minimized = w->state == FS_MINIMIZED;
            bool hov = k_hover(ui, r);
            uint32_t bg = focused ? 0x3C4C94 : hov ? 0x2E3A78 : 0x232C60;
            kc_panel(cv, r, focused ? KB_SUNKEN : KB_RAISED, bg);
            k_icon(cv, KI_YINYANG, r.x + 6, r.y + (r.h - 16) / 2, 16);
            kc_text_in(cv, kr(r.x + 26, r.y, r.w - 30, r.h), w->title, minimized ? 0x9AA3D0 : 0xF4EFD8, KA_LEFT);
            if (hov) k_tooltip_text(ui, w->title);
            if (k_clicked(ui, r)) {
                if (focused && !minimized) fairy_window_set_state_id(kapp_display(G.app), w->id, FS_MINIMIZED);
                else fairy_window_activate(kapp_display(G.app), w->id);
            }
            if (hov && ui->released[2]) fairy_window_close_request(kapp_display(G.app), w->id);
        }
    }
    ui->redraw_soon = true;
}

void panel_open(void) {
    G.panel = kwin_new(G.app, 0, G.screen_h - PANEL_H, G.screen_w, PANEL_H, "Panel", FW_PANEL | FW_NO_DECOR | FW_NO_FOCUS | FW_SKIP_TASKS, panel_draw, NULL);
    if (G.panel) fairy_set_strut(kwin_fairy(G.panel), 1, PANEL_H);
}

/* the app table is filtered when menus are built */
