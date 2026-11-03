/* Status messages, one-line prompts, yes/no questions and the help screen. */
#include "pnote.h"

void set_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.msg, sizeof E.msg, fmt, ap);
    va_end(ap);
}

/* Waits for a key. While idle it keeps an eye on the window size, so a resized console is repainted. */
int next_key(void) {
    for (;;) {
        int k = pt_term_getkey(&E.term, 250);
        if (k == PT_KEY_NONE) {
            if (pt_term_size(&E.term)) { pts_invalidate(&E.scr); view_draw(); }
            continue;
        }
        return k;
    }
}

/* Edits a line of text on the status row. Returns 1 when Enter ends it, 0 when it is cancelled. `onkey` may claim
 * keys (it can also change the label). */
int prompt_line(char *label, size_t labcap, char *buf, size_t cap, int (*onkey)(int key, char *label, size_t labcap)) {
    size_t len = strlen(buf), cur = len;
    int ok = 0;
    for (;;) {
        E.prompt_label = label;
        E.prompt_text = buf;
        E.prompt_cur = cur;
        view_draw();
        int k = next_key();
        if (k == PT_KEY_EOF) break;
        if (onkey && onkey(k, label, labcap)) continue;
        if (k == PT_K_ENTER) { ok = 1; break; }
        if (k == PT_K_ESC || k == PT_CTRL('c') || k == PT_CTRL('g')) break;
        if (k == PT_K_BACKSPACE) {
            if (cur > 0) {
                size_t p = pt_u8_prev(buf, cur);
                memmove(buf + p, buf + cur, len - cur + 1);
                len -= cur - p;
                cur = p;
            }
        } else if (k == PT_K_DEL || k == PT_CTRL('d')) {
            if (cur < len) {
                size_t n = (size_t)pt_u8_next(buf, len, cur);
                memmove(buf + cur, buf + cur + n, len - cur - n + 1);
                len -= n;
            }
        } else if (k == PT_K_LEFT || k == PT_CTRL('b')) {
            if (cur > 0) cur = pt_u8_prev(buf, cur);
        } else if (k == PT_K_RIGHT || k == PT_CTRL('f')) {
            if (cur < len) cur += (size_t)pt_u8_next(buf, len, cur);
        } else if (k == PT_K_HOME || k == PT_CTRL('a')) {
            cur = 0;
        } else if (k == PT_K_END || k == PT_CTRL('e')) {
            cur = len;
        } else if (k == PT_CTRL('k')) {
            buf[cur] = 0;
            len = cur;
        } else if (k == PT_CTRL('u')) {
            memmove(buf, buf + cur, len - cur + 1);
            len -= cur;
            cur = 0;
        } else if (k >= 0x20 && k < PT_K_BASE && !(k & PT_M_ALT) && !(k >= 0x7F && k < 0xA0)) {
            char u[4];
            int l = pt_u8_encode((uint32_t)k, u);
            if (len + (size_t)l + 1 < cap) {
                memmove(buf + cur + l, buf + cur, len - cur + 1);
                memcpy(buf + cur, u, (size_t)l);
                cur += (size_t)l;
                len += (size_t)l;
            }
        }
    }
    E.prompt_label = NULL;
    E.prompt_text = NULL;
    return ok;
}

/* Shows a question on the status row and waits for one of the given keys (matched case-insensitively).
 * Returns the lower-case key, or 0 if the question was cancelled with Esc, ^C or ^G. */
int ask_key(const char *question, const char *keys) {
    static const char empty[] = "";
    for (;;) {
        E.prompt_label = question;
        E.prompt_text = empty;
        E.prompt_cur = 0;
        view_draw();
        int k = next_key();
        E.prompt_label = NULL;
        E.prompt_text = NULL;
        if (k == PT_KEY_EOF || k == PT_K_ESC || k == PT_CTRL('c') || k == PT_CTRL('g')) return 0;
        if (k < 0x80 && k > 0) {
            int c = tolower(k);
            if (strchr(keys, c)) return c;
        }
    }
}

/* ------------------------------------------------------------------ help */

static const char *const help_text[] = {
    "Patchouli's Notebook",
    "A grimoire for plain text. The library is quiet; the incantations below are exact.",
    "",
    "#FILES",
    "  ^S            Save the notebook (asks for a name if it has none)",
    "  ^X            Exit; if there are unsaved changes it asks: Y saves, N discards, Esc stays",
    "  ^O            Insert another file at the cursor",
    "",
    "#MOVING",
    "  Arrows, Home, End, PgUp, PgDn          move, as you would expect",
    "  ^A  ^E        start of the line / end of the line",
    "  ^B  ^F        left / right",
    "  Alt-B  Alt-F  previous word / next word (Ctrl-Left and Ctrl-Right on an xterm)",
    "  ^L            go to a line: a number, or LINE,COLUMN",
    "  ^V  Alt-V     page down / page up",
    "  Alt-\\  Alt-/  first line / last line (Ctrl-Home and Ctrl-End on an xterm)",
    "",
    "#EDITING",
    "  ^K            cut the current line; pressing ^K again adds the next line to the cut buffer",
    "  ^U            paste the cut buffer (cut lines go above the current line)",
    "  Alt-A         start selecting; move to the other end; ^K cuts, Alt-6 copies, Alt-A cancels",
    "  ^Z  ^Y        undo / redo (also Alt-U and Alt-E); the history is many levels deep",
    "  Tab           insert a tab (or spaces, see ^T)       Shift-Tab   unindent the line",
    "  ^D  Delete    delete the character under the cursor  Backspace   delete the one before it",
    "  Enter         new line, indented like the one above when auto-indent is on",
    "",
    "#SEARCHING",
    "  ^W            search forward (smartcase: an upper case letter makes it exact)",
    "  ^N  ^P        next match / previous match (also F3 and Alt-W)",
    "  ^R            search and replace: Y replaces, N skips, A replaces all, Esc stops",
    "  In the search prompt:  Alt-C turns case-sensitive matching on or off,",
    "                         Alt-R turns regular expressions (extended syntax) on or off.",
    "",
    "#OTHER",
    "  ^T            toggles: N line numbers, H highlighting, L language, A auto-indent,",
    "                T tabs as spaces, D CRLF/LF line endings",
    "  ^C            where am I: line, column and byte position",
    "  ^G  F1        this help                 F5  Alt-L   repaint the screen",
    "",
    "#COMMAND LINE",
    "  pnote [options] [+LINE[,COL]] [FILE]      pnote -   edits what arrives on standard input",
    "  -n line numbers   -S no highlighting   -I no auto-indent   -E tabs as spaces   -T N tab width",
    "  -R view only (no edits)   -l LANG  force c, sh or md highlighting   -V version",
    "  Files without a final newline, CRLF files and UTF-8 text are kept as they are.",
    "",
    "Nothing leaves this room unwritten. -- P.K.",
};

void show_help(void) {
    int n = (int)(sizeof help_text / sizeof help_text[0]);
    int top = 0;
    PtScreen *s = &E.scr;
    for (;;) {
        view_layout();
        int body = E.rows - 2;
        if (body < 1) body = 1;
        PtRow *t = pts_row(s, 0);
        pr_sgr(t, "1;37;41");
        pr_str(t, " Patchouli's Notebook  -  Help", E.cols);
        pr_spaces(t, E.cols - t->w);
        pr_reset(t);
        for (int r = 0; r < body; r++) {
            PtRow *row = pts_row(s, 1 + r);
            if (top + r < n) {
                const char *l = help_text[top + r];
                if (l[0] == '#') { pr_sgr(row, "1;31"); pr_str(row, l + 1, E.cols); pr_reset(row); }
                else if (top + r == 0) { pr_sgr(row, "1"); pr_str(row, l, E.cols); pr_reset(row); }
                else pr_str(row, l, E.cols);
            }
        }
        PtRow *f = pts_row(s, E.rows - 1);
        pr_sgr(f, "7");
        pr_str(f, " SPACE/PgDn next page   b/PgUp previous page   q or Esc: back to the notebook ", E.cols);
        pr_spaces(f, E.cols - f->w);
        pr_reset(f);
        pts_flush(s, &E.term, -1, 0, 0);
        int k = next_key();
        if (k == PT_KEY_EOF || k == 'q' || k == 'Q' || k == PT_K_ESC || k == PT_CTRL('g') || k == PT_CTRL('x')) break;
        if (k == ' ' || k == PT_K_PGDN || k == PT_CTRL('v')) { if (top + body < n) top += body - 1; }
        else if (k == PT_K_DOWN || k == PT_K_ENTER || k == 'j') { if (top + body < n) top++; }
        else if (k == 'b' || k == PT_K_PGUP) { top -= body - 1; if (top < 0) top = 0; }
        else if (k == PT_K_UP || k == 'k') { if (top > 0) top--; }
        else if (k == PT_K_HOME) top = 0;
    }
    pts_invalidate(s);
}
