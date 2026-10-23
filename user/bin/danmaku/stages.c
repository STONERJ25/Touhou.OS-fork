/* The three stages: scripted waves of fairies, then the bosses with their attacks and spell cards. All names and patterns are original. */
#include "dm.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- the wave script */

enum { EV_ENEMY, EV_BOSS };

typedef struct Ev {
    int t;
    uint8_t kind, type, move, shoot, drop, mind;
    int16_t x, y;
    int16_t p[4], s[2];
    int hp;
} Ev;

#define MAX_EV 520
static Ev g_ev[MAX_EV];
static int g_nev, g_next;
static long g_sclock;                         /* the script clock: it stops while a boss is on the field */

static void add_ev(int t, int type, int x, int y, int move, int shoot, int hp, int drop, int p0, int p1, int p2, int p3, int s0, int s1, int mind) {
    if (g_nev >= MAX_EV) return;
    Ev *e = &g_ev[g_nev++];
    e->t = t; e->kind = EV_ENEMY; e->type = (uint8_t)type; e->x = (int16_t)x; e->y = (int16_t)y;
    e->move = (uint8_t)move; e->shoot = (uint8_t)shoot; e->hp = hp; e->drop = (uint8_t)drop; e->mind = (uint8_t)mind;
    e->p[0] = (int16_t)p0; e->p[1] = (int16_t)p1; e->p[2] = (int16_t)p2; e->p[3] = (int16_t)p3;
    e->s[0] = (int16_t)s0; e->s[1] = (int16_t)s1;
}

static void add_boss(int t, int id) {
    if (g_nev >= MAX_EV) return;
    Ev *e = &g_ev[g_nev++];
    memset(e, 0, sizeof *e);
    e->t = t;
    e->kind = EV_BOSS;
    e->type = (uint8_t)id;
}

#define EV(t, ty, x, y, mv, sh, hp, dr, p0, p1, p2, p3, s0, s1) add_ev(t, ty, x, y, mv, sh, hp, dr, p0, p1, p2, p3, s0, s1, 0)
#define EVD(md, t, ty, x, y, mv, sh, hp, dr, p0, p1, p2, p3, s0, s1) add_ev(t, ty, x, y, mv, sh, hp, dr, p0, p1, p2, p3, s0, s1, md)

static void sort_events(void) {
    for (int i = 1; i < g_nev; i++) {                         /* insertion sort: the lists are written in order, so this is quick */
        Ev tmp = g_ev[i];
        int j = i - 1;
        while (j >= 0 && g_ev[j].t > tmp.t) { g_ev[j + 1] = g_ev[j]; j--; }
        g_ev[j + 1] = tmp;
    }
}

/* Stage 1: the cloud road. Blue and red fairies, a maid with a spiral, rain. */
static void build_stage1(void) {
    for (int i = 0; i < 6; i++) EV(120 + i * 22, ET_FAIRY_BLUE, 40, -20, MV_DOWN, SH_AIM, 6, i == 5 ? DROP_POWER : DROP_POINT, 16, 8, 0, 0, 100, BC_RED);
    for (int i = 0; i < 6; i++) EV(420 + i * 22, ET_FAIRY_BLUE, 344, -20, MV_DOWN, SH_AIM, 6, i == 5 ? DROP_POWER : DROP_POINT, 16, -8, 0, 0, 100, BC_ORANGE);
    for (int i = 0; i < 8; i++) EV(740 + i * 20, ET_FAIRY_GREEN, 192, -20, MV_SINE, SH_AIM, 6, i % 3 == 2 ? DROP_POINT : DROP_NONE, 16, 90, 11, 0, 90, BC_PINK);
    EV(1120, ET_FAIRY_RED, 96, -20, MV_STOP, SH_RING, 24, DROP_POWER, 22, 90, 220, 0, 70, BC_RED);
    EV(1120, ET_FAIRY_RED, 288, -20, MV_STOP, SH_RING, 24, DROP_POINT, 22, 90, 220, 512, 70, BC_ORANGE);
    for (int i = 0; i < 5; i++) {
        EV(1500 + i * 30, ET_FAIRY_YELLOW, -20, 60, MV_SWOOP, SH_AIM, 6, DROP_NONE, 26, 1, 170, 150, 80, BC_YELLOW);
        EV(1520 + i * 30, ET_FAIRY_YELLOW, 404, 60, MV_SWOOP, SH_AIM, 6, DROP_NONE, 26, -1, 170, 150, 80, BC_WHITE);
    }
    EV(1980, ET_MAID, 192, -30, MV_STOP, SH_SPIN, 150, DROP_BIG, 14, 110, 420, 768, 0, BC_PINK);
    for (int i = 0; i < 4; i++) EV(2120 + i * 30, ET_FAIRY_BLUE, 60 + i * 90, -20, MV_DOWN, SH_AIM, 6, DROP_POINT, 18, 0, 0, 0, 80, BC_CYAN);
    for (int i = 0; i < 12; i++) EV(2640 + i * 14, ET_FAIRY_GREEN, 30 + i * 30, -20, MV_DOWN, SH_RAIN, 5, i % 4 == 3 ? DROP_POINT : DROP_NONE, 20, 0, 0, 0, 0, BC_CYAN);
    for (int i = 0; i < 4; i++) {
        EV(3020 + i * 30, ET_FAIRY_RED, -20, 40, MV_ARC, SH_AIM3, 12, i == 3 ? DROP_BOMB : DROP_POINT, 26, 40, 12, 0, 110, BC_RED);
        EV(3040 + i * 30, ET_FAIRY_RED, 404, 40, MV_ARC, SH_AIM3, 12, DROP_POINT, 26, 472, -12, 0, 110, BC_ORANGE);
    }
    EV(3400, ET_MAID, 100, -30, MV_STOP, SH_FLOWER, 130, DROP_BIG, 14, 100, 380, 768, 90, BC_PINK);
    EV(3400, ET_MAID, 284, -30, MV_STOP, SH_RING, 130, DROP_BIG, 14, 100, 380, 768, 60, BC_YELLOW);
    EVD(DIFF_HARD, 3520, ET_FAIRY_BLUE, 192, -20, MV_SINE, SH_AIM, 6, DROP_NONE, 18, 120, 9, 0, 60, BC_WHITE);
    add_boss(4000, 0);
}

/* Stage 2: the bamboo hollow. Wisps drift, orbs spin, two bamboo spirits. */
static void build_stage2(void) {
    for (int i = 0; i < 7; i++) EV(100 + i * 24, ET_FAIRY_GREEN, 340, -20, MV_SINE, SH_AIM, 7, i == 6 ? DROP_POWER : DROP_POINT, 17, 70, 10, 0, 90, BC_GREEN);
    for (int i = 0; i < 7; i++) EV(300 + i * 24, ET_FAIRY_YELLOW, 44, -20, MV_SINE, SH_AIM, 7, i == 6 ? DROP_POWER : DROP_POINT, 17, 70, 10, 512, 90, BC_YELLOW);
    for (int i = 0; i < 4; i++) EV(620 + i * 40, ET_WISP, 70 + i * 80, -20, MV_DRIFT, SH_RING, 14, i == 3 ? DROP_BIG : DROP_NONE, 12, 30, 9, 0, 110, BC_CYAN);
    EV(980, ET_ORB, 192, -20, MV_STOP, SH_SPIN, 70, DROP_POWER, 20, 100, 360, 768, 0, BC_GREEN);
    for (int i = 0; i < 6; i++) {
        EV(1200 + i * 26, ET_FAIRY_BLUE, -20, 40, MV_ARC, SH_AIM3, 8, DROP_POINT, 28, 50, 14, 0, 100, BC_CYAN);
        EV(1210 + i * 26, ET_FAIRY_BLUE, 404, 40, MV_ARC, SH_AIM3, 8, DROP_POINT, 28, 462, -14, 0, 100, BC_WHITE);
    }
    for (int i = 0; i < 10; i++) EV(1620 + i * 16, ET_FAIRY_GREEN, 24 + i * 38, -20, MV_DOWN, SH_RAIN, 5, i % 3 == 2 ? DROP_POINT : DROP_NONE, 22, 0, 0, 0, 0, BC_GREEN);
    EV(1900, ET_MAID, 192, -30, MV_STOP, SH_FLOWER, 150, DROP_BIG, 14, 110, 420, 768, 80, BC_GREEN);
    EV(1980, ET_WISP, 60, -20, MV_DRIFT, SH_RING, 14, DROP_POINT, 12, 40, 8, 0, 100, BC_YELLOW);
    EV(1980, ET_WISP, 324, -20, MV_DRIFT, SH_RING, 14, DROP_POINT, 12, 40, 8, 0, 100, BC_YELLOW);
    add_boss(2500, 1);
    for (int i = 0; i < 8; i++) EV(2640 + i * 24, ET_FAIRY_RED, 40 + (i % 2) * 304, -20, MV_SINE, SH_BURST, 7, DROP_POINT, 18, 60, 12, 0, 150, BC_RED);
    for (int i = 0; i < 3; i++) EV(2900 + i * 50, ET_ORB, 80 + i * 112, -20, MV_STOP, SH_SPIN, 60, i == 1 ? DROP_BOMB : DROP_POWER, 20, 90 + (i % 2) * 40, 300, 768, 0, i % 2 ? BC_GREEN : BC_CYAN);
    for (int i = 0; i < 12; i++) {
        EV(3300 + i * 20, ET_FAIRY_YELLOW, 192, -20, MV_SINE, SH_AIM, 6, i % 3 == 0 ? DROP_POINT : DROP_NONE, 18, 120, 11, i * 20, 70, BC_YELLOW);
    }
    EV(3700, ET_MAID, 110, -30, MV_STOP, SH_RING, 140, DROP_BIG, 14, 100, 380, 768, 55, BC_GREEN);
    EV(3700, ET_MAID, 274, -30, MV_STOP, SH_AIM3, 140, DROP_POWER, 14, 100, 380, 768, 70, BC_CYAN);
    EVD(DIFF_NORMAL, 3860, ET_WISP, 192, -20, MV_DRIFT, SH_RING, 14, DROP_POINT, 12, 60, 7, 0, 90, BC_WHITE);
    add_boss(4400, 2);
}

/* Stage 3: the lantern lake. Everything is faster and shoots more. */
static void build_stage3(void) {
    for (int i = 0; i < 8; i++) EV(100 + i * 18, ET_FAIRY_BLUE, 30 + i * 46, -20, MV_DOWN, SH_AIM, 7, i % 3 == 2 ? DROP_POINT : DROP_NONE, 20, i % 2 ? 4 : -4, 0, 0, 80, BC_BLUE);
    for (int i = 0; i < 6; i++) {
        EV(420 + i * 28, ET_FAIRY_RED, -20, 70, MV_SWOOP, SH_AIM3, 8, DROP_POINT, 28, 1, 200, 140, 100, BC_PURPLE);
        EV(434 + i * 28, ET_FAIRY_RED, 404, 70, MV_SWOOP, SH_AIM3, 8, DROP_POINT, 28, -1, 200, 140, 100, BC_PINK);
    }
    EV(860, ET_MAID, 192, -30, MV_STOP, SH_SPIN, 170, DROP_BIG, 14, 100, 440, 768, 0, BC_BLUE);
    for (int i = 0; i < 4; i++) EV(900 + i * 30, ET_WISP, 60 + i * 88, -20, MV_DRIFT, SH_RING, 14, i % 2 ? DROP_POINT : DROP_NONE, 14, 36, 9, 0, 90, BC_WHITE);
    for (int i = 0; i < 14; i++) EV(1400 + i * 14, ET_FAIRY_YELLOW, 20 + i * 26, -20, MV_DOWN, SH_RAIN, 5, i % 4 == 3 ? DROP_POINT : DROP_NONE, 24, 0, 0, 0, 0, BC_BLUE);
    EV(1780, ET_ORB, 96, -20, MV_STOP, SH_SPIN, 80, DROP_POWER, 20, 110, 360, 768, 0, BC_PURPLE);
    EV(1780, ET_ORB, 288, -20, MV_STOP, SH_SPIN, 80, DROP_POWER, 20, 110, 360, 768, 0, BC_PINK);
    for (int i = 0; i < 6; i++) EV(2100 + i * 24, ET_FAIRY_GREEN, 192, -20, MV_SINE, SH_BURST, 7, DROP_POINT, 20, 150, 10, i * 30, 150, BC_YELLOW);
    EV(2380, ET_MAID, 192, -30, MV_STOP, SH_FLOWER, 160, DROP_BOMB, 14, 100, 300, 768, 80, BC_PINK);
    add_boss(2800, 3);
    for (int i = 0; i < 6; i++) {
        EV(2940 + i * 26, ET_FAIRY_BLUE, -20, 40, MV_ARC, SH_AIM3, 9, DROP_POINT, 30, 50, 15, 0, 90, BC_BLUE);
        EV(2950 + i * 26, ET_FAIRY_BLUE, 404, 40, MV_ARC, SH_AIM3, 9, DROP_POINT, 30, 462, -15, 0, 90, BC_CYAN);
    }
    for (int i = 0; i < 5; i++) EV(3300 + i * 36, ET_WISP, 40 + i * 76, -20, MV_DRIFT, SH_RING, 16, i == 2 ? DROP_LIFE : DROP_POINT, 15, 40, 8, 0, 80, BC_CYAN);
    EV(3600, ET_MAID, 90, -30, MV_STOP, SH_RING, 150, DROP_BIG, 14, 100, 380, 768, 50, BC_PURPLE);
    EV(3600, ET_MAID, 294, -30, MV_STOP, SH_RING, 150, DROP_BIG, 14, 100, 380, 768, 50, BC_BLUE);
    for (int i = 0; i < 10; i++) EV(3800 + i * 18, ET_FAIRY_RED, 40 + (i % 2) * 304, -20, MV_SINE, SH_BURST, 7, DROP_POINT, 20, 80, 12, 0, 140, BC_RED);
    add_boss(4700, 4);
}

void stage_begin(int stage) {
    g_nev = g_next = 0;
    g_sclock = 0;
    switch (stage) {
    case 0: build_stage1(); break;
    case 1: build_stage2(); break;
    default: build_stage3(); break;
    }
    sort_events();
}

void stage_skip_to_boss(bool midboss) {
    int pick = -1;
    for (int i = 0; i < g_nev; i++)
        if (g_ev[i].kind == EV_BOSS) {
            if (midboss) { if (BOSSES[g_ev[i].type].midboss) { pick = i; break; } }
            else pick = i;                                             /* the last boss event is the end boss */
        }
    if (pick < 0) return;
    g_next = pick;
    g_sclock = g_ev[pick].t - 1;
}

void stage_run(long t) {
    (void)t;
    if (G.boss.active) return;                                  /* the script waits while a boss is on the field */
    g_sclock++;
    while (g_next < g_nev && g_ev[g_next].t <= g_sclock) {
        Ev *e = &g_ev[g_next++];
        if (e->kind == EV_BOSS) { boss_begin(e->type); return; }
        if (G.diff < e->mind) continue;
        Enemy *en = enemy_spawn(e->type, FX(e->x), FX(e->y), e->move, e->shoot, e->hp, e->drop);
        if (!en) continue;
        for (int i = 0; i < 4; i++) en->p[i] = e->p[i];
        for (int i = 0; i < 2; i++) en->s[i] = e->s[i];
    }
}

static const char *const STAGE_NAMES[N_STAGES] = { "Starlit Cloud Road", "Whispering Bamboo Hollow", "Lake of a Thousand Lanterns" };
static const char *const STAGE_SUBS[N_STAGES] = { "The sky is wide awake tonight", "Something small lives among the stalks", "Midnight keeps its own lights" };
const char *stage_name(int s) { return STAGE_NAMES[s < 0 || s >= N_STAGES ? 0 : s]; }
const char *stage_subtitle(int s) { return STAGE_SUBS[s < 0 || s >= N_STAGES ? 0 : s]; }

/* ---------------------------------------------------------------- boss attacks */

static int s_beam_angle, s_beam_angle2;                         /* a beam keeps its direction for its whole duration */

static void boss_wander(Boss *b, int t, int period, int y) {
    static const int xs[4] = { 110, 274, 192, 240 };
    if (t % period == 0) boss_goto(b, FX(xs[(t / period) % 4]), FX(y + ((t / period) % 2) * 30));
}

/* ---- Asahi, the dawn fairy (stage 1) */

static void asahi_1(Boss *b, int t) {
    boss_wander(b, t, 160, 90);
    if (t > 40 && t % dsel(90, 60, 50, 40) == 0) pat_aimed(b->x, b->y, dsel(3, 5, 7, 9), 90, FXF(2.0f), BK_ORB, BC_PINK);
    if (t > 70 && t % dsel(120, 90, 70, 60) == 0) pat_ring(b->x, b->y, dsel(14, 18, 24, 30), t * 3, FXF(1.2f), BK_PELLET, BC_ORANGE);
    if (G.diff >= DIFF_HARD && t > 90 && t % 24 == 0) pat_aimed(b->x, b->y, 2, 12, FXF(3.0f), BK_KUNAI, BC_RED);
}

static void asahi_2(Boss *b, int t) {                           /* Dawn Sign: Rosy Fingers of Morning */
    if (t == 0) boss_goto(b, FX(192), FX(100));
    if (t < 70) return;
    int tt = t - 70, iv = dsel(100, 80, 64, 52);
    if (tt % iv == 0) {
        int k = tt / iv;
        pat_flower(b->x, b->y, dsel(36, 48, 60, 72), 5 + (k % 2), k * 37, FXF(1.7f), 130, BK_PELLET, k % 2 ? BC_PINK : BC_ORANGE);
    }
    if (tt > iv && tt % dsel(14, 10, 8, 6) == 0)
        bullet_add(b->x, b->y, aim_angle(b->x, b->y) + ((tt / 7) % 5 - 2) * 12, FXF(2.4f), BK_RICE, BC_YELLOW);
}

static void asahi_3(Boss *b, int t) {                           /* Cloud Sign: Silk Road of Rain */
    if (t == 0) boss_goto(b, FX(192), FX(70));
    if (t < 60) return;
    b->tx = FX(192) + fx_mul(fx_sin(t * 3), FX(120));
    b->ty = FX(70);
    int rate = dsel(5, 3, 2, 1);
    if (t % rate == 0) {
        Bullet *bu = bullet_add(FX((int)(kg_rng_next(&G.rng) % PFW)), FX(-8), 256 + (int)(kg_rng_next(&G.rng) % 40) - 20, FXF(0.7f),
                                BK_RICE, (int[]){ BC_CYAN, BC_BLUE, BC_WHITE }[(t / rate) % 3]);
        bullet_gravity(bu, FXF(0.010f));
    }
    int iv = dsel(240, 190, 150, 120);
    int ph = (t - 60) % iv;
    if (ph == 0) s_beam_angle = aim_angle(b->x, b->y);
    if (ph < dsel(0, 26, 34, 40)) pat_beam_step(b->x, b->y, s_beam_angle, FXF(5.0f), BC_WHITE);
    if ((t - 60) % dsel(150, 120, 100, 80) == 0) pat_ring(b->x, b->y, dsel(10, 14, 18, 22), t, FXF(1.0f), BK_ORB, BC_BLUE);
}

/* ---- Takeru, the bamboo spirit (stage 2 mid-boss) */

static void takeru_1(Boss *b, int t) {
    boss_wander(b, t, 200, 100);
    if (t > 40) {
        int step = dsel(6, 4, 3, 2);
        if (t % step == 0) {
            int a = t * 9;
            bullet_add(b->x, b->y, a, FXF(1.6f), BK_PELLET, BC_GREEN);
            bullet_add(b->x, b->y, a + ANG / 2, FXF(1.6f), BK_PELLET, BC_CYAN);
        }
        if (t % dsel(170, 130, 100, 80) == 0) pat_aimed(b->x, b->y, dsel(5, 7, 9, 11), 120, FXF(1.8f), BK_ORB, BC_YELLOW);
    }
}

static void takeru_2(Boss *b, int t) {                          /* Bamboo Sign: Hundred Joints */
    if (t == 0) boss_goto(b, FX(192), FX(80));
    if (t < 70) return;
    int tt = t - 70, iv = dsel(130, 100, 80, 64);
    if (tt % iv == 0) {
        int n = dsel(9, 12, 14, 16);
        int off = ((tt / iv) % 2) * (PFW / n / 2);
        for (int i = 0; i < n; i++) {
            Bullet *bu = bullet_add(FX(off + i * PFW / n + PFW / n / 2), FX(-6), ANG / 4, FXF(2.2f), BK_BILL, BC_GREEN);
            bullet_stopgo(bu, 40 + (i % 3) * 8, FXF(2.8f), -1);
        }
    }
    if (tt % dsel(34, 24, 18, 14) == 0) bullet_add(b->x, b->y, aim_angle(b->x, b->y) + ((tt / 5) % 3 - 1) * 20, FXF(2.0f), BK_RICE, BC_YELLOW);
}

/* ---- Mochiko, the moon rabbit (stage 2 boss) */

static void mochiko_1(Boss *b, int t) {
    boss_wander(b, t, 180, 100);
    if (t > 40 && t % dsel(7, 5, 4, 3) == 0) {
        int a = ANG / 4 + (fx_sin(t * 4) >> 8);                      /* a sweep of +-256 units */
        Bullet *bu = bullet_add(b->x, b->y, a, FXF(1.8f), BK_RICE, BC_WHITE);
        bullet_curve(bu, (t / 70) % 2 ? 2 : -2, 100);
    }
    if (t > 60 && t % dsel(110, 90, 70, 56) == 0) pat_ring(b->x, b->y, dsel(10, 14, 18, 24), t, FXF(1.3f), BK_ORB, BC_YELLOW);
}

static void mochiko_2(Boss *b, int t) {                         /* Moon Sign: Pestle of the Waxing Moon */
    if (t == 0) boss_goto(b, FX(192), FX(90));
    if (t < 70) return;
    int tt = t - 70, iv = dsel(110, 90, 75, 66);
    if (tt % iv == 0) {
        int n = dsel(5, 7, 8, 9), parts = dsel(10, 14, 16, 18);
        for (int i = 0; i < n; i++) {
            Bullet *bu = bullet_add(b->x, b->y, tt * 5 + i * ANG / n, FXF(1.4f), BK_BIG, BC_YELLOW);
            bullet_split(bu, 50, parts, BK_PELLET, BC_WHITE, FXF(1.3f));
        }
    }
    if (tt % dsel(26, 18, 14, 11) == 0) bullet_add(b->x, b->y, aim_angle(b->x, b->y), FXF(2.6f), BK_RICE, BC_PINK);
}

static void mochiko_3(Boss *b, int t) {
    boss_wander(b, t, 220, 110);
    if (t < 50) return;
    int ph = (t - 50) % 260;
    if (G.diff >= DIFF_NORMAL && ph < 130) {
        int a = ANG / 4 + (ph - 65) * 3;
        pat_beam_step(b->x, b->y, a, FXF(4.2f), BC_RED);
        if (G.diff >= DIFF_HARD) pat_beam_step(b->x, b->y, ANG / 2 - a, FXF(4.2f), BC_RED);
    }
    if (t % dsel(40, 50, 40, 32) == 0) pat_aimed(b->x, b->y, dsel(5, 5, 7, 9), 100, FXF(1.9f), BK_ORB, BC_PINK);
    if (t % dsel(160, 110, 90, 72) == 0) pat_ring(b->x, b->y, dsel(12, 16, 22, 28), t, FXF(1.2f), BK_PELLET, BC_WHITE);
}

static void mochiko_4(Boss *b, int t) {                         /* Rabbit Sign: Eight Hundred Mochi Strikes */
    if (t == 0) boss_goto(b, FX(192), FX(80));
    if (t < 70) return;
    int tt = t - 70;
    if (tt % dsel(10, 7, 5, 4) == 0) {
        int a = 64 + (tt * 37) % 384;
        Bullet *bu = bullet_add(b->x, b->y, a, FXF(2.2f), BK_BILL, BC_PINK);
        bullet_bounce(bu, 2);
    }
    if (tt % dsel(70, 50, 40, 32) == 0) {
        for (int side = 0; side < 2; side++) {
            int y = 40 + (int)(kg_rng_next(&G.rng) % 200);
            Bullet *bu = bullet_add(FX(side ? PFW : 0), FX(y), side ? ANG / 2 : 0, FXF(0.6f), BK_ORB, BC_YELLOW);
            bullet_accel(bu, side ? ANG / 2 : 0, FXF(0.03f), 90);
        }
    }
    if (tt % dsel(90, 72, 60, 50) == 0) pat_aimed(b->x, b->y, dsel(5, 7, 9, 11), 110, FXF(1.8f), BK_RICE, BC_WHITE);
}

/* ---- Chochin, the lantern ghost (stage 3 mid-boss) */

static void chochin_1(Boss *b, int t) {
    boss_wander(b, t, 180, 100);
    if (t > 40 && t % dsel(110, 90, 76, 64) == 0) pat_ring(b->x, b->y, dsel(6, 8, 10, 12), t * 2, FXF(1.0f), BK_BIG, BC_ORANGE);
    if (t > 60 && t % dsel(60, 44, 36, 30) == 0) pat_aimed(b->x, b->y, dsel(3, 5, 7, 9), 80, FXF(2.2f), BK_RICE, BC_YELLOW);
}

static void chochin_2(Boss *b, int t) {                         /* Lantern Sign: Hundred Paper Moons */
    if (t == 0) boss_goto(b, FX(192), FX(100));
    if (t < 70) return;
    int tt = t - 70, iv = dsel(90, 70, 58, 48);
    if (tt % iv == 0) {
        int n = dsel(6, 8, 9, 10);
        for (int i = 0; i < n; i++) {
            Bullet *bu = bullet_add(b->x, b->y, tt * 3 + i * ANG / n, FXF(1.1f), BK_BIG, BC_ORANGE);
            bullet_split(bu, 40, dsel(8, 10, 12, 14), BK_ORB, BC_RED, FXF(1.2f));
        }
    }
    if (tt % dsel(28, 20, 15, 12) == 0) bullet_add(b->x, b->y, aim_angle(b->x, b->y) + (int)(kg_rng_next(&G.rng) % 41) - 20, FXF(2.4f), BK_KUNAI, BC_YELLOW);
}

/* ---- Yozora, the midnight lady (stage 3 boss) */

static void yozora_1(Boss *b, int t) {
    boss_wander(b, t, 170, 100);
    if (t > 40) {
        int step = dsel(5, 4, 3, 2);
        if (t % step == 0) {
            int a = t * 11;
            bullet_add(b->x, b->y, a, FXF(1.7f), BK_PELLET, BC_BLUE);
            bullet_add(b->x, b->y, -a, FXF(1.7f), BK_PELLET, BC_PURPLE);
        }
        if (t % dsel(110, 80, 62, 50) == 0) pat_aimed(b->x, b->y, dsel(5, 7, 9, 11), 110, FXF(2.1f), BK_KUNAI, BC_WHITE);
    }
}

static void yozora_2(Boss *b, int t) {                          /* Night Sign: Starless Sky */
    if (t == 0) boss_goto(b, FX(192), FX(110));
    if (t < 70) return;
    int tt = t - 70, arms = dsel(4, 5, 6, 8);
    if (tt % 4 == 0) {
        for (int k = 0; k < arms; k++) {
            if ((tt / 40) % 3 == 2 && (k & 1)) continue;                 /* gaps open up regularly */
            bullet_add(b->x, b->y, tt * 6 + k * ANG / arms, FXF(1.5f), BK_PELLET, k & 1 ? BC_PURPLE : BC_BLUE);
        }
    }
    if (tt % dsel(210, 170, 140, 120) == 0) pat_ring(b->x, b->y, dsel(8, 10, 12, 14), tt, FXF(0.9f), BK_BIG, BC_WHITE);
}

static void yozora_3(Boss *b, int t) {                          /* Dream Sign: Mirror of Midnight */
    if (t == 0) boss_goto(b, FX(192), FX(90));
    if (t < 70) return;
    int tt = t - 70;
    if (tt % dsel(8, 6, 5, 4) == 0) {
        int a = 40 + (tt * 7) % 440;
        Bullet *bu = bullet_add(b->x, b->y, a, FXF(1.8f), BK_RICE, BC_PURPLE);
        bullet_curve(bu, 2, 120);
        bu = bullet_add(b->x, b->y, ANG / 2 - a, FXF(1.8f), BK_RICE, BC_BLUE);
        bullet_curve(bu, -2, 120);
    }
    if (tt % dsel(100, 76, 62, 50) == 0) pat_aimed(b->x, b->y, dsel(5, 7, 9, 11), 100, FXF(2.3f), BK_KUNAI, BC_WHITE);
    if (tt % dsel(180, 140, 110, 90) == 0) pat_ring(b->x, b->y, dsel(12, 16, 20, 24), tt, FXF(1.1f), BK_ORB, BC_PINK);
}

static void yozora_4(Boss *b, int t) {                          /* Moon Sign: Tide of Silver */
    if (t == 0) boss_goto(b, FX(192), FX(80));
    if (t < 70) return;
    int tt = t - 70;
    if (tt % dsel(5, 3, 3, 2) == 0) {
        int x = 192 + (int)(fx_sin(tt * 5) >> 9) * 160 / 128;
        bullet_add(FX(x), FX(-6), ANG / 4, dsel(FXF(1.4f), FXF(1.7f), FXF(1.9f), FXF(2.1f)), BK_STAR, BC_CYAN);
        bullet_add(FX(PFW - x), FX(-6), ANG / 4, dsel(FXF(1.4f), FXF(1.7f), FXF(1.9f), FXF(2.1f)), BK_STAR, BC_WHITE);
    }
    int iv = dsel(300, 220, 180, 150);
    int ph = tt % iv;
    if (G.diff >= DIFF_NORMAL) {
        if (ph == 0) { s_beam_angle = aim_angle(FX(0), FX(40)); s_beam_angle2 = aim_angle(FX(PFW), FX(40)); }
        if (ph < 40) {
            pat_beam_step(FX(0), FX(40), s_beam_angle, FXF(4.4f), BC_WHITE);
            pat_beam_step(FX(PFW), FX(40), s_beam_angle2, FXF(4.4f), BC_WHITE);
        }
    }
    if (tt % dsel(70, 52, 42, 34) == 0) bullet_add(b->x, b->y, aim_angle(b->x, b->y), FXF(2.6f), BK_RICE, BC_BLUE);
}

static void yozora_5(Boss *b, int t) {                          /* Last Word: Dawn Never Comes */
    if (t == 0) boss_goto(b, FX(192), FX(100));
    if (t < 80) return;
    int tt = t - 80;
    int beams = dsel(2, 3, 4, 4);
    int sweep = tt * 2;
    if (G.diff >= DIFF_NORMAL || tt % 2 == 0)
        for (int k = 0; k < beams; k++) pat_beam_step(b->x, b->y, sweep + k * ANG / beams, FXF(3.6f), k & 1 ? BC_PURPLE : BC_BLUE);
    if (tt % dsel(110, 85, 70, 56) == 0) {
        int n = dsel(14, 18, 22, 26);
        for (int i = 0; i < n; i++) {
            Bullet *bu = bullet_add(b->x, b->y, tt + i * ANG / n, FXF(0.5f), BK_ORB, BC_WHITE);
            bullet_accel(bu, tt + i * ANG / n, FXF(0.025f), 70);
        }
    }
    if (tt % dsel(150, 110, 90, 72) == 0) {
        int n = dsel(5, 7, 9, 11);
        for (int i = 0; i < n; i++) {
            Bullet *bu = bullet_add(b->x, b->y, aim_angle(b->x, b->y) + (i - n / 2) * 30, FXF(2.0f), BK_KUNAI, BC_PINK);
            bullet_stopgo(bu, 30 + i * 3, FXF(3.0f), -1);
        }
    }
}

/* ---------------------------------------------------------------- the cast */

static const PhaseDef P_ASAHI[] = {
    { NULL, 2600, 2400, 0, asahi_1 },
    { "Dawn Sign \"Rosy Fingers of Morning\"", 3800, 3000, 1000000, asahi_2 },
    { "Cloud Sign \"Silk Road of Rain\"", 4400, 3300, 1500000, asahi_3 },
};
static const PhaseDef P_TAKERU[] = {
    { NULL, 2000, 2000, 0, takeru_1 },
    { "Bamboo Sign \"Hundred Joints\"", 3000, 2700, 800000, takeru_2 },
};
static const PhaseDef P_MOCHIKO[] = {
    { NULL, 2800, 2400, 0, mochiko_1 },
    { "Moon Sign \"Pestle of the Waxing Moon\"", 4200, 3000, 1500000, mochiko_2 },
    { NULL, 3000, 2400, 0, mochiko_3 },
    { "Rabbit Sign \"Eight Hundred Mochi Strikes\"", 4800, 3300, 2000000, mochiko_4 },
};
static const PhaseDef P_CHOCHIN[] = {
    { NULL, 2400, 2100, 0, chochin_1 },
    { "Lantern Sign \"Hundred Paper Moons\"", 3400, 2700, 1200000, chochin_2 },
};
static const PhaseDef P_YOZORA[] = {
    { NULL, 3200, 2400, 0, yozora_1 },
    { "Night Sign \"Starless Sky\"", 4800, 3000, 2000000, yozora_2 },
    { "Dream Sign \"Mirror of Midnight\"", 5200, 3300, 2500000, yozora_3 },
    { "Moon Sign \"Tide of Silver\"", 5400, 3300, 2800000, yozora_4 },
    { "Last Word \"Dawn Never Comes\"", 7000, 4200, 5000000, yozora_5 },
};

const BossDef BOSSES[] = {
    { "Asahi", "Dawn Fairy of the Clouds", P_ASAHI, 3, false, 0, 0xF08A9A },
    { "Takeru", "Spirit of the Hundred Joints", P_TAKERU, 2, true, 1, 0x58C878 },
    { "Mochiko", "Rabbit of the Waxing Moon", P_MOCHIKO, 4, false, 2, 0xE8D86A },
    { "Chochin", "Lantern Ghost of the Shore", P_CHOCHIN, 2, true, 3, 0xF0A040 },
    { "Yozora", "Lady of the Moonless Midnight", P_YOZORA, 5, false, 4, 0x7A6AE8 },
};
const int N_BOSSES = (int)(sizeof BOSSES / sizeof BOSSES[0]);

/* ---------------------------------------------------------------- the boss machinery */

void boss_goto(Boss *b, fx x, fx y) {
    b->tx = x;
    b->ty = y;
}

void boss_begin(int id) {
    Boss *b = &G.boss;
    memset(b, 0, sizeof *b);
    b->active = true;
    b->id = id;
    b->def = &BOSSES[id];
    b->x = FX(PFW / 2);
    b->y = FX(-50);
    b->tx = FX(PFW / 2);
    b->ty = FX(100);
    b->phase = -1;
    b->invuln = 1 << 20;
    G.boss_name_t = 0;
    bullets_clear(false);
}

static void start_phase(Boss *b, int n) {
    const PhaseDef *ph = &b->def->phases[n];
    b->phase = n;
    b->maxhp = b->hp = b->shown_hp = ph->hp;
    b->timer = ph->time;
    b->t = 0;
    b->spell = ph->name != NULL;
    b->failed = false;
    b->invuln = b->spell ? 100 : 40;
    if (b->spell) G.spell_banner_t = 0;
    if (b->spell) boss_goto(b, FX(PFW / 2), FX(100));
    fx_ring(b->x, b->y, BC_WHITE);
}

static void end_phase(Boss *b) {
    const PhaseDef *ph = &b->def->phases[b->phase];
    bool killed = b->hp <= 0;
    if (G.trace) printf("danmaku: %s phase %d (%s) ended: %s, hp %d/%d, %d ticks used, power %d\n", b->def->name, b->phase, ph->name ? ph->name : "attack",
                        killed ? (b->failed ? "defeated (no bonus)" : "defeated") : "time up", b->hp, b->maxhp, b->t, G.p.power);
    if (b->spell) {
        G.stat_cards++;
        if (killed && !b->failed) {
            long bonus = ph->bonus + ph->bonus * b->timer / (ph->time * 2);
            add_score(bonus);
            G.last_bonus = bonus;
            G.last_bonus_t = 220;
            G.last_bonus_ok = true;
            G.stat_captures++;
        } else {
            G.last_bonus = 0;
            G.last_bonus_t = 150;
            G.last_bonus_ok = false;
        }
    }
    add_score(killed ? 40000 : 8000);
    bullets_clear(killed);
    fx_puff(b->x, b->y, b->def->sprite % 5 + 3, 14);
    fx_ring(b->x, b->y, BC_WHITE);
    G.shake = 10;
    if (killed) {
        item_burst(IT_POWER, b->x, b->y, 3, 30);
        item_burst(IT_POINT, b->x, b->y, 5, 36);
    }
    if (b->phase + 1 < b->def->nphases) start_phase(b, b->phase + 1);
    else {
        b->dying = 1;
        b->invuln = 1 << 20;
        G.shake = 30;
        G.flash = 10;
    }
}

int boss_damage(int dmg) {
    Boss *b = &G.boss;
    if (!b->active || b->invuln > 0 || b->dying || b->phase < 0) return 0;
    b->hp -= dmg;
    b->flash = 2;
    if (b->hp < 0) b->hp = 0;
    return dmg;
}

void boss_tick(void) {
    Boss *b = &G.boss;
    if (!b->active) return;
    b->entry++;
    fx dx = b->tx - b->x, dy = b->ty - b->y;
    b->x += dx / 14 + (dx > 0 ? 1200 : dx < 0 ? -1200 : 0);
    b->y += dy / 14 + (dy > 0 ? 1200 : dy < 0 ? -1200 : 0);
    if (iabs(FXI(dx)) < 1) b->x = b->tx;
    if (iabs(FXI(dy)) < 1) b->y = b->ty;
    if (b->flash > 0) b->flash--;
    if (b->dying) {
        b->dying++;
        if (b->dying % 6 == 0) {
            fx ox = FX((int)(kg_rng_next(&G.rng) % 60) - 30), oy = FX((int)(kg_rng_next(&G.rng) % 60) - 30);
            fx_puff(b->x + ox, b->y + oy, BC_WHITE, 4);
            fx_ring(b->x + ox, b->y + oy, b->dying % 12 ? BC_PINK : BC_YELLOW);
        }
        if (b->dying > 90) {
            item_burst(IT_POWER, b->x, b->y, 4, 40);
            item_burst(IT_POINT, b->x, b->y, 8, 50);
            if (!b->def->midboss) item_spawn(IT_FULL, b->x, b->y, 3);
            else item_spawn(IT_BOMB, b->x, b->y, 3);
            fx_puff(b->x, b->y, BC_WHITE, 30);
            b->active = false;
            if (!b->def->midboss) {
                long bonus = 2000000L * (G.stage + 1) + 600000L * G.p.lives + 150000L * G.p.bombs;
                add_score(bonus);
                G.clear_bonus = bonus;
                G.state = GS_STAGE_CLEAR;
                G.clear_t = 330;
                bullets_clear(true);
            }
        }
        return;
    }
    if (b->phase < 0) {
        if (b->entry >= 60) { b->invuln = 0; start_phase(b, G.dbg_phase < b->def->nphases ? G.dbg_phase : 0); G.dbg_phase = 0; }
        return;
    }
    const PhaseDef *ph = &b->def->phases[b->phase];
    b->t++;
    b->timer--;
    if (b->invuln > 0) b->invuln--;
    if (b->shown_hp > b->hp) b->shown_hp -= (b->shown_hp - b->hp + 7) / 8;
    ph->fn(b, b->t - 1);
    if (b->hp <= 0 || b->timer <= 0) end_phase(b);
}
