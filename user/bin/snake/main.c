/*
 * snake: Eat the dango. A smooth-moving snake in a lantern garden.
 *
 *   arrows / WASD   steer           Space / P / Esc   pause          F3   frame rate
 *   Enter           start / again   M                 mode (walls or wrap-around)
 *
 * Command line: --selftest   --seed N   --wrap   --level N   --start (skip the title)   --bot (autopilot)   --trace
 */
#include <kappa.h>
#include <kappa_gfx2d.h>
#include <kappa_gamekit.h>
#include "logic.h"
#include "art.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int sn_selftest(void);

#define SCORE_FILE "snake.scores"
#define MAX_PARTS 160

enum { S_LOADING, S_TITLE, S_READY, S_PLAY, S_PAUSE, S_DYING, S_OVER, S_SCORES };
enum { M_START, M_MODE, M_LEVEL, M_SCORES, M_QUIT, M_COUNT };

typedef struct Part { int x, y, vx, vy; int life, max; uint8_t kind; } Part;     /* positions in 1/16 pixel */
typedef struct Popup { int x, y, life; char text[12]; uint32_t col; } Popup;

typedef struct App {
    KApp *app;
    SArt art;
    int load_stage;
    int state;
    SnakeGame sg;
    KStepper step;
    KFps fps;
    bool show_fps;
    bool trace, bot, skip_title, fixed_seed;
    int bot_limit;                      /* the autopilot stops steering after this many dango (0 = never) */
    uint32_t seed, games_started;
    /* menu */
    int sel;
    bool wrap;
    int start_level;
    KSprite *menu_item[M_COUNT][2];
    bool menu_stale;
    int score_tab;
    KScoreTable scores;
    /* effects */
    Part parts[MAX_PARTS];
    Popup popups[6];
    int shake, flash;
    uint64_t state_since_ms, banner_until_ms;
    char banner[24];
    KSprite *banner_sprite;
    int ticks;
    uint64_t tongue_ms;
    /* screen bookkeeping */
    bool hud_dirty, full_redraw, overlay_drawn;
    long shown_score;
    int shown_level, shown_len, shown_best;
    long best;
    bool name_asked, new_record;
    KRng fx_rng;
} App;

static App A;

static void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void trace(const char *fmt, ...) {
    if (!A.trace) return;
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("snake: %s\n", buf);
    fflush(stdout);
}

/* ---------------------------------------------------------------- particles */

static void spawn_part(int x, int y, int vx, int vy, int life, int kind) {
    for (int i = 0; i < MAX_PARTS; i++)
        if (A.parts[i].life <= 0) { A.parts[i] = (Part){ x, y, vx, vy, life, life, (uint8_t)kind }; return; }
}

static void burst(int cx, int cy, int n, int kind, int speed) {
    for (int i = 0; i < n; i++) {
        int a = kg_rng_range(&A.fx_rng, 360);
        float rad = (float)a * (float)M_PI / 180.f;
        int sp = speed / 2 + kg_rng_range(&A.fx_rng, speed);
        spawn_part(cx * 16, cy * 16, (int)(cosf(rad) * (float)sp), (int)(sinf(rad) * (float)sp) - 10, 20 + kg_rng_range(&A.fx_rng, 20), kind);
    }
}

static void add_popup(int x, int y, const char *text, uint32_t col) {
    for (int i = 0; i < 6; i++)
        if (A.popups[i].life <= 0) {
            A.popups[i].x = x;
            A.popups[i].y = y;
            A.popups[i].life = 40;
            A.popups[i].col = col;
            snprintf(A.popups[i].text, sizeof A.popups[i].text, "%s", text);
            return;
        }
}

static void update_parts(void) {
    for (int i = 0; i < MAX_PARTS; i++) {
        Part *p = &A.parts[i];
        if (p->life <= 0) continue;
        p->x += p->vx;
        p->y += p->vy;
        if (p->kind == 0) { p->vy += 1; p->vx += (A.ticks + i) % 7 == 0 ? 2 : -(A.ticks + i) % 3 == 0; }
        else p->vy += 2;
        p->life--;
    }
    for (int i = 0; i < 6; i++) if (A.popups[i].life > 0) { A.popups[i].life--; A.popups[i].y -= 1; }
}

static void draw_parts(KCanvas *cv, int ox, int oy) {
    for (int i = 0; i < MAX_PARTS; i++) {
        Part *p = &A.parts[i];
        if (p->life <= 0) continue;
        int x = ox + (p->x >> 4), y = oy + (p->y >> 4);
        if (p->kind == 0) ks_blit_key(cv, x - 3, y - 2, A.art.petal);
        else {
            int s = p->life > 24 ? 2 : p->life > 12 ? 1 : 0;
            ks_blit_key(cv, x - A.art.spark[s]->w / 2, y - A.art.spark[s]->h / 2, A.art.spark[s]);
        }
    }
    for (int i = 0; i < 6; i++) {
        Popup *p = &A.popups[i];
        if (p->life <= 0) continue;
        kc_text_shadow(cv, ox + p->x - (int)strlen(p->text) * 4, oy + p->y, p->text, p->col, 0x101820);
    }
}

/* ---------------------------------------------------------------- game flow */

static void set_state(int st) {
    trace("state %d -> %d", A.state, st);
    A.state = st;
    A.state_since_ms = k_now_ms();
    A.overlay_drawn = false;
    A.full_redraw = true;
    A.hud_dirty = true;
    if (st == S_TITLE) A.menu_stale = true;
}

static void show_banner(const char *text, int ms) {
    snprintf(A.banner, sizeof A.banner, "%s", text);
    A.banner_until_ms = k_now_ms() + (uint64_t)ms;
    ks_free(A.banner_sprite);
    A.banner_sprite = ks_text(text, 3, 0xFFF4C8, 0x3A1030);
}

static void start_game(void) {
    uint32_t seed = A.fixed_seed ? A.seed + A.games_started : kg_seed_from_time();
    A.games_started++;
    sg_new(&A.sg, A.wrap, A.start_level, seed);
    kg_step_init(&A.step, sg_step_ms(&A.sg) * 1000, 4);
    memset(A.parts, 0, sizeof A.parts);
    memset(A.popups, 0, sizeof A.popups);
    A.best = kg_scores_best(&A.scores, A.wrap ? 1 : 0, 0);
    A.name_asked = A.new_record = false;
    A.shake = A.flash = 0;
    set_state(S_READY);
    show_banner("Ready?", 900);
    trace("new game wrap=%d level=%d seed=%u", A.wrap, A.start_level, (unsigned)seed);
}

static void name_cb(const char *name, void *user) {
    (void)user;
    if (name && *name) {
        int rank = kg_scores_add(&A.scores, A.sg.wrap ? 1 : 0, A.sg.score, name, A.sg.start_level, A.sg.level);
        kg_scores_save(&A.scores, SCORE_FILE);
        A.new_record = rank >= 0;
        A.overlay_drawn = false;
        kapp_redraw(A.app);
        trace("high score saved: %ld rank %d name %s", A.sg.score, rank, name);
    }
}

static void game_over(void) {
    set_state(S_OVER);
    trace("game over: score %ld level %d length %d death %d", A.sg.score, A.sg.level, A.sg.len, A.sg.death);
    if (!A.name_asked && A.sg.score > 0 && (!A.bot || A.bot_limit) && kg_scores_rank(&A.scores, A.sg.wrap ? 1 : 0, A.sg.score) >= 0) {
        A.name_asked = true;
        const char *user = getenv("USER");
        k_input_dialog(A.app, "High score", "A new high score! Your name:", user && *user ? user : "Reimu", name_cb, NULL);
    }
}

static void on_step_events(long score_before) {
    SnakeGame *g = &A.sg;
    if (g->ev_eat) {
        int px = (int)g->ev_eat_at.x * CELL + CELL / 2, py = (int)g->ev_eat_at.y * CELL + CELL / 2;
        burst(px, py, g->ev_eat == EAT_GOLD ? 18 : 9, g->ev_eat == EAT_GOLD ? 1 : 0, g->ev_eat == EAT_GOLD ? 40 : 28);
        char t[16];
        snprintf(t, sizeof t, "+%ld", g->score - score_before);
        add_popup(px, py - 10, t, g->ev_eat == EAT_GOLD ? 0xFFE27A : 0xFFFFFF);
        trace("ate %s: score %ld length %d level %d", g->ev_eat == EAT_GOLD ? "gold dango" : "dango", g->score, g->len, g->level);
    }
    if (g->ev_level_up) {
        char t[24];
        snprintf(t, sizeof t, "Level %d", g->level);
        show_banner(t, 1100);
        kg_step_set(&A.step, sg_step_ms(g) * 1000);
        trace("level %d (step %d ms, %d stones)", g->level, sg_step_ms(g), g->n_obst);
    }
    if (g->ev_gold_spawn) trace("a golden dango appeared");
    if (g->status == SS_DEAD) {
        A.shake = 10;
        A.flash = 8;
        set_state(S_DYING);
        burst(g->body[0].x * CELL + CELL / 2, g->body[0].y * CELL + CELL / 2, 16, 1, 50);
    }
}

/* ---------------------------------------------------------------- drawing: the field */

typedef struct Vec { float x, y; } Vec;

static void draw_disc_chain(KCanvas *cv, int ox, int oy, const Vec *v, int n, float total, bool hurt) {
    /* three passes: outlines, bodies in two tones, highlights; the discs sit 3.5 px apart along the path */
    for (int pass = 0; pass < 3; pass++) {
        float spacing = 3.5f, dist_head = 0.f, carry = 0.f;
        for (int i = 0; i + 1 < n; i++) {
            float dx = v[i + 1].x - v[i].x, dy = v[i + 1].y - v[i].y;
            float len = __builtin_sqrtf(dx * dx + dy * dy);
            if (len < 0.01f) continue;
            float t = carry;
            for (; t < len; t += spacing) {
                float x = v[i].x + dx * t / len, y = v[i].y + dy * t / len;
                float from_tail = total - (dist_head + t);
                float taper = total * 0.7f < 100.f ? total * 0.7f : 100.f;      /* the last stretch narrows to a point */
                int ri = 0;
                if (from_tail < taper && taper > 1.f) { ri = (int)((taper - from_tail) * (float)NRAD / taper); if (ri > NRAD - 1) ri = NRAD - 1; }
                if (from_tail < 0.f) ri = NRAD - 1;
                int tone = (int)((dist_head + t) / 15.f) & 1;
                const KSprite *s = pass == 0 ? A.art.outline[ri] : pass == 1 ? A.art.fill[hurt][tone][ri] : A.art.shine[ri];
                int ix = ox + (int)(x + 0.5f) - s->w / 2, iy = oy + (int)(y + 0.5f) - s->h / 2;
                if (pass == 2) ks_blit(cv, ix, iy, s); else ks_blit_key(cv, ix, iy, s);
            }
            carry = t - len;
            dist_head += len;
        }
    }
}

static void draw_snake(KCanvas *cv, int ox, int oy, int alpha256, bool hurt) {
    SnakeGame *g = &A.sg;
    const float W = (float)FIELD_W, H = (float)FIELD_H;
    Vec v[SN_CELLS + 3];
    int n = 0;
    float a = (float)alpha256 / 256.f;
    if (g->status != SS_PLAYING) a = 1.f;
    /* head position: slides from the previous head cell into the new one (the very first frame has no previous cell) */
    float hx = (float)g->body[0].x * CELL + CELL / 2, hy = (float)g->body[0].y * CELL + CELL / 2;
    Vec c1 = { (float)g->body[1].x * CELL + CELL / 2, (float)g->body[1].y * CELL + CELL / 2 };
    /* unwrap the neighbouring cells so that the path is continuous across the wrap-around edges */
    Vec pts[SN_CELLS + 3];
    int np = 0;
    pts[np++] = (Vec){ hx, hy };
    for (int i = 1; i < g->len; i++) {
        Vec p = { (float)g->body[i].x * CELL + CELL / 2, (float)g->body[i].y * CELL + CELL / 2 };
        Vec q = pts[np - 1];
        if (p.x - q.x > W / 2) p.x -= W;
        if (p.x - q.x < -W / 2) p.x += W;
        if (p.y - q.y > H / 2) p.y -= H;
        if (p.y - q.y < -H / 2) p.y += H;
        pts[np++] = p;
    }
    (void)c1;
    Vec head = pts[0], next = np > 1 ? pts[1] : pts[0];
    /* head_vis: from the second cell towards the head */
    Vec hv = { next.x + (head.x - next.x) * a, next.y + (head.y - next.y) * a };
    v[n++] = hv;
    for (int i = 1; i < np; i++) v[n++] = pts[i];
    /* the tail tip slides from the cell it left towards the last cell (unless the snake just grew) */
    if (!g->grew && g->status == SS_PLAYING && g->steps > 0) {
        Vec last = pts[np - 1];
        Vec old = { (float)g->old_tail.x * CELL + CELL / 2, (float)g->old_tail.y * CELL + CELL / 2 };
        if (old.x - last.x > W / 2) old.x -= W;
        if (old.x - last.x < -W / 2) old.x += W;
        if (old.y - last.y > H / 2) old.y -= H;
        if (old.y - last.y < -H / 2) old.y += H;
        v[n++] = (Vec){ last.x + (old.x - last.x) * (1.f - a), last.y + (old.y - last.y) * (1.f - a) };
    }
    float total = 0.f;
    for (int i = 0; i + 1 < n; i++) {
        float dx = v[i + 1].x - v[i].x, dy = v[i + 1].y - v[i].y;
        total += __builtin_sqrtf(dx * dx + dy * dy);
    }
    /* draw the path, and shifted copies when it crosses the border of the wrap-around field */
    float minx = v[0].x, maxx = v[0].x, miny = v[0].y, maxy = v[0].y;
    for (int i = 1; i < n; i++) {
        if (v[i].x < minx) minx = v[i].x;
        if (v[i].x > maxx) maxx = v[i].x;
        if (v[i].y < miny) miny = v[i].y;
        if (v[i].y > maxy) maxy = v[i].y;
    }
    for (int sy = -1; sy <= 1; sy++)
        for (int sx = -1; sx <= 1; sx++) {
            float offx = (float)sx * W, offy = (float)sy * H;
            if (maxx + offx < -20.f || minx + offx > W + 20.f || maxy + offy < -20.f || miny + offy > H + 20.f) continue;
            if ((sx || sy) && !g->wrap) continue;
            Vec shifted[SN_CELLS + 3];
            for (int i = 0; i < n; i++) shifted[i] = (Vec){ v[i].x + offx, v[i].y + offy };
            draw_disc_chain(cv, ox, oy, shifted, n, total, hurt);
            /* the head */
            bool tongue = ((k_now_ms() - A.tongue_ms) / 180) % 6 == 0;
            const KSprite *hs = g->status == SS_PLAYING ? A.art.head[g->dir][tongue] : A.art.head_dead[g->dir];
            ks_blit_key(cv, ox + (int)(shifted[0].x + 0.5f) - hs->w / 2, oy + (int)(shifted[0].y + 0.5f) - hs->h / 2, hs);
        }
}

static void draw_field(KCanvas *cv, KUI *ui, int alpha256) {
    SnakeGame *g = &A.sg;
    (void)ui;
    int sx = 0, sy = 0;
    if (A.shake > 0) { sx = (A.ticks % 3 - 1) * (A.shake > 4 ? 3 : 2); sy = ((A.ticks / 2) % 3 - 1) * (A.shake > 4 ? 3 : 2); }
    KRect field = kr(0, HUD_H, FIELD_W, FIELD_H);
    kc_set_clip(cv, field);
    int ox = sx, oy = HUD_H + sy;
    /* the garden (rows are copied in two pieces when the picture is shifted by a shake) */
    KCanvas floor_cv = { .px = A.art.floor->px, .w = A.art.floor->w, .h = A.art.floor->h, .stride = A.art.floor->w, .clip = kr(0, 0, A.art.floor->w, A.art.floor->h) };
    ks_copy_rect(cv, 0, HUD_H, &floor_cv, kr(8 - sx, 8 - sy, FIELD_W, FIELD_H));
    uint64_t now = k_now_ms();
    /* border */
    const KSprite *border = g->wrap ? A.art.portal[(now / 250) & 1] : A.art.fence;
    ks_blit_part_key(cv, ox, oy, border, kr(0, 0, FIELD_W, 12));
    ks_blit_part_key(cv, ox, oy + FIELD_H - 12, border, kr(0, FIELD_H - 12, FIELD_W, 12));
    ks_blit_part_key(cv, ox, oy + 12, border, kr(0, 12, 12, FIELD_H - 24));
    ks_blit_part_key(cv, ox + FIELD_W - 12, oy + 12, border, kr(FIELD_W - 12, 12, 12, FIELD_H - 24));
    /* stones, with a soft lantern glow at night */
    for (int y = 0; y < SN_H; y++)
        for (int x = 0; x < SN_W; x++)
            if (g->obst[sg_idx(x, y)]) ks_blit_key(cv, ox + x * CELL, oy + y * CELL, A.art.stone);
    /* dango bob up and down */
    int bob = (int)(sinf((float)now * 0.006f) * 2.f);
    if (g->food.x >= 0) ks_blit_key(cv, ox + g->food.x * CELL, oy + g->food.y * CELL + bob, A.art.dango);
    if (g->gold_ttl > 0) {
        bool blink = g->gold_ttl < 16 && (now / 120) & 1;
        if (!blink) {
            ks_blit_at(cv, ox + g->gold.x * CELL + CELL / 2, oy + g->gold.y * CELL + CELL / 2, A.art.gold_halo);
            ks_blit_key(cv, ox + g->gold.x * CELL, oy + g->gold.y * CELL + bob, A.art.gold);
            int ph = (int)(now / 90);
            for (int i = 0; i < 3; i++) {
                int a = (ph + i * 5) % 12;
                if (a > 2) continue;
                ks_blit_key(cv, ox + g->gold.x * CELL + 3 + i * 8, oy + g->gold.y * CELL + 2 + (i * 7) % 15, A.art.spark[a]);
            }
        }
    }
    bool hurt = A.state == S_DYING && (A.flash > 0) && (A.flash & 1);
    draw_snake(cv, ox, oy, alpha256, hurt);
    draw_parts(cv, ox, oy);
    if (A.flash > 5) ks_add_rect(cv, field, 0x301010);
    /* banner */
    if (A.banner_sprite && now < A.banner_until_ms) {
        int left = (int)(A.banner_until_ms - now);
        int fade = left < 250 ? left * 256 / 250 : 256;
        ks_blit_fade(cv, (FIELD_W - A.banner_sprite->w) / 2, HUD_H + FIELD_H / 2 - A.banner_sprite->h / 2 - 20, A.banner_sprite, fade);
    }
    kc_set_clip(cv, kr(0, 0, cv->w, cv->h));
}

static void draw_hud(KCanvas *cv) {
    SnakeGame *g = &A.sg;
    kc_gradient_v(cv, kr(0, 0, WIN_W, HUD_H), 0x161C46, 0x0A0E26);
    kc_hline(cv, 0, HUD_H - 2, WIN_W, 0xD9A441);
    kc_hline(cv, 0, HUD_H - 1, WIN_W, 0x5A4418);
    char buf[40];
    int x = 12;
    kc_text_shadow(cv, x, 12, "SCORE", 0xD9A441, 0x000000);
    snprintf(buf, sizeof buf, "%ld", g->score);
    kc_text_shadow(cv, x + 48, 12, buf, 0xFFFFFF, 0x000000);
    x = 158;
    kc_text_shadow(cv, x, 12, "BEST", 0xD9A441, 0x000000);
    snprintf(buf, sizeof buf, "%ld", A.best > g->score ? A.best : g->score);
    kc_text_shadow(cv, x + 40, 12, buf, 0xFFFFFF, 0x000000);
    x = 290;
    kc_text_shadow(cv, x, 12, "LEVEL", 0xD9A441, 0x000000);
    snprintf(buf, sizeof buf, "%d", g->level);
    kc_text_shadow(cv, x + 48, 12, buf, 0xFFFFFF, 0x000000);
    x = 384;
    kc_text_shadow(cv, x, 12, "LENGTH", 0xD9A441, 0x000000);
    snprintf(buf, sizeof buf, "%d", g->len);
    kc_text_shadow(cv, x + 56, 12, buf, 0xFFFFFF, 0x000000);
    kc_text_shadow(cv, WIN_W - 12 - (g->wrap ? 4 : 5) * 8, 12, g->wrap ? "WRAP" : "WALLS", g->wrap ? 0x7FD0FF : 0xFF8090, 0x000000);
    A.shown_score = g->score;
    A.shown_level = g->level;
    A.shown_len = g->len;
    A.shown_best = (int)A.best;
    A.hud_dirty = false;
}

/* ---------------------------------------------------------------- drawing: title, overlays */

static void rebuild_menu(void) {
    for (int i = 0; i < M_COUNT; i++)
        for (int h = 0; h < 2; h++) {
            ks_free(A.menu_item[i][h]);
            char t[48];
            switch (i) {
            case M_START: snprintf(t, sizeof t, "Start game"); break;
            case M_MODE: snprintf(t, sizeof t, "Mode: %s", A.wrap ? "Wrap-around" : "Walls"); break;
            case M_LEVEL: snprintf(t, sizeof t, "Start level: %d", A.start_level); break;
            case M_SCORES: snprintf(t, sizeof t, "High scores"); break;
            default: snprintf(t, sizeof t, "Quit"); break;
            }
            A.menu_item[i][h] = ks_text(t, 2, h ? 0xFFF4C8 : 0xC6CCF0, h ? 0x7A1030 : 0x0C1030);
        }
    A.menu_stale = false;
}

static KRect menu_rect(int i) {
    KSprite *s = A.menu_item[i][0];
    return kr((WIN_W - s->w) / 2 - 4, 214 + i * 42, s->w + 8, 38);
}

static void draw_title(KCanvas *cv, KUI *ui) {
    ks_copy_rect(cv, 0, 0, &(KCanvas){ .px = A.art.title_bg->px, .w = WIN_W, .h = WIN_H, .stride = WIN_W, .clip = kr(0, 0, WIN_W, WIN_H) }, kr(0, 0, WIN_W, WIN_H));
    uint64_t now = k_now_ms();
    for (int i = 0; i < 40; i++) {                            /* twinkling stars */
        uint32_t r = (uint32_t)(i * 2654435761u);
        if (((now / 400) + (r >> 7)) % 5 != 0) continue;
        ks_blit_key(cv, (int)(r % WIN_W), (int)((r >> 12) % 200), A.art.spark[0]);
    }
    int bob = (int)(sinf((float)now * 0.003f) * 3.f);
    ks_blit_key(cv, (WIN_W - A.art.logo->w) / 2, 62 + bob, A.art.logo);
    ks_blit_key(cv, (WIN_W - A.art.logo_sub->w) / 2, 150, A.art.logo_sub);
    if (A.menu_stale) rebuild_menu();
    for (int i = 0; i < M_COUNT; i++) {
        KSprite *s = A.menu_item[i][A.sel == i];
        KRect r = menu_rect(i);
        if (A.sel == i) {
            ks_stipple_rect(cv, kr(r.x - 30, r.y, r.w + 60, r.h), 0x1B2A66, 0);
            ks_blit_key(cv, r.x - 28, r.y + 7, A.art.dango);
            ks_blit_key(cv, r.x + r.w + 4, r.y + 7, A.art.dango);
        }
        ks_blit_key(cv, (WIN_W - s->w) / 2, r.y + 3, s);
    }
    kc_text_shadow(cv, (WIN_W - 56 * 8) / 2, WIN_H - 40, "Arrows or WASD steer   Space pauses   F3 shows the frame rate", 0xC6CCF0, 0x000000);
    kc_text_shadow(cv, (WIN_W - 41 * 8) / 2, WIN_H - 20, "Up/Down choose   Left/Right change   Enter", 0x8A92C8, 0x000000);
    (void)ui;
}

static void draw_overlay_text(KCanvas *cv, const char *big, const char *line1, const char *line2, uint32_t col) {
    ks_shade_rect(cv, kr(0, HUD_H, FIELD_W, FIELD_H), 90);
    KSprite *t = ks_text(big, 4, col, 0x20102A);
    ks_blit_key(cv, (WIN_W - t->w) / 2, HUD_H + 120, t);
    ks_free(t);
    int y = HUD_H + 210;
    if (line1) { kc_text_shadow(cv, (WIN_W - (int)strlen(line1) * 8) / 2, y, line1, 0xFFFFFF, 0x000000); y += 26; }
    if (line2) kc_text_shadow(cv, (WIN_W - (int)strlen(line2) * 8) / 2, y + 12, line2, 0xC6CCF0, 0x000000);
}

static void draw_scores(KCanvas *cv) {
    ks_copy_rect(cv, 0, 0, &(KCanvas){ .px = A.art.title_bg->px, .w = WIN_W, .h = WIN_H, .stride = WIN_W, .clip = kr(0, 0, WIN_W, WIN_H) }, kr(0, 0, WIN_W, WIN_H));
    ks_shade_rect(cv, kr(40, 40, WIN_W - 80, WIN_H - 80), 70);
    kc_outline(cv, kr(40, 40, WIN_W - 80, WIN_H - 80), 0xD9A441);
    KSprite *t = ks_text("High scores", 3, 0xFFF4C8, 0x7A1030);
    ks_blit_key(cv, (WIN_W - t->w) / 2, 52, t);
    ks_free(t);
    char buf[80];
    snprintf(buf, sizeof buf, "< %s >", A.score_tab ? "Wrap-around" : "Walls");
    kc_text_shadow(cv, (WIN_W - (int)strlen(buf) * 8) / 2, 112, buf, 0x7FD0FF, 0x000000);
    const KScoreEntry *list[12];
    int n = kg_scores_list(&A.scores, A.score_tab, list, 10);
    if (!n) kc_text_shadow(cv, (WIN_W - 15 * 8) / 2, 200, "No scores yet.", 0xC6CCF0, 0x000000);
    for (int i = 0; i < n; i++) {
        snprintf(buf, sizeof buf, "%2d. %-12.12s %8ld   lvl %ld", i + 1, list[i]->name, list[i]->score, list[i]->extra[1]);
        kc_text_shadow(cv, 100, 148 + i * 24, buf, i == 0 ? 0xFFE27A : 0xFFFFFF, 0x000000);
    }
    kc_text_shadow(cv, (WIN_W - 36 * 8) / 2, WIN_H - 70, "Left/Right: mode    Esc or Enter: back", 0x8A92C8, 0x000000);
}

/* ---------------------------------------------------------------- input */

static int key_to_dir(uint8_t code) {
    switch (code) {
    case KEY_UP: case KEY_W: return DIR_UP;
    case KEY_RIGHT: case KEY_D: return DIR_RIGHT;
    case KEY_DOWN: case KEY_S: return DIR_DOWN;
    case KEY_LEFT: case KEY_A: return DIR_LEFT;
    default: return -1;
    }
}

static void menu_activate(void) {
    switch (A.sel) {
    case M_START: start_game(); break;
    case M_MODE: A.wrap = !A.wrap; A.menu_stale = true; break;
    case M_LEVEL: A.start_level = A.start_level % 9 + 1; A.menu_stale = true; break;
    case M_SCORES: A.score_tab = A.wrap ? 1 : 0; set_state(S_SCORES); break;
    default: kapp_quit(A.app, 0); break;
    }
}

static void handle_title_input(KUI *ui) {
    for (int i = 0; i < ui->nkeys; i++) {
        KKey *k = &ui->keys[i];
        if (!k->down) continue;
        switch (k->code) {
        case KEY_UP: case KEY_W: A.sel = (A.sel + M_COUNT - 1) % M_COUNT; break;
        case KEY_DOWN: case KEY_S: A.sel = (A.sel + 1) % M_COUNT; break;
        case KEY_LEFT: case KEY_A:
            if (A.sel == M_MODE) { A.wrap = !A.wrap; A.menu_stale = true; }
            if (A.sel == M_LEVEL) { A.start_level = (A.start_level + 7) % 9 + 1; A.menu_stale = true; }
            break;
        case KEY_RIGHT: case KEY_D:
            if (A.sel == M_MODE) { A.wrap = !A.wrap; A.menu_stale = true; }
            if (A.sel == M_LEVEL) { A.start_level = A.start_level % 9 + 1; A.menu_stale = true; }
            break;
        case KEY_M: A.wrap = !A.wrap; A.menu_stale = true; break;
        case KEY_ENTER: case KEY_SPACE: case KEY_KPENTER: menu_activate(); break;
        case KEY_ESC: kapp_quit(A.app, 0); break;
        default: break;
        }
    }
    if (A.state != S_TITLE || A.menu_stale) return;
    static int last_mx = -1, last_my = -1;
    bool moved = ui->mx != last_mx || ui->my != last_my;
    last_mx = ui->mx;
    last_my = ui->my;
    for (int i = 0; i < M_COUNT; i++) {
        KRect r = menu_rect(i);
        if (ui->mouse_inside && kr_contains(r, ui->mx, ui->my)) {
            if (moved) A.sel = i;
            if (ui->pressed[0]) { A.sel = i; menu_activate(); break; }
        }
    }
}

static void pause_game(void) {
    if (A.state == S_PLAY || A.state == S_READY) { set_state(S_PAUSE); trace("paused"); }
}

static void resume_game(void) {
    set_state(S_PLAY);
    kg_step_reset(&A.step);
    trace("resumed");
}

static void handle_play_input(KUI *ui) {
    for (int i = 0; i < ui->nkeys; i++) {
        KKey *k = &ui->keys[i];
        if (!k->down) continue;
        int d = key_to_dir(k->code);
        if (d >= 0 && k->down == 1 && !A.bot) { sg_turn(&A.sg, d); if (A.trace) trace("turn %d", d); }
        else if (k->down == 1 && (k->code == KEY_SPACE || k->code == KEY_P || k->code == KEY_ESC)) pause_game();
        else if (k->code == KEY_F3 && k->down == 1) { A.show_fps = !A.show_fps; A.full_redraw = true; }
    }
}

/* ---------------------------------------------------------------- the frame */

static void draw_frame_state(KApp *app, KUI *ui) {
    KCanvas *cv = &ui->cv;
    uint64_t now = k_now_ms();
    A.ticks++;
    if (A.trace && A.ticks % 100 == 0) trace("frames: state %d fps %d.%d, %d us of work per frame (worst %d us)", A.state, A.fps.fps10 / 10, A.fps.fps10 % 10, A.fps.work_us_avg, A.fps.work_us_max);
    switch (A.state) {
    case S_TITLE:
        handle_title_input(ui);
        if (A.state != S_TITLE) break;
        update_parts();
        if (A.ticks % 6 == 0) spawn_part(kg_rng_range(&A.fx_rng, WIN_W) * 16, -20 * 16, 0, 12 + kg_rng_range(&A.fx_rng, 14), 400, 0);
        draw_title(cv, ui);
        draw_parts(cv, 0, 0);
        k_damage(ui, kr(0, 0, WIN_W, WIN_H));
        kapp_set_tick(app, 40);
        return;
    case S_SCORES:
        for (int i = 0; i < ui->nkeys; i++) {
            KKey *k = &ui->keys[i];
            if (!k->down) continue;
            if (k->code == KEY_ESC || k->code == KEY_ENTER || k->code == KEY_SPACE) set_state(S_TITLE);
            if (k->code == KEY_LEFT || k->code == KEY_RIGHT) A.score_tab ^= 1;
        }
        if (ui->pressed[0] && A.state == S_SCORES) set_state(S_TITLE);
        if (A.state == S_SCORES) { draw_scores(cv); k_damage(ui, kr(0, 0, WIN_W, WIN_H)); kapp_set_tick(app, 80); }
        else kapp_redraw(app);
        return;
    default: break;
    }

    /* ---- the game ---- */
    if (!ui->has_focus && A.state == S_PLAY) pause_game();
    int steps = 0, alpha = 256;
    if (A.state == S_READY) {
        handle_play_input(ui);
        if (A.state == S_READY && now >= A.banner_until_ms) { set_state(S_PLAY); kg_step_reset(&A.step); }
    }
    if (A.state == S_PLAY) {
        handle_play_input(ui);
        if (A.state == S_PLAY) {
            steps = kg_step_due(&A.step);
            for (int i = 0; i < steps && A.state == S_PLAY; i++) {
                if (A.bot && (A.bot_limit == 0 || A.sg.eaten < A.bot_limit)) sg_turn(&A.sg, sg_bot_dir(&A.sg));
                long before = A.sg.score;
                sg_step(&A.sg);
                on_step_events(before);
            }
            alpha = kg_step_alpha256(&A.step);
        }
    } else if (A.state == S_PAUSE) {
        for (int i = 0; i < ui->nkeys; i++) {
            KKey *k = &ui->keys[i];
            if (!k->down || k->down != 1) continue;
            if (k->code == KEY_SPACE || k->code == KEY_P || k->code == KEY_ESC || k->code == KEY_ENTER) resume_game();
            else if (k->code == KEY_Q) set_state(S_TITLE);
            else if (k->code == KEY_F3) { A.show_fps = !A.show_fps; A.overlay_drawn = false; }
        }
        if (ui->pressed[0] && ui->has_focus) resume_game();
    } else if (A.state == S_DYING) {
        for (int i = 0; i < ui->nkeys; i++) if (ui->keys[i].down == 1 && ui->keys[i].code == KEY_F3) A.show_fps = !A.show_fps;
        if (now - A.state_since_ms > 1100) game_over();
    } else if (A.state == S_OVER) {
        for (int i = 0; i < ui->nkeys; i++) {
            KKey *k = &ui->keys[i];
            if (k->down != 1) continue;
            if (k->code == KEY_ENTER || k->code == KEY_SPACE || k->code == KEY_KPENTER) { start_game(); break; }
            if (k->code == KEY_ESC) { set_state(S_TITLE); break; }
            if (k->code == KEY_F3) { A.show_fps = !A.show_fps; A.overlay_drawn = false; }
        }
    }
    if (A.state == S_TITLE) { kapp_redraw(app); return; }
    if ((A.state == S_PAUSE || A.state == S_OVER) && A.overlay_drawn && !A.full_redraw) {    /* nothing moves: leave the picture alone */
        k_damage(ui, kr(0, 0, 0, 0));
        kapp_set_tick(app, 100);
        return;
    }

    if (A.shake > 0 || A.flash > 0) { if (A.ticks % 2 == 0) { if (A.shake > 0) A.shake--; if (A.flash > 0) A.flash--; } }
    update_parts();

    uint64_t t0 = kg_now_us();
    if (A.full_redraw) { kc_fill(cv, kr(0, 0, cv->w, cv->h), 0x0A0E26); }
    draw_field(cv, ui, alpha);
    if (A.sg.score != A.shown_score || A.sg.level != A.shown_level || A.sg.len != A.shown_len || (int)A.best != A.shown_best) A.hud_dirty = true;
    bool hud_now = A.full_redraw || A.hud_dirty;
    if (hud_now) draw_hud(cv);
    if (A.state == S_PAUSE) draw_overlay_text(cv, "PAUSED", "Space or Esc: continue", "Q: back to the title", 0xFFF4C8);
    else if (A.state == S_OVER) {
        char l1[64], l2[64];
        snprintf(l1, sizeof l1, "Score %ld   Level %d   Length %d", A.sg.score, A.sg.level, A.sg.len);
        snprintf(l2, sizeof l2, "%s", A.new_record ? "New high score!   Enter: again   Esc: title" : "Enter: play again   Esc: title");
        draw_overlay_text(cv, "GAME OVER", l1, l2, 0xFF8090);
    }
    if (A.show_fps) {
        char buf[64];
        snprintf(buf, sizeof buf, "fps %d.%d  %d.%d ms", A.fps.fps10 / 10, A.fps.fps10 % 10, A.fps.work_us_avg / 1000, A.fps.work_us_avg / 100 % 10);
        kc_fill(cv, kr(4, HUD_H + 4, (int)strlen(buf) * 8 + 8, 20), 0x000000);
        kc_text(cv, 8, HUD_H + 6, buf, 0x80FF80);
    }
    if (hud_now) k_damage(ui, kr(0, 0, WIN_W, WIN_H));
    else k_damage(ui, kr(0, HUD_H, FIELD_W, FIELD_H));
    A.full_redraw = false;
    if (A.state == S_PAUSE || A.state == S_OVER) A.overlay_drawn = true;
    kg_fps_frame(&A.fps, kg_now_us() - t0);
    kapp_set_tick(app, A.state == S_PLAY || A.state == S_READY || A.state == S_DYING ? 25 : 60);
}

static void draw(KApp *app, KUI *ui, void *user) {
    (void)user;
    if (A.state == S_LOADING) {
        KCanvas *cv = &ui->cv;
        if (A.load_stage < SART_STAGES) sart_build_stage(&A.art, A.load_stage++);
        kc_fill(cv, kr(0, 0, cv->w, cv->h), 0x0A0E26);
        kc_text_shadow(cv, (WIN_W - 24 * 8) / 2, WIN_H / 2 - 30, "Rolling the dango...", 0xFFF4C8, 0x000000);
        int w = 240 * A.load_stage / SART_STAGES;
        kc_fill(cv, kr((WIN_W - 240) / 2, WIN_H / 2, 240, 14), 0x1D2A5C);
        kc_fill(cv, kr((WIN_W - 240) / 2, WIN_H / 2, w, 14), 0xF2709A);
        kc_outline(cv, kr((WIN_W - 240) / 2 - 1, WIN_H / 2 - 1, 242, 16), 0xD9A441);
        k_damage(ui, kr(0, 0, cv->w, cv->h));
        if (A.load_stage >= SART_STAGES) {
            if (A.skip_title) start_game(); else set_state(S_TITLE);
        }
        kapp_redraw(app);
        return;
    }
    draw_frame_state(app, ui);
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv) {
    A.wrap = false;
    A.start_level = 1;
    A.seed = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) return sn_selftest();
        else if (!strcmp(argv[i], "--trace")) A.trace = true;
        else if (!strcmp(argv[i], "--wrap")) A.wrap = true;
        else if (!strcmp(argv[i], "--start")) A.skip_title = true;
        else if (!strcmp(argv[i], "--bot")) { A.bot = true; A.skip_title = true; }
        else if (!strcmp(argv[i], "--bot-limit") && i + 1 < argc) A.bot_limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) { A.fixed_seed = true; A.seed = (uint32_t)strtoul(argv[++i], NULL, 10); }
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) { A.start_level = atoi(argv[++i]); if (A.start_level < 1) A.start_level = 1; if (A.start_level > 9) A.start_level = 9; }
        else { fprintf(stderr, "usage: snake [--selftest] [--seed N] [--wrap] [--level N] [--start] [--bot [--bot-limit N]] [--trace]\n"); return 2; }
    }
    kg_scores_init(&A.scores, false, 10);
    kg_scores_load(&A.scores, SCORE_FILE);
    kg_rng_seed(&A.fx_rng, 12345);
    A.state = S_LOADING;
    kapp_set_hidpi(false);                                   /* draws its own pixels: 1x windows, enlarged by the window server on a hi-dpi screen */
    A.app = kapp_new("Snake", WIN_W, WIN_H, FW_CENTER);
    if (!A.app) return 1;
    kapp_set_draw(A.app, draw, NULL);
    kapp_set_tick(A.app, 30);
    A.tongue_ms = k_now_ms();
    int rc = kapp_run(A.app);
    sart_free(&A.art);
    kapp_destroy(A.app);
    return rc;
}
