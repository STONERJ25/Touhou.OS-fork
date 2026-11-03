/* Where a tooltip goes and how its text breaks: pure functions (no toolkit, no drawing), so that tests/host/test_kappa_tooltip.py can check them over
 * every pointer position and a range of window sizes.
 *
 * A tooltip is drawn inside the window that asked for it (the window server has no tooltip windows for Kappa yet), so it has to fit in that window:
 * the panel is 36 pixels high and a Calculator window 220 pixels wide, and a tip placed "below and right of the pointer, or above it" used to be cut off there
 * (2026-11-03, the tip of the Notebook launcher: its top 8 pixels were missing). The rules now:
 *   - the box is never wider or taller than the window (the caller wraps long text to tooltip_max_text_width());
 *   - it sits below and right of the pointer when there is room, else above the pointer, else as high as the window allows (the panel's tip lands at its
 *     top edge, beside the pointer), and is always moved back inside the window on every side.
 */
#ifndef KAPPA_TOOLTIP_H
#define KAPPA_TOOLTIP_H
#include <stddef.h>
#include <string.h>

#define TIP_PAD_X     5            /* text inset inside the box, left and right */
#define TIP_PAD_Y     3
#define TIP_LINE_H    16           /* the bitmap font's line height: one line makes the 22 pixel box the toolkit always drew */
#define TIP_LINES     5            /* at most this many lines; the last one ends in "..." when there is more */
#define TIP_LINE_MAX  160

typedef struct { int x, y, w, h; } TipBox;

static inline int tooltip_box_height(int lines) { return 2 * TIP_PAD_Y + TIP_LINE_H * lines; }

/* the widest a line of text may be in a window of width bw */
static inline int tooltip_max_text_width(int bw) {
    int w = bw - 2 * TIP_PAD_X - 4;
    return w < 8 ? 8 : w;
}

/* The box for a tip w x h pixels (frame included) in a window bw x bh whose pointer is at (mx, my). */
static inline TipBox tooltip_place(int bw, int bh, int mx, int my, int w, int h) {
    TipBox b;
    if (w > bw) w = bw;
    if (h > bh) h = bh;
    int x = mx + 14, y = my + 18;                 /* below and right of the pointer */
    if (x + w > bw) x = bw - w;
    if (x < 0) x = 0;
    if (y + h > bh) y = my - h - 4;               /* no room below: above the pointer */
    if (y + h > bh) y = bh - h;
    if (y < 0) y = 0;                             /* no room above either (a panel): as high as the window allows */
    b.x = x; b.y = y; b.w = w; b.h = h;
    return b;
}

typedef int (*TipWidthFn)(const char *text);

/* Breaks TEXT into lines of at most MAXW pixels (as WIDTH measures them) at the spaces; a word wider than a line is cut where it overflows. At most
 * MAXLINES lines of TIP_LINE_MAX bytes; when text is left over the last line ends in "...". Returns the number of lines (at least 1 for a text with any
 * character, 0 for an empty one). */
static inline int tooltip_wrap(const char *text, int maxw, TipWidthFn width, char lines[][TIP_LINE_MAX], int maxlines) {
    int n = 0;
    const char *p = text;
    while (*p == ' ') p++;
    while (*p && n < maxlines) {
        char *line = lines[n];
        size_t len = 0;
        line[0] = 0;
        while (*p) {
            const char *e = p;
            while (*e && *e != ' ') e++;
            size_t wl = (size_t)(e - p), sep = len ? 1 : 0;
            if (len + sep + 1 >= TIP_LINE_MAX) break;                                    /* the buffer is full: the rest goes on the next line */
            if (len + sep + wl >= TIP_LINE_MAX) wl = TIP_LINE_MAX - 1 - len - sep;       /* cannot happen with sane tips; keeps the buffer whole */
            char trial[TIP_LINE_MAX];
            memcpy(trial, line, len);
            if (sep) trial[len] = ' ';
            memcpy(trial + len + sep, p, wl);
            trial[len + sep + wl] = 0;
            if (width(trial) <= maxw) {                 /* the word fits on this line */
                memcpy(line, trial, len + sep + wl + 1);
                len += sep + wl;
                p += wl;
                while (*p == ' ') p++;
                continue;
            }
            if (len) break;                              /* it fits on the next line */
            size_t cut = wl;                             /* one word wider than a whole line: cut it where it overflows */
            while (cut > 1) {
                trial[cut] = 0;
                if (width(trial) <= maxw) break;
                cut--;
            }
            memcpy(line, trial, cut);
            line[cut] = 0;
            len = cut;
            p += cut;
            break;
        }
        n++;
    }
    if (*p && n > 0) {                                   /* text left over: say so */
        char *last = lines[n - 1];
        size_t len = strlen(last);
        while (len > 0 && width(last) + width("...") > maxw && (last[--len] = 0, 1)) { }
        if (len + 4 < TIP_LINE_MAX) strcpy(last + len, "...");
    }
    return n;
}

#endif
