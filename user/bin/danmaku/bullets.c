/* The bullet pool, its behaviours (accelerating, curving, stop-and-go, splitting, bouncing) and the pattern library. */
#include "dm.h"
#include <string.h>

/* hit radius of every shape plus the radius of the player's hitbox (1.7 px), in 1/16 pixel, squared */
static const int HIT_R16[BK_COUNT] = { 42, 61, 136, 45, 45, 54, 51, 45 };
#define PLAYER_R16 27
#define GRAZE_R16  (13 * 16)

static inline bool is_directional(int kind) { return kind == BK_RICE || kind == BK_KUNAI || kind == BK_BILL || kind == BK_BEAM; }

void bullets_reset(void) {
    G.nb = 0;
}

Bullet *bullet_add_v(fx x, fx y, fx vx, fx vy, int kind, int color) {
    if (G.nb >= MAX_BULLETS) return NULL;
    Bullet *b = &G.bl[G.nb++];
    memset(b, 0, sizeof *b);
    b->x = x;
    b->y = y;
    b->vx = vx;
    b->vy = vy;
    b->kind = (uint8_t)kind;
    b->color = (uint8_t)color;
    b->beh = BH_LINEAR;
    if (is_directional(kind)) { b->flags |= BF_DIR; b->ang = (uint16_t)fx_atan2(vy, vx); }
    if (G.nb > G.bullets_peak) G.bullets_peak = G.nb;
    return b;
}

Bullet *bullet_add(fx x, fx y, int angle, fx speed, int kind, int color) {
    Bullet *b = bullet_add_v(x, y, fx_mul(fx_cos(angle), speed), fx_mul(fx_sin(angle), speed), kind, color);
    if (b) { b->ang = (uint16_t)(angle & (ANG - 1)); b->spd = (uint16_t)(speed >> 8); }
    return b;
}

void bullet_accel(Bullet *b, int angle, fx accel, int ticks) {
    if (!b) return;
    b->beh = BH_ACCEL;
    b->ax = (int16_t)fx_mul(fx_cos(angle), accel);
    b->ay = (int16_t)fx_mul(fx_sin(angle), accel);
    b->timer = (int16_t)ticks;
}

void bullet_gravity(Bullet *b, fx g) {
    if (!b) return;
    b->beh = BH_ACCEL;
    b->ax = 0;
    b->ay = (int16_t)g;
    b->timer = -1;
}

void bullet_curve(Bullet *b, int turn, int ticks) {
    if (!b) return;
    b->beh = BH_CURVE;
    b->arg = (int16_t)turn;
    b->timer = (int16_t)ticks;
}

void bullet_stopgo(Bullet *b, int stop_after, fx go_speed, int angle_or_aim) {
    if (!b) return;
    b->beh = BH_STOPGO;
    b->timer = (int16_t)stop_after;
    b->arg = (int16_t)angle_or_aim;
    b->spd = (uint16_t)(go_speed >> 8);
}

void bullet_split(Bullet *b, int after, int n, int kind, int color, fx speed) {
    if (!b) return;
    b->beh = BH_SPLIT;
    b->timer = (int16_t)after;
    b->arg = (int16_t)(n | (kind << 8) | (color << 12));        /* n < 256, kind < 16, color < 16 */
    b->spd = (uint16_t)(speed >> 8);
}

void bullet_delay(Bullet *b, int ticks) {
    if (!b) return;
    b->flags |= BF_GHOST;
    b->timer = (int16_t)ticks;                                  /* the velocity simply waits: ghosts do not move */
    b->beh = BH_DELAY;
}

void bullet_bounce(Bullet *b, int times) {
    if (!b) return;
    b->flags |= BF_BOUNCE;
    b->arg = (int16_t)times;
}

int aim_angle_to(fx x, fx y, fx tx, fx ty) { return fx_atan2(ty - y, tx - x); }
int aim_angle(fx x, fx y) { return aim_angle_to(x, y, G.p.x, G.p.y); }

/* ---------------------------------------------------------------- per-tick update */

static void remove_at(int i) {
    G.bl[i] = G.bl[--G.nb];
}

void bullets_update(void) {
    Bullet *bl = G.bl;
    const fx px = G.p.x, py = G.p.y;
    bool can_graze = G.p.death_t == 0 && G.p.respawn_t == 0;
    bool can_hit = can_graze && G.p.invuln == 0 && !G.god;
    for (int i = 0; i < G.nb; i++) {
        Bullet *b = &bl[i];
        switch (b->beh) {
        case BH_LINEAR:
            break;
        case BH_ACCEL:
            if (b->timer != 0) {
                b->vx += b->ax;
                b->vy += b->ay;
                if (b->timer > 0) b->timer--;
                if (b->flags & BF_DIR) b->ang = (uint16_t)fx_atan2(b->vy, b->vx);
            }
            break;
        case BH_CURVE:
            if (b->timer != 0) {
                b->ang = (uint16_t)((b->ang + b->arg) & (ANG - 1));
                b->vx = (fx)(((int64_t)fx_cos(b->ang) * b->spd) >> 8);
                b->vy = (fx)(((int64_t)fx_sin(b->ang) * b->spd) >> 8);
                if (b->timer > 0) b->timer--;
            }
            break;
        case BH_STOPGO:
            if (b->timer > 0) {
                b->vx -= b->vx >> 4;                           /* brakes */
                b->vy -= b->vy >> 4;
                if (--b->timer == 0) {
                    int a = b->arg < 0 ? aim_angle(b->x, b->y) : b->arg;
                    b->ang = (uint16_t)a;
                    b->vx = b->vy = 0;
                    b->beh = BH_ACCEL;
                    fx sp = (fx)b->spd << 8;
                    b->ax = (int16_t)(fx_mul(fx_cos(a), sp) / 24);
                    b->ay = (int16_t)(fx_mul(fx_sin(a), sp) / 24);
                    b->timer = 24;
                }
            }
            break;
        case BH_SPLIT:
            if (--b->timer <= 0) {
                int n = b->arg & 255, kind = (b->arg >> 8) & 15, color = (b->arg >> 12) & 15;
                fx sp = (fx)b->spd << 8;
                int a0 = (G.tick * 7) & (ANG - 1);
                for (int k = 0; k < n; k++) bullet_add(b->x, b->y, a0 + k * ANG / n, sp, kind, color);
                fx_puff(b->x, b->y, b->color, 3);
                remove_at(i--);
                continue;
            }
            break;
        case BH_DELAY:
            if (--b->timer <= 0) {
                b->flags &= (uint8_t)~BF_GHOST;
                b->beh = BH_LINEAR;
            }
            break;
        default:
            break;
        }
        if (b->flags & BF_GHOST) continue;                       /* waiting to launch: no movement, no collision */
        b->x += b->vx;
        b->y += b->vy;
        if (b->flags & BF_BOUNCE) {
            if (b->x < 0) { b->x = -b->x; b->vx = -b->vx; b->arg--; }
            else if (b->x > FX(PFW)) { b->x = FX(2 * PFW) - b->x; b->vx = -b->vx; b->arg--; }
            if (b->y < 0) { b->y = -b->y; b->vy = -b->vy; b->arg--; }
            if (b->flags & BF_DIR) b->ang = (uint16_t)fx_atan2(b->vy, b->vx);
            if (b->arg <= 0) b->flags &= (uint8_t)~BF_BOUNCE;
        }
        int margin = b->kind == BK_BIG ? 48 : 28;
        if (b->x < FX(-margin) || b->x > FX(PFW + margin) || b->y < FX(-margin) || b->y > FX(PFH + margin)) { remove_at(i--); continue; }
        /* the player */
        int dx = (b->x - px) >> 12, dy = (b->y - py) >> 12;
        if (dx > GRAZE_R16 + 160 || dx < -GRAZE_R16 - 160 || dy > GRAZE_R16 + 160 || dy < -GRAZE_R16 - 160) continue;
        int d2 = dx * dx + dy * dy;
        int hr = HIT_R16[b->kind] + PLAYER_R16;
        if (can_hit && d2 < hr * hr) {
            can_hit = false;
            player_hit();
            remove_at(i--);
            continue;
        }
        if (can_graze && !(b->flags & BF_GRAZED)) {
            int gr = GRAZE_R16 + HIT_R16[b->kind] - 20;
            if (d2 < gr * gr) {
                b->flags |= BF_GRAZED;
                G.graze++;
                add_score(500);
                fx_spark(b->x, b->y, BC_WHITE, 1);
            }
        }
    }
}

/* ---------------------------------------------------------------- clearing */

void bullets_clear(bool to_items) {
    int every = 0;
    for (int i = 0; i < G.nb; i++) {
        Bullet *b = &G.bl[i];
        if (to_items && (every++ % 2) == 0 && b->kind != BK_PELLET) item_spawn(IT_STAR, b->x, b->y, 0);
    }
    G.nb = 0;
}

void bullets_clear_circle(fx cx, fx cy, int radius, bool to_items) {
    int r16 = radius * 16, every = 0;
    for (int i = 0; i < G.nb; i++) {
        Bullet *b = &G.bl[i];
        int dx = (b->x - cx) >> 12, dy = (b->y - cy) >> 12;
        if (iabs(dx) > r16 || iabs(dy) > r16) continue;
        if (dx * dx + dy * dy > r16 * r16) continue;
        if (to_items && (every++ % 3) == 0) item_spawn(IT_STAR, b->x, b->y, 0);
        remove_at(i--);
    }
}

void bullets_clear_column(fx x0, fx x1, bool to_items) {
    int every = 0;
    for (int i = 0; i < G.nb; i++) {
        Bullet *b = &G.bl[i];
        if (b->x < x0 || b->x > x1) continue;
        if (to_items && (every++ % 3) == 0) item_spawn(IT_STAR, b->x, b->y, 0);
        remove_at(i--);
    }
}

/* ---------------------------------------------------------------- pattern library */

int pat_ring(fx x, fx y, int n, int a0, fx speed, int kind, int color) {
    int c = 0;
    for (int i = 0; i < n; i++)
        if (bullet_add(x, y, a0 + i * ANG / n, speed, kind, color)) c++;
    return c;
}

int pat_fan(fx x, fx y, int n, int center, int spread, fx speed, int kind, int color) {
    int c = 0;
    for (int i = 0; i < n; i++) {
        int a = n == 1 ? center : center - spread / 2 + i * spread / (n - 1);
        if (bullet_add(x, y, a, speed, kind, color)) c++;
    }
    return c;
}

int pat_aimed(fx x, fx y, int n, int spread, fx speed, int kind, int color) {
    return pat_fan(x, y, n, aim_angle(x, y), spread, speed, kind, color);
}

/* A ring whose speed follows 1 + depth * cos(petals * angle): the bullets spread out into a flower with `petals` petals. */
int pat_flower(fx x, fx y, int n, int petals, int a0, fx speed, int depth256, int kind, int color) {
    int c = 0;
    for (int i = 0; i < n; i++) {
        int th = i * ANG / n;
        int mod = 256 + (int)(((int64_t)fx_cos(th * petals) * depth256) >> 16);
        fx sp = (fx)(((int64_t)speed * mod) >> 8);
        if (bullet_add(x, y, a0 + th, sp, kind, color)) c++;
    }
    return c;
}

void pat_beam_step(fx x, fx y, int angle, fx speed, int color) {
    bullet_add(x, y, angle, speed, BK_BEAM, color);
}
