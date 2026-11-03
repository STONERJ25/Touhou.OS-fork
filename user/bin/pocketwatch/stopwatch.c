/* The stopwatch tab: centisecond resolution from the monotonic clock, laps, keyboard control. */
#include "watch.h"
#include <stdio.h>
#include <string.h>

#define trace pw_trace

void format_hms(uint64_t ms, bool cs, char *out, size_t cap) {
    uint64_t total_s = ms / 1000;
    unsigned h = (unsigned)(total_s / 3600), m = (unsigned)(total_s / 60 % 60), s = (unsigned)(total_s % 60);
    unsigned c = (unsigned)(ms % 1000 / 10);
    if (h > 0) {
        if (cs) snprintf(out, cap, "%u:%02u:%02u.%02u", h, m, s, c);
        else snprintf(out, cap, "%u:%02u:%02u", h, m, s);
    } else {
        if (cs) snprintf(out, cap, "%02u:%02u.%02u", m, s, c);
        else snprintf(out, cap, "%02u:%02u", m, s);
    }
}

uint64_t sw_elapsed(void) { return W.sw_accum_ms + (W.sw_running ? W.now_ms - W.sw_started_ms : 0); }

void sw_toggle(void) {
    if (W.sw_running) {
        W.sw_accum_ms += W.now_ms - W.sw_started_ms;
        W.sw_running = false;
        char b[32];
        format_hms(W.sw_accum_ms, true, b, sizeof b);
        trace("pocketwatch: stopwatch stopped at %s", b);
    } else {
        W.sw_started_ms = W.now_ms;
        W.sw_running = true;
        trace("pocketwatch: stopwatch started");
    }
}

void sw_lap(void) {
    if (!W.sw_running || W.sw_nlaps >= MAX_LAPS) return;
    W.sw_laps[W.sw_nlaps++] = sw_elapsed();
    W.sw_scroll = 0;
    char b[32];
    format_hms(W.sw_laps[W.sw_nlaps - 1], true, b, sizeof b);
    trace("pocketwatch: lap %d at %s", W.sw_nlaps, b);
}

void sw_reset(void) {
    if (W.sw_running) return;
    W.sw_accum_ms = 0;
    W.sw_nlaps = 0;
    W.sw_scroll = 0;
    trace("pocketwatch: stopwatch reset");
}

static void big_text(KCanvas *cv, int cx, int y, const char *s, int scale, uint32_t color, uint32_t shadow) {
    int w = (int)strlen(s) * 8 * scale;
    int x = cx - w / 2;
    kc_text_scaled(cv, x + 2, y + 2, s, shadow, scale);
    kc_text_scaled(cv, x + 1, y, s, color, scale);
    kc_text_scaled(cv, x, y, s, color, scale);
}

void stopwatch_tab(KUI *ui, KRect b) {
    KCanvas *cv = &ui->cv;
    char buf[40];
    format_hms(sw_elapsed(), true, buf, sizeof buf);
    int cells = (int)strlen(buf);
    int scale = 4;
    while (scale > 1 && cells * 8 * scale > b.w - 24) scale--;
    int cx = b.x + b.w / 2;
    int y = b.y + 14;
    KRect panel = kr(cx - (cells * 8 * scale) / 2 - 14, y - 8, cells * 8 * scale + 28, 16 * scale + 16);
    kc_panel(cv, panel, KB_SUNKEN, 0x141B42);
    big_text(cv, cx, y, buf, scale, W.sw_running ? 0xF4EFD8 : 0xB9C2F0, 0x050816);
    W.live = panel;
    y += 16 * scale + 22;

    /* buttons */
    int bw = 104, bh = 30, gap = 10;
    int total = bw * 3 + gap * 2;
    int bx = cx - total / 2;
    if (k_button(ui, kr(bx, y, bw, bh), W.sw_running ? "Stop" : "Start")) sw_toggle();
    bool can_lap = W.sw_running && W.sw_nlaps < MAX_LAPS;
    KRect lapr = kr(bx + bw + gap, y, bw, bh);
    if (can_lap) { if (k_button(ui, lapr, "Lap")) sw_lap(); }
    else { kc_panel(cv, lapr, KB_RAISED, kc_shade(ui->theme->button_bg, 96)); kc_text_in(cv, lapr, "Lap", ui->theme->text_dim, KA_CENTER); }
    KRect rr = kr(bx + 2 * (bw + gap), y, bw, bh);
    if (!W.sw_running) { if (k_button(ui, rr, "Reset")) sw_reset(); }
    else { kc_panel(cv, rr, KB_RAISED, kc_shade(ui->theme->button_bg, 96)); kc_text_in(cv, rr, "Reset", ui->theme->text_dim, KA_CENTER); }
    y += bh + 12;
    kc_text_in(cv, kr(b.x + 10, y, b.w - 20, 16), "Space starts and stops, L takes a lap, R resets", ui->theme->text_dim, KA_CENTER);
    y += 24;

    /* laps, newest first */
    KRect list = kr(b.x + 12, y, b.w - 24, b.y + b.h - y - 8);
    if (list.h < 40) return;
    kc_panel(cv, list, KB_SUNKEN, ui->theme->field_bg);
    KRect in = kr_inset(list, 2);
    kc_fill(cv, kr(in.x, in.y, in.w, 20), kc_shade(ui->theme->window_bg, 96));
    int c0 = in.x + 10, c1 = in.x + in.w * 28 / 100, c2 = in.x + in.w * 64 / 100;
    kc_text_bold(cv, c0, in.y + 2, "Lap", ui->theme->text);
    kc_text_bold(cv, c1, in.y + 2, "Lap time", ui->theme->text);
    kc_text_bold(cv, c2, in.y + 2, "Total", ui->theme->text);
    kc_hline(cv, in.x, in.y + 20, in.w, kc_shade(ui->theme->window_bg, 75));
    KRect rows = kr(in.x, in.y + 21, in.w, in.h - 21);
    const int rowh = 20;
    bool bar = W.sw_nlaps * rowh > rows.h;
    KRect body = bar ? kr(rows.x, rows.y, rows.w - 14, rows.h) : rows;
    int content = W.sw_nlaps * rowh;
    if (k_hover(ui, list) && ui->wheel) W.sw_scroll -= ui->wheel * rowh * 2;
    int maxs = content > body.h ? content - body.h : 0;
    if (W.sw_scroll > maxs) W.sw_scroll = maxs;
    if (W.sw_scroll < 0) W.sw_scroll = 0;

    uint64_t best = ~0ull, worst = 0;
    for (int i = 0; i < W.sw_nlaps; i++) {
        uint64_t lt = W.sw_laps[i] - (i ? W.sw_laps[i - 1] : 0);
        if (lt < best) best = lt;
        if (lt > worst) worst = lt;
    }
    KRect old = cv->clip;
    kc_set_clip(cv, kr_intersect(old, body));
    if (W.sw_nlaps == 0) kc_text_in(cv, kr(body.x + 10, body.y + 4, body.w - 20, 16), "No laps yet.", ui->theme->text_dim, KA_LEFT);
    for (int row = 0; row < W.sw_nlaps; row++) {
        int i = W.sw_nlaps - 1 - row;                             /* newest on top */
        int ry = body.y + row * rowh - W.sw_scroll;
        if (ry + rowh < body.y || ry > body.y + body.h) continue;
        uint64_t lt = W.sw_laps[i] - (i ? W.sw_laps[i - 1] : 0);
        uint32_t col = ui->theme->text;
        if (W.sw_nlaps >= 3 && lt == best) col = 0x1E7A3C;
        else if (W.sw_nlaps >= 3 && lt == worst) col = 0xB5122B;
        if (row & 1) kc_fill(cv, kr(body.x, ry, body.w, rowh), kc_shade(ui->theme->field_bg, 96));
        char s[40];
        snprintf(s, sizeof s, "%d", i + 1);
        kc_text(cv, c0, ry + 2, s, col);
        format_hms(lt, true, s, sizeof s);
        kc_text(cv, c1, ry + 2, s, col);
        format_hms(W.sw_laps[i], true, s, sizeof s);
        kc_text(cv, c2, ry + 2, s, ui->theme->text_dim);
    }
    cv->clip = old;
    if (bar) k_scrollbar(ui, kr(rows.x + rows.w - 14, rows.y, 14, rows.h), &W.sw_scroll, content, body.h);
}
