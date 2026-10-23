/*
 * hakurei-greeter: the login screen. It runs as root inside its own window server (hakurei-session --greeter), lists
 * the residents of the Human Village, checks the password against /etc/shadow and tells init who is at the gate by
 * writing the name to the pipe init handed down in $GREETER_FD. init then opens that resident's desktop.
 */
#include <kappa.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>
#include <shadow.h>
#include <gensokyo.h>
#include <audio.h>
#include <sys/utsname.h>

#define MAX_RESIDENTS 8
#define THROTTLE_MS   1500          /* after a wrong password the gate stays shut for a moment */
#define GREETER_EXIT_TEXT 10        /* exit status that asks init for the text door (login on the console) once */

typedef struct {
    char name[32], gecos[48];
    bool open;                      /* no password needed */
} Resident;

static Resident g_res[MAX_RESIDENTS];
static int g_nres, g_sel;
static char g_pw[64];
static const char *g_msg;
static uint64_t g_fail_ms;
static bool g_refocus = true;
static KApp *g_app;
static uint32_t *g_wall;
static int g_wall_w, g_wall_h;

/* The desktop needs the display and input devices, which belong to group shrine: outsiders get the text console only. */
static bool has_a_seat(const struct passwd *p) {
    struct group *g = getgrnam("shrine");
    if (!g) return true;                                            /* no such group configured: everyone does */
    if (g->gr_gid == p->pw_gid) return true;
    for (char **m = g->gr_mem; m && *m; m++) if (!strcmp(*m, p->pw_name)) return true;
    return false;
}

static void load_residents(void) {
    struct passwd *p;
    setpwent();
    while ((p = getpwent()) && g_nres < MAX_RESIDENTS) {
        if (p->pw_uid < 1000 || p->pw_uid >= 60000) continue;
        if (strstr(p->pw_shell, "nologin") || strstr(p->pw_shell, "false")) continue;
        if (!has_a_seat(p)) continue;
        Resident *r = &g_res[g_nres++];
        snprintf(r->name, sizeof r->name, "%s", p->pw_name);
        snprintf(r->gecos, sizeof r->gecos, "%s", p->pw_gecos);
        char hash[160];
        r->open = gap_shadow_lookup(r->name, hash, sizeof hash) == 0 && hash[0] == 0;
    }
    endpwent();
}

static void select_resident(int i) {
    if (g_nres == 0) return;
    g_sel = (i % g_nres + g_nres) % g_nres;
    memset(g_pw, 0, sizeof g_pw);
    g_msg = NULL;
    g_refocus = true;
}

/* Tells init who was let in. Standalone runs (no $GREETER_FD) just print the name. */
static void announce(const char *name) {
    const char *fdenv = getenv("GREETER_FD");
    int fd = fdenv ? atoi(fdenv) : -1;
    char line[48];
    int n = snprintf(line, sizeof line, "%s\n", name);
    if (fd > 0) { if (write(fd, line, (size_t)n) < 0) perror("greeter: write"); }
    else fputs(line, stdout);
}

static void attempt(void) {
    Resident *r = &g_res[g_sel];
    if (gap_authenticate(r->name, g_pw)) {
        gap_log("the gate opens");
        announce(r->name);
        memset(g_pw, 0, sizeof g_pw);
        kapp_quit(g_app, 0);
        return;
    }
    char note[96];
    snprintf(note, sizeof note, "%s was turned away at the gate", r->name);
    gap_log(note);
    memset(g_pw, 0, sizeof g_pw);
    g_msg = "Login incorrect";
    audio_event(AUDIO_EV_ERROR);
    g_fail_ms = k_now_ms();
    g_refocus = true;
}

static void ensure_wallpaper(KCanvas *cv) {
    if (g_wall && g_wall_w == cv->w && g_wall_h == cv->h) return;
    free(g_wall);
    g_wall = malloc((size_t)cv->w * (size_t)cv->h * 4);
    g_wall_w = cv->w;
    g_wall_h = cv->h;
    if (g_wall) k_scenery_night(g_wall, cv->w, cv->h, cv->w);
}

static void greeter_draw(KApp *app, KUI *ui, void *user) {
    (void)app; (void)user;
    KCanvas *cv = &ui->cv;
    ensure_wallpaper(cv);
    if (g_wall) for (int y = 0; y < cv->h; y++) memcpy(cv->px + (size_t)y * cv->stride, g_wall + (size_t)y * g_wall_w, (size_t)cv->w * 4);
    else kc_fill(cv, kr(0, 0, cv->w, cv->h), KC_NIGHT);
    kc_fill_alpha(cv, kr(0, 0, cv->w, cv->h), 0x050818, 70);

    /* clock and date, top right */
    static const char *const days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    char hm[16], date[40];
    snprintf(hm, sizeof hm, "%02d:%02d", t.tm_hour, t.tm_min);
    snprintf(date, sizeof date, "%s %d %s %d", days[t.tm_wday % 7], t.tm_mday, months[t.tm_mon % 12], t.tm_year + 1900);
    kc_text_scaled(cv, cv->w - 24 - (int)strlen(hm) * 24, 20, hm, 0xFFF6E0, 3);
    kc_text(cv, cv->w - 24 - kc_text_width(date), 72, date, 0xB9C2F0);

    /* the card */
    int cw = 480, ch = 372;
    int cx = (cv->w - cw) / 2, cy = (cv->h - ch) / 2 - 10;
    uint64_t since = ui->now_ms - g_fail_ms;
    if (g_fail_ms && since < 450) {                                   /* the card shakes its head */
        int amp = 10 - (int)(since / 50);
        cx += ((since / 45) % 2 ? 1 : -1) * amp;
        ui->redraw_soon = true;
    }
    kc_fill_alpha(cv, kr(cx + 6, cy + 9, cw, ch), 0x000000, 100);
    kc_fill_alpha(cv, kr(cx, cy, cw, ch), 0x0E1330, 238);
    kc_outline(cv, kr(cx, cy, cw, ch), 0x3A4A8C);
    kc_fill(cv, kr(cx, cy, cw, 5), KC_SHRINE);
    kc_fill(cv, kr(cx, cy + 5, cw, 1), KC_GOLD);

    struct utsname u;
    uname(&u);
    k_icon(cv, KI_YINYANG, cx + 24, cy + 22, 48);
    kc_text_scaled(cv, cx + 84, cy + 22, "Gensokyo", 0xFFF6E0, 2);
    char sub[96];
    snprintf(sub, sizeof sub, "%s  -  TouhouOS %s", u.nodename, u.release);
    kc_text(cv, cx + 86, cy + 60, sub, 0x9FA9DD);
    kc_text(cv, cx + 24, cy + 94, "Who is at the shrine gate?", KC_GOLD);

    /* the residents */
    if (g_nres == 0) {
        kc_text(cv, cx + 24, cy + 150, "Nobody lives in the Human Village yet.", 0xFFB0B0);
        kc_text(cv, cx + 24, cy + 172, "(No account with a uid of 1000 or more in /etc/passwd.)", 0x9FA9DD);
    }
    int cell = g_nres ? (cw - 48) / g_nres : 100;
    if (cell > 104) cell = 104;
    int rowx = cx + (cw - cell * g_nres) / 2, ay = cy + 124;
    for (int i = 0; i < g_nres; i++) {
        KRect cellr = kr(rowx + i * cell, ay - 6, cell, 98);
        bool hover = k_hover(ui, cellr);
        if (hover) ui->cursor_shape = FC_HAND;
        if (hover && ui->pressed[0] && i != g_sel) select_resident(i);
        int ax = cellr.x + cell / 2;
        k_avatar(cv, ax, ay + 32, g_res[i].name, i == g_sel ? 2 : hover ? 1 : 0);
        int tw = kc_text_width(g_res[i].name);
        kc_text(cv, ax - tw / 2, ay + 76, g_res[i].name, i == g_sel ? 0xFFF6E0 : 0x9FA9DD);
    }

    if (g_nres) {
        Resident *r = &g_res[g_sel];
        int nw = kc_text_width(r->gecos);
        kc_text_bold(cv, cx + (cw - nw) / 2, cy + 226, r->gecos, 0xFFE9A8);

        KRect fr = kr(cx + 60, cy + 250, cw - 120, 30);
        if (g_refocus) { k_focus_textfield(ui, g_pw); g_refocus = false; }
        bool enter = k_textfield_pw(ui, fr, g_pw, sizeof g_pw);
        if (!g_pw[0]) kc_text(cv, fr.x + 8, fr.y + 7, r->open ? "no password: just press Enter" : "Password", 0x8A8A96);

        if (g_msg) {
            int mw = kc_text_width(g_msg);
            kc_text(cv, cx + (cw - mw) / 2, cy + 288, g_msg, 0xFF8A8A);
            const char *eiki = "Eiki has made a note of it.";
            kc_text(cv, cx + (cw - kc_text_width(eiki)) / 2, cy + 306, eiki, 0x9FA9DD);
        }
        bool go = k_button(ui, kr(cx + 60, cy + 326, cw - 120, 34), "Log in");
        bool throttled = g_fail_ms && ui->now_ms - g_fail_ms < THROTTLE_MS;
        if ((enter || go) && !throttled) attempt();
        if (k_key_pressed(ui, KEY_DOWN) || (k_key_pressed(ui, KEY_TAB) && !(ui->mods & MOD_SHIFT))) select_resident(g_sel + 1);
        else if (k_key_pressed(ui, KEY_UP) || k_key_pressed(ui, KEY_TAB)) select_resident(g_sel - 1);
    }

    if (k_key_pressed(ui, KEY_POWER)) { audio_event_wait(AUDIO_EV_SHUTDOWN); sync(); gap_reboot(REBOOT_POWEROFF); }     /* the machine's power button */

    /* footer: a way out to the text console, and the power buttons */
    if (k_button_icon(ui, kr(16, cv->h - 48, 144, 32), KI_TERMINAL, "Text console")) kapp_quit(g_app, GREETER_EXIT_TEXT);
    if (k_button_icon(ui, kr(cv->w - 280, cv->h - 48, 128, 32), KI_POWER, "Shut down")) { audio_event_wait(AUDIO_EV_SHUTDOWN); sync(); gap_reboot(REBOOT_POWEROFF); }
    if (k_button_icon(ui, kr(cv->w - 144, cv->h - 48, 128, 32), KI_REFRESH, "Restart")) { audio_event_wait(AUDIO_EV_SHUTDOWN); sync(); gap_reboot(REBOOT_RESTART); }
}

int main(void) {
    gap_setname("Hakurei");
    load_residents();
    g_app = kapp_new_screen("Login", FW_FULLSCREEN | FW_NO_DECOR | FW_SKIP_TASKS);
    if (!g_app) return 1;
    kapp_set_draw(g_app, greeter_draw, NULL);
    kapp_set_tick(g_app, 1000);
    int rc = kapp_run(g_app);
    kapp_destroy(g_app);
    return rc;
}
