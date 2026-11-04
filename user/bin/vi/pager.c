/* pager.c: multi-line output (:reg, :marks, :g//p, :help) shown over the text. A screenful or less ends with "Press ENTER or type
 * command to continue" (Enter, Space and Ctrl-C dismiss it, any other key is put back and runs as a command, like Vim); longer
 * output shows "-- More --" and scrolls with Enter / j (a line), Space / f (a page), d (half a page), b / k / u (back), q. */
#include "vi.h"

extern PtScreen scr;

void pager_show(const char *text) {
    pt_term_size(&term);
    if (term.rows != scr.rows || term.cols != scr.cols) pts_resize(&scr, term.rows, term.cols);
    int rows = term.rows, cols = term.cols, h = rows - 1;
    if (h < 1) h = 1;
    size_t n = 0, cap = 64;
    size_t *start = malloc(cap * sizeof *start);                 /* where each line begins */
    if (!start) pt_oom();
    for (const char *p = text; *p;) {
        if (n == cap) {
            cap *= 2;
            start = realloc(start, cap * sizeof *start);
            if (!start) pt_oom();
        }
        start[n++] = (size_t)(p - text);
        const char *e = strchr(p, '\n');
        p = e ? e + 1 : p + strlen(p);
    }
    long top = 0, last_top = n > (size_t)h ? (long)n - h : 0;
    const char *hint = NULL;
    for (;;) {
        int r = 0;
        for (long i = top; r < h && i < (long)n; i++, r++) {
            const char *p = text + start[i];
            const char *e = strchr(p, '\n');
            pr_text(pts_row(&scr, r), p, e ? (size_t)(e - p) : strlen(p), cols);
        }
        for (; r < h; r++) pts_row(&scr, r);
        int final = top >= last_top;
        const char *prompt = hint ? hint : final ? "Press ENTER or type command to continue" : "-- More --";
        pr_str(pts_row(&scr, rows - 1), prompt, cols - 1);
        int cc = (int)strlen(prompt);
        pts_flush(&scr, &term, rows - 1, cc < cols ? cc : cols - 1, 1);
        int k = getkey_plain();
        hint = NULL;
        if (final) {
            if (k != K_ENTER && k != ' ' && k != K_ESC && k != 3 && k != K_EOF) { int kk = k | K_NOMAP; ta_push(&kk, 1); }
            break;
        }
        if (k == 'q' || k == K_ESC || k == 3 || k == K_EOF) break;
        if (k == ':') { int kk = k | K_NOMAP; ta_push(&kk, 1); break; }
        if (k == K_ENTER || k == 'j' || k == PT_K_DOWN) top++;
        else if (k == ' ' || k == 'f' || k == PT_K_PGDN) top += h;
        else if (k == 'd') top += h / 2 > 0 ? h / 2 : 1;
        else if (k == 'k' || k == PT_K_UP) top--;
        else if (k == 'b' || k == PT_K_PGUP) top -= h;
        else if (k == 'u') top -= h / 2 > 0 ? h / 2 : 1;
        else hint = "-- More -- SPACE/d/j: screen/page/line down, b/u/k: up, q: quit";
        if (top > last_top) top = last_top;
        if (top < 0) top = 0;
    }
    free(start);
    force_repaint();
}
