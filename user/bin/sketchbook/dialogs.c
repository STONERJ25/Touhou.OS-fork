/* Our own dialogs: new picture / resize canvas, messages, questions, About and the shortcut list. They are windows of the
 * main application (kwin_new) that call back when the user decides. Closed dialogs are freed a frame later, because the
 * window server may still deliver one more event to a window we have just closed. */
#include "sketchbook.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { DK_SIZE, DK_MESSAGE, DK_ASK, DK_ABOUT, DK_SHORTCUTS } DlgKind;

typedef struct Dlg {
    struct Dlg *next;
    KWin *win;
    DlgKind kind;
    bool closed;
    uint32_t closed_iter;
    bool resize, stretch, focused, adjusted;          /* size dialog */
    int which;
    char wbuf[8], hbuf[8];
    char err[112];
    char text[640];                                   /* message / question */
    KIcon icon;
    char b[3][24];
    int nb;
    AskCb cb;
    void *user;
} Dlg;

static Dlg *g_dlgs;

void dlg_modal_enter(void) { S.modal++; }
void dlg_modal_leave(void) { if (S.modal > 0) S.modal--; }

static void dlg_close(Dlg *d) {
    if (d->closed) return;
    d->closed = true;
    d->closed_iter = S.iter;
    kwin_close(d->win);
    dlg_modal_leave();
}

static void ask_finish(Dlg *d, int choice) {
    AskCb cb = d->cb;
    void *user = d->user;
    d->cb = NULL;
    dlg_close(d);
    if (cb) cb(choice, user);
}

static void dlg_closed_by_server(KWin *w, void *user) {
    (void)w;
    Dlg *d = user;
    if (d->closed) return;
    if (d->kind == DK_ASK) ask_finish(d, d->nb - 1);
    else dlg_close(d);
}

void dlg_gc(void) {
    for (Dlg **pp = &g_dlgs; *pp;) {
        Dlg *d = *pp;
        if (d->closed && S.iter > d->closed_iter) { *pp = d->next; free(d); }
        else pp = &d->next;
    }
}

static Dlg *dlg_create(DlgKind kind, const char *title, int w, int h, KDrawFn draw) {
    Dlg *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->kind = kind;
    d->win = kwin_new(S.app, -1, -1, w, h, title, FW_DIALOG | FW_CENTER | FW_TOP, draw, d);
    if (!d->win) { free(d); return NULL; }
    kwin_set_close_handler(d->win, dlg_closed_by_server, d);
    d->next = g_dlgs;
    g_dlgs = d;
    dlg_modal_enter();
    return d;
}

/* word-wrapped text; returns the y below the last line */
static int draw_wrapped(KCanvas *cv, int x, int y, int maxcols, int ymax, const char *p, uint32_t color) {
    if (maxcols < 8) maxcols = 8;
    while (*p && y < ymax) {
        int cols = 0, last_space = -1;
        const char *q = p;
        while (*q && *q != '\n' && cols < maxcols) { if (*q == ' ') last_space = (int)(q - p); q++; cols++; }
        size_t len = (size_t)(q - p);
        if (*q && *q != '\n' && last_space > 0) len = (size_t)last_space;
        kc_text_n(cv, x, y, p, len, color);
        p += len;
        if (*p == ' ' || *p == '\n') p++;
        y += 18;
    }
    return y;
}

static int wrapped_lines(const char *p, int maxcols) {
    int lines = 0;
    while (*p) {
        int cols = 0, last_space = -1;
        const char *q = p;
        while (*q && *q != '\n' && cols < maxcols) { if (*q == ' ') last_space = (int)(q - p); q++; cols++; }
        size_t len = (size_t)(q - p);
        if (*q && *q != '\n' && last_space > 0) len = (size_t)last_space;
        p += len;
        if (*p == ' ' || *p == '\n') p++;
        lines++;
    }
    return lines ? lines : 1;
}

/* ---------------------------------------------------------------- new picture / resize canvas */

static void size_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    Dlg *d = user;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    if (k_key_pressed(ui, KEY_ESC)) { dlg_close(d); return; }
    if (!d->focused) { k_focus_textfield(ui, d->wbuf); d->focused = true; d->which = 0; }
    int w = cv->w;
    kc_text_bold(cv, 16, 14, d->resize ? "New size of the canvas" : "Size of the new picture", ui->theme->accent_dark);
    int y = 42;
    k_label(ui, 16, y + 4, "Width");
    bool e1 = k_textfield(ui, kr(68, y, 76, 24), d->wbuf, sizeof d->wbuf);
    k_label(ui, 164, y + 4, "Height");
    bool e2 = k_textfield(ui, kr(220, y, 76, 24), d->hbuf, sizeof d->hbuf);
    k_label(ui, 304, y + 4, "pixels");
    if (k_key_pressed(ui, KEY_TAB)) { d->which ^= 1; k_focus_textfield(ui, d->which ? d->hbuf : d->wbuf); }
    y += 40;
    if (d->resize) {
        char cur[64];
        snprintf(cur, sizeof cur, "Now %d x %d. New space is filled with the background colour.", g_doc.w, g_doc.h);
        kc_text_in(cv, kr(16, y, w - 32, 16), cur, ui->theme->text_dim, KA_LEFT);
        y += 24;
        k_checkbox(ui, 16, y, "Stretch the picture to the new size", &d->stretch);
        y += 26;
    } else {
        static const struct { int w, h; } presets[4] = { { 320, 240 }, { 640, 480 }, { 800, 600 }, { 1024, 768 } };
        for (int i = 0; i < 4; i++) {
            char l[24];
            snprintf(l, sizeof l, "%dx%d", presets[i].w, presets[i].h);
            if (k_button(ui, kr(16 + i * 86, y, 82, 24), l)) {
                snprintf(d->wbuf, sizeof d->wbuf, "%d", presets[i].w);
                snprintf(d->hbuf, sizeof d->hbuf, "%d", presets[i].h);
                d->err[0] = 0;
            }
        }
        y += 34;
    }
    if (d->err[0]) kc_text_in(cv, kr(16, y, w - 32, 16), d->err, ui->theme->accent, KA_LEFT);
    int by = cv->h - 40;
    bool ok = k_button(ui, kr(w - 2 * 88 - 20, by, 88, 26), "OK") || e1 || e2;
    bool cancel = k_button(ui, kr(w - 88 - 12, by, 88, 26), "Cancel");
    if (cancel) { dlg_close(d); return; }
    if (!ok) return;
    int nw = atoi(d->wbuf), nh = atoi(d->hbuf);
    if (nw < 1 || nh < 1) { snprintf(d->err, sizeof d->err, "Width and height must be at least 1 pixel."); return; }
    if ((nw > SK_MAX_DIM || nh > SK_MAX_DIM) && !d->adjusted) {
        nw = sk_min(nw, SK_MAX_DIM);
        nh = sk_min(nh, SK_MAX_DIM);
        snprintf(d->wbuf, sizeof d->wbuf, "%d", nw);
        snprintf(d->hbuf, sizeof d->hbuf, "%d", nh);
        snprintf(d->err, sizeof d->err, "The largest picture is %d x %d; press OK to use %d x %d.", SK_MAX_DIM, SK_MAX_DIM, nw, nh);
        d->adjusted = true;
        return;
    }
    nw = sk_min(nw, SK_MAX_DIM);
    nh = sk_min(nh, SK_MAX_DIM);
    bool resize = d->resize, stretch = d->stretch;
    dlg_close(d);
    if (resize) app_resize_canvas(nw, nh, stretch);
    else app_new_picture(nw, nh);
}

static void open_size_dialog(bool resize) {
    Dlg *d = dlg_create(DK_SIZE, resize ? "Resize Canvas" : "New Picture", 380, resize ? 214 : 196, size_draw);
    if (!d) return;
    d->resize = resize;
    snprintf(d->wbuf, sizeof d->wbuf, "%d", resize ? g_doc.w : SK_DEF_W);
    snprintf(d->hbuf, sizeof d->hbuf, "%d", resize ? g_doc.h : SK_DEF_H);
}

void dlg_new_image(void) { open_size_dialog(false); }
void dlg_resize_canvas(void) { open_size_dialog(true); }

/* ---------------------------------------------------------------- messages and questions */

static void message_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    Dlg *d = user;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    k_icon(cv, d->icon, 16, 16, 32);
    draw_wrapped(cv, 62, 16, (cv->w - 78) / 8, cv->h - 50, d->text, ui->theme->text);
    if (k_button(ui, kr(cv->w - 100, cv->h - 40, 84, 26), "OK") || k_key_pressed(ui, KEY_ENTER) || k_key_pressed(ui, KEY_ESC)) dlg_close(d);
}

void dlg_message(const char *title, const char *text, KIcon icon) {
    int lines = wrapped_lines(text, 46);
    int h = sk_clamp(96 + lines * 18, 132, 420);
    Dlg *d = dlg_create(DK_MESSAGE, title, 460, h, message_draw);
    if (!d) return;
    snprintf(d->text, sizeof d->text, "%s", text);
    d->icon = icon;
}

static void ask_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    Dlg *d = user;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    k_icon(cv, d->icon, 16, 16, 32);
    draw_wrapped(cv, 62, 16, (cv->w - 78) / 8, cv->h - 50, d->text, ui->theme->text);
    int bw = 96, gap = 10, total = d->nb * bw + (d->nb - 1) * gap;
    int x = cv->w - total - 16;
    for (int i = 0; i < d->nb; i++, x += bw + gap) {
        if (k_button(ui, kr(x, cv->h - 40, bw, 26), d->b[i])) { ask_finish(d, i); return; }
    }
    if (k_key_pressed(ui, KEY_ENTER)) { ask_finish(d, 0); return; }
    if (k_key_pressed(ui, KEY_ESC)) ask_finish(d, d->nb - 1);
}

void dlg_ask(const char *title, const char *text, KIcon icon, const char *b0, const char *b1, const char *b2, AskCb cb, void *user) {
    int lines = wrapped_lines(text, 46);
    int h = sk_clamp(96 + lines * 18, 132, 420);
    Dlg *d = dlg_create(DK_ASK, title, 460, h, ask_draw);
    if (!d) { if (cb) cb(b2 ? 2 : b1 ? 1 : 0, user); return; }
    snprintf(d->text, sizeof d->text, "%s", text);
    d->icon = icon;
    const char *bs[3] = { b0, b1, b2 };
    for (int i = 0; i < 3; i++) if (bs[i]) { snprintf(d->b[d->nb], sizeof d->b[0], "%s", bs[i]); d->nb++; }
    d->cb = cb;
    d->user = user;
}

/* ---------------------------------------------------------------- About and shortcuts */

static void about_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    Dlg *d = user;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    kc_gradient_v(cv, kr(0, 0, cv->w, 92), 0x0E1330, 0x3A2A78);
    k_icon(cv, KI_SKETCH, 24, 14, 64);
    kc_text_scaled(cv, 108, 20, "Sketchbook", 0xFFF6E0, 2);
    kc_text(cv, 108, 56, "Marisa's drawing book", 0xB9C2F0);
    k_label(ui, 24, 106, "A small paint program: pencil, brush, spray can,");
    k_label(ui, 24, 124, "shapes, text, selections and flood fill, zoom up");
    k_label(ui, 24, 142, "to 8x with a pixel grid. Opens and saves 24-bit");
    k_label(ui, 24, 160, "BMP and binary PPM pictures (P6).");
    kc_text(cv, 24, 188, "\"I only borrowed the colours, ze. I'll return them", ui->theme->accent_dark);
    kc_text(cv, 24, 206, " when I'm dead.\"  - M. Kirisame, probably", ui->theme->accent_dark);
    k_separator(ui, kr(16, 232, cv->w - 32, 2));
    kc_text(cv, 24, 242, "Press F1 for the list of keyboard shortcuts.", ui->theme->text_dim);
    if (k_button(ui, kr(cv->w - 100, cv->h - 40, 84, 26), "OK") || k_key_pressed(ui, KEY_ENTER) || k_key_pressed(ui, KEY_ESC)) dlg_close(d);
}

void dlg_about(void) { dlg_create(DK_ABOUT, "About Sketchbook", 440, 320, about_draw); }

static void shortcuts_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    Dlg *d = user;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    static const char *const left[] = {
        "Tools", "P  Pencil", "B  Brush", "E  Eraser", "A  Spray can", "L  Line", "R  Rectangle", "O  Ellipse", "F  Fill",
        "I  Colour picker", "T  Text", "S  Select", "", "[ ]  Smaller / larger brush", "X  Swap the colours",
    };
    static const char *const right[] = {
        "Picture", "Ctrl+N  New", "Ctrl+O  Open", "Ctrl+S  Save", "Ctrl+Shift+S  Save as", "Ctrl+Z  Undo", "Ctrl+Y  Redo",
        "Ctrl+X C V  Cut, copy, paste", "Ctrl+A  Select all", "Delete  Clear selection", "Esc  Cancel or deselect",
        "", "+ -  Zoom in and out", "1 to 8  Zoom 100% to 800%", "G  Pixel grid",
    };
    for (int i = 0; i < 15; i++) {
        if (left[i][0]) { if (i == 0) kc_text_bold(cv, 20, 14, left[i], ui->theme->accent_dark); else kc_text(cv, 20, 14 + i * 18, left[i], ui->theme->text); }
        if (right[i][0]) { if (i == 0) kc_text_bold(cv, 270, 14, right[i], ui->theme->accent_dark); else kc_text(cv, 270, 14 + i * 18, right[i], ui->theme->text); }
    }
    kc_text(cv, 20, 14 + 16 * 18, "Shift while dragging: straight lines, squares, circles.", ui->theme->text_dim);
    kc_text(cv, 20, 14 + 17 * 18, "Mouse wheel scrolls, Ctrl+wheel zooms, middle button pans.", ui->theme->text_dim);
    kc_text(cv, 20, 14 + 18 * 18, "Ctrl+F12 takes a screenshot; File > Open Screenshot loads it.", ui->theme->text_dim);
    if (k_button(ui, kr(cv->w - 100, cv->h - 40, 84, 26), "OK") || k_key_pressed(ui, KEY_ENTER) || k_key_pressed(ui, KEY_ESC)) dlg_close(d);
}

void dlg_shortcuts(void) { dlg_create(DK_SHORTCUTS, "Sketchbook Shortcuts", 560, 400, shortcuts_draw); }
