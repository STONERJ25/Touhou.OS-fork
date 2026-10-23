/* Danmaku art: every sprite is drawn procedurally at start-up (gfx.c, bg.c) and kept as a pre-rendered keyed sprite. */
#ifndef DM_GFX_H
#define DM_GFX_H

#include "dm.h"

typedef struct Art {
    KSprite *bullet[BK_COUNT][BC_COUNT][NROT];      /* keyed: the fast path */
    KSprite *halo;                                  /* additive glow for grazing */
    KSprite *enemy[ET_COUNT][2];                    /* two wing frames */
    KSprite *boss[5][2];
    KSprite *boss_flash[5];
    KSprite *player[SHOT_COUNT][3];                 /* straight, banking left, banking right */
    KSprite *option[SHOT_COUNT];
    KSprite *hitbox;
    KSprite *pshot[4];
    KSprite *item[IT_COUNT];
    KSprite *spark[3];
    KSprite *puff[BC_COUNT][3];                     /* additive soft discs */
    KSprite *orb[7];                                /* bomb orbs, additive */
    KSprite *circle[6];                             /* the magic circle behind a boss, additive, six rotations */
    KSprite *beam_glow;                             /* the witch's bomb beam, additive, 1 px wide strip scaled by drawing rows */
    KSprite *petal;
    KSprite *logo;                                  /* title */
    KSprite *logo_sub;
    KSprite *title_bg;                              /* 640x480 */
    KSprite *frame;                                 /* the lacquered border and the HUD panel, 640x480, with a hole for the field */
    KSprite *hud_orb;                               /* yin-yang decoration */
    KSprite *life_icon, *bomb_icon;
    KSprite *portrait[SHOT_COUNT];                  /* large character art for the title and the HUD */
} Art;

extern Art A;

/* shared helpers (gfx.c) */
typedef struct BPal { uint32_t base, light, dark; } BPal;
extern const BPal BPAL[BC_COUNT];
KSprite *spr_scale_nn(const KSprite *s, int k);                    /* nearest-neighbour enlargement */
void spr_shear(KSprite *s, int lean);                              /* shifts the rows: the top moves by `lean` px relative to the bottom */
void chars_build(int part);                                        /* chars.c: 0 enemies, 1 bosses, 2 players */
void titleart_build(int part);                                     /* titleart.c: 0 logo and title scene, 1 hud, 2 portraits */

#define GFX_STAGES 9
void gfx_build_stage(int stage);                   /* builds one chunk of the art; call 0..GFX_STAGES-1 */
const char *gfx_stage_label(int stage);
void gfx_free(void);

/* bg.c */
#define BG_MARGIN 8                                 /* spare pixels on each side of a tile for the screen shake */
#define BG_W (PFW + 2 * BG_MARGIN)
#define BG_H 512
void bg_build(int stage);                          /* one stage's backgrounds (and their spell card versions) */
void bg_draw(KCanvas *cv, int ox, int oy, int stage, bool spell, int tint, int scroll, int t);   /* field-sized, clipped to the field */
void bg_free(void);

/* render.c */
void render_game(KCanvas *cv, int alpha);          /* the playfield and everything on it */
void render_hud(KCanvas *cv);                      /* the right-hand side and frame */
void render_frame(KCanvas *cv);                    /* the decorated border around the field */
void render_overlays(KCanvas *cv);                 /* stage title, spell card banner, boss bars, messages */
extern bool g_show_fps;
extern int g_fps10, g_work_us, g_work_max;

#endif
