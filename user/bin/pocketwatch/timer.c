/* The timer tab: a countdown with presets and a custom time. When it ends the window flashes and the terminal bell
 * character is written to stdout once a second until the alarm is dismissed. */
#include "watch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define trace pw_trace

#define MAX_TIMER_MS ((uint64_t)(99 * 3600 + 59 * 60 + 59) * 1000)

static uint64_t fields_total_ms(void) {
    long v[3];
    for (int i = 0; i < 3; i++) { v[i] = atol(W.tm_field[i]); if (v[i] < 0) v[i] = 0; }
    uint64_t total = ((uint64_t)v[0] * 3600 + (uint64_t)v[1] * 60 + (uint64_t)v[2]) * 1000;
    return total > MAX_TIMER_MS ? MAX_TIMER_MS : total;
}

static void set_fields(uint64_t ms) {
    uint64_t s = ms / 1000;
    snprintf(W.tm_field[0], sizeof W.tm_field[0], "%u", (unsigned)(s / 3600));
    snprintf(W.tm_field[1], sizeof W.tm_field[1], "%u", (unsigned)(s / 60 % 60));
    snprintf(W.tm_field[2], sizeof W.tm_field[2], "%u", (unsigned)(s % 60));
}

uint64_t timer_remaining(void) {
    switch (W.tm_state) {
    case TM_RUNNING: return W.tm_end_ms > W.now_ms ? W.tm_end_ms - W.now_ms : 0;
    case TM_PAUSED: return W.tm_left_ms;
    case TM_RINGING: return 0;
    default: return fields_total_ms();
    }
}

void timer_start(uint64_t ms) {
    if (ms == 0) return;
    W.tm_total_ms = ms;
    W.tm_end_ms = W.now_ms + ms;
    W.tm_state = TM_RUNNING;
    trace("pocketwatch: timer started for %llu s", (unsigned long long)(ms / 1000));
}

static void timer_pause_toggle(void) {
    if (W.tm_state == TM_RUNNING) {
        W.tm_left_ms = W.tm_end_ms > W.now_ms ? W.tm_end_ms - W.now_ms : 0;
        W.tm_state = TM_PAUSED;
        trace("pocketwatch: timer paused");
    } else if (W.tm_state == TM_PAUSED) {
        W.tm_end_ms = W.now_ms + W.tm_left_ms;
        W.tm_state = TM_RUNNING;
        trace("pocketwatch: timer resumed");
    }
}

static void timer_cancel(void) {
    if (W.tm_state == TM_RUNNING || W.tm_state == TM_PAUSED) {
        W.tm_state = TM_IDLE;
        trace("pocketwatch: timer cancelled");
    }
}

void timer_dismiss(void) {
    if (W.tm_state != TM_RINGING) return;
    W.tm_state = TM_IDLE;
    trace("pocketwatch: alarm dismissed");
}

void timer_update(void) {
    if (W.tm_state == TM_RUNNING && W.now_ms >= W.tm_end_ms) {
        W.tm_state = TM_RINGING;
        W.tm_ring_ms = W.now_ms;
        W.tm_bell_ms = 0;
        trace("pocketwatch: timer finished, ringing");
        kapp_set_state(W.app, FS_NORMAL);                       /* un-minimize and bring the window forward */
        fairy_window_raise(kapp_window(W.app));
    }
    if (W.tm_state == TM_RINGING && (W.tm_bell_ms == 0 || W.now_ms - W.tm_bell_ms >= 1000)) {
        fputc('\a', stdout);                                    /* the terminal bell, repeated until the alarm is dismissed */
        fflush(stdout);
        W.tm_bell_ms = W.now_ms;
    }
}

void timer_title(char *out, size_t cap) {
    char t[32];
    switch (W.tm_state) {
    case TM_RUNNING:
    case TM_PAUSED: {
        uint64_t rem = timer_remaining();
        format_hms((rem + 999) / 1000 * 1000, false, t, sizeof t);
        snprintf(out, cap, W.tm_state == TM_RUNNING ? "%s left - Clock" : "%s paused - Clock", t);
        break;
    }
    case TM_RINGING:
        snprintf(out, cap, ((W.now_ms - W.tm_ring_ms) / 500) % 2 ? "* Timer finished * - Clock" : "TIME IS UP! - Clock");
        break;
    default:
        snprintf(out, cap, "Clock");
        break;
    }
}

static void big_text(KCanvas *cv, int cx, int y, const char *s, int scale, uint32_t color, uint32_t shadow) {
    int w = (int)strlen(s) * 8 * scale;
    int x = cx - w / 2;
    kc_text_scaled(cv, x + 2, y + 2, s, shadow, scale);
    kc_text_scaled(cv, x + 1, y, s, color, scale);
    kc_text_scaled(cv, x, y, s, color, scale);
}

static void digits_only(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) if (isdigit((unsigned char)*p)) *o++ = *p;
    *o = 0;
}

void timer_tab(KUI *ui, KRect b) {
    KCanvas *cv = &ui->cv;
    int cx = b.x + b.w / 2;
    int y = b.y + 14;
    uint64_t show = timer_remaining();
    if (W.tm_state == TM_RUNNING || W.tm_state == TM_PAUSED) show = (show + 999) / 1000 * 1000;     /* count down to 00:00:00, not from it */
    unsigned s = (unsigned)(show / 1000);
    char buf[40];
    snprintf(buf, sizeof buf, "%02u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    int cells = (int)strlen(buf), scale = 4;
    while (scale > 1 && cells * 8 * scale > b.w - 24) scale--;
    bool ringing = W.tm_state == TM_RINGING;
    bool flash = ringing && ((W.now_ms - W.tm_ring_ms) / 500) % 2 == 0;
    KRect panel = kr(cx - cells * 4 * scale - 14, y - 8, cells * 8 * scale + 28, 16 * scale + 16);
    kc_panel(cv, panel, KB_SUNKEN, flash ? 0x8E0B21 : 0x141B42);
    uint32_t col = ringing ? (flash ? 0xFFF6E0 : 0xFF9A86) : W.tm_state == TM_PAUSED ? 0x8B97C8 : W.tm_state == TM_RUNNING ? 0xF4EFD8 : 0xB9C2F0;
    big_text(cv, cx, y, buf, scale, col, 0x050816);
    y += 16 * scale + 22;

    if (ringing) {
        const char *big = "Time is up!";
        uint32_t bc = flash ? ui->theme->accent : ui->theme->accent_dark;
        big_text(cv, cx, y, big, 2, bc, kc_shade(ui->theme->window_bg, 80));
        y += 44;
        char msg[96];
        char dur[32];
        format_hms(W.tm_total_ms, false, dur, sizeof dur);
        snprintf(msg, sizeof msg, "The timer for %s has finished.", dur);
        kc_text_in(cv, kr(b.x + 8, y, b.w - 16, 16), msg, ui->theme->text, KA_CENTER);
        y += 28;
        if (k_button(ui, kr(cx - 70, y, 140, 34), "Dismiss")) timer_dismiss();
        ui->redraw_soon = true;
        return;
    }

    if (W.tm_state == TM_RUNNING || W.tm_state == TM_PAUSED) {
        int pct = W.tm_total_ms ? (int)(100 - show * 100 / W.tm_total_ms) : 0;
        int pw = b.w - 60 > 360 ? 360 : b.w - 60;
        k_progress(ui, kr(cx - pw / 2, y, pw, 18), pct < 0 ? 0 : pct);
        if (W.tm_state == TM_RUNNING) W.live = kr(b.x, panel.y, b.w, y + 18 - panel.y);
        y += 34;
        int bw = 120, gap = 12;
        if (k_button(ui, kr(cx - bw - gap / 2, y, bw, 32), W.tm_state == TM_RUNNING ? "Pause" : "Resume")) timer_pause_toggle();
        if (k_button(ui, kr(cx + gap / 2, y, bw, 32), "Cancel")) timer_cancel();
        y += 46;
        char dur[32], line[96];
        format_hms(W.tm_total_ms, false, dur, sizeof dur);
        snprintf(line, sizeof line, "Counting down from %s", dur);
        kc_text_in(cv, kr(b.x + 8, y, b.w - 16, 16), line, ui->theme->text_dim, KA_CENTER);
        return;
    }

    /* idle: choose a duration */
    static const char *const labels[3] = { "hours", "min", "sec" };
    int fw = 44, spacing = 10;
    int total_w = 0;
    for (int i = 0; i < 3; i++) total_w += fw + 6 + (int)strlen(labels[i]) * 8 + (i < 2 ? spacing + 10 : 0);
    int fx = cx - total_w / 2;
    bool enter = false;
    for (int i = 0; i < 3; i++) {
        if (k_textfield(ui, kr(fx, y, fw, 26), W.tm_field[i], sizeof W.tm_field[i])) enter = true;
        digits_only(W.tm_field[i]);
        fx += fw + 6;
        k_label(ui, fx, y + 5, labels[i]);
        fx += (int)strlen(labels[i]) * 8 + spacing + 10;
    }
    y += 40;
    static const struct { const char *label; unsigned secs; } presets[] = {
        { "1 min", 60 }, { "3 min", 180 }, { "5 min", 300 }, { "10 min", 600 }, { "15 min", 900 }, { "30 min", 1800 }, { "1 hour", 3600 },
    };
    const int np = (int)(sizeof presets / sizeof presets[0]);
    int pbw = 64, pgap = 6;
    int per_row = (b.w - 24 + pgap) / (pbw + pgap);
    if (per_row < 2) per_row = 2;
    if (per_row > np) per_row = np;
    int rows = (np + per_row - 1) / per_row;
    for (int r = 0; r < rows; r++) {
        int n_in_row = r == rows - 1 ? np - r * per_row : per_row;
        int rowx = cx - (n_in_row * pbw + (n_in_row - 1) * pgap) / 2;
        for (int c = 0; c < n_in_row; c++) {
            int i = r * per_row + c;
            if (k_button(ui, kr(rowx + c * (pbw + pgap), y, pbw, 26), presets[i].label)) set_fields((uint64_t)presets[i].secs * 1000);
        }
        y += 32;
    }
    y += 8;
    uint64_t total = fields_total_ms();
    KRect sr = kr(cx - 70, y, 140, 34);
    if (total > 0) { if (k_button(ui, sr, "Start") || enter) timer_start(total); }
    else { kc_panel(cv, sr, KB_RAISED, kc_shade(ui->theme->button_bg, 96)); kc_text_in(cv, sr, "Start", ui->theme->text_dim, KA_CENTER); }
}

/* keys that work on the timer tab when no text field is being edited */
void timer_keys(KUI *ui) {
    if (W.tm_state == TM_RINGING) {
        if (k_key_pressed(ui, KEY_SPACE) || k_key_pressed(ui, KEY_ENTER) || k_key_pressed(ui, KEY_ESC) || k_key_pressed(ui, KEY_KPENTER)) timer_dismiss();
        return;
    }
    if (W.tab != TAB_TIMER) return;
    if (W.tm_state == TM_RUNNING || W.tm_state == TM_PAUSED) {
        if (k_key_pressed(ui, KEY_SPACE)) timer_pause_toggle();
        if (k_key_pressed(ui, KEY_ESC)) timer_cancel();
    }
}
