/* The demo player (--demo): follows the targets, dodges what it can see coming and bombs as a last resort. It plays by the same
 * rules as a person (the same Input), so a demo run is a test of the whole simulation. */
#include "dm.h"
#include <string.h>

typedef struct Near { int x, y, vx, vy, r; } Near;      /* 1/16 pixel and 1/16 pixel per tick */

static const int DIR8[9][2] = { { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
static const int BULLET_R16[BK_COUNT] = { 42, 61, 136, 45, 45, 54, 51, 45 };

static int danger_at(const Near *nb, int n, int px, int py) {       /* px, py in 1/16 px */
    int danger = 0;
    for (int i = 0; i < n; i++) {
        for (int k = 1; k <= 3; k++) {
            int bx = nb[i].x + nb[i].vx * k, by = nb[i].y + nb[i].vy * k;
            int dx = bx - px, dy = by - py;
            int lim = nb[i].r + 27 + 40 + k * 10;                    /* hit radius plus a safety margin */
            if (iabs(dx) > lim || iabs(dy) > lim) continue;
            int d2 = dx * dx + dy * dy;
            if (d2 < lim * lim) danger += 2000 / (k + (d2 < (nb[i].r + 27) * (nb[i].r + 27) ? 0 : 1)) + (lim * lim - d2) / 64;
        }
    }
    return danger;
}

void bot_input(Input *in) {
    memset(in, 0, sizeof *in);
    Player *p = &G.p;
    in->shot = true;
    if (p->respawn_t > 0) return;
    int px16 = (int)(p->x >> 12), py16 = (int)(p->y >> 12);

    /* what is close */
    Near nb[96];
    int n = 0, closest = 1 << 30;
    for (int i = 0; i < G.nb && n < 96; i++) {
        const Bullet *b = &G.bl[i];
        if (b->flags & BF_GHOST) continue;
        int dx = (b->x >> 12) - px16, dy = (b->y >> 12) - py16;
        if (iabs(dx) > 64 * 16 || iabs(dy) > 64 * 16) continue;
        nb[n].x = b->x >> 12;
        nb[n].y = b->y >> 12;
        nb[n].vx = b->vx >> 12;
        nb[n].vy = b->vy >> 12;
        nb[n].r = BULLET_R16[b->kind];
        n++;
        int d = dx * dx + dy * dy;
        if (d < closest) closest = d;
    }
    bool focus = closest < 56 * 56 * 256 || (G.boss.active && G.boss.phase >= 0 && p->y > G.boss.y);
    in->focus = focus;

    /* where to be */
    int want_x = PFW / 2, want_y = PFH - 70;
    int best_d = 1 << 30;
    if (G.boss.active && G.boss.phase >= 0 && !G.boss.dying) { want_x = FXI(G.boss.x); }
    else {
        for (int i = 0; i < MAX_ENEMIES; i++) {
            const Enemy *e = &G.en[i];
            if (!e->active || e->y < FX(10) || e->y > p->y) continue;
            int d = (int)((p->y - e->y) >> 16) + iabs(FXI(e->x - p->x)) / 2;
            if (d < best_d) { best_d = d; want_x = FXI(e->x); }
        }
    }
    int item_pull = 0, nitems = 0, item_x = 0;
    for (int i = 0; i < MAX_ITEMS; i++) {                          /* fetch items that fall nearby */
        const Item *it = &G.it[i];
        if (!it->active || it->collect || it->type == IT_STAR) continue;
        nitems++;
        item_x += FXI(it->x);
        int dy = FXI(it->y - p->y), dx = FXI(it->x - p->x);
        if (!item_pull && dy > -10 && dy < 120 && iabs(dx) < 90 && (it->type == IT_BOMB || it->type == IT_LIFE || it->type == IT_POWER_BIG || it->type == IT_POWER)) {
            want_x = FXI(it->x);
            want_y = FXI(it->y) + 6;
            item_pull = 1;
        }
    }
    /* when the field is calm and there are items about, fly above the collection line and let them come */
    bool calm = closest > 100 * 100 * 256;
    if (G.bot_poc > 0) {
        if (--G.bot_poc == 0 || nitems == 0 || !calm || G.boss.active) G.bot_poc = -300;          /* rest before the next trip */
    } else if (G.bot_poc < 0) G.bot_poc++;
    else if (nitems >= 3 && calm && p->y > FX(160) && !G.boss.active) G.bot_poc = 200;
    if (G.bot_poc > 0) { want_y = 90; want_x = nitems ? item_x / nitems : want_x; item_pull = 1; }
    if (G.boss.active && G.boss.phase >= 0 && !item_pull) want_y = PFH - 90;
    if (G.p.power >= 300 && !item_pull && G.boss.active && G.boss.hp < G.boss.maxhp / 3) want_y = PFH - 130;

    /* choose the safest step */
    fx speed = focus ? FXF(1.7f) : FXF(3.7f);
    int best = 0, best_cost = 1 << 30, best_danger = 0;
    int stay_danger = 0;
    for (int c = 0; c < 9; c++) {
        int dx = DIR8[c][0], dy = DIR8[c][1];
        fx sp = (dx && dy) ? fx_mul(speed, FXF(0.7071f)) : speed;
        fx nx = p->x + dx * sp, ny = p->y + dy * sp;
        if (nx < FX(10)) nx = FX(10);
        if (nx > FX(PFW - 10)) nx = FX(PFW - 10);
        if (ny < FX(G.bot_poc > 0 ? 60 : PFH / 2)) ny = FX(G.bot_poc > 0 ? 60 : PFH / 2);
        if (ny > FX(PFH - 14)) ny = FX(PFH - 14);
        int danger = danger_at(nb, n, (int)(nx >> 12), (int)(ny >> 12));
        if (c == 0) stay_danger = danger;
        int cost = danger * 8 + iabs(FXI(nx) - want_x) * 3 + iabs(FXI(ny) - want_y) * 2;
        /* walls and corners are traps */
        if (FXI(nx) < 40 || FXI(nx) > PFW - 40) cost += 40;
        if (cost < best_cost) { best_cost = cost; best = c; best_danger = danger; }
    }
    in->left = DIR8[best][0] < 0;
    in->right = DIR8[best][0] > 0;
    in->up = DIR8[best][1] < 0;
    in->down = DIR8[best][1] > 0;
    /* hopeless: use a bomb */
    if ((best_danger > 3500 || (stay_danger > 6000 && best_danger > 2200)) && p->bombs > 0 && p->bomb_t == 0 && p->invuln == 0 && !G.god) in->bomb = true;
    if (p->death_t > 0 && p->bombs > 0) in->bomb = true;                    /* the deathbomb window */
}
