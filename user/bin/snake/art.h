/* Procedural art for Snake: the garden, dango, lanterns, the serpent and its head. Everything is drawn at start-up. */
#ifndef SNAKE_ART_H
#define SNAKE_ART_H

#include <kappa.h>
#include <kappa_gfx2d.h>
#include "logic.h"

#define CELL 24
#define FIELD_W (SN_W * CELL)           /* 576 */
#define FIELD_H (SN_H * CELL)           /* 432 */
#define HUD_H 40
#define WIN_W FIELD_W
#define WIN_H (FIELD_H + HUD_H)         /* 472 */

#define NRAD 6                          /* body disc radii: tail taper */

typedef struct SArt {
    KSprite *floor;                     /* the garden, (FIELD_W + 16) x (FIELD_H + 16): 8 px of margin for the screen shake */
    KSprite *fence;                     /* border of the walled field (keyed) */
    KSprite *portal[2];                 /* border of the wrap-around field, two animation phases (keyed) */
    KSprite *dango;                     /* 24 x 24 */
    KSprite *gold;                      /* golden dango with its sparkle (translucent halo) */
    KSprite *gold_halo;
    KSprite *stone;                     /* lantern */
    KSprite *outline[NRAD];             /* body discs: outline, two body tones, highlight; alive and hurt palettes */
    KSprite *fill[2][2][NRAD];          /* [hurt][tone][radius] */
    KSprite *shine[NRAD];
    KSprite *head[4][2];                /* [direction][tongue out] 44 x 44 */
    KSprite *head_dead[4];
    KSprite *petal;                     /* 5x4 */
    KSprite *spark[3];                  /* little stars */
    KSprite *title_bg;                  /* WIN_W x WIN_H night scene */
    KSprite *logo;                      /* "Eat the dango" */
    KSprite *logo_sub;
} SArt;

void sart_build_stage(SArt *a, int stage);       /* stages 0..N-1: so the loading bar can move between them */
#define SART_STAGES 6
void sart_free(SArt *a);

#endif
