/*
 * hakurei-lock: locks the screen. One full-screen window of the kind the window server treats as the only thing that
 * exists: no shortcuts, no focus changes, nothing in front of it. The password is checked by /sbin/pwcheck, a setuid
 * helper that only ever answers for the user running it.
 */
#include <kappa.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pwd.h>
#include <sys/wait.h>
#include <gensokyo.h>
#include <audio.h>

#define THROTTLE_MS 1500

static char g_user[32], g_gecos[48];
static bool g_open;                                 /* the account has no password */
static char g_pw[64];
static const char *g_msg;
static uint64_t g_fail_ms;
static bool g_refocus = true;
static KApp *g_app;
static uint32_t *g_wall;
static int g_wall_w, g_wall_h;

/* Asks /sbin/pwcheck whether `pw` is the caller's password. */
static bool password_ok(const char *pw) {
    int fds[2];
    if (pipe(fds) < 0) return false;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return false; }
    if (pid == 0) {
        dup2(fds[0], 0);
        close(fds[0]);
        close(fds[1]);
        execl("/sbin/pwcheck", "pwcheck", (char *)NULL);
        _exit(127);
    }
    close(fds[0]);
    if (write(fds[1], pw, strlen(pw)) < 0 || write(fds[1], "\n", 1) < 0) { /* the child just reads nothing */ }
    close(fds[1]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void ensure_wallpaper(KCanvas *cv) {
    if (g_wall && g_wall_w == cv->w && g_wall_h == cv->h) return;
    free(g_wall);
    g_wall = malloc((size_t)cv->w * (size_t)cv->h * 4);
    g_wall_w = cv->w;
    g_wall_h = cv->h;
    if (g_wall) k_scenery_night(g_wall, cv->w, cv->h, cv->w);
}

static void lock_draw(KApp *app, KUI *ui, void *user) {
    (void)app; (void)user;
    KCanvas *cv = &ui->cv;
    ensure_wallpaper(cv);
    if (g_wall) for (int y = 0; y < cv->h; y++) memcpy(cv->px + (size_t)y * cv->stride, g_wall + (size_t)y * g_wall_w, (size_t)cv->w * 4);
    else kc_fill(cv, kr(0, 0, cv->w, cv->h), KC_NIGHT);
    kc_fill_alpha(cv, kr(0, 0, cv->w, cv->h), 0x050818, 150);                 /* darker than the login screen: this one is asleep */

    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    char hm[16], date[48];
    snprintf(hm, sizeof hm, "%02d:%02d", t.tm_hour, t.tm_min);
    strftime(date, sizeof date, "%A, %B %e", &t);
    int hw = (int)strlen(hm) * 8 * 6;
    kc_text_scaled(cv, (cv->w - hw) / 2, cv->h / 8, hm, 0xFFF6E0, 6);
    kc_text(cv, (cv->w - kc_text_width(date)) / 2, cv->h / 8 + 110, date, 0xB9C2F0);

    uint64_t since = ui->now_ms - g_fail_ms;
    int shake = 0;
    if (g_fail_ms && since < 450) { shake = ((since / 45) % 2 ? 1 : -1) * (10 - (int)(since / 50)); ui->redraw_soon = true; }
    int cw = 360, cx = (cv->w - cw) / 2 + shake, cy = cv->h / 2 - 20;
    k_avatar(cv, cx + cw / 2, cy + 36, g_user, 0);
    int nw = kc_text_width(g_gecos);
    kc_text_bold(cv, cx + (cw - nw) / 2, cy + 86, g_gecos, 0xFFE9A8);
    const char *locked = "The shrine is locked.";
    kc_text(cv, cx + (cw - kc_text_width(locked)) / 2, cy + 108, locked, 0x9FA9DD);

    KRect fr = kr(cx + 30, cy + 134, cw - 60, 30);
    if (g_refocus) { k_focus_textfield(ui, g_pw); g_refocus = false; }
    bool enter = k_textfield_pw(ui, fr, g_pw, sizeof g_pw);
    if (!g_pw[0]) kc_text(cv, fr.x + 8, fr.y + 7, g_open ? "no password: just press Enter" : "Password", 0x8A8A96);
    if (g_msg) kc_text(cv, cx + (cw - kc_text_width(g_msg)) / 2, cy + 172, g_msg, 0xFF8A8A);
    bool go = k_button(ui, kr(cx + 30, cy + 194, cw - 60, 32), "Unlock");
    bool throttled = g_fail_ms && ui->now_ms - g_fail_ms < THROTTLE_MS;
    if ((enter || go) && !throttled) {
        if (password_ok(g_pw)) {
            memset(g_pw, 0, sizeof g_pw);
            audio_event(AUDIO_EV_DIALOG);
            kapp_quit(g_app, 0);
            return;
        }
        memset(g_pw, 0, sizeof g_pw);
        g_msg = "That is not the password.";
        audio_event(AUDIO_EV_ERROR);
        g_fail_ms = k_now_ms();
        g_refocus = true;
        char note[80];
        snprintf(note, sizeof note, "%s failed to unlock the screen", g_user);
        gap_log(note);
    }
}

int main(void) {
    struct passwd *p = getpwuid(getuid());
    snprintf(g_user, sizeof g_user, "%s", p ? p->pw_name : "someone");
    snprintf(g_gecos, sizeof g_gecos, "%s", p && p->pw_gecos[0] ? p->pw_gecos : g_user);
    g_open = password_ok("") ;                                                 /* an account without a password accepts the empty one */
    g_app = kapp_new_screen("Locked", FW_LOCK);
    if (!g_app) return 1;
    kapp_set_draw(g_app, lock_draw, NULL);
    kapp_set_tick(g_app, 1000);
    gap_setname("lock");
    int rc = kapp_run(g_app);
    kapp_destroy(g_app);
    return rc;
}
