#include "board.h"
#include <string.h>

void board_clamp(int *w, int *h, int *mines) {
    if (*w < 9) *w = 9;                                     /* the smallest field the header still fits on */
    if (*w > MS_MAX_W) *w = MS_MAX_W;
    if (*h < 9) *h = 9;
    if (*h > MS_MAX_H) *h = MS_MAX_H;
    int max = (*w - 1) * (*h - 1);                          /* leaves room for the safe first click */
    if (*mines < 1) *mines = 1;
    if (*mines > max) *mines = max;
}

void board_new(Board *b, int w, int h, int mines, uint32_t seed) {
    memset(b, 0, sizeof *b);
    if (w > MS_MAX_W) w = MS_MAX_W;
    if (h > MS_MAX_H) h = MS_MAX_H;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (mines > w * h - 1) mines = w * h - 1;
    if (mines < 0) mines = 0;
    b->w = w;
    b->h = h;
    b->mines = mines;
    b->status = ST_READY;
    b->hit_x = b->hit_y = -1;
    kg_rng_seed(&b->rng, seed);
}

static void compute_adjacent(Board *b) {
    for (int y = 0; y < b->h; y++)
        for (int x = 0; x < b->w; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    if ((dx || dy) && board_in(b, x + dx, y + dy) && b->mine[board_idx(b, x + dx, y + dy)]) n++;
            b->adj[board_idx(b, x, y)] = (uint8_t)n;
        }
}

int board_count_mines(const Board *b) {
    int n = 0;
    for (int i = 0; i < b->w * b->h; i++) n += b->mine[i];
    return n;
}

void board_place_for_test(Board *b, const uint8_t *mine_map) {
    memcpy(b->mine, mine_map, (size_t)(b->w * b->h));
    b->mines = board_count_mines(b);
    compute_adjacent(b);
    b->placed = true;
}

/* The first click is always safe, and so is its neighbourhood when the field is big enough (so it opens up a bit). */
static void place_mines(Board *b, int safe_x, int safe_y) {
    int cells = b->w * b->h;
    bool ring = b->mines <= cells - 9;
    int cand[MS_MAX_W * MS_MAX_H], n = 0;
    for (int y = 0; y < b->h; y++)
        for (int x = 0; x < b->w; x++) {
            int dx = x - safe_x, dy = y - safe_y;
            bool excluded = ring ? (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1) : (dx == 0 && dy == 0);
            if (!excluded) cand[n++] = board_idx(b, x, y);
        }
    memset(b->mine, 0, sizeof b->mine);
    int want = b->mines < n ? b->mines : n;
    for (int i = 0; i < want; i++) {                        /* partial Fisher-Yates */
        int j = i + kg_rng_range(&b->rng, n - i);
        int t = cand[i]; cand[i] = cand[j]; cand[j] = t;
        b->mine[cand[i]] = 1;
    }
    b->mines = want;
    compute_adjacent(b);
    b->placed = true;
}

static void check_win(Board *b) {
    if (b->status == ST_PLAYING && b->opened == b->w * b->h - b->mines) {
        b->status = ST_WON;
        for (int i = 0; i < b->w * b->h; i++)
            if (b->mine[i] && b->cell[i] != CELL_FLAG) { b->cell[i] = CELL_FLAG; b->flags++; }
    }
}

int board_reveal(Board *b, int x, int y) {
    if (!board_in(b, x, y) || b->status == ST_WON || b->status == ST_LOST) return 0;
    int i = board_idx(b, x, y);
    if (b->cell[i] != CELL_COVERED) return 0;
    if (!b->placed) place_mines(b, x, y);
    if (b->status == ST_READY) b->status = ST_PLAYING;
    if (b->mine[i]) {
        b->status = ST_LOST;
        b->hit_x = x;
        b->hit_y = y;
        return -1;
    }
    int stack[MS_MAX_W * MS_MAX_H], sp = 0, opened = 0;
    b->cell[i] = CELL_OPEN;
    b->opened++;
    opened++;
    stack[sp++] = i;
    while (sp) {
        int c = stack[--sp];
        if (b->adj[c]) continue;                            /* only empty cells spread */
        int cx = c % b->w, cy = c / b->w;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int nx = cx + dx, ny = cy + dy;
                if (!board_in(b, nx, ny)) continue;
                int ni = board_idx(b, nx, ny);
                if (b->cell[ni] != CELL_COVERED || b->mine[ni]) continue;
                b->cell[ni] = CELL_OPEN;
                b->opened++;
                opened++;
                stack[sp++] = ni;
            }
    }
    check_win(b);
    return opened;
}

void board_flag(Board *b, int x, int y) {
    if (!board_in(b, x, y) || b->status == ST_WON || b->status == ST_LOST) return;
    int i = board_idx(b, x, y);
    if (b->cell[i] == CELL_COVERED) { b->cell[i] = CELL_FLAG; b->flags++; }
    else if (b->cell[i] == CELL_FLAG) { b->cell[i] = CELL_COVERED; b->flags--; }
}

int board_flags_around(const Board *b, int x, int y) {
    int n = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if ((dx || dy) && board_in(b, x + dx, y + dy) && b->cell[board_idx(b, x + dx, y + dy)] == CELL_FLAG) n++;
    return n;
}

int board_chord(Board *b, int x, int y) {
    if (!board_in(b, x, y) || b->status != ST_PLAYING) return 0;
    int i = board_idx(b, x, y);
    if (b->cell[i] != CELL_OPEN || b->adj[i] == 0) return 0;
    if (board_flags_around(b, x, y) != b->adj[i]) return 0;
    int total = 0;
    bool lost = false;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if (!board_in(b, nx, ny) || b->cell[board_idx(b, nx, ny)] != CELL_COVERED) continue;
            int r = board_reveal(b, nx, ny);
            if (r < 0) lost = true;
            else total += r;
        }
    return lost ? -1 : total;
}
