/*
 * The crossword of the day. The grid is generated from the word list, seeded by the date: the first word goes down in the
 * middle, every further word must cross exactly the way crosswords do (no side-by-side letters, nothing stretched past its
 * ends). The reader can click a cell and type, check the letters, reveal the answers or clear the grid.
 */
#include "news.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define GW 27                           /* working grid, larger than any puzzle */
#define MAXW 9

typedef struct Placed { int word, x, y; bool across; } Placed;

typedef struct Work {
    char g[GW][GW];
    Placed pl[16];
    int n;
} Work;

static int wlen(int w) { return (int)strlen(g_words[w].word); }

static bool inside(int x, int y) { return x >= 0 && y >= 0 && x < GW && y < GW; }

/* can `w` go at (x,y) in the given direction? Returns the number of crossings or -1. */
static int try_place(const Work *k, int w, int x, int y, bool across) {
    const char *s = g_words[w].word;
    int len = (int)strlen(s), dx = across ? 1 : 0, dy = across ? 0 : 1;
    int ex = x + dx * len, ey = y + dy * len;
    if (!inside(x, y) || !inside(x + dx * (len - 1), y + dy * (len - 1))) return -1;
    if (inside(x - dx, y - dy) && k->g[y - dy][x - dx]) return -1;                  /* nothing before the first letter... */
    if (inside(ex, ey) && k->g[ey][ex]) return -1;                                  /* ...or after the last */
    int crossings = 0;
    for (int i = 0; i < len; i++) {
        int cx = x + dx * i, cy = y + dy * i;
        char have = k->g[cy][cx];
        if (have) {
            if (have != s[i]) return -1;
            crossings++;
            /* the cell must not already belong to a word that runs the same way */
            bool before = inside(cx - dx, cy - dy) && k->g[cy - dy][cx - dx];
            bool after = inside(cx + dx, cy + dy) && k->g[cy + dy][cx + dx];
            if (before || after) return -1;
        } else {
            int px = cx + dy, py = cy + dx, nx = cx - dy, ny = cy - dx;           /* the two neighbours across the word */
            if (inside(px, py) && k->g[py][px]) return -1;
            if (inside(nx, ny) && k->g[ny][nx]) return -1;
        }
    }
    return crossings;
}

static void do_place(Work *k, int w, int x, int y, bool across) {
    const char *s = g_words[w].word;
    for (int i = 0; s[i]; i++) k->g[y + (across ? 0 : i)][x + (across ? i : 0)] = s[i];
    k->pl[k->n++] = (Placed){ w, x, y, across };
}

static bool already_used(const Work *k, int w) {
    for (int i = 0; i < k->n; i++) if (k->pl[i].word == w) return true;
    return false;
}

static void bbox(const Work *k, int *x0, int *y0, int *x1, int *y1) {
    *x0 = GW; *y0 = GW; *x1 = -1; *y1 = -1;
    for (int y = 0; y < GW; y++)
        for (int x = 0; x < GW; x++)
            if (k->g[y][x]) {
                if (x < *x0) *x0 = x;
                if (x > *x1) *x1 = x;
                if (y < *y0) *y0 = y;
                if (y > *y1) *y1 = y;
            }
}

static bool generate(Work *k, uint32_t seed) {
    memset(k, 0, sizeof *k);
    int order[128];
    int m = 0;
    for (int i = 0; i < g_nwords && m < 128; i++) { int l = wlen(i); if (l >= 3 && l <= MAXW) order[m++] = i; }
    shuffle_indices(order, m, seed);
    /* the first word runs across the middle */
    int first = -1;
    for (int i = 0; i < m; i++) if (wlen(order[i]) >= 6) { first = i; break; }
    if (first < 0) return false;
    int w0 = order[first];
    order[first] = order[0];
    order[0] = w0;
    do_place(k, w0, (GW - wlen(w0)) / 2, GW / 2, true);
    for (int pass = 0; pass < 3 && k->n < 9; pass++) {
        for (int oi = 1; oi < m && k->n < 9; oi++) {
            int w = order[oi];
            if (already_used(k, w)) continue;
            const char *s = g_words[w].word;
            int best = -1, bx = 0, by = 0, bscore = -1;
            bool bacross = false;
            for (int y = 0; y < GW; y++)
                for (int x = 0; x < GW; x++) {
                    char c = k->g[y][x];
                    if (!c) continue;
                    for (int i = 0; s[i]; i++) {
                        if (s[i] != c) continue;
                        for (int dir = 0; dir < 2; dir++) {
                            bool across = dir == 0;
                            int px = across ? x - i : x, py = across ? y : y - i;
                            int cr = try_place(k, w, px, py, across);
                            if (cr < 1) continue;
                            /* prefer more crossings, then a compact grid (placements near the centre) */
                            int dxm = px + (across ? wlen(w) / 2 : 0) - GW / 2, dym = py + (across ? 0 : wlen(w) / 2) - GW / 2;
                            int score = cr * 100 - (abs(dxm) + abs(dym));
                            if (score > bscore) { bscore = score; best = w; bx = px; by = py; bacross = across; }
                        }
                    }
                }
            if (best >= 0) do_place(k, best, bx, by, bacross);
        }
    }
    int x0, y0, x1, y1;
    bbox(k, &x0, &y0, &x1, &y1);
    return k->n >= 6 && x1 - x0 + 1 <= PZ_MAX && y1 - y0 + 1 <= PZ_MAX;
}

void puzzle_build(Puzzle *p, long daynum) {
    memset(p, 0, sizeof *p);
    p->sel_x = p->sel_y = -1;
    p->across = true;
    Work best, w;
    int best_n = -1, best_area = 1 << 30;
    bool have = false;
    for (int attempt = 0; attempt < 60; attempt++) {
        if (!generate(&w, hash32((uint32_t)daynum * 40503u + (uint32_t)attempt * 7919u + 11u))) continue;
        int x0, y0, x1, y1;
        bbox(&w, &x0, &y0, &x1, &y1);
        int area = (x1 - x0 + 1) * (y1 - y0 + 1);
        if (w.n > best_n || (w.n == best_n && area < best_area)) { best = w; best_n = w.n; best_area = area; have = true; }
        if (best_n >= 8 && best_area <= 90) break;
    }
    if (!have) return;
    int x0, y0, x1, y1;
    bbox(&best, &x0, &y0, &x1, &y1);
    p->w = x1 - x0 + 1;
    p->h = y1 - y0 + 1;
    for (int y = 0; y < p->h; y++)
        for (int x = 0; x < p->w; x++) p->sol[y][x] = best.g[y0 + y][x0 + x];
    /* number the cells where a word starts */
    int number = 0;
    for (int y = 0; y < p->h; y++)
        for (int x = 0; x < p->w; x++) {
            if (!p->sol[y][x]) continue;
            bool across_start = (x == 0 || !p->sol[y][x - 1]) && x + 1 < p->w && p->sol[y][x + 1];
            bool down_start = (y == 0 || !p->sol[y - 1][x]) && y + 1 < p->h && p->sol[y + 1][x];
            if (!across_start && !down_start) continue;
            p->num[y][x] = ++number;
            for (int dir = 0; dir < 2; dir++) {
                bool across = dir == 0;
                if (!(across ? across_start : down_start)) continue;
                int len = 0;
                while ((across ? x + len < p->w : y + len < p->h) && p->sol[y + (across ? 0 : len)][x + (across ? len : 0)]) len++;
                if (p->nwords >= 16) continue;
                const char *clue = "?", *word = "";
                for (int i = 0; i < best.n; i++)
                    if (best.pl[i].across == across && best.pl[i].x - x0 == x && best.pl[i].y - y0 == y) { clue = g_words[best.pl[i].word].clue; word = g_words[best.pl[i].word].word; }
                p->words[p->nwords].num = number;
                p->words[p->nwords].across = across;
                p->words[p->nwords].x = x;
                p->words[p->nwords].y = y;
                p->words[p->nwords].len = len;
                p->words[p->nwords].clue = clue;
                p->words[p->nwords].word = word;
                p->nwords++;
            }
        }
}

void puzzle_dump(const Puzzle *p) {
    printf("crossword %d x %d, %d words\n", p->w, p->h, p->nwords);
    for (int y = 0; y < p->h; y++) {
        for (int x = 0; x < p->w; x++) putchar(p->sol[y][x] ? p->sol[y][x] : '.');
        putchar('\n');
    }
    for (int i = 0; i < p->nwords; i++)
        printf("%d %s (%d): %s = %s\n", p->words[i].num, p->words[i].across ? "across" : "down", p->words[i].len, p->words[i].clue, p->words[i].word);
}

/* ---------------------------------------------------------------- drawing and typing */

static const uint8_t k_digits[10][5] = {
    { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 }, { 5, 5, 7, 1, 1 },
    { 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 }, { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 },
};

static void tiny_number(KCanvas *cv, int x, int y, int n, uint32_t color) {
    char s[8];
    snprintf(s, sizeof s, "%d", n);
    for (int i = 0; s[i]; i++) {
        int d = s[i] - '0';
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < 3; c++)
                if (k_digits[d][r] & (4 >> c)) kc_pixel(cv, x + i * 4 + c, y + r, color);
    }
}

static int cell_size(const Puzzle *p, int width) {
    int cs = (width - 4) / (p->w > 0 ? p->w : 1);
    if (cs > 28) cs = 28;
    if (cs < 16) cs = 16;
    return cs;
}

int puzzle_height(const Puzzle *p, int width) { return p->h * cell_size(p, width) + 4 + 34; }

static bool solved(const Puzzle *p) {
    for (int y = 0; y < p->h; y++) for (int x = 0; x < p->w; x++) if (p->sol[y][x] && p->fill[y][x] != p->sol[y][x]) return false;
    return p->w > 0;
}

void puzzle_reveal(Puzzle *p) {
    for (int y = 0; y < p->h; y++) for (int x = 0; x < p->w; x++) p->fill[y][x] = p->sol[y][x];
    p->solved = true;
}

void puzzle_clear(Puzzle *p) {
    memset(p->fill, 0, sizeof p->fill);
    p->checked = false;
    p->solved = false;
}

void puzzle_check(Puzzle *p) {
    p->checked = true;
    p->solved = solved(p);
}

static void move_sel(Puzzle *p, int dx, int dy) {
    int x = p->sel_x + dx, y = p->sel_y + dy;
    if (x < 0 || y < 0 || x >= p->w || y >= p->h) return;
    p->sel_x = x;
    p->sel_y = y;
}

bool puzzle_key(Puzzle *p, const KKey *k) {
    if (p->sel_x < 0) return false;
    if (k->mods & (MOD_CTRL | MOD_ALT)) return false;
    switch (k->code) {
    case KEY_LEFT: move_sel(p, -1, 0); return true;
    case KEY_RIGHT: move_sel(p, 1, 0); return true;
    case KEY_UP: move_sel(p, 0, -1); return true;
    case KEY_DOWN: move_sel(p, 0, 1); return true;
    case KEY_TAB: case KEY_ENTER: p->across = !p->across; return true;
    case KEY_ESC: p->sel_x = p->sel_y = -1; return true;
    case KEY_BACKSPACE: case KEY_DELETE:
        if (p->fill[p->sel_y][p->sel_x]) p->fill[p->sel_y][p->sel_x] = 0;
        else move_sel(p, p->across ? -1 : 0, p->across ? 0 : -1);
        p->checked = false;
        return true;
    default: break;
    }
    bool letter = (k->ch >= 'a' && k->ch <= 'z') || (k->ch >= 'A' && k->ch <= 'Z');
    if (!letter) return false;
    if (!p->sol[p->sel_y][p->sel_x]) return true;
    p->fill[p->sel_y][p->sel_x] = (char)toupper((int)k->ch);
    p->checked = false;
    p->solved = solved(p);
    /* step to the next letter cell in the typing direction */
    int dx = p->across ? 1 : 0, dy = p->across ? 0 : 1;
    int nx = p->sel_x + dx, ny = p->sel_y + dy;
    if (nx < p->w && ny < p->h && p->sol[ny][nx]) { p->sel_x = nx; p->sel_y = ny; }
    return true;
}

void puzzle_draw(KUI *ui, KRect r, Puzzle *p) {
    KCanvas *cv = &ui->cv;
    if (p->w == 0) { kc_text(cv, r.x, r.y, "(no puzzle today)", PAGE_DIM); return; }
    int cs = cell_size(p, r.w);
    int gx = r.x + (r.w - cs * p->w) / 2, gy = r.y + 2;
    for (int y = 0; y < p->h; y++)
        for (int x = 0; x < p->w; x++) {
            KRect c = kr(gx + x * cs, gy + y * cs, cs, cs);
            char s = p->sol[y][x];
            if (!s) { kc_fill(cv, c, 0x2A2832); continue; }
            bool sel = x == p->sel_x && y == p->sel_y;
            bool in_word = false;
            if (p->sel_x >= 0 && p->sol[p->sel_y][p->sel_x]) {
                int dx = p->across ? 1 : 0, dy = p->across ? 0 : 1;
                int sx = p->sel_x, sy = p->sel_y;
                while (sx - dx >= 0 && sy - dy >= 0 && p->sol[sy - dy][sx - dx]) { sx -= dx; sy -= dy; }
                while (sx < p->w && sy < p->h && p->sol[sy][sx]) { if (sx == x && sy == y) in_word = true; sx += dx; sy += dy; }
            }
            uint32_t bg = sel ? 0xF6C9A8 : in_word ? 0xFBE9C8 : 0xFFFDF4;
            kc_fill(cv, c, bg);
            kc_outline(cv, c, 0x2A2832);
            if (p->num[y][x]) tiny_number(cv, c.x + 3, c.y + 3, p->num[y][x], PAGE_DIM);
            char f = p->fill[y][x];
            if (f) {
                char t[2] = { f, 0 };
                uint32_t col = PAGE_INK;
                if (p->checked && f != s) col = PAGE_ACCENT;
                kc_text_bold(cv, c.x + (cs - 8) / 2, c.y + (cs - 16) / 2 + 3, t, col);
            }
            if (k_hover(ui, c) && ui->pressed[0]) {
                if (sel) p->across = !p->across;
                p->sel_x = x;
                p->sel_y = y;
                ui->focus = 0;
                news_trace("bunbunmaru: puzzle cell %d,%d", x, y);
            }
        }
    /* clicks outside the grid leave it */
    if (ui->pressed[0] && !kr_contains(kr(gx, gy, cs * p->w, cs * p->h), ui->mx, ui->my) && !k_hover(ui, kr(r.x, r.y + p->h * cs + 4, r.w, 34))) {
        p->sel_x = p->sel_y = -1;
    }
    int by = gy + p->h * cs + 6;
    KRect b1 = kr(r.x + 2, by, 70, 24), b2 = kr(r.x + 76, by, 80, 24), b3 = kr(r.x + 160, by, 70, 24);
    if (k_button(ui, b1, "Check")) { puzzle_check(p); news_trace("bunbunmaru: puzzle checked%s", p->solved ? " (solved)" : ""); }
    if (k_button(ui, b2, "Reveal")) { puzzle_reveal(p); news_trace("bunbunmaru: puzzle revealed"); }
    if (k_button(ui, b3, "Clear")) { puzzle_clear(p); news_trace("bunbunmaru: puzzle cleared"); }
    if (p->solved) kc_text_bold(cv, r.x + 238, by + 4, "Solved!", 0x1E7A3C);
}
