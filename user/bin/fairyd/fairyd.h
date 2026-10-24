/* fairyd internals: the Fairies window server. */
#ifndef FAIRYD_H
#define FAIRYD_H

#include <fairy.h>
#include <kappa.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MAX_WINS 128
#define BORDER   4
#define TITLE_H  22
#define DECOR_T  (BORDER + TITLE_H)
#define LAYER_DESKTOP 0
#define LAYER_NORMAL  1
#define LAYER_PANEL   2
#define LAYER_TOP     3
#define LAYER_FULL    4
#define LAYER_POPUP   5
#define LAYER_LOCK    6

typedef struct Client {
    int fd;
    uint32_t id;
    int pid;
    char name[32];
    uint8_t in[FAIRY_MSG_MAX * 2];
    size_t in_len;
    bool subscribed, is_shell, dead;
    struct Client *next;
} Client;

typedef struct Win {
    uint32_t id;
    Client *client;
    int x, y;                       /* outer top-left */
    int cw, ch;                     /* content size */
    uint32_t flags;
    unsigned state;
    char title[64];
    uint32_t *buf;                  /* client's pixels (shared mapping) */
    int bw, bh, bstride;                    /* buffer size in buffer pixels */
    int bscale;                             /* buffer pixels per logical pixel: S.scale for FW_HIDPI windows, else 1 */
    size_t map_len;
    int rx, ry, rcw, rch;           /* geometry to restore after maximize */
    int minw, minh;
    unsigned cursor;
    uint32_t parent;
    bool visible;
    int strut_edge, strut_size;
    uint64_t last_pointer_ms;
    char shm_path[64];
    bool maximized_before_min;
} Win;

typedef KRect Rect;

/* hit-test zones */
enum { Z_NONE, Z_CONTENT, Z_TITLE, Z_CLOSE, Z_MAX, Z_MIN, Z_N, Z_S, Z_E, Z_W, Z_NE, Z_NW, Z_SE, Z_SW };

typedef struct Server {
    int screen_w, screen_h;         /* logical: what clients see; the window manager works in these units */
    int scale;                      /* ui scale: physical pixels per logical pixel (1, 2, ...), see comp_init() */
    int phys_w, phys_h;             /* the part of the framebuffer in use: screen_w * scale x screen_h * scale */
    uint32_t *back;                 /* composed screen, 0x00RRGGBB, phys_w x phys_h */
    uint32_t *fb;                   /* mapped framebuffer */
    int fb_pitch;                   /* bytes */
    bool swap_rb;
    Rect dirty;                     /* bounding box of pending damage (w == 0: none) */
    Win *wins[MAX_WINS];            /* bottom to top */
    int nwins;
    Win *focus;
    Win *lock;                      /* the screen-lock window while one exists */
    Client *clients;
    uint32_t next_client, next_win;
    int cursor_x, cursor_y;         /* logical pointer position (ptr / scale) */
    int ptr_x, ptr_y;               /* the pointer in physical pixels: mouse motion counts in screen pixels */
    unsigned cursor_shape;
    int work_x, work_y, work_w, work_h;
    uint8_t buttons, mods;
    bool numlock;
    /* interaction */
    Win *grab;                      /* window receiving the pointer while a button is held */
    Win *drag_win;                  /* window being moved or resized */
    uint64_t last_title_click;
    Win *last_title_win;
    int drag_mode;                  /* 0 none, 1 move, 2 resize */
    int drag_zone;
    int drag_ox, drag_oy;           /* pointer offset inside the window at drag start */
    Rect drag_start;                /* outer rect at drag start */
    int drag_px, drag_py;
    Win *hover;
    /* alt-tab switcher */
    bool switching;
    bool super_alone;               /* the Super key went down and nothing else has been pressed since: a tap opens the launcher */
    int switch_index;
    Win *switch_list[MAX_WINS];
    int switch_n;
    /* clipboard */
    char clip[1025];
    size_t cliplen;
    bool quit;
    int input_fd, fb_fd, listen_fd;
    uint64_t now_ms;
    bool list_dirty;
} Server;

extern Server S;

/* server.c */
void server_init(void);
void server_accept(void);
void server_read_client(Client *c);
void server_drop_client(Client *c);
void send_msg(Client *c, uint16_t type, uint32_t win, uint32_t serial, const void *payload, size_t plen);
void server_send_window_list(void);
void server_mark_list_dirty(void);
void server_flush_list(void);
void server_send_hotkey(uint32_t which);
bool server_have_shell(void);
void win_destroy(Win *w);

/* wm.c */
int  layer_of(const Win *w);
Rect win_outer(const Win *w);
Rect win_content(const Win *w);
bool win_button(const Win *w, int zone, Rect *out);       /* position of the close/maximize/minimize button, if the window has it */
bool win_has_decor(const Win *w);
Win *win_at(int x, int y, int *zone);
int  zone_at(const Win *w, int x, int y);
void win_insert(Win *w);
void win_raise(Win *w);
void win_focus(Win *w);
void win_set_state(Win *w, unsigned state);
void win_set_geometry(Win *w, int x, int y, int cw, int ch);
void work_area_recompute(void);
void wm_pointer_event(int x, int y, uint8_t buttons, int button_changed, bool pressed, int wheel);
bool wm_key_shortcut(uint8_t code, uint8_t down);          /* true: handled by the window manager, do not forward */
void wm_focus_fallback(void);                               /* give the focus to the topmost focusable window */
void wm_close_popups(Win *except);
void wm_place_new(Win *w, int req_x, int req_y);
void wm_request_close(Win *w);
void wm_switcher_cycle(bool backwards);
void wm_switcher_commit(void);
Win *win_by_id(uint32_t id);
void send_key_to_focus(uint8_t code, uint8_t mods, uint8_t down, uint32_t ch);

/* comp.c */
void comp_init(int w, int h, uint32_t *fb, int pitch, bool swap_rb);
void comp_damage(Rect r);
void comp_damage_win(const Win *w);
void comp_flush(void);
void comp_screenshot(const char *path);
Rect rect_union(Rect a, Rect b);
Rect rect_intersect(Rect a, Rect b);
bool rect_empty(Rect r);
void cursor_move_to(int x, int y);          /* logical position */
void cursor_move_by(int dx, int dy);        /* relative motion in physical pixels (the mouse) */
void cursor_set_shape(unsigned shape);

#endif
