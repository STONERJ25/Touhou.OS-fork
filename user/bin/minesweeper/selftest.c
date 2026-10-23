/* minesweeper --selftest: exercises the board logic and the score table without opening a window. */
#include "board.h"
#include <stdio.h>
#include <string.h>

static int g_checks, g_failed;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_failed++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static int brute_adjacent(const Board *b, int x, int y) {
    int n = 0;
    for (int yy = y - 1; yy <= y + 1; yy++)
        for (int xx = x - 1; xx <= x + 1; xx++)
            if ((xx != x || yy != y) && xx >= 0 && yy >= 0 && xx < b->w && yy < b->h && b->mine[yy * b->w + xx]) n++;
    return n;
}

static void test_placement(void) {
    static const int levels[3][3] = { { 9, 9, 10 }, { 16, 16, 40 }, { 30, 16, 99 } };
    for (int l = 0; l < 3; l++) {
        for (uint32_t seed = 1; seed <= 100; seed++) {
            Board b;
            board_new(&b, levels[l][0], levels[l][1], levels[l][2], seed * 7919u + (uint32_t)l);
            KRng r;
            kg_rng_seed(&r, seed);
            int fx = kg_rng_range(&r, b.w), fy = kg_rng_range(&r, b.h);
            int opened = board_reveal(&b, fx, fy);
            CHECK(opened > 0, "first click must open something (level %d seed %u)", l, seed);
            CHECK(b.mine[fy * b.w + fx] == 0, "the first click hit a fairy (level %d seed %u)", l, seed);
            CHECK(b.status != ST_LOST, "lost on the first click");
            CHECK(board_count_mines(&b) == levels[l][2], "mine count %d != %d", board_count_mines(&b), levels[l][2]);
            bool ring_ok = true, adj_ok = true;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    if (board_in(&b, fx + dx, fy + dy) && b.mine[(fy + dy) * b.w + fx + dx]) ring_ok = false;
            CHECK(ring_ok, "the neighbours of the first click should be empty too (level %d seed %u)", l, seed);
            for (int y = 0; y < b.h && adj_ok; y++)
                for (int x = 0; x < b.w; x++)
                    if (b.adj[y * b.w + x] != brute_adjacent(&b, x, y)) { adj_ok = false; break; }
            CHECK(adj_ok, "adjacent counts differ from brute force (level %d seed %u)", l, seed);
        }
    }
    /* a crowded field only protects the clicked cell itself */
    Board b;
    board_new(&b, 9, 9, 80, 5);
    CHECK(board_reveal(&b, 0, 0) >= 1 && b.mine[0] == 0, "crowded first click");
    CHECK(board_count_mines(&b) == 80, "crowded field keeps all mines");
    /* determinism */
    Board c, d;
    board_new(&c, 16, 16, 40, 1234);
    board_new(&d, 16, 16, 40, 1234);
    board_reveal(&c, 8, 8);
    board_reveal(&d, 8, 8);
    CHECK(memcmp(c.mine, d.mine, sizeof c.mine) == 0, "same seed, same layout");
    board_new(&d, 16, 16, 40, 1235);
    board_reveal(&d, 8, 8);
    CHECK(memcmp(c.mine, d.mine, sizeof c.mine) != 0, "different seed, different layout");
}

/* 6x5 test field:      fairies at (1,1) (4,1) (2,3)
 *      0 1 2 3 4 5
 *   0  . . . . . .
 *   1  . F . . F .
 *   2  . . . . . .
 *   3  . . F . . .
 *   4  . . . . . .
 */
static void make_field(Board *b) {
    board_new(b, 6, 5, 3, 1);
    uint8_t map[30] = { 0 };
    map[1 * 6 + 1] = map[1 * 6 + 4] = map[3 * 6 + 2] = 1;
    board_place_for_test(b, map);
    b->status = ST_PLAYING;
}

static void test_logic(void) {
    Board b;
    make_field(&b);
    CHECK(b.adj[0] == 1 && b.adj[1 * 6 + 2] == 1 && b.adj[2 * 6 + 2] == 2 && b.adj[5] == 1 && b.adj[4 * 6 + 5] == 0, "adjacent numbers of the test field");

    /* flood fill from the empty bottom-right corner */
    int n = board_reveal(&b, 5, 4);
    CHECK(n > 0 && b.cell[4 * 6 + 5] == CELL_OPEN, "flood fill opens the corner");
    CHECK(b.cell[1 * 6 + 1] != CELL_OPEN && b.cell[1 * 6 + 4] != CELL_OPEN, "flood fill never opens a fairy");
    int open_count = 0;
    for (int i = 0; i < 30; i++) open_count += b.cell[i] == CELL_OPEN;
    CHECK(open_count == b.opened, "opened counter (%d vs %d)", open_count, b.opened);
    /* the flood must stop at numbers: a cell with adj > 0 that was opened must have an opened zero neighbour */
    for (int y = 0; y < 5; y++)
        for (int x = 0; x < 6; x++) {
            int i = y * 6 + x;
            if (b.cell[i] != CELL_OPEN || b.adj[i] == 0) continue;
            bool zero_nb = false;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    if (board_in(&b, x + dx, y + dy) && b.cell[(y + dy) * 6 + x + dx] == CELL_OPEN && b.adj[(y + dy) * 6 + x + dx] == 0) zero_nb = true;
            CHECK(zero_nb, "numbered cell (%d,%d) was opened without an opened empty neighbour", x, y);
        }

    /* flags */
    make_field(&b);
    board_flag(&b, 1, 1);
    CHECK(b.cell[1 * 6 + 1] == CELL_FLAG && b.flags == 1, "flag placed");
    CHECK(board_reveal(&b, 1, 1) == 0 && b.status == ST_PLAYING, "a flagged cell cannot be uncovered");
    board_flag(&b, 1, 1);
    CHECK(b.cell[1 * 6 + 1] == CELL_COVERED && b.flags == 0, "flag removed");
    board_reveal(&b, 0, 0);
    board_flag(&b, 0, 0);
    CHECK(b.cell[0] == CELL_OPEN && b.flags == 0, "an open cell cannot be flagged");

    /* chord with the right flags */
    make_field(&b);
    board_reveal(&b, 0, 0);                                 /* a 1 next to the fairy at (1,1) */
    CHECK(b.cell[0] == CELL_OPEN && b.adj[0] == 1, "setup for chord");
    CHECK(board_chord(&b, 0, 0) == 0, "chord without flags does nothing");
    board_flag(&b, 1, 1);
    int c = board_chord(&b, 0, 0);
    CHECK(c >= 0 && b.cell[1] == CELL_OPEN && b.cell[6] == CELL_OPEN && b.status != ST_LOST, "chord opens the neighbours (%d)", c);

    /* chord with a wrong flag loses */
    make_field(&b);
    board_reveal(&b, 0, 0);
    board_flag(&b, 0, 1);                                   /* wrong cell */
    CHECK(board_chord(&b, 0, 0) == -1 && b.status == ST_LOST, "wrong flag + chord = lost");
    CHECK(b.hit_x == 1 && b.hit_y == 1, "the uncovered fairy is remembered (%d,%d)", b.hit_x, b.hit_y);

    /* loss */
    make_field(&b);
    CHECK(board_reveal(&b, 4, 1) == -1 && b.status == ST_LOST && b.hit_x == 4 && b.hit_y == 1, "uncovering a fairy loses");
    CHECK(board_reveal(&b, 0, 0) == 0, "no moves after losing");

    /* win */
    make_field(&b);
    for (int y = 0; y < 5; y++)
        for (int x = 0; x < 6; x++)
            if (!b.mine[y * 6 + x]) board_reveal(&b, x, y);
    CHECK(b.status == ST_WON, "uncovering every safe cell wins (status %d)", b.status);
    CHECK(b.flags == 3 && b.cell[1 * 6 + 1] == CELL_FLAG && b.cell[3 * 6 + 2] == CELL_FLAG, "a win flags the remaining fairies");
    CHECK(board_reveal(&b, 1, 1) == 0, "no moves after winning");

    /* clamping of custom fields */
    int w = 3, h = 100, m = 5000;
    board_clamp(&w, &h, &m);
    CHECK(w == 9 && h == MS_MAX_H && m == (w - 1) * (h - 1), "clamp custom field (%d x %d, %d)", w, h, m);
    w = 30; h = 16; m = 0;
    board_clamp(&w, &h, &m);
    CHECK(w == 30 && h == 16 && m == 1, "clamp low mine count");
}

static void test_scores(void) {
    KScoreTable t;
    kg_scores_init(&t, true, 3);
    CHECK(kg_scores_rank(&t, 0, 50000) == 0, "empty table: first place");
    CHECK(kg_scores_add(&t, 0, 50000, "Reimu", 0, 0) == 0, "add first");
    CHECK(kg_scores_add(&t, 0, 30000, "Marisa", 0, 0) == 0, "faster time takes first place");
    CHECK(kg_scores_add(&t, 0, 40000, "Sakuya", 0, 0) == 1, "middle place");
    CHECK(kg_scores_add(&t, 0, 90000, "Slow", 0, 0) == -1, "slower than a full table is rejected");
    CHECK(kg_scores_add(&t, 0, 10000, "Quick", 0, 0) == 0 && t.n == 3, "a new best pushes out the last place");
    CHECK(kg_scores_add(&t, 1, 99000, "Other", 0, 0) == 0, "categories are independent");
    const KScoreEntry *list[8];
    int n = kg_scores_list(&t, 0, list, 8);
    CHECK(n == 3 && list[0]->score == 10000 && list[1]->score == 30000 && list[2]->score == 40000, "sorted ascending");
    CHECK(kg_scores_best(&t, 0, -1) == 10000 && kg_scores_best(&t, 2, -1) == -1, "best per category");
    char buf[1024];
    kg_scores_format(&t, buf, sizeof buf);
    KScoreTable u;
    kg_scores_init(&u, true, 3);
    kg_scores_parse(&u, buf);
    CHECK(u.n == t.n && u.e[0].score == 10000 && strcmp(u.e[0].name, "Quick") == 0, "text round trip");
    kg_scores_parse(&u, "garbage\n1 2\n0 123 0 0 Fine Name\n");
    CHECK(u.n == 1 && u.e[0].score == 123 && strcmp(u.e[0].name, "Fine Name") == 0, "malformed lines are skipped, names keep spaces");
}

int ms_selftest(void) {
    printf("minesweeper selftest\n");
    test_placement();
    test_logic();
    test_scores();
    if (g_failed) {
        printf("minesweeper selftest: FAIL (%d of %d checks failed)\n", g_failed, g_checks);
        return 1;
    }
    printf("minesweeper selftest: PASS (%d checks)\n", g_checks);
    return 0;
}
