/* The application table and process launching. */
#include "shell.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

const AppEntry g_apps[] = {
    { "Terminal",              "Run commands with gapsh",       KI_TERMINAL,  "gapterm",       "Programs" },
    { "Gensokyo Explorer",     "Browse your files",              KI_FOLDER,    "explorer",      "Programs" },
    { "Patchouli's Notebook",  "Edit text files",                KI_NOTEBOOK,  "notebook",      "Programs" },
    { "Ran's Calculator",      "Do the sums",                    KI_CALC,      "calculator",    "Programs" },
    { "Sakuya's Pocket Watch", "Clock, stopwatch and calendar",  KI_CLOCK,     "pocketwatch",   "Programs" },
    { "Bunbunmaru News",       "Today's paper",                  KI_NEWS,      "bunbunmaru",    "Programs" },
    { "Marisa's Sketchbook",   "Draw pictures",                  KI_SKETCH,    "sketchbook",    "Programs" },
    { "Reimu's Jukebox",       "Play music (Prismriver Concert Hall)", KI_TORII, "jukebox",       "Programs" },
    { "Spell Manager",         "Running spells (processes)",     KI_SPELLCARD, "spellmgr",      "System" },
    { "Kappa Control Panel",   "Settings and hardware",          KI_GEAR,      "kappa-panel",   "System" },
    { "Danmaku",               "Dodge the bullets",              KI_STAR,      "danmaku",       "Games" },
    { "Minesweeper",           "Find the fairies",               KI_GAME,      "minesweeper",   "Games" },
    { "Snake",                 "Eat the dango",                  KI_GAME,      "snake",         "Games" },
};
const int g_napps = (int)(sizeof g_apps / sizeof g_apps[0]);

static void split_args(char *cmd, char **argv, int max) {
    int n = 0;
    char *p = cmd;
    while (*p && n < max - 1) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[n++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    argv[n] = NULL;
}

void launch(const char *command) {
    char cmd[256];
    strncpy(cmd, command, sizeof cmd - 1);
    cmd[sizeof cmd - 1] = 0;
    char *argv[16];
    split_args(cmd, argv, 16);
    if (!argv[0]) return;
    pid_t pid = fork();
    if (pid != 0) return;
    setsid();
    int null = open("/dev/null", O_RDONLY);
    if (null >= 0) { dup2(null, 0); close(null); }
    char log[96];
    snprintf(log, sizeof log, "/tmp/%s.log", k_basename(argv[0]));
    int lf = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); if (lf > 2) close(lf); }
    const char *home = getenv("HOME");
    if (home) chdir(home);
    execvp(argv[0], argv);
    _exit(127);
}

void launch_app(int index) {
    if (index >= 0 && index < g_napps) launch(g_apps[index].command);
}

void reap_children(void) {
    while (waitpid(-1, NULL, WNOHANG) > 0) { }
}
