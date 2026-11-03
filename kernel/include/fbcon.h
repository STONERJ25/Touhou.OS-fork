/* Text console drawn on the framebuffer: cell grid, UTF-8, ANSI SGR colours and basic cursor control. */
#ifndef K_FBCON_H
#define K_FBCON_H

#include "types.h"

void fbcon_init(void);
void fbcon_write(const char *s, size_t n);
void fbcon_clear(void);
void fbcon_set_enabled(bool on);            /* off: keep the cell grid current but stop drawing */
bool fbcon_enabled(void);
void fbcon_redraw(void);
void fbcon_force_on(void);                  /* panic path: take the screen back from the window server and repaint it */
void fbcon_taint_rect(int x, int y, int w, int h);   /* pixels somebody drew over the grid (the boot banner): repaint the cells when they scroll */
u64  fbcon_speed_probe(u64 tsc_mhz);        /* MB/s of a timed fill of the bottom text rows (repainted afterwards); 0 = not measurable */
void fbcon_set_fg(u32 rgb);
void fbcon_reset_colors(void);
void fbcon_size(int *cols, int *rows);
int  fbcon_selftest(void);                  /* "selftest" on the command line: the number of failed checks (0 = the screen always equalled the grid) */

#endif
