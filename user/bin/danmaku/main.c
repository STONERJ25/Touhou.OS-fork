/*
 * danmaku: a vertically scrolling bullet-hell shooter in the spirit of the series. Three stages, three bosses, spell cards.
 *
 *   arrows move    Z shoot    X bomb    Shift focus (slow, shows the hitbox)    Esc pause    F3 frame rate
 *
 * Command line (for testing and for looking at a particular moment):
 *   --selftest             run the logic self-test and exit
 *   --demo                 let the autopilot play; implies --start
 *   --start                skip the menus and begin a game
 *   --stage N (1-3)        start in practice mode at that stage     --diff 0..3 (easy..lunatic)    --shot 0|1 (A or B)
 *   --boss / --midboss     jump to the boss of the stage            --phase K   begin the boss at phase K
 *   --power N (0..400)     starting power                           --seed N    fixed random seed
 *   --warp N               simulate N ticks before showing anything --hold      freeze after the warp (a still picture)
 *   --god                  invincible                               --quit-after N   leave after N ticks and print the result
 *   --noscores             do not touch the score file              --trace     print what happens
 */
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    Options o;
    memset(&o, 0, sizeof o);
    o.diff = o.shot = -1;
    o.power = -1;
    fxm_init();
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool has = i + 1 < argc;
        if (!strcmp(a, "--selftest")) return dm_selftest();
        else if (!strcmp(a, "--demo")) { o.demo = true; o.start_now = true; }
        else if (!strcmp(a, "--start")) o.start_now = true;
        else if (!strcmp(a, "--god")) o.god = true;
        else if (!strcmp(a, "--trace")) o.trace = true;
        else if (!strcmp(a, "--hold")) o.hold = true;
        else if (!strcmp(a, "--noscores")) o.noscores = true;
        else if (!strcmp(a, "--boss")) { o.boss_only = true; o.start_now = true; }
        else if (!strcmp(a, "--midboss")) { o.midboss_only = true; o.start_now = true; }
        else if (!strcmp(a, "--stage") && has) { o.stage = atoi(argv[++i]); o.start_now = true; }
        else if (!strcmp(a, "--diff") && has) { o.diff = atoi(argv[++i]); if (o.diff < 0 || o.diff >= DIFF_COUNT) o.diff = DIFF_NORMAL; }
        else if (!strcmp(a, "--shot") && has) { o.shot = atoi(argv[++i]) ? SHOT_B : SHOT_A; }
        else if (!strcmp(a, "--phase") && has) o.phase = atoi(argv[++i]);
        else if (!strcmp(a, "--power") && has) o.power = atoi(argv[++i]);
        else if (!strcmp(a, "--warp") && has) o.warp = atoi(argv[++i]);
        else if (!strcmp(a, "--quit-after") && has) o.quit_after = atoi(argv[++i]);
        else if (!strcmp(a, "--seed") && has) { o.fixed_seed = true; o.seed = (uint32_t)strtoul(argv[++i], NULL, 10); }
        else {
            fprintf(stderr, "usage: danmaku [--selftest] [--demo] [--start] [--stage N] [--diff 0-3] [--shot 0|1] [--boss|--midboss] [--phase K] [--power N]\n"
                            "               [--seed N] [--warp N [--hold]] [--god] [--quit-after N] [--noscores] [--trace]\n");
            return 2;
        }
    }
    if (o.stage > N_STAGES) o.stage = N_STAGES;
    ui_run(&o);
    return 0;
}
