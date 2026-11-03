/* The application shell: loading screen, menus, the 60 Hz game loop, pause, game over, ending, name entry and the score table. */
#include "ui.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCORE_FILE "danmaku.scores"

typedef enum { UI_LOADING, UI_TITLE, UI_DIFF, UI_SHOT, UI_STAGE, UI_PLAY, UI_PAUSE, UI_NAME, UI_SCORES, UI_ENDING } UiState;

static struct {
    KApp *app;
    Options opt;
    int state;
    int sel, diff, shot, stage, mode;
    int load_stage;
    int tab;
    char name[16];
    int name_len;
    bool name_for_ending;
    KStepper step;
    KFps fps;
    uint64_t anim;                                  /* frames since start, for menu animations */
    bool latch_z, latch_x;
    bool full_redraw, overlay_done;
    int hud_last_tick;
    long hud_score, hud_hi;
    int hud_lives, hud_bombs, hud_power, hud_graze, hud_points;
    KScoreTable scores;
    bool finished;
    long result_score;
    int result_stage;
    char result_text[160];
    int petal_seed;
} U;

/* ---------------------------------------------------------------- text cache */

typedef struct TxtEntry { char s[96]; int scale; uint32_t fg, out; KSprite *spr; } TxtEntry;
static TxtEntry g_txt[96];
static int g_txt_next;

static KSprite *txt(const char *s, int scale, uint32_t fg, uint32_t out) {
    for (int i = 0; i < 96; i++)
        if (g_txt[i].spr && g_txt[i].scale == scale && g_txt[i].fg == fg && g_txt[i].out == out && !strcmp(g_txt[i].s, s)) return g_txt[i].spr;
    TxtEntry *e = &g_txt[g_txt_next];
    g_txt_next = (g_txt_next + 1) % 96;
    ks_free(e->spr);
    snprintf(e->s, sizeof e->s, "%s", s);
    e->scale = scale;
    e->fg = fg;
    e->out = out;
    e->spr = ks_text(s, scale, fg, out);
    return e->spr;
}

static void text_c(KCanvas *cv, int cx, int y, const char *s, int scale, uint32_t fg, uint32_t out) {
    KSprite *t = txt(s, scale, fg, out);
    ks_blit_key(cv, cx - t->w / 2, y, t);
}

/* ---------------------------------------------------------------- helpers */

static void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void trace(const char *fmt, ...) {
    if (!U.opt.trace) return;
    char buf[220];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("danmaku: %s\n", buf);
    fflush(stdout);
}

static void set_state(int st) {
    trace("state %d -> %d", U.state, st);
    U.state = st;
    U.sel = 0;
    U.full_redraw = true;
    U.overlay_done = false;
}

static bool key_pressed(KUI *ui, uint8_t code) {
    for (int i = 0; i < ui->nkeys; i++) if (ui->keys[i].code == code && ui->keys[i].down) { ui->keys[i].down = 0; return true; }
    return false;
}
static bool key_once(KUI *ui, uint8_t code) {
    for (int i = 0; i < ui->nkeys; i++) if (ui->keys[i].code == code && ui->keys[i].down == 1) { ui->keys[i].down = 0; return true; }
    return false;
}
static bool confirm(KUI *ui) { return key_once(ui, KEY_Z) || key_once(ui, KEY_ENTER) || key_once(ui, KEY_KPENTER) || key_once(ui, KEY_SPACE); }
static bool cancel(KUI *ui) { return key_once(ui, KEY_X) || key_once(ui, KEY_ESC); }

static int menu_move(KUI *ui, int count) {
    int d = 0;
    if (key_pressed(ui, KEY_UP)) d = -1;
    if (key_pressed(ui, KEY_DOWN)) d = 1;
    if (d) U.sel = (U.sel + d + count) % count;
    return d;
}

/* ---------------------------------------------------------------- starting and ending runs */

static long best_for(int diff) { return kg_scores_best(&U.scores, diff, 0); }

static void start_run(int mode, int diff, int shot, int stage) {
    U.mode = mode;
    U.diff = diff;
    U.shot = shot;
    U.stage = stage;
    uint32_t seed = U.opt.fixed_seed ? U.opt.seed : kg_seed_from_time();
    game_new(mode, diff, shot, stage, seed);
    G.demo = U.opt.demo;
    G.god = U.opt.god;
    G.trace = U.opt.trace;
    G.hiscore = best_for(diff);
    if (U.opt.power >= 0) G.p.power = U.opt.power > MAX_POWER ? MAX_POWER : U.opt.power;
    G.dbg_phase = U.opt.phase;
    if (U.opt.boss_only || U.opt.midboss_only) stage_skip_to_boss(U.opt.midboss_only);
    kg_step_init(&U.step, TICK_US, 5);
    U.latch_z = U.latch_x = false;
    U.hud_last_tick = -100;
    /* fast-forward without drawing (for repeatable screenshots) */
    for (int i = 0; i < U.opt.warp && G.state == GS_PLAY; i++) {
        Input in;
        memset(&in, 0, sizeof in);
        if (U.opt.demo) bot_input(&in);
        game_tick(&in);
    }
    set_state(UI_PLAY);
    trace("%s run: %s, %s, %s, seed %u", mode == MODE_PRACTICE ? "practice" : "new", diff_name(diff), shot_name(shot), stage_name(stage), (unsigned)seed);
}

static void finish_run(bool cleared) {
    U.result_score = G.score;
    U.result_stage = cleared ? N_STAGES : G.stage;
    trace("run over: score %ld, stage %d, %s, graze %d, captures %d/%d, deaths %d, bombs %d", G.score, G.stage + 1, cleared ? "cleared" : "game over",
          G.graze, G.stat_captures, G.stat_cards, G.stat_deaths, G.stat_bombs);
    if (U.opt.demo || U.opt.quit_after > 0) { U.finished = true; kapp_quit(U.app, 0); return; }
    if (U.mode == MODE_PRACTICE || U.opt.noscores) { set_state(cleared ? UI_ENDING : UI_TITLE); U.name_for_ending = false; return; }
    U.name_for_ending = cleared;
    if (kg_scores_rank(&U.scores, U.diff, G.score) >= 0 && G.score > 0) {
        const char *user = getenv("USER");
        snprintf(U.name, sizeof U.name, "%s", user && *user ? user : "Reimu");
        U.name[10] = 0;
        U.name_len = (int)strlen(U.name);
        set_state(cleared ? UI_ENDING : UI_NAME);
    } else set_state(cleared ? UI_ENDING : UI_TITLE);
}

static void save_score(void) {
    int rank = kg_scores_add(&U.scores, U.diff, U.result_score, U.name[0] ? U.name : "Anonymous", U.shot, U.result_stage);
    kg_scores_save(&U.scores, SCORE_FILE);
    trace("score saved: %ld rank %d name %s", U.result_score, rank, U.name);
    U.tab = U.diff;
    set_state(UI_SCORES);
}

/* ---------------------------------------------------------------- screens */

static const char *const TITLE_ITEMS[] = { "Start", "Practice", "High Scores", "Quit" };
static const char *const DIFF_DESC[DIFF_COUNT] = {
    "Gentle curtains and extra lives. A good place to learn.",
    "The festival as it was meant to be played.",
    "Denser patterns, faster bullets. Bring your reflexes.",
    "Everything at once. Good luck.",
};
static const uint32_t DIFF_COL[DIFF_COUNT] = { 0x6AE08A, 0x7AB0FF, 0xFFB050, 0xFF5CB0 };

static void draw_backdrop(KCanvas *cv, int dim) {
    ks_copy_rect(cv, 0, 0, &(KCanvas){ .px = A.title_bg->px, .w = WIN_W, .h = WIN_H, .stride = WIN_W, .clip = kr(0, 0, WIN_W, WIN_H) }, kr(0, 0, WIN_W, WIN_H));
    if (dim < 256) ks_shade_rect(cv, kr(0, 0, WIN_W, WIN_H), dim);
    /* petals drifting down */
    for (int i = 0; i < 20; i++) {
        uint32_t r = (uint32_t)(i * 2654435761u);
        int x = (int)((r % WIN_W) + (int)(fx_sin((int)(U.anim * 3 + (r >> 8))) >> 12)) % WIN_W;
        int y = (int)((r >> 12) % WIN_H + U.anim * (1 + (r >> 20) % 3) / 2) % (WIN_H + 20) - 10;
        ks_blit_key(cv, x, y, A.petal);
    }
}

static void draw_loading(KCanvas *cv) {
    ks_fill_rect(cv, kr(0, 0, WIN_W, WIN_H), 0x0A0E26);
    const char *msg = gfx_stage_label(U.load_stage < GFX_STAGES ? U.load_stage : GFX_STAGES - 1);
    kc_text_shadow(cv, (WIN_W - (int)strlen(msg) * 8) / 2, WIN_H / 2 - 36, msg, 0xFFF4C8, 0x000000);
    int w = 300 * U.load_stage / GFX_STAGES;
    ks_fill_rect(cv, kr((WIN_W - 300) / 2, WIN_H / 2, 300, 14), 0x1D2A5C);
    ks_fill_rect(cv, kr((WIN_W - 300) / 2, WIN_H / 2, w, 14), 0xF2709A);
    kc_outline(cv, kr((WIN_W - 300) / 2 - 1, WIN_H / 2 - 1, 302, 16), 0xD9A441);
}

static void title_activate(void) {
    switch (U.sel) {
    case 0: U.mode = MODE_NORMAL; set_state(UI_DIFF); U.sel = 1; break;
    case 1: U.mode = MODE_PRACTICE; set_state(UI_DIFF); U.sel = 1; break;
    case 2: U.tab = 1; set_state(UI_SCORES); break;
    default: kapp_quit(U.app, 0); break;
    }
}

static void screen_title(KCanvas *cv, KUI *ui) {
    menu_move(ui, 4);
    if (confirm(ui)) { title_activate(); return; }
    if (key_once(ui, KEY_ESC)) { kapp_quit(U.app, 0); return; }
    /* a mouse can pick entries too */
    static int last_mx, last_my;
    for (int i = 0; i < 4; i++) {
        KRect r = kr(WIN_W / 2 - 120, 256 + i * 44, 240, 36);
        if (ui->mouse_inside && kr_contains(r, ui->mx, ui->my)) {
            if (ui->mx != last_mx || ui->my != last_my) U.sel = i;
            if (ui->pressed[0]) { U.sel = i; title_activate(); return; }
        }
    }
    last_mx = ui->mx;
    last_my = ui->my;
    draw_backdrop(cv, 256);
    int bob = (int)(fx_sin((int)(U.anim * 5)) >> 14);
    ks_blit(cv, WIN_W / 2 - A.logo->w / 2, 40 + bob, A.logo);
    ks_blit_key(cv, WIN_W / 2 - A.logo_sub->w / 2, 178, A.logo_sub);
    for (int i = 0; i < 4; i++) {
        bool sel = U.sel == i;
        int y = 256 + i * 44;
        if (sel) {
            ks_stipple_rect(cv, kr(WIN_W / 2 - 150, y - 2, 300, 38), 0x40205A, (int)U.anim);
            ks_blit_key(cv, WIN_W / 2 - 136, y + 8, A.life_icon);
            ks_blit_key(cv, WIN_W / 2 + 121, y + 8, A.life_icon);
        }
        text_c(cv, WIN_W / 2, y, TITLE_ITEMS[i], 2, sel ? 0xFFF4C8 : 0xB4BCE8, sel ? 0x8A1030 : 0x101030);
    }
    char buf[96];
    long hi = best_for(DIFF_NORMAL);
    if (hi) {
        char num[32];
        kg_format_int(num, sizeof num, hi);
        snprintf(buf, sizeof buf, "Best on Normal: %s", num);
        kc_text_shadow(cv, (WIN_W - (int)strlen(buf) * 8) / 2, 436, buf, 0xD9A441, 0x000010);
    }
    kc_text_shadow(cv, (WIN_W - 61 * 8) / 2, 456, "Arrows: choose   Z or Enter: select   X or Esc: back   F3: fps", 0x8A92C8, 0x000010);
}

static void screen_diff(KCanvas *cv, KUI *ui) {
    menu_move(ui, DIFF_COUNT);
    if (confirm(ui)) {
        U.diff = U.sel;
        set_state(UI_SHOT);
        U.sel = U.shot;
        return;
    }
    if (cancel(ui)) { set_state(UI_TITLE); return; }
    draw_backdrop(cv, 130);
    text_c(cv, WIN_W / 2, 40, U.mode == MODE_PRACTICE ? "Practice: choose a difficulty" : "Choose a difficulty", 2, 0xFFF4C8, 0x601020);
    for (int i = 0; i < DIFF_COUNT; i++) {
        bool sel = U.sel == i;
        int y = 120 + i * 62;
        KRect r = kr(WIN_W / 2 - 170, y, 340, 50);
        if (sel) { ks_stipple_rect(cv, kr(r.x - 4, r.y - 4, r.w + 8, r.h + 8), DIFF_COL[i], (int)U.anim); }
        ks_shade_rect(cv, r, 90);
        kc_outline(cv, r, sel ? DIFF_COL[i] : 0x4A5090);
        text_c(cv, WIN_W / 2, y + 8, diff_name(i), 2, sel ? 0xFFFFFF : DIFF_COL[i], 0x101030);
    }
    kc_text_shadow(cv, (WIN_W - (int)strlen(DIFF_DESC[U.sel]) * 8) / 2, 380, DIFF_DESC[U.sel], 0xFFFFFF, 0x000010);
    long hi = best_for(U.sel);
    if (hi) {
        char num[32], buf[64];
        kg_format_int(num, sizeof num, hi);
        snprintf(buf, sizeof buf, "Best on %s: %s", diff_name(U.sel), num);
        kc_text_shadow(cv, (WIN_W - (int)strlen(buf) * 8) / 2, 404, buf, 0xD9A441, 0x000010);
    }
}

static void screen_shot(KCanvas *cv, KUI *ui) {
    if (key_pressed(ui, KEY_LEFT) || key_pressed(ui, KEY_RIGHT) || key_pressed(ui, KEY_UP) || key_pressed(ui, KEY_DOWN)) U.sel ^= 1;
    if (confirm(ui)) {
        U.shot = U.sel;
        if (U.mode == MODE_PRACTICE) { set_state(UI_STAGE); U.sel = 0; }
        else start_run(MODE_NORMAL, U.diff, U.shot, 0);
        return;
    }
    if (cancel(ui)) { set_state(UI_DIFF); U.sel = U.diff; return; }
    draw_backdrop(cv, 130);
    text_c(cv, WIN_W / 2, 30, "Choose your heroine", 2, 0xFFF4C8, 0x601020);
    static const char *const DESC[2][3] = {
        { "Amulet streams that fan out,", "plus needles that find their target.", "Bomb: Hundred Colour Orbs" },
        { "Stars that gather into a beam", "when you hold Shift. Hits hard.", "Bomb: Starlight Lance" },
    };
    for (int i = 0; i < 2; i++) {
        bool sel = U.sel == i;
        int cx = 170 + i * 300, y = 90;
        KRect r = kr(cx - 130, y, 260, 330);
        ks_shade_rect(cv, r, sel ? 70 : 110);
        kc_outline(cv, r, sel ? 0xFFE070 : 0x4A5090);
        if (sel) ks_stipple_rect(cv, kr(r.x - 3, r.y - 3, r.w + 6, r.h + 6), 0xFFE070, (int)U.anim);
        const KSprite *p = A.portrait[i];
        if (sel) ks_blit_key(cv, cx - p->w / 2, y + 20 + (int)(fx_sin((int)U.anim * 6) >> 15), p);
        else ks_blit_fade(cv, cx - p->w / 2, y + 20, p, 130);
        text_c(cv, cx, y + 168, shot_name(i), 2, sel ? 0xFFF4C8 : 0xA0A8D0, 0x101030);
        for (int l = 0; l < 3; l++) kc_text_shadow(cv, cx - (int)strlen(DESC[i][l]) * 4, y + 214 + l * 20, DESC[i][l], sel ? 0xFFFFFF : 0x8A92C8, 0x000010);
    }
    kc_text_shadow(cv, (WIN_W - 52 * 8) / 2, 440, "Left / Right: choose   Z: confirm   X: back", 0x8A92C8, 0x000010);
}

static void screen_stage(KCanvas *cv, KUI *ui) {
    menu_move(ui, N_STAGES);
    if (confirm(ui)) { start_run(MODE_PRACTICE, U.diff, U.shot, U.sel); return; }
    if (cancel(ui)) { set_state(UI_SHOT); U.sel = U.shot; return; }
    draw_backdrop(cv, 130);
    text_c(cv, WIN_W / 2, 40, "Practice: choose a stage", 2, 0xFFF4C8, 0x601020);
    for (int i = 0; i < N_STAGES; i++) {
        bool sel = U.sel == i;
        int y = 130 + i * 74;
        KRect r = kr(WIN_W / 2 - 200, y, 400, 60);
        if (sel) ks_stipple_rect(cv, kr(r.x - 4, r.y - 4, r.w + 8, r.h + 8), 0xFFE070, (int)U.anim);
        ks_shade_rect(cv, r, 90);
        kc_outline(cv, r, sel ? 0xFFE070 : 0x4A5090);
        char buf[16];
        snprintf(buf, sizeof buf, "Stage %d", i + 1);
        text_c(cv, WIN_W / 2, y + 6, buf, 2, sel ? 0xFFF4C8 : 0xA0A8D0, 0x101030);
        const char *nm = stage_name(i);
        kc_text_shadow(cv, (WIN_W - (int)strlen(nm) * 8) / 2, y + 38, nm, sel ? 0xFFFFFF : 0x8A92C8, 0x000010);
    }
}

static void screen_scores(KCanvas *cv, KUI *ui) {
    if (key_pressed(ui, KEY_LEFT)) U.tab = (U.tab + DIFF_COUNT - 1) % DIFF_COUNT;
    if (key_pressed(ui, KEY_RIGHT)) U.tab = (U.tab + 1) % DIFF_COUNT;
    if (confirm(ui) || cancel(ui)) { set_state(UI_TITLE); return; }
    draw_backdrop(cv, 110);
    text_c(cv, WIN_W / 2, 24, "High Scores", 3, 0xFFF4C8, 0x601020);
    char buf[96];
    snprintf(buf, sizeof buf, "<  %s  >", diff_name(U.tab));
    text_c(cv, WIN_W / 2, 84, buf, 2, DIFF_COL[U.tab], 0x101030);
    const KScoreEntry *list[12];
    int n = kg_scores_list(&U.scores, U.tab, list, 10);
    KRect panel = kr(60, 124, WIN_W - 120, 296);
    ks_shade_rect(cv, panel, 80);
    kc_outline(cv, panel, 0xD9A441);
    if (!n) text_c(cv, WIN_W / 2, 250, "No scores yet", 2, 0xB4BCE8, 0x101030);
    for (int i = 0; i < n; i++) {
        char num[32];
        kg_format_int(num, sizeof num, list[i]->score);
        int y = 134 + i * 28;
        uint32_t col = i == 0 ? 0xFFE070 : i < 3 ? 0xFFFFFF : 0xB4BCE8;
        snprintf(buf, sizeof buf, "%2d", i + 1);
        kc_text_shadow(cv, 80, y, buf, col, 0x000010);
        kc_text_shadow(cv, 112, y, list[i]->name, col, 0x000010);
        kc_text_shadow(cv, 276 + (14 - (int)strlen(num)) * 8, y, num, col, 0x000010);
        snprintf(buf, sizeof buf, "%c  %s", list[i]->extra[0] == SHOT_A ? 'A' : 'B', list[i]->extra[1] >= N_STAGES ? "All clear" : (snprintf(num, sizeof num, "Stage %ld", list[i]->extra[1] + 1), num));
        kc_text_shadow(cv, 430, y, buf, col, 0x000010);
    }
    kc_text_shadow(cv, (WIN_W - 47 * 8) / 2, 442, "Left / Right: difficulty   Z or X: back", 0x8A92C8, 0x000010);
}

static void screen_name(KCanvas *cv, KUI *ui) {
    for (int i = 0; i < ui->nkeys; i++) {
        KKey *k = &ui->keys[i];
        if (!k->down) continue;
        if (k->code == KEY_ENTER || k->code == KEY_KPENTER) { save_score(); return; }
        if (k->code == KEY_BACKSPACE) { if (U.name_len > 0) U.name[--U.name_len] = 0; }
        else if (k->ch >= 32 && k->ch < 127 && U.name_len < 10) { U.name[U.name_len++] = (char)k->ch; U.name[U.name_len] = 0; }
    }
    if (U.state != UI_NAME) return;
    draw_backdrop(cv, 110);
    text_c(cv, WIN_W / 2, 90, "New High Score!", 3, 0xFFE070, 0x601020);
    char num[32], buf[64];
    kg_format_int(num, sizeof num, U.result_score);
    text_c(cv, WIN_W / 2, 160, num, 3, 0xFFFFFF, 0x201040);
    snprintf(buf, sizeof buf, "%s  %s", diff_name(U.diff), shot_name(U.shot));
    text_c(cv, WIN_W / 2, 214, buf, 1, 0xC8D8FF, 0x101030);
    text_c(cv, WIN_W / 2, 262, "Enter your name", 2, 0xFFF4C8, 0x601020);
    KRect box = kr(WIN_W / 2 - 120, 306, 240, 40);
    ks_shade_rect(cv, box, 60);
    kc_outline(cv, box, 0xD9A441);
    snprintf(buf, sizeof buf, "%s%s", U.name, (U.anim / 15) & 1 ? "_" : " ");
    text_c(cv, WIN_W / 2, 314, buf, 2, 0xFFFFFF, 0x101030);
    kc_text_shadow(cv, (WIN_W - 36 * 8) / 2, 380, "Type a name, then press Enter", 0x8A92C8, 0x000010);
}

static void screen_ending(KCanvas *cv, KUI *ui) {
    if (confirm(ui)) {
        if (!U.opt.noscores && U.mode == MODE_NORMAL && kg_scores_rank(&U.scores, U.diff, U.result_score) >= 0 && U.result_score > 0) set_state(UI_NAME);
        else set_state(UI_TITLE);
        return;
    }
    draw_backdrop(cv, 100);
    bool cleared = U.result_stage >= N_STAGES;
    text_c(cv, WIN_W / 2, 36, cleared ? (U.mode == MODE_PRACTICE ? "Stage Clear" : "Festival Over") : "Practice Over", 3, 0xFFF4C8, 0x601020);
    static const char *const STORY[2][4] = {
        { "The last lantern drifts out over the lake,", "and the night finally runs out of ideas.", "Somewhere a fairy is already planning tomorrow.", "The donation box, as ever, is empty." },
        { "The midnight is broken and the stars fall quiet.", "You catch the dawn on the brim of your hat.", "Not a bad evening's work for one witch.", "Tomorrow: breakfast." },
    };
    if (U.mode == MODE_NORMAL) for (int i = 0; i < 4; i++) text_c(cv, WIN_W / 2, 100 + i * 24, STORY[U.shot][i], 1, 0xE8ECFF, 0x101030);
    char buf[96], num[32];
    kg_format_int(num, sizeof num, U.result_score);
    snprintf(buf, sizeof buf, "Score  %s", num);
    text_c(cv, WIN_W / 2, 230, buf, 2, 0xFFFFFF, 0x201040);
    snprintf(buf, sizeof buf, "Graze %d    Spell cards %d / %d", G.graze, G.stat_captures, G.stat_cards);
    text_c(cv, WIN_W / 2, 278, buf, 1, 0xC8D8FF, 0x101030);
    snprintf(buf, sizeof buf, "Lives lost %d    Bombs used %d", G.stat_deaths, G.stat_bombs);
    text_c(cv, WIN_W / 2, 302, buf, 1, 0xC8D8FF, 0x101030);
    snprintf(buf, sizeof buf, "%s  %s", diff_name(U.diff), shot_name(U.shot));
    text_c(cv, WIN_W / 2, 332, buf, 1, 0xFFE070, 0x101030);
    text_c(cv, WIN_W / 2, 400, "Thank you for playing", 2, 0xF2A0B0, 0x401030);
    kc_text_shadow(cv, (WIN_W - 24 * 8) / 2, 450, "Press Z or Enter", 0x8A92C8, 0x000010);
}

/* ---------------------------------------------------------------- the game */

static void build_input(KUI *ui, Input *in) {
    memset(in, 0, sizeof *in);
    if (U.opt.demo) { bot_input(in); return; }
    in->left = ui->key_down[KEY_LEFT];
    in->right = ui->key_down[KEY_RIGHT];
    in->up = ui->key_down[KEY_UP];
    in->down = ui->key_down[KEY_DOWN];
    in->shot = ui->key_down[KEY_Z] || U.latch_z;
    in->bomb = ui->key_down[KEY_X] || U.latch_x;
    in->focus = ui->key_down[KEY_LSHIFT] || ui->key_down[KEY_RSHIFT];
    U.latch_z = U.latch_x = false;
}

static void draw_hud_if_needed(KCanvas *cv, bool force, bool *whole) {
    bool changed = G.score != U.hud_score || G.hiscore != U.hud_hi || G.p.lives != U.hud_lives || G.p.bombs != U.hud_bombs || G.p.power != U.hud_power ||
                   G.graze != U.hud_graze || G.points != U.hud_points;
    if (!force && !(changed && (int)G.tick - U.hud_last_tick >= 6)) return;
    render_hud(cv);
    U.hud_score = G.score; U.hud_hi = G.hiscore; U.hud_lives = G.p.lives; U.hud_bombs = G.p.bombs; U.hud_power = G.p.power;
    U.hud_graze = G.graze; U.hud_points = G.points;
    U.hud_last_tick = (int)G.tick;
    *whole = true;
}

static void pause_menu(KCanvas *cv, KUI *ui) {
    static const char *const ITEMS[3] = { "Resume", "Restart", "Quit to title" };
    menu_move(ui, 3);
    if (key_once(ui, KEY_ESC) || key_once(ui, KEY_X)) { set_state(UI_PLAY); kg_step_reset(&U.step); return; }
    if (confirm(ui)) {
        if (U.sel == 0) { set_state(UI_PLAY); kg_step_reset(&U.step); }
        else if (U.sel == 1) start_run(U.mode, U.diff, U.shot, U.mode == MODE_PRACTICE ? U.stage : 0);
        else set_state(UI_TITLE);
        return;
    }
    if (U.overlay_done && !ui->pressed[0]) {
        /* still show the menu selection change */
    }
    KRect field = kr(PFX, PFY, PFW, PFH);
    kc_set_clip(cv, field);
    render_game(cv, 256);
    ks_shade_rect(cv, field, 70);
    kc_set_clip(cv, kr(0, 0, cv->w, cv->h));
    text_c(cv, PFX + PFW / 2, PFY + 120, "Paused", 3, 0xFFF4C8, 0x601020);
    for (int i = 0; i < 3; i++) text_c(cv, PFX + PFW / 2, PFY + 190 + i * 40, ITEMS[i], 2, U.sel == i ? 0xFFFFFF : 0x9AA4D8, U.sel == i ? 0x8A1030 : 0x101030);
    k_damage(ui, field);
}

static void play_frame(KApp *app, KUI *ui) {
    KCanvas *cv = &ui->cv;
    if (U.full_redraw) { render_frame(cv); }
    /* pause on Escape, or when the window loses the keyboard */
    if (key_once(ui, KEY_ESC) || (!ui->has_focus && !U.opt.demo)) { set_state(UI_PAUSE); kapp_redraw(app); return; }
    if (key_once(ui, KEY_F3)) { g_show_fps = !g_show_fps; U.overlay_done = false; }
    for (int i = 0; i < ui->nkeys; i++) {
        if (ui->keys[i].down == 1 && ui->keys[i].code == KEY_Z) U.latch_z = true;
        if (ui->keys[i].down == 1 && ui->keys[i].code == KEY_X) U.latch_x = true;
    }
    int steps = U.opt.hold && U.opt.warp > 0 ? 0 : kg_step_due(&U.step);
    uint64_t t0 = kg_now_us();
    for (int i = 0; i < steps; i++) {
        Input in;
        build_input(ui, &in);
        game_tick(&in);
        if (U.opt.quit_after > 0 && (int)G.tick >= U.opt.quit_after) { finish_run(false); return; }
        if (G.state == GS_GAME_OVER && G.clear_t <= 0) { finish_run(false); return; }
        if (G.state == GS_ENDING) { finish_run(true); return; }
    }
    if (steps == 0 && !U.full_redraw && !(U.opt.hold && U.opt.warp > 0 && !U.overlay_done)) {
        k_damage(ui, kr(0, 0, 0, 0));
        kapp_set_tick(app, kg_step_wait_ms(&U.step));
        return;
    }
    bool whole = U.full_redraw;
    render_game(cv, 256);
    draw_hud_if_needed(cv, U.full_redraw, &whole);
    U.full_redraw = false;
    U.overlay_done = true;
    if (G.state == GS_GAME_OVER && G.clear_t < 150) {
        int fade = (150 - G.clear_t) * 256 / 150;
        ks_shade_rect(cv, kr(PFX, PFY, PFW, PFH), 256 - fade * 190 / 256);
        text_c(cv, PFX + PFW / 2, PFY + 170, "Game Over", 3, ks_mul_px(0xFFB0B8, fade), 0x401020);
        whole = true;
    }
    k_damage(ui, whole ? kr(0, 0, cv->w, cv->h) : kr(PFX, PFY, PFW, PFH));
    uint64_t used = kg_now_us() - t0;
    kg_fps_frame(&U.fps, used);
    g_fps10 = U.fps.fps10;
    g_work_us = U.fps.work_us_avg;
    g_work_max = U.fps.work_us_max;
    if (U.opt.trace && U.fps.t0 && (G.tick % 300) < 6 && steps) trace("frames: %d.%d fps, %d us per frame (worst %d us), %d bullets, step catch-up %d dropped", U.fps.fps10 / 10, U.fps.fps10 % 10, U.fps.work_us_avg,
                                                                      U.fps.work_us_max, G.nb, U.step.dropped);
    kapp_set_tick(app, kg_step_wait_ms(&U.step));
}

/* ---------------------------------------------------------------- the draw callback */

static void draw(KApp *app, KUI *ui, void *user) {
    (void)user;
    KCanvas *cv = &ui->cv;
    U.anim++;
    if (U.state == UI_LOADING) {
        if (U.load_stage < GFX_STAGES) gfx_build_stage(U.load_stage++);
        draw_loading(cv);
        k_damage(ui, kr(0, 0, cv->w, cv->h));
        if (U.load_stage >= GFX_STAGES) {
            if (U.opt.start_now) start_run(U.opt.stage > 0 ? MODE_PRACTICE : MODE_NORMAL, U.opt.diff < 0 ? DIFF_NORMAL : U.opt.diff,
                                           U.opt.shot < 0 ? SHOT_A : U.opt.shot, U.opt.stage > 0 ? U.opt.stage - 1 : 0);
            else set_state(UI_TITLE);
        }
        kapp_redraw(app);
        return;
    }
    if (key_once(ui, KEY_F3) && U.state != UI_PLAY) { g_show_fps = !g_show_fps; }
    switch (U.state) {
    case UI_PLAY:
        play_frame(app, ui);
        return;
    case UI_PAUSE:
        if (U.overlay_done && !ui->nkeys) { k_damage(ui, kr(0, 0, 0, 0)); kapp_set_tick(app, 100); return; }
        pause_menu(cv, ui);
        U.overlay_done = U.state == UI_PAUSE;
        if (U.state == UI_PLAY) { U.full_redraw = true; kapp_redraw(app); }
        kapp_set_tick(app, 100);
        return;
    case UI_TITLE: screen_title(cv, ui); break;
    case UI_DIFF: screen_diff(cv, ui); break;
    case UI_SHOT: screen_shot(cv, ui); break;
    case UI_STAGE: screen_stage(cv, ui); break;
    case UI_SCORES: screen_scores(cv, ui); break;
    case UI_NAME: screen_name(cv, ui); break;
    case UI_ENDING: screen_ending(cv, ui); break;
    default: break;
    }
    if (U.state == UI_PLAY) { kapp_redraw(app); return; }
    k_damage(ui, kr(0, 0, cv->w, cv->h));
    kapp_set_tick(app, 33);
}

static bool on_close(KApp *app, void *user) {
    (void)app; (void)user;
    return true;
}

void ui_run(const Options *opt) {
    memset(&U, 0, sizeof U);
    U.opt = *opt;
    U.state = UI_LOADING;
    U.diff = DIFF_NORMAL;
    U.shot = SHOT_A;
    kg_scores_init(&U.scores, false, 10);
    kg_scores_load(&U.scores, SCORE_FILE);
    kapp_set_hidpi(false);                                   /* draws its own pixels: 1x windows, enlarged by the window server on a hi-dpi screen */
    U.app = kapp_new("Danmaku", WIN_W, WIN_H, FW_CENTER);
    if (!U.app) return;
    kapp_set_draw(U.app, draw, NULL);
    kapp_set_close_handler(U.app, on_close, NULL);
    kapp_set_tick(U.app, 20);
    g_show_fps = false;
    kapp_run(U.app);
    kapp_destroy(U.app);
}
