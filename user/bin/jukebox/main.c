/*
 * jukebox: Reimu's Jukebox, the music player, in the Prismriver Concert Hall.
 *
 *   jukebox [-p] [FILE.wav|FILE.tms|FILE.mod|DIRECTORY ...]   the window; with files, they come first in the playlist; -p starts playing
 *   jukebox --list                                   print the playlist (title, length, path) and exit
 *   jukebox --trace ...                              also print one line on stdout for each thing that happens
 *
 * The songs that ship with the system live in /ScarletDevilMansion/share/music; ~/Music is searched too. See tms(5).
 */
#include "jukebox.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void add_argument(const char *arg) {
    struct stat st;
    if (stat(arg, &st) != 0) { fprintf(stderr, "jukebox: %s: not found\n", arg); return; }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(arg);
        if (!d) return;
        struct dirent *e;
        while ((e = readdir(d))) {
            char path[320];
            snprintf(path, sizeof path, "%s/%s", arg, e->d_name);
            if (e->d_name[0] != '.') playlist_add(path, false);
        }
        closedir(d);
        return;
    }
    if (playlist_add(arg, false) < 0) fprintf(stderr, "jukebox: %s: not a .wav, .tms or .mod file\n", arg);
}

int main(int argc, char **argv) {
    bool autoplay = false, list_only = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        if (!strcmp(argv[first], "--trace")) J.trace = true;
        else if (!strcmp(argv[first], "--list")) list_only = true;
        else if (!strcmp(argv[first], "-p") || !strcmp(argv[first], "--play")) autoplay = true;
        else if (!strcmp(argv[first], "--")) { first++; break; }
        else { fprintf(stderr, "usage: jukebox [-p] [--trace] [--list] [FILE.wav|FILE.tms|FILE.mod|DIRECTORY ...]\n"); return 2; }
    }
    for (int i = first; i < argc; i++) add_argument(argv[i]);
    playlist_scan();

    if (list_only) {
        for (int i = 0; i < J.ntr; i++) {
            char clock[16];
            fmt_clock(J.tr[i].length, clock, sizeof clock);
            printf("%2d. %s (%s) %s%s%s\n", i + 1, J.tr[i].title, clock, J.tr[i].path, J.tr[i].err[0] ? " -- " : "", J.tr[i].err);
        }
        return 0;
    }

    player_init();
    KApp *app = kapp_new("Jukebox", 660, 440, FW_RESIZABLE);
    if (!app) { fprintf(stderr, "jukebox: no window server is running\n"); player_shutdown(); return 1; }
    J.app = app;
    kapp_set_minsize(app, 600, 380);
    ui_init();
    kapp_set_draw(app, ui_draw, NULL);
    kapp_set_tick(app, 200);
    trace("ready with %d tracks%s", J.ntr, J.no_device ? " (no sound device)" : "");
    if (autoplay && J.ntr) {
        J.list.selected = 0;                                 /* the first file named on the command line, else the first song */
        player_load(0, 0, true);
    }
    int rc = kapp_run(app);
    player_shutdown();
    kapp_destroy(app);
    playlist_free();
    return rc;
}
