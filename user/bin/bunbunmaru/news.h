/* Bunbunmaru News: the data of one issue, and the places it comes from. */
#ifndef BUN_NEWS_H
#define BUN_NEWS_H

#include "layout.h"
#include <stddef.h>
#include <stdint.h>

/* the newsprint palette */
#define PAGE_BG     0xF3EEDC
#define PAGE_INK    0x1D1B22
#define PAGE_DIM    0x5E5A4A
#define PAGE_ACCENT 0xA8112A
#define PAGE_RULE   0x3A372E

extern bool g_news_trace;
void news_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---------------------------------------------------------------- the fixed material (content.c) */
typedef struct Ad { const char *text; } Ad;
typedef struct SpellCard {
    const char *name;                   /* "Tea-Time Sign 'Perfectly Steeped Afternoon'" */
    const char *owner;                  /* who casts it */
    const char *rating;                 /* Gentle, Tricky, Fiendish, Absurd */
    const char *text;                   /* what it does, in the paper's voice */
    int pattern;                        /* danmaku picture: see spellcard.c */
    int a, b, c;                        /* pattern parameters */
} SpellCard;
typedef struct Word { const char *word, *clue; } Word;

extern const Article g_fan_news[];      /* the pool: at least 40 */
extern const int g_nfan_news;
extern const Ad g_ads[];
extern const int g_nads;
extern const SpellCard g_cards[];
extern const int g_ncards;
extern const char *const g_zodiac[12];
extern const char *const g_zodiac_item[12];
extern const char *const g_fortunes[];
extern const int g_nfortunes;
extern const char *const g_lucky_items[];
extern const int g_nlucky;
extern const char *const g_weather[];
extern const int g_nweather;
extern const char *const g_letters[];
extern const int g_nletters;
extern const char *const g_overheard[];
extern const int g_noverheard;
extern const char *const g_tips[];
extern const int g_ntips;
extern const Word g_words[];
extern const int g_nwords;

/* ---------------------------------------------------------------- the crossword (puzzle.c) */
#define PZ_MAX 13
typedef struct Puzzle {
    int w, h;                           /* trimmed size of the grid */
    char sol[PZ_MAX][PZ_MAX];           /* solution letters, 0 = block */
    char fill[PZ_MAX][PZ_MAX];          /* what the reader typed, 0 = empty */
    int num[PZ_MAX][PZ_MAX];            /* clue number starting here, 0 = none */
    int nwords;
    struct { int num; bool across; int x, y, len; const char *clue; const char *word; } words[16];
    int sel_x, sel_y;                   /* selected cell, -1 when the grid has no focus */
    bool across;                        /* typing direction */
    bool checked;                       /* show wrong letters in red */
    bool solved;
} Puzzle;
void puzzle_build(Puzzle *p, long daynum);
void puzzle_draw(KUI *ui, KRect r, Puzzle *p);
int  puzzle_height(const Puzzle *p, int width);
bool puzzle_key(Puzzle *p, const KKey *k);                  /* returns true when the key was used */
void puzzle_reveal(Puzzle *p);
void puzzle_check(Puzzle *p);
void puzzle_clear(Puzzle *p);
void puzzle_dump(const Puzzle *p);

/* ---------------------------------------------------------------- spell card pictures (spellcard.c) */
void spellcard_picture(KUI *ui, KRect r, const SpellCard *c, long daynum);

/* ---------------------------------------------------------------- the issue (issue.c) */
#define ISSUE_POOL 40000
#define MAX_SYS 12
#define N_PAGES 5
typedef struct Issue {
    int year, month, day, weekday;
    long daynum;
    int yday;
    char date_line[128];
    char user_name[48];                 /* who the paper is delivered to */
    Article sys[MAX_SYS];
    int nsys;
    int sys_lead;                       /* index of the lead story in sys[] */
    const Article *fan[24];             /* chosen for today */
    int nfan;
    const Ad *ads[10];
    int nads;
    const SpellCard *card;
    int weather;
    int temp_hi, temp_lo;
    int tip;
    int letters[3];
    int overheard[3];
    Puzzle puzzle;
    char pool[ISSUE_POOL];              /* the text of the system stories lives here */
    size_t pool_len;
} Issue;

void issue_build(Issue *is, int y, int m, int d);          /* the date picks the stories, the machine supplies the system news */
const char *issue_page_name(int page);
void issue_print(const Issue *is);                          /* the whole paper as plain text (bunbunmaru --print) */

/* ---------------------------------------------------------------- system news (system_news.c) */
void sysnews_build(Issue *is);

/* ---------------------------------------------------------------- pages (pages.c) */
void pages_build(Layout *L, const Issue *is, int page, int width);

/* small helpers shared by the sources */
char *pool_printf(Issue *is, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
uint32_t hash32(uint32_t x);
uint32_t rng_next(uint32_t *state);
void shuffle_indices(int *idx, int n, uint32_t seed);

#endif
