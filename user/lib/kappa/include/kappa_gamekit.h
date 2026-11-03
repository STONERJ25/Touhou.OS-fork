/*
 * kappa_gamekit: the small things every game wants: a deterministic random generator, a fixed-timestep clock, an FPS meter,
 * per-user settings files in ~/.config and a high-score table.
 *
 * Typical loop with KApp (the draw callback runs after every event batch and on the timer):
 *
 *     static void draw(KApp *app, KUI *ui, void *user) {
 *         int steps = kg_step_due(&g.stepper);          // how many 1/60 s logic steps are owed
 *         if (!steps) { k_damage(ui, kr(0, 0, 0, 0)); kapp_set_tick(app, kg_step_wait_ms(&g.stepper)); return; }
 *         while (steps--) update();
 *         render(ui);
 *         kapp_set_tick(app, kg_step_wait_ms(&g.stepper));
 *     }
 */
#ifndef KAPPA_GAMEKIT_H
#define KAPPA_GAMEKIT_H

#include <kappa.h>

/* ---------------------------------------------------------------- random numbers (PCG32: deterministic for a given seed) */
typedef struct KRng { uint64_t state; } KRng;
void     kg_rng_seed(KRng *r, uint32_t seed);
uint32_t kg_rng_next(KRng *r);
int      kg_rng_range(KRng *r, int n);                           /* 0 .. n-1 (n <= 0 gives 0) */
int      kg_rng_between(KRng *r, int lo, int hi);                /* lo .. hi inclusive */
uint32_t kg_seed_from_time(void);

/* ---------------------------------------------------------------- time */
uint64_t kg_now_us(void);                                        /* CLOCK_MONOTONIC (the kernel clock ticks in ms) */

typedef struct KStepper {
    int64_t step_us;                                             /* length of one logic step */
    int64_t acc_us;                                              /* time received but not yet consumed by steps */
    uint64_t last_us;
    int max_steps;                                               /* catch-up limit per call; the rest is dropped (slow motion instead of a spiral) */
    bool started;
    int last_steps;                                              /* what the latest kg_step_due() returned */
    int dropped;                                                 /* total steps dropped because the machine is too slow */
} KStepper;
void kg_step_init(KStepper *st, int step_us, int max_steps);
void kg_step_set(KStepper *st, int step_us);                     /* change the step length (snake speed levels) */
void kg_step_reset(KStepper *st);                                /* forget the accumulated time (after a pause) */
int  kg_step_due(KStepper *st);                                  /* logic steps to run now (0 .. max_steps) */
int  kg_step_alpha256(const KStepper *st);                       /* 0..256: how far we are into the next step (for interpolation) */
int  kg_step_wait_ms(const KStepper *st);                        /* milliseconds until the next step is due, at least 1 */

typedef struct KFps {
    uint64_t t0;
    int frames;
    uint64_t work_us;
    int fps10;                                                   /* frames per second * 10 over the last full second */
    int work_us_avg;                                             /* average time spent per rendered frame, microseconds */
    int work_us_max;
    int max_acc;
} KFps;
void kg_fps_frame(KFps *f, uint64_t work_us);                    /* call once per rendered frame with the time it took */

/* ---------------------------------------------------------------- settings files in ~/.config */
bool  kg_conf_path(char *out, size_t cap, const char *name);     /* "$HOME/.config/<name>"; creates the directory */
char *kg_conf_load(const char *name, size_t *len);               /* malloc'd, NUL-terminated; NULL when missing */
bool  kg_conf_save(const char *name, const void *data, size_t len);   /* writes <name>.tmp and renames it over the file */

/* ---------------------------------------------------------------- high-score tables */
#define KG_SCORE_NAME 20
#define KG_SCORE_MAX  96

typedef struct KScoreEntry {
    int category;                                                /* difficulty, board size, game mode ... */
    long score;
    long extra[2];                                               /* game specific (stage reached, shot type, seconds ...) */
    char name[KG_SCORE_NAME];
} KScoreEntry;

typedef struct KScoreTable {
    KScoreEntry e[KG_SCORE_MAX];
    int n;
    bool ascending;                                              /* lower is better (times) */
    int per_category;                                            /* entries kept per category */
} KScoreTable;

void kg_scores_init(KScoreTable *t, bool ascending, int per_category);
int  kg_scores_rank(const KScoreTable *t, int category, long score);          /* 0-based place the score would take, -1 if it would not make the table */
int  kg_scores_add(KScoreTable *t, int category, long score, const char *name, long extra0, long extra1);   /* returns the place or -1 */
int  kg_scores_list(const KScoreTable *t, int category, const KScoreEntry **out, int max);   /* the category's entries, best first */
long kg_scores_best(const KScoreTable *t, int category, long none);           /* best score of a category, or `none` */
size_t kg_scores_format(const KScoreTable *t, char *buf, size_t cap);         /* text form, one entry per line */
bool kg_scores_parse(KScoreTable *t, const char *text);                       /* replaces the contents; ignores malformed lines */
bool kg_scores_load(KScoreTable *t, const char *file);                        /* from ~/.config/<file>; false if there is none */
bool kg_scores_save(const KScoreTable *t, const char *file);

/* ---------------------------------------------------------------- small helpers */
int  kg_isqrt(int v);                                            /* integer square root */
void kg_format_int(char *out, size_t cap, long v);               /* 1234567 -> "1,234,567" */

#endif
