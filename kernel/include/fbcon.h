/* Text console drawn on the framebuffer: cell grid, UTF-8, ANSI SGR colours and basic cursor control. */
#ifndef K_FBCON_H
#define K_FBCON_H

#include "types.h"

void fbcon_init(void);
void fbcon_write(const char *s, size_t n);
void fbcon_clear(void);
void fbcon_set_enabled(bool on);            /* off: keep the cell buffer current but stop drawing */
bool fbcon_enabled(void);
void fbcon_redraw(void);
void fbcon_set_fg(u32 rgb);
void fbcon_reset_colors(void);
void fbcon_size(int *cols, int *rows);

#endif
