/* hakurei-shell: the HakureiDE session shell: wallpaper and icons, the panel with start menu, task list and clock. */
#ifndef HAKUREI_SHELL_H
#define HAKUREI_SHELL_H

#include <kappa.h>

typedef struct AppEntry {
    const char *name;               /* shown in menus and on icons */
    const char *tooltip;
    KIcon icon;
    const char *command;            /* argv split on spaces */
    const char *section;            /* "Programs", "Games", "System" */
} AppEntry;

extern const AppEntry g_apps[];
extern const int g_napps;

typedef struct Shell {
    KApp *app;
    KWin *panel;
    KWin *menu;                     /* start menu popup, or NULL */
    KWin *calendar;
    KWin *context;                  /* desktop right-click menu */
    KWin *about, *shutdown;
    int screen_w, screen_h;
    FeWindows windows;              /* latest task list */
    int selected_icon;
    int menu_hover;
    int cal_month_offset;
    uint64_t start_ms;
    int cpu_percent, mem_percent;
    unsigned long last_cpu_ticks, last_idle;
    uint64_t last_sample_ms;
    bool quit;
} Shell;

extern Shell G;

/* the wallpaper itself lives in libkappa (scenery.c) so the login screen can share it */
#define wallpaper_render k_scenery_night

/* panel.c */
#define PANEL_H 36
void panel_open(void);
void panel_draw(KApp *app, KUI *ui, void *user);
void menu_open(void);
void menu_close(void);
void calendar_toggle(void);
void sample_system(void);

/* volume.c: the speaker in the tray, only when the machine has a sound device */
bool volume_present(void);
void volume_tray(KUI *ui, KRect r);
void volume_close(void);

/* desktop.c */
void desktop_draw(KApp *app, KUI *ui, void *user);
void context_open(int x, int y);
void about_open(void);
void shutdown_open(void);

/* launch.c */
void launch(const char *command);
void launch_app(int index);
void reap_children(void);

#endif
