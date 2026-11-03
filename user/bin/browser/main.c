/*
 * Kyouko's browser (browser): the window. A Kappa application: menu bar, toolbar (back, forward, reload or stop, home, a lock for
 * secure pages, the address field, Go), an optional find bar, the page, a scroll bar and a status line. Everything about pages,
 * history and interaction is in the core (core.h); this file turns toolkit events into core calls and shows what comes back.
 *
 *   browser [--trace] [URL | PATH]
 *
 * --trace writes one line per navigation, title change and finished load to standard output (the tests read it). With nothing
 * to open it shows about:home.
 */
#include <kappa.h>
#include <keys.h>

#include "core.h"
#include "urls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MENU_H 24
#define TOOL_H 34
#define FIND_H 30
#define STATUS_H 22
#define SB_W 16

typedef struct Browser {
    KApp *app;
    BrCore *core;
    int scale;
    char addr[BR_MAX_URL], addr_synced[BR_MAX_URL];
    char find[128], find_last[128];
    bool find_open, want_find_focus, want_addr_focus;
    uint32_t *px;                                   /* the page as pixels, at the window's scale */
    int pw, ph;
    int vw, vh;                                     /* the viewport in logical (CSS) pixels */
    char title[300];
    int clicks;
    uint64_t last_click_ms;
    int last_click_x, last_click_y;
    bool dragging, inside_prev;
    bool paint_pending;
    bool trace;
    unsigned traced_loads;
    bool report_load;
    unsigned paints;
    uint64_t paint_ms, paint_max, paint_report_ms;
    char traced_url[BR_MAX_URL];
    int tick_ms;
    int menu_was_open;
} Browser;

static Browser B;

/* ---------------------------------------------------------------- the core's host */

static void host_invalidate(void *user) { ((Browser *)user)->paint_pending = true; }
static void host_clipboard(void *user, const char *text, size_t len) { kapp_clipboard_copy(((Browser *)user)->app, text, len); }
static void on_wake(KApp *app, int fd, void *user) { (void)app; (void)fd; brc_pump(((Browser *)user)->core, k_now_ms()); }

/* ---------------------------------------------------------------- helpers */

static uint32_t addr_field_id(void) { return (uint32_t)(uintptr_t)B.addr * 2654435761u | 1; }   /* how k_textfield names the field it edits */
static uint32_t find_field_id(void) { return (uint32_t)(uintptr_t)B.find * 2654435761u | 1; }

static void focus_address(KUI *ui) {
    k_focus_textfield(ui, B.addr);
    ui->sel_anchor = 0;                                                                    /* all of it selected: typing replaces the address */
    brc_blur_field(B.core);
}

static void open_find(KUI *ui) {
    B.find_open = true;
    k_focus_textfield(ui, B.find);
    ui->sel_anchor = 0;
    brc_blur_field(B.core);
}

static void close_find(KUI *ui) {
    B.find_open = false;
    B.find[0] = B.find_last[0] = 0;
    if (ui->focus == find_field_id()) ui->focus = 0;
    brc_find_done(B.core);
}

static void ensure_surface(int vw, int vh) {
    int pw = vw * B.scale, ph = vh * B.scale;
    if (B.px && pw == B.pw && ph == B.ph) return;
    free(B.px);
    B.px = malloc((size_t)pw * (size_t)ph * 4);
    B.pw = B.px ? pw : 0;
    B.ph = B.px ? ph : 0;
    if (B.px) for (size_t i = 0; i < (size_t)pw * (size_t)ph; i++) B.px[i] = 0xFFFFFF;
    B.paint_pending = true;
}

static unsigned cursor_shape(int web_cursor) {
    switch (web_cursor) {
    case WEB_CURSOR_POINTER: return FC_HAND;
    case WEB_CURSOR_TEXT: return FC_TEXT;
    case WEB_CURSOR_MOVE: case WEB_CURSOR_GRAB: return FC_MOVE;
    case WEB_CURSOR_WAIT: return FC_BUSY;
    case WEB_CURSOR_CROSSHAIR: return FC_CROSS;
    default: return FC_ARROW;
    }
}

static void trace_state(void) {
    if (!B.trace) return;
    const char *u = brc_url(B.core);
    if (strcmp(u, B.traced_url) != 0) { snprintf(B.traced_url, sizeof B.traced_url, "%s", u); printf("browser: url %s\n", u); fflush(stdout); }
    if (brc_loads(B.core) != B.traced_loads) { B.traced_loads = brc_loads(B.core); B.report_load = true; }
    if (B.report_load && !brc_busy(B.core)) {                                  /* the page and everything it asked for have arrived */
        B.report_load = false;
        printf("browser: loaded %s \"%s\" status=%s\n", u, brc_title(B.core), brc_status(B.core));
        fflush(stdout);
    }
    uint64_t now = k_now_ms();
    if (B.paints && now - B.paint_report_ms >= 2000) {                         /* how long the page takes to paint, for judging the speed on a machine */
        printf("browser: paint %u frames, average %u ms, worst %u ms\n", B.paints, (unsigned)(B.paint_ms / B.paints), (unsigned)B.paint_max);
        fflush(stdout);
        B.paints = 0;
        B.paint_ms = 0;
        B.paint_max = 0;
        B.paint_report_ms = now;
    }
}

/* ---------------------------------------------------------------- the toolbar */

static bool tool_button(KUI *ui, KRect r, KIcon icon, bool enabled, const char *tip) {
    KCanvas *cv = &ui->cv;
    bool hit = false;
    if (enabled) hit = k_icon_button(ui, r, icon, false);
    else {
        int isz = r.h >= 28 ? 20 : 16;
        k_icon(cv, icon, r.x + (r.w - isz) / 2, r.y + (r.h - isz) / 2, isz);
        kc_fill_alpha(cv, kr_inset(r, 2), ui->theme->window_bg, 150);                 /* dimmed: nothing to do */
    }
    if (k_hover(ui, r)) k_tooltip_text(ui, tip);
    return hit;
}

static void draw_toolbar(KUI *ui, KRect r) {
    KCanvas *cv = &ui->cv;
    BrCore *c = B.core;
    kc_hline(cv, r.x, r.y + r.h - 1, r.w, kc_shade(ui->theme->window_bg, 72));
    int x = 6, y = r.y + 4;
    if (tool_button(ui, kr(x, y, 28, 26), KI_BACK, brc_can_back(c), "Back (Alt+Left)")) brc_back(c);
    x += 30;
    if (tool_button(ui, kr(x, y, 28, 26), KI_FORWARD, brc_can_forward(c), "Forward (Alt+Right)")) brc_forward(c);
    x += 30;
    if (brc_busy(c)) { if (tool_button(ui, kr(x, y, 28, 26), KI_CLOSE, true, "Stop (Esc)")) brc_stop(c); }
    else if (tool_button(ui, kr(x, y, 28, 26), KI_REFRESH, true, "Reload (F5)")) brc_reload(c);
    x += 30;
    if (tool_button(ui, kr(x, y, 28, 26), KI_HOME, true, "Home")) brc_go(c, "about:home");
    x += 34;
    char sec[200];
    BrSecurity sc = brc_security(c, sec, sizeof sec);
    KRect lock = kr(x, y + 4, 18, 18);
    if (sc == BR_SEC_SECURE) k_icon(cv, KI_LOCK, lock.x, lock.y, 16);
    else if (sc == BR_SEC_PLAIN) k_icon(cv, KI_WARNING, lock.x, lock.y, 16);
    if (sc != BR_SEC_NONE && k_hover(ui, lock)) k_tooltip_text(ui, sec);
    if (sc != BR_SEC_NONE) x += 22;
    int gw = 44;
    KRect field = kr(x, y, r.w - x - gw - 12, 26);
    if (B.want_addr_focus) { focus_address(ui); B.want_addr_focus = false; }
    bool enter = k_textfield(ui, field, B.addr, sizeof B.addr);
    if (enter || k_button(ui, kr(field.x + field.w + 6, y, gw, 26), "Go")) {
        brc_go(c, B.addr);
        snprintf(B.addr, sizeof B.addr, "%s", brc_url(c));
        snprintf(B.addr_synced, sizeof B.addr_synced, "%s", B.addr);
        if (ui->focus == addr_field_id()) ui->focus = 0;
    }
}

static void draw_findbar(KUI *ui, KRect r) {
    KCanvas *cv = &ui->cv;
    BrCore *c = B.core;
    kc_hline(cv, r.x, r.y + r.h - 1, r.w, kc_shade(ui->theme->window_bg, 72));
    k_label(ui, 8, r.y + 7, "Find in page:");
    KRect field = kr(8 + kc_text_width("Find in page:") + 8, r.y + 3, 240, 24);               /* measured: the toolkit's text is proportional now */
    bool enter = k_textfield(ui, field, B.find, sizeof B.find);
    if (B.want_find_focus) { B.want_find_focus = false; }
    if (strcmp(B.find, B.find_last) != 0) { snprintf(B.find_last, sizeof B.find_last, "%s", B.find); brc_find(c, B.find, 0); }
    int dir = 0;
    if (enter) dir = (ui->mods & MOD_SHIFT) ? -1 : 1;
    KRect prev = kr(field.x + field.w + 8, r.y + 3, 74, 24), next = kr(prev.x + prev.w + 4, r.y + 3, 56, 24);
    if (k_button(ui, prev, "Previous")) dir = -1;
    if (k_button(ui, next, "Next")) dir = 1;
    if (dir && B.find[0]) brc_find(c, B.find, dir);
    char info[80] = "";
    if (B.find[0]) {
        int idx = brc_find_index(c);
        if (idx) snprintf(info, sizeof info, "%d of %d", idx, brc_find_count(c));
        else snprintf(info, sizeof info, "Not found");
    }
    k_label_col(ui, next.x + next.w + 12, r.y + 7, info, B.find[0] && !brc_find_index(c) ? 0xA01020 : ui->theme->text_dim);
    if (k_button(ui, kr(r.w - 74, r.y + 3, 66, 24), "Close")) close_find(ui);
}

/* ---------------------------------------------------------------- menus */

static void cb_open_file(const char *path, void *user) {
    (void)user;
    if (path && *path) brc_go(B.core, path);
}

static void cmd_about(void) {
    static const char *const ok[] = { "Close" };
    k_messagebox(kapp_display(B.app), "About the browser",
                 "Kyouko's browser (phase 1: pages without JavaScript).\n\nKyouko Kasodani is the yamabiko, the mountain echo: "
                 "you shout an address and an answer comes back. Hatate looks at the page, Kosuzu will read its scripts, "
                 "and Yukari opens the gaps the packets travel through.\n\nabout:browser lists the keys and the limits.",
                 KI_TORII, ok, 1);
}

static void run_menubar(KUI *ui, int W) {
    static const char *const titles[] = { "File", "Edit", "View", "Go", "Help" };
    KMenuItem file[] = { { "Open Location...", 0, false, false, "Ctrl+L" }, { "Open File...", 0, false, false, "Ctrl+O" }, { "", 0, true, false, NULL }, { "Quit", 0, false, false, "Ctrl+Q" } };
    KMenuItem edit[] = { { "Copy", 0, false, false, "Ctrl+C" }, { "Select All", 0, false, false, "Ctrl+A" }, { "", 0, true, false, NULL }, { "Find in Page...", 0, false, false, "Ctrl+F" } };
    KMenuItem view[] = { { "Reload", 0, false, false, "F5" }, { "Stop", 0, false, !brc_busy(B.core), "Esc" } };
    KMenuItem go[] = { { "Back", 0, false, !brc_can_back(B.core), "Alt+Left" }, { "Forward", 0, false, !brc_can_forward(B.core), "Alt+Right" }, { "Home", 0, false, false, NULL } };
    KMenuItem help[] = { { "Keys and Limits", 0, false, false, NULL }, { "About the Browser", 0, false, false, NULL } };
    const KMenuItem *menus[] = { file, edit, view, go, help };
    int counts[] = { 4, 4, 2, 3, 2 };
    int r = k_menubar(ui, kr(0, 0, W, MENU_H), titles, menus, counts, 5);
    if (!r) return;
    kapp_redraw(B.app);
    int m = (r >> 8) - 1, it = (r & 255) - 1;
    switch (m) {
    case 0:
        if (it == 0) B.want_addr_focus = true;
        else if (it == 1) k_file_dialog(B.app, 0, "Open a page or picture", getenv("HOME") ? getenv("HOME") : "/", NULL, cb_open_file, NULL);
        else if (it == 3) kapp_quit(B.app, 0);
        break;
    case 1:
        if (it == 0) brc_copy(B.core);
        else if (it == 1) brc_select_all(B.core);
        else if (it == 3) { open_find(ui); }
        break;
    case 2: if (it == 0) brc_reload(B.core); else if (it == 1) brc_stop(B.core); break;
    case 3: if (it == 0) brc_back(B.core); else if (it == 1) brc_forward(B.core); else brc_go(B.core, "about:home"); break;
    case 4: if (it == 0) brc_go(B.core, "about:browser"); else cmd_about(); break;
    }
}

/* ---------------------------------------------------------------- input */

static void handle_keys(KUI *ui) {
    BrCore *c = B.core;
    for (int i = 0; i < ui->nkeys; i++) {
        KKey *k = &ui->keys[i];
        if (!k->down || !k->code) continue;
        bool ctrl = (k->mods & MOD_CTRL) != 0, alt = (k->mods & MOD_ALT) != 0, shift = (k->mods & MOD_SHIFT) != 0;
        bool in_field = ui->focus != 0;
        bool used = true;
        if ((ctrl && !alt && k->code == KEY_L) || (!ctrl && !alt && k->code == KEY_F6)) B.want_addr_focus = true;
        else if (alt && !ctrl && k->code == KEY_LEFT) brc_back(c);
        else if (alt && !ctrl && k->code == KEY_RIGHT) brc_forward(c);
        else if (k->code == KEY_F5 || (ctrl && !alt && k->code == KEY_R)) brc_reload(c);
        else if (ctrl && !alt && k->code == KEY_F) open_find(ui);
        else if (k->code == KEY_F3 && B.find[0]) brc_find(c, B.find, shift ? -1 : 1);
        else if (ctrl && !alt && (k->code == KEY_Q || k->code == KEY_W)) kapp_quit(B.app, 0);
        else if (ctrl && !alt && k->code == KEY_O) k_file_dialog(B.app, 0, "Open a page or picture", getenv("HOME") ? getenv("HOME") : "/", NULL, cb_open_file, NULL);
        else if (k->code == KEY_ESC && B.find_open && (in_field || !brc_has_field_focus(c))) close_find(ui);
        else if (k->code == KEY_ESC && brc_busy(c) && !in_field) brc_stop(c);
        else if (!in_field) {
            if (ctrl && !alt && k->code == KEY_V && brc_has_field_focus(c)) {
                size_t n;
                const char *clip = kapp_clipboard(B.app, &n);
                if (clip && n) brc_paste(c, clip, n);
            } else if (ctrl && !alt && k->code == KEY_C) brc_copy(c);
            else used = brc_key(c, k->code, k->mods, k->ch);
        } else used = false;
        if (used) { k->down = 0; k->code = 0; }
    }
}

static void handle_pointer(KUI *ui, KRect view) {
    BrCore *c = B.core;
    bool inside = ui->mouse_inside && !ui->menu_open && kr_contains(view, ui->mx, ui->my);
    int x = ui->mx - view.x, y = ui->my - view.y;
    if (inside && ui->wheel) brc_wheel(c, x, y, ui->wheel);
    if (inside && ui->pressed[0]) {
        if (ui->now_ms - B.last_click_ms < 450 && abs(x - B.last_click_x) < 6 && abs(y - B.last_click_y) < 6) B.clicks = B.clicks >= 3 ? 1 : B.clicks + 1;
        else B.clicks = 1;
        B.last_click_ms = ui->now_ms;
        B.last_click_x = x;
        B.last_click_y = y;
        ui->focus = 0;                                                   /* the page takes the keyboard from the address and find fields */
        brc_pointer_down(c, x, y, 1, B.clicks);
        B.dragging = true;
    }
    if (inside || B.dragging) {
        int cur = brc_pointer_move(c, x, y, ui->buttons);
        if (inside) ui->cursor_shape = cursor_shape(cur);
        B.inside_prev = inside;
    } else if (B.inside_prev) {
        brc_pointer_move(c, -1, -1, 0);                                  /* the pointer left the page: no hover, no link in the status line */
        B.inside_prev = false;
    }
    if (B.dragging && (ui->released[0] || !(ui->buttons & 1))) {
        brc_pointer_up(c, x, y, 1);
        B.dragging = false;
    }
}

/* ---------------------------------------------------------------- the frame */

static void draw(KApp *app, KUI *ui, void *user) {
    (void)user;
    KCanvas *cv = &ui->cv;
    BrCore *c = B.core;
    int W = cv->w, H = cv->h;
    int y = MENU_H + TOOL_H;
    KRect tool = kr(0, MENU_H, W, TOOL_H);
    KRect findr = kr(0, y, W, FIND_H);
    if (B.find_open) y += FIND_H;
    KRect status = kr(0, H - STATUS_H, W, STATUS_H);
    KRect view = kr(0, y, W - SB_W, status.y - y);
    KRect sbar = kr(view.w, y, SB_W, view.h);
    if (view.w < 40) view.w = 40;
    if (view.h < 40) view.h = 40;
    k_clear(ui);
    B.scale = kc_scale(cv);
    if (view.w != B.vw || view.h != B.vh) { B.vw = view.w; B.vh = view.h; brc_set_viewport(c, B.vw, B.vh); }
    ensure_surface(view.w, view.h);
    brc_pump(c, ui->now_ms);

    handle_keys(ui);
    handle_pointer(ui, view);

    if (B.px && (brc_take_dirty(c) || B.paint_pending)) {
        uint64_t t0 = k_now_ms();
        bool painted = brc_render(c, &(BrSurface){ B.px, B.pw, B.ph, B.pw }, ui->now_ms);
        B.paint_pending = !painted;
        if (painted) {
            uint64_t dt = k_now_ms() - t0;
            B.paints++;
            B.paint_ms += dt;
            if (dt > B.paint_max) B.paint_max = dt;
        }
    }
    if (B.px) kc_blit_native(cv, view.x, view.y, B.px, B.pw, B.ph, B.pw);

    int cw, ch;
    brc_content_size(c, &cw, &ch);
    int off = brc_scroll_y(c), before = off;
    k_scrollbar(ui, sbar, &off, ch, view.h);
    if (off != before) brc_scroll_to(c, off);

    const char *url = brc_url(c);
    if (strcmp(url, B.addr_synced) != 0) {
        snprintf(B.addr_synced, sizeof B.addr_synced, "%s", url);
        if (ui->focus != addr_field_id()) snprintf(B.addr, sizeof B.addr, "%s", url);
    }
    draw_toolbar(ui, tool);
    if (B.find_open) draw_findbar(ui, findr);

    kc_panel(cv, status, KB_RAISED, ui->theme->window_bg);
    kc_text_in(cv, kr(status.x + 8, status.y, status.w - 16, status.h), brc_status(c), ui->theme->text, KA_LEFT);

    const char *title = brc_title(c);
    if (strcmp(title, B.title) != 0) { snprintf(B.title, sizeof B.title, "%s", title); kapp_set_title(app, title[0] ? title : "Browser"); }
    trace_state();

    bool busy = brc_busy(c);
    int want_tick = busy ? 100 : 1000;
    if (want_tick != B.tick_ms) { B.tick_ms = want_tick; kapp_set_tick(app, want_tick); }
    if (brc_has_field_focus(c) || busy || B.paint_pending) ui->redraw_soon = true;
    (void)find_field_id;

    run_menubar(ui, W);
    if (B.menu_was_open && !ui->menu_open) kapp_redraw(app);
    B.menu_was_open = ui->menu_open;
}

int main(int argc, char **argv) {
    memset(&B, 0, sizeof B);
    const char *target = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) B.trace = true;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { puts("usage: browser [--trace] [URL | PATH]"); return 0; }
        else if (argv[i][0] == '-' && argv[i][1]) { fprintf(stderr, "browser: unknown option %s\nusage: browser [--trace] [URL | PATH]\n", argv[i]); return 2; }
        else target = argv[i];
    }
    KApp *app = kapp_new("Browser", 880, 620, FW_RESIZABLE);
    if (!app) return 1;
    B.app = app;
    B.scale = kapp_scale(app);
    BrHost host = { &B, host_invalidate, host_clipboard };
    B.core = brc_new(&host, B.scale);
    if (!B.core) { fprintf(stderr, "browser: out of memory\n"); return 1; }
    kapp_set_draw(app, draw, NULL);
    kapp_set_minsize(app, 480, 320);
    kapp_watch_fd(app, brc_wake_fd(B.core), on_wake, &B);
    B.tick_ms = 1000;
    kapp_set_tick(app, 1000);
    B.paint_pending = true;
    brc_go(B.core, target ? target : "about:home");
    int rc = kapp_run(app);
    brc_free(B.core);
    free(B.px);
    kapp_destroy(app);
    return rc;
}
