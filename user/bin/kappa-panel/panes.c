/* The System, Display, Devices, Power and Appearance panes. */
#include "panel.h"
#include <fcntl.h>
#include <gensokyo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <pwd.h>
#include <unistd.h>

/* ---------------------------------------------------------------- System */

static void system_summary(char *out, size_t cap) {
    struct utsname u;
    uname(&u);
    struct gsysinfo si;
    bool have = gap_sysinfo(&si) == 0;
    struct passwd *pw = getpwuid(getuid());
    snprintf(out, cap, "TouhouOS %s \"%s\"\nKernel: %s %s (%s)\nComputer name: %s\nProcessor: %s\nMemory: %lu MiB\nUser: %s\n",
             u.release, u.codename, u.sysname, u.release, u.machine, u.nodename, have ? si.cpu_brand : "unknown",
             have ? (unsigned long)(si.total_ram >> 20) : 0UL, pw ? pw->pw_name : "unknown");
}

void pane_system(KUI *ui, KRect pane) {
    KCanvas *cv = &ui->cv;
    draw_heading(ui, pane, KI_YINYANG, "System", "About this TouhouOS installation");
    KRect b = pane_body(pane);
    struct utsname u;
    uname(&u);
    struct gsysinfo si;
    bool have = gap_sysinfo(&si) == 0;

    /* a banner with the logo */
    KRect banner = kr(b.x + 4, b.y + 4, b.w - 8, 84);
    kc_gradient_v(cv, banner, 0x0E1330, 0x2A3A78);
    kc_outline(cv, banner, 0x0A0F2A);
    k_icon(cv, KI_YINYANG, banner.x + 14, banner.y + 10, 64);
    kc_text_scaled(cv, banner.x + 96, banner.y + 12, "TouhouOS", 0xFFF6E0, 2);
    kc_text_scaled(cv, banner.x + 97, banner.y + 12, "TouhouOS", 0xFFF6E0, 2);
    char line[160];
    snprintf(line, sizeof line, "Gensokyo kernel %s \"%s\"", u.release, u.codename);
    kc_text_in(cv, kr(banner.x + 96, banner.y + 52, banner.w - 104, 16), line, 0xB9C2F0, KA_LEFT);

    int x = b.x + 12, y = banner.y + banner.h + 14, lw = 150, w = b.w - 24;
    char buf[200], a[48], c[48];
    snprintf(buf, sizeof buf, "%s %s (%s)", u.sysname, u.release, u.machine);
    kv_line(ui, x, y, lw, w, "Kernel", buf); y += 20;
    kv_line(ui, x, y, lw, w, "Computer name", u.nodename); y += 20;
    char up[32] = "unknown";
    if (read_file("/proc/uptime", buf, sizeof buf) > 0) fmt_duration((unsigned long long)(atof(buf) * 1000.0), up, sizeof up);
    kv_line(ui, x, y, lw, w, "Uptime", up); y += 20;
    snprintf(buf, sizeof buf, "%s", have ? si.cpu_brand : "unknown");
    if (have && si.cpu_mhz) { char t[48]; snprintf(t, sizeof t, " (%u MHz)", si.cpu_mhz); strncat(buf, t, sizeof buf - strlen(buf) - 1); }
    kv_line(ui, x, y, lw, w, "Processor", buf); y += 20;
    unsigned long total = 0, used = 0;
    char mem[512];
    if (read_file("/proc/meminfo", mem, sizeof mem) > 0) {
        char *p = strstr(mem, "MemTotal:");
        if (p) total = strtoul(p + 9, NULL, 10);
        p = strstr(mem, "MemUsed:");
        if (p) used = strtoul(p + 8, NULL, 10);
    }
    fmt_kb(used, a, sizeof a);
    fmt_kb(total, c, sizeof c);
    snprintf(buf, sizeof buf, "%s used of %s", a, c);
    kv_line(ui, x, y, lw, w, "Memory", buf);
    if (total) k_progress(ui, kr(x + lw, y + 20, w - lw > 280 ? 280 : w - lw, 12), (int)(used * 100 / total));
    y += 40;
    int sw = 0, sh = 0;
    fairy_screen_size(kapp_display(P.app), &sw, &sh);
    snprintf(buf, sizeof buf, "%d x %d pixels", sw, sh);
    kv_line(ui, x, y, lw, w, "Display", buf); y += 20;
    struct passwd *pw = getpwuid(getuid());
    snprintf(buf, sizeof buf, "%s%s%s%s", pw ? pw->pw_name : "unknown", pw && pw->pw_gecos && pw->pw_gecos[0] ? " (" : "", pw && pw->pw_gecos ? pw->pw_gecos : "",
             pw && pw->pw_gecos && pw->pw_gecos[0] ? ")" : "");
    kv_line(ui, x, y, lw, w, "Signed in as", buf); y += 28;

    y += draw_wrapped(ui, x, y, w, "The computer name is kept in /Hakurei/etc/hostname; only root (Yukari) may change it.", ui->theme->text_dim) + 10;
    if (k_button(ui, kr(x, y, 170, 28), "Copy system summary")) {
        char text[512];
        system_summary(text, sizeof text);
        kapp_clipboard_copy(P.app, text, strlen(text));
        trace("kappa-panel: copied the system summary");
    }
}

/* ---------------------------------------------------------------- Display */

void pane_display(KUI *ui, KRect pane) {
    KCanvas *cv = &ui->cv;
    draw_heading(ui, pane, KI_MONITOR, "Display", "The screen and how it is drawn (read only)");
    KRect b = pane_body(pane);
    int sw = 0, sh = 0, wx = 0, wy = 0, ww = 0, wh = 0;
    fairy_screen_size(kapp_display(P.app), &sw, &sh);
    fairy_work_area(kapp_display(P.app), &wx, &wy, &ww, &wh);
    struct gfbinfo fi;
    bool have_fb = false;
    int fd = open("/dev/fb0", O_RDONLY);
    if (fd >= 0) { have_fb = ioctl(fd, FBIOGET_INFO, &fi) == 0; close(fd); }

    /* a little monitor with the right proportions */
    int mw = 150, mh = sw > 0 && sh > 0 ? mw * sh / sw : 94;
    if (mh > 110) { mh = 110; mw = sh ? mh * sw / sh : 150; }
    KRect mon = kr(b.x + 24, b.y + 18, mw, mh);
    kc_fill(cv, kr(mon.x - 8, mon.y - 8, mon.w + 16, mon.h + 16), 0x3A3A44);
    kc_outline(cv, kr(mon.x - 8, mon.y - 8, mon.w + 16, mon.h + 16), 0x15151C);
    kc_gradient_v(cv, mon, 0x1D2A5C, 0x0E1330);
    k_icon(cv, KI_YINYANG, mon.x + mon.w / 2 - 12, mon.y + mon.h / 2 - 20, 24);
    char res[48];
    snprintf(res, sizeof res, "%d x %d", sw, sh);
    kc_text_in(cv, kr(mon.x, mon.y + mon.h / 2 + 6, mon.w, 16), res, 0xF4EFD8, KA_CENTER);
    kc_fill(cv, kr(mon.x + mon.w / 2 - 14, mon.y + mon.h + 8, 28, 6), 0x3A3A44);
    kc_fill(cv, kr(mon.x + mon.w / 2 - 30, mon.y + mon.h + 14, 60, 5), 0x2A2A32);

    int x = b.x + mon.w + 70, y = b.y + 16, lw = 150, w = b.w - (mon.w + 70) - 12;
    char buf[160], t[48];
    snprintf(buf, sizeof buf, "%d x %d pixels", sw, sh);
    kv_line(ui, x, y, lw, w, "Screen size", buf); y += 20;
    snprintf(buf, sizeof buf, "%d x %d pixels", ww, wh);
    kv_line(ui, x, y, lw, w, "Room for windows", buf); y += 20;
    if (have_fb) {
        snprintf(buf, sizeof buf, "%u bits, %s", fi.bpp, (fi.flags & FBINFO_SWAP_RB) ? "RGBX order" : "BGRX order");
        kv_line(ui, x, y, lw, w, "Pixel format", buf); y += 20;
        fmt_bytes((unsigned long long)fi.pitch * fi.height, t, sizeof t);
        snprintf(buf, sizeof buf, "%s (%u bytes per row)", t, fi.pitch);
        kv_line(ui, x, y, lw, w, "Frame buffer", buf); y += 20;
    } else {
        kv_line(ui, x, y, lw, w, "Frame buffer", "cannot be queried"); y += 20;
    }
    kv_line(ui, x, y, lw, w, "Colours", "16.7 million"); y += 20;
    kv_line(ui, x, y, lw, w, "Window server", "Fairies (software)"); y += 20;
    kv_line(ui, x, y, lw, w, "Redraws", "when something changes"); y += 32;

    char cmd[200] = "";
    read_file("/proc/cmdline", cmd, sizeof cmd);
    for (size_t n = strlen(cmd); n && (cmd[n - 1] == '\n' || cmd[n - 1] == ' '); ) cmd[--n] = 0;
    const char *video = strstr(cmd, "video=");
    int ty = b.y + mh + 60;
    if (ty < y) ty = y;
    kc_text_bold(cv, b.x + 12, ty, "Changing the resolution", ui->theme->accent_dark);
    ty += 22;
    ty += draw_wrapped(ui, b.x + 12, ty, b.w - 24,
                       "The screen size is chosen at boot. Add video=WIDTHxHEIGHT (for example video=1280x800) to the kernel "
                       "command line and restart. The window server never resizes the screen itself.", ui->theme->text) + 8;
    snprintf(buf, sizeof buf, "Kernel command line: %s", cmd[0] ? cmd : "(empty)");
    ty += draw_wrapped(ui, b.x + 12, ty, b.w - 24, buf, ui->theme->text_dim);
    if (video) {
        snprintf(buf, sizeof buf, "A video= request is present: %.30s", video);
        draw_wrapped(ui, b.x + 12, ty, b.w - 24, buf, ui->theme->text_dim);
    }
}

/* ---------------------------------------------------------------- Devices */

static Doc g_dev;
static uint64_t g_dev_ms;

Doc *devices_doc(void) { return &g_dev; }

void devices_refresh(void) {
    doc_clear(&g_dev);
    char buf[4096];

    doc_head(&g_dev, "Disks");
    if (read_file("/proc/partitions", buf, sizeof buf) <= 0 || !strchr(buf, '\n')) doc_note(&g_dev, "No disks were found.");
    else {
        static const int xs[6] = { 0, 70, 150, 270, 360, 450 };
        DocRow *h = doc_row(&g_dev, DOC_HEADROW, xs, 6);
        if (h) {
            snprintf(h->text[0], 96, "Name"); snprintf(h->text[1], 96, "Size"); snprintf(h->text[2], 96, "Kind");
            snprintf(h->text[3], 96, "Label"); snprintf(h->text[4], 96, "Driver"); snprintf(h->text[5], 96, "Model");
        }
        for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
            char name[32], parent[32], label[48], driver[24], model[64] = "";
            unsigned long long size, start;
            unsigned ss;
            int ro;
            if (sscanf(line, "%31s %llu %u %d %31s %llu %47s %23s %63[^\n]", name, &size, &ss, &ro, parent, &start, label, driver, model) < 8) continue;
            DocRow *r = doc_row(&g_dev, DOC_ROW, xs, 6);
            if (!r) break;
            char sz[32];
            fmt_bytes(size, sz, sizeof sz);
            snprintf(r->text[0], 96, "%s", name);
            snprintf(r->text[1], 96, "%s", sz);
            snprintf(r->text[2], 96, "%s%s", strcmp(parent, "-") ? "partition of " : "disk", strcmp(parent, "-") ? parent : "");
            snprintf(r->text[3], 96, "%s", strcmp(label, "-") ? label : "");
            snprintf(r->text[4], 96, "%s%s", driver, ro ? " (ro)" : "");
            snprintf(r->text[5], 96, "%s", strcmp(model, "-") ? model : "");
        }
    }
    doc_gap(&g_dev);

    doc_head(&g_dev, "PCI devices");
    if (read_file("/proc/pci", buf, sizeof buf) <= 0) doc_note(&g_dev, "The PCI bus cannot be read (/proc/pci).");
    else {
        static const int xs[4] = { 0, 70, 150, 250 };
        DocRow *h = doc_row(&g_dev, DOC_HEADROW, xs, 4);
        if (h) { snprintf(h->text[0], 96, "Address"); snprintf(h->text[1], 96, "Class"); snprintf(h->text[2], 96, "ID"); snprintf(h->text[3], 96, "Device"); }
        int count = 0;
        for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
            if (line[0] == ' ' || line[0] == '\t') continue;
            char addr[24], cls[24], ids[24];
            int off = 0;
            if (sscanf(line, "%23s %23s %23s %n", addr, cls, ids, &off) < 3 || off <= 0) continue;
            DocRow *r = doc_row(&g_dev, DOC_ROW, xs, 4);
            if (!r) break;
            snprintf(r->text[0], 96, "%s", addr);
            snprintf(r->text[1], 96, "%s", cls);
            snprintf(r->text[2], 96, "%s", ids);
            snprintf(r->text[3], 96, "%s", line + off);
            count++;
        }
        if (!count) doc_note(&g_dev, "No PCI devices were found.");
    }
    doc_gap(&g_dev);

    doc_head(&g_dev, "USB devices");
    if (read_file("/proc/usb", buf, sizeof buf) <= 0) doc_note(&g_dev, "No USB devices are reported (this kernel has no /proc/usb).");
    else {
        int count = 0;
        for (char *line = strtok(buf, "\n"); line && count < 40; line = strtok(NULL, "\n"))
            if (line[0]) { doc_note(&g_dev, "%s", line); count++; }
        if (!count) doc_note(&g_dev, "No USB devices are connected.");
    }
    g_dev_ms = k_now_ms();
    trace("kappa-panel: device list has %d lines", g_dev.n);
}

void pane_devices(KUI *ui, KRect pane) {
    draw_heading(ui, pane, KI_DRIVE, "Devices", "Disks, PCI and USB devices the kernel found");
    KRect b = pane_body(pane);
    if (!g_dev_ms || k_now_ms() - g_dev_ms > 10000) devices_refresh();
    if (k_button(ui, kr(pane.x + pane.w - 92, pane.y + 10, 84, 26), "Refresh")) devices_refresh();
    doc_draw(ui, b, &g_dev);
}

/* ---------------------------------------------------------------- Power */

static void power_answer(bool yes, void *user) {
    int how = (int)(long)user;
    if (!yes) { trace("kappa-panel: %s cancelled", how == REBOOT_POWEROFF ? "shut down" : "restart"); return; }
    if (P.dry_run) { trace("kappa-panel: would %s now", how == REBOOT_POWEROFF ? "shut down" : "restart"); return; }
    trace("kappa-panel: %s", how == REBOOT_POWEROFF ? "shutting down" : "restarting");
    sync();
    gap_reboot(how);
}

static bool tile(KUI *ui, KRect r, KIcon icon, const char *label, const char *hint) {
    KCanvas *cv = &ui->cv;
    uint32_t id = k_id(label, r);
    bool hover = k_hover(ui, r);
    if (hover && ui->pressed[0]) ui->active = id;
    bool clicked = false;
    if (ui->active == id && ui->released[0]) { clicked = hover; ui->active = 0; }
    bool down = ui->active == id && (ui->buttons & 1) && hover;
    kc_panel(cv, r, down ? KB_SUNKEN : KB_RAISED, hover ? kc_shade(ui->theme->button_bg, 106) : ui->theme->button_bg);
    int off = down ? 1 : 0;
    k_icon(cv, icon, r.x + (r.w - 48) / 2 + off, r.y + 14 + off, 48);
    int tw = (int)kc_utf8_len(label) * 8;
    kc_text_bold(cv, r.x + (r.w - tw) / 2 + off, r.y + 70 + off, label, ui->theme->text);
    kc_text_in(cv, kr(r.x + 4 + off, r.y + 92 + off, r.w - 8, 16), hint, ui->theme->text_dim, KA_CENTER);
    return clicked;
}

void pane_power(KUI *ui, KRect pane) {
    KCanvas *cv = &ui->cv;
    draw_heading(ui, pane, KI_POWER, "Power", "Close the shrine for the night, or start the day again");
    KRect b = pane_body(pane);
    int tw = 200, th = 118, gap = 20;
    int x = b.x + 16, y = b.y + 18;
    if (tile(ui, kr(x, y, tw, th), KI_POWER, "Shut down", "Switch the machine off")) {
        trace("kappa-panel: shut down requested");
        k_confirm_dialog(P.app, "Shut down", "Close every program and switch the machine off?\n\nUnsaved work in open programs will be lost.",
                         KI_QUESTION, "Shut down", "Cancel", power_answer, (void *)(long)REBOOT_POWEROFF);
    }
    if (tile(ui, kr(x + tw + gap, y, tw, th), KI_REFRESH, "Restart", "Boot the machine again")) {
        trace("kappa-panel: restart requested");
        k_confirm_dialog(P.app, "Restart", "Close every program and restart the machine?\n\nUnsaved work in open programs will be lost.",
                         KI_QUESTION, "Restart", "Cancel", power_answer, (void *)(long)REBOOT_RESTART);
    }
    y += th + 30;
    k_icon(cv, KI_INFO, x, y, 24);
    int hh = draw_wrapped(ui, x + 34, y + 2, b.w - 60, "Both buttons ask for confirmation first and write out the disks (sync) before the machine goes down.", ui->theme->text);
    draw_wrapped(ui, x + 34, y + 8 + hh, b.w - 60, "To leave only the desktop and keep the machine running, use the Gensokyo menu, then Shut down, then Log out.", ui->theme->text_dim);
}

/* ---------------------------------------------------------------- Appearance */

static const struct { const char *name; uint32_t color; const char *role; } k_palette[] = {
    { "Shrine red", KC_SHRINE, "title bars, accents, the start button" },
    { "Deep shrine", KC_SHRINE_D, "headings, shadows" },
    { "Washi paper", KC_PAPER, "window bodies" },
    { "Aged paper", KC_PAPER_D, "inactive tabs, frames" },
    { "Ink", KC_INK, "text" },
    { "Night blue", KC_NIGHT, "sky, displays" },
    { "Dusk blue", KC_NIGHT_L, "the panel" },
    { "Gold", KC_GOLD, "trim and highlights" },
    { "Moon", KC_MOON, "light text on dark" },
};

static void clock_toggled(void) {
    char err[160];
    if (conf_set("clock24", P.clock24 ? "1" : "0", err, sizeof err)) {
        char path[300] = "";
        conf_path(path, sizeof path);
        snprintf(P.conf_note, sizeof P.conf_note, "Saved clock24=%d to %s", P.clock24 ? 1 : 0, path);
        trace("kappa-panel: saved clock24=%d", P.clock24 ? 1 : 0);
    } else {
        P.clock24 = !P.clock24;                                                   /* it did not stick */
        snprintf(P.conf_note, sizeof P.conf_note, "Could not save: %s", err);
        const char *btn[] = { "OK" };
        k_messagebox(kapp_display(P.app), "Control Panel", err, KI_WARNING, btn, 1);
        trace("kappa-panel: saving failed: %s", err);
    }
}

void pane_appearance(KUI *ui, KRect pane) {
    KCanvas *cv = &ui->cv;
    draw_heading(ui, pane, KI_SKETCH, "Appearance", "How HakureiDE looks, and the clock");
    KRect b = pane_body(pane);
    if (!P.conf_loaded) {
        char v[16];
        P.clock24 = true;
        if (conf_get("clock24", v, sizeof v)) P.clock24 = !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
        P.conf_loaded = true;
    }
    int x = b.x + 12, y = b.y + 8;
    kc_text_bold(cv, x, y, "Theme: Shrine", ui->theme->accent_dark);
    y += 22;
    y += draw_wrapped(ui, x, y, b.w - 24, "Washi paper windows, shrine red accents, night blue panels. Hover over a colour to see where it is used.", ui->theme->text_dim) + 6;
    int sw = (b.w - 24 - 8 * 2) / 3;
    if (sw < 120) sw = 120;
    int cols = (b.w - 24 + 8) / (sw + 8);
    if (cols < 1) cols = 1;
    for (int i = 0; i < (int)(sizeof k_palette / sizeof k_palette[0]); i++) {
        KRect cell = kr(x + (i % cols) * (sw + 8), y + (i / cols) * 40, sw, 34);
        kc_fill(cv, kr(cell.x, cell.y, 30, 30), k_palette[i].color);
        kc_outline(cv, kr(cell.x, cell.y, 30, 30), 0x4A4638);
        kc_text_in(cv, kr(cell.x + 36, cell.y - 1, cell.w - 38, 16), k_palette[i].name, ui->theme->text, KA_LEFT);
        char hex[16];
        snprintf(hex, sizeof hex, "#%06X", (unsigned)k_palette[i].color);
        kc_text_in(cv, kr(cell.x + 36, cell.y + 15, cell.w - 38, 16), hex, ui->theme->text_dim, KA_LEFT);
        if (k_hover(ui, cell)) k_tooltip_text(ui, k_palette[i].role);
    }
    y += ((int)(sizeof k_palette / sizeof k_palette[0]) + cols - 1) / cols * 40 + 6;
    y += draw_wrapped(ui, x, y, b.w - 24,
                      "Text is an 8x16 bitmap font drawn without smoothing. Changing themes is not possible yet; this page only "
                      "describes the one that is installed.", ui->theme->text_dim) + 14;

    int gh = 128;
    k_group(ui, kr(b.x + 6, y, b.w - 12, gh), "Clock");
    bool before = P.clock24;
    k_checkbox(ui, b.x + 24, y + 26, "Show the time in 24-hour format (14:30 instead of 2:30 PM)", &P.clock24);
    if (P.clock24 != before) clock_toggled();
    char path[300] = "", line[420];
    conf_path(path, sizeof path);
    snprintf(line, sizeof line, "Kept in %s as clock24=%d. Sakuya's Pocket Watch follows this at once; the desktop panel will adopt it later.",
             path[0] ? path : "~/.config/hakurei.conf", P.clock24 ? 1 : 0);
    int th = draw_wrapped(ui, b.x + 24, y + 50, b.w - 48, line, ui->theme->text_dim);
    if (P.conf_note[0]) draw_wrapped(ui, b.x + 24, y + 54 + th, b.w - 48, P.conf_note, ui->theme->accent_dark);
}
