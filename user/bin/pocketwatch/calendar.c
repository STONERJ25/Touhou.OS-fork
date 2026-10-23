/* The calendar tab: a month grid with ISO week numbers, today highlighted, year and month navigation. */
#include "watch.h"
#include "dates.h"
#include <stdio.h>
#include <string.h>

#define trace pw_trace

static void today(int *y, int *m, int *d) {
    time_t now = time(NULL);
    struct tm t;
    gmtime_r(&now, &t);
    *y = t.tm_year + 1900;
    *m = t.tm_mon + 1;
    *d = t.tm_mday;
}

void calendar_init(void) {
    today(&W.sel_y, &W.sel_m, &W.sel_d);
    W.cal_year = W.sel_y;
    W.cal_month = W.sel_m;
}

static void show_month_of_selection(void) {
    W.cal_year = W.sel_y;
    W.cal_month = W.sel_m;
}

static void select_date(int y, int m, int d) {
    if (y < 1) { y = 1; m = 1; d = 1; }
    if (y > 9999) { y = 9999; m = 12; d = 31; }
    W.sel_y = y; W.sel_m = m; W.sel_d = d;
    show_month_of_selection();
}

static void move_selection_days(long delta) {
    int y = W.sel_y, m = W.sel_m, d = W.sel_d;
    date_add_days(&y, &m, &d, delta);
    select_date(y, m, d);
}

static void move_selection_months(int delta) {
    int y = W.sel_y, m = W.sel_m, d = W.sel_d;
    date_add_months(&y, &m, delta);
    int dim = date_days_in_month(y, m);
    select_date(y, m, d > dim ? dim : d);
}

void calendar_keys(KUI *ui) {
    if (W.tab != TAB_CALENDAR) return;
    for (int i = 0; i < ui->nkeys; i++) {
        KKey *k = &ui->keys[i];
        if (!k->down || !k->code) continue;
        bool ctrl = (k->mods & MOD_CTRL) || (k->mods & MOD_SHIFT);
        bool used = true;
        switch (k->code) {
        case KEY_LEFT: move_selection_days(-1); break;
        case KEY_RIGHT: move_selection_days(1); break;
        case KEY_UP: move_selection_days(-7); break;
        case KEY_DOWN: move_selection_days(7); break;
        case KEY_PAGEUP: if (ctrl) move_selection_months(-12); else move_selection_months(-1); break;
        case KEY_PAGEDOWN: if (ctrl) move_selection_months(12); else move_selection_months(1); break;
        case KEY_HOME: { int y, m, d; today(&y, &m, &d); select_date(y, m, d); break; }
        default: used = false; break;
        }
        if (used) { k->down = 0; k->code = 0; }
    }
}

static const char *season_note(int m) {
    switch (m) {
    case 12: case 1: case 2: return "Winter: the shrine path needs sweeping again.";
    case 3: case 4: case 5: return "Spring: cherry blossoms are due, and so are the picnics.";
    case 6: case 7: case 8: return "Summer: fireflies by the lake, cicadas everywhere else.";
    default: return "Autumn: the maple leaves are showing off.";
    }
}

void calendar_tab(KUI *ui, KRect b) {
    KCanvas *cv = &ui->cv;
    int ty, tm, td;
    today(&ty, &tm, &td);
    W.live = b;

    /* navigation row */
    int y = b.y + 8;
    KRect nav = kr(b.x + 8, y, b.w - 16, 28);
    int bw = 30;
    if (k_button(ui, kr(nav.x, y, bw + 6, 28), "<<")) { W.cal_year--; if (W.cal_year < 1) W.cal_year = 1; }
    if (k_button(ui, kr(nav.x + bw + 8, y, bw, 28), "<")) date_add_months(&W.cal_year, &W.cal_month, -1);
    if (k_button(ui, kr(nav.x + nav.w - bw - 8 - bw - 0, y, bw, 28), ">")) date_add_months(&W.cal_year, &W.cal_month, 1);
    if (k_button(ui, kr(nav.x + nav.w - bw - 6, y, bw + 6, 28), ">>")) { W.cal_year++; if (W.cal_year > 9999) W.cal_year = 9999; }
    char title[64];
    snprintf(title, sizeof title, "%s %d", g_month_names[W.cal_month - 1], W.cal_year);
    KRect tr = kr(nav.x + 72, y, nav.w - 144, 28);
    int today_w = 64;
    if (tr.w > today_w + 150) {
        if (k_button(ui, kr(tr.x + tr.w - today_w, y + 2, today_w, 24), "Today")) select_date(ty, tm, td);
        tr.w -= today_w + 8;
    }
    kc_text_bold(cv, tr.x + (tr.w - (int)strlen(title) * 8) / 2, y + 6, title, ui->theme->accent_dark);
    y += 38;

    /* the grid: week number + 7 days */
    KRect grid = kr(b.x + 8, y, b.w - 16, b.y + b.h - y - 70);
    if (grid.h < 7 * 18) grid.h = 7 * 18;
    int cols = 8;
    int cw = grid.w / cols;
    if (cw < 24) cw = 24;
    int ch = grid.h / 7;
    if (ch > 36) ch = 36;
    if (ch < 16) ch = 16;
    int gx = grid.x + (grid.w - cw * cols) / 2;
    static const char *const heads[8] = { "Wk", "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };
    kc_fill(cv, kr(gx, y, cw * cols, ch), kc_shade(ui->theme->window_bg, 94));
    for (int c = 0; c < cols; c++) {
        uint32_t col = c == 0 ? ui->theme->text_dim : (c >= 6 ? ui->theme->accent_dark : ui->theme->text);
        kc_text_in(cv, kr(gx + c * cw, y, cw, ch), heads[c], col, KA_CENTER);
    }
    kc_hline(cv, gx, y + ch, cw * cols, kc_shade(ui->theme->window_bg, 70));

    int first_wd = date_weekday(W.cal_year, W.cal_month, 1);           /* Sunday = 0 */
    int lead = (first_wd + 6) % 7;                                     /* days from Monday to the 1st */
    long start = date_to_days(W.cal_year, W.cal_month, 1) - lead;
    KRect old = cv->clip;
    for (int r = 0; r < 6; r++) {
        int ry = y + ch * (r + 1) + 2;
        int wy, wm, wdd;
        date_from_days(start + r * 7, &wy, &wm, &wdd);
        int iso_y;
        int wk = date_iso_week(wy, wm, wdd, &iso_y);
        char wks[8];
        snprintf(wks, sizeof wks, "%d", wk);
        kc_text_in(cv, kr(gx, ry, cw, ch - 2), wks, ui->theme->text_dim, KA_CENTER);
        kc_vline(cv, gx + cw - 1, ry, ch - 2, kc_shade(ui->theme->window_bg, 82));
        for (int c = 0; c < 7; c++) {
            int dy_, dm_, dd_;
            date_from_days(start + r * 7 + c, &dy_, &dm_, &dd_);
            KRect cell = kr(gx + (c + 1) * cw + 1, ry, cw - 2, ch - 2);
            bool in_month = dm_ == W.cal_month && dy_ == W.cal_year;
            bool is_today = dy_ == ty && dm_ == tm && dd_ == td;
            bool is_sel = dy_ == W.sel_y && dm_ == W.sel_m && dd_ == W.sel_d;
            bool hover = k_hover(ui, cell);
            uint32_t fg = in_month ? (c >= 5 ? ui->theme->accent_dark : ui->theme->text) : kc_mix(ui->theme->window_bg, ui->theme->text_dim, 150);
            if (is_today) { kc_panel(cv, cell, KB_RAISED, ui->theme->accent); fg = ui->theme->text_on_accent; }
            else if (hover) kc_fill(cv, cell, kc_shade(ui->theme->window_bg, 92));
            if (is_sel) kc_outline(cv, kr_inset(cell, is_today ? 3 : 0), is_today ? 0xFFF6E0 : ui->theme->select_bg);
            char num[8];
            snprintf(num, sizeof num, "%d", dd_);
            kc_set_clip(cv, kr_intersect(old, cell));
            int tw = (int)strlen(num) * 8;
            kc_text(cv, cell.x + (cell.w - tw) / 2, cell.y + (cell.h - 16) / 2, num, fg);
            if (is_today) kc_text(cv, cell.x + (cell.w - tw) / 2 + 1, cell.y + (cell.h - 16) / 2, num, fg);
            cv->clip = old;
            if (hover && ui->released[0]) {
                select_date(dy_, dm_, dd_);
                trace("pocketwatch: selected %04d-%02d-%02d", dy_, dm_, dd_);
            }
        }
    }

    /* details of the selected day */
    int fy = y + ch * 7 + 12;
    if (fy + 48 > b.y + b.h) return;
    int wd = date_weekday(W.sel_y, W.sel_m, W.sel_d);
    int iso_y;
    int wk = date_iso_week(W.sel_y, W.sel_m, W.sel_d, &iso_y);
    int doy = date_day_of_year(W.sel_y, W.sel_m, W.sel_d);
    int diy = date_is_leap(W.sel_y) ? 366 : 365;
    char l1[96], l2[128];
    snprintf(l1, sizeof l1, "%s, %d %s %d", g_weekday_names[wd], W.sel_d, g_month_names[W.sel_m - 1], W.sel_y);
    long diff = date_to_days(W.sel_y, W.sel_m, W.sel_d) - date_to_days(ty, tm, td);
    char rel[48];
    if (diff == 0) snprintf(rel, sizeof rel, "today");
    else if (diff == 1) snprintf(rel, sizeof rel, "tomorrow");
    else if (diff == -1) snprintf(rel, sizeof rel, "yesterday");
    else if (diff > 0) snprintf(rel, sizeof rel, "in %ld days", diff);
    else snprintf(rel, sizeof rel, "%ld days ago", -diff);
    snprintf(l2, sizeof l2, "Day %d of %d, ISO week %d of %d - %s", doy, diy, wk, iso_y, rel);
    kc_text_in(cv, kr(b.x + 10, fy, b.w - 20, 18), l1, ui->theme->text, KA_LEFT);
    kc_text_in(cv, kr(b.x + 10 + 1, fy, b.w - 20, 18), l1, ui->theme->text, KA_LEFT);
    kc_text_in(cv, kr(b.x + 10, fy + 20, b.w - 20, 16), l2, ui->theme->text_dim, KA_LEFT);
    if (fy + 56 <= b.y + b.h) kc_text_in(cv, kr(b.x + 10, fy + 38, b.w - 20, 16), season_note(W.cal_month), ui->theme->text_dim, KA_LEFT);
}
