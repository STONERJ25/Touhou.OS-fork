/*
 * pocketwatch: Sakuya's Pocket Watch. Clock, stopwatch, timer and calendar in one window.
 *
 *   pocketwatch                          the desktop application
 *   pocketwatch --calendar [YEAR MONTH]  print a month (ISO weeks, Monday first) and exit
 *   pocketwatch --date YYYY-MM-DD        print weekday, day of the year and ISO week and exit
 * Options for the window: --trace (log events on stdout), --tab clock|stopwatch|timer|calendar, --timer SECONDS.
 *
 * Sakuya can stop time: press T on the clock.
 */
#include "watch.h"
#include "dates.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

Watch W;

void pw_trace(const char *fmt, ...) {
    if (!W.trace) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

/* ---------------------------------------------------------------- command line */

static void print_month(int y, int m) {
    char title[64];
    snprintf(title, sizeof title, "%s %d", g_month_names[m - 1], y);
    printf("%*s%s\n", (int)(26 - strlen(title)) / 2, "", title);
    puts("Wk Mo Tu We Th Fr Sa Su");
    int lead = (date_weekday(y, m, 1) + 6) % 7;
    int dim = date_days_in_month(y, m);
    int day = 1 - lead;
    while (day <= dim) {
        int dy, dm, dd;
        date_from_days(date_to_days(y, m, 1) + day - 1, &dy, &dm, &dd);
        printf("%2d", date_iso_week(dy, dm, dd, NULL));
        for (int c = 0; c < 7; c++, day++) {
            if (day < 1 || day > dim) printf("   ");
            else printf(" %2d", day);
        }
        putchar('\n');
    }
}

static int cli(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--calendar")) {
            time_t now = time(NULL);
            struct tm t;
            localtime_r(&now, &t);
            int y = t.tm_year + 1900, m = t.tm_mon + 1;
            if (i + 2 < argc) { y = atoi(argv[i + 1]); m = atoi(argv[i + 2]); }
            if (y < 1 || y > 9999 || m < 1 || m > 12) { fprintf(stderr, "pocketwatch: year 1..9999 and month 1..12 expected\n"); return 2; }
            print_month(y, m);
            return 0;
        }
        if (!strcmp(argv[i], "--date") && i + 1 < argc) {
            int y, m, d;
            if (sscanf(argv[i + 1], "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > date_days_in_month(y, m) || y < 1) {
                fprintf(stderr, "pocketwatch: not a date: %s\n", argv[i + 1]);
                return 2;
            }
            int iso_y;
            int wk = date_iso_week(y, m, d, &iso_y);
            printf("%s day %d week %d-W%02d\n", g_weekday_names[date_weekday(y, m, d)], date_day_of_year(y, m, d), iso_y, wk);
            return 0;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------- the window */

static void toggle_frozen(void) {
    if (!W.frozen) {
        W.frozen = true;
        W.frozen_at = time(NULL);
        W.frozen_since_ms = W.now_ms;
        pw_trace("pocketwatch: Sakuya has stopped time");
    } else {
        W.frozen = false;
        pw_trace("pocketwatch: time flows again");
    }
}

static void set_tab(KUI *ui, int tab) {
    if (tab < 0 || tab >= TAB_COUNT || tab == W.tab) return;
    W.tab = tab;
    ui->focus = 0;
    static const char *const names[TAB_COUNT] = { "clock", "stopwatch", "timer", "calendar" };
    pw_trace("pocketwatch: tab %s", names[tab]);
}

static void global_keys(KUI *ui) {
    for (int i = 0; i < ui->nkeys; i++) {
        KKey *k = &ui->keys[i];
        if (!k->down || !k->code) continue;
        if ((k->mods & MOD_CTRL) && k->code >= KEY_1 && k->code <= KEY_4) { set_tab(ui, k->code - KEY_1); k->down = 0; k->code = 0; }
        else if ((k->mods & MOD_CTRL) && (k->code == KEY_Q || k->code == KEY_W)) { kapp_quit(W.app, 0); k->down = 0; k->code = 0; }
    }
    timer_keys(ui);
    if (ui->focus != 0 || (ui->mods & (MOD_CTRL | MOD_ALT))) return;           /* a text field is being edited */
    if (k_key_pressed(ui, KEY_T)) toggle_frozen();
    if (W.tab == TAB_STOPWATCH) {
        if (k_key_pressed(ui, KEY_SPACE)) sw_toggle();
        if (k_key_pressed(ui, KEY_L)) sw_lap();
        if (k_key_pressed(ui, KEY_R)) sw_reset();
    }
    calendar_keys(ui);
}

static void status_bar(KUI *ui, KRect r) {
    KCanvas *cv = &ui->cv;
    kc_fill(cv, r, kc_shade(ui->theme->window_bg, 94));
    kc_hline(cv, r.x, r.y, r.w, kc_shade(ui->theme->window_bg, 72));
    kc_hline(cv, r.x, r.y + 1, r.w, 0xFFFFFF);
    char line[160] = "";
    char t[40];
    if (W.tm_state == TM_RINGING) {
        bool on = ((W.now_ms - W.tm_ring_ms) / 500) % 2 == 0;
        kc_text_bold(cv, r.x + 8, r.y + 5, "Time is up! Press Space to dismiss the alarm.", on ? ui->theme->accent : ui->theme->accent_dark);
        if (k_button(ui, kr(r.x + r.w - 84, r.y + 2, 76, r.h - 4), "Dismiss")) timer_dismiss();
        return;
    }
    size_t n = 0;
    if (W.tm_state == TM_RUNNING || W.tm_state == TM_PAUSED) {
        format_hms((timer_remaining() + 999) / 1000 * 1000, false, t, sizeof t);
        n += (size_t)snprintf(line + n, sizeof line - n, "Timer %s%s", t, W.tm_state == TM_PAUSED ? " (paused)" : "");
    }
    if (W.sw_running || W.sw_accum_ms > 0) {
        format_hms(sw_elapsed(), W.tab == TAB_STOPWATCH, t, sizeof t);
        n += (size_t)snprintf(line + n, sizeof line - n, "%sStopwatch %s%s", n ? "   |   " : "", t, W.sw_running ? "" : " (stopped)");
    }
    if (W.frozen) n += (size_t)snprintf(line + n, sizeof line - n, "%sTime is stopped", n ? "   |   " : "");
    if (!n) {
        time_t zn = time(NULL);
        struct tm zt;
        localtime_r(&zn, &zt);
        snprintf(line, sizeof line, "All times are %s.", zt.tm_zone ? zt.tm_zone : "UTC");
    }
    kc_text_in(cv, kr(r.x + 8, r.y + 2, r.w - 16, r.h - 2), line, ui->theme->text_dim, KA_LEFT);
}

static void draw(KApp *app, KUI *ui, void *user) {
    (void)user;
    KCanvas *cv = &ui->cv;
    W.now_ms = ui->now_ms;
    bool input = ui->nkeys || ui->wheel || ui->double_click || ui->pressed[0] || ui->pressed[1] || ui->pressed[2] || ui->released[0] ||
                 ui->released[1] || ui->released[2] || ui->mx != W.last_mx || ui->my != W.last_my || ui->buttons != W.last_buttons ||
                 cv->w != W.last_w || cv->h != W.last_h || W.tab != W.last_tab || ui->focus != 0;
    W.last_mx = ui->mx; W.last_my = ui->my; W.last_buttons = ui->buttons; W.last_w = cv->w; W.last_h = cv->h; W.last_tab = W.tab;
    W.live = kr(0, 0, 0, 0);
    if (W.now_ms - W.conf_checked_ms > 3000) { conf_reload(); W.conf_checked_ms = W.now_ms; }
    timer_update();

    bool flash = W.tm_state == TM_RINGING && ((W.now_ms - W.tm_ring_ms) / 500) % 2 == 0;
    if (flash) kc_fill(cv, kr(0, 0, cv->w, cv->h), 0xF2D27A);
    else k_clear(ui);
    if (W.tab != TAB_TIMER || W.tm_state != TM_IDLE) ui->focus = 0;
    global_keys(ui);

    static const char *const tabs[TAB_COUNT] = { "Clock", "Stopwatch", "Timer", "Calendar" };
    int sel = W.tab;
    k_tabs(ui, kr(8, 8, cv->w - 16, 26), tabs, TAB_COUNT, &sel);
    set_tab(ui, sel);

    int status_h = 24;
    KRect body = kr(8, 34, cv->w - 16, cv->h - 34 - status_h - 6);
    kc_panel(cv, body, KB_FLAT, flash ? 0xF8E4A6 : ui->theme->window_bg);
    KRect old = cv->clip;
    kc_set_clip(cv, kr_intersect(old, kr_inset(body, 1)));
    KRect inner = kr_inset(body, 2);
    switch (W.tab) {
    case TAB_CLOCK: clock_tab(ui, inner); break;
    case TAB_STOPWATCH: stopwatch_tab(ui, inner); break;
    case TAB_TIMER: timer_tab(ui, inner); break;
    case TAB_CALENDAR: calendar_tab(ui, inner); break;
    }
    cv->clip = old;
    KRect status = kr(0, cv->h - status_h, cv->w, status_h);
    status_bar(ui, status);
    bool ringing = W.tm_state == TM_RINGING;
    if (!input && !kr_empty(W.live) && !ringing && !W.last_ringing && W.frozen == W.last_frozen) {   /* only the clock moved: commit just that */
        k_damage(ui, W.live);
        k_damage(ui, status);
    }
    W.last_ringing = ringing;
    W.last_frozen = W.frozen;

    /* the title shows the time left on the timer */
    char title[64];
    timer_title(title, sizeof title);
    if (W.frozen && W.tm_state == TM_IDLE) snprintf(title, sizeof title, "Clock (time stopped)");
    if (strcmp(title, W.title)) { snprintf(W.title, sizeof W.title, "%s", title); kapp_set_title(app, title); }

    /* wake up again exactly when something on screen is going to change (the emulated machine is slow: never poll faster than needed) */
    int want = 5000;
    if (W.tm_state == TM_RINGING) want = 100;                       /* flashing and ringing */
    else {
        if (W.sw_running) {
            if (W.tab == TAB_STOPWATCH) want = 50;                  /* centiseconds */
            else { int t = 1000 - (int)(sw_elapsed() % 1000) + 6; if (t < want) want = t; }
        }
        if (W.tm_state == TM_RUNNING) {
            uint64_t rem = timer_remaining();
            int t = (int)(rem % 1000) + 6;
            if ((uint64_t)t > rem + 4) t = (int)(rem + 4);          /* the alarm itself */
            if (t < want) want = t;
        }
        if (W.tab == TAB_CLOCK && !W.frozen) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            int t = 1000 - (int)(ts.tv_nsec / 1000000) + 6;         /* the next second of the wall clock */
            if (t < want) want = t;
        }
    }
    if (want < 20) want = 20;
    kapp_set_tick(app, want);
}

static bool on_close(KApp *app, void *user) { (void)app; (void)user; return true; }

int main(int argc, char **argv) {
    int rc = cli(argc, argv);
    if (rc >= 0) return rc;
    memset(&W, 0, sizeof W);
    W.clock24 = true;
    snprintf(W.tm_field[0], sizeof W.tm_field[0], "0");
    snprintf(W.tm_field[1], sizeof W.tm_field[1], "5");
    snprintf(W.tm_field[2], sizeof W.tm_field[2], "0");
    W.tab = TAB_CLOCK;
    uint64_t start_timer_s = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) W.trace = true;
        else if (!strcmp(argv[i], "--tab") && i + 1 < argc) {
            const char *n = argv[++i];
            W.tab = !strcmp(n, "stopwatch") ? TAB_STOPWATCH : !strcmp(n, "timer") ? TAB_TIMER : !strcmp(n, "calendar") ? TAB_CALENDAR : TAB_CLOCK;
        } else if (!strcmp(argv[i], "--timer") && i + 1 < argc) start_timer_s = (uint64_t)atol(argv[++i]);
    }
    KApp *app = kapp_new("Clock", 580, 440, FW_RESIZABLE);
    if (!app) return 1;
    W.app = app;
    W.now_ms = k_now_ms();
    snprintf(W.title, sizeof W.title, "Clock");
    conf_reload();
    W.conf_checked_ms = W.now_ms;
    calendar_init();
    if (start_timer_s) timer_start(start_timer_s * 1000);
    kapp_set_draw(app, draw, NULL);
    kapp_set_close_handler(app, on_close, NULL);
    kapp_set_minsize(app, 400, 360);
    kapp_set_tick(app, 1000);
    pw_trace("pocketwatch: ready");
    rc = kapp_run(app);
    kapp_destroy(app);
    return rc;
}
