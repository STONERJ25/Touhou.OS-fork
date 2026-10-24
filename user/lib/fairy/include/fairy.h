/*
 * Fairies: the window system client library and wire protocol.
 *
 * A client connects to the window server (fairyd) over a UNIX socket, creates windows backed by shared-memory pixel
 * buffers (files in /tmp/fairy mapped MAP_SHARED by both sides), draws into them and sends COMMIT messages naming the
 * damaged rectangle. The server sends back input and window-management events. Pixels are 0x00RRGGBB.
 *
 * Messages are a 16-byte header followed by a fixed-layout payload; the whole message is at most FAIRY_MSG_MAX bytes.
 */
#ifndef FAIRY_H
#define FAIRY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <keys.h>

#define FAIRY_SOCKET   "/tmp/fairy/fairyd.sock"
#define FAIRY_SHM_DIR  "/tmp/fairy"
#define FAIRY_VERSION  1
#define FAIRY_MSG_MAX  4096
#define FAIRY_MAX_LIST 24                   /* windows reported to task lists */

/* ---- window flags (FM_CREATE) ---- */
#define FW_RESIZABLE   0x0001
#define FW_NO_DECOR    0x0002               /* client draws everything, no frame */
#define FW_DESKTOP     0x0004               /* bottom layer: the wallpaper and icons */
#define FW_PANEL       0x0008               /* docked panel (use FM_SET_STRUT to reserve space) */
#define FW_POPUP       0x0010               /* menus and tooltips: top layer, never takes focus */
#define FW_DIALOG      0x0020               /* no minimize/maximize buttons, centred over its parent */
#define FW_TOP         0x0040               /* always on top */
#define FW_NO_FOCUS    0x0080               /* clicks do not focus it */
#define FW_SKIP_TASKS  0x0100               /* hidden from task lists */
#define FW_FULLSCREEN  0x0200               /* covers the whole screen, no frame (games) */
#define FW_CENTER      0x0400               /* open in the middle of the work area */
#define FW_NO_CLOSE    0x0800               /* the close button is disabled */
#define FW_LOCK        0x1000               /* the screen lock: above everything, keeps keyboard and pointer to itself (one at a time) */
#define FW_HIDPI       0x2000               /* the pixel buffer is at the server's ui scale (w*scale x h*scale pixels); without it a buffer of
                                               w x h is enlarged by the server (nearest neighbour). Ignored at scale 1. See fairy_scale(). */

/* ---- window states ---- */
#define FS_NORMAL      0
#define FS_MAXIMIZED   1
#define FS_MINIMIZED   2

/* ---- cursor shapes ---- */
enum { FC_ARROW = 0, FC_TEXT, FC_HAND, FC_BUSY, FC_RESIZE_H, FC_RESIZE_V, FC_RESIZE_NWSE, FC_RESIZE_NESW, FC_MOVE, FC_CROSS, FC_NONE };

/* ---- message types ---- */
enum {
    FM_HELLO = 1, FM_CREATE, FM_DESTROY, FM_ATTACH, FM_COMMIT, FM_SET_TITLE, FM_SET_STATE, FM_SET_CURSOR, FM_MOVE, FM_RAISE,
    FM_ACTIVATE, FM_SUBSCRIBE, FM_SET_STRUT, FM_CLIPBOARD_SET, FM_CLIPBOARD_GET, FM_REGISTER_SHELL, FM_SET_PARENT, FM_SET_MINSIZE,
    FM_REQUEST_CLOSE, FM_SCREENSHOT, FM_QUIT_SERVER, FM_SET_ICON, FM_WARP_POINTER, FM_SET_MODE,
};
enum {
    FE_WELCOME = 100, FE_CREATED, FE_CONFIGURE, FE_KEY, FE_POINTER, FE_FOCUS, FE_CLOSE, FE_WINDOWS, FE_CLIPBOARD, FE_HOTKEY, FE_ERROR,
};

#pragma pack(push, 1)
typedef struct FairyHdr {
    uint32_t size;                          /* whole message including this header */
    uint16_t type;
    uint16_t pad;
    uint32_t win;                           /* window id, 0 if none */
    uint32_t serial;
} FairyHdr;

/* client -> server */
typedef struct { uint32_t version; int32_t pid; char name[32]; } FmHello;
typedef struct { int32_t w, h; uint32_t flags; int32_t x, y; uint32_t parent; char title[64]; char shm[64]; } FmCreate;   /* x,y = -1: let the server place it */
typedef struct { int32_t w, h, stride; char shm[64]; } FmAttach;            /* new pixel buffer; the old one is released (w, h: content size) */
typedef struct { int32_t x, y, w, h; } FmCommit;                            /* content coordinates; w <= 0: the whole window */
typedef struct { char title[64]; } FmTitle;
typedef struct { uint32_t state; } FmState;
typedef struct { uint32_t shape; } FmCursor;
typedef struct { int32_t x, y, w, h; } FmMove;                              /* outer position/size request; w,h <= 0 keeps the size */
typedef struct { uint32_t edge; int32_t size; } FmStrut;                    /* edge: 0 top, 1 bottom, 2 left, 3 right */
typedef struct { int32_t w, h; } FmMinSize;
typedef struct { uint32_t len; char data[1024]; } FmClipboard;
typedef struct { char path[256]; } FmPath;                                  /* FM_SCREENSHOT: write the picture there (.png, .bmp or .ppm; "" = the defaults) */
typedef struct { int32_t x, y; } FmWarp;
typedef struct { int32_t w, h; } FmMode;

/* server -> client */
typedef struct { uint32_t version; uint32_t client_id; int32_t screen_w, screen_h; int32_t work_x, work_y, work_w, work_h; int32_t scale; } FeWelcome;   /* sizes in logical pixels; scale: physical pixels per logical pixel */
typedef struct { int32_t content_w, content_h; int32_t outer_x, outer_y, outer_w, outer_h; uint32_t decor_l, decor_t, decor_r, decor_b; } FeCreated;
typedef struct { int32_t w, h; uint32_t state; uint32_t focused; } FeConfigure;     /* content size to adopt; reattach a buffer of that size */
typedef struct { uint8_t code, mods, down, pad; uint32_t ch; } FeKey;              /* down: 1 press, 0 release, 2 repeat; ch: Unicode code point or 0 */
enum { FP_MOVE = 0, FP_PRESS, FP_RELEASE, FP_ENTER, FP_LEAVE, FP_WHEEL };
typedef struct { int32_t x, y; uint8_t kind, button, buttons, mods; int32_t wheel; } FePointer;   /* buttons: bit0 left, bit1 right, bit2 middle */
typedef struct { uint32_t gained; } FeFocus;
typedef struct {
    uint32_t id; uint32_t flags; uint32_t state; uint32_t focused; int32_t pid; int32_t x, y, w, h; char title[48];
} FeWinInfo;
typedef struct { uint32_t count; FeWinInfo win[FAIRY_MAX_LIST]; } FeWindows;
typedef struct { uint32_t len; char data[1024]; } FeClipboard;
enum { FH_LAUNCHER = 1, FH_TERMINAL, FH_TASKS, FH_LOCK, FH_SCREENSHOT, FH_DESKTOP, FH_RUN, FH_POWER };
typedef struct { uint32_t which; } FeHotkey;
typedef struct { int32_t code; char text[64]; } FeError;
#pragma pack(pop)

/* ---------------------------------------------------------------- client library */

typedef struct FairyDisplay FairyDisplay;
typedef struct FairyWindow FairyWindow;

typedef enum {
    FEV_NONE = 0, FEV_KEY, FEV_POINTER, FEV_FOCUS, FEV_CONFIGURE, FEV_CLOSE, FEV_WINDOWS, FEV_CLIPBOARD, FEV_HOTKEY, FEV_DISCONNECTED,
} FairyEventType;

typedef struct FairyEvent {
    FairyEventType type;
    FairyWindow *window;                    /* NULL for events that belong to the display (window lists, hotkeys) */
    union {
        FeKey key;
        FePointer pointer;
        FeFocus focus;
        FeConfigure configure;
        FeWindows windows;
        FeClipboard clipboard;
        FeHotkey hotkey;
    } u;
} FairyEvent;

/* Coordinates: everything the protocol carries (screen and work area, window positions and sizes, pointer events, commits)
 * is in logical pixels. At ui scale N (2 on large screens, or ui.scale=N on the kernel command line) the screen has N x N
 * physical pixels per logical pixel: windows created with FW_HIDPI draw into a buffer N times as wide and high, the others are
 * enlarged by the server. At scale 1 the two are the same thing. */
FairyDisplay *fairy_connect(const char *app_name);                  /* NULL if no window server answers */
int           fairy_scale(FairyDisplay *d);                         /* the ui scale: 1, 2, ... */
void          fairy_disconnect(FairyDisplay *d);
int           fairy_fd(FairyDisplay *d);                            /* poll() this to learn about events */
void          fairy_screen_size(FairyDisplay *d, int *w, int *h);
void          fairy_work_area(FairyDisplay *d, int *x, int *y, int *w, int *h);   /* screen minus panels */

FairyWindow  *fairy_window_create(FairyDisplay *d, int w, int h, const char *title, unsigned flags);
FairyWindow  *fairy_window_create_at(FairyDisplay *d, int x, int y, int w, int h, const char *title, unsigned flags, FairyWindow *parent);
void          fairy_window_destroy(FairyWindow *win);
uint32_t     *fairy_window_pixels(FairyWindow *win, int *w, int *h, int *stride);   /* buffer size and stride in buffer pixels */
int           fairy_window_scale(FairyWindow *win);                 /* buffer pixels per logical pixel: fairy_scale() for FW_HIDPI windows, else 1 */
void          fairy_window_commit(FairyWindow *win, int x, int y, int w, int h);    /* logical content rectangle; w <= 0: everything */
int           fairy_window_resize(FairyWindow *win, int w, int h);  /* give the window a new buffer for content size w x h (answer to FEV_CONFIGURE); contents are lost */
void          fairy_window_set_title(FairyWindow *win, const char *title);
void          fairy_window_set_state(FairyWindow *win, unsigned state);
void          fairy_window_set_cursor(FairyWindow *win, unsigned shape);
void          fairy_window_move(FairyWindow *win, int x, int y, int w, int h);
void          fairy_window_raise(FairyWindow *win);
void          fairy_window_activate(FairyDisplay *d, uint32_t id);  /* focus + raise + unminimize any window (task lists) */
void          fairy_window_set_state_id(FairyDisplay *d, uint32_t id, unsigned state);   /* shell clients only: minimize/restore any window */
void          fairy_window_close_request(FairyDisplay *d, uint32_t id);
void          fairy_window_set_minsize(FairyWindow *win, int w, int h);
uint32_t      fairy_window_id(FairyWindow *win);
void          fairy_window_geometry(FairyWindow *win, int *x, int *y, int *w, int *h);   /* outer rectangle as last reported */
unsigned      fairy_window_state(FairyWindow *win);
void          fairy_window_set_user(FairyWindow *win, void *user);
void         *fairy_window_user(FairyWindow *win);
FairyWindow  *fairy_window_by_id(FairyDisplay *d, uint32_t id);

void          fairy_subscribe_windows(FairyDisplay *d);             /* receive FEV_WINDOWS whenever the window list changes */
void          fairy_register_shell(FairyDisplay *d);                /* receive FEV_HOTKEY for launcher/terminal/lock chords */
void          fairy_set_strut(FairyWindow *win, int edge, int size);
void          fairy_clipboard_set(FairyDisplay *d, const char *text, size_t len);
void          fairy_clipboard_request(FairyDisplay *d);             /* answer arrives as FEV_CLIPBOARD */
void          fairy_screenshot(FairyDisplay *d, const char *path);  /* by extension: .png (written in the background), .bmp, else PPM; "" = /tmp/screenshot.ppm plus a PNG in ~/Pictures */
void          fairy_quit_server(FairyDisplay *d);
void          fairy_warp_pointer(FairyDisplay *d, int x, int y);
void          fairy_set_mode(FairyDisplay *d, int w, int h);

/* Waits up to timeout_ms (-1: forever, 0: poll) for the next event. Returns 1 with *ev filled, 0 on timeout, -1 if the
 * server went away. Messages for destroyed windows are dropped silently. */
int           fairy_next_event(FairyDisplay *d, FairyEvent *ev, int timeout_ms);

/* Key helpers shared by the server and clients: US-layout translation of a key press. */
uint32_t      fairy_key_char(uint8_t code, uint8_t mods, bool numlock);

#endif
