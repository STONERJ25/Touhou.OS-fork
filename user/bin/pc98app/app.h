/*
 * The desktop side of the PC-98 application: the window, the keys, the sound device, the menus and the dialogs, around the machine of emu.h.
 * (The machine and everything that does not need a window is tested on the host: tests/host/test_pc98app.py; this part is tested in
 * QEMU: tests/case_pc98app.py.) See docs/PC98_APP.md.
 *
 *   main.c     the command line, the settings files, the benchmark, the window and its loop
 *   ui.c       one frame of the window: the machine's picture, the menu bar, the status line, the F12 menu, the commands behind all of them
 *   dialogs.c  message boxes, the About box, "save changes?", the file dialogs for inserting and saving images
 *   sound.c    the sound device: what the machine's sound pipe holds goes to /dev/audio without ever blocking the emulation
 */
#ifndef PC98APP_APP_H
#define PC98APP_APP_H

#include <kappa.h>
#include <audio.h>
#include "emu.h"

/* The window: a menu bar, the 640 x 400 picture (scaled by a whole number, centred), a status line. */
#define MENU_H 24
#define STATUS_H 22

/* The settings the menus change (and the settings file then remembers): what the command line says for one run is not written back. */
#define TOUCH_SPEED 1
#define TOUCH_SOUND 2
#define TOUCH_VOLUME 4
#define TOUCH_SCALE 8
#define TOUCH_IDLE 16

typedef struct App {
    KApp *kapp;
    Emu *emu;
    Pc98Recent recent;
    Pc98Config file_cfg;            /* the settings file as it was read */
    unsigned touched;               /* TOUCH_* bits: settings changed from a menu, to be merged into file_cfg when it is written */
    int trace;                      /* --trace: lines on stdout for the tests */
    /* sound */
    Audio *audio;                   /* NULL: the machine has no sound device (or sound is off) */
    int audio_error_shown;
    /* what the window shows and does */
    int overlay;                    /* the F12 menu is open */
    int overlay_sel;
    int dialogs;                    /* windows of this program that are open besides the main one (a message, a file dialog) */
    int quit_asked;                 /* the window was asked to close with changes unsaved: a question is on the screen */
    int confirmed_quit;
    int focused;
    int menu_was_open;
    uint64_t last_status_ns;        /* the status line is rebuilt about twice a second */
    char status[300];
    char title[220];
    uint64_t picture_serial;        /* counts the pictures the machine made */
    int last_w, last_h;
    uint64_t last_changed_check_ns;
    uint32_t changed_blocks;        /* what the guest has changed in its disks, as last counted */
    char dir[CONFIG_PATH_MAX];      /* the folder the last image came from: where the file dialogs start */
    int save_slot;                  /* the drive whose changes a save dialog is about */
    char save_default[DISK_PATH_MAX];
    int stop_shown;                 /* the stop message of the current stop was shown already */
    int quit_after_save;            /* a save was started from the "quit?" question: quit when nothing is left to save */
} App;

extern App A;

uint64_t now_ns(void);
void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));       /* a trace line (with --trace) */

/* main.c */
void settings_save(void);           /* the settings file: the file as it was read with the settings changed from a menu merged in */
void recent_note(const char *path); /* an image was used: first in the recent list (written at once) */

/* ui.c */
void ui_draw(KApp *app, KUI *ui, void *user);
bool ui_event(KApp *app, const FairyEvent *ev);                              /* the key and focus events, before the widgets see them; true: used */
void ui_command(int cmd);
void ui_update_title(void);
enum {
    CMD_NONE = 0, CMD_PAUSE, CMD_RESET, CMD_RESET_HD, CMD_INSERT_FD1, CMD_INSERT_FD2, CMD_INSERT_HD, CMD_EJECT_FD1, CMD_EJECT_FD2,
    CMD_SAVE, CMD_REVERT, CMD_RECENT0, CMD_QUIT = CMD_RECENT0 + RECENT_MAX, CMD_SPEED_50, CMD_SPEED_100, CMD_SPEED_200, CMD_SPEED_MAX,
    CMD_SOUND, CMD_VOLUME_UP, CMD_VOLUME_DOWN, CMD_SCALE_1, CMD_SCALE_2, CMD_SCALE_3, CMD_SCALE_FIT, CMD_IDLE, CMD_ABOUT, CMD_OVERLAY,
};

/* dialogs.c */
void dlg_message(const char *title, KIcon icon, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void dlg_about(void);
void dlg_insert(int slot);                                                   /* a file dialog, then the image goes into that drive */
void dlg_save(int slot);                                                     /* a file dialog for the new image's name, then the save */
void dlg_ask_quit(void);                                                     /* changes are unsaved: save them, discard them, or stay */
void dlg_open_recent(int index);
bool dlg_close_handler(KApp *app, void *user);
void dlg_default_save_name(int slot, char *out, size_t cap);

/* sound.c */
void sound_open(void);
void sound_pump(void);
void sound_flush(void);                                                      /* throws away what the device still holds (pause, reset) */
void sound_close(void);

#endif
