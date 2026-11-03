/* The clock tab: the pocket watch face on one side, the digital time and the date on the other. All times follow the system time zone (see timezone(5)). */
#include "watch.h"
#include "dates.h"
#include <stdio.h>
#include <string.h>

void clock_now(struct tm *t, long *ms_in_second) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    time_t secs = W.frozen ? W.frozen_at : ts.tv_sec;
    localtime_r(&secs, t);
    if (ms_in_second) *ms_in_second = ts.tv_nsec / 1000000;
}

void clock_tab(KUI *ui, KRect b) {
    KCanvas *cv = &ui->cv;
    struct tm t;
    clock_now(&t, NULL);
    W.live = b;

    bool wide = b.w >= b.h * 13 / 10;
    KRect face_area, info;
    if (W.frozen) kc_fill_alpha(cv, b, 0x9DB6F0, 40);           /* the room goes pale blue while time is stopped */
    if (wide) {
        int side = b.h - 8;
        if (side > b.w * 54 / 100) side = b.w * 54 / 100;
        face_area = kr(b.x + 4, b.y + 4, side, b.h - 8);
        info = kr(face_area.x + side + 8, b.y + 8, b.x + b.w - (face_area.x + side + 8) - 8, b.h - 16);
    } else {
        int side = b.h * 62 / 100;
        if (side > b.w - 8) side = b.w - 8;
        face_area = kr(b.x + (b.w - side) / 2, b.y + 4, side, side);
        info = kr(b.x + 8, face_area.y + side + 6, b.w - 16, b.y + b.h - (face_area.y + side + 6) - 4);
    }
    int radius = (face_area.w < face_area.h ? face_area.w : face_area.h) / 2 - 24;
    if (radius > 160) radius = 160;
    if (radius < 24) radius = 24;
    int cx = face_area.x + face_area.w / 2, cy = face_area.y + face_area.h / 2 + 6;
    face_draw(cv, cx, cy, radius, &t, W.frozen);

    /* digital time */
    char big[24], ampm[4] = "";
    if (W.clock24) snprintf(big, sizeof big, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    else {
        int h12 = t.tm_hour % 12 ? t.tm_hour % 12 : 12;
        snprintf(big, sizeof big, "%d:%02d:%02d", h12, t.tm_min, t.tm_sec);
        snprintf(ampm, sizeof ampm, "%s", t.tm_hour < 12 ? "AM" : "PM");
    }
    int scale = 3;
    int cells = (int)strlen(big) + (ampm[0] ? 3 : 0);
    while (scale > 1 && cells * 8 * scale > info.w) scale--;
    int dx = info.x + (info.w - cells * 8 * scale) / 2;
    if (dx < info.x) dx = info.x;
    int dy = info.y + (wide ? (info.h - 150) / 2 : 0);
    if (dy < info.y) dy = info.y;
    kc_panel(cv, kr(dx - 10, dy - 6, cells * 8 * scale + 20, 16 * scale + 12), KB_SUNKEN, W.frozen ? 0x1B2A55 : 0x141B42);
    uint32_t tcol = W.frozen ? 0xB8CCFF : 0xF4EFD8;
    kc_text_scaled(cv, dx + 1, dy, big, tcol, scale);
    int adv = kc_text_scaled(cv, dx, dy, big, tcol, scale);
    if (ampm[0]) kc_text_bold(cv, dx + adv + 8, dy + 16 * scale - 18, ampm, 0xD9A441);
    int y = dy + 16 * scale + 14;

    char caption[48];
    const char *zone = t.tm_zone && t.tm_zone[0] ? t.tm_zone : "UTC";
    if (!strcmp(zone, "UTC")) snprintf(caption, sizeof caption, "Universal Time (UTC)");
    else snprintf(caption, sizeof caption, "Local time (%s)", zone);
    kc_text_in(cv, kr(info.x, y, info.w, 16), caption, W.frozen ? 0x3F5088 : ui->theme->accent_dark, KA_CENTER);
    y += 24;
    char date[80];
    snprintf(date, sizeof date, "%s", g_weekday_names[t.tm_wday]);
    int tw = (int)strlen(date) * 8;
    kc_text_bold(cv, info.x + (info.w - tw) / 2, y, date, ui->theme->text);
    y += 20;
    snprintf(date, sizeof date, "%d %s %d", t.tm_mday, g_month_names[t.tm_mon], t.tm_year + 1900);
    kc_text_in(cv, kr(info.x, y, info.w, 16), date, ui->theme->text, KA_CENTER);
    y += 20;
    int iso_y;
    int wk = date_iso_week(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, &iso_y);
    snprintf(date, sizeof date, "Day %d, week %d", t.tm_yday + 1, wk);
    kc_text_in(cv, kr(info.x, y, info.w, 16), date, ui->theme->text_dim, KA_CENTER);
    y += 30;
    if (W.frozen && y + 16 < info.y + info.h) {
        kc_text_in(cv, kr(info.x, y, info.w, 16), "Sakuya has stopped time.", 0x3F5088, KA_CENTER);
        kc_text_in(cv, kr(info.x + 1, y, info.w, 16), "Sakuya has stopped time.", 0x3F5088, KA_CENTER);
        if (y + 20 + 16 < info.y + info.h) kc_text_in(cv, kr(info.x, y + 20, info.w, 16), "Press T to resume.", ui->theme->text_dim, KA_CENTER);
    }
}
