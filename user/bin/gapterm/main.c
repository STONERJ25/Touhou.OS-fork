/* gapterm: the terminal for the desktop. A pty, the VT emulator from vt.c and a Kappa window around them. */
#include <kappa.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include "vt.h"

#define PAD 3
#define COL_BG 0x10131F
#define COL_FG 0xE8E4D4

typedef struct Term {
    KApp *app;
    Vt vt;
    int master;
    pid_t child;
    int cols, rows;
    int view_offset;
    int last_sb;
    bool full;                      /* repaint everything */
    bool sel_active, sel_dragging;
    int sax, say, sbx, sby;         /* selection anchor / end in absolute coordinates (line, column) */
    bool cursor_on;
    uint64_t blink_ms;
    int last_cx, last_cy;
    bool focused;
    char title[96];
} Term;

static Term T;
static const char *g_run;               /* gapterm -e COMMAND: run this shell command line instead of a login shell (the Explorer uses it) */

/* ---------------------------------------------------------------- pty */

static void pty_write(const void *data, size_t n) {
    const char *p = data;
    while (n > 0) {
        ssize_t w = write(T.master, p, n);
        if (w < 0) { if (errno == EINTR || errno == EAGAIN) continue; return; }
        p += w;
        n -= (size_t)w;
    }
}

static void vt_reply(void *user, const char *data, size_t n) { (void)user; pty_write(data, n); }
static void vt_title(void *user, const char *title) {
    (void)user;
    snprintf(T.title, sizeof T.title, "%s", title);
    kapp_set_title(T.app, T.title);
}

static void on_pty(KApp *app, int fd, void *user) {
    (void)app; (void)user;
    uint8_t buf[16384];
    for (int rounds = 0; rounds < 8; rounds++) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            int before = vt_scrollback_count(&T.vt);
            vt_input(&T.vt, buf, (size_t)n);
            int added = vt_scrollback_count(&T.vt) - before;
            if (T.view_offset > 0 && added > 0) T.view_offset += added;
            if (T.sel_active && added > 0 && !T.vt.alt) { T.say += added; T.sby += added; }
            if ((size_t)n < sizeof buf) break;
        } else if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
            kapp_quit(T.app, 0);                               /* the shell exited: close the window */
            return;
        } else break;
    }
}

static pid_t start_shell(const char *slave_path, int cols, int rows) {
    pid_t pid = fork();
    if (pid != 0) return pid;
    setsid();
    int s = open(slave_path, O_RDWR);
    if (s < 0) _exit(126);
    struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
    ioctl(s, TIOCSWINSZ, &ws);
    dup2(s, 0); dup2(s, 1); dup2(s, 2);
    if (s > 2) close(s);
    close(T.master);
    setenv("TERM", "xterm", 1);
    setenv("COLORTERM", "truecolor", 1);
    setenv("NOMOTD", "1", 1);
    const char *home = getenv("HOME");
    if (home) chdir(home);
    if (g_run) {
        tcsetpgrp(0, getpid());                                 /* Ctrl+C must reach the command: it is not an interactive shell */
        execl("/bin/gapsh", "gapsh", "-c", g_run, (char *)NULL);
    }
    execl("/bin/gapsh", "gapsh", "-l", (char *)NULL);
    _exit(127);
}

/* ---------------------------------------------------------------- selection helpers */

static const VtCell *abs_cell(int line, int col) {
    int sb = vt_scrollback_count(&T.vt);
    return vt_view_cell(&T.vt, 0, line - sb, col);
}

static void sel_order(int *l0, int *c0, int *l1, int *c1) {
    *l0 = T.say; *c0 = T.sax; *l1 = T.sby; *c1 = T.sbx;
    if (*l0 > *l1 || (*l0 == *l1 && *c0 > *c1)) { int t = *l0; *l0 = *l1; *l1 = t; t = *c0; *c0 = *c1; *c1 = t; }
}

static bool selected(int line, int col) {
    if (!T.sel_active) return false;
    int l0, c0, l1, c1;
    sel_order(&l0, &c0, &l1, &c1);
    if (line < l0 || line > l1) return false;
    if (line == l0 && col < c0) return false;
    if (line == l1 && col > c1) return false;
    return true;
}

static void copy_selection(void) {
    if (!T.sel_active) return;
    int l0, c0, l1, c1;
    sel_order(&l0, &c0, &l1, &c1);
    char *out = malloc((size_t)(l1 - l0 + 1) * (size_t)(T.cols + 1) * 4 + 8);
    size_t n = 0;
    for (int l = l0; l <= l1; l++) {
        int a = l == l0 ? c0 : 0, b = l == l1 ? c1 : T.cols - 1;
        int last = b;
        while (last >= a && abs_cell(l, last)->cp == ' ') last--;
        if (l != l1 && b == T.cols - 1) { /* full line: trim trailing blanks */ }
        for (int c = a; c <= last; c++) {
            uint32_t cp = abs_cell(l, c)->cp;
            n += (size_t)kc_utf8_encode(cp ? cp : ' ', out + n);
        }
        if (l != l1) out[n++] = '\n';
    }
    if (n) kapp_clipboard_copy(T.app, out, n);
    free(out);
}

static void paste(void) {
    size_t n;
    const char *clip = kapp_clipboard(T.app, &n);
    if (!clip || !n) return;
    if (T.vt.bracketed_paste) pty_write("\033[200~", 6);
    char *tmp = malloc(n);
    for (size_t i = 0; i < n; i++) tmp[i] = clip[i] == '\n' ? '\r' : clip[i];
    pty_write(tmp, n);
    free(tmp);
    if (T.vt.bracketed_paste) pty_write("\033[201~", 6);
}

/* ---------------------------------------------------------------- keys */

static int csi_mod(const KKey *k) {
    int m = 1;
    if (k->mods & MOD_SHIFT) m += 1;
    if (k->mods & MOD_ALT) m += 2;
    if (k->mods & MOD_CTRL) m += 4;
    return m;
}

static int encode_key(const KKey *k, char *out) {
    int n = 0;
    int m = csi_mod(k);
    bool app = T.vt.app_cursor;
    #define SEQ(...) (n = snprintf(out, 32, __VA_ARGS__))
    switch (k->code) {
    case KEY_UP: case KEY_DOWN: case KEY_RIGHT: case KEY_LEFT: {
        char d = k->code == KEY_UP ? 'A' : k->code == KEY_DOWN ? 'B' : k->code == KEY_RIGHT ? 'C' : 'D';
        if (m > 1) SEQ("\033[1;%d%c", m, d); else if (app) SEQ("\033O%c", d); else SEQ("\033[%c", d);
        return n;
    }
    case KEY_HOME: if (m > 1) SEQ("\033[1;%dH", m); else if (app) SEQ("\033OH"); else SEQ("\033[H"); return n;
    case KEY_END: if (m > 1) SEQ("\033[1;%dF", m); else if (app) SEQ("\033OF"); else SEQ("\033[F"); return n;
    case KEY_INSERT: SEQ(m > 1 ? "\033[2;%d~" : "\033[2~", m); return n;
    case KEY_DELETE: SEQ(m > 1 ? "\033[3;%d~" : "\033[3~", m); return n;
    case KEY_PAGEUP: SEQ(m > 1 ? "\033[5;%d~" : "\033[5~", m); return n;
    case KEY_PAGEDOWN: SEQ(m > 1 ? "\033[6;%d~" : "\033[6~", m); return n;
    case KEY_ENTER: case KEY_KPENTER: out[0] = '\r'; return 1;
    case KEY_BACKSPACE: out[0] = (k->mods & MOD_CTRL) ? 0x08 : 0x7F; return 1;
    case KEY_TAB: if (k->mods & MOD_SHIFT) { memcpy(out, "\033[Z", 3); return 3; } out[0] = '\t'; return 1;
    case KEY_ESC: out[0] = 27; return 1;
    default: break;
    }
    if (k->code >= KEY_F1 && k->code <= KEY_F4) {
        char d = (char)('P' + (k->code - KEY_F1));
        if (m > 1) SEQ("\033[1;%d%c", m, d); else SEQ("\033O%c", d);
        return n;
    }
    static const int fcode[] = { 15, 17, 18, 19, 20, 21, 23, 24 };       /* F5..F12 */
    if (k->code >= KEY_F5 && k->code <= KEY_F10) { int c = fcode[k->code - KEY_F5]; SEQ(m > 1 ? "\033[%d;%d~" : "\033[%d~", c, m); return n; }
    if (k->code == KEY_F11) { SEQ(m > 1 ? "\033[23;%d~" : "\033[23~", m); return n; }
    if (k->code == KEY_F12) { SEQ(m > 1 ? "\033[24;%d~" : "\033[24~", m); return n; }
    #undef SEQ
    uint32_t ch = k->ch;
    if (!ch) return 0;
    if (k->mods & MOD_CTRL) {
        uint32_t lc = ch >= 'A' && ch <= 'Z' ? ch + 32 : ch;
        if (lc >= 'a' && lc <= 'z') ch = lc - 'a' + 1;
        else if (lc == '[' || lc == '{') ch = 27;
        else if (lc == '\\' || lc == '|') ch = 28;
        else if (lc == ']' || lc == '}') ch = 29;
        else if (lc == ' ' || lc == '2' || lc == '@') ch = 0;
        else if (lc == '/' || lc == '_' || lc == '-') ch = 31;
        else if (lc == '6' || lc == '^') ch = 30;
        else return 0;
    }
    if (k->mods & MOD_ALT) out[n++] = 27;
    n += kc_utf8_encode(ch, out + n);
    return n;
}

static void handle_key(const KKey *k) {
    bool shift = k->mods & MOD_SHIFT, ctrl = k->mods & MOD_CTRL;
    if (ctrl && shift && k->code == KEY_C) { copy_selection(); return; }
    if ((ctrl && shift && k->code == KEY_V) || (shift && k->code == KEY_INSERT)) { paste(); return; }
    if (ctrl && shift && k->code == KEY_N) { if (fork() == 0) { setsid(); execl("/bin/gapterm", "gapterm", (char *)NULL); _exit(127); } return; }
    if (shift && k->code == KEY_PAGEUP) { T.view_offset += T.rows - 1; T.full = true; return; }
    if (shift && k->code == KEY_PAGEDOWN) { T.view_offset -= T.rows - 1; T.full = true; return; }
    char buf[40];
    int n = encode_key(k, buf);
    if (n > 0) {
        if (T.view_offset) { T.view_offset = 0; T.full = true; }
        if (T.sel_active) { T.sel_active = false; T.full = true; }
        pty_write(buf, (size_t)n);
    }
}

/* ---------------------------------------------------------------- mouse reporting */

static void report_mouse(int button, bool press, int col, int row, uint8_t mods) {
    if (!T.vt.mouse_mode) return;
    int cb = button;
    if (mods & MOD_SHIFT) cb |= 4;
    if (mods & MOD_ALT) cb |= 8;
    if (mods & MOD_CTRL) cb |= 16;
    char buf[32];
    int n;
    if (T.vt.mouse_sgr) n = snprintf(buf, sizeof buf, "\033[<%d;%d;%d%c", cb, col + 1, row + 1, press ? 'M' : 'm');
    else {
        if (!press) cb = 3;
        n = snprintf(buf, sizeof buf, "\033[M%c%c%c", 32 + cb, 32 + col + 1 > 255 ? 255 : 32 + col + 1, 32 + row + 1 > 255 ? 255 : 32 + row + 1);
    }
    pty_write(buf, (size_t)n);
}

/* ---------------------------------------------------------------- drawing */

static uint32_t resolve_fg(const VtCell *c, bool *bold_out) {
    uint32_t fg = c->fg == VT_DEFAULT ? COL_FG : c->fg;
    if ((c->attr & A_BOLD) && c->fg != VT_DEFAULT) {
        for (int i = 0; i < 8; i++) if (vt_palette(i) == c->fg) { fg = vt_palette(i + 8); break; }
    }
    if (c->attr & A_DIM) fg = kc_mix(fg, COL_BG, 110);
    *bold_out = (c->attr & A_BOLD) != 0;
    return fg;
}

static void draw_row(KUI *ui, int r) {
    KCanvas *cv = &ui->cv;
    int y = PAD + r * 16;
    kc_fill(cv, kr(0, y, cv->w, 16), COL_BG);
    int sb = vt_scrollback_count(&T.vt);
    int abs_line = sb + r - T.view_offset;
    for (int col = 0; col < T.cols; col++) {
        const VtCell *c = vt_view_cell(&T.vt, T.view_offset, r, col);
        bool bold;
        uint32_t fg = resolve_fg(c, &bold);
        uint32_t bg = c->bg == VT_DEFAULT ? COL_BG : c->bg;
        if (c->attr & A_REVERSE) { uint32_t t = fg; fg = bg; bg = t; }
        if (selected(abs_line, col)) { bg = 0xB5122B; fg = 0xFFF6E0; }
        bool cursor = T.view_offset == 0 && T.vt.cursor_visible && r == T.vt.cy && col == T.vt.cx;
        if (cursor) {
            if (T.focused && T.cursor_on) { uint32_t t = fg; fg = bg == COL_BG ? COL_BG : bg; bg = 0xE8E4D4; (void)t; }
        }
        int x = PAD + col * 8;
        if (bg != COL_BG) kc_fill(cv, kr(x, y, 8, 16), bg);
        if (c->cp != ' ' && c->cp != 0 && !(c->attr & A_INVISIBLE)) {
            kc_glyph(cv, x, y, c->cp, fg, 1);
            if (bold && c->fg == VT_DEFAULT) kc_glyph(cv, x + 1, y, c->cp, fg, 1);
        }
        if (c->attr & A_UNDERLINE) kc_hline(cv, x, y + 14, 8, fg);
        if (c->attr & A_STRIKE) kc_hline(cv, x, y + 8, 8, fg);
        if (cursor && !T.focused) kc_outline(cv, kr(x, y, 8, 16), 0xE8E4D4);
    }
}

static void term_draw(KApp *app, KUI *ui, void *user) {
    (void)app; (void)user;
    KCanvas *cv = &ui->cv;
    T.focused = ui->has_focus;

    /* geometry */
    int cols = (cv->w - 2 * PAD) / 8, rows = (cv->h - 2 * PAD) / 16;
    if (cols < 10) cols = 10;
    if (rows < 3) rows = 3;
    if (cols != T.cols || rows != T.rows) {
        T.cols = cols; T.rows = rows;
        vt_resize(&T.vt, cols, rows);
        struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
        ioctl(T.master, TIOCSWINSZ, &ws);
        T.full = true;
        kc_fill(cv, kr(0, 0, cv->w, cv->h), COL_BG);
    }

    /* keyboard */
    for (int i = 0; i < ui->nkeys; i++) {
        KKey *k = &ui->keys[i];
        if (k->down) handle_key(k);
    }
    ui->nkeys = 0;

    /* mouse: wheel scrolls the history (or goes to the application when it asked for the mouse) */
    int mcol = (ui->mx - PAD) / 8, mrow = (ui->my - PAD) / 16;
    if (mcol < 0) mcol = 0;
    if (mcol >= T.cols) mcol = T.cols - 1;
    if (mrow < 0) mrow = 0;
    if (mrow >= T.rows) mrow = T.rows - 1;
    bool mouse_app = T.vt.mouse_mode && !(ui->mods & MOD_SHIFT);
    if (ui->wheel) {
        if (mouse_app) for (int i = 0; i < abs(ui->wheel); i++) report_mouse(ui->wheel > 0 ? 64 : 65, true, mcol, mrow, ui->mods);
        else if (T.vt.alt) for (int i = 0; i < abs(ui->wheel) * 3; i++) pty_write(ui->wheel > 0 ? "\033[A" : "\033[B", 3);
        else { T.view_offset += ui->wheel * 3; T.full = true; }
    }
    int max_off = vt_scrollback_count(&T.vt);
    if (T.view_offset > max_off) T.view_offset = max_off;
    if (T.view_offset < 0) T.view_offset = 0;

    if (mouse_app) {
        if (ui->pressed[0]) report_mouse(0, true, mcol, mrow, ui->mods);
        if (ui->pressed[1]) report_mouse(2, true, mcol, mrow, ui->mods);
        if (ui->pressed[2]) report_mouse(1, true, mcol, mrow, ui->mods);
        if (ui->released[0] || ui->released[1] || ui->released[2]) report_mouse(0, false, mcol, mrow, ui->mods);
    } else {
        int abs_line = vt_scrollback_count(&T.vt) + mrow - T.view_offset;
        if (ui->pressed[0]) {
            bool dbl = ui->double_click;
            T.sel_dragging = true;
            T.sel_active = true;
            T.sax = T.sbx = mcol;
            T.say = T.sby = abs_line;
            if (dbl) {                                          /* double click selects a word */
                int a = mcol, b = mcol;
                while (a > 0 && abs_cell(abs_line, a - 1)->cp > ' ') a--;
                while (b < T.cols - 1 && abs_cell(abs_line, b + 1)->cp > ' ') b++;
                T.sax = a; T.sbx = b;
            }
            T.full = true;
        } else if (T.sel_dragging && (ui->buttons & 1)) {
            if (T.sbx != mcol || T.sby != abs_line) { T.sbx = mcol; T.sby = abs_line; T.full = true; }
        }
        if (ui->released[0] && T.sel_dragging) {
            T.sel_dragging = false;
            if (T.sax == T.sbx && T.say == T.sby && !ui->double_click) { T.sel_active = false; T.full = true; }
            else copy_selection();
        }
        if (ui->pressed[1] || ui->pressed[2]) paste();
    }
    ui->cursor_shape = mouse_app ? FC_ARROW : FC_TEXT;

    /* cursor blink */
    if (ui->has_focus) {
        bool on = ((ui->now_ms - T.blink_ms) / 500) % 2 == 0;
        if (on != T.cursor_on) { T.cursor_on = on; vt_mark_dirty(&T.vt, T.vt.cy); }
        ui->redraw_soon = true;
    } else if (!T.cursor_on) { T.cursor_on = true; vt_mark_dirty(&T.vt, T.vt.cy); }
    if (T.vt.cy != T.last_cy) { vt_mark_dirty(&T.vt, T.last_cy); T.last_cy = T.vt.cy; }
    T.last_cx = T.vt.cx;

    /* paint what changed */
    bool full = T.full || T.vt.all_dirty;
    if (full) kc_fill(cv, kr(0, 0, cv->w, PAD), COL_BG);
    int y0 = -1, y1 = -1;
    for (int r = 0; r < T.rows; r++) {
        if (!full && !T.vt.dirty[r]) continue;
        draw_row(ui, r);
        if (y0 < 0) y0 = r;
        y1 = r;
    }
    if (full) {
        kc_fill(cv, kr(0, PAD + T.rows * 16, cv->w, cv->h - PAD - T.rows * 16), COL_BG);
        kc_fill(cv, kr(PAD + T.cols * 8, 0, cv->w - PAD - T.cols * 8, cv->h), COL_BG);
    } else if (y0 >= 0) k_damage(ui, kr(0, PAD + y0 * 16, cv->w, (y1 - y0 + 1) * 16));
    else k_damage(ui, kr(0, 0, 0, 0));
    memset(T.vt.dirty, 0, (size_t)T.vt.rows * sizeof(bool));
    T.vt.all_dirty = false;
    T.full = false;
}

static bool on_close(KApp *app, void *user) {
    (void)app; (void)user;
    if (T.child > 0) kill(-T.child, SIGHUP);
    return true;
}

int main(int argc, char **argv) {
    T.cols = 80; T.rows = 25;
    snprintf(T.title, sizeof T.title, "Terminal");
    for (int i = 1; i < argc; i++) {                            /* -t TITLE, -e COMMAND */
        if (!strcmp(argv[i], "-e") && i + 1 < argc) g_run = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) snprintf(T.title, sizeof T.title, "%s", argv[++i]);
    }
    KApp *app = kapp_new(T.title, PAD * 2 + 80 * 8, PAD * 2 + 25 * 16, FW_RESIZABLE);
    if (!app) return 1;
    T.app = app;
    char slave[40];
    T.master = ptyopen(slave, sizeof slave);
    if (T.master < 0) { fprintf(stderr, "gapterm: cannot open a pty: %s\n", strerror(errno)); return 1; }
    vt_init(&T.vt, T.cols, T.rows, 3000);
    T.vt.reply = vt_reply;
    T.vt.set_title = vt_title;
    T.cursor_on = true;
    T.full = true;
    T.last_sb = 0;
    T.child = start_shell(slave, T.cols, T.rows);
    kapp_set_draw(app, term_draw, NULL);
    kapp_watch_fd(app, T.master, on_pty, NULL);
    kapp_set_close_handler(app, on_close, NULL);
    kapp_set_minsize(app, 200, 80);
    signal(SIGCHLD, SIG_DFL);
    int rc = kapp_run(app);
    close(T.master);
    if (T.child > 0) waitpid(T.child, NULL, WNOHANG);
    return rc;
}
