/* KApp: window, back buffer, event loop; turns window-system events into KUI input. */
#include <kappa.h>
#include <poll.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#define MAX_WATCH 8

struct KApp {
    FairyDisplay *d;
    FairyWindow *win;
    KUI ui;
    uint32_t *back;
    int bw, bh;
    KDrawFn draw;
    void *user;
    int tick_ms;
    uint64_t last_tick;
    bool dirty, quit;
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
    unsigned cursor;
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

static void resize_back(KApp *a, int w, int h) {
    free(a->back);
    a->back = calloc((size_t)w * (size_t)h, 4);
    a->bw = w;
    a->bh = h;
}

KApp *kapp_new_at(const char *title, int x, int y, int w, int h, unsigned flags) {
    FairyDisplay *d = fairy_connect(title);
    if (!d) {
        fprintf(stderr, "%s: no window server answered (is fairyd running?)\n", title);
        return NULL;
    }
    KApp *a = calloc(1, sizeof *a);
    if (!a) { fairy_disconnect(d); return NULL; }
    a->d = d;
    a->win = fairy_window_create_at(d, x, y, w, h, title, flags, NULL);
    if (!a->win) { fairy_disconnect(d); free(a); return NULL; }
    a->bw = a->bh = 0;
    /* The server may have given us a different content size than we asked for; adopt it. */
    int cw, ch, stride;
    fairy_window_pixels(a->win, &cw, &ch, &stride);
    resize_back(a, cw, ch);
    a->ui.theme = &k_theme_shrine;
    a->ui.app = a;
    a->ui.mouse_inside = false;
    a->ui.has_focus = true;
    a->ui.caret_ms = k_now_ms();
    a->dirty = true;
    return a;
}

KApp *kapp_new(const char *title, int w, int h, unsigned flags) { return kapp_new_at(title, -1, -1, w, h, flags); }

void kapp_set_draw(KApp *a, KDrawFn fn, void *user) { a->draw = fn; a->user = user; }
void kapp_set_tick(KApp *a, int ms) { a->tick_ms = ms; a->last_tick = k_now_ms(); }
void kapp_set_key_hook(KApp *a, KKeyFn fn, void *user) { a->keyhook = fn; a->keyuser = user; }
void kapp_set_event_hook(KApp *a, void (*fn)(KApp *, const FairyEvent *, void *), void *user) { a->evhook = fn; a->evuser = user; }
void kapp_set_close_handler(KApp *a, bool (*fn)(KApp *, void *), void *user) { a->on_close = fn; a->close_user = user; }
void kapp_redraw(KApp *a) { a->dirty = true; }
void kapp_quit(KApp *a, int code) { a->quit = true; a->code = code; }
void kapp_set_title(KApp *a, const char *t) { fairy_window_set_title(a->win, t); }
void kapp_set_cursor(KApp *a, unsigned shape) { if (shape != a->cursor) { a->cursor = shape; fairy_window_set_cursor(a->win, shape); } }
void kapp_set_minsize(KApp *a, int w, int h) { fairy_window_set_minsize(a->win, w, h); }
void kapp_move(KApp *a, int x, int y, int w, int h) { fairy_window_move(a->win, x, y, w, h); }
void kapp_set_state(KApp *a, unsigned s) { fairy_window_set_state(a->win, s); }
FairyDisplay *kapp_display(KApp *a) { return a->d; }
FairyWindow *kapp_window(KApp *a) { return a->win; }
int kapp_width(KApp *a) { return a->bw; }
int kapp_height(KApp *a) { return a->bh; }

void kapp_watch_fd(KApp *a, int fd, KFdFn fn, void *user) {
    if (a->nwatch >= MAX_WATCH) return;
    a->watch[a->nwatch].fd = fd;
    a->watch[a->nwatch].fn = fn;
    a->watch[a->nwatch].user = user;
    a->nwatch++;
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
    fairy_window_destroy(a->win);
    fairy_disconnect(a->d);
    free(a->back);
    free(a);
}

/* ---------------------------------------------------------------- one frame */

static void frame(KApp *a) {
    KUI *ui = &a->ui;
    ui->now_ms = k_now_ms();
    ui->redraw_soon = false;
    ui->cursor_shape = FC_ARROW;
    ui->tooltip = NULL;
    ui->hot = 0;
    ui->input_blocked = ui->menu_open != 0;
    kc_init(&ui->cv, a->back, a->bw, a->bh, a->bw);
    if (a->draw) a->draw(a, ui, a->user);

    if (ui->tooltip && ui->tooltip[0]) {
        int tw = kc_text_width(ui->tooltip) + 10;
        int tx = ui->tooltip_x, ty = ui->tooltip_y;
        if (tx + tw > a->bw) tx = a->bw - tw;
        if (ty + 22 > a->bh) ty = ui->my - 26;
        kc_set_clip(&ui->cv, kr(0, 0, a->bw, a->bh));
        kc_panel(&ui->cv, kr(tx, ty, tw, 22), KB_FRAME, 0xFFF8D0);
        kc_text(&ui->cv, tx + 5, ty + 3, ui->tooltip, KC_INK);
    }

    int w, h, stride;
    uint32_t *px = fairy_window_pixels(a->win, &w, &h, &stride);
    if (px && w == a->bw && h == a->bh) {
        for (int y = 0; y < h; y++) memcpy(px + (size_t)y * stride, a->back + (size_t)y * a->bw, (size_t)w * 4);
        fairy_window_commit(a->win, 0, 0, w, h);
    }
    kapp_set_cursor(a, ui->cursor_shape);

    memset(ui->pressed, 0, sizeof ui->pressed);
    memset(ui->released, 0, sizeof ui->released);
    ui->double_click = false;
    ui->wheel = 0;
    ui->nkeys = 0;
}

static void handle_event(KApp *a, FairyEvent *ev) {
    KUI *ui = &a->ui;
    if (a->evhook) a->evhook(a, ev, a->evuser);
    switch (ev->type) {
    case FEV_POINTER: {
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
        if (p->kind == FP_LEAVE) ui->mouse_inside = false;
        break;
    }
    case FEV_KEY: {
        FeKey *k = &ev->u.key;
        ui->mods = k->mods;
        ui->key_down[k->code] = k->down != 0;
        if (a->keyhook) {
            KKey kk = { k->code, k->mods, k->down, k->ch };
            a->keyhook(a, &kk, a->keyuser);
        }
        if (k->down && ui->nkeys < KUI_MAX_KEYS) ui->keys[ui->nkeys++] = (KKey){ k->code, k->mods, k->down, k->ch };
        break;
    }
    case FEV_FOCUS:
        ui->has_focus = ev->u.focus.gained != 0;
        if (!ui->has_focus) { memset(ui->key_down, 0, sizeof ui->key_down); ui->menu_open = 0; }
        else fairy_clipboard_request(a->d);
        break;
    case FEV_CONFIGURE: {
        FeConfigure *c = &ev->u.configure;
        if (c->w > 0 && c->h > 0 && (c->w != a->bw || c->h != a->bh)) {
            if (fairy_window_resize(a->win, c->w, c->h) == 0) resize_back(a, c->w, c->h);
        }
        break;
    }
    case FEV_CLOSE:
        if (!a->on_close || a->on_close(a, a->close_user)) a->quit = true;
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
    a->dirty = true;
}

int kapp_run(KApp *a) {
    if (!a) return 1;
    frame(a);
    a->dirty = false;
    while (!a->quit) {
        struct pollfd pf[1 + MAX_WATCH];
        pf[0].fd = fairy_fd(a->d);
        pf[0].events = POLLIN;
        pf[0].revents = 0;
        for (int i = 0; i < a->nwatch; i++) { pf[1 + i].fd = a->watch[i].fd; pf[1 + i].events = POLLIN; pf[1 + i].revents = 0; }
        int timeout = a->dirty ? 0 : -1;
        uint64_t now = k_now_ms();
        if (!a->dirty) {
            if (a->tick_ms > 0) {
                int64_t left = (int64_t)a->last_tick + a->tick_ms - (int64_t)now;
                timeout = left < 0 ? 0 : (int)left;
            }
            if (a->ui.redraw_soon && (timeout < 0 || timeout > 250)) timeout = 250;
        }
        poll(pf, (nfds_t)(1 + a->nwatch), timeout);
        FairyEvent ev;
        for (int i = 0; i < a->nwatch; i++)
            if (pf[1 + i].revents & (POLLIN | POLLHUP | POLLERR)) { a->watch[i].fn(a, a->watch[i].fd, a->watch[i].user); a->dirty = true; }
        for (int guard = 0; guard < 512; guard++) {
            int r = fairy_next_event(a->d, &ev, 0);
            if (r == 0) break;
            handle_event(a, &ev);
            if (r < 0 || a->quit) break;
        }
        now = k_now_ms();
        bool tick_due = a->tick_ms > 0 && now - a->last_tick >= (uint64_t)a->tick_ms;
        if (tick_due) a->last_tick = now;
        if (a->dirty || tick_due || (a->ui.redraw_soon && timeout == 250)) {
            if (a->quit) break;
            a->dirty = false;
            frame(a);
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
    /* word wrap the message into the space right of the icon */
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
