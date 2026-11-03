/* danmaku --selftest: the fixed-point maths, the bullet behaviours, collisions, items, bosses and a full demo run of every stage,
 * all without a window. */
#include "dm.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_checks, g_failed;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_failed++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void fresh(int diff, int shot, int stage, uint32_t seed) {
    game_new(MODE_NORMAL, diff, shot, stage, seed);
    G.stage_t = 0;
    G.p.invuln = 0;
    G.p.x = FX(192);
    G.p.y = FX(400);
}

static void idle_tick(void) {
    Input in;
    memset(&in, 0, sizeof in);
    game_tick(&in);
}

static void test_math(void) {
    CHECK(fx_sin(0) == 0 && fx_sin(256) == 65536 && fx_sin(512) == 0 && fx_sin(768) == -65536, "sine at the quarter points");
    CHECK(fx_cos(0) == 65536 && fx_cos(256) == 0 && fx_cos(512) == -65536, "cosine at the quarter points");
    CHECK(fx_sin(-100) == -fx_sin(100) && fx_sin(100 + ANG) == fx_sin(100), "sine is odd and periodic");
    KRng r;
    kg_rng_seed(&r, 5);
    int worst = 0;
    for (int i = 0; i < 3000; i++) {
        fx dx = (fx)(kg_rng_next(&r) % 4000000) - 2000000, dy = (fx)(kg_rng_next(&r) % 4000000) - 2000000;
        if (iabs(dx) < 3000 && iabs(dy) < 3000) continue;
        int a = fx_atan2(dy, dx);
        double ref = atan2((double)dy, (double)dx) * (double)ANG / 6.283185307179586;
        if (ref < 0) ref += ANG;
        int d = iabs((int)(a - ref + 0.5));
        if (d > ANG / 2) d = ANG - d;
        if (d > worst) worst = d;
    }
    CHECK(worst <= 2, "atan2 within 2 units of the real thing (worst %d)", worst);
    CHECK(fx_atan2(0, FX(5)) == 0 && fx_atan2(FX(5), 0) == 256 && fx_atan2(0, -FX(5)) == 512 && fx_atan2(-FX(5), 0) == 768, "atan2 axes");
    CHECK(fx_dist(FX(3), FX(4)) == 5 && fx_dist(0, 0) == 0, "distance");
}

static void test_bullets(void) {
    fresh(DIFF_NORMAL, SHOT_A, 0, 1);
    G.p.invuln = 1000;
    bullets_reset();
    Bullet *b = bullet_add(FX(100), FX(100), 0, FX(2), BK_ORB, BC_RED);
    CHECK(b && G.nb == 1, "bullet added");
    for (int i = 0; i < 10; i++) bullets_update();
    CHECK(G.bl[0].x == FX(120) && G.bl[0].y == FX(100), "straight line: x=%d", FXI(G.bl[0].x));

    bullets_reset();
    b = bullet_add(FX(100), FX(100), ANG / 4, FX(1), BK_ORB, BC_RED);
    bullet_accel(b, ANG / 4, FXF(0.05f), 20);
    for (int i = 0; i < 30; i++) bullets_update();
    CHECK(G.bl[0].vy > FXF(1.95f) && G.bl[0].vy < FXF(2.05f), "acceleration stops after its ticks (vy=%d)", G.bl[0].vy);

    bullets_reset();
    b = bullet_add(FX(100), FX(100), 0, FX(0), BK_ORB, BC_RED);
    bullet_gravity(b, FXF(0.02f));
    for (int i = 0; i < 50; i++) bullets_update();
    CHECK(G.bl[0].vy > FXF(0.95f) && G.bl[0].vy < FXF(1.05f), "gravity keeps pulling (vy=%d)", G.bl[0].vy);

    bullets_reset();
    b = bullet_add(FX(100), FX(100), 0, FX(2), BK_RICE, BC_RED);
    bullet_curve(b, 4, 50);
    for (int i = 0; i < 25; i++) bullets_update();
    CHECK(G.nb == 1 && iabs((int)G.bl[0].ang - 100) <= 1, "a curving bullet turns 4 units per tick (angle %d)", G.bl[0].ang);
    int sp = kg_isqrt((G.bl[0].vx >> 8) * (G.bl[0].vx >> 8) + (G.bl[0].vy >> 8) * (G.bl[0].vy >> 8)) << 8;
    CHECK(iabs(sp - FX(2)) < 4000, "the speed is kept while curving (%d)", sp);

    bullets_reset();
    G.p.x = FX(300);
    G.p.y = FX(300);
    b = bullet_add(FX(100), FX(100), 0, FX(2), BK_ORB, BC_RED);
    bullet_stopgo(b, 20, FX(3), -1);
    for (int i = 0; i < 22; i++) bullets_update();
    CHECK(G.nb == 1 && iabs(G.bl[0].vx) < FXF(0.6f), "stop-and-go: it has slowed down");
    for (int i = 0; i < 40; i++) bullets_update();
    int want = aim_angle_to(FX(100), FX(100), FX(300), FX(300));
    CHECK(G.nb == 1 && iabs((int)G.bl[0].ang - want) <= 14 && G.bl[0].vx > FX(1), "stop-and-go: then heads for the player (angle %d, wanted %d)", G.bl[0].ang, want);

    bullets_reset();
    b = bullet_add(FX(100), FX(100), 0, FX(1), BK_BIG, BC_RED);
    bullet_split(b, 10, 12, BK_PELLET, BC_WHITE, FX(2));
    for (int i = 0; i < 12; i++) bullets_update();
    CHECK(G.nb == 12, "a splitting bullet bursts into its children (%d)", G.nb);

    bullets_reset();
    b = bullet_add(FX(100), FX(100), 0, FX(2), BK_ORB, BC_RED);
    bullet_delay(b, 15);
    for (int i = 0; i < 10; i++) bullets_update();
    CHECK(G.bl[0].x == FX(100), "a delayed bullet waits");
    for (int i = 0; i < 10; i++) bullets_update();
    CHECK(G.bl[0].x > FX(100), "and then goes");

    bullets_reset();
    b = bullet_add(FX(10), FX(100), ANG / 2, FX(3), BK_BILL, BC_RED);
    bullet_bounce(b, 1);
    int bounced = 0;
    for (int i = 0; i < 20; i++) { bullets_update(); if (G.nb && G.bl[0].vx > 0) bounced = 1; }
    CHECK(bounced && G.bl[0].x >= 0, "a bouncing bullet turns around at the wall");

    bullets_reset();
    for (int i = 0; i < 2000; i++) bullet_add(FX(50), FX(50), i, FX(1), BK_PELLET, BC_RED);
    CHECK(G.nb == MAX_BULLETS && bullet_add(FX(1), FX(1), 0, FX(1), BK_PELLET, BC_RED) == NULL, "the pool holds %d bullets and refuses more", MAX_BULLETS);
    bullets_reset();
    for (int i = 0; i < 100; i++) bullet_add(FX(50), FX(50), i, FX(1), BK_PELLET, BC_RED);
    for (int i = 0; i < 700; i++) bullets_update();
    CHECK(G.nb == 0, "bullets leave the field and are removed (%d left)", G.nb);

    bullets_reset();
    CHECK(pat_ring(FX(100), FX(100), 20, 0, FX(1), BK_PELLET, BC_RED) == 20 && G.nb == 20, "ring");
    CHECK(pat_fan(FX(100), FX(100), 5, 256, 100, FX(1), BK_PELLET, BC_RED) == 5 && G.nb == 25, "fan");
    CHECK(pat_flower(FX(100), FX(100), 40, 5, 0, FX(1), 128, BK_PELLET, BC_RED) == 40 && G.nb == 65, "flower");
    bullets_clear_circle(FX(100), FX(100), 20, false);
    CHECK(G.nb == 0, "clear circle");
}

static void test_player(void) {
    fresh(DIFF_NORMAL, SHOT_A, 0, 2);
    bullets_reset();
    G.p.invuln = 0;
    bullet_add(FX(192), FX(400), 0, FX(0), BK_ORB, BC_RED);
    bullets_update();
    CHECK(G.p.death_t > 0 && G.nb == 0, "a bullet on the player hits (death window %d)", G.p.death_t);
    fresh(DIFF_NORMAL, SHOT_A, 0, 2);
    G.p.invuln = 50;
    bullet_add(FX(192), FX(400), 0, FX(0), BK_ORB, BC_RED);
    bullets_update();
    CHECK(G.p.death_t == 0, "invulnerable players are not hit");
    fresh(DIFF_NORMAL, SHOT_A, 0, 2);
    bullets_reset();
    bullet_add(FX(192 + 10), FX(400), 0, FX(0), BK_ORB, BC_RED);
    bullets_update();
    bullets_update();
    CHECK(G.graze == 1 && G.p.death_t == 0 && G.score == 500, "grazing counts once (graze %d, score %ld)", G.graze, G.score);

    /* dying: the death window, a bomb inside it saves us */
    fresh(DIFF_NORMAL, SHOT_A, 0, 2);
    bullets_reset();
    bullet_add(FX(192), FX(400), 0, FX(0), BK_ORB, BC_RED);
    bullets_update();
    Input in;
    memset(&in, 0, sizeof in);
    in.bomb = true;
    game_tick(&in);
    CHECK(G.p.death_t == 0 && G.p.bombs == 2 && G.p.lives == 3 && G.p.bomb_t > 0, "a bomb inside the death window saves the player");
    fresh(DIFF_NORMAL, SHOT_A, 0, 2);
    bullets_reset();
    bullet_add(FX(192), FX(400), 0, FX(0), BK_ORB, BC_RED);
    bullets_update();
    for (int i = 0; i < 14; i++) idle_tick();
    CHECK(G.p.lives == 2 && G.p.respawn_t > 0 && G.p.invuln > 100, "without a bomb the player loses a life (lives %d)", G.p.lives);
    for (int i = 0; i < 60; i++) idle_tick();
    CHECK(G.p.respawn_t == 0 && G.p.y < FX(PFH) && G.p.invuln > 0, "and comes back");

    /* movement */
    fresh(DIFF_NORMAL, SHOT_A, 0, 2);
    memset(&in, 0, sizeof in);
    in.left = true;
    fx x0 = G.p.x;
    game_tick(&in);
    fx fast = x0 - G.p.x;
    in.focus = true;
    x0 = G.p.x;
    game_tick(&in);
    fx slow = x0 - G.p.x;
    CHECK(fast > slow && slow > 0, "focus slows the player (%d vs %d)", fast, slow);
    in.focus = false;
    for (int i = 0; i < 200; i++) game_tick(&in);
    CHECK(G.p.x >= FX(10), "the player stays inside the field");

    /* shooting and killing a fairy; items */
    fresh(DIFF_NORMAL, SHOT_A, 0, 3);
    G.p.power = 200;
    enemies_reset();
    Enemy *e = enemy_spawn(ET_FAIRY_BLUE, FX(192), FX(200), MV_DOWN, SH_NONE, 30, DROP_POWER);
    e->p[0] = 0;
    memset(&in, 0, sizeof in);
    in.shot = true;
    long before = G.score;
    for (int i = 0; i < 60; i++) game_tick(&in);
    CHECK(!e->active && G.score > before, "shots kill a fairy (score +%ld)", G.score - before);
    bool item = false;
    for (int i = 0; i < MAX_ITEMS; i++) if (G.it[i].active && G.it[i].type == IT_POWER) item = true;
    CHECK(item, "and it dropped a power item");
    int power0 = G.p.power;
    item_spawn(IT_POWER, G.p.x, G.p.y, 0);
    memset(&in, 0, sizeof in);
    idle_tick();
    CHECK(G.p.power == power0 + 5, "picking up power (%d -> %d)", power0, G.p.power);

    /* extends */
    fresh(DIFF_NORMAL, SHOT_A, 0, 4);
    int lives = G.p.lives;
    add_score(5000000);
    CHECK(G.p.lives == lives + 1 && G.extends_given == 1, "an extra life at five million");
    add_score(1000);
    CHECK(G.p.lives == lives + 1, "but only once");
}

static void test_boss(void) {
    fresh(DIFF_NORMAL, SHOT_A, 0, 5);
    G.god = true;
    boss_begin(0);
    for (int i = 0; i < 70; i++) idle_tick();
    CHECK(G.boss.phase == 0 && G.boss.hp == 2600, "the boss starts its first attack (phase %d, hp %d)", G.boss.phase, G.boss.hp);
    int hp = G.boss.hp;
    boss_damage(100);
    CHECK(G.boss.hp == hp || G.boss.invuln > 0, "the boss starts out invulnerable");
    for (int i = 0; i < 60; i++) idle_tick();
    CHECK(G.boss.invuln == 0, "then it can be hurt");
    hp = G.boss.hp;
    int done = boss_damage(100);
    CHECK(done == 100 && G.boss.hp == hp - 100, "damage lands");
    G.boss.hp = 1;
    boss_damage(5);
    idle_tick();
    CHECK(G.boss.phase == 1 && G.boss.spell && G.nb == 0, "the next phase is a spell card with a clean field (phase %d)", G.boss.phase);
    for (int i = 0; i < 130; i++) idle_tick();
    CHECK(G.boss.invuln == 0 && G.nb > 0, "the card is under way (%d bullets)", G.nb);
    long before = G.score;
    G.boss.hp = 1;
    boss_damage(5);
    idle_tick();
    CHECK(G.stat_captures == 1 && G.score - before > 1000000 && G.last_bonus_ok, "capturing a spell card pays a bonus (%ld)", G.score - before);
    for (int i = 0; i < 130; i++) idle_tick();
    G.boss.failed = true;                                       /* a bomb or a death in the card */
    G.boss.hp = 1;
    boss_damage(5);
    idle_tick();
    CHECK(G.stat_captures == 1 && !G.last_bonus_ok, "no bonus when the card was failed");
    CHECK(G.boss.phase >= 3 || G.boss.dying, "the last phase is over (phase %d)", G.boss.phase);
    for (int i = 0; i < 300 && G.state == GS_PLAY; i++) idle_tick();
    CHECK(G.state == GS_STAGE_CLEAR && !G.boss.active, "defeating the boss clears the stage");

    /* time out */
    fresh(DIFF_EASY, SHOT_B, 1, 6);
    G.god = true;
    boss_begin(1);
    for (int i = 0; i < 70; i++) idle_tick();
    int t0 = G.boss.timer;
    for (int i = 0; i < t0 + 5; i++) idle_tick();
    CHECK(G.boss.phase == 1, "a plain attack ends when its time is up (phase %d)", G.boss.phase);
}

static unsigned long hash_state(void) {
    unsigned long h = 1469598103934665603UL;
    long v[] = { G.score, G.tick, G.nb, G.p.x, G.p.y, G.graze, G.boss.hp, (long)G.rng.state, G.p.power, G.stat_captures };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) { h ^= (unsigned long)v[i]; h *= 1099511628211UL; }
    return h;
}

static unsigned long demo_run(int diff, int shot, int stage, int ticks, uint32_t seed) {
    game_new(MODE_NORMAL, diff, shot, stage, seed);
    G.god = true;
    for (int i = 0; i < ticks && G.state == GS_PLAY; i++) {
        Input in;
        bot_input(&in);
        game_tick(&in);
    }
    return hash_state();
}

static void test_determinism(void) {
    unsigned long a = demo_run(DIFF_HARD, SHOT_B, 0, 2500, 77), b = demo_run(DIFF_HARD, SHOT_B, 0, 2500, 77), c = demo_run(DIFF_HARD, SHOT_B, 0, 2500, 78);
    CHECK(a == b, "the same seed gives the same run");
    CHECK(a != c, "another seed gives another run");
}

static void test_stages(void) {
    for (int stage = 0; stage < N_STAGES; stage++) {
        for (int diff = DIFF_EASY; diff <= DIFF_LUNATIC; diff += (stage == 0 ? 1 : 3)) {
            game_new(MODE_NORMAL, diff, (stage + diff) & 1, stage, 100 + (uint32_t)stage * 7 + (uint32_t)diff);
            G.god = true;
            G.trace = stage == 0 && diff == DIFF_NORMAL;
            long limit = 30000;
            long t = 0;
            int phases_seen = 0, last_phase = -2, power_at_boss = -1;
            while (G.state == GS_PLAY && t < limit) {
                Input in;
                bot_input(&in);
                game_tick(&in);
                t++;
                if (G.trace && G.boss.active && G.boss.phase == 1 && t % 200 == 0)
                    printf("  [t=%ld] bot at %d,%d (focus %d, in L%d R%d U%d D%d) boss at %d,%d hp %d bullets %d shots %d\n", t, FXI(G.p.x), FXI(G.p.y), in.focus, in.left, in.right,
                           in.up, in.down, FXI(G.boss.x), FXI(G.boss.y), G.boss.hp, G.nb, in.shot);
                if (G.boss.active && G.boss.phase != last_phase) { last_phase = G.boss.phase; phases_seen++; if (power_at_boss < 0) power_at_boss = G.p.power; }
            }
            printf("  stage %d %-7s: %s after %ld ticks, score %ld, peak bullets %d, %d boss phases, %d cards captured, power %d, %s\n", stage + 1,
                   diff_name(diff), G.state == GS_STAGE_CLEAR ? "cleared" : "NOT CLEARED", t, G.score, G.bullets_peak, phases_seen, G.stat_captures,
                   power_at_boss, shot_name(G.shot));
            CHECK(G.state == GS_STAGE_CLEAR, "stage %d on %s must end with a defeated boss", stage + 1, diff_name(diff));
            CHECK(G.bullets_peak <= MAX_BULLETS, "bullet pool never overflows");
            CHECK(G.bullets_peak <= 1500, "the patterns stay under 1500 bullets (peak %d)", G.bullets_peak);
        }
    }
}

static void test_scores(void) {
    KScoreTable t;
    kg_scores_init(&t, false, 10);
    for (int i = 0; i < 12; i++) kg_scores_add(&t, DIFF_NORMAL, 1000000L * (i + 1), "Test", SHOT_A, 2);
    const KScoreEntry *l[16];
    int n = kg_scores_list(&t, DIFF_NORMAL, l, 16);
    CHECK(n == 10 && l[0]->score == 12000000 && l[9]->score == 3000000, "the table keeps the best ten");
    char buf[2048];
    kg_scores_format(&t, buf, sizeof buf);
    KScoreTable u;
    kg_scores_init(&u, false, 10);
    kg_scores_parse(&u, buf);
    CHECK(u.n == 10 && u.e[0].score == 12000000, "text round trip");
}

int dm_selftest(void) {
    printf("danmaku selftest\n");
    fxm_init();
    test_math();
    test_bullets();
    test_player();
    test_boss();
    test_determinism();
    test_scores();
    test_stages();
    if (g_failed) {
        printf("danmaku selftest: FAIL (%d of %d checks failed)\n", g_failed, g_checks);
        return 1;
    }
    printf("danmaku selftest: PASS (%d checks)\n", g_checks);
    return 0;
}
