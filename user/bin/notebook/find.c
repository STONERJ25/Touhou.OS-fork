/* Searching: forward and backward search with an optional case-insensitive mode, replace, and the find/replace bar. */
#include "notebook.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c; }

static bool match_at(const char *t, const char *pat, size_t plen, bool match_case) {
    if (match_case) return memcmp(t, pat, plen) == 0;
    for (size_t k = 0; k < plen; k++) if (lower((unsigned char)t[k]) != lower((unsigned char)pat[k])) return false;
    return true;
}

size_t nb_find(Doc *d, const char *pat, size_t plen, size_t from, bool match_case, bool backward) {
    size_t len = doc_len(d);
    if (!plen || plen > len) return SIZE_MAX;
    const char *t = doc_all(d);
    if (!backward) {
        unsigned char c0 = match_case ? (unsigned char)pat[0] : lower((unsigned char)pat[0]);
        size_t last = len - plen;
        for (size_t i = from; i <= last; i++) {
            unsigned char x = match_case ? (unsigned char)t[i] : lower((unsigned char)t[i]);
            if (x != c0) continue;
            if (plen == 1 || match_at(t + i + 1, pat + 1, plen - 1, match_case)) return i;
        }
        return SIZE_MAX;
    }
    size_t i = from > len - plen ? len - plen : from;
    for (;;) {
        if (match_at(t + i, pat, plen, match_case)) return i;
        if (i == 0) break;
        i--;
    }
    return SIZE_MAX;
}

int nb_count_matches(Doc *d, const char *pat, size_t plen, bool match_case, int limit) {
    int n = 0;
    size_t from = 0;
    while (n < limit) {
        size_t m = nb_find(d, pat, plen, from, match_case, false);
        if (m == SIZE_MAX) break;
        n++;
        from = m + plen;
    }
    return n;
}

static void show_match(size_t m, size_t plen) {
    ed_set_selection(&A.ed, m, m + plen);
    A.ed.blink_ms = k_now_ms();
}

bool find_next(bool backward, bool quiet) {
    Editor *e = &A.ed;
    size_t plen = strlen(A.find_buf);
    if (!plen) { if (!quiet) msg_set("Type something to look for."); return false; }
    size_t lo, hi;
    ed_selection(e, &lo, &hi);
    size_t m = SIZE_MAX;
    bool wrapped = false;
    if (!backward) {
        m = nb_find(&e->doc, A.find_buf, plen, hi, A.match_case, false);
        if (m == SIZE_MAX && hi > 0) { m = nb_find(&e->doc, A.find_buf, plen, 0, A.match_case, false); wrapped = m != SIZE_MAX; }
    } else {
        if (lo > 0) m = nb_find(&e->doc, A.find_buf, plen, lo - 1, A.match_case, true);
        if (m == SIZE_MAX) { m = nb_find(&e->doc, A.find_buf, plen, SIZE_MAX, A.match_case, true); wrapped = m != SIZE_MAX; }
    }
    if (m == SIZE_MAX) {
        if (!quiet) msg_set("No match for \"%.60s\".", A.find_buf);
        A.match_count = 0;
        return false;
    }
    show_match(m, plen);
    if (wrapped) msg_set(backward ? "Wrapped around to the end." : "Wrapped around to the top.");
    nb_trace("found at %zu", m);
    return true;
}

/* While typing in the find field: keep the current match if it still matches, otherwise move to the next one. */
static void find_live(void) {
    Editor *e = &A.ed;
    size_t plen = strlen(A.find_buf);
    if (!plen) return;
    size_t lo, hi;
    ed_selection(e, &lo, &hi);
    size_t m = nb_find(&e->doc, A.find_buf, plen, lo, A.match_case, false);
    if (m == SIZE_MAX && lo > 0) m = nb_find(&e->doc, A.find_buf, plen, 0, A.match_case, false);
    if (m != SIZE_MAX) show_match(m, plen);
    else msg_set("No match for \"%.60s\".", A.find_buf);
}

bool replace_current(void) {
    Editor *e = &A.ed;
    if (A.readonly) { msg_set("Read-only file: use Save As to keep a copy."); return false; }
    size_t plen = strlen(A.find_buf), rlen = strlen(A.repl_buf);
    if (!plen) { msg_set("Type something to look for."); return false; }
    size_t lo, hi;
    ed_selection(e, &lo, &hi);
    bool is_match = false;
    if (hi - lo == plen) {
        char tmp[256];
        doc_get(&e->doc, lo, plen, tmp);
        is_match = match_at(tmp, A.find_buf, plen, A.match_case);
    }
    if (!is_match) return find_next(false, false);
    ed_insert_text(e, A.repl_buf, rlen);
    A.count_valid = false;
    find_next(false, true);
    return true;
}

int replace_all(void) {
    Editor *e = &A.ed;
    Doc *d = &e->doc;
    if (A.readonly) { msg_set("Read-only file: use Save As to keep a copy."); return 0; }
    size_t plen = strlen(A.find_buf), rlen = strlen(A.repl_buf);
    if (!plen) { msg_set("Type something to look for."); return 0; }
    size_t first = nb_find(d, A.find_buf, plen, 0, A.match_case, false);
    if (first == SIZE_MAX) { msg_set("No match for \"%.60s\".", A.find_buf); return 0; }
    int count = nb_count_matches(d, A.find_buf, plen, A.match_case, INT_MAX);
    size_t len = doc_len(d);
    size_t cap = (len - first) + (size_t)count * (rlen > plen ? rlen - plen : 0) + 8;
    char *out = malloc(cap);
    if (!out) { msg_set("Out of memory: Yuyuko is hungry."); return 0; }
    const char *t = doc_all(d);
    size_t o = 0, last_end = first, pos = first;
    for (int i = 0; i < count; i++) {
        size_t m = nb_find(d, A.find_buf, plen, pos, A.match_case, false);
        if (m == SIZE_MAX) break;
        nb_memmove(out + o, t + last_end, m - last_end);
        o += m - last_end;
        nb_memmove(out + o, A.repl_buf, rlen);
        o += rlen;
        last_end = m + plen;
        pos = last_end;
    }
    ed_replace_range(e, first, last_end - first, out, o, first);
    free(out);
    A.count_valid = false;
    msg_set("Replaced %d occurrence%s.", count, count == 1 ? "" : "s");
    nb_trace("replaced %d", count);
    return count;
}

/* ---------------------------------------------------------------- the bar */

int findbar_height(void) { return !A.find_open ? 0 : A.repl_open ? 62 : 33; }

void find_open(bool replace) {
    Editor *e = &A.ed;
    A.find_open = true;
    if (replace) A.repl_open = true;
    size_t lo, hi;
    ed_selection(e, &lo, &hi);
    if (hi > lo && hi - lo < 120) {
        char tmp[128];
        doc_get(&e->doc, lo, hi - lo, tmp);
        tmp[hi - lo] = 0;
        if (!strchr(tmp, '\n')) { snprintf(A.find_buf, sizeof A.find_buf, "%s", tmp); A.count_valid = false; A.count_due = 1; }
    }
    snprintf(A.find_prev, sizeof A.find_prev, "%s", A.find_buf);
    A.want_find_focus = true;
    if (A.mainui) {                                             /* focus right away so keys typed in the same batch land in the field */
        k_focus_textfield(A.mainui, A.find_buf);
        A.mainui->sel_anchor = 0;
        A.want_find_focus = false;
    }
    nb_trace("find %s", replace ? "replace" : "open");
}

void find_close(void) {
    A.find_open = false;
    A.repl_open = false;
    A.want_editor_focus = true;
    if (A.mainui) { A.mainui->focus = ED_ID; A.want_editor_focus = false; }
}

static void update_count(KUI *ui) {
    size_t plen = strlen(A.find_buf);
    if (!plen) { A.match_count = 0; A.count_valid = true; A.count_ver = A.ed.doc.version; return; }
    if (A.count_valid && A.count_ver == A.ed.doc.version) return;
    if (A.count_due == 0) A.count_due = ui->now_ms + 300;
    if (ui->now_ms < A.count_due) { ui->redraw_soon = true; return; }
    A.match_count = nb_count_matches(&A.ed.doc, A.find_buf, plen, A.match_case, 99999);
    A.count_valid = true;
    A.count_ver = A.ed.doc.version;
    A.count_due = 0;
    nb_trace("find count=%d", A.match_count);
}


/* Focus requests made by key presses and clicks are applied at the start of the next frame, before keys are dispatched. */
void find_apply_focus(KUI *ui) {
    if (!A.find_open) { A.want_find_focus = A.want_repl_focus = A.want_find_keep = false; return; }
    if (A.want_find_focus) {
        k_focus_textfield(ui, A.find_buf);
        if (!A.want_find_keep) ui->sel_anchor = 0;
        A.want_find_focus = A.want_find_keep = false;
    }
    if (A.want_repl_focus) {
        k_focus_textfield(ui, A.repl_buf);
        ui->sel_anchor = 0;
        A.want_repl_focus = false;
    }
}
static bool take_tab(KUI *ui, bool *shift) {
    for (int i = 0; i < ui->nkeys; i++) {
        KKey *k = &ui->keys[i];
        if (k->down && k->code == KEY_TAB) { *shift = (k->mods & MOD_SHIFT) != 0; k->down = 0; k->code = 0; return true; }
    }
    return false;
}

void findbar_draw(KUI *ui, KRect r) {
    if (!A.find_open || r.h <= 0) return;
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    kc_fill(cv, r, th->window_bg);
    kc_hline(cv, r.x, r.y, r.w, kc_shade(th->window_bg, 72));
    kc_hline(cv, r.x, r.y + 1, r.w, 0xFFFFFF);
    int y1 = r.y + 5, lw = 66;
    int fx = r.x + 8 + lw;
    bool wide = r.w >= 600;

    /* right-hand controls of the first row */
    int right = r.x + r.w - 6;
    KRect closeb = kr(right - 22, y1, 22, 24);
    right = closeb.x - 6;
    int count_w = wide ? 96 : 0;
    KRect countr = kr(right - count_w, y1, count_w, 24);
    right = countr.x - (count_w ? 6 : 0);
    KRect casebox = kr(right - 102, y1 + 4, 102, 16);
    right = casebox.x - 8;
    KRect nextb = kr(right - 52, y1, 52, 24);
    KRect prevb = kr(nextb.x - 54, y1, 52, 24);
    int fw = prevb.x - 6 - fx;
    if (fw < 60) fw = 60;
    KRect f1 = kr(fx, y1, fw, 24);

    kc_text(cv, r.x + 8, y1 + 4, "Find:", th->text);
    find_apply_focus(ui);
    bool enter = k_textfield(ui, f1, A.find_buf, sizeof A.find_buf);
    bool find_focused = ui->focus == ((uint32_t)(uintptr_t)A.find_buf * 2654435761u | 1);
    if (strcmp(A.find_buf, A.find_prev)) {
        snprintf(A.find_prev, sizeof A.find_prev, "%s", A.find_buf);
        A.count_valid = false;
        A.count_due = 1;
        find_live();
    }
    if (enter) find_next((ui->mods & MOD_SHIFT) != 0, false);
    if (k_button(ui, prevb, "Prev")) find_next(true, false);
    if (k_button(ui, nextb, "Next")) find_next(false, false);
    bool before = A.match_case;
    k_checkbox(ui, casebox.x, casebox.y, "Match case", &A.match_case);
    if (before != A.match_case) { A.count_valid = false; A.count_due = 1; find_live(); A.want_find_focus = A.want_find_keep = true; }
    if (k_icon_button(ui, closeb, KI_CLOSE, false)) find_close();
    if (prevb.x > fx + 60) {
        if (k_hover(ui, prevb)) k_tooltip_text(ui, "Previous match (Shift+F3)");
        if (k_hover(ui, nextb)) k_tooltip_text(ui, "Next match (F3)");
        if (k_hover(ui, closeb)) k_tooltip_text(ui, "Close the find bar (Esc)");
    }

    update_count(ui);
    if (count_w) {
        char buf[48];
        size_t plen = strlen(A.find_buf);
        if (!plen) buf[0] = 0;
        else if (A.match_count == 0) snprintf(buf, sizeof buf, "No matches");
        else snprintf(buf, sizeof buf, "%d match%s", A.match_count, A.match_count == 1 ? "" : "es");
        kc_text_in(cv, countr, buf, plen && A.match_count == 0 ? th->accent : th->text_dim, KA_LEFT);
    }

    bool tab_shift = false;
    if (A.repl_open) {
        int y2 = r.y + 33;
        kc_text(cv, r.x + 8, y2 + 4, "Replace:", th->text);
        KRect rb_all = kr(r.x + r.w - 6 - 100, y2, 100, 24);
        KRect rb = kr(rb_all.x - 6 - 72, y2, 72, 24);
        int rw = rb.x - 6 - fx;
        if (rw < 60) rw = 60;
        bool renter = k_textfield(ui, kr(fx, y2, rw, 24), A.repl_buf, sizeof A.repl_buf);
        if (renter) replace_current();
        if (k_button(ui, rb, "Replace")) replace_current();
        if (k_button(ui, rb_all, "Replace All")) replace_all();
        bool repl_focused = ui->focus == ((uint32_t)(uintptr_t)A.repl_buf * 2654435761u | 1);
        if ((find_focused || repl_focused) && take_tab(ui, &tab_shift)) {
            if (find_focused && !tab_shift) A.want_repl_focus = true;
            else if (repl_focused && tab_shift) A.want_find_focus = true;
            else A.want_editor_focus = true;
        }
    } else if (find_focused && take_tab(ui, &tab_shift)) A.want_editor_focus = true;
}
