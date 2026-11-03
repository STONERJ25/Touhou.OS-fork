/* Danmaku: the application shell: menus, the game loop, pause, game over, ending and the score table. */
#ifndef DM_UI_H
#define DM_UI_H

#include "gfx.h"

typedef struct Options {
    bool demo, god, trace, hold, boss_only, midboss_only, noscores, fixed_seed, sheet;
    int diff, shot, stage, phase, power, warp, quit_after;
    uint32_t seed;
    bool start_now;                                 /* skip the menus */
} Options;

int  dm_selftest(void);
void ui_run(const Options *opt);                    /* opens the window and runs until the player quits */

#endif
