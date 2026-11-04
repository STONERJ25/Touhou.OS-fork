/* hakurei-shell: wallpaper, icons and panel for the desktop session. */
#include "shell.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <gensokyo.h>
#include <audio.h>

Shell G;

static void event_hook(KApp *app, const FairyEvent *ev, void *user) {
    (void)app; (void)user;
    switch (ev->type) {
    case FEV_WINDOWS:
        G.windows = ev->u.windows;
        if (G.panel) kwin_redraw(G.panel);
        break;
    case FEV_HOTKEY:
        switch (ev->u.hotkey.which) {
        case FH_LAUNCHER: menu_open(); break;
        case FH_TERMINAL: launch("gapterm"); break;
        case FH_TASKS: launch("spellmgr"); break;
        case FH_LOCK: menu_close(); launch("hakurei-lock"); break;
        case FH_POWER:                                                  /* the power button: ask first, and a second press means it */
            if (G.shutdown) { audio_event_wait(AUDIO_EV_SHUTDOWN); sync(); gap_reboot(REBOOT_POWEROFF); }
            else shutdown_open();
            break;
        default: break;
        }
        break;
    default:
        break;
    }
}

int main(void) {
    G.app = kapp_new_screen("Desktop", FW_DESKTOP | FW_NO_DECOR | FW_SKIP_TASKS);
    if (!G.app) return 1;
    fairy_screen_size(kapp_display(G.app), &G.screen_w, &G.screen_h);
    G.selected_icon = -1;
    kapp_set_draw(G.app, desktop_draw, NULL);
    kapp_set_event_hook(G.app, event_hook, NULL);
    kapp_set_tick(G.app, 1000);
    fairy_register_shell(kapp_display(G.app));
    fairy_subscribe_windows(kapp_display(G.app));
    panel_open();
    audio_event(AUDIO_EV_LOGIN);                                      /* the shrine bell: the desktop is open (silent without sound hardware, or with sounds off) */
    int rc = kapp_run(G.app);
    kapp_destroy(G.app);
    return rc;
}
