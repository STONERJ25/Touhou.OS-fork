/* The Minesweeper board: pure game logic, no drawing. Fairies hide under the grass instead of mines. */
#ifndef MS_BOARD_H
#define MS_BOARD_H

#include <stdint.h>
#include <stdbool.h>
#include <kappa_gamekit.h>

#define MS_MAX_W 30
#define MS_MAX_H 24

enum { CELL_COVERED = 0, CELL_OPEN = 1, CELL_FLAG = 2 };
enum { ST_READY, ST_PLAYING, ST_WON, ST_LOST };

typedef struct Board {
    int w, h, mines;
    uint8_t mine[MS_MAX_W * MS_MAX_H];      /* 1 = a fairy hides here */
    uint8_t cell[MS_MAX_W * MS_MAX_H];      /* CELL_* */
    uint8_t adj[MS_MAX_W * MS_MAX_H];       /* fairies in the 8 neighbours */
    bool placed;                            /* fairies are placed on the first click so that it is always safe */
    int opened, flags;
    int status;                             /* ST_* */
    int hit_x, hit_y;                       /* the fairy that was uncovered (when lost) */
    KRng rng;
} Board;

void board_new(Board *b, int w, int h, int mines, uint32_t seed);
void board_clamp(int *w, int *h, int *mines);                   /* forces a custom size into the allowed range */
int  board_reveal(Board *b, int x, int y);                      /* cells opened (0 if nothing happened, -1 if a fairy was uncovered) */
void board_flag(Board *b, int x, int y);                        /* toggles an ofuda on a covered cell */
int  board_chord(Board *b, int x, int y);                       /* opens the neighbours of a satisfied number; -1 if a flag was wrong */
int  board_flags_around(const Board *b, int x, int y);
void board_place_for_test(Board *b, const uint8_t *mine_map);   /* fixed layout (tests); marks the board as placed */
int  board_count_mines(const Board *b);

static inline int board_idx(const Board *b, int x, int y) { return y * b->w + x; }
static inline bool board_in(const Board *b, int x, int y) { return x >= 0 && y >= 0 && x < b->w && y < b->h; }

#endif
