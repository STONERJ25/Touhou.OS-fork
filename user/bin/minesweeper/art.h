/* Procedural art for Minesweeper: grass tiles, ofuda flags, hiding fairies, the smiley and the red LED digits. */
#ifndef MS_ART_H
#define MS_ART_H

#include <kappa.h>
#include <kappa_gfx2d.h>

enum { FACE_NORMAL, FACE_OOH, FACE_COOL, FACE_DEAD, FACE_COUNT };

typedef struct Art {
    int cell;
    KSprite *covered[3];            /* grass tile variants */
    KSprite *pressed;               /* an uncovered empty cell (also the background of numbers) */
    KSprite *num[9];                /* [1..8]: the open tile with its number */
    KSprite *flag;                  /* grass with an ofuda stuck in it */
    KSprite *fairy;                 /* a fairy on open ground (shown when the game ends) */
    KSprite *fairy_hit;             /* the fairy that was uncovered: red background */
    KSprite *wrong;                 /* a flag on a cell without a fairy: crossed out */
    KSprite *face[FACE_COUNT];      /* 32x32 smiley faces */
} Art;

void art_build(Art *a, int cell);
void art_free(Art *a);
void art_draw_led(KCanvas *c, int x, int y, int value);      /* three red 7-segment digits on black */

#define LED_W 46
#define LED_H 27
#define FACE_SIZE 32

#endif
