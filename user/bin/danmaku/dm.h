/*
 * Danmaku: shared definitions. The simulation (game.c, bullets.c, enemies.c, stages.c, bot.c) is pure fixed-point integer
 * code without any drawing, so that it is deterministic and can be tested in the guest; the renderer (render.c, gfx.c, bg.c)
 * only reads the state.
 */
#ifndef DM_H
#define DM_H

#include <stdint.h>
#include <stdbool.h>
#include <kappa.h>
#include <kappa_gfx2d.h>
#include <kappa_gamekit.h>

/* ---------------------------------------------------------------- fixed point */

typedef int32_t fx;
#define FX(n)    ((fx)((n) * 65536))            /* for constants */
#define FXF(f)   ((fx)((f) * 65536.0f))         /* for float constants: folded by the compiler, never used at run time */
#define FXI(v)   ((int)((v) >> 16))
#define ANG      1024                           /* angle units in a full circle */
#define A_DEG(d) ((int)((d) * 1024 / 360))

void  fxm_init(void);
fx    fx_sin(int a);                            /* 16.16, any angle (wraps) */
fx    fx_cos(int a);
int   fx_atan2(fx dy, fx dx);                   /* angle 0..1023, 0 = +x, 256 = +y (down on the screen) */
int   fx_dist(fx dx, fx dy);                    /* pixels */
static inline fx fx_mul(fx a, fx b) { return (fx)(((int64_t)a * b) >> 16); }
static inline int iabs(int v) { return v < 0 ? -v : v; }

/* ---------------------------------------------------------------- field */

#define PFW     384
#define PFH     448
#define PFX     32                              /* where the field sits in the 640x480 window */
#define PFY     16
#define WIN_W   640
#define WIN_H   480
#define TICK_US 16667

enum { DIFF_EASY, DIFF_NORMAL, DIFF_HARD, DIFF_LUNATIC, DIFF_COUNT };
enum { SHOT_A, SHOT_B, SHOT_COUNT };
#define N_STAGES 3
#define MAX_POWER 400

/* ---------------------------------------------------------------- bullets */

enum { BK_PELLET, BK_ORB, BK_BIG, BK_RICE, BK_KUNAI, BK_STAR, BK_BILL, BK_BEAM, BK_COUNT };
enum { BC_RED, BC_ORANGE, BC_YELLOW, BC_GREEN, BC_CYAN, BC_BLUE, BC_PURPLE, BC_PINK, BC_WHITE, BC_COUNT };
#define NROT 16                                 /* rotation steps of directional bullets */
enum { BH_LINEAR, BH_ACCEL, BH_CURVE, BH_STOPGO, BH_SPLIT, BH_DELAY };
enum { BF_GRAZED = 1, BF_DIR = 2, BF_BOUNCE = 4, BF_GHOST = 8 };

typedef struct Bullet {
    fx x, y, vx, vy;
    int16_t ax, ay;                             /* acceleration, 1/65536 px per tick^2 */
    int16_t timer, arg;                         /* behaviour clock and argument */
    uint8_t kind, color, flags, beh;
    uint16_t ang;                               /* direction for drawing and curving (0..1023) */
    uint16_t spd;                               /* speed in 1/256 px per tick (curve, stop-and-go) */
} Bullet;

#define MAX_BULLETS 1600

/* ---------------------------------------------------------------- enemies, items, shots, effects */

enum { ET_FAIRY_BLUE, ET_FAIRY_RED, ET_FAIRY_GREEN, ET_FAIRY_YELLOW, ET_MAID, ET_ORB, ET_WISP, ET_COUNT };
enum { MV_DOWN, MV_SINE, MV_ARC, MV_STOP, MV_SWOOP, MV_DRIFT, MV_COUNT };
enum { SH_NONE, SH_AIM, SH_AIM3, SH_RING, SH_SPIN, SH_RAIN, SH_BURST, SH_FLOWER, SH_COUNT };
enum { DROP_NONE, DROP_POWER, DROP_POINT, DROP_BIG, DROP_BOMB, DROP_LIFE, DROP_MIXED };

typedef struct Enemy {
    bool active;
    uint8_t type, move, shoot, drop;
    fx x, y, ox, oy;
    int hp, maxhp;
    int t;                                      /* age in ticks */
    int16_t p[4];                               /* movement parameters */
    int16_t s[2];                               /* shooting parameters */
    int score;
    int flash;
} Enemy;
#define MAX_ENEMIES 40

enum { IT_POWER, IT_POWER_BIG, IT_POINT, IT_BOMB, IT_LIFE, IT_STAR, IT_FULL, IT_COUNT };
typedef struct Item {
    bool active, collect;
    uint8_t type;
    fx x, y, vy;
    int t;
} Item;
#define MAX_ITEMS 220

enum { PS_AMULET, PS_NEEDLE, PS_STAR, PS_BEAM };
typedef struct PShot {
    bool active;
    uint8_t kind;
    fx x, y, vx, vy;
    int dmg;
    int angle;
} PShot;
#define MAX_PSHOTS 160

enum { FXK_SPARK, FXK_PUFF, FXK_RING, FXK_GRAZE, FXK_TEXT, FXK_PETAL, FXK_BOMB_ORB };
typedef struct Fx {
    bool active;
    uint8_t kind, color;
    fx x, y, vx, vy;
    int t, life;
    int val;                                    /* text popup: the number; bomb orb: its index */
} Fx;
#define MAX_FX 320

typedef struct Input {
    bool left, right, up, down, shot, bomb, focus;
} Input;

/* ---------------------------------------------------------------- bosses */

struct Boss;
typedef struct PhaseDef {
    const char *name;                           /* spell card name, NULL for a plain attack */
    int hp, time;                               /* hit points, time limit in ticks */
    long bonus;                                 /* capture bonus */
    void (*fn)(struct Boss *b, int t);          /* called every tick with the ticks since the phase began */
} PhaseDef;

typedef struct BossDef {
    const char *name, *title;
    const PhaseDef *phases;
    int nphases;
    bool midboss;
    int sprite;                                 /* which boss artwork */
    int bg_tint;                                /* colour of the spell card backdrop */
} BossDef;

typedef struct Boss {
    bool active;
    int id;                                     /* index into BOSSES */
    fx x, y, tx, ty;
    int phase;
    int hp, maxhp;
    int t, timer;                               /* ticks into the phase, ticks left */
    int invuln;
    bool spell, failed;                         /* failed: the player died or bombed in this card */
    int flash;
    int entry;                                  /* ticks since the boss came in */
    int dying;                                  /* >0: death animation running */
    const BossDef *def;
    int shown_hp;
} Boss;

extern const BossDef BOSSES[];
extern const int N_BOSSES;

/* ---------------------------------------------------------------- the game */

enum { GS_PLAY, GS_STAGE_CLEAR, GS_GAME_OVER, GS_ENDING };
enum { MODE_NORMAL, MODE_PRACTICE };

typedef struct Player {
    fx x, y;
    int lives, bombs, power;                    /* power 0..MAX_POWER */
    int invuln, death_t, respawn_t, bomb_t, shot_cd;
    bool focus;
    int move_dx;                                /* -1, 0, 1: for the bank animation */
    int hit_flash;
} Player;

typedef struct Game {
    int state, mode, diff, shot, stage;
    long score, hiscore;
    int extends_given;
    int graze, points, stat_deaths, stat_bombs, stat_captures, stat_cards;
    long tick, stage_t;
    int clear_t;
    Player p;
    Input in, prev_in;
    Boss boss;
    Bullet *bl;
    int nb;
    Enemy en[MAX_ENEMIES];
    Item it[MAX_ITEMS];
    PShot ps[MAX_PSHOTS];
    Fx fx[MAX_FX];
    KRng rng;
    /* presentation helpers set by the simulation */
    int shake, flash;                           /* screen shake / white flash ticks */
    int stage_banner_t;                         /* ticks since the stage title appeared */
    int spell_banner_t;                         /* ticks since the current spell card was announced */
    long last_bonus;                            /* capture bonus shown after a card */
    int last_bonus_t;
    int extend_t;                               /* ticks since an extra life was granted */
    int boss_name_t;                            /* ticks since a boss came in (name banner) */
    int dbg_phase;                              /* --phase: boss phase to start with */
    int bot_poc;                                /* demo player: 1 while it fetches items from above the collection line */
    bool last_bonus_ok;
    /* options and statistics */
    bool god, demo, trace;
    int bullets_peak;
    long clear_bonus;
} Game;

extern Game G;

/* ---------------------------------------------------------------- simulation api */

/* game.c */
void game_new(int mode, int diff, int shot, int stage, uint32_t seed);
void game_tick(const Input *in);                /* one 1/60 s step */
void game_start_stage(int stage);
int  dsel(int easy, int normal, int hard, int lunatic);          /* picks a value by the difficulty */
void add_score(long n);
void player_hit(void);
void item_spawn(int type, fx x, fx y, int vy_kick);
void item_burst(int type, fx x, fx y, int n, int spread_px);
void fx_spark(fx x, fx y, int color, int n);
void fx_puff(fx x, fx y, int color, int n);
void fx_ring(fx x, fx y, int color);
void fx_text(fx x, fx y, int value, int color);
const char *shot_name(int shot);
const char *diff_name(int diff);

/* bullets.c: the pool, the behaviours and the pattern library */
void    bullets_reset(void);
Bullet *bullet_add(fx x, fx y, int angle, fx speed, int kind, int color);        /* linear; NULL if the pool is full */
Bullet *bullet_add_v(fx x, fx y, fx vx, fx vy, int kind, int color);
void    bullet_accel(Bullet *b, int angle, fx accel, int ticks);                  /* accelerate along `angle` for `ticks` ticks (-1: forever) */
void    bullet_gravity(Bullet *b, fx g);                                           /* constant downward acceleration */
void    bullet_curve(Bullet *b, int turn_per_tick, int ticks);
void    bullet_stopgo(Bullet *b, int stop_after, fx go_speed, int angle_or_aim);   /* angle_or_aim < 0: aim at the player */
void    bullet_split(Bullet *b, int after, int n, int kind, int color, fx speed);
void    bullet_delay(Bullet *b, int ticks);                                        /* harmless until the delay is over */
void    bullet_bounce(Bullet *b, int times);
void    bullets_update(void);                    /* moves, collides with the player, grazes */
void    bullets_clear(bool to_items);
void    bullets_clear_circle(fx cx, fx cy, int radius, bool to_items);
void    bullets_clear_column(fx x0, fx x1, bool to_items);
int     aim_angle(fx x, fx y);                   /* towards the player */
int     aim_angle_to(fx x, fx y, fx tx, fx ty);
int     pat_ring(fx x, fx y, int n, int a0, fx speed, int kind, int color);
int     pat_fan(fx x, fx y, int n, int center, int spread, fx speed, int kind, int color);
int     pat_aimed(fx x, fx y, int n, int spread, fx speed, int kind, int color);
int     pat_flower(fx x, fx y, int n, int petals, int a0, fx speed, int depth256, int kind, int color);
void    pat_beam_step(fx x, fx y, int angle, fx speed, int color);                 /* one bullet of a dense line: call every tick */

/* enemies.c */
Enemy  *enemy_spawn(int type, fx x, fx y, int move, int shoot, int hp, int drop);
void    enemies_update(void);
void    enemies_reset(void);
void    enemy_defeat(Enemy *e, bool by_player);

/* stages.c */
void    stage_begin(int stage);
void    stage_run(long t);                       /* spawns what is due at stage tick t */
void    boss_begin(int id);
void    boss_tick(void);
int     boss_damage(int dmg);                    /* returns the damage done (0 while invulnerable) */
void    boss_goto(Boss *b, fx x, fx y);
void    stage_skip_to_boss(bool midboss);        /* debugging: jump the script to the (mid-)boss */
const char *stage_name(int stage);
const char *stage_subtitle(int stage);

/* bot.c */
void    bot_input(Input *in);

#endif
