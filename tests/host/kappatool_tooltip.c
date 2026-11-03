/* Checks of user/lib/kappa/src/tooltip.h (the placement and the wrapping of Kappa's tooltips): see tests/host/test_kappa_tooltip.py. A character is 8 pixels wide here. */
#include "../../user/lib/kappa/src/tooltip.h"
#include <stdio.h>
#include <stdlib.h>

static int w8(const char *s) { return (int)strlen(s) * 8; }

static int failures;
#define CHECK(c, ...) do { if (!(c)) { if (failures++ < 20) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static void placement(int bw, int bh, int w, int h, int step) {
    for (int my = 0; my < bh; my += step)
        for (int mx = 0; mx < bw; mx += step) {
            TipBox b = tooltip_place(bw, bh, mx, my, w, h);
            CHECK(b.w <= bw && b.h <= bh, "window %dx%d tip %dx%d: the box %dx%d is bigger than the window", bw, bh, w, h, b.w, b.h);
            CHECK(b.x >= 0 && b.y >= 0 && b.x + b.w <= bw && b.y + b.h <= bh,
                  "window %dx%d tip %dx%d pointer %d,%d: the box %d,%d %dx%d is not inside the window", bw, bh, w, h, mx, my, b.x, b.y, b.w, b.h);
            if (mx + 14 + w <= bw && my + 18 + h <= bh)
                CHECK(b.x == mx + 14 && b.y == my + 18, "window %dx%d tip %dx%d pointer %d,%d: room below and right, but the box is at %d,%d", bw, bh, w, h, mx, my, b.x, b.y);
            if (my + 18 + h > bh && my - h - 4 >= 0 && h <= bh)
                CHECK(b.y == my - h - 4, "window %dx%d tip %dx%d pointer %d,%d: no room below, room above, but y is %d", bw, bh, w, h, mx, my, b.y);
        }
}

static int total_len(char lines[][TIP_LINE_MAX], int n) {
    int t = 0;
    for (int i = 0; i < n; i++) t += (int)strlen(lines[i]);
    return t;
}

int main(void) {
    /* every pointer position, windows from the panel's strip to a screen, tips from one word to a sentence */
    static const int win[][2] = { {1280, 36}, {200, 36}, {1024, 36}, {220, 300}, {1024, 768}, {80, 40}, {30, 20}, {400, 22}, {120, 60}, {640, 480} };
    static const int tip[][2] = { {60, 22}, {120, 22}, {210, 38}, {400, 22}, {200, 70} };
    for (unsigned i = 0; i < sizeof win / sizeof win[0]; i++)
        for (unsigned j = 0; j < sizeof tip / sizeof tip[0]; j++) placement(win[i][0], win[i][1], tip[j][0], tip[j][1], win[i][0] * win[i][1] > 100000 ? 7 : 1);

    /* the panel, as it was reported: 36 pixels high, the pointer on the middle of an icon, a 22 pixel tip */
    TipBox b = tooltip_place(1280, 36, 70, 18, 80, 22);
    CHECK(b.y == 0 && b.h == 22 && b.x == 84, "the panel's tip is at %d,%d %dx%d", b.x, b.y, b.w, b.h);

    char lines[TIP_LINES][TIP_LINE_MAX];
    int n;

    n = tooltip_wrap("Notebook", 100, w8, lines, TIP_LINES);
    CHECK(n == 1 && !strcmp(lines[0], "Notebook"), "a short tip stays whole: %d lines, '%s'", n, lines[0]);

    const char *sentence = "Click: reuse the expression - right click: use the result";
    for (int maxw = 80; maxw <= 480; maxw += 8) {
        n = tooltip_wrap(sentence, maxw, w8, lines, TIP_LINES);
        int widest = 0;
        for (int i = 0; i < n; i++) if (w8(lines[i]) > widest) widest = w8(lines[i]);
        CHECK(n >= 1 && n <= TIP_LINES && widest <= maxw, "wrap at %d: %d lines, widest %d", maxw, n, widest);
        char joined[400] = "";
        for (int i = 0; i < n; i++) { if (i) strcat(joined, " "); strcat(joined, lines[i]); }
        if (strncmp(joined + strlen(joined) - 3, "...", 3)) CHECK(!strcmp(joined, sentence), "wrap at %d lost words: '%s'", maxw, joined);
    }

    n = tooltip_wrap("Supercalifragilisticexpialidocious", 80, w8, lines, TIP_LINES);
    CHECK(n == 4 && total_len(lines, n) == 34 && w8(lines[0]) <= 80, "a word wider than the line is cut: %d lines, %d characters", n, total_len(lines, n));

    const char *many = "one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen eighteen nineteen twenty";
    n = tooltip_wrap(many, 100, w8, lines, TIP_LINES);
    CHECK(n == TIP_LINES && !strcmp(lines[n - 1] + strlen(lines[n - 1]) - 3, "...") && w8(lines[n - 1]) <= 100, "too much text: %d lines, last '%s'", n, lines[n - 1]);

    CHECK(tooltip_wrap("", 100, w8, lines, TIP_LINES) == 0, "an empty tip has no lines");
    CHECK(tooltip_wrap("    ", 100, w8, lines, TIP_LINES) == 0, "a tip of spaces has no lines");
    n = tooltip_wrap("   padded   words  ", 200, w8, lines, TIP_LINES);
    CHECK(n == 1 && !strcmp(lines[0], "padded words"), "spaces are squeezed: '%s'", lines[0]);

    n = tooltip_wrap("a b c d e f g h", 8, w8, lines, TIP_LINES);                  /* a line of one character: no endless loop, no overflow */
    CHECK(n == TIP_LINES && w8(lines[0]) <= 8, "one character a line: %d lines", n);

    char big[400];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    n = tooltip_wrap(big, 100000, w8, lines, TIP_LINES);                           /* longer than a line buffer: whole, bounded */
    CHECK(n >= 1 && n <= TIP_LINES && (int)strlen(lines[0]) < TIP_LINE_MAX, "a 399 character word: %d lines", n);

    CHECK(tooltip_box_height(1) == 22, "one line makes the 22 pixel box the toolkit always drew: %d", tooltip_box_height(1));
    CHECK(tooltip_max_text_width(10) == 8 && tooltip_max_text_width(36) == 22 && tooltip_max_text_width(220) == 206, "text widths: %d %d %d",
          tooltip_max_text_width(10), tooltip_max_text_width(36), tooltip_max_text_width(220));

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("OK\n");
    return 0;
}
