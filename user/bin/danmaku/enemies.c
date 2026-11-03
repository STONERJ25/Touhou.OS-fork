/* The small enemies: fairies, orbs and wisps. Movement and shooting are small scripts selected by id and tuned by parameters. */
#include "dm.h"
#include <string.h>

static inline fx tenth(int v) { return (fx)v * 6554; }              /* tenths of a pixel -> fixed point */

Enemy *enemy_spawn(int type, fx x, fx y, int move, int shoot, int hp, int drop) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &G.en[i];
        if (e->active) continue;
        memset(e, 0, sizeof *e);
        e->active = true;
        e->type = (uint8_t)type;
        e->move = (uint8_t)move;
        e->shoot = (uint8_t)shoot;
        e->drop = (uint8_t)drop;
        e->x = e->ox = x;
        e->y = e->oy = y;
        e->hp = e->maxhp = hp;
        e->score = type == ET_MAID ? 2000 : type == ET_ORB ? 500 : type == ET_WISP ? 600 : 300;
        return e;
    }
    return NULL;
}

void enemies_reset(void) {
    memset(G.en, 0, sizeof G.en);
}

void enemy_defeat(Enemy *e, bool by_player) {
    if (!e->active) return;
    e->active = false;
    static const int tint[ET_COUNT] = { BC_BLUE, BC_RED, BC_GREEN, BC_YELLOW, BC_PINK, BC_ORANGE, BC_CYAN };
    fx_puff(e->x, e->y, tint[e->type], e->type == ET_MAID ? 12 : 7);
    fx_ring(e->x, e->y, tint[e->type]);
    if (!by_player) return;
    add_score(e->score);
    switch (e->drop) {
    case DROP_POWER: item_spawn(IT_POWER, e->x, e->y, 1); break;
    case DROP_POINT: item_spawn(IT_POINT, e->x, e->y, 1); break;
    case DROP_BIG: item_spawn(IT_POWER_BIG, e->x, e->y, 2); item_burst(IT_POINT, e->x, e->y, 2, 18); break;
    case DROP_BOMB: item_spawn(IT_BOMB, e->x, e->y, 2); item_burst(IT_POINT, e->x, e->y, 2, 18); break;
    case DROP_LIFE: item_spawn(IT_LIFE, e->x, e->y, 2); item_burst(IT_POINT, e->x, e->y, 3, 18); break;
    case DROP_MIXED: item_burst(IT_POWER, e->x, e->y, 3, 20); item_burst(IT_POINT, e->x, e->y, 3, 20); break;
    default: break;
    }
}

/* ---------------------------------------------------------------- movement */

static void move_enemy(Enemy *e) {
    int t = e->t;
    switch (e->move) {
    case MV_DOWN:                                    /* p0: speed, p1: sideways speed (tenths of px per tick) */
        e->x += tenth(e->p[1]);
        e->y = e->oy + (fx)t * tenth(e->p[0]);
        break;
    case MV_SINE:                                    /* p0: speed, p1: amplitude (px), p2: angular speed (units per tick) */
        e->y = e->oy + (fx)t * tenth(e->p[0]);
        e->x = e->ox + fx_mul(fx_sin(t * e->p[2] + e->p[3]), FX(e->p[1]));
        break;
    case MV_ARC: {                                   /* p0: speed, p1: start angle, p2: turn (units per 8 ticks) */
        int a = e->p[1] + (e->p[2] * t) / 8;
        e->x += fx_mul(fx_cos(a), tenth(e->p[0]));
        e->y += fx_mul(fx_sin(a), tenth(e->p[0]));
        break;
    }
    case MV_STOP: {                                  /* p0: speed, p1: stop at y (px), p2: wait (ticks), p3: leave angle */
        int stop_t = e->p[1] > 0 ? (int)((FX(e->p[1]) - e->oy) / (tenth(e->p[0]) ? tenth(e->p[0]) : 1)) : 0;
        if (t < stop_t) e->y = e->oy + (fx)t * tenth(e->p[0]);
        else if (t < stop_t + e->p[2]) { e->y = e->oy + (fx)stop_t * tenth(e->p[0]); }
        else {
            int dt = t - stop_t - e->p[2];
            fx sp = tenth(e->p[0]) * 14 / 10;
            e->x += fx_mul(fx_cos(e->p[3]), sp * (1 + dt / 40));
            e->y += fx_mul(fx_sin(e->p[3]), sp * (1 + dt / 40));
        }
        break;
    }
    case MV_SWOOP:                                   /* p0: speed, p1: direction (+1 / -1), p2: depth (px), p3: period (ticks) */
        e->x = e->ox + (fx)t * tenth(e->p[0]) * e->p[1];
        e->y = e->oy + fx_mul(fx_sin(t * 512 / (e->p[3] ? e->p[3] : 1)), FX(e->p[2]));
        break;
    case MV_DRIFT:                                   /* p0: speed, p1: amplitude, p2: angular speed */
        e->y = e->oy + (fx)t * tenth(e->p[0]);
        e->x = e->ox + fx_mul(fx_sin(t * e->p[2]), FX(e->p[1])) + fx_mul(fx_cos(t * e->p[2] / 2 + 100), FX(e->p[1] / 2));
        break;
    default: break;
    }
}

/* ---------------------------------------------------------------- shooting */

static void shoot_enemy(Enemy *e) {
    int t = e->t - 24;                               /* nothing happens while they fly in */
    if (t < 0 || e->y < FX(8) || e->y > FX(PFH - 70)) return;
    int iv = e->s[0] > 0 ? e->s[0] : 60;
    int color = e->s[1];
    switch (e->shoot) {
    case SH_AIM:
        if (t % iv == 0) {
            fx sp = dsel(FXF(1.6f), FXF(2.0f), FXF(2.4f), FXF(2.8f));
            pat_aimed(e->x, e->y, dsel(1, 1, 2, 3), 14, sp, BK_RICE, color);
        }
        break;
    case SH_AIM3:
        if (t % iv == 0) {
            fx sp = dsel(FXF(1.5f), FXF(1.9f), FXF(2.2f), FXF(2.6f));
            pat_aimed(e->x, e->y, dsel(3, 3, 5, 7), dsel(60, 70, 80, 90), sp, BK_ORB, color);
        }
        break;
    case SH_RING:
        if (t % iv == 0) {
            int n = dsel(8, 12, 16, 22);
            fx sp = dsel(FXF(1.1f), FXF(1.4f), FXF(1.7f), FXF(2.0f));
            pat_ring(e->x, e->y, n, (e->t * 5) & (ANG - 1), sp, BK_ORB, color);
        }
        break;
    case SH_SPIN:
        if (t % dsel(5, 4, 3, 2) == 0) {
            int a = t * 23;
            fx sp = dsel(FXF(1.4f), FXF(1.7f), FXF(2.0f), FXF(2.2f));
            bullet_add(e->x, e->y, a, sp, BK_PELLET, color);
            bullet_add(e->x, e->y, a + ANG / 2, sp, BK_PELLET, color);
            if (G.diff >= DIFF_HARD) { bullet_add(e->x, e->y, a + ANG / 4, sp, BK_PELLET, color); bullet_add(e->x, e->y, a + 3 * ANG / 4, sp, BK_PELLET, color); }
        }
        break;
    case SH_RAIN:
        if (t % dsel(18, 12, 8, 5) == 0) {
            int a = ANG / 4 - 70 + (int)(kg_rng_next(&G.rng) % 141);
            fx sp = FXF(1.2f) + (fx)(kg_rng_next(&G.rng) % 30000);
            bullet_add(e->x, e->y, a, sp, BK_RICE, color);
        }
        break;
    case SH_BURST:
        if (t % iv < dsel(3, 4, 5, 6) * 6 && t % 6 == 0) {
            fx sp = dsel(FXF(2.2f), FXF(2.6f), FXF(3.0f), FXF(3.4f));
            bullet_add(e->x, e->y, aim_angle(e->x, e->y), sp, BK_KUNAI, color);
        }
        break;
    case SH_FLOWER:
        if (t % iv == 0)
            pat_flower(e->x, e->y, dsel(16, 24, 32, 40), 5, e->t, FXF(1.4f), 120, BK_PELLET, color);
        break;
    default: break;
    }
}

void enemies_update(void) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &G.en[i];
        if (!e->active) continue;
        move_enemy(e);
        e->t++;
        if (e->flash > 0) e->flash--;
        if (G.p.respawn_t == 0 || e->shoot == SH_RAIN) shoot_enemy(e);
        if (e->x < FX(-48) || e->x > FX(PFW + 48) || e->y > FX(PFH + 48) || (e->y < FX(-90) && e->t > 90)) e->active = false;
        else if (e->hp <= 0) enemy_defeat(e, true);
    }
}
