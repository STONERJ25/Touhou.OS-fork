/* Marisa's Sketchbook (sketchbook): a paint program for the desktop. This file is the application shell: the menu bar,
 * keyboard shortcuts, file commands with the "unsaved changes" guard, and the main window's frame. */
#include "sketchbook.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

Sk S;

/* ---------------------------------------------------------------- naming and title */

static void set_named(const char *path) {
    snprintf(S.path, sizeof S.path, "%s", path);
    const char *b = strrchr(path, '/');
    snprintf(S.name, sizeof S.name, "%s", b && b[1] ? b + 1 : path);
}

static void set_untitled(const char *name) {
    S.path[0] = 0;
    snprintf(S.name, sizeof S.name, "%s", name);
}

static void make_abs(const char *in, char *out, size_t cap) {
    if (in[0] == '/') { snprintf(out, cap, "%s", in); return; }
    char cwd[512];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/");
    while (in[0] == '.' && in[1] == '/') in += 2;
    snprintf(out, cap, "%s/%s", strcmp(cwd, "/") ? cwd : "", in);
}

bool app_modified(void) { return hist_version() != S.saved_version || hist_pending_dirty(); }

void app_update_title(void) {
    static char last[300];
    char t[300];
    snprintf(t, sizeof t, "Sketchbook - %s%s", S.name, app_modified() ? " *" : "");
    if (strcmp(t, last)) {
        snprintf(last, sizeof last, "%s", t);
        kapp_set_title(S.app, t);
    }
}

/* ---------------------------------------------------------------- the "unsaved changes" guard and saving */

static struct { void (*then)(void *); void *user; } g_after;          /* what to do once the picture is saved */
static bool g_guard_open;

static void run_after(void) {
    void (*fn)(void *) = g_after.then;
    void *u = g_after.user;
    g_after.then = NULL;
    g_after.user = NULL;
    if (fn) fn(u);
}

static void clear_after(void) { g_after.then = NULL; g_after.user = NULL; }

static bool do_save(const char *path) {
    tools_commit_all();
    const char *err = file_save(path, &g_doc);
    if (err) {
        char msg[700];
        snprintf(msg, sizeof msg, "%s\n\n%s", err, path);
        dlg_message("Cannot save picture", msg, KI_ERROR);
        return false;
    }
    set_named(path);
    S.saved_version = hist_version();
    return true;
}

static void save_dir_and_name(char *dir, size_t dcap, char *name, size_t ncap) {
    const char *slash = strrchr(S.path, '/');
    if (S.path[0] && slash) {
        snprintf(dir, dcap, "%.*s", slash == S.path ? 1 : (int)(slash - S.path), S.path);
        snprintf(name, ncap, "%s", slash + 1);
    } else {
        const char *home = getenv("HOME");
        snprintf(dir, dcap, "%s", home ? home : "/");
        snprintf(name, ncap, "%s", S.name[0] && strcmp(S.name, "untitled") ? S.name : "untitled.bmp");
    }
}

static void replace_answer(int choice, void *u) {
    char *path = u;
    if (choice == 0 && do_save(path)) run_after();
    else clear_after();
    free(path);
}

static void on_save_as(const char *path, void *u) {
    (void)u;
    dlg_modal_leave();
    if (!path) { clear_after(); return; }
    char full[600];
    make_abs(path, full, sizeof full);
    if (file_format_for(full) == FMT_UNKNOWN) {
        const char *base = strrchr(full, '/');
        if (base && !strchr(base, '.')) strncat(full, ".bmp", sizeof full - strlen(full) - 1);
        else {
            dlg_message("Cannot save picture", "Sketchbook saves .bmp and .ppm pictures. Please give the file a name ending in .bmp or .ppm.", KI_WARNING);
            clear_after();
            return;
        }
    }
    struct stat st;
    if (stat(full, &st) == 0) {
        char msg[700];
        snprintf(msg, sizeof msg, "%s already exists. Do you want to replace it?", full);
        char *copy = strdup(full);
        if (copy) { dlg_ask("Replace file", msg, KI_QUESTION, "Replace", "Cancel", NULL, replace_answer, copy); return; }
    }
    if (do_save(full)) run_after(); else clear_after();
}

void app_cmd_save_as(void) {
    if (tools_dragging()) return;
    tools_commit_all();
    char dir[512], name[160];
    save_dir_and_name(dir, sizeof dir, name, sizeof name);
    dlg_modal_enter();
    k_file_dialog(S.app, 1, "Save picture as", dir, name, on_save_as, NULL);
}

void app_cmd_save(void) {
    if (tools_dragging()) return;
    tools_commit_all();
    if (S.path[0] && file_format_for(S.path) != FMT_UNKNOWN) {
        if (do_save(S.path)) run_after(); else clear_after();
    } else app_cmd_save_as();
}

static void guard_answer(int choice, void *u) {
    (void)u;
    g_guard_open = false;
    if (choice == 0) app_cmd_save();              /* run_after() fires when the save succeeded */
    else if (choice == 1) run_after();
    else clear_after();
}

void app_guard(void (*then)(void *user), void *user) {
    if (g_guard_open) return;
    tools_commit_all();
    if (!app_modified()) { then(user); return; }
    g_after.then = then;
    g_after.user = user;
    g_guard_open = true;
    char msg[400];
    snprintf(msg, sizeof msg, "Save the changes to \"%s\" before going on?", S.name);
    dlg_ask("Unsaved changes", msg, KI_QUESTION, "Save", "Discard", "Cancel", guard_answer, NULL);
}

/* ---------------------------------------------------------------- creating and opening pictures */

void app_new_picture(int w, int h) {
    tools_reset();
    if (!doc_new(w, h, S.bg)) { dlg_message("Out of memory", "There is not enough memory for a picture that large.", KI_ERROR); return; }
    set_untitled("untitled");
    S.saved_version = 0;
    canvas_reset_view();
}

void app_resize_canvas(int w, int h, bool stretch) {
    tools_commit_all();
    tools_reset();
    if (w == g_doc.w && h == g_doc.h) return;
    uint32_t *np = img_make_resized(w, h, S.bg, stretch);
    if (!np || !hist_replace("Resize canvas", np, sk_clamp(w, 1, SK_MAX_DIM), sk_clamp(h, 1, SK_MAX_DIM)))
        dlg_message("Out of memory", "There is not enough memory to resize the canvas.", KI_ERROR);
}

static bool load_into_doc(const char *path, bool named) {
    uint32_t *px;
    int w, h;
    const char *err = file_load(path, &px, &w, &h);
    if (err) {
        char msg[700];
        snprintf(msg, sizeof msg, "%s\n\n%s", err, path);
        dlg_message("Cannot open picture", msg, KI_ERROR);
        return false;
    }
    tools_reset();
    doc_adopt(px, w, h);
    if (named) set_named(path);
    else set_untitled(strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    S.saved_version = 0;
    canvas_reset_view();
    return true;
}

void app_open_path(const char *path, bool is_arg) {
    char full[600];
    make_abs(path, full, sizeof full);
    struct stat st;
    if (stat(full, &st) != 0) {
        if (is_arg && errno == ENOENT) { set_named(full); return; }            /* a new picture with that name */
        char msg[700];
        snprintf(msg, sizeof msg, "The file does not exist: %s", full);
        dlg_message("Cannot open picture", msg, KI_ERROR);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        char msg[700];
        snprintf(msg, sizeof msg, "%s is a folder, not a picture.", full);
        dlg_message("Cannot open picture", msg, KI_ERROR);
        return;
    }
    load_into_doc(full, true);
}

static void on_open_file(const char *path, void *u) {
    (void)u;
    dlg_modal_leave();
    if (path) app_open_path(path, false);
}

static void open_dialog_now(void *u) {
    (void)u;
    char dir[512], name[160];
    save_dir_and_name(dir, sizeof dir, name, sizeof name);
    dlg_modal_enter();
    k_file_dialog(S.app, 0, "Open picture", dir, "", on_open_file, NULL);
}

static void new_dialog_now(void *u) { (void)u; dlg_new_image(); }

static void screenshot_now(void *u) {
    (void)u;
    struct stat st;
    if (stat("/tmp/screenshot.ppm", &st) != 0) {
        dlg_message("No screenshot yet", "There is no screenshot to open. Press Ctrl+F12 anywhere on the desktop to take one, then try again.", KI_INFO);
        return;
    }
    load_into_doc("/tmp/screenshot.ppm", false);
}

static void quit_now(void *u) { (void)u; kapp_quit(S.app, 0); }

void app_cmd_new(void) { if (!tools_dragging()) app_guard(new_dialog_now, NULL); }
void app_cmd_open(void) { if (!tools_dragging()) app_guard(open_dialog_now, NULL); }
void app_cmd_open_screenshot(void) { if (!tools_dragging()) app_guard(screenshot_now, NULL); }
void app_cmd_quit(void) { app_guard(quit_now, NULL); }

static bool on_close(KApp *app, void *user) {
    (void)app; (void)user;
    tools_commit_all();
    if (!app_modified()) return true;
    app_cmd_quit();
    return false;
}

/* ---------------------------------------------------------------- editing commands */

void app_cmd_undo(void) {
    if (tools_dragging()) return;
    if (tools_cancel_overlays()) return;
    if (hist_undo()) tools_reset();
}

void app_cmd_redo(void) {
    if (tools_dragging()) return;
    tools_commit_all();
    if (hist_redo()) tools_reset();
}

void app_cmd_op(int op, const char *label) {
    if (tools_dragging()) return;
    tools_commit_all();
    tools_reset();
    if (!hist_op(op, label)) dlg_message("Out of memory", "There is not enough memory for that operation.", KI_ERROR);
}

void app_cmd_clear(void) {
    if (tools_dragging()) return;
    tools_commit_all();
    tools_reset();
    hist_begin("Clear image");
    g_isink.color = S.bg;
    rs_fill_rect(&g_isink, 0, 0, g_doc.w - 1, g_doc.h - 1);
    hist_commit();
}

/* ---------------------------------------------------------------- menus */

enum { M_FILE, M_EDIT, M_IMAGE, M_VIEW, M_HELP };
#define SEP { "", 0, true, false, NULL }

static void run_menubar(KUI *ui, int W) {
    static const char *const titles[] = { "File", "Edit", "Image", "View", "Help" };
    static char undo_l[64], redo_l[64];
    const char *ul = hist_undo_label(), *rl = hist_redo_label();
    snprintf(undo_l, sizeof undo_l, "%s%s%s", "Undo", ul ? " " : "", ul ? ul : "");
    snprintf(redo_l, sizeof redo_l, "%s%s%s", "Redo", rl ? " " : "", rl ? rl : "");
    bool sel = tools_has_selection();
    bool busy = tools_dragging();
    KMenuItem file[] = {
        { "New...", 0, false, busy, "Ctrl+N" }, { "Open...", 0, false, busy, "Ctrl+O" }, { "Open Screenshot", 0, false, busy, NULL }, SEP,
        { "Save", 0, false, busy, "Ctrl+S" }, { "Save As...", 0, false, busy, "Ctrl+Shift+S" }, SEP, { "Quit", 0, false, false, "Ctrl+Q" },
    };
    KMenuItem edit[] = {
        { undo_l, 0, false, !ul || busy, "Ctrl+Z" }, { redo_l, 0, false, !rl || busy, "Ctrl+Y" }, SEP,
        { "Cut", 0, false, !sel, "Ctrl+X" }, { "Copy", 0, false, !sel, "Ctrl+C" }, { "Paste", 0, false, !clip_has_content(), "Ctrl+V" },
        { "Clear Selection", 0, false, !sel, "Delete" }, SEP,
        { "Select All", 0, false, false, "Ctrl+A" }, { "Deselect", 0, false, !sel, "Ctrl+D" },
    };
    KMenuItem image[] = {
        { "Flip Horizontal", 0, false, busy, NULL }, { "Flip Vertical", 0, false, busy, NULL },
        { "Rotate 90 Clockwise", 0, false, busy, NULL }, { "Rotate 90 Anticlockwise", 0, false, busy, NULL },
        { "Invert Colours", 0, false, busy, NULL }, SEP,
        { "Resize Canvas...", 0, false, busy, NULL }, { "Crop to Selection", 0, false, !sel || busy, NULL }, { "Clear Image", 0, false, busy, NULL },
    };
    KMenuItem view[] = {
        { "Zoom In", 0, false, S.zoom >= 8, "+" }, { "Zoom Out", 0, false, S.zoom <= 1, "-" }, { "Actual Size", 0, false, false, "1" },
        { "200%", 0, false, false, "2" }, { "400%", 0, false, false, "4" }, { "800%", 0, false, false, "8" }, SEP,
        { S.grid ? "Hide Pixel Grid" : "Show Pixel Grid", 0, false, false, "G" },
    };
    KMenuItem help[] = { { "Keyboard Shortcuts", 0, false, false, "F1" }, { "About Sketchbook", 0, false, false, NULL } };
    const KMenuItem *menus[] = { file, edit, image, view, help };
    int counts[] = { (int)(sizeof file / sizeof file[0]), (int)(sizeof edit / sizeof edit[0]), (int)(sizeof image / sizeof image[0]),
                     (int)(sizeof view / sizeof view[0]), (int)(sizeof help / sizeof help[0]) };
    static int was_open;
    int r = k_menubar(ui, kr(0, 0, W, MENU_H), titles, menus, counts, 5);
    if (was_open && !ui->menu_open) kapp_redraw(S.app);          /* the bar closes its menu after drawing it: show that */
    was_open = ui->menu_open;
    if (!r) return;
    kapp_redraw(S.app);
    int m = (r >> 8) - 1, it = (r & 255) - 1;
    switch (m) {
    case M_FILE:
        switch (it) {
        case 0: app_cmd_new(); break;
        case 1: app_cmd_open(); break;
        case 2: app_cmd_open_screenshot(); break;
        case 4: app_cmd_save(); break;
        case 5: app_cmd_save_as(); break;
        case 7: app_cmd_quit(); break;
        }
        break;
    case M_EDIT:
        switch (it) {
        case 0: app_cmd_undo(); break;
        case 1: app_cmd_redo(); break;
        case 3: cmd_cut(); break;
        case 4: cmd_copy(); break;
        case 5: cmd_paste(); break;
        case 6: cmd_delete(); break;
        case 8: cmd_select_all(); break;
        case 9: cmd_deselect(); break;
        }
        break;
    case M_IMAGE:
        switch (it) {
        case 0: app_cmd_op(HOP_FLIP_H, "Flip horizontal"); break;
        case 1: app_cmd_op(HOP_FLIP_V, "Flip vertical"); break;
        case 2: app_cmd_op(HOP_ROT_CW, "Rotate clockwise"); break;
        case 3: app_cmd_op(HOP_ROT_CCW, "Rotate anticlockwise"); break;
        case 4: app_cmd_op(HOP_INVERT, "Invert colours"); break;
        case 6: tools_commit_all(); dlg_resize_canvas(); break;
        case 7: cmd_crop_to_selection(); break;
        case 8: app_cmd_clear(); break;
        }
        break;
    case M_VIEW:
        switch (it) {
        case 0: canvas_set_zoom(S.zoom + 1, false); break;
        case 1: canvas_set_zoom(S.zoom - 1, false); break;
        case 2: canvas_set_zoom(1, false); break;
        case 3: canvas_set_zoom(2, false); break;
        case 4: canvas_set_zoom(4, false); break;
        case 5: canvas_set_zoom(8, false); break;
        case 7: S.grid = !S.grid; break;
        }
        break;
    case M_HELP:
        if (it == 0) dlg_shortcuts();
        else if (it == 1) dlg_about();
        break;
    }
}

/* ---------------------------------------------------------------- keyboard */

static Tool tool_for_char(uint32_t ch) {
    for (int t = 0; t < T_COUNT; t++) if ((int)(ch | 0x20) == (tool_key((Tool)t) | 0x20)) return (Tool)t;
    return T_COUNT;
}

static void handle_keys(KUI *ui) {
    if (ui->menu_open || S.modal) return;
    for (int i = 0; i < ui->nkeys; i++) {
        KKey *k = &ui->keys[i];
        if (!k->down || !k->code) continue;
        bool ctrl = (k->mods & MOD_CTRL) != 0, shift = (k->mods & MOD_SHIFT) != 0, alt = (k->mods & MOD_ALT) != 0;
        bool used = true;
        if (alt) continue;
        if (ctrl) {
            switch (k->code) {
            case KEY_N: app_cmd_new(); break;
            case KEY_O: app_cmd_open(); break;
            case KEY_S: if (shift) app_cmd_save_as(); else app_cmd_save(); break;
            case KEY_Q: case KEY_W: app_cmd_quit(); break;
            case KEY_Z: if (shift) app_cmd_redo(); else app_cmd_undo(); break;
            case KEY_Y: app_cmd_redo(); break;
            case KEY_X: if (!ui->focus) cmd_cut(); else used = false; break;
            case KEY_C: if (!ui->focus) cmd_copy(); else used = false; break;
            case KEY_V: if (!ui->focus) cmd_paste(); else used = false; break;
            case KEY_A: if (!ui->focus) cmd_select_all(); else used = false; break;
            case KEY_D: cmd_deselect(); break;
            case KEY_EQUAL: canvas_set_zoom(S.zoom + 1, false); break;
            case KEY_MINUS: canvas_set_zoom(S.zoom - 1, false); break;
            case KEY_0: canvas_set_zoom(1, false); break;
            default: used = false; break;
            }
        } else if (k->code == KEY_F1) dlg_shortcuts();
        else if (k->code == KEY_ESC) {
            if (ui->focus) used = false;                 /* a focused field wants it (Esc leaves the field) */
            else if (tools_dragging()) tools_cancel_drag();
            else if (!tools_cancel_overlays()) cmd_deselect();
        } else if (ui->focus) used = false;
        else if (k->code == KEY_DELETE) cmd_delete();
        else if (k->code == KEY_LEFT || k->code == KEY_RIGHT || k->code == KEY_UP || k->code == KEY_DOWN) {
            int dx = k->code == KEY_LEFT ? -1 : k->code == KEY_RIGHT ? 1 : 0, dy = k->code == KEY_UP ? -1 : k->code == KEY_DOWN ? 1 : 0;
            if (tools_has_selection() && S.tool == T_SELECT) tools_nudge(dx * (shift ? 8 : 1), dy * (shift ? 8 : 1));
            else { S.scroll_x += dx * 32; S.scroll_y += dy * 32; }
        } else if (k->code == KEY_PAGEUP) S.scroll_y -= S.view.h * 3 / 4;
        else if (k->code == KEY_PAGEDOWN) S.scroll_y += S.view.h * 3 / 4;
        else if (k->code == KEY_HOME) S.scroll_x = S.scroll_y = 0;
        else if (k->ch == '+' || k->ch == '=') canvas_set_zoom(S.zoom + 1, false);
        else if (k->ch == '-' || k->code == KEY_KPMINUS) canvas_set_zoom(S.zoom - 1, false);
        else if (k->ch >= '1' && k->ch <= '8') canvas_set_zoom((int)(k->ch - '0'), false);
        else if (k->ch == '[' || k->ch == ']') {
            Tool t = S.tool;
            if (t == T_BRUSH || t == T_ERASER || t == T_SPRAY || t == T_LINE || t == T_RECT || t == T_ELLIPSE)
                S.size[t] = sk_clamp(S.size[t] + (k->ch == ']' ? 1 : -1), 1, 32);
        } else if (k->ch == 'g' || k->ch == 'G') S.grid = !S.grid;
        else if (k->ch == 'x' || k->ch == 'X') color_swap();
        else if (k->ch >= 32 && tool_for_char(k->ch) != T_COUNT) tool_set(tool_for_char(k->ch));
        else used = false;
        if (used) { k->down = 0; k->code = 0; kapp_redraw(S.app); }
    }
}

/* ---------------------------------------------------------------- the main window */

static void draw(KApp *app, KUI *ui, void *user) {
    (void)app; (void)user;
    S.iter++;
    S.now = ui->now_ms;
    dlg_gc();
    int W = ui->cv.w, H = ui->cv.h;
    k_clear(ui);
    int top = MENU_H + 2, bottom = H - STATUS_H - PALETTE_H;
    KRect toolbox = kr(4, top + 2, TOOLBOX_W, bottom - top - 4);
    KRect frame = kr(TOOLBOX_W + 8, top + 2, W - TOOLBOX_W - 12, bottom - top - 4);
    tools_frame(ui);
    handle_keys(ui);                                     /* keys before the pointer events of the same batch: "P, then click" works in order */
    canvas_frame(ui, frame);
    panels_toolbox(ui, toolbox);
    panels_palette(ui, kr(0, bottom, W, PALETTE_H));
    panels_status(ui, kr(0, H - STATUS_H, W, STATUS_H));
    run_menubar(ui, W);
    app_update_title();
}

int main(int argc, char **argv) {
    memset(&S, 0, sizeof S);
    srand((unsigned)time(NULL) ^ (unsigned)getpid() << 8);
    tools_init();
    panels_init();
    canvas_init();
    if (!doc_new(SK_DEF_W, SK_DEF_H, S.bg)) { fprintf(stderr, "sketchbook: not enough memory\n"); return 1; }
    set_untitled("untitled");
    KApp *app = kapp_new("Sketchbook", 770, 640, FW_RESIZABLE);
    if (!app) return 1;
    S.app = app;
    kapp_set_draw(app, draw, NULL);
    kapp_set_event_hook(app, canvas_event_hook, NULL);
    kapp_set_close_handler(app, on_close, NULL);
    kapp_set_minsize(app, 560, 440);
    if (argc > 1) app_open_path(argv[1], true);
    app_update_title();
    int rc = kapp_run(app);
    tools_reset();
    hist_clear();
    doc_free();
    kapp_destroy(app);
    return rc;
}
