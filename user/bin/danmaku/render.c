/* Drawing the game: the field with everything on it, the banners and the HUD. Reads the simulation, never changes it. */
#include "gfx.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

bool g_show_fps;
int g_fps10, g_work_us, g_work_max;

static int s_ox, s_oy;                                          /* screen shake */
static int s_opt_spread = 22 * 16;                              /* option distance, eased between focus and normal */

static void blit_center(KCanvas *cv, const KSprite *s, int x, int y) { ks_blit(cv, x - s->ax + s_ox, y - s->ay + s_oy, s); }
static void blit_center_key(KCanvas *cv, const KSprite *s, int x, int y) { ks_blit_key(cv, x - s->w / 2 + s_ox, y - s->h / 2 + s_oy, s); }
static void add_center(KCanvas *cv, const KSprite *s, int x, int y) { ks_blit_add(cv, x - s->w / 2 + s_ox, y - s->h / 2 + s_oy, s); }

/* a circle outline that adds light: midpoint circle algorithm */
static void ring_add(KCanvas *cv, int cx, int cy, int r, uint32_t col, int thick) {
    for (int k = 0; k < thick; k++) {
        int rr = r - k, x = rr, y = 0, err = 1 - rr;
        if (rr <= 0) break;
        while (x >= y) {
            static const int sx[8] = { 1, -1, 1, -1, 1, -1, 1, -1 }, sy[8] = { 1, 1, -1, -1, 1, 1, -1, -1 };
            for (int q = 0; q < 8; q++) {
                int px = cx + sx[q] * (q < 4 ? x : y), py = cy + sy[q] * (q < 4 ? y : x);
                if (px >= cv->clip.x && py >= cv->clip.y && px < cv->clip.x + cv->clip.w && py < cv->clip.y + cv->clip.h) {
                    uint32_t *d = &cv->px[py * cv->stride + px];
                    *d = ks_add_px(*d, col);
                }
            }
            y++;
            if (err < 0) err += 2 * y + 1;
            else { x--; err += 2 * (y - x) + 1; }
        }
    }
}

/* ---------------------------------------------------------------- cached texts */

typedef struct TextSlot { KSprite *spr; int key; } TextSlot;

static KSprite *slot_text(TextSlot *t, int key, const char *s, int scale, uint32_t fg, uint32_t outline) {
    if (t->spr && t->key == key) return t->spr;
    ks_free(t->spr);
    t->spr = ks_text(s, scale, fg, outline);
    t->key = key;
    return t->spr;
}

static TextSlot s_stage_title, s_stage_sub, s_card_name, s_boss_name, s_card_intro;

/* ---------------------------------------------------------------- the field */

static void draw_bullets(KCanvas *cv) {
    for (int i = 0; i < G.nb; i++) {
        const Bullet *b = &G.bl[i];
        int x = PFX + s_ox + FXI(b->x), y = PFY + s_oy + FXI(b->y);
        if (b->flags & BF_GHOST) {                                 /* a bullet that has not launched yet: a twinkle */
            const KSprite *sp = A.spark[(G.tick + i) % 3];
            ks_blit_key(cv, x - sp->w / 2, y - sp->h / 2, sp);
            continue;
        }
        int rot = (b->flags & BF_DIR) ? ((b->ang + 32) >> 6) & (NROT - 1) : 0;
        const KSprite *sp = A.bullet[b->kind][b->color][rot];
        ks_blit_key(cv, x - sp->w / 2, y - sp->h / 2, sp);
    }
}

static void draw_enemies(KCanvas *cv) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Enemy *e = &G.en[i];
        if (!e->active) continue;
        int frame = ((e->t >> 3) + i) & 1;
        const KSprite *sp = A.enemy[e->type][frame];
        int x = PFX + FXI(e->x), y = PFY + FXI(e->y);
        blit_center(cv, sp, x, y);
        if (e->flash > 0) ks_blit_add(cv, x - sp->ax + s_ox, y - sp->ay + s_oy, sp);
    }
}

static void draw_boss(KCanvas *cv) {
    const Boss *b = &G.boss;
    if (!b->active) return;
    int x = PFX + FXI(b->x), y = PFY + FXI(b->y) + (int)(fx_sin((int)G.tick * 6) >> 14);
    if (b->phase >= 0 || b->entry > 10) {
        if (b->spell && b->phase >= 0) {                           /* a magic circle behind the boss */
            add_center(cv, A.circle[(G.tick / 2) % 6], x, y + 8);
            const KSprite *glow = A.puff[b->def->sprite == 0 ? BC_PINK : b->def->sprite == 1 ? BC_GREEN : b->def->sprite == 2 ? BC_YELLOW : b->def->sprite == 3 ? BC_ORANGE : BC_PURPLE][2];
            add_center(cv, glow, x, y);
        }
    }
    int sid = b->def->sprite;
    const KSprite *sp = A.boss[sid][(G.tick >> 4) & 1];
    if (b->dying) {
        int lv = 256 - b->dying * 256 / 100;
        ks_blit_fade(cv, x - sp->ax + s_ox, y - sp->ay + s_oy, sp, lv < 0 ? 0 : lv);
        return;
    }
    if (b->invuln > 0 && b->invuln < (1 << 19) && (G.tick & 2)) {
        ks_blit_fade(cv, x - sp->ax + s_ox, y - sp->ay + s_oy, sp, 190);
    } else if (b->flash > 0 && (G.tick & 3) == 0) {
        ks_blit(cv, x - sp->ax + s_ox, y - sp->ay + s_oy, A.boss_flash[sid]);              /* a quick flicker when she is hit */
    } else blit_center(cv, sp, x, y);
}

static void draw_items(KCanvas *cv) {
    for (int i = 0; i < MAX_ITEMS; i++) {
        const Item *it = &G.it[i];
        if (!it->active) continue;
        const KSprite *sp = A.item[it->type];
        int x = PFX + FXI(it->x), y = PFY + FXI(it->y);
        if (y < PFY) {                                             /* above the field: a marker at the edge */
            ks_blit_key(cv, x - 4 + s_ox, PFY + 1, A.spark[0]);
            continue;
        }
        blit_center_key(cv, sp, x, y);
    }
}

static void draw_pshots(KCanvas *cv) {
    for (int i = 0; i < MAX_PSHOTS; i++) {
        const PShot *s = &G.ps[i];
        if (!s->active) continue;
        const KSprite *sp = A.pshot[s->kind];
        blit_center(cv, sp, PFX + FXI(s->x), PFY + FXI(s->y));
    }
}

static void draw_player(KCanvas *cv) {
    const Player *p = &G.p;
    if (G.state == GS_GAME_OVER && G.clear_t < 150) return;
    int x = PFX + FXI(p->x), y = PFY + FXI(p->y);
    bool blink = p->invuln > 0 && p->bomb_t == 0 && p->respawn_t == 0 && (G.tick & 2) && p->invuln < 400;
    int target = p->focus ? 11 * 16 : 22 * 16;
    s_opt_spread += (target - s_opt_spread) / 4;
    if (p->power >= 100 && p->death_t == 0) {
        int n = p->power >= 300 ? 4 : 2;
        for (int i = 0; i < n; i++) {
            int side = (i & 1) ? 1 : -1, row = i / 2;
            int ox = side * (s_opt_spread / 16 + row * (p->focus ? 8 : 12));
            int oy = (p->focus ? -10 : 4) + row * (p->focus ? 0 : 6) + (int)(fx_sin((int)G.tick * 12 + i * 256) >> 14);
            blit_center_key(cv, A.option[G.shot], x + ox, y + oy);
        }
    }
    int bank = p->move_dx < 0 ? 1 : p->move_dx > 0 ? 2 : 0;
    if (!blink) {
        const KSprite *sp = A.player[G.shot][bank];
        if (p->death_t > 0 && (G.tick & 1)) ks_blit_add(cv, x - sp->ax + s_ox, y - sp->ay + s_oy, sp);
        else blit_center(cv, sp, x, y);
    }
    if (p->focus && p->death_t == 0) blit_center_key(cv, A.hitbox, x, y);
}

static void draw_fx(KCanvas *cv) {
    for (int i = 0; i < MAX_FX; i++) {
        const Fx *f = &G.fx[i];
        if (!f->active) continue;
        int x = PFX + FXI(f->x), y = PFY + FXI(f->y);
        switch (f->kind) {
        case FXK_SPARK: {
            int s = f->t < f->life / 3 ? 2 : f->t < 2 * f->life / 3 ? 1 : 0;
            ks_blit_key(cv, x - A.spark[s]->w / 2 + s_ox, y - A.spark[s]->h / 2 + s_oy, A.spark[s]);
            break;
        }
        case FXK_PUFF: {
            int s = f->t * 3 / (f->life ? f->life : 1);
            add_center(cv, A.puff[f->color][s > 2 ? 2 : s], x, y);
            break;
        }
        case FXK_RING:
            ring_add(cv, x + s_ox, y + s_oy, 6 + f->t * 5, ks_mul_px(BPAL[f->color].base, 256 - f->t * 256 / f->life), 2);
            break;
        case FXK_TEXT: {
            char buf[16];
            snprintf(buf, sizeof buf, "%d", f->val);
            kc_text_shadow(cv, x - (int)strlen(buf) * 4 + s_ox, y + s_oy - 6, buf, f->color == BC_YELLOW ? 0xFFE878 : 0xFFFFFF, 0x201020);
            break;
        }
        case FXK_BOMB_ORB:
            add_center(cv, A.orb[f->color % 7], x, y);
            add_center(cv, A.orb[f->color % 7], x + ((int)fx_cos(f->t * 16) >> 14), y + 6);
            break;
        default: break;
        }
    }
}

static void draw_bomb(KCanvas *cv) {
    const Player *p = &G.p;
    if (p->bomb_t <= 0) return;
    int total = G.shot == SHOT_A ? 150 : 130, age = total - p->bomb_t;
    int x = PFX + FXI(p->x), y = PFY + FXI(p->y);
    if (G.shot == SHOT_A) {
        int r = age * 5;
        static const uint32_t RING[3] = { 0xFF9080, 0xFFE070, 0x80B0FF };
        for (int k = 0; k < 3; k++) if (r - k * 10 > 0) ring_add(cv, x + s_ox, y + s_oy, r - k * 10, ks_mul_px(RING[k], age > 90 ? (130 - age) * 256 / 40 : 256), 2);
    } else {
        int half = age * 9;
        if (half > PFW) half = PFW;
        int fade = age > 100 ? (130 - age) * 256 / 30 : 256;
        if (fade < 0) fade = 0;
        int x0 = x - half, x1 = x + half;
        KRect col = kr(x0 + s_ox, PFY, x1 - x0, y - PFY + 14);
        ks_stipple_rect(cv, col, ks_mul_px(0xA0C0FF, fade), (int)G.tick);
        int core = half < 56 ? half : 56;
        ks_add_rect(cv, kr(x - core + s_ox, PFY, core * 2, y - PFY), ks_mul_px(0x304880, fade));
        ks_add_rect(cv, kr(x - core / 2 + s_ox, PFY, core, y - PFY), ks_mul_px(0x6070A0, fade));
        for (int k = 0; k < 6; k++) {                              /* stars rising in the beam */
            int sx = x + ((int)(fx_sin((int)G.tick * 7 + k * 170) >> 10) * core / 64);
            int sy = y - ((age * 14 + k * 90) % (y - PFY + 20));
            ks_blit_key(cv, sx - 6 + s_ox, sy - 6, A.pshot[PS_STAR]);
        }
    }
}

void render_game(KCanvas *cv, int alpha) {
    (void)alpha;
    KRect field = kr(PFX, PFY, PFW, PFH);
    KRect old = cv->clip;
    kc_set_clip(cv, field);
    s_ox = s_oy = 0;
    if (G.shake > 0) {
        int mag = G.shake > 12 ? 4 : G.shake > 4 ? 3 : 2;
        s_ox = (int)((G.tick * 5) % (2 * mag + 1)) - mag;
        s_oy = (int)((G.tick * 3 + 1) % (2 * mag + 1)) - mag;
    }
    bool spell = G.boss.active && G.boss.spell && G.boss.phase >= 0;
    bg_draw(cv, s_ox, s_oy, G.stage, spell, 0, (int)G.tick, (int)G.tick);
    draw_boss(cv);
    draw_enemies(cv);
    draw_items(cv);
    draw_pshots(cv);
    draw_player(cv);
    draw_bomb(cv);
    draw_bullets(cv);
    draw_fx(cv);
    if (G.flash > 0) ks_tint_rect(cv, field, 0xFFFFFF, G.flash > 6 ? 190 : G.flash * 28);
    render_overlays(cv);
    cv->clip = old;
}

/* ---------------------------------------------------------------- banners */

void render_overlays(KCanvas *cv) {
    const Boss *b = &G.boss;
    char buf[96];
    /* stage title */
    if (G.stage_banner_t > 30 && G.stage_banner_t < 230 && G.state == GS_PLAY) {
        int t = G.stage_banner_t - 30, fade = t < 20 ? t * 256 / 20 : t > 160 ? (180 - t) * 256 / 20 : 256;
        if (fade < 0) fade = 0;
        snprintf(buf, sizeof buf, "Stage %d", G.stage + 1);
        KSprite *a = slot_text(&s_stage_title, G.stage, buf, 3, 0xFFF4C8, 0x601020);
        KSprite *c = slot_text(&s_stage_sub, G.stage, stage_name(G.stage), 2, 0xFFFFFF, 0x201040);
        ks_blit_fade(cv, PFX + (PFW - a->w) / 2, PFY + 130, a, fade);
        ks_blit_fade(cv, PFX + (PFW - c->w) / 2, PFY + 176, c, fade);
        const char *sub = stage_subtitle(G.stage);
        kc_text_shadow(cv, PFX + (PFW - (int)strlen(sub) * 8) / 2, PFY + 210, sub, ks_mul_px(0xE0E8FF, fade), 0x000020);
    }
    if (b->active && b->phase >= 0 && !b->dying) {
        /* health bar and the remaining attacks */
        int bw = PFW - 150, bx = PFX + 20, by = PFY + 8;
        int fill = bw * (b->shown_hp > 0 ? b->shown_hp : 0) / (b->maxhp ? b->maxhp : 1);
        ks_fill_rect(cv, kr(bx - 1, by - 1, bw + 2, 7), 0x100810);
        ks_fill_rect(cv, kr(bx, by, fill, 5), b->spell ? 0xF04A64 : 0xF0F0F0);
        ks_fill_rect(cv, kr(bx, by, fill, 1), b->spell ? 0xFF9AB0 : 0xFFFFFF);
        int left = b->def->nphases - b->phase - 1;
        for (int i = 0; i < left; i++) ks_blit_key(cv, bx + bw + 8 + i * 12, by - 5, A.life_icon);
        /* timer */
        int secs = b->timer / 60;
        snprintf(buf, sizeof buf, "%d", secs < 0 ? 0 : secs);
        uint32_t col = secs < 10 ? 0xFF6070 : 0xFFE0A0;
        kc_text_scaled(cv, PFX + PFW - 12 - (int)strlen(buf) * 16, PFY + 14, buf, 0x201020, 2);
        kc_text_scaled(cv, PFX + PFW - 14 - (int)strlen(buf) * 16, PFY + 12, buf, col, 2);
        /* names */
        kc_text_shadow(cv, PFX + 22, PFY + 16, b->def->name, 0xFFFFFF, 0x201040);
        if (b->spell) {
            const char *nm = b->def->phases[b->phase].name;
            KSprite *c = slot_text(&s_card_name, b->id * 16 + b->phase, nm, 1, 0xFFF4C8, 0x601020);
            int slide = G.spell_banner_t < 40 ? (40 - G.spell_banner_t) * 12 : 0;
            ks_blit_key(cv, PFX + PFW - 6 - c->w + slide, PFY + 38, c);
            if (G.spell_banner_t < 100) {                                    /* the big announcement */
                int t = G.spell_banner_t, fade = t < 10 ? t * 256 / 10 : t > 80 ? (100 - t) * 256 / 20 : 256;
                KSprite *big = slot_text(&s_card_intro, 1, "Spell Card Attack!", 3, 0xFFF4C8, 0xA01830);
                ks_blit_fade(cv, PFX + (PFW - big->w) / 2, PFY + 150, big, fade);
                KSprite *nm2 = slot_text(&s_boss_name, b->id * 16 + b->phase, nm, 1, 0xFFFFFF, 0x401060);
                ks_blit_fade(cv, PFX + (PFW - nm2->w) / 2, PFY + 190, nm2, fade);
            }
        }
    } else if (b->active && b->phase < 0 && b->entry > 10 && b->entry < 160) {
        int t = b->entry - 10, fade = t < 15 ? t * 256 / 15 : t > 120 ? (150 - t) * 256 / 30 : 256;
        if (fade < 0) fade = 0;
        KSprite *n = slot_text(&s_boss_name, 9000 + b->id, b->def->name, 3, 0xFFF4C8, 0x601020);
        ks_blit_fade(cv, PFX + (PFW - n->w) / 2, PFY + 150, n, fade);
        const char *ti = b->def->title;
        kc_text_shadow(cv, PFX + (PFW - (int)strlen(ti) * 8) / 2, PFY + 190, ti, ks_mul_px(0xFFFFFF, fade), 0x201040);
    }
    if (G.last_bonus_t > 0) {
        int fade = G.last_bonus_t > 20 ? 256 : G.last_bonus_t * 256 / 20;
        if (G.last_bonus_ok) {
            char num[32];
            kg_format_int(num, sizeof num, G.last_bonus);
            kc_text_shadow(cv, PFX + (PFW - 19 * 8) / 2, PFY + 120, "Spell Card Bonus!", ks_mul_px(0xFFE878, fade), 0x401010);
            kc_text_shadow(cv, PFX + (PFW - (int)strlen(num) * 8) / 2, PFY + 140, num, ks_mul_px(0xFFFFFF, fade), 0x401010);
        } else kc_text_shadow(cv, PFX + (PFW - 15 * 8) / 2, PFY + 130, "Bonus failed...", ks_mul_px(0xB0B8D0, fade), 0x101020);
    }
    if (G.extend_t > 0) kc_text_shadow(cv, PFX + (PFW - 6 * 8) / 2, PFY + 250, "Extend!", ks_mul_px(0xFFB0D0, G.extend_t > 30 ? 256 : G.extend_t * 256 / 30), 0x401040);
    if (G.state == GS_STAGE_CLEAR) {
        char num[32];
        kc_text_scaled(cv, PFX + (PFW - 11 * 16) / 2, PFY + 100, "Stage Clear", 0x401010, 2);
        kc_text_scaled(cv, PFX + (PFW - 11 * 16) / 2 - 2, PFY + 98, "Stage Clear", 0xFFF4C8, 2);
        kg_format_int(num, sizeof num, G.clear_bonus);
        snprintf(buf, sizeof buf, "Clear bonus  %s", num);
        kc_text_shadow(cv, PFX + (PFW - (int)strlen(buf) * 8) / 2, PFY + 150, buf, 0xFFFFFF, 0x201040);
        snprintf(buf, sizeof buf, "Graze %d   Spell cards %d / %d", G.graze, G.stat_captures, G.stat_cards);
        kc_text_shadow(cv, PFX + (PFW - (int)strlen(buf) * 8) / 2, PFY + 174, buf, 0xC8D0F0, 0x201040);
    }
    if (g_show_fps) {
        snprintf(buf, sizeof buf, "fps %d.%d  %d.%d ms (max %d.%d)  bullets %d", g_fps10 / 10, g_fps10 % 10, g_work_us / 1000, g_work_us / 100 % 10,
                 g_work_max / 1000, g_work_max / 100 % 10, G.nb);
        ks_fill_rect(cv, kr(PFX + 2, PFY + PFH - 20, (int)strlen(buf) * 8 + 8, 18), 0x000000);
        kc_text(cv, PFX + 6, PFY + PFH - 19, buf, 0x80FF80);
    }
}

/* ---------------------------------------------------------------- the frame and the HUD */

void render_frame(KCanvas *cv) {
    ks_blit_key(cv, 0, 0, A.frame);
}

static void label(KCanvas *cv, int x, int y, const char *s) { kc_text_shadow(cv, x, y, s, 0xE8B850, 0x000010); }

void render_hud(KCanvas *cv) {
    const int hx = PFX + PFW + 22;                                  /* panel content starts here */
    const int pw = WIN_W - hx - 22;
    KRect panel = kr(PFX + PFW + 14 + 4, PFY + 4, WIN_W - (PFX + PFW + 14) - 16 - 8, PFH - 8);
    for (int y = panel.y; y < panel.y + panel.h; y++)
        ks_fill32(cv->px + (size_t)y * cv->stride + panel.x, ks_lerp_px(0x0E1434, 0x1C2860, (y - PFY) * 256 / PFH), (size_t)panel.w);
    for (int y = panel.y + 2; y < panel.y + panel.h - 2; y += 4) ks_stipple_rect(cv, kr(panel.x + 2, y, panel.w - 4, 1), 0x1E2A64, y / 4);
    char buf[48], num[32];
    int y = PFY + 14;
    label(cv, hx, y, "HiScore");
    kg_format_int(num, sizeof num, G.hiscore > G.score ? G.hiscore : G.score);
    kc_text_shadow(cv, hx + pw - (int)strlen(num) * 8, y + 16, num, 0xFFFFFF, 0x000010);
    y += 40;
    label(cv, hx, y, "Score");
    kg_format_int(num, sizeof num, G.score);
    kc_text_shadow(cv, hx + pw - (int)strlen(num) * 8, y + 16, num, 0xFFE8A8, 0x000010);
    y += 46;
    label(cv, hx, y, "Player");
    for (int i = 0; i < G.p.lives && i < 8; i++) ks_blit_key(cv, hx + 62 + i * 15, y + 1, A.life_icon);
    y += 24;
    label(cv, hx, y, "Bomb");
    for (int i = 0; i < G.p.bombs && i < 8; i++) ks_blit_key(cv, hx + 62 + i * 15, y + 1, A.bomb_icon);
    y += 32;
    label(cv, hx, y, "Power");
    snprintf(buf, sizeof buf, "%d.%02d / 4.00", G.p.power / 100, G.p.power % 100);
    kc_text_shadow(cv, hx + 62, y, buf, G.p.power >= MAX_POWER ? 0xFFE878 : 0xFFFFFF, 0x000010);
    ks_fill_rect(cv, kr(hx, y + 19, pw, 7), 0x080A1C);
    ks_fill_rect(cv, kr(hx + 1, y + 20, (pw - 2) * G.p.power / MAX_POWER, 5), G.p.power >= MAX_POWER ? 0xF2D030 : 0xE0405C);
    y += 36;
    label(cv, hx, y, "Graze");
    snprintf(buf, sizeof buf, "%d", G.graze);
    kc_text_shadow(cv, hx + pw - (int)strlen(buf) * 8, y, buf, 0xFFFFFF, 0x000010);
    y += 22;
    label(cv, hx, y, "Point");
    snprintf(buf, sizeof buf, "%d", G.points);
    kc_text_shadow(cv, hx + pw - (int)strlen(buf) * 8, y, buf, 0xFFFFFF, 0x000010);
    y += 30;
    kc_hline(cv, hx, y, pw, 0x8A6A28);
    y += 8;
    snprintf(buf, sizeof buf, "Stage %d", G.stage + 1);
    kc_text_shadow(cv, hx, y, buf, 0xFFF4C8, 0x000010);
    kc_text_shadow(cv, hx + pw - (int)strlen(diff_name(G.diff)) * 8, y, diff_name(G.diff), 0xC8D8FF, 0x000010);
    y += 20;
    kc_text_shadow(cv, hx, y, shot_name(G.shot), 0xC8D8FF, 0x000010);
    y += 20;
    if (G.mode == MODE_PRACTICE) { kc_text_shadow(cv, hx, y, "Practice", 0xF2A0B0, 0x000010); y += 18; }
    if (G.demo) { kc_text_shadow(cv, hx, y, "Demo (autopilot)", 0xF2A0B0, 0x000010); y += 18; }
    if (G.god) kc_text_shadow(cv, hx, y, "Invincible", 0xF2A0B0, 0x000010);
    ks_blit_key(cv, panel.x + (panel.w - A.hud_orb->w) / 2, PFY + PFH - 78, A.hud_orb);
    kc_text_shadow(cv, hx, PFY + PFH - 18, "Z shoot X bomb Shift", 0x7A88C0, 0x000010);
}
