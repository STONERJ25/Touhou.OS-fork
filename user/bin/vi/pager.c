/* pager.c: multi-line output (:reg, :marks, :g//p, :help) shown over the text with a "-- More --" prompt. */
#include "vi.h"

extern PtScreen scr;

void pager_show(const char *text) {
    pt_term_size(&term);
    if (term.rows != scr.rows || term.cols != scr.cols) pts_resize(&scr, term.rows, term.cols);
    int rows = term.rows, cols = term.cols;
    const char *p = text;
    for (;;) {
        int r = 0;
        while (r < rows - 1 && *p) {
            const char *e = strchr(p, '\n');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            PtRow *row = pts_row(&scr, r);
            pr_text(row, p, n, cols);
            r++;
            p += n;
            if (*p == '\n') p++;
        }
        for (; r < rows - 1; r++) pts_row(&scr, r);
        PtRow *pr = pts_row(&scr, rows - 1);
        const char *prompt = *p ? "-- More --" : "Press ENTER or type command to continue";
        pr_str(pr, prompt, cols - 1);
        int cc = (int)strlen(prompt);
        pts_flush(&scr, &term, rows - 1, cc < cols ? cc : cols - 1, 1);
        int k = getkey();
        if (!*p || k == 'q' || k == K_ESC || k == 3) break;
    }
    force_repaint();
}
