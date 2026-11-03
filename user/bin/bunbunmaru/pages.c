/* The five pages of the paper, typeset for a given window width. */
#include "news.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_arena[24000];
static size_t g_arena_n;

static char *arena_printf(const char *fmt, ...) {
    size_t room = sizeof g_arena - g_arena_n;
    if (room < 2) return (char *)"";
    char *out = g_arena + g_arena_n;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, room, fmt, ap);
    va_end(ap);
    if (n < 0) return (char *)"";
    if ((size_t)n >= room) n = (int)room - 1;
    g_arena_n += (size_t)n + 1;
    return out;
}

static int running_head(Layout *L, int page, int x, int y, int w) {
    lay_text(L, x, y, "THE BUNBUNMARU NEWS", 1, true, PAGE_ACCENT);
    const char *name = issue_page_name(page);
    if (w >= 420) lay_text_centered(L, x, y, w, name, 1, true, PAGE_INK);
    char *pg = arena_printf("Page %d of %d", page + 1, N_PAGES);
    lay_text(L, x + w - (int)strlen(pg) * 8, y, pg, 1, false, PAGE_DIM);
    lay_double_rule(L, x, y + 20, w, PAGE_RULE);
    return y + 34;
}

static int section_banner(Layout *L, int x, int y, int w, const char *title, const char *sub) {
    y = lay_banner(L, x, y, w, title, w >= 520 ? 2 : 1, PAGE_INK, true);
    if (sub) { lay_text_centered(L, x, y, w, sub, 1, false, PAGE_DIM); y += 22; }
    lay_hrule(L, x, y, w, 1, PAGE_RULE);
    return y + 12;
}

/* ---------------------------------------------------------------- custom blocks */

static int wrap_draw(KCanvas *cv, int x, int y, int w, const char *text, uint32_t color) {
    int cols = w / 8 > 4 ? w / 8 : 4, lines = 0;
    const char *p = text;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *q = p, *last = NULL;
        int c = 0;
        while (*q && c < cols) { if (*q == ' ') last = q; q++; c++; }
        const char *e = q;
        if (*q && *q != ' ' && last && last > p) e = last;
        kc_text_n(cv, x, y + lines * 18, p, (size_t)(e - p), color);
        p = e;
        lines++;
    }
    return lines * 18;
}

static void weather_draw(KUI *ui, KRect r, void *data) {
    const Issue *is = data;
    KCanvas *cv = &ui->cv;
    kc_fill(cv, r, 0xEFE8D0);
    kc_outline(cv, r, PAGE_RULE);
    kc_outline(cv, kr_inset(r, 3), PAGE_RULE);
    kc_text_bold(cv, r.x + 12, r.y + 8, "GENSOKYO WEATHER", PAGE_ACCENT);
    int h = wrap_draw(cv, r.x + 12, r.y + 28, r.w - 24, g_weather[is->weather], PAGE_INK);
    char line[80];
    snprintf(line, sizeof line, "High %d, low %d degrees.", is->temp_hi, is->temp_lo);
    kc_text(cv, r.x + 12, r.y + 32 + h, line, PAGE_DIM);
    kc_text(cv, r.x + 12, r.y + 50 + h, "Barrier: intact (Yukari napping).", PAGE_DIM);
}

static void card_draw(KUI *ui, KRect r, void *data) {
    const Issue *is = data;
    int s = r.w < 180 ? r.w : 180;
    spellcard_picture(ui, kr(r.x + (r.w - s) / 2, r.y + 2, s, s), is->card, is->daynum);
}

static void puzzle_block(KUI *ui, KRect r, void *data) { puzzle_draw(ui, r, data); }

/* ---------------------------------------------------------------- pages */

static void front_page(Layout *L, const Issue *is, int x, int w, int ncols, int gut) {
    int y = 10;
    int ts = w >= 640 ? 3 : (w >= 400 ? 2 : 1);
    const char *title = "THE BUNBUNMARU NEWS";
    int tw = (int)strlen(title) * 8 * ts;
    lay_text_centered(L, x, y, w, title, ts, true, PAGE_INK);
    lay_text_centered(L, x, y + 0, w, title, ts, false, PAGE_INK);
    if (w >= tw + 150) {
        lay_icon(L, x + (w - tw) / 2 - 56, y + (16 * ts - 40) / 2 + 4, 40, KI_NEWS);
        lay_icon(L, x + (w + tw) / 2 + 16, y + (16 * ts - 40) / 2 + 4, 40, KI_NEWS);
    }
    y += 16 * ts + 8;
    lay_text_centered(L, x, y, w, "Gensokyo's fastest paper. Accuracy sold separately.", 1, false, PAGE_DIM);
    y += 24;
    lay_double_rule(L, x, y, w, PAGE_RULE);
    y += 14;
    y = lay_banner(L, x, y, w, is->date_line, 1, PAGE_INK, true);
    lay_text_centered(L, x, y, w, arena_printf("Special delivery for %s", is->user_name), 1, false, PAGE_DIM);
    y += 24;
    lay_double_rule(L, x, y, w, PAGE_RULE);
    y += 16;

    const Article *lead = &is->sys[is->sys_lead];
    y = lay_banner(L, x, y, w, lead->headline, w >= 560 ? 2 : 1, PAGE_INK, false);
    if (lead->byline) { lay_text(L, x, y, lead->byline, 1, false, PAGE_DIM); y += 22; }
    Article body_only = { NULL, NULL, NULL, lead->body };
    Flow f = { 0 };
    flow_article(&f, &body_only, 1, true);
    y = lay_flow(L, x, y, w, ncols >= 3 ? 2 : ncols, gut, &f);
    flow_free(&f);
    y += 8;
    lay_hrule(L, x, y, w, 1, PAGE_RULE);
    y += 12;

    Flow g = { 0 };
    if (is->nfan > 0) flow_article(&g, is->fan[0], 1, false);
    flow_custom(&g, 120, weather_draw, (void *)is);
    flow_gap(&g, 10);
    if (is->nfan > 1) flow_article(&g, is->fan[1], 1, false);
    flow_para(&g, "IN THIS ISSUE", PAGE_ACCENT, true, false);
    flow_para(&g, "Page 2: what your machine has been up to. Page 3: more gossip. Page 4: a spell card, your horoscope and a crossword. Page 5: classifieds.", PAGE_DIM, false, true);
    y = lay_flow(L, x, y, w, ncols, gut, &g);
    flow_free(&g);
    L->height = y + 16;
}

static void features_page(Layout *L, const Issue *is, int x, int y0, int w) {
    int cols = w >= 700 ? 3 : 1, gut = 22;
    int cw = (w - (cols - 1) * gut) / cols;
    int y = y0;
    int ends[3] = { y0, y0, y0 };
    for (int part = 0; part < 3; part++) {
        Flow f = { 0 };
        if (part == 0) {
            const SpellCard *c = is->card;
            flow_para(&f, "SPELL CARD OF THE DAY", PAGE_ACCENT, true, false);
            flow_para(&f, c->name, PAGE_INK, true, false);
            flow_para(&f, arena_printf("Cast by %s. Difficulty: %s.", c->owner, c->rating), PAGE_DIM, false, false);
            flow_gap(&f, 4);
            flow_custom(&f, (cw < 180 ? cw : 180) + 8, card_draw, (void *)is);
            flow_para(&f, c->text, PAGE_INK, false, true);
        } else if (part == 1) {
            flow_para(&f, "HOROSCOPE", PAGE_ACCENT, true, false);
            flow_para(&f, "The star of the month is marked with an asterisk.", PAGE_DIM, false, false);
            flow_gap(&f, 4);
            for (int s = 0; s < 12; s++) {
                uint32_t h = hash32((uint32_t)is->daynum * 12u + (uint32_t)s);
                flow_para(&f, arena_printf("%s%s (%s)", s == is->month - 1 ? "* " : "", g_zodiac[s], g_zodiac_item[s]), PAGE_INK, true, false);
                flow_para(&f, arena_printf("%s Lucky: %s, number %u.", g_fortunes[h % (uint32_t)g_nfortunes], g_lucky_items[(h >> 8) % (uint32_t)g_nlucky], 1 + (h >> 16) % 99), PAGE_INK, false, true);
                flow_gap(&f, 4);
            }
        } else {
            const Puzzle *p = &is->puzzle;
            flow_para(&f, "CROSSWORD OF THE DAY", PAGE_ACCENT, true, false);
            flow_para(&f, "Click a square and type. Tab switches direction.", PAGE_DIM, false, false);
            flow_gap(&f, 4);
            flow_custom(&f, puzzle_height(p, cw), puzzle_block, (void *)p);
            flow_gap(&f, 6);
            for (int dir = 0; dir < 2; dir++) {
                flow_para(&f, dir == 0 ? "ACROSS" : "DOWN", PAGE_INK, true, false);
                for (int i = 0; i < p->nwords; i++)
                    if (p->words[i].across == (dir == 0)) flow_para(&f, arena_printf("%d. %s (%d)", p->words[i].num, p->words[i].clue, p->words[i].len), PAGE_INK, false, false);
                flow_gap(&f, 4);
            }
        }
        int px = cols == 3 ? x + part * (cw + gut) : x;
        int py = cols == 3 ? y0 : y;
        int end = lay_flow(L, px, py, cw, 1, gut, &f);
        flow_free(&f);
        ends[part] = end;
        if (cols == 1) y = end + 12;
    }
    int bottom = ends[0] > ends[1] ? ends[0] : ends[1];
    if (ends[2] > bottom) bottom = ends[2];
    if (cols == 3) {
        for (int i = 0; i < 2; i++) {
            DrawOp *op = lay_op(L, OP_VRULE);
            if (op) { op->x = x + (i + 1) * (cw + gut) - gut / 2; op->y = y0; op->h = bottom - y0; op->w = 1; op->color = PAGE_RULE; }
        }
    }
    L->height = bottom + 16;
}

void pages_build(Layout *L, const Issue *is, int page, int width) {
    lay_reset(L, width);
    g_arena_n = 0;
    int m = 18, x = m, w = width - 2 * m;
    if (w < 240) w = 240;
    int ncols = w >= 700 ? 3 : (w >= 440 ? 2 : 1), gut = 20;
    if (page == 0) { front_page(L, is, x, w, ncols, gut); return; }
    int y = running_head(L, page, x, 8, w);
    Flow f = { 0 };
    switch (page) {
    case 1:
        y = section_banner(L, x, y, w, "THE SHRINE GAZETTE", "What your machine has been up to, as of the moment you opened the paper");
        for (int i = 0; i < is->nsys; i++) if (i != is->sys_lead) flow_article(&f, &is->sys[i], 1, i == 1);
        y = lay_flow(L, x, y, w, ncols, gut, &f);
        break;
    case 2:
        y = section_banner(L, x, y, w, "AROUND GENSOKYO", "Gossip, rumour and the occasional fact");
        for (int i = 2; i < is->nfan && i < 12; i++) flow_article(&f, is->fan[i], 1, false);
        y = lay_flow(L, x, y, w, ncols, gut, &f);
        flow_free(&f);
        y += 6;
        lay_hrule(L, x, y, w, 1, PAGE_RULE);
        y += 12;
        flow_para(&f, "OVERHEARD IN GENSOKYO", PAGE_ACCENT, true, false);
        for (int i = 0; i < 3; i++) { flow_para(&f, g_overheard[is->overheard[i]], PAGE_INK, false, false); flow_gap(&f, 4); }
        y = lay_flow(L, x, y, w, ncols == 3 ? 1 : ncols, gut, &f);
        break;
    case 3:
        y = section_banner(L, x, y, w, "FEATURES", "The spell card, the stars and the crossword");
        flow_free(&f);
        features_page(L, is, x, y, w);
        return;
    default:
        y = section_banner(L, x, y, w, "CLASSIFIEDS", "Small notices from small people");
        for (int i = 0; i < is->nads; i++) { flow_boxed(&f, is->ads[i]->text, PAGE_INK); flow_gap(&f, 8); }
        y = lay_flow(L, x, y, w, ncols, gut, &f);
        flow_free(&f);
        y += 6;
        lay_hrule(L, x, y, w, 1, PAGE_RULE);
        y += 12;
        flow_para(&f, "LETTERS TO THE EDITOR", PAGE_ACCENT, true, false);
        for (int i = 0; i < 3; i++) { flow_para(&f, g_letters[is->letters[i]], PAGE_INK, false, false); flow_gap(&f, 6); }
        flow_rule(&f);
        flow_para(&f, "KAPPA'S CORNER", PAGE_ACCENT, true, false);
        flow_para(&f, g_tips[is->tip], PAGE_INK, false, true);
        y = lay_flow(L, x, y, w, ncols == 3 ? 2 : ncols, gut, &f);
        break;
    }
    flow_free(&f);
    L->height = y + 20;
}
