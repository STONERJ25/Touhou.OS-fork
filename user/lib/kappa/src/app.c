/* KApp / KWin: windows, back buffers, the event loop; turns window-system events into KUI input. */
#include <kappa.h>
#include <poll.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#define MAX_WATCH 8
#define MAX_WINDOWS 12

struct KWin {
    KApp *app;
    FairyWindow *win;
    KUI ui;
    uint32_t *back;
    int bw, bh;
    KDrawFn draw;
    void *user;
    bool dirty, popup, dead;
    unsigned cursor;
    void (*on_close)(KWin *, void *);
    void *close_user;
};

struct KApp {
    FairyDisplay *d;
    KWin *wins[MAX_WINDOWS];
    int nwins;
    KWin *main;
    int tick_ms;
    uint64_t last_tick;
    bool quit;
    int code;
    struct { int fd; KFdFn fn; void *user; } watch[MAX_WATCH];
    int nwatch;
    void (*evhook)(KApp *, const FairyEvent *, void *);
    void *evuser;
    KKeyFn keyhook;
    void *keyuser;
    bool (*on_close)(KApp *, void *);
    void *close_user;
    uint64_t last_click_ms;
    int last_click_x, last_click_y;
    char clip[1025];
    size_t cliplen;
};

uint64_t k_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

const char *k_basename(const char *path) {
    const char *s = strrchr(path, '/');
    return s && s[1] ? s + 1 : path;
}

/* ---------------------------------------------------------------- windows */

static void resize_back(KWin *w, int nw, int nh) {
    free(w->back);
    w->back = calloc((size_t)nw * (size_t)nh, 4);
    w->bw = nw;
    w->bh = nh;
}

static KWin *kwin_create(KApp *a, int x, int y, int w, int h, const char *title, unsigned flags, KDrawFn draw, void *user, bool popup) {
    if (a->nwins >= MAX_WINDOWS) return NULL;
    KWin *kw = calloc(1, sizeof *kw);
    if (!kw) return NULL;
    kw->win = fairy_window_create_at(a->d, x, y, w, h, title, flags, a->main ? a->main->win : NULL);
    if (!kw->win) { free(kw); return NULL; }
    kw->app = a;
    kw->popup = popup;
    kw->draw = draw;
    kw->user = user;
    int cw, ch, stride;
    fairy_window_pixels(kw->win, &cw, &ch, &stride);
    resize_back(kw, cw, ch);
    kw->ui.theme = &k_theme_shrine;
    kw->ui.app = a;
    kw->ui.window = kw;
    kw->ui.has_focus = !popup;
    kw->ui.caret_ms = k_now_ms();
    kw->dirty = true;
    fairy_window_set_user(kw->win, kw);
    a->wins[a->nwins++] = kw;
    return kw;
}

KWin *kwin_new(KApp *a, int x, int y, int w, int h, const char *title, unsigned flags, KDrawFn draw, void *user) {
    return kwin_create(a, x, y, w, h, title, flags, draw, user, (flags & FW_POPUP) != 0);
}

void kwin_set_close_handler(KWin *w, void (*fn)(KWin *, void *), void *user) { w->on_close = fn; w->close_user = user; }
void kwin_redraw(KWin *w) { if (w) w->dirty = true; }
void kwin_close(KWin *w) { if (w) w->dead = true; }
KUI *kwin_ui(KWin *w) { return &w->ui; }
int kwin_width(KWin *w) { return w->bw; }
int kwin_height(KWin *w) { return w->bh; }
FairyWindow *kwin_fairy(KWin *w) { return w->win; }
void kwin_move(KWin *w, int x, int y, int cw, int ch) { fairy_window_move(w->win, x, y, cw, ch); }
void kwin_set_title(KWin *w, const char *t) { fairy_window_set_title(w->win, t); }

static void kwin_destroy(KWin *w) {
    KApp *a = w->app;
    for (int i = 0; i < a->nwins; i++)
        if (a->wins[i] == w) { memmove(&a->wins[i], &a->wins[i + 1], (size_t)(a->nwins - i - 1) * sizeof(KWin *)); a->nwins--; break; }
    if (w == a->main) a->main = NULL;
    fairy_window_destroy(w->win);
    free(w->back);
    free(w);
}

static KApp *kapp_open(FairyDisplay *d, const char *title, int x, int y, int w, int h, unsigned flags);

KApp *kapp_new_at(const char *title, int x, int y, int w, int h, unsigned flags) {
    FairyDisplay *d = fairy_connect(title);
    if (!d) {
        fprintf(stderr, "%s: no window server answered (is fairyd running?)\n", title);
        return NULL;
    }
    return kapp_open(d, title, x, y, w, h, flags);
}

/* A window as large as the screen (desktop, fullscreen games). */
KApp *kapp_new_screen(const char *title, unsigned flags) {
    FairyDisplay *d = fairy_connect(title);
    if (!d) {
        fprintf(stderr, "%s: no window server answered (is fairyd running?)\n", title);
        return NULL;
    }
    int w, h;
    fairy_screen_size(d, &w, &h);
    return kapp_open(d, title, 0, 0, w, h, flags);
}

static KApp *kapp_open(FairyDisplay *d, const char *title, int x, int y, int w, int h, unsigned flags) {
    KApp *a = calloc(1, sizeof *a);
    if (!a) { fairy_disconnect(d); return NULL; }
    a->d = d;
    a->main = kwin_create(a, x, y, w, h, title, flags, NULL, NULL, false);
    if (!a->main) { fairy_disconnect(d); free(a); return NULL; }
    return a;
}

KApp *kapp_new(const char *title, int w, int h, unsigned flags) { return kapp_new_at(title, -1, -1, w, h, flags); }

KWin *kapp_main(KApp *a) { return a->main; }
void kapp_set_draw(KApp *a, KDrawFn fn, void *user) { a->main->draw = fn; a->main->user = user; }
void kapp_set_tick(KApp *a, int ms) { a->tick_ms = ms; a->last_tick = k_now_ms(); }
void kapp_set_key_hook(KApp *a, KKeyFn fn, void *user) { a->keyhook = fn; a->keyuser = user; }
void kapp_set_event_hook(KApp *a, void (*fn)(KApp *, const FairyEvent *, void *), void *user) { a->evhook = fn; a->evuser = user; }
void kapp_set_close_handler(KApp *a, bool (*fn)(KApp *, void *), void *user) { a->on_close = fn; a->close_user = user; }
void kapp_redraw(KApp *a) { for (int i = 0; i < a->nwins; i++) a->wins[i]->dirty = true; }
void kapp_quit(KApp *a, int code) { a->quit = true; a->code = code; }
void kapp_set_title(KApp *a, const char *t) { fairy_window_set_title(a->main->win, t); }
void kapp_set_cursor(KApp *a, unsigned shape) {
    if (shape != a->main->cursor) { a->main->cursor = shape; fairy_window_set_cursor(a->main->win, shape); }
}
void kapp_set_minsize(KApp *a, int w, int h) { fairy_window_set_minsize(a->main->win, w, h); }
void kapp_move(KApp *a, int x, int y, int w, int h) { fairy_window_move(a->main->win, x, y, w, h); }
void kapp_set_state(KApp *a, unsigned s) { fairy_window_set_state(a->main->win, s); }
FairyDisplay *kapp_display(KApp *a) { return a->d; }
FairyWindow *kapp_window(KApp *a) { return a->main->win; }
int kapp_width(KApp *a) { return a->main->bw; }
int kapp_height(KApp *a) { return a->main->bh; }
KUI *kapp_ui(KApp *a) { return &a->main->ui; }

void kapp_watch_fd(KApp *a, int fd, KFdFn fn, void *user) {
    if (a->nwatch >= MAX_WATCH) return;
    a->watch[a->nwatch].fd = fd;
    a->watch[a->nwatch].fn = fn;
    a->watch[a->nwatch].user = user;
    a->nwatch++;
}

void kapp_unwatch_fd(KApp *a, int fd) {
    for (int i = 0; i < a->nwatch; i++)
        if (a->watch[i].fd == fd) { a->watch[i] = a->watch[a->nwatch - 1]; a->nwatch--; return; }
}

void kapp_clipboard_copy(KApp *a, const char *text, size_t len) {
    if (!a) return;
    if (len > sizeof a->clip - 1) len = sizeof a->clip - 1;
    memcpy(a->clip, text, len);
    a->clip[len] = 0;
    a->cliplen = len;
    fairy_clipboard_set(a->d, text, len);
}

const char *kapp_clipboard(KApp *a, size_t *len) {
    if (!a) return NULL;
    if (len) *len = a->cliplen;
    return a->clip;
}

void kapp_clipboard_fetch(KApp *a) { fairy_clipboard_request(a->d); }

void kapp_destroy(KApp *a) {
    if (!a) return;
    while (a->nwins) kwin_destroy(a->wins[a->nwins - 1]);
    fairy_disconnect(a->d);
    free(a);
}

/* ---------------------------------------------------------------- one frame */

static void frame(KWin *kw) {
    KUI *ui = &kw->ui;
    ui->now_ms = k_now_ms();
    ui->redraw_soon = false;
    ui->cursor_shape = FC_ARROW;
    ui->tooltip = NULL;
    ui->hot = 0;
    ui->input_blocked = ui->menu_open != 0;
    kc_init(&ui->cv, kw->back, kw->bw, kw->bh, kw->bw);
    ui->has_damage = false;
    if (kw->draw) kw->draw(kw->app, ui, kw->user);

    bool tooltip_shown = ui->tooltip && ui->tooltip[0];
    if (tooltip_shown) {
        int tw = kc_text_width(ui->tooltip) + 10;
        int tx = ui->tooltip_x, ty = ui->tooltip_y;
        if (tx + tw > kw->bw) tx = kw->bw - tw;
        if (ty + 22 > kw->bh) ty = ui->my - 26;
        kc_set_clip(&ui->cv, kr(0, 0, kw->bw, kw->bh));
        kc_panel(&ui->cv, kr(tx, ty, tw, 22), KB_FRAME, 0xFFF8D0);
        kc_text(&ui->cv, tx + 5, ty + 3, ui->tooltip, KC_INK);
    }

    int w, h, stride;
    uint32_t *px = fairy_window_pixels(kw->win, &w, &h, &stride);
    if (px && w == kw->bw && h == kw->bh) {
        KRect d = ui->has_damage ? kr_intersect(ui->damage, kr(0, 0, w, h)) : kr(0, 0, w, h);
        if (tooltip_shown) d = kr(0, 0, w, h);
        if (!kr_empty(d)) {
            for (int y = d.y; y < d.y + d.h; y++) memcpy(px + (size_t)y * stride + d.x, kw->back + (size_t)y * kw->bw + d.x, (size_t)d.w * 4);
            fairy_window_commit(kw->win, d.x, d.y, d.w, d.h);
        }
    }
    ui->has_damage = false;
    if (ui->cursor_shape != kw->cursor) { kw->cursor = ui->cursor_shape; fairy_window_set_cursor(kw->win, kw->cursor); }

    memset(ui->pressed, 0, sizeof ui->pressed);
    memset(ui->released, 0, sizeof ui->released);
    ui->double_click = false;
    ui->wheel = 0;
    ui->nkeys = 0;
}

static void push_key(KUI *ui, const FeKey *k) {
    ui->mods = k->mods;
    ui->key_down[k->code] = k->down != 0;
    if (k->down && ui->nkeys < KUI_MAX_KEYS) ui->keys[ui->nkeys++] = (KKey){ k->code, k->mods, k->down, k->ch };
}

static void handle_event(KApp *a, FairyEvent *ev) {
    if (a->evhook) a->evhook(a, ev, a->evuser);
    KWin *kw = ev->window ? fairy_window_user(ev->window) : NULL;
    switch (ev->type) {
    case FEV_POINTER: {
        if (!kw) break;
        KUI *ui = &kw->ui;
        FePointer *p = &ev->u.pointer;
        ui->mx = p->x;
        ui->my = p->y;
        ui->mods = p->mods;
        ui->mouse_inside = p->kind != FP_LEAVE;
        if (p->kind == FP_PRESS || p->kind == FP_RELEASE) {
            int b = p->button >= 1 && p->button <= 3 ? p->button - 1 : 0;
            if (p->kind == FP_PRESS) {
                ui->pressed[b] = true;
                if (b == 0) {
                    uint64_t now = k_now_ms();
                    if (now - a->last_click_ms < 400 && abs(p->x - a->last_click_x) < 6 && abs(p->y - a->last_click_y) < 6) {
                        ui->double_click = true;
                        a->last_click_ms = 0;
                    } else {
                        a->last_click_ms = now;
                        a->last_click_x = p->x;
                        a->last_click_y = p->y;
                    }
                }
            } else ui->released[b] = true;
        }
        if (p->kind == FP_WHEEL) ui->wheel += p->wheel;
        ui->buttons = p->buttons;
        kw->dirty = true;
        break;
    }
    case FEV_KEY: {
        FeKey *k = &ev->u.key;
        if (a->keyhook) {
            KKey kk = { k->code, k->mods, k->down, k->ch };
            a->keyhook(a, &kk, a->keyuser);
        }
        if (kw) { push_key(&kw->ui, k); kw->dirty = true; }
        for (int i = 0; i < a->nwins; i++)                      /* popups never get the focus: they see the keys of their owner */
            if (a->wins[i]->popup) { push_key(&a->wins[i]->ui, k); a->wins[i]->dirty = true; }
        break;
    }
    case FEV_FOCUS:
        if (!kw) break;
        kw->ui.has_focus = ev->u.focus.gained != 0;
        if (!kw->ui.has_focus) { memset(kw->ui.key_down, 0, sizeof kw->ui.key_down); kw->ui.menu_open = 0; }
        else fairy_clipboard_request(a->d);
        kw->dirty = true;
        break;
    case FEV_CONFIGURE: {
        if (!kw) break;
        FeConfigure *c = &ev->u.configure;
        if (c->w > 0 && c->h > 0 && (c->w != kw->bw || c->h != kw->bh)) {
            if (fairy_window_resize(kw->win, c->w, c->h) == 0) resize_back(kw, c->w, c->h);
        }
        kw->dirty = true;
        break;
    }
    case FEV_CLOSE:
        if (!kw) break;
        if (kw == a->main) { if (!a->on_close || a->on_close(a, a->close_user)) a->quit = true; }
        else { if (kw->on_close) kw->on_close(kw, kw->close_user); kw->dead = true; }
        break;
    case FEV_CLIPBOARD: {
        size_t n = ev->u.clipboard.len;
        if (n > sizeof a->clip - 1) n = sizeof a->clip - 1;
        memcpy(a->clip, ev->u.clipboard.data, n);
        a->clip[n] = 0;
        a->cliplen = n;
        break;
    }
    case FEV_DISCONNECTED:
        a->quit = true;
        break;
    default:
        break;
    }
}

static bool any_dirty(KApp *a) {
    for (int i = 0; i < a->nwins; i++) if (a->wins[i]->dirty) return true;
    return false;
}

static void reap_closed(KApp *a) {
    for (int i = a->nwins - 1; i >= 0; i--) {
        KWin *w = a->wins[i];
        if (w->dead && w != a->main) kwin_destroy(w);
    }
}

int kapp_run(KApp *a) {
    if (!a || !a->main) return 1;
    for (int i = 0; i < a->nwins; i++) { frame(a->wins[i]); a->wins[i]->dirty = false; }
    while (!a->quit) {
        struct pollfd pf[1 + MAX_WATCH];
        pf[0].fd = fairy_fd(a->d);
        pf[0].events = POLLIN;
        pf[0].revents = 0;
        for (int i = 0; i < a->nwatch; i++) { pf[1 + i].fd = a->watch[i].fd; pf[1 + i].events = POLLIN; pf[1 + i].revents = 0; }
        int timeout = any_dirty(a) ? 0 : -1;
        uint64_t now = k_now_ms();
        bool soon = false;
        for (int i = 0; i < a->nwins; i++) if (a->wins[i]->ui.redraw_soon) soon = true;
        if (timeout != 0) {
            if (a->tick_ms > 0) {
                int64_t left = (int64_t)a->last_tick + a->tick_ms - (int64_t)now;
                timeout = left < 0 ? 0 : (int)left;
            }
            if (soon && (timeout < 0 || timeout > 250)) timeout = 250;
        }
        poll(pf, (nfds_t)(1 + a->nwatch), timeout);
        FairyEvent ev;
        for (int i = 0; i < a->nwatch; i++)
            if (pf[1 + i].revents & (POLLIN | POLLHUP | POLLERR)) { a->watch[i].fn(a, a->watch[i].fd, a->watch[i].user); kapp_redraw(a); }
        for (int guard = 0; guard < 512; guard++) {
            int r = fairy_next_event(a->d, &ev, 0);
            if (r == 0) break;
            handle_event(a, &ev);
            if (r < 0 || a->quit) break;
        }
        if (a->quit) break;
        now = k_now_ms();
        bool tick_due = a->tick_ms > 0 && now - a->last_tick >= (uint64_t)a->tick_ms;
        if (tick_due) { a->last_tick = now; kapp_redraw(a); }
        reap_closed(a);
        for (int i = 0; i < a->nwins; i++) {
            KWin *w = a->wins[i];
            if (w->dirty || (w->ui.redraw_soon && timeout == 250)) {
                w->dirty = false;
                frame(w);
            }
        }
    }
    return a->code;
}

/* ---------------------------------------------------------------- message box */

typedef struct { const char *msg; KIcon icon; const char *const *buttons; int n; int result; } MsgBox;

static void msgbox_draw(KApp *app, KUI *ui, void *user) {
    MsgBox *m = user;
    k_clear(ui);
    int w = kapp_width(app);
    k_icon(&ui->cv, m->icon, 16, 16, 32);
    int maxcols = (w - 72 - 12) / 8;
    int y = 16;
    const char *p = m->msg;
    while (*p) {
        const char *line = p;
        int cols = 0, last_space = -1;
        const char *q = p;
        while (*q && *q != '\n' && cols < maxcols) { if (*q == ' ') last_space = (int)(q - p); q++; cols++; }
        size_t len = (size_t)(q - p);
        if (*q && *q != '\n' && last_space > 0) len = (size_t)last_space;
        kc_text_n(&ui->cv, 60, y, line, len, ui->theme->text);
        p += len;
        if (*p == ' ' || *p == '\n') p++;
        y += 18;
    }
    int bw = 84, gap = 10, total = m->n * bw + (m->n - 1) * gap;
    int x = (w - total) / 2;
    int by = kapp_height(app) - 40;
    for (int i = 0; i < m->n; i++, x += bw + gap)
        if (k_button(ui, kr(x, by, bw, 26), m->buttons[i])) { m->result = i; kapp_quit(app, 0); }
    if (k_key_pressed(ui, KEY_ESC)) { m->result = -1; kapp_quit(app, 0); }
    if (k_key_pressed(ui, KEY_ENTER) && m->n > 0) { m->result = 0; kapp_quit(app, 0); }
}

int k_messagebox(FairyDisplay *d, const char *title, const char *message, KIcon icon, const char *const *buttons, int n) {
    (void)d;
    int lines = 1;
    int maxcols = 40;
    size_t len = strlen(message);
    lines += (int)(len / (size_t)maxcols);
    for (const char *c = message; *c; c++) if (*c == '\n') lines++;
    int h = 100 + lines * 18;
    if (h < 140) h = 140;
    KApp *a = kapp_new(title, 440, h, FW_DIALOG | FW_CENTER | FW_TOP);
    if (!a) return -1;
    MsgBox m = { message, icon, buttons, n, -1 };
    kapp_set_draw(a, msgbox_draw, &m);
    kapp_run(a);
    kapp_destroy(a);
    return m.result;
}
