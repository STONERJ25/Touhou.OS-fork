/* The simulation core: the player, the player's shots and bombs, items, effects, scoring and the flow from stage to stage. */
#include "dm.h"
#include <stdlib.h>
#include <string.h>

Game G;

static const char *const DIFF_NAMES[DIFF_COUNT] = { "Easy", "Normal", "Hard", "Lunatic" };
static const char *const SHOT_NAMES[SHOT_COUNT] = { "Shrine Maiden", "Witch" };
const char *diff_name(int d) { return DIFF_NAMES[d < 0 || d >= DIFF_COUNT ? 1 : d]; }
const char *shot_name(int s) { return SHOT_NAMES[s < 0 || s >= SHOT_COUNT ? 0 : s]; }

int dsel(int easy, int normal, int hard, int lunatic) {
    switch (G.diff) { case DIFF_EASY: return easy; case DIFF_NORMAL: return normal; case DIFF_HARD: return hard; default: return lunatic; }
}

static const long EXTEND_AT[] = { 5000000L, 12000000L, 25000000L, 45000000L, 80000000L };

void add_score(long n) {
    G.score += n;
    while (G.extends_given < (int)(sizeof EXTEND_AT / sizeof EXTEND_AT[0]) && G.score >= EXTEND_AT[G.extends_given]) {
        G.extends_given++;
        if (G.p.lives < 8) G.p.lives++;
        fx_ring(G.p.x, G.p.y, BC_PINK);
        G.extend_t = 150;
    }
    if (G.score > G.hiscore) G.hiscore = G.score;
}

/* ---------------------------------------------------------------- effects */

static Fx *fx_alloc(void) {
    for (int i = 0; i < MAX_FX; i++) if (!G.fx[i].active) { memset(&G.fx[i], 0, sizeof G.fx[i]); G.fx[i].active = true; return &G.fx[i]; }
    return NULL;
}

void fx_spark(fx x, fx y, int color, int n) {
    for (int i = 0; i < n; i++) {
        Fx *f = fx_alloc();
        if (!f) return;
        int a = (int)(kg_rng_next(&G.rng) & (ANG - 1));
        int sp = 20000 + (int)(kg_rng_next(&G.rng) % 40000);
        f->kind = FXK_SPARK;
        f->color = (uint8_t)color;
        f->x = x; f->y = y;
        f->vx = fx_mul(fx_cos(a), sp);
        f->vy = fx_mul(fx_sin(a), sp);
        f->life = 10 + (int)(kg_rng_next(&G.rng) % 8);
    }
}

void fx_puff(fx x, fx y, int color, int n) {
    for (int i = 0; i < n; i++) {
        Fx *f = fx_alloc();
        if (!f) return;
        int a = (int)(kg_rng_next(&G.rng) & (ANG - 1));
        int sp = 10000 + (int)(kg_rng_next(&G.rng) % 50000);
        f->kind = FXK_PUFF;
        f->color = (uint8_t)color;
        f->x = x; f->y = y;
        f->vx = fx_mul(fx_cos(a), sp);
        f->vy = fx_mul(fx_sin(a), sp);
        f->life = 18 + (int)(kg_rng_next(&G.rng) % 10);
    }
}

void fx_ring(fx x, fx y, int color) {
    Fx *f = fx_alloc();
    if (!f) return;
    f->kind = FXK_RING;
    f->color = (uint8_t)color;
    f->x = x; f->y = y;
    f->life = 26;
}

void fx_text(fx x, fx y, int value, int color) {
    Fx *f = fx_alloc();
    if (!f) return;
    f->kind = FXK_TEXT;
    f->color = (uint8_t)color;
    f->x = x; f->y = y;
    f->vy = -FXF(0.5f);
    f->life = 40;
    f->val = value;
}

static void fx_update(void) {
    for (int i = 0; i < MAX_FX; i++) {
        Fx *f = &G.fx[i];
        if (!f->active) continue;
        f->x += f->vx;
        f->y += f->vy;
        if (f->kind == FXK_SPARK || f->kind == FXK_PUFF) { f->vx -= f->vx >> 4; f->vy -= f->vy >> 4; }
        if (f->kind == FXK_BOMB_ORB) continue;                   /* positioned by the bomb itself */
        if (++f->t >= f->life) f->active = false;
    }
}

/* ---------------------------------------------------------------- items */

void item_spawn(int type, fx x, fx y, int kick) {
    for (int i = 0; i < MAX_ITEMS; i++) {
        Item *it = &G.it[i];
        if (it->active) continue;
        memset(it, 0, sizeof *it);
        it->active = true;
        it->type = (uint8_t)type;
        it->x = x;
        it->y = y;
        it->vy = type == IT_STAR ? 0 : -FX(2) - kick * 8000;
        it->collect = type == IT_STAR;
        return;
    }
}

void item_burst(int type, fx x, fx y, int n, int spread) {
    for (int i = 0; i < n; i++) {
        int dx = (int)(kg_rng_next(&G.rng) % (unsigned)(spread * 2 + 1)) - spread;
        int dy = (int)(kg_rng_next(&G.rng) % (unsigned)(spread + 1)) - spread / 2;
        item_spawn(type, x + FX(dx), y + FX(dy), (int)(kg_rng_next(&G.rng) % 5));
    }
}

#define POC_LINE FX(120)

static void items_update(void) {
    Player *p = &G.p;
    bool can_take = p->respawn_t == 0 && p->death_t == 0;
    bool above_line = can_take && p->y < POC_LINE;
    for (int i = 0; i < MAX_ITEMS; i++) {
        Item *it = &G.it[i];
        if (!it->active) continue;
        it->t++;
        if (above_line || p->bomb_t > 0) it->collect = true;
        if (it->collect && can_take) {
            int a = fx_atan2(p->y - it->y, p->x - it->x);
            fx sp = FX(9);
            it->x += fx_mul(fx_cos(a), sp);
            it->y += fx_mul(fx_sin(a), sp);
        } else {
            it->collect = it->collect && can_take;
            it->vy += FXF(0.07f);
            if (it->vy > FXF(2.4f)) it->vy = FXF(2.4f);
            it->y += it->vy;
            if (it->y > FX(PFH + 16)) { it->active = false; continue; }
        }
        if (!can_take) continue;
        int dx = (it->x - p->x) >> 12, dy = (it->y - p->y) >> 12;
        int r16 = (p->focus ? 20 : 24) * 16;
        if (iabs(dx) > r16 || iabs(dy) > r16 || dx * dx + dy * dy > r16 * r16) continue;
        /* collected */
        it->active = false;
        switch (it->type) {
        case IT_POWER:
            if (p->power >= MAX_POWER) add_score(2000);
            else { p->power += 5; if (p->power > MAX_POWER) p->power = MAX_POWER; add_score(10); }
            break;
        case IT_POWER_BIG:
            if (p->power >= MAX_POWER) add_score(10000);
            else { p->power += 100; if (p->power > MAX_POWER) p->power = MAX_POWER; add_score(500); }
            break;
        case IT_FULL: p->power = MAX_POWER; add_score(5000); break;
        case IT_POINT: {
            long v = it->collect ? 26000 : 8000 + (long)FXI(FX(PFH) - it->y) * 38;
            G.points++;
            add_score(v);
            fx_text(it->x, it->y, (int)(v / 100), it->collect ? BC_YELLOW : BC_WHITE);
            break;
        }
        case IT_BOMB: if (p->bombs < 6) p->bombs++; add_score(5000); break;
        case IT_LIFE: if (p->lives < 8) p->lives++; add_score(10000); fx_ring(p->x, p->y, BC_PINK); break;
        default: add_score(600); break;
        }
    }
}

/* ---------------------------------------------------------------- player shots */

static void pshot_add(int kind, fx x, fx y, int angle, fx speed, int dmg) {
    for (int i = 0; i < MAX_PSHOTS; i++) {
        PShot *s = &G.ps[i];
        if (s->active) continue;
        s->active = true;
        s->kind = (uint8_t)kind;
        s->x = x; s->y = y;
        s->angle = angle;
        s->vx = fx_mul(fx_cos(angle), speed);
        s->vy = fx_mul(fx_sin(angle), speed);
        s->dmg = dmg;
        return;
    }
}

#define UP (3 * ANG / 4)

static void player_fire(void) {
    Player *p = &G.p;
    int L = p->power / 100;
    if (L > 4) L = 4;
    bool odd = (G.tick & 4) != 0;                               /* needles every second volley */
    if (G.shot == SHOT_A) {
        static const int main_n[5] = { 2, 2, 4, 4, 6 };
        int n = main_n[L];
        for (int i = 0; i < n; i++) {
            int pos = n % 2 == 0 ? (i - n / 2) * 2 + 1 : (i - n / 2) * 2;      /* symmetric half-slots around the centre */
            pshot_add(PS_AMULET, p->x + FX(pos * (p->focus ? 2 : 4)), p->y - FX(10), UP + (p->focus ? 0 : pos * 3), FX(13), 3);
        }
        if (L >= 1 && odd) {
            int nn = L >= 3 ? 4 : 2;
            for (int i = 0; i < nn; i++) {
                int side = (i & 1) ? 1 : -1;
                fx ox = FX(side * (p->focus ? 12 : 22) * (1 + i / 2));
                pshot_add(PS_NEEDLE, p->x + ox, p->y - FX(6), UP + side * (p->focus ? 8 : 26), FX(9), 2);
            }
        }
    } else {
        static const int star_n[5] = { 2, 3, 4, 5, 6 };
        int n = star_n[L];
        for (int i = 0; i < n; i++) {
            int pos = n % 2 == 0 ? (i - n / 2) * 2 + 1 : (i - n / 2) * 2;
            pshot_add(p->focus ? PS_BEAM : PS_STAR, p->x + FX(pos * (p->focus ? 2 : 3)), p->y - FX(10), UP + (p->focus ? 0 : pos * 3),
                      FX(p->focus ? 15 : 12), p->focus ? 5 : 4);
        }
    }
}

static int nearest_target(fx x, fx y, fx *tx, fx *ty) {
    int best = 1 << 30;
    bool found = false;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &G.en[i];
        if (!e->active || e->y < 0 || e->y > FX(PFH - 20)) continue;
        int dx = (e->x - x) >> 12, dy = (e->y - y) >> 12;
        int d = dx * dx / 4 + dy * dy;                          /* prefers targets straight ahead */
        if (d < best) { best = d; *tx = e->x; *ty = e->y; found = true; }
    }
    if (G.boss.active && G.boss.dying == 0 && G.boss.entry > 30) {
        int dx = (G.boss.x - x) >> 12, dy = (G.boss.y - y) >> 12;
        int d = dx * dx / 4 + dy * dy - 6000;
        if (d < best) { *tx = G.boss.x; *ty = G.boss.y; found = true; }
    }
    return found;
}

static void pshots_update(void) {
    for (int i = 0; i < MAX_PSHOTS; i++) {
        PShot *s = &G.ps[i];
        if (!s->active) continue;
        /* hits are tested where the shot is now, before it moves: a shot born inside a target still counts */
        bool hit = false;
        for (int j = 0; j < MAX_ENEMIES && !hit; j++) {
            Enemy *e = &G.en[j];
            if (!e->active || e->y < FX(-6)) continue;
            int rx = e->type == ET_MAID ? 22 : 15, ry = e->type == ET_MAID ? 20 : 15;
            if (iabs(FXI(e->x - s->x)) < rx && iabs(FXI(e->y - s->y)) < ry) {
                e->hp -= s->dmg;
                e->flash = 3;
                fx_spark(s->x, s->y, BC_WHITE, 1);
                hit = true;
                if (e->hp <= 0) enemy_defeat(e, true);
            }
        }
        if (!hit && G.boss.active && G.boss.dying == 0 && iabs(FXI(G.boss.x - s->x)) < 30 && iabs(FXI(G.boss.y - s->y)) < 30) {
            boss_damage(s->dmg);
            fx_spark(s->x, s->y, BC_WHITE, 1);
            hit = true;
        }
        if (hit) { s->active = false; continue; }
        if (s->kind == PS_NEEDLE) {
            fx tx, ty;
            if (nearest_target(s->x, s->y, &tx, &ty)) {
                int want = fx_atan2(ty - s->y, tx - s->x);
                int diff = ((want - s->angle + 1536) & (ANG - 1)) - 512;          /* -512..511 */
                if (diff > 14) diff = 14;
                if (diff < -14) diff = -14;
                s->angle = (s->angle + diff) & (ANG - 1);
                s->vx = fx_mul(fx_cos(s->angle), FX(9));
                s->vy = fx_mul(fx_sin(s->angle), FX(9));
            }
        }
        s->x += s->vx;
        s->y += s->vy;
        if (s->y < FX(-16) || s->x < FX(-16) || s->x > FX(PFW + 16) || s->y > FX(PFH + 16)) s->active = false;
    }
}

/* ---------------------------------------------------------------- the player */

void player_hit(void) {
    Player *p = &G.p;
    if (p->invuln > 0 || p->death_t > 0 || p->respawn_t > 0 || G.god) return;
    p->death_t = 12;                                            /* a short window in which a bomb still saves us */
    fx_puff(p->x, p->y, BC_WHITE, 6);
    G.shake = 6;
}

static void do_bomb(void) {
    Player *p = &G.p;
    p->bombs--;
    G.stat_bombs++;
    p->bomb_t = G.shot == SHOT_A ? 150 : 130;
    if (p->invuln < p->bomb_t + 60) p->invuln = p->bomb_t + 60;
    if (G.boss.active && G.boss.spell) G.boss.failed = true;
    G.flash = 8;
    G.shake = 24;
    if (G.shot == SHOT_A) {
        for (int i = 0; i < 7; i++) {
            Fx *f = fx_alloc();
            if (!f) break;
            f->kind = FXK_BOMB_ORB;
            f->val = i;
            f->life = p->bomb_t;
            f->color = (uint8_t)(i % 7);
            f->x = p->x;
            f->y = p->y;
        }
    }
    fx_ring(p->x, p->y, BC_WHITE);
}

static void bomb_update(void) {
    Player *p = &G.p;
    if (p->bomb_t <= 0) return;
    int total = G.shot == SHOT_A ? 150 : 130;
    int age = total - p->bomb_t;
    p->bomb_t--;
    if (G.shot == SHOT_A) {
        int radius = age * 5;
        bullets_clear_circle(p->x, p->y, radius, true);
        if (age > 40 && age < 110 && (age & 1) == 0) bullets_clear(false);
        for (int i = 0; i < MAX_FX; i++) {                       /* the orbs spiral outwards */
            Fx *f = &G.fx[i];
            if (!f->active || f->kind != FXK_BOMB_ORB) continue;
            int a = age * 11 + f->val * ANG / 7;
            int r = 14 + age * 3 + (age > 60 ? (age - 60) * 2 : 0);
            f->x = p->x + fx_mul(fx_cos(a), FX(r));
            f->y = p->y + fx_mul(fx_sin(a), FX(r)) * 4 / 5;
            f->t = age;
            if (age >= f->life - 1) f->active = false;
        }
        for (int j = 0; j < MAX_ENEMIES; j++) {
            Enemy *e = &G.en[j];
            if (!e->active) continue;
            int dx = FXI(e->x - p->x), dy = FXI(e->y - p->y);
            if (dx * dx + dy * dy < (radius + 20) * (radius + 20)) { e->hp -= 14; e->flash = 3; if (e->hp <= 0) enemy_defeat(e, true); }
        }
        if (G.boss.active) {
            int dx = FXI(G.boss.x - p->x), dy = FXI(G.boss.y - p->y);
            if (dx * dx + dy * dy < (radius + 30) * (radius + 30)) boss_damage(7);
        }
    } else {
        int half = age * 9;                                       /* the beam widens until it covers the field */
        if (half > PFW) half = PFW;
        bullets_clear_column(p->x - FX(half), p->x + FX(half), true);
        if (age > 36 && age < 100 && (age & 1) == 0) bullets_clear(false);
        for (int j = 0; j < MAX_ENEMIES; j++) {
            Enemy *e = &G.en[j];
            if (!e->active) continue;
            if (iabs(FXI(e->x - p->x)) < half + 12 && e->y < p->y) { e->hp -= 16; e->flash = 3; if (e->hp <= 0) enemy_defeat(e, true); }
        }
        if (G.boss.active && iabs(FXI(G.boss.x - p->x)) < half + 24 && G.boss.y < p->y) boss_damage(age < 90 ? 9 : 4);
    }
}

static void player_die(void) {
    Player *p = &G.p;
    G.stat_deaths++;
    if (G.boss.active && G.boss.spell) G.boss.failed = true;
    item_burst(IT_POWER, p->x, p->y, 5, 28);
    p->power -= 60;
    if (p->power < 0) p->power = 0;
    fx_puff(p->x, p->y, BC_WHITE, 16);
    fx_ring(p->x, p->y, BC_WHITE);
    G.shake = 14;
    G.flash = 4;
    bullets_clear_circle(p->x, p->y, 90, false);
    if (--p->lives < 0) {
        p->lives = 0;
        G.state = GS_GAME_OVER;
        G.clear_t = 200;
        return;
    }
    if (p->bombs < 3) p->bombs = 3;
    p->x = FX(PFW / 2);
    p->y = FX(PFH + 30);
    p->respawn_t = 40;
    p->invuln = 40 + 150;
}

static void player_update(void) {
    Player *p = &G.p;
    const Input *in = &G.in;
    bool bomb_pressed = in->bomb && !G.prev_in.bomb;
    if (p->hit_flash > 0) p->hit_flash--;
    if (p->respawn_t > 0) {                                      /* flying back in */
        p->respawn_t--;
        p->y -= FXF(2.2f);
        if (p->respawn_t == 0) p->y = FX(PFH - 60);
        if (p->invuln > 0) p->invuln--;
        return;
    }
    if (p->death_t > 0) {
        if (bomb_pressed && p->bombs > 0) { p->death_t = 0; do_bomb(); }
        else if (--p->death_t == 0) { player_die(); return; }
    }
    p->focus = in->focus;
    int dx = (in->right ? 1 : 0) - (in->left ? 1 : 0), dy = (in->down ? 1 : 0) - (in->up ? 1 : 0);
    fx speed = p->focus ? FXF(1.7f) : FXF(3.7f);
    if (dx && dy) speed = fx_mul(speed, FXF(0.7071f));
    p->x += dx * speed;
    p->y += dy * speed;
    p->move_dx = dx;
    if (p->x < FX(10)) p->x = FX(10);
    if (p->x > FX(PFW - 10)) p->x = FX(PFW - 10);
    if (p->y < FX(14)) p->y = FX(14);
    if (p->y > FX(PFH - 14)) p->y = FX(PFH - 14);
    if (p->shot_cd > 0) p->shot_cd--;
    if (in->shot && p->shot_cd == 0 && p->death_t == 0) {
        player_fire();
        p->shot_cd = 4;
    }
    if (bomb_pressed && p->bombs > 0 && p->bomb_t == 0 && p->death_t == 0) do_bomb();
    if (p->invuln > 0) p->invuln--;
    bomb_update();
}

/* ---------------------------------------------------------------- flow */

void game_start_stage(int stage) {
    G.stage = stage;
    G.state = GS_PLAY;
    G.stage_t = 0;
    G.stage_banner_t = 0;
    G.spell_banner_t = 1000;
    bullets_reset();
    enemies_reset();
    memset(G.it, 0, sizeof G.it);
    memset(G.ps, 0, sizeof G.ps);
    memset(G.fx, 0, sizeof G.fx);
    memset(&G.boss, 0, sizeof G.boss);
    G.p.x = FX(PFW / 2);
    G.p.y = FX(PFH - 60);
    G.p.invuln = 120;
    G.p.death_t = G.p.respawn_t = G.p.bomb_t = 0;
    stage_begin(stage);
}

void game_new(int mode, int diff, int shot, int stage, uint32_t seed) {
    Bullet *bl = G.bl;
    bool god = G.god, demo = G.demo, trace = G.trace;
    long hi = G.hiscore;
    memset(&G, 0, sizeof G);
    G.bl = bl ? bl : calloc(MAX_BULLETS, sizeof(Bullet));
    G.god = god; G.demo = demo; G.trace = trace; G.hiscore = hi;
    G.mode = mode; G.diff = diff; G.shot = shot;
    kg_rng_seed(&G.rng, seed);
    G.p.lives = diff == DIFF_EASY ? 4 : diff == DIFF_LUNATIC ? 2 : 3;
    G.p.bombs = 3;
    G.p.power = mode == MODE_PRACTICE ? 100 + 100 * stage : 0;
    if (G.p.power > MAX_POWER) G.p.power = MAX_POWER;
    game_start_stage(stage);
}

void game_tick(const Input *in) {
    G.prev_in = G.in;
    G.in = *in;
    G.tick++;
    if (G.shake > 0) G.shake--;
    if (G.flash > 0) G.flash--;
    if (G.extend_t > 0) G.extend_t--;
    if (G.last_bonus_t > 0) G.last_bonus_t--;
    G.stage_banner_t++;
    G.spell_banner_t++;
    G.boss_name_t++;
    if (G.state == GS_GAME_OVER) {
        if (G.clear_t > 0) G.clear_t--;
        fx_update();
        bullets_update();
        return;
    }
    if (G.state == GS_ENDING) { fx_update(); return; }
    player_update();
    if (G.state == GS_GAME_OVER) return;
    if (G.state == GS_PLAY) stage_run(G.stage_t);
    enemies_update();
    boss_tick();
    pshots_update();
    bullets_update();
    items_update();
    fx_update();
    G.stage_t++;
    if (G.state == GS_STAGE_CLEAR && --G.clear_t <= 0) {
        bool last = G.stage + 1 >= N_STAGES;
        if (G.mode == MODE_NORMAL && !last) game_start_stage(G.stage + 1);
        else { G.state = GS_ENDING; G.clear_t = 0; }
    }
}
