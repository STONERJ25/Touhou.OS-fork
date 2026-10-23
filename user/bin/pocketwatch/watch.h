/* Sakuya's Pocket Watch: state shared by the tabs. */
#ifndef PW_WATCH_H
#define PW_WATCH_H

#include <kappa.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

enum { TAB_CLOCK, TAB_STOPWATCH, TAB_TIMER, TAB_CALENDAR, TAB_COUNT };

#define MAX_LAPS 200

typedef struct Watch {
    KApp *app;
    int tab;
    bool trace;
    uint64_t now_ms;                    /* CLOCK_MONOTONIC, refreshed at the start of every frame */
    bool clock24;                       /* from ~/.config/hakurei.conf (clock24=0 shows 12 hour time) */
    uint64_t conf_checked_ms;

    /* clock */
    bool frozen;                        /* Sakuya has stopped time */
    time_t frozen_at;
    uint64_t frozen_since_ms;

    /* stopwatch */
    bool sw_running;
    uint64_t sw_started_ms;             /* monotonic time of the last start */
    uint64_t sw_accum_ms;               /* elapsed time before that start */
    uint64_t sw_laps[MAX_LAPS];         /* elapsed time at each lap */
    int sw_nlaps;
    int sw_scroll;

    /* timer */
    char tm_field[3][4];                /* hours, minutes, seconds as typed */
    enum { TM_IDLE, TM_RUNNING, TM_PAUSED, TM_RINGING } tm_state;
    uint64_t tm_total_ms;               /* the duration that was started */
    uint64_t tm_end_ms;                 /* monotonic end time while running */
    uint64_t tm_left_ms;                /* what is left while paused */
    uint64_t tm_ring_ms;                /* when the alarm started */
    uint64_t tm_bell_ms;                /* the last time the bell character was written */

    /* calendar */
    int cal_year, cal_month;            /* month being shown */
    int sel_y, sel_m, sel_d;            /* selected day */

    char title[64];

    /* partial redraws: a frame caused only by the clock ticking commits just what changes */
    KRect live;                         /* set by the tab being drawn: the part of the window that changes every tick */
    int last_mx, last_my, last_w, last_h, last_tab;
    uint8_t last_buttons;
    bool last_ringing, last_frozen;
} Watch;

extern Watch W;

/* prints a line on stdout when started with --trace (the tests read these from the serial console) */
void pw_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* conf.c */
void conf_reload(void);                 /* reads ~/.config/hakurei.conf */

/* face.c */
void face_draw(KCanvas *cv, int cx, int cy, int radius, const struct tm *t, bool frozen);
void fill_poly(KCanvas *cv, const int *xy, int n, uint32_t color, int alpha);

/* tabs */
void clock_tab(KUI *ui, KRect body);
void stopwatch_tab(KUI *ui, KRect body);
void timer_tab(KUI *ui, KRect body);
void calendar_tab(KUI *ui, KRect body);

/* stopwatch.c / timer.c: state transitions and queries used by main.c */
uint64_t sw_elapsed(void);
void sw_toggle(void);
void sw_lap(void);
void sw_reset(void);
void timer_update(void);                /* detects the alarm going off, rings the bell */
uint64_t timer_remaining(void);
void timer_start(uint64_t ms);
void timer_dismiss(void);
void timer_title(char *out, size_t cap);        /* "04:32 left - Clock" etc. */
void timer_keys(KUI *ui);                       /* Space/Esc/Enter on the timer tab, and to dismiss a ringing alarm on any tab */
void calendar_init(void);
void calendar_keys(KUI *ui);
void format_hms(uint64_t ms, bool cs, char *out, size_t cap);

#endif
