#include "logic.h"
#include <string.h>

/* Stone lanterns appear as the level rises. Each entry is a rectangle of cells, placed once the level is reached. The layouts
 * keep the border and the middle row free so that the snake can always get around. */
static const struct { int level, x, y, w, h; } STONES[] = {
    { 3, 5, 4, 1, 1 },  { 3, 18, 4, 1, 1 }, { 3, 5, 13, 1, 1 }, { 3, 18, 13, 1, 1 },
    { 5, 10, 3, 4, 1 }, { 5, 10, 14, 4, 1 },
    { 7, 3, 7, 1, 4 },  { 7, 20, 7, 1, 4 },
    { 9, 8, 5, 3, 1 },  { 9, 8, 6, 1, 2 }, { 9, 13, 12, 3, 1 }, { 9, 15, 10, 1, 2 },
};
#define NSTONES ((int)(sizeof STONES / sizeof STONES[0]))

static const int DX[4] = { 0, 1, 0, -1 };
static const int DY[4] = { -1, 0, 1, 0 };

static bool pt_eq(SPt a, SPt b) { return a.x == b.x && a.y == b.y; }

bool sg_is_snake(const SnakeGame *g, int x, int y) {
    for (int i = 0; i < g->len; i++) if (g->body[i].x == x && g->body[i].y == y) return true;
    return false;
}

bool sg_is_stone(const SnakeGame *g, int x, int y) {
    return x >= 0 && y >= 0 && x < SN_W && y < SN_H && g->obst[sg_idx(x, y)];
}

int sg_level_stones(int level) {
    int n = 0;
    for (int i = 0; i < NSTONES; i++) if (STONES[i].level <= level) n += STONES[i].w * STONES[i].h;
    return n;
}

int sg_step_ms(const SnakeGame *g) {
    int l = g->level < SN_MAX_LEVEL ? g->level : SN_MAX_LEVEL;
    int ms = 200 - 13 * (l - 1);
    return ms < 60 ? 60 : ms;
}

static bool cell_free(const SnakeGame *g, int x, int y) {
    if (sg_is_snake(g, x, y) || sg_is_stone(g, x, y)) return false;
    if (g->food.x == x && g->food.y == y) return false;
    if (g->gold_ttl > 0 && g->gold.x == x && g->gold.y == y) return false;
    return true;
}

static bool random_free_cell(SnakeGame *g, SPt *out) {
    for (int tries = 0; tries < 300; tries++) {
        int x = kg_rng_range(&g->rng, SN_W), y = kg_rng_range(&g->rng, SN_H);
        if (cell_free(g, x, y)) { out->x = (int8_t)x; out->y = (int8_t)y; return true; }
    }
    int start = kg_rng_range(&g->rng, SN_CELLS);                  /* crowded: scan from a random place */
    for (int i = 0; i < SN_CELLS; i++) {
        int c = (start + i) % SN_CELLS, x = c % SN_W, y = c / SN_W;
        if (cell_free(g, x, y)) { out->x = (int8_t)x; out->y = (int8_t)y; return true; }
    }
    return false;
}

static int add_stones(SnakeGame *g) {
    int added = 0;
    SPt head = g->body[0];
    for (int i = 0; i < NSTONES; i++) {
        if (STONES[i].level > g->level) continue;
        for (int y = STONES[i].y; y < STONES[i].y + STONES[i].h; y++)
            for (int x = STONES[i].x; x < STONES[i].x + STONES[i].w; x++) {
                if (g->obst[sg_idx(x, y)] || !cell_free(g, x, y)) continue;
                int dist = (x > head.x ? x - head.x : head.x - x) + (y > head.y ? y - head.y : head.y - y);
                if (dist < 4) continue;                              /* never right in front of the snake */
                g->obst[sg_idx(x, y)] = 1;
                g->n_obst++;
                added++;
            }
    }
    return added;
}

void sg_new(SnakeGame *g, bool wrap, int start_level, uint32_t seed) {
    memset(g, 0, sizeof *g);
    g->wrap = wrap;
    if (start_level < 1) start_level = 1;
    if (start_level > 9) start_level = 9;
    g->start_level = g->level = start_level;
    g->len = 3;
    g->body[0] = (SPt){ SN_W / 2, SN_H / 2 };
    g->body[1] = (SPt){ SN_W / 2 - 1, SN_H / 2 };
    g->body[2] = (SPt){ SN_W / 2 - 2, SN_H / 2 };
    g->dir = DIR_RIGHT;
    g->old_tail = g->body[2];
    g->prev_head = g->body[1];
    g->status = SS_PLAYING;
    g->food = (SPt){ -1, -1 };
    kg_rng_seed(&g->rng, seed);
    add_stones(g);
    random_free_cell(g, &g->food);
}

static int opposite(int d) { return (d + 2) & 3; }

void sg_turn(SnakeGame *g, int dir) {
    if (dir < 0 || dir > 3 || g->status != SS_PLAYING) return;
    int last = g->nq ? g->queue[g->nq - 1] : g->dir;
    if (dir == last || dir == opposite(last)) return;
    if (g->nq < 2) g->queue[g->nq++] = dir;
}

static void die(SnakeGame *g, int why) {
    g->status = SS_DEAD;
    g->death = why;
}

void sg_step(SnakeGame *g) {
    if (g->status != SS_PLAYING) return;
    g->ev_eat = EAT_NONE;
    g->ev_level_up = g->ev_gold_spawn = g->ev_gold_expire = false;
    g->ev_new_stones = 0;
    if (g->nq) {
        g->dir = g->queue[0];
        g->queue[0] = g->queue[1];
        g->nq--;
    }
    SPt head = g->body[0];
    int nx = head.x + DX[g->dir], ny = head.y + DY[g->dir];
    if (nx < 0 || ny < 0 || nx >= SN_W || ny >= SN_H) {
        if (!g->wrap) { die(g, DEATH_WALL); return; }
        nx = (nx + SN_W) % SN_W;
        ny = (ny + SN_H) % SN_H;
    }
    SPt nh = { (int8_t)nx, (int8_t)ny };
    if (g->obst[sg_idx(nx, ny)]) { die(g, DEATH_STONE); return; }
    bool eats_food = pt_eq(nh, g->food);
    bool eats_gold = g->gold_ttl > 0 && pt_eq(nh, g->gold);
    bool grow = eats_food || eats_gold;
    for (int i = 0; i < g->len; i++) {
        if (!pt_eq(g->body[i], nh)) continue;
        if (i == g->len - 1 && !grow) continue;                     /* the tail moves out of the way */
        die(g, DEATH_SELF);
        return;
    }
    g->prev_head = head;
    g->old_tail = g->body[g->len - 1];
    int move = grow ? g->len : g->len - 1;
    memmove(&g->body[1], &g->body[0], (size_t)move * sizeof g->body[0]);
    g->body[0] = nh;
    if (grow) g->len++;
    g->grew = grow;
    g->steps++;

    if (g->gold_ttl > 0 && !eats_gold) {
        if (--g->gold_ttl == 0) g->ev_gold_expire = true;
    }
    if (eats_gold) {
        g->score += 50L * g->level + (long)g->gold_ttl * g->level;
        g->gold_ttl = 0;
        g->ev_eat = EAT_GOLD;
        g->ev_eat_at = nh;
        g->eaten++;
    }
    if (eats_food) {
        g->score += 10L * g->level;
        g->ev_eat = EAT_DANGO;
        g->ev_eat_at = nh;
        g->eaten++;
        int level = g->start_level + g->eaten / SN_PER_LEVEL;
        if (level > 20) level = 20;
        if (level != g->level) {
            g->level = level;
            g->ev_level_up = true;
            g->ev_new_stones = add_stones(g);
        }
        if (!random_free_cell(g, &g->food)) { g->food = (SPt){ -1, -1 }; die(g, DEATH_NONE); return; }   /* the whole field is full */
        if (g->level >= 2 && g->eaten % 7 == 0 && g->gold_ttl == 0 && random_free_cell(g, &g->gold)) {
            g->gold_ttl = SN_GOLD_STEPS;
            g->ev_gold_spawn = true;
        }
    }
}

/* ---------------------------------------------------------------- autopilot */

/* Cells reachable from (x, y) without crossing the snake (the tail counts as free: it will have moved) or a stone. */
static int reachable(const SnakeGame *g, int x, int y, int limit) {
    static int16_t queue[SN_CELLS];
    uint8_t seen[SN_CELLS];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < g->len - 1; i++) seen[sg_idx(g->body[i].x, g->body[i].y)] = 1;
    int head = 0, tail = 0, count = 0;
    queue[tail++] = (int16_t)sg_idx(x, y);
    seen[sg_idx(x, y)] = 1;
    while (head < tail && count < limit) {
        int c = queue[head++], cx = c % SN_W, cy = c / SN_W;
        count++;
        for (int d = 0; d < 4; d++) {
            int nx = cx + DX[d], ny = cy + DY[d];
            if (nx < 0 || ny < 0 || nx >= SN_W || ny >= SN_H) {
                if (!g->wrap) continue;
                nx = (nx + SN_W) % SN_W;
                ny = (ny + SN_H) % SN_H;
            }
            int ni = sg_idx(nx, ny);
            if (seen[ni] || g->obst[ni]) continue;
            seen[ni] = 1;
            queue[tail++] = (int16_t)ni;
        }
    }
    return count;
}

static int axis_dist(int a, int b, int n, bool wrap) {
    int d = a > b ? a - b : b - a;
    if (wrap && n - d < d) d = n - d;
    return d;
}

/* Shortest path from the head to `target` around the body and the stones; returns the first direction or -1. */
static int path_first_step(const SnakeGame *g, SPt target) {
    static int16_t queue[SN_CELLS];
    int8_t first[SN_CELLS];
    uint8_t seen[SN_CELLS];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < g->len - 1; i++) seen[sg_idx(g->body[i].x, g->body[i].y)] = 1;   /* the tail will have moved */
    int head = 0, tail = 0;
    SPt h = g->body[0];
    seen[sg_idx(h.x, h.y)] = 1;
    for (int d = 0; d < 4; d++) {
        if (d == ((g->dir + 2) & 3)) continue;
        int nx = h.x + DX[d], ny = h.y + DY[d];
        if (nx < 0 || ny < 0 || nx >= SN_W || ny >= SN_H) {
            if (!g->wrap) continue;
            nx = (nx + SN_W) % SN_W;
            ny = (ny + SN_H) % SN_H;
        }
        int ni = sg_idx(nx, ny);
        if (seen[ni] || g->obst[ni]) continue;
        seen[ni] = 1;
        first[ni] = (int8_t)d;
        queue[tail++] = (int16_t)ni;
    }
    while (head < tail) {
        int c = queue[head++], cx = c % SN_W, cy = c / SN_W;
        if (cx == target.x && cy == target.y) return first[c];
        for (int d = 0; d < 4; d++) {
            int nx = cx + DX[d], ny = cy + DY[d];
            if (nx < 0 || ny < 0 || nx >= SN_W || ny >= SN_H) {
                if (!g->wrap) continue;
                nx = (nx + SN_W) % SN_W;
                ny = (ny + SN_H) % SN_H;
            }
            int ni = sg_idx(nx, ny);
            if (seen[ni] || g->obst[ni]) continue;
            seen[ni] = 1;
            first[ni] = first[c];
            queue[tail++] = (int16_t)ni;
        }
    }
    return -1;
}

int sg_bot_dir(const SnakeGame *g) {
    SPt target = g->food;
    if (g->gold_ttl > 0) {
        int dg = axis_dist(g->body[0].x, g->gold.x, SN_W, g->wrap) + axis_dist(g->body[0].y, g->gold.y, SN_H, g->wrap);
        int df = axis_dist(g->body[0].x, g->food.x, SN_W, g->wrap) + axis_dist(g->body[0].y, g->food.y, SN_H, g->wrap);
        if (dg < g->gold_ttl - 4 && dg <= df + 6) target = g->gold;
    }
    int d = target.x >= 0 ? path_first_step(g, target) : -1;
    if (d >= 0) {
        int nx = g->body[0].x + DX[d], ny = g->body[0].y + DY[d];
        if (g->wrap) { nx = (nx + SN_W) % SN_W; ny = (ny + SN_H) % SN_H; }
        if (reachable(g, nx, ny, g->len + 40) >= g->len + 2) return d;       /* the way is clear and does not trap us */
    }
    /* otherwise stay alive: the move with the most room around it */
    int best = g->dir, best_area = -1;
    for (int k = 0; k < 4; k++) {
        if (k == ((g->dir + 2) & 3)) continue;
        int nx = g->body[0].x + DX[k], ny = g->body[0].y + DY[k];
        if (nx < 0 || ny < 0 || nx >= SN_W || ny >= SN_H) {
            if (!g->wrap) continue;
            nx = (nx + SN_W) % SN_W;
            ny = (ny + SN_H) % SN_H;
        }
        if (g->obst[sg_idx(nx, ny)]) continue;
        bool body = false;
        for (int i = 0; i < g->len - 1; i++) if (g->body[i].x == nx && g->body[i].y == ny) { body = true; break; }
        if (body) continue;
        int area = reachable(g, nx, ny, SN_CELLS);
        if (area > best_area) { best_area = area; best = k; }
    }
    return best;
}
