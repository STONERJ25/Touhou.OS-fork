#include "panel.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void trace(const char *fmt, ...) {
    if (!P.trace) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

ssize_t read_file(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t n = 0;
    while (n + 1 < cap) {
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r < 0) { close(fd); return -1; }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = 0;
    return (ssize_t)n;
}

void fmt_bytes(unsigned long long b, char *out, size_t cap) {
    if (b < 1024) snprintf(out, cap, "%llu B", b);
    else if (b < 1024ull * 1024) snprintf(out, cap, "%llu.%llu KiB", b >> 10, (b & 1023) * 10 >> 10);
    else if (b < 1024ull * 1024 * 1024) snprintf(out, cap, "%llu.%llu MiB", b >> 20, (b & 0xFFFFF) * 10 >> 20);
    else snprintf(out, cap, "%llu.%llu GiB", b >> 30, (b & 0x3FFFFFFF) * 10 >> 30);
}

void fmt_kb(unsigned long kb, char *out, size_t cap) {
    if (kb < 1024) snprintf(out, cap, "%lu kB", kb);
    else if (kb < 1024 * 1024) snprintf(out, cap, "%lu.%lu MB", kb >> 10, (kb & 1023) * 10 >> 10);
    else snprintf(out, cap, "%lu.%lu GB", kb >> 20, (kb & 0xFFFFF) * 10 >> 20);
}

void fmt_duration(unsigned long long ms, char *out, size_t cap) {
    unsigned long long s = ms / 1000;
    if (s < 60) snprintf(out, cap, "%llu seconds", s);
    else if (s < 3600) snprintf(out, cap, "%llu min %llu s", s / 60, s % 60);
    else if (s < 86400) snprintf(out, cap, "%llu h %llu min", s / 3600, s / 60 % 60);
    else snprintf(out, cap, "%llu d %llu h", s / 86400, s / 3600 % 24);
}

/* The title of a pane: an icon, a bold heading and a line saying what the pane is for. */
void draw_heading(KUI *ui, KRect pane, KIcon icon, const char *title, const char *subtitle) {
    KCanvas *cv = &ui->cv;
    k_icon(cv, icon, pane.x + 8, pane.y + 6, 32);
    kc_text_bold(cv, pane.x + 52, pane.y + 6, title, ui->theme->accent_dark);
    kc_text_in(cv, kr(pane.x + 52, pane.y + 24, pane.w - 60, 16), subtitle, ui->theme->text_dim, KA_LEFT);
    kc_hline(cv, pane.x + 4, pane.y + 46, pane.w - 8, kc_shade(ui->theme->window_bg, 78));
    kc_hline(cv, pane.x + 4, pane.y + 47, pane.w - 8, 0xFFFFFF);
}

KRect pane_body(KRect pane) { return kr(pane.x + 4, pane.y + 54, pane.w - 8, pane.h - 58); }

/* Word-wrapped text; returns the height used. */
int draw_wrapped(KUI *ui, int x, int y, int w, const char *text, uint32_t color) {
    KCanvas *cv = &ui->cv;
    int cols = w / 8;
    if (cols < 4) cols = 4;
    const char *p = text;
    int lines = 0;
    while (*p) {
        int count = 0;
        const char *q = p, *last_space = NULL;
        while (*q && *q != '\n' && count < cols) {
            uint32_t cp;
            int b = kc_utf8_next(q, &cp);
            if (b <= 0) break;
            if (cp == ' ') last_space = q;
            q += b;
            count++;
        }
        const char *end = q;
        if (*q && *q != '\n' && last_space && last_space > p) end = last_space;
        kc_text_n(cv, x, y + lines * 18, p, (size_t)(end - p), color);
        p = end;
        if (*p == ' ' || *p == '\n') p++;
        lines++;
    }
    return lines * 18;
}

void kv_line(KUI *ui, int x, int y, int label_w, int w, const char *key, const char *value) {
    kc_text_in(&ui->cv, kr(x, y, label_w - 6, 16), key, ui->theme->text_dim, KA_LEFT);
    kc_text_in(&ui->cv, kr(x + label_w, y, w - label_w, 16), value, ui->theme->text, KA_LEFT);
}
