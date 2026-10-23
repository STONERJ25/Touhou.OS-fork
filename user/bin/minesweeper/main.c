/*
 * minesweeper: the classic, with fairies hiding under the grass and ofuda for flags.
 *
 *   left click       uncover          right click   place or lift an ofuda
 *   middle click     chord            left + right  chord (uncover the neighbours of a satisfied number)
 *   arrows + space   keyboard play    F flags       Enter chords      F2 new game
 *
 * Command line: --selftest   --level beginner|intermediate|expert   --size WxH --mines N   --seed N   --cell PX   --trace
 */
#include <kappa.h>
#include <kappa_gfx2d.h>
#include <kappa_gamekit.h>
#include "board.h"
#include "art.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ms_selftest(void);

#define MENU_H   24
#define HEADER_H 44
#define STATUS_H 20
#define SCORE_FILE "minesweeper.scores"

enum { LV_BEGINNER, LV_INTERMEDIATE, LV_EXPERT, LV_CUSTOM };
static const struct { const char *name; int w, h, mines; } LEVELS[3] = {
    { "Beginner", 9, 9, 10 }, { "Intermediate", 16, 16, 40 }, { "Expert", 30, 16, 99 },
};

typedef struct Game {
    KApp *app;
    Board board;
    Art art;
    int cell;
    int level;
    int cw, ch, cmines;                 /* the custom field */
    bool trace, fixed_seed, force_cell;
    uint32_t seed;
    int boards_made;
    /* layout of the main window */
    int win_w, win_h, bx, by;
    int decor_w, decor_h;
    /* clock */
    uint64_t start_ms, final_ms;
    bool timing;
    /* pointer and keyboard */
    int hover_x, hover_y;
    int kx, ky;
    bool kbd;
    bool lmb, rmb, mmb, both, both_done, face_down;
    int face;
    /* what was painted last, to skip redundant frames */
    bool dirty, menu_was_open, ptr_valid;
    int last_mx, last_my;
    int shown_time, shown_mines, shown_hover_x, shown_hover_y, shown_face;
    int last_w, last_h;
    char status[64];
    KScoreTable scores;
    /* new record waiting for a name */
    int rec_level;
    long rec_ms;
    KWin *custom_win, *times_win;
} Game;

/* ---------------------------------------------------------------- helpers */

static void trace(Game *g, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void trace(Game *g, const char *fmt, ...) {
    if (!g->trace) return;
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("minesweeper: %s\n", buf);
    fflush(stdout);
}

static long elapsed_ms(const Game *g) {
    if (g->board.status == ST_PLAYING && g->timing) return (long)(k_now_ms() - g->start_ms);
    if (g->board.status == ST_WON || g->board.status == ST_LOST) return (long)g->final_ms;
    return 0;
}

static int level_category(const Game *g) { return g->level; }

static const char *level_name(const Game *g) { return g->level == LV_CUSTOM ? "Custom" : LEVELS[g->level].name; }

static void set_status(Game *g, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_status(Game *g, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g->status, sizeof g->status, fmt, ap);
    va_end(ap);
}

static void compute_layout(Game *g) {
    int bw = g->board.w * g->cell, bh = g->board.h * g->cell;
    g->win_w = bw + 22;
    if (g->win_w < 206) g->win_w = 206;
    g->win_h = MENU_H + 6 + HEADER_H + 6 + bh + 6 + 6 + STATUS_H + 6 + 6;
    g->bx = (g->win_w - bw) / 2;
    g->by = MENU_H + 6 + HEADER_H + 6 + 3 + 3;
}

static int choose_cell(Game *g, int w, int h) {
    if (g->force_cell) return g->cell;
    int ww = 1024, wh = 700;
    int x, y;
    fairy_work_area(kapp_display(g->app), &x, &y, &ww, &wh);
    static const int sizes[] = { 20, 16 };
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        int c = sizes[i];
        if (w * c + 22 + g->decor_w <= ww - 8 && h * c + 124 + g->decor_h <= wh - 8) return c;
    }
    return 16;
}

static void rebuild_art(Game *g, int cell) {
    if (g->art.cell == cell && g->art.pressed) return;
    art_free(&g->art);
    art_build(&g->art, cell);
}

static void center_window(Game *g) {
    int x, y, w, h;
    fairy_work_area(kapp_display(g->app), &x, &y, &w, &h);
    int ow = g->win_w + g->decor_w, oh = g->win_h + g->decor_h;
    kapp_move(g->app, x + (w - ow) / 2, y + (h - oh) / 2, g->win_w, g->win_h);
}

/* ---------------------------------------------------------------- game flow */

static void new_game(Game *g, bool resize) {
    int w, h, m;
    if (g->level == LV_CUSTOM) { w = g->cw; h = g->ch; m = g->cmines; }
    else { w = LEVELS[g->level].w; h = LEVELS[g->level].h; m = LEVELS[g->level].mines; }
    board_clamp(&w, &h, &m);
    if (g->level != LV_CUSTOM) { w = LEVELS[g->level].w; h = LEVELS[g->level].h; m = LEVELS[g->level].mines; }
    uint32_t seed = g->fixed_seed ? g->seed + (uint32_t)g->boards_made : kg_seed_from_time();
    g->boards_made++;
    board_new(&g->board, w, h, m, seed);
    g->cell = choose_cell(g, w, h);
    rebuild_art(g, g->cell);
    compute_layout(g);
    g->timing = false;
    g->final_ms = 0;
    g->face = FACE_NORMAL;
    g->hover_x = g->hover_y = -1;
    g->kx = w / 2;
    g->ky = h / 2;
    g->kbd = false;
    g->lmb = g->rmb = g->mmb = g->both = g->both_done = false;
    set_status(g, "Click the grass!");
    char title[80];
    snprintf(title, sizeof title, "Minesweeper");
    kapp_set_title(g->app, title);
    kapp_set_tick(g->app, 0);
    if (resize && (g->win_w != kapp_width(g->app) || g->win_h != kapp_height(g->app))) {
        center_window(g);
        g->ptr_valid = false;                                /* the pointer position we know is for the old window */
    }
    g->dirty = true;
    kapp_redraw(g->app);
    trace(g, "new game %s %dx%d fairies=%d seed=%u cell=%d", level_name(g), w, h, m, (unsigned)seed, g->cell);
}

static void record_cb(const char *name, void *user) {
    Game *g = user;
    if (name && *name) {
        int rank = kg_scores_add(&g->scores, g->rec_level, g->rec_ms, name, 0, 0);
        kg_scores_save(&g->scores, SCORE_FILE);
        trace(g, "record saved: level %d rank %d time %ld ms name %s", g->rec_level, rank, g->rec_ms, name);
    }
}

static void finish_game(Game *g) {
    Board *b = &g->board;
    g->final_ms = (uint64_t)(k_now_ms() - g->start_ms);
    g->timing = false;
    kapp_set_tick(g->app, 0);
    if (b->status == ST_WON) {
        g->face = FACE_COOL;
        long ms = (long)g->final_ms;
        trace(g, "won in %ld ms", ms);
        bool recordable = g->level != LV_CUSTOM && kg_scores_rank(&g->scores, level_category(g), ms) >= 0;
        if (recordable) {
            set_status(g, "New record! %ld.%ld s", ms / 1000, ms / 100 % 10);
            g->rec_level = level_category(g);
            g->rec_ms = ms;
            char prompt[120], who[40];
            const char *user = getenv("USER");
            snprintf(who, sizeof who, "%s", user && *user ? user : "Reimu");
            snprintf(prompt, sizeof prompt, "Fastest time on %s! Your name:", level_name(g));
            k_input_dialog(g->app, "New record", prompt, who, record_cb, g);
        } else set_status(g, "All found in %ld.%ld s", ms / 1000, ms / 100 % 10);
    } else {
        g->face = FACE_DEAD;
        set_status(g, "Pranked! Press F2.");
        trace(g, "lost at %d,%d after %ld ms", b->hit_x, b->hit_y, (long)g->final_ms);
    }
    g->dirty = true;
}

static void after_move(Game *g, const char *what, int x, int y, int result) {
    Board *b = &g->board;
    if (b->status == ST_PLAYING && !g->timing) {
        g->timing = true;
        g->start_ms = k_now_ms();
        kapp_set_tick(g->app, 200);
        if (g->trace) {                                      /* the layout is revealed on the first click so that tests can aim */
            char line[1500];
            size_t n = 0;
            for (int i = 0; i < b->w * b->h && n + 12 < sizeof line; i++)
                if (b->mine[i]) n += (size_t)snprintf(line + n, sizeof line - n, "%d,%d ", i % b->w, i / b->w);
            trace(g, "fairies at: %s", line);
        }
    }
    trace(g, "%s %d %d -> %d (status %d, opened %d, flags %d)", what, x, y, result, b->status, b->opened, b->flags);
    if (b->status == ST_WON || b->status == ST_LOST) {
        if (!g->final_ms) finish_game(g);
    } else if (b->status == ST_PLAYING) {
        int left = b->mines - b->flags;
        set_status(g, left == 1 ? "1 fairy hiding" : "%d fairies hiding", left);
    }
    g->dirty = true;
}

static void act_reveal(Game *g, int x, int y) {
    Board *b = &g->board;
    if (!board_in(b, x, y)) return;
    if (b->cell[board_idx(b, x, y)] == CELL_OPEN) {
        int r = board_chord(b, x, y);
        if (r != 0) after_move(g, "chord", x, y, r);
        return;
    }
    int r = board_reveal(b, x, y);
    if (r != 0) after_move(g, "reveal", x, y, r);
}

static void act_flag(Game *g, int x, int y) {
    Board *b = &g->board;
    if (!board_in(b, x, y)) return;
    int before = b->flags;
    board_flag(b, x, y);
    if (b->flags != before) after_move(g, "flag", x, y, b->flags - before);
}

static void act_chord(Game *g, int x, int y) {
    Board *b = &g->board;
    if (!board_in(b, x, y)) return;
    int r = board_chord(b, x, y);
    if (r != 0) after_move(g, "chord", x, y, r);
}

/* ---------------------------------------------------------------- dialogs */

typedef struct { Game *g; KWin *win; char w[8], h[8], m[8]; bool focused; } CustomDlg;

static void custom_close(CustomDlg *d) {
    d->g->custom_win = NULL;
    kwin_close(d->win);
}

static void custom_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    CustomDlg *d = user;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    k_label(ui, 16, 18, "Width (9-30):");
    k_label(ui, 16, 48, "Height (9-24):");
    k_label(ui, 16, 78, "Fairies:");
    if (!d->focused) { k_focus_textfield(ui, d->w); d->focused = true; }
    k_textfield(ui, kr(150, 14, 80, 24), d->w, sizeof d->w);
    k_textfield(ui, kr(150, 44, 80, 24), d->h, sizeof d->h);
    k_textfield(ui, kr(150, 74, 80, 24), d->m, sizeof d->m);
    bool ok = k_button(ui, kr(cv->w - 190, cv->h - 40, 84, 26), "OK") || k_key_pressed(ui, KEY_ENTER);
    bool cancel = k_button(ui, kr(cv->w - 96, cv->h - 40, 84, 26), "Cancel") || k_key_pressed(ui, KEY_ESC);
    if (ok) {
        Game *g = d->g;
        g->cw = atoi(d->w);
        g->ch = atoi(d->h);
        g->cmines = atoi(d->m);
        board_clamp(&g->cw, &g->ch, &g->cmines);
        g->level = LV_CUSTOM;
        custom_close(d);
        new_game(g, true);
    } else if (cancel) custom_close(d);
}

static void custom_closed(KWin *w, void *user) {
    (void)w;
    CustomDlg *d = user;
    d->g->custom_win = NULL;
}

static void open_custom(Game *g) {
    if (g->custom_win) return;
    CustomDlg *d = calloc(1, sizeof *d);
    if (!d) return;
    d->g = g;
    snprintf(d->w, sizeof d->w, "%d", g->board.w);
    snprintf(d->h, sizeof d->h, "%d", g->board.h);
    snprintf(d->m, sizeof d->m, "%d", g->board.mines);
    d->win = kwin_new(g->app, -1, -1, 260, 150, "Custom field", FW_DIALOG | FW_CENTER | FW_TOP, custom_draw, d);
    if (!d->win) { free(d); return; }
    g->custom_win = d->win;
    kwin_set_close_handler(d->win, custom_closed, d);
}

typedef struct { Game *g; KWin *win; } TimesDlg;

static void times_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    TimesDlg *d = user;
    Game *g = d->g;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    int y = 12;
    for (int l = 0; l < 3; l++) {
        k_heading(ui, 16, y, LEVELS[l].name);
        y += 20;
        const KScoreEntry *list[8];
        int n = kg_scores_list(&g->scores, l, list, 5);
        if (!n) { k_label_col(ui, 28, y, "no records yet", ui->theme->text_dim); y += 18; }
        for (int i = 0; i < n; i++) {
            char line[64];
            snprintf(line, sizeof line, "%d. %3ld.%ld s  %s", i + 1, list[i]->score / 1000, list[i]->score / 100 % 10, list[i]->name);
            k_label(ui, 28, y, line);
            y += 18;
        }
        y += 8;
    }
    if (k_button(ui, kr(16, cv->h - 40, 96, 26), "Reset")) {
        kg_scores_init(&g->scores, true, 5);
        kg_scores_save(&g->scores, SCORE_FILE);
    }
    if (k_button(ui, kr(cv->w - 100, cv->h - 40, 84, 26), "Close") || k_key_pressed(ui, KEY_ESC) || k_key_pressed(ui, KEY_ENTER)) {
        g->times_win = NULL;
        kwin_close(d->win);
    }
}

static void times_closed(KWin *w, void *user) {
    (void)w;
    TimesDlg *d = user;
    d->g->times_win = NULL;
}

static void open_times(Game *g) {
    if (g->times_win) return;
    TimesDlg *d = calloc(1, sizeof *d);
    if (!d) return;
    d->g = g;
    d->win = kwin_new(g->app, -1, -1, 280, 400, "Best times", FW_DIALOG | FW_CENTER | FW_TOP, times_draw, d);
    if (!d->win) { free(d); return; }
    g->times_win = d->win;
    kwin_set_close_handler(d->win, times_closed, d);
}

static void show_help(Game *g) {
    static const char *const buttons[] = { "Got it" };
    k_messagebox(kapp_display(g->app), "How to play",
                 "Fairies are hiding under the grass. Uncover every square that has none.\n\n"
                 "A number tells how many fairies hide in the 8 squares around it.\n\n"
                 "Left click uncovers. Right click puts an ofuda on a square you think hides a fairy.\n"
                 "Middle click (or both buttons) on a number uncovers its neighbours once you have placed that many ofuda.\n\n"
                 "Keyboard: arrows move, Space uncovers, F places an ofuda, Enter chords, F2 starts again.",
                 KI_INFO, buttons, 1);
}

static void show_about(Game *g) {
    static const char *const buttons[] = { "Close" };
    k_messagebox(kapp_display(g->app), "About Minesweeper",
                 "Minesweeper for HakureiDE\n\nThe fairies of Gensokyo love to hide. Find them all before they prank you.\n\nAll art is drawn at start-up from primitives.",
                 KI_STAR, buttons, 1);
}

/* ---------------------------------------------------------------- input */

static bool cell_at(const Game *g, int px, int py, int *cx, int *cy) {
    int x = px - g->bx, y = py - g->by;
    if (x < 0 || y < 0) return false;
    x /= g->cell;
    y /= g->cell;
    if (x >= g->board.w || y >= g->board.h) return false;
    *cx = x;
    *cy = y;
    return true;
}

static KRect face_rect(const Game *g) { return kr((g->win_w - 36) / 2, MENU_H + 6 + (HEADER_H - 36) / 2, 36, 36); }

static void handle_pointer(Game *g, KUI *ui) {
    int hx = -1, hy = -1;
    if (!g->ptr_valid && (ui->mx != g->last_mx || ui->my != g->last_my || ui->pressed[0] || ui->pressed[1] || ui->pressed[2])) g->ptr_valid = true;
    g->last_mx = ui->mx;
    g->last_my = ui->my;
    bool over = g->ptr_valid && ui->mouse_inside && cell_at(g, ui->mx, ui->my, &hx, &hy);
    if (!over) hx = hy = -1;
    if (hx != g->hover_x || hy != g->hover_y) { g->hover_x = hx; g->hover_y = hy; if (g->lmb || g->mmb || g->both) g->dirty = true; }
    if (over) g->kbd = false;

    KRect fr = face_rect(g);
    bool on_face = ui->mouse_inside && kr_contains(fr, ui->mx, ui->my);
    if (ui->pressed[0] && on_face) g->face_down = true;
    if (ui->released[0]) {
        if (g->face_down && on_face) new_game(g, false);
        g->face_down = false;
    }

    bool board_event = over || g->lmb || g->rmb || g->mmb;
    if (!board_event && !ui->pressed[0] && !ui->pressed[1] && !ui->pressed[2]) return;
    if (ui->pressed[0] && over) g->lmb = true;
    if (ui->pressed[1] && over) g->rmb = true;
    if (ui->pressed[2] && over) g->mmb = true;
    if (g->lmb && g->rmb) g->both = true;
    if (g->lmb || g->mmb || g->both) {
        if (g->board.status == ST_READY || g->board.status == ST_PLAYING) g->face = FACE_OOH;
        g->dirty = true;
    }
    bool released = ui->released[0] || ui->released[1] || ui->released[2];
    if (released) {
        if ((g->both || g->mmb) && !g->both_done && (ui->released[2] || ui->released[0] || ui->released[1])) {
            g->both_done = true;
            if (over) act_chord(g, hx, hy);
        } else if (!g->both && !g->both_done) {
            if (ui->released[0] && g->lmb && over) act_reveal(g, hx, hy);
            if (ui->released[1] && g->rmb && over) act_flag(g, hx, hy);
        }
        if (ui->released[0]) g->lmb = false;
        if (ui->released[1]) g->rmb = false;
        if (ui->released[2]) g->mmb = false;
        if (!g->lmb && !g->rmb && !g->mmb) { g->both = g->both_done = false; }
        if (g->board.status == ST_PLAYING || g->board.status == ST_READY) g->face = FACE_NORMAL;
        g->dirty = true;
    }
}

static void handle_keys(Game *g, KUI *ui) {
    Board *b = &g->board;
    int dx = 0, dy = 0;
    if (k_key_pressed(ui, KEY_LEFT)) dx = -1;
    if (k_key_pressed(ui, KEY_RIGHT)) dx = 1;
    if (k_key_pressed(ui, KEY_UP)) dy = -1;
    if (k_key_pressed(ui, KEY_DOWN)) dy = 1;
    if (dx || dy) {
        if (!g->kbd) g->kbd = true;
        else { g->kx += dx; g->ky += dy; }
        if (g->kx < 0) g->kx = 0;
        if (g->ky < 0) g->ky = 0;
        if (g->kx >= b->w) g->kx = b->w - 1;
        if (g->ky >= b->h) g->ky = b->h - 1;
        g->dirty = true;
    }
    if (k_key_pressed(ui, KEY_SPACE)) { g->kbd = true; act_reveal(g, g->kx, g->ky); g->dirty = true; }
    if (k_key_pressed(ui, KEY_F)) { g->kbd = true; act_flag(g, g->kx, g->ky); g->dirty = true; }
    if (k_key_pressed(ui, KEY_ENTER)) { g->kbd = true; act_chord(g, g->kx, g->ky); g->dirty = true; }
    if (k_key_pressed(ui, KEY_F2)) new_game(g, false);
    if ((ui->mods & MOD_CTRL) && k_key_pressed(ui, KEY_Q)) kapp_quit(g->app, 0);
}

/* ---------------------------------------------------------------- drawing */

static void draw_board(Game *g, KUI *ui) {
    KCanvas *cv = &ui->cv;
    Board *b = &g->board;
    Art *a = &g->art;
    bool lost = b->status == ST_LOST, won = b->status == ST_WON;
    bool preview = (g->lmb || g->mmb || g->both) && (b->status == ST_READY || b->status == ST_PLAYING);
    bool wide = g->both || g->mmb;
    for (int y = 0; y < b->h; y++)
        for (int x = 0; x < b->w; x++) {
            int i = board_idx(b, x, y);
            int px = g->bx + x * g->cell, py = g->by + y * g->cell;
            const KSprite *s;
            uint8_t st = b->cell[i];
            if (st == CELL_OPEN) s = b->adj[i] ? a->num[b->adj[i]] : a->pressed;
            else if (lost && b->mine[i] && st != CELL_FLAG) s = (x == b->hit_x && y == b->hit_y) ? a->fairy_hit : a->fairy;
            else if (lost && st == CELL_FLAG && !b->mine[i]) s = a->wrong;
            else if (st == CELL_FLAG) s = a->flag;
            else {
                s = a->covered[(x * 7 + y * 13) % 3];
                if (preview && g->hover_x >= 0) {
                    int ddx = x - g->hover_x, ddy = y - g->hover_y;
                    if ((wide && ddx >= -1 && ddx <= 1 && ddy >= -1 && ddy <= 1) || (!wide && ddx == 0 && ddy == 0)) s = a->pressed;
                }
            }
            ks_blit_key(cv, px, py, s);
            if (!lost && !won && st == CELL_COVERED && x == g->hover_x && y == g->hover_y && !preview)
                ks_add_rect(cv, kr(px + 2, py + 2, g->cell - 4, g->cell - 4), 0x182010);       /* a soft highlight under the pointer */
        }
    if (g->kbd && !lost && !won) {
        KRect r = kr(g->bx + g->kx * g->cell, g->by + g->ky * g->cell, g->cell, g->cell);
        kc_outline(cv, r, 0xD0182C);
        kc_outline(cv, kr_inset(r, 1), 0xFFF4F4);
    }
}

static void draw_all(Game *g, KUI *ui) {
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    /* header */
    KRect header = kr(8, MENU_H + 6, g->win_w - 16, HEADER_H);
    kc_panel(cv, header, KB_SUNKEN, ui->theme->window_bg);
    int mines_left = g->board.mines - g->board.flags;
    art_draw_led(cv, header.x + 8, header.y + (HEADER_H - LED_H) / 2, mines_left);
    long ms = elapsed_ms(g);
    int secs = (int)(ms / 1000);
    art_draw_led(cv, header.x + header.w - 8 - LED_W, header.y + (HEADER_H - LED_H) / 2, secs);
    KRect fr = face_rect(g);
    bool down = g->face_down && ui->mouse_inside && kr_contains(fr, ui->mx, ui->my);
    kc_panel(cv, fr, down ? KB_SUNKEN : KB_RAISED, ui->theme->button_bg);
    ks_blit(cv, fr.x + 2 + (down ? 1 : 0), fr.y + 2 + (down ? 1 : 0), g->art.face[g->face]);
    if (kr_contains(fr, ui->mx, ui->my)) k_tooltip_text(ui, "New game (F2)");
    /* board frame */
    int bw = g->board.w * g->cell, bh = g->board.h * g->cell;
    KRect frame = kr(g->bx - 3, g->by - 3, bw + 6, bh + 6);
    kc_panel(cv, frame, KB_SUNKEN, kc_shade(ui->theme->window_bg, 86));
    kc_hline(cv, frame.x + 2, frame.y + 2, frame.w - 4, kc_shade(ui->theme->window_bg, 70));
    draw_board(g, ui);
    /* status bar */
    KRect sb = kr(8, g->win_h - 6 - STATUS_H, g->win_w - 16, STATUS_H);
    kc_panel(cv, sb, KB_SUNKEN, ui->theme->window_bg);
    kc_text_in(cv, kr(sb.x + 4, sb.y, sb.w - 8, sb.h), g->status, ui->theme->text, KA_LEFT);
    g->shown_time = secs;
    g->shown_mines = mines_left;
    g->shown_hover_x = g->hover_x;
    g->shown_hover_y = g->hover_y;
    g->shown_face = g->face;
}

static void draw(KApp *app, KUI *ui, void *user) {
    Game *g = user;
    KCanvas *cv = &ui->cv;
    if (cv->w != g->win_w || cv->h != g->win_h) {                  /* the window server has not resized us yet */
        k_damage(ui, kr(0, 0, 0, 0));
        return;
    }
    if (cv->w != g->last_w || cv->h != g->last_h) { g->last_w = cv->w; g->last_h = cv->h; g->dirty = true; }
    bool menu_open = ui->menu_open != 0;
    if (!menu_open) {
        handle_pointer(g, ui);
        handle_keys(g, ui);
    }
    /* anything that shows on screen changed? (the clock, the counter, the face, the highlighted cell) */
    int secs = (int)(elapsed_ms(g) / 1000);
    if (secs != g->shown_time || g->board.mines - g->board.flags != g->shown_mines || g->face != g->shown_face) g->dirty = true;
    if (g->hover_x != g->shown_hover_x || g->hover_y != g->shown_hover_y) g->dirty = true;
    bool need = g->dirty || menu_open || g->menu_was_open;
    if (need) {
        draw_all(g, ui);
        g->dirty = false;
    }
    /* menu bar: drawn last so that its drop-downs cover the board; it also reads the pointer */
    static KMenuItem game_items[10], help_items[2];
    int ngame = 0;
    #define ITEM(lbl, ic, sc) game_items[ngame++] = (KMenuItem){ lbl, ic, false, false, sc }
    #define SEP() game_items[ngame++] = (KMenuItem){ "", 0, true, false, NULL }
    ITEM("New game", 0, "F2");
    SEP();
    ITEM("Beginner", g->level == LV_BEGINNER ? KI_CHECK : 0, NULL);
    ITEM("Intermediate", g->level == LV_INTERMEDIATE ? KI_CHECK : 0, NULL);
    ITEM("Expert", g->level == LV_EXPERT ? KI_CHECK : 0, NULL);
    ITEM("Custom...", g->level == LV_CUSTOM ? KI_CHECK : 0, NULL);
    SEP();
    ITEM("Best times...", 0, NULL);
    SEP();
    ITEM("Quit", 0, "Ctrl+Q");
    #undef ITEM
    #undef SEP
    help_items[0] = (KMenuItem){ "How to play", 0, false, false, "F1" };
    help_items[1] = (KMenuItem){ "About", 0, false, false, NULL };
    static const char *const titles[] = { "Game", "Help" };
    const KMenuItem *menus[] = { game_items, help_items };
    int counts[] = { ngame, 2 };
    int pick = k_menubar(ui, kr(0, 0, cv->w, MENU_H - 1), titles, menus, counts, 2);
    bool menu_now = ui->menu_open != 0;
    if (g->menu_was_open && !menu_now) { g->dirty = true; kapp_redraw(app); }     /* the drop-down closed: repaint what it covered */
    g->menu_was_open = menu_now;
    if (pick) {
        int m = pick >> 8, i = (pick & 255) - 1;
        if (m == 1) {
            switch (i) {
            case 0: new_game(g, false); break;
            case 2: g->level = LV_BEGINNER; new_game(g, true); break;
            case 3: g->level = LV_INTERMEDIATE; new_game(g, true); break;
            case 4: g->level = LV_EXPERT; new_game(g, true); break;
            case 5: open_custom(g); break;
            case 7: open_times(g); break;
            case 9: kapp_quit(app, 0); break;
            default: break;
            }
        } else if (m == 2) {
            if (i == 0) show_help(g);
            else show_about(g);
        }
        g->dirty = true;
        kapp_redraw(app);
    }
    if (k_key_pressed(ui, KEY_F1)) show_help(g);
    if (need || menu_now || pick) k_damage(ui, kr(0, 0, cv->w, cv->h));
    else k_damage(ui, kr(0, 0, cv->w, MENU_H + 1));              /* just the bar: its hover highlight */
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv) {
    Game *g = calloc(1, sizeof *g);
    if (!g) return 1;
    int forced_w = 0, forced_h = 0, forced_m = 0;
    g->level = LV_BEGINNER;
    g->cell = 20;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) return ms_selftest();
        else if (!strcmp(argv[i], "--trace")) g->trace = true;
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) { g->fixed_seed = true; g->seed = (uint32_t)strtoul(argv[++i], NULL, 10); }
        else if (!strcmp(argv[i], "--cell") && i + 1 < argc) { g->cell = atoi(argv[++i]); g->force_cell = true; if (g->cell < 16) g->cell = 16; if (g->cell > 32) g->cell = 32; }
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) {
            const char *l = argv[++i];
            g->level = !strcmp(l, "expert") ? LV_EXPERT : !strcmp(l, "intermediate") ? LV_INTERMEDIATE : LV_BEGINNER;
        } else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &forced_w, &forced_h);
        else if (!strcmp(argv[i], "--mines") && i + 1 < argc) forced_m = atoi(argv[++i]);
        else { fprintf(stderr, "usage: minesweeper [--selftest] [--level beginner|intermediate|expert] [--size WxH --mines N] [--seed N] [--cell PX] [--trace]\n"); return 2; }
    }
    if (forced_w && forced_h) {
        g->level = LV_CUSTOM;
        g->cw = forced_w;
        g->ch = forced_h;
        g->cmines = forced_m ? forced_m : forced_w * forced_h / 6;
    }
    g->cw = g->cw ? g->cw : 16;
    g->ch = g->ch ? g->ch : 16;
    g->cmines = g->cmines ? g->cmines : 40;
    kg_scores_init(&g->scores, true, 5);
    kg_scores_load(&g->scores, SCORE_FILE);
    g->hover_x = g->hover_y = -1;
    g->shown_time = g->shown_mines = -1000;
    g->shown_hover_x = g->shown_hover_y = -2;
    g->shown_face = -1;

    g->app = kapp_new("Minesweeper", 206, 300, FW_CENTER);
    if (!g->app) return 1;
    {
        int x, y, ow, oh;
        fairy_window_geometry(kapp_window(g->app), &x, &y, &ow, &oh);
        g->decor_w = ow - kapp_width(g->app);
        g->decor_h = oh - kapp_height(g->app);
        if (g->decor_w < 0 || g->decor_w > 40) g->decor_w = 8;
        if (g->decor_h < 0 || g->decor_h > 60) g->decor_h = 30;
    }
    new_game(g, true);
    g->last_w = kapp_width(g->app);
    g->last_h = kapp_height(g->app);
    kapp_set_draw(g->app, draw, g);
    int rc = kapp_run(g->app);
    art_free(&g->art);
    kapp_destroy(g->app);
    return rc;
}
