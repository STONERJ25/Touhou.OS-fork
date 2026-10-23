/* The rules of Snake (Eat the dango): pure logic without any drawing, so that it can be tested in the guest. */
#ifndef SNAKE_LOGIC_H
#define SNAKE_LOGIC_H

#include <stdint.h>
#include <stdbool.h>
#include <kappa_gamekit.h>

#define SN_W 24
#define SN_H 18
#define SN_CELLS (SN_W * SN_H)
#define SN_GOLD_STEPS 60                    /* a golden dango stays this many steps */
#define SN_PER_LEVEL 5                      /* dango per level */
#define SN_MAX_LEVEL 12

typedef struct SPt { int8_t x, y; } SPt;

enum { DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT };
enum { SS_PLAYING, SS_DEAD };
enum { DEATH_NONE, DEATH_WALL, DEATH_SELF, DEATH_STONE };
enum { EAT_NONE, EAT_DANGO, EAT_GOLD };

typedef struct SnakeGame {
    bool wrap;                              /* no walls: the field is a torus */
    int start_level, level, eaten;
    SPt body[SN_CELLS + 1];                 /* body[0] is the head */
    int len;
    int dir;
    int queue[3], nq;                       /* turns waiting for the next steps */
    SPt old_tail;                           /* the cell the tail left in the last step (for smooth drawing) */
    bool grew;                              /* the last step added a segment instead of moving the tail */
    SPt prev_head;
    uint8_t obst[SN_CELLS];                 /* 1 = a stone lantern */
    int n_obst;
    SPt food;
    SPt gold;                               /* golden dango, valid while gold_ttl > 0 */
    int gold_ttl;
    long score;
    int status, death;
    int steps;
    KRng rng;
    /* events of the latest step for the interface (cleared at the start of every step) */
    int ev_eat;                             /* EAT_* */
    SPt ev_eat_at;
    bool ev_level_up;
    bool ev_gold_spawn, ev_gold_expire;
    int ev_new_stones;                      /* obstacles added by a level-up */
} SnakeGame;

void sg_new(SnakeGame *g, bool wrap, int start_level, uint32_t seed);
void sg_turn(SnakeGame *g, int dir);        /* queues a turn; reversing or repeating the current direction is ignored */
void sg_step(SnakeGame *g);                 /* advances by one cell */
int  sg_step_ms(const SnakeGame *g);        /* time between steps at the current level */
int  sg_level_stones(int level);            /* how many obstacle cells the layout of this level has in total */
int  sg_bot_dir(const SnakeGame *g);        /* autopilot for --bot and tests: a direction that heads for the dango without trapping itself */
bool sg_is_snake(const SnakeGame *g, int x, int y);
bool sg_is_stone(const SnakeGame *g, int x, int y);

static inline int sg_idx(int x, int y) { return y * SN_W + x; }

#endif
