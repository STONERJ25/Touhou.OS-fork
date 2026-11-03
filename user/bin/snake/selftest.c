/* snake --selftest: the game rules and the score table, without a window. */
#include "logic.h"
#include <stdio.h>
#include <string.h>

static int g_checks, g_failed;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_failed++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static const int DX[4] = { 0, 1, 0, -1 };
static const int DY[4] = { -1, 0, 1, 0 };

static void put_food_ahead(SnakeGame *g) {
    int x = g->body[0].x + DX[g->dir], y = g->body[0].y + DY[g->dir];
    if (g->wrap) { x = (x + SN_W) % SN_W; y = (y + SN_H) % SN_H; }
    g->food = (SPt){ (int8_t)x, (int8_t)y };
}

static void check_invariants(SnakeGame *g, const char *where, int seed) {
    bool ok = true;
    for (int i = 0; i < g->len && ok; i++) {
        if (g->body[i].x < 0 || g->body[i].x >= SN_W || g->body[i].y < 0 || g->body[i].y >= SN_H) ok = false;
        for (int j = 0; j < i && ok; j++) if (g->body[i].x == g->body[j].x && g->body[i].y == g->body[j].y) ok = false;
        if (ok && g->obst[sg_idx(g->body[i].x, g->body[i].y)]) ok = false;
    }
    CHECK(ok, "%s: body cells must be on the field, distinct and off the stones (seed %d)", where, seed);
    if (g->status == SS_PLAYING || g->death != DEATH_NONE) {
        CHECK(g->food.x >= 0 && !sg_is_snake(g, g->food.x, g->food.y) && !sg_is_stone(g, g->food.x, g->food.y),
              "%s: the dango must lie on a free cell (seed %d)", where, seed);
    }
}

static void test_basics(void) {
    SnakeGame g;
    sg_new(&g, false, 1, 7);
    CHECK(g.len == 3 && g.dir == DIR_RIGHT && g.status == SS_PLAYING, "initial snake");
    CHECK(g.n_obst == 0 && g.level == 1, "no stones on level 1");
    SPt h0 = g.body[0], t0 = g.body[2];
    sg_step(&g);
    CHECK(g.body[0].x == h0.x + 1 && g.body[0].y == h0.y, "head moves right");
    CHECK(g.len == 3 && g.old_tail.x == t0.x && g.old_tail.y == t0.y && !g.grew, "tail follows, length unchanged");
    CHECK(g.body[1].x == h0.x && g.body[2].x == h0.x - 1, "body follows the head");

    /* turning */
    sg_new(&g, false, 1, 7);
    sg_turn(&g, DIR_LEFT);
    CHECK(g.nq == 0, "a 180 degree turn is ignored");
    sg_turn(&g, DIR_RIGHT);
    CHECK(g.nq == 0, "repeating the direction is ignored");
    sg_turn(&g, DIR_UP);
    sg_turn(&g, DIR_LEFT);
    CHECK(g.nq == 2, "two turns can be queued");
    sg_turn(&g, DIR_DOWN);
    CHECK(g.nq == 2, "no more than two");
    SPt h = g.body[0];
    sg_step(&g);
    CHECK(g.dir == DIR_UP && g.body[0].y == h.y - 1 && g.body[0].x == h.x, "first queued turn is taken");
    sg_step(&g);
    CHECK(g.dir == DIR_LEFT && g.body[0].x == h.x - 1, "second queued turn follows on the next step");
    sg_new(&g, false, 1, 7);
    sg_turn(&g, DIR_UP);
    sg_turn(&g, DIR_LEFT);                                  /* up then left is fine; left would be a reversal only from right */
    sg_step(&g);
    sg_step(&g);
    CHECK(g.status == SS_PLAYING, "quick up-left turn does not kill");
}

static void test_walls(void) {
    SnakeGame g;
    sg_new(&g, false, 1, 3);
    g.food = (SPt){ 0, 0 };
    int steps = 0;
    while (g.status == SS_PLAYING && steps < 100) { sg_step(&g); steps++; }
    CHECK(g.status == SS_DEAD && g.death == DEATH_WALL, "running into the wall kills");
    CHECK(g.body[0].x == SN_W - 1, "the head stays at the edge (x=%d)", g.body[0].x);
    CHECK(steps == SN_W - SN_W / 2, "died after %d steps", steps);
    int score = (int)g.score;
    sg_step(&g);
    CHECK(g.score == score && g.status == SS_DEAD, "a dead snake does not move");

    sg_new(&g, true, 1, 3);
    g.food = (SPt){ 0, 0 };
    for (int i = 0; i < SN_W / 2; i++) sg_step(&g);
    CHECK(g.status == SS_PLAYING && g.body[0].x == 0 && g.body[0].y == SN_H / 2, "wrap: the head comes out on the other side (x=%d)", g.body[0].x);
    sg_turn(&g, DIR_UP);
    for (int i = 0; i < SN_H / 2 + 1; i++) sg_step(&g);
    CHECK(g.status == SS_PLAYING && g.body[0].y == SN_H - 1, "wrap works vertically too (y=%d)", g.body[0].y);
}

static void test_eating(void) {
    SnakeGame g;
    sg_new(&g, false, 1, 11);
    put_food_ahead(&g);
    sg_step(&g);
    CHECK(g.len == 4 && g.grew && g.score == 10 && g.ev_eat == EAT_DANGO, "eating grows the snake and scores 10 on level 1");
    CHECK(g.food.x >= 0 && !sg_is_snake(&g, g.food.x, g.food.y), "new dango placed on a free cell");
    SPt t = g.body[g.len - 1];
    CHECK(t.x == g.old_tail.x && t.y == g.old_tail.y, "the tail stays put when growing");
    for (int i = 0; i < 4; i++) { put_food_ahead(&g); sg_step(&g); }
    CHECK(g.eaten == 5 && g.level == 2 && g.ev_level_up, "five dango make level 2 (eaten %d, level %d)", g.eaten, g.level);
    CHECK(g.score == 10 + 4 * 10 && g.len == 8, "score %ld len %d", g.score, g.len);
    int ms1 = 0, prev = 1000;
    sg_new(&g, false, 1, 1);
    ms1 = sg_step_ms(&g);
    for (int lv = 1; lv <= 14; lv++) {
        g.level = lv;
        int ms = sg_step_ms(&g);
        CHECK(ms <= prev && ms >= 60, "speed rises with the level (level %d: %d ms)", lv, ms);
        prev = ms;
    }
    CHECK(ms1 == 200, "level 1 steps every 200 ms");
}

static void test_collisions(void) {
    SnakeGame g;
    /* a curled snake runs into itself */
    sg_new(&g, false, 1, 5);
    g.len = 6;
    SPt curl[6] = { { 5, 5 }, { 6, 5 }, { 6, 6 }, { 5, 6 }, { 4, 6 }, { 4, 5 } };
    memcpy(g.body, curl, sizeof curl);
    g.dir = DIR_LEFT;
    g.food = (SPt){ 20, 15 };
    sg_turn(&g, DIR_DOWN);
    sg_step(&g);
    CHECK(g.status == SS_DEAD && g.death == DEATH_SELF, "biting the body kills");

    /* but the tail cell is free if the tail moves away */
    sg_new(&g, false, 1, 5);
    g.len = 4;
    SPt loop[4] = { { 6, 5 }, { 6, 6 }, { 5, 6 }, { 5, 5 } };
    memcpy(g.body, loop, sizeof loop);
    g.dir = DIR_LEFT;                                       /* the head steps into (5,5): the tail cell */
    g.food = (SPt){ 20, 15 };
    sg_step(&g);
    CHECK(g.status == SS_PLAYING && g.body[0].x == 5 && g.body[0].y == 5, "following one's own tail is allowed");
    CHECK(g.len == 4, "length unchanged after chasing the tail");

    /* ... unless the dango is eaten in the same step: the tail then stays */
    sg_new(&g, false, 1, 5);
    g.len = 4;
    memcpy(g.body, loop, sizeof loop);
    g.dir = DIR_LEFT;
    g.food = (SPt){ 5, 5 };                                 /* a dango on the tail cell cannot happen; but eating there must be fatal */
    sg_step(&g);
    CHECK(g.status == SS_DEAD && g.death == DEATH_SELF, "growing into the tail cell is fatal");
}

static void test_stones(void) {
    SnakeGame g;
    sg_new(&g, false, 3, 9);
    CHECK(g.n_obst == sg_level_stones(3) && g.n_obst == 4, "level 3 starts with four stone lanterns (%d)", g.n_obst);
    check_invariants(&g, "level 3", 9);
    sg_new(&g, false, 9, 9);
    CHECK(g.n_obst == sg_level_stones(9), "all layouts at level 9 (%d of %d)", g.n_obst, sg_level_stones(9));
    check_invariants(&g, "level 9", 9);
    CHECK(!sg_is_stone(&g, SN_W / 2, SN_H / 2) && !sg_is_stone(&g, SN_W / 2 + 3, SN_H / 2), "the start row is free of stones");
    for (int i = 0; i < SN_W; i++) CHECK(!sg_is_stone(&g, i, 0) && !sg_is_stone(&g, i, SN_H - 1), "border row stays free");

    /* hitting a stone kills */
    sg_new(&g, false, 1, 9);
    g.obst[sg_idx(g.body[0].x + 1, g.body[0].y)] = 1;
    sg_step(&g);
    CHECK(g.status == SS_DEAD && g.death == DEATH_STONE, "a stone lantern stops the snake");

    /* levelling up adds stones that avoid the snake */
    sg_new(&g, false, 2, 21);
    for (int i = 0; i < 5 && g.status == SS_PLAYING; i++) {
        put_food_ahead(&g);
        if (g.body[0].x + DX[g.dir] >= SN_W - 1) sg_turn(&g, DIR_UP);
        sg_step(&g);
    }
    CHECK(g.level == 3 || g.status == SS_DEAD, "five dango from level 2 give level 3 (level %d)", g.level);
    if (g.level == 3) CHECK(g.n_obst == 4, "the level-3 stones appear (%d)", g.n_obst);
    check_invariants(&g, "after level up", 21);
}

static void test_gold(void) {
    SnakeGame g;
    sg_new(&g, true, 2, 31);
    int found = 0;
    for (int i = 0; i < 20 && g.status == SS_PLAYING && !found; i++) {
        put_food_ahead(&g);
        sg_step(&g);
        if (g.ev_gold_spawn) found = 1;
    }
    CHECK(found && g.gold_ttl == SN_GOLD_STEPS, "a golden dango appears every seventh dango (eaten %d)", g.eaten);
    CHECK(!sg_is_snake(&g, g.gold.x, g.gold.y) && !(g.gold.x == g.food.x && g.gold.y == g.food.y), "gold on a free cell");
    /* eat it quickly: bonus */
    g.food = (SPt){ 0, 0 };
    g.gold = (SPt){ (int8_t)((g.body[0].x + DX[g.dir] + SN_W) % SN_W), (int8_t)((g.body[0].y + DY[g.dir] + SN_H) % SN_H) };
    long before = g.score;
    int ttl = g.gold_ttl;
    g.grew = false;
    sg_step(&g);
    if (g.status == SS_PLAYING) {
        CHECK(g.ev_eat == EAT_GOLD && g.score - before == 50L * g.level + (long)ttl * g.level, "golden dango bonus (%ld)", g.score - before);
        CHECK(g.gold_ttl == 0, "gold is gone after eating it");
    }
    /* it vanishes if ignored */
    sg_new(&g, true, 2, 31);
    g.gold = (SPt){ 1, 1 };
    g.gold_ttl = 3;
    g.food = (SPt){ 0, 0 };
    bool expired = false;
    for (int i = 0; i < 3; i++) { sg_step(&g); if (g.ev_gold_expire) expired = true; }
    CHECK(expired && g.gold_ttl == 0, "an ignored golden dango disappears");
}

/* A random-walk bot that avoids immediate death: checks the invariants for many seeds. */
static void test_random_play(void) {
    for (int seed = 1; seed <= 150; seed++) {
        SnakeGame g;
        bool wrap = seed & 1;
        sg_new(&g, wrap, 1 + seed % 9, (uint32_t)seed * 2654435761u);
        KRng r;
        kg_rng_seed(&r, (uint32_t)seed);
        for (int step = 0; step < 400 && g.status == SS_PLAYING; step++) {
            int dir = g.dir;
            if (kg_rng_range(&r, 4) == 0) dir = kg_rng_range(&r, 4);
            for (int tries = 0; tries < 4; tries++) {                     /* turn away from certain death */
                int nx = g.body[0].x + DX[dir], ny = g.body[0].y + DY[dir];
                if (wrap) { nx = (nx + SN_W) % SN_W; ny = (ny + SN_H) % SN_H; }
                bool bad = nx < 0 || ny < 0 || nx >= SN_W || ny >= SN_H || sg_is_stone(&g, nx, ny) || (sg_is_snake(&g, nx, ny) && !(nx == g.body[g.len - 1].x && ny == g.body[g.len - 1].y));
                if (!bad && dir != ((g.dir + 2) & 3)) break;
                dir = (dir + 1) & 3;
            }
            sg_turn(&g, dir);
            if (kg_rng_range(&r, 3) == 0) { /* head for the dango sometimes */
                if (g.food.x > g.body[0].x) sg_turn(&g, DIR_RIGHT);
                else if (g.food.x < g.body[0].x) sg_turn(&g, DIR_LEFT);
            }
            sg_step(&g);
            if (step % 50 == 0) check_invariants(&g, "random play", seed);
        }
        check_invariants(&g, "random play end", seed);
    }
}

/* The autopilot (used by --bot and the GUI tests) must keep the snake alive and fed on open and crowded fields. */
static void test_bot(void) {
    int alive = 0, runs = 0, eaten_total = 0;
    for (int seed = 1; seed <= 24; seed++) {
        SnakeGame g;
        sg_new(&g, seed & 1, 1 + seed % 9, (uint32_t)seed * 7919u);
        for (int step = 0; step < 1200 && g.status == SS_PLAYING; step++) {
            sg_turn(&g, sg_bot_dir(&g));
            sg_step(&g);
        }
        check_invariants(&g, "bot", seed);
        runs++;
        if (g.status == SS_PLAYING) alive++;
        eaten_total += g.eaten;
    }
    printf("  bot: %d of %d runs survived 1200 steps, %d dango eaten in total\n", alive, runs, eaten_total);
    CHECK(alive >= runs * 3 / 4, "the autopilot should survive most runs (%d of %d)", alive, runs);
    CHECK(eaten_total >= runs * 12, "and keep eating (%d dango in %d runs)", eaten_total, runs);
}

static void test_determinism(void) {
    SnakeGame a, b;
    sg_new(&a, false, 1, 99);
    sg_new(&b, false, 1, 99);
    bool same = true;
    for (int i = 0; i < 30; i++) {
        put_food_ahead(&a);
        b.food = a.food;
        sg_step(&a);
        sg_step(&b);
        if (a.food.x != b.food.x || a.food.y != b.food.y || a.status != b.status) same = false;
        if (a.status != SS_PLAYING) break;
        if (i % 6 == 5) { sg_turn(&a, DIR_UP); sg_turn(&b, DIR_UP); }
    }
    CHECK(same, "same seed, same game");
}

static void test_scores(void) {
    KScoreTable t;
    kg_scores_init(&t, false, 3);
    CHECK(kg_scores_add(&t, 0, 500, "Reimu", 1, 0) == 0, "first score");
    CHECK(kg_scores_add(&t, 0, 900, "Marisa", 3, 0) == 0, "higher score first");
    CHECK(kg_scores_add(&t, 0, 700, "Sakuya", 1, 0) == 1, "middle");
    CHECK(kg_scores_add(&t, 0, 100, "Low", 1, 0) == -1, "too low for a full table");
    CHECK(kg_scores_add(&t, 1, 100, "Wrap", 1, 0) == 0, "the other mode has its own table");
    const KScoreEntry *list[4];
    int n = kg_scores_list(&t, 0, list, 4);
    CHECK(n == 3 && list[0]->score == 900 && list[1]->score == 700 && list[2]->score == 500, "descending order");
    CHECK(kg_scores_rank(&t, 0, 800) == 1 && kg_scores_rank(&t, 0, 50) == -1, "ranks");
}

int sn_selftest(void) {
    printf("snake selftest\n");
    test_basics();
    test_walls();
    test_eating();
    test_collisions();
    test_stones();
    test_gold();
    test_random_play();
    test_bot();
    test_determinism();
    test_scores();
    if (g_failed) {
        printf("snake selftest: FAIL (%d of %d checks failed)\n", g_failed, g_checks);
        return 1;
    }
    printf("snake selftest: PASS (%d checks)\n", g_checks);
    return 0;
}
