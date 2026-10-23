/* Fairies client library: connection, windows with shared pixel buffers, and the event queue. */
#include <fairy.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define QUEUE_LEN 256

struct FairyWindow {
    FairyDisplay *d;
    uint32_t id;
    int w, h, stride;
    uint32_t *px;
    size_t map_len;
    int ox, oy, ow, oh;                     /* outer geometry as last reported */
    unsigned state;
    unsigned flags;
    void *user;
    bool created;
    struct FairyWindow *next;
};

struct FairyDisplay {
    int fd;
    uint32_t client_id;
    int screen_w, screen_h;
    int work_x, work_y, work_w, work_h;
    uint32_t serial;
    int shm_counter;
    FairyWindow *windows;
    uint8_t in[FAIRY_MSG_MAX * 2];
    size_t in_len;
    FairyEvent queue[QUEUE_LEN];
    int q_head, q_tail;
    bool dead;
    int pid;
};

/* ---------------------------------------------------------------- low level */

static bool send_msg(FairyDisplay *d, uint16_t type, uint32_t win, const void *payload, size_t plen) {
    if (d->dead) return false;
    uint8_t buf[FAIRY_MSG_MAX];
    FairyHdr h = { (uint32_t)(sizeof h + plen), type, 0, win, ++d->serial };
    memcpy(buf, &h, sizeof h);
    if (plen) memcpy(buf + sizeof h, payload, plen);
    size_t off = 0;
    while (off < h.size) {
        ssize_t n = write(d->fd, buf + off, h.size - off);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            d->dead = true;
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

static FairyWindow *find_window(FairyDisplay *d, uint32_t id) {
    for (FairyWindow *w = d->windows; w; w = w->next) if (w->id == id) return w;
    return NULL;
}

static void push_event(FairyDisplay *d, const FairyEvent *ev) {
    int next = (d->q_head + 1) % QUEUE_LEN;
    if (next == d->q_tail) return;                          /* queue full: drop the newest */
    d->queue[d->q_head] = *ev;
    d->q_head = next;
}

/* Handles one complete message. Returns the message type (so synchronous calls can wait for a specific reply). */
static int dispatch(FairyDisplay *d, const FairyHdr *h, const uint8_t *payload, FairyWindow **created_out) {
    FairyEvent ev;
    memset(&ev, 0, sizeof ev);
    FairyWindow *w = h->win ? find_window(d, h->win) : NULL;
    switch (h->type) {
    case FE_WELCOME: {
        const FeWelcome *m = (const FeWelcome *)payload;
        d->client_id = m->client_id;
        d->screen_w = m->screen_w; d->screen_h = m->screen_h;
        d->work_x = m->work_x; d->work_y = m->work_y; d->work_w = m->work_w; d->work_h = m->work_h;
        break;
    }
    case FE_CREATED: {
        const FeCreated *m = (const FeCreated *)payload;
        w = find_window(d, 0x80000000u | h->serial);        /* the reply echoes the serial of our FM_CREATE */
        if (w) {
            w->id = h->win;                                 /* the server's real window id replaces the provisional one */
            w->created = true;
            w->ox = m->outer_x; w->oy = m->outer_y; w->ow = m->outer_w; w->oh = m->outer_h;
            if ((m->content_w != w->w || m->content_h != w->h) && m->content_w > 0 && m->content_h > 0) {
                ev.type = FEV_CONFIGURE; ev.window = w;
                ev.u.configure = (FeConfigure){ m->content_w, m->content_h, w->state, 1 };
                push_event(d, &ev);
            }
            if (created_out) *created_out = w;
        }
        break;
    }
    case FE_CONFIGURE: {
        if (!w) break;
        const FeConfigure *m = (const FeConfigure *)payload;
        w->state = m->state;
        ev.type = FEV_CONFIGURE; ev.window = w; ev.u.configure = *m;
        push_event(d, &ev);
        break;
    }
    case FE_KEY:
        if (!w) break;
        ev.type = FEV_KEY; ev.window = w; ev.u.key = *(const FeKey *)payload;
        push_event(d, &ev);
        break;
    case FE_POINTER:
        if (!w) break;
        ev.type = FEV_POINTER; ev.window = w; ev.u.pointer = *(const FePointer *)payload;
        push_event(d, &ev);
        break;
    case FE_FOCUS:
        if (!w) break;
        ev.type = FEV_FOCUS; ev.window = w; ev.u.focus = *(const FeFocus *)payload;
        push_event(d, &ev);
        break;
    case FE_CLOSE:
        if (!w) break;
        ev.type = FEV_CLOSE; ev.window = w;
        push_event(d, &ev);
        break;
    case FE_WINDOWS: {
        ev.type = FEV_WINDOWS;
        size_t n = h->size - sizeof *h;
        memcpy(&ev.u.windows, payload, n < sizeof ev.u.windows ? n : sizeof ev.u.windows);
        push_event(d, &ev);
        break;
    }
    case FE_CLIPBOARD:
        ev.type = FEV_CLIPBOARD;
        memcpy(&ev.u.clipboard, payload, sizeof ev.u.clipboard < h->size - sizeof *h ? sizeof ev.u.clipboard : h->size - sizeof *h);
        push_event(d, &ev);
        break;
    case FE_HOTKEY:
        ev.type = FEV_HOTKEY; ev.u.hotkey = *(const FeHotkey *)payload;
        push_event(d, &ev);
        break;
    default:
        break;
    }
    return h->type;
}

/* Reads from the socket (waiting up to timeout_ms) and dispatches every complete message. Returns the type of the last
 * message handled, 0 if nothing arrived, -1 on disconnect. */
static int pump(FairyDisplay *d, int timeout_ms, FairyWindow **created_out) {
    if (d->dead) return -1;
    struct pollfd pf = { d->fd, POLLIN, 0 };
    if (timeout_ms != 0 || d->in_len == 0) {
        int r = poll(&pf, 1, timeout_ms);
        if (r < 0) return errno == EINTR ? 0 : -1;
        if (r == 0) return 0;
        if (pf.revents & POLLIN) {
            ssize_t n = read(d->fd, d->in + d->in_len, sizeof d->in - d->in_len);
            if (n <= 0) { if (n < 0 && (errno == EINTR || errno == EAGAIN)) return 0; d->dead = true; return -1; }
            d->in_len += (size_t)n;
        } else if (pf.revents & (POLLHUP | POLLERR)) {
            d->dead = true;
            return -1;
        }
    }
    int last = 0;
    size_t off = 0;
    while (d->in_len - off >= sizeof(FairyHdr)) {
        FairyHdr h;
        memcpy(&h, d->in + off, sizeof h);
        if (h.size < sizeof h || h.size > FAIRY_MSG_MAX) { d->dead = true; return -1; }
        if (d->in_len - off < h.size) break;
        last = dispatch(d, &h, d->in + off + sizeof h, created_out);
        off += h.size;
    }
    if (off) {
        memmove(d->in, d->in + off, d->in_len - off);
        d->in_len -= off;
    }
    return last ? last : 0;
}

/* ---------------------------------------------------------------- connection */

FairyDisplay *fairy_connect(const char *app_name) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, FAIRY_SOCKET);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return NULL; }
    FairyDisplay *d = calloc(1, sizeof *d);
    if (!d) { close(fd); return NULL; }
    d->fd = fd;
    d->pid = getpid();
    FmHello hello;
    memset(&hello, 0, sizeof hello);
    hello.version = FAIRY_VERSION;
    hello.pid = d->pid;
    strncpy(hello.name, app_name ? app_name : "app", sizeof hello.name - 1);
    send_msg(d, FM_HELLO, 0, &hello, sizeof hello);
    for (int tries = 0; tries < 200 && d->screen_w == 0; tries++) {
        if (pump(d, 50, NULL) < 0) break;
    }
    if (d->screen_w == 0) { close(fd); free(d); return NULL; }
    return d;
}

void fairy_disconnect(FairyDisplay *d) {
    if (!d) return;
    while (d->windows) fairy_window_destroy(d->windows);
    close(d->fd);
    free(d);
}

int fairy_fd(FairyDisplay *d) { return d->fd; }
void fairy_screen_size(FairyDisplay *d, int *w, int *h) { if (w) *w = d->screen_w; if (h) *h = d->screen_h; }
void fairy_work_area(FairyDisplay *d, int *x, int *y, int *w, int *h) {
    if (x) *x = d->work_x; if (y) *y = d->work_y; if (w) *w = d->work_w; if (h) *h = d->work_h;
}

/* ---------------------------------------------------------------- windows */

static int map_buffer(FairyDisplay *d, FairyWindow *win, int w, int h, char *path_out, size_t path_cap,
                      uint32_t **px, size_t *len) {
    size_t bytes = (size_t)w * (size_t)h * 4;
    size_t maplen = (bytes + 4095) & ~(size_t)4095;
    if (!maplen) maplen = 4096;
    mkdir(FAIRY_SHM_DIR, 0777);
    snprintf(path_out, path_cap, "%s/%d-%d.buf", FAIRY_SHM_DIR, d->pid, d->shm_counter++);
    int fd = open(path_out, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)maplen) < 0) { close(fd); unlink(path_out); return -1; }
    void *m = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { unlink(path_out); return -1; }
    (void)win;
    *px = m;
    *len = maplen;
    return 0;
}

FairyWindow *fairy_window_create_at(FairyDisplay *d, int x, int y, int w, int h, const char *title, unsigned flags, FairyWindow *parent) {
    if (!d || d->dead || w < 1 || h < 1) return NULL;
    FairyWindow *win = calloc(1, sizeof *win);
    if (!win) return NULL;
    FmCreate cr;
    memset(&cr, 0, sizeof cr);
    if (map_buffer(d, win, w, h, cr.shm, sizeof cr.shm, &win->px, &win->map_len) < 0) { free(win); return NULL; }
    win->d = d;
    win->w = w; win->h = h; win->stride = w;
    win->flags = flags;
    cr.w = w; cr.h = h; cr.flags = flags; cr.x = x; cr.y = y;
    cr.parent = parent ? parent->id : 0;
    strncpy(cr.title, title ? title : "", sizeof cr.title - 1);
    /* The server answers with the window id in the header's serial field; register under a temporary id first. */
    uint32_t my_serial = d->serial + 1;
    win->id = 0x80000000u | my_serial;                      /* provisional: replies carry the serial we sent */
    win->next = d->windows;
    d->windows = win;
    send_msg(d, FM_CREATE, win->id, &cr, sizeof cr);
    for (int tries = 0; tries < 400 && !win->created; tries++) {
        int r = pump(d, 25, NULL);
        if (r < 0) break;
    }
    if (!win->created) {
        fairy_window_destroy(win);
        return NULL;
    }
    return win;
}

FairyWindow *fairy_window_create(FairyDisplay *d, int w, int h, const char *title, unsigned flags) {
    return fairy_window_create_at(d, -1, -1, w, h, title, flags, NULL);
}

void fairy_window_destroy(FairyWindow *win) {
    if (!win) return;
    FairyDisplay *d = win->d;
    if (win->created) send_msg(d, FM_DESTROY, win->id, NULL, 0);
    for (FairyWindow **pp = &d->windows; *pp; pp = &(*pp)->next)
        if (*pp == win) { *pp = win->next; break; }
    if (win->px) munmap(win->px, win->map_len);
    /* drop queued events that mention it */
    for (int i = d->q_tail; i != d->q_head; i = (i + 1) % QUEUE_LEN)
        if (d->queue[i].window == win) d->queue[i].type = FEV_NONE;
    free(win);
}

uint32_t *fairy_window_pixels(FairyWindow *win, int *w, int *h, int *stride) {
    if (w) *w = win->w; if (h) *h = win->h; if (stride) *stride = win->stride;
    return win->px;
}

void fairy_window_commit(FairyWindow *win, int x, int y, int w, int h) {
    FmCommit c = { x, y, w, h };
    if (w <= 0 || h <= 0) { c.x = 0; c.y = 0; c.w = win->w; c.h = win->h; }
    send_msg(win->d, FM_COMMIT, win->id, &c, sizeof c);
}

int fairy_window_resize(FairyWindow *win, int w, int h) {
    if (w < 1 || h < 1) return -1;
    if (w == win->w && h == win->h) return 0;
    FmAttach at;
    memset(&at, 0, sizeof at);
    uint32_t *px;
    size_t len;
    if (map_buffer(win->d, win, w, h, at.shm, sizeof at.shm, &px, &len) < 0) return -1;
    at.w = w;                                               /* contents are lost; the caller redraws */ at.h = h; at.stride = w;
    send_msg(win->d, FM_ATTACH, win->id, &at, sizeof at);
    munmap(win->px, win->map_len);
    win->px = px; win->map_len = len; win->w = w; win->h = h; win->stride = w;
    return 0;
}

void fairy_window_set_title(FairyWindow *win, const char *title) {
    FmTitle t;
    memset(&t, 0, sizeof t);
    strncpy(t.title, title, sizeof t.title - 1);
    send_msg(win->d, FM_SET_TITLE, win->id, &t, sizeof t);
}

void fairy_window_set_state(FairyWindow *win, unsigned state) { FmState s = { state }; send_msg(win->d, FM_SET_STATE, win->id, &s, sizeof s); }
void fairy_window_set_cursor(FairyWindow *win, unsigned shape) { FmCursor c = { shape }; send_msg(win->d, FM_SET_CURSOR, win->id, &c, sizeof c); }
void fairy_window_move(FairyWindow *win, int x, int y, int w, int h) { FmMove m = { x, y, w, h }; send_msg(win->d, FM_MOVE, win->id, &m, sizeof m); }
void fairy_window_raise(FairyWindow *win) { send_msg(win->d, FM_RAISE, win->id, NULL, 0); }
void fairy_window_activate(FairyDisplay *d, uint32_t id) { send_msg(d, FM_ACTIVATE, id, NULL, 0); }
void fairy_window_close_request(FairyDisplay *d, uint32_t id) { send_msg(d, FM_REQUEST_CLOSE, id, NULL, 0); }
void fairy_window_set_minsize(FairyWindow *win, int w, int h) { FmMinSize m = { w, h }; send_msg(win->d, FM_SET_MINSIZE, win->id, &m, sizeof m); }
uint32_t fairy_window_id(FairyWindow *win) { return win->id; }
void fairy_window_geometry(FairyWindow *win, int *x, int *y, int *w, int *h) {
    if (x) *x = win->ox; if (y) *y = win->oy; if (w) *w = win->ow; if (h) *h = win->oh;
}
unsigned fairy_window_state(FairyWindow *win) { return win->state; }
void fairy_window_set_user(FairyWindow *win, void *user) { win->user = user; }
void *fairy_window_user(FairyWindow *win) { return win->user; }
FairyWindow *fairy_window_by_id(FairyDisplay *d, uint32_t id) { return find_window(d, id); }

void fairy_subscribe_windows(FairyDisplay *d) { send_msg(d, FM_SUBSCRIBE, 0, NULL, 0); }
void fairy_register_shell(FairyDisplay *d) { send_msg(d, FM_REGISTER_SHELL, 0, NULL, 0); }
void fairy_set_strut(FairyWindow *win, int edge, int size) { FmStrut s = { (uint32_t)edge, size }; send_msg(win->d, FM_SET_STRUT, win->id, &s, sizeof s); }

void fairy_clipboard_set(FairyDisplay *d, const char *text, size_t len) {
    FmClipboard c;
    if (len > sizeof c.data) len = sizeof c.data;
    c.len = (uint32_t)len;
    memcpy(c.data, text, len);
    send_msg(d, FM_CLIPBOARD_SET, 0, &c, sizeof c.len + len);
}
void fairy_clipboard_request(FairyDisplay *d) { send_msg(d, FM_CLIPBOARD_GET, 0, NULL, 0); }
void fairy_screenshot(FairyDisplay *d, const char *path) { FmPath p; memset(&p, 0, sizeof p); strncpy(p.path, path, sizeof p.path - 1); send_msg(d, FM_SCREENSHOT, 0, &p, sizeof p); }
void fairy_quit_server(FairyDisplay *d) { send_msg(d, FM_QUIT_SERVER, 0, NULL, 0); }
void fairy_warp_pointer(FairyDisplay *d, int x, int y) { FmWarp w = { x, y }; send_msg(d, FM_WARP_POINTER, 0, &w, sizeof w); }
void fairy_set_mode(FairyDisplay *d, int w, int h) { FmMode m = { w, h }; send_msg(d, FM_SET_MODE, 0, &m, sizeof m); }

/* ---------------------------------------------------------------- events */

int fairy_next_event(FairyDisplay *d, FairyEvent *ev, int timeout_ms) {
    for (;;) {
        while (d->q_tail != d->q_head) {
            FairyEvent *e = &d->queue[d->q_tail];
            d->q_tail = (d->q_tail + 1) % QUEUE_LEN;
            if (e->type != FEV_NONE) { *ev = *e; return 1; }
        }
        if (d->dead) { ev->type = FEV_DISCONNECTED; ev->window = NULL; return -1; }
        int r = pump(d, timeout_ms, NULL);
        if (r < 0) { ev->type = FEV_DISCONNECTED; ev->window = NULL; return -1; }
        if (d->q_tail == d->q_head) return 0;               /* timeout, or only internal messages arrived */
    }
}

/* ---------------------------------------------------------------- keys */

static const struct { char base, shift; } g_keymap[0x59] = {
    [0x01] = { 27, 27 }, [0x02] = { '1', '!' }, [0x03] = { '2', '@' }, [0x04] = { '3', '#' }, [0x05] = { '4', '$' },
    [0x06] = { '5', '%' }, [0x07] = { '6', '^' }, [0x08] = { '7', '&' }, [0x09] = { '8', '*' }, [0x0A] = { '9', '(' },
    [0x0B] = { '0', ')' }, [0x0C] = { '-', '_' }, [0x0D] = { '=', '+' }, [0x0E] = { 8, 8 }, [0x0F] = { '\t', '\t' },
    [0x10] = { 'q', 'Q' }, [0x11] = { 'w', 'W' }, [0x12] = { 'e', 'E' }, [0x13] = { 'r', 'R' }, [0x14] = { 't', 'T' },
    [0x15] = { 'y', 'Y' }, [0x16] = { 'u', 'U' }, [0x17] = { 'i', 'I' }, [0x18] = { 'o', 'O' }, [0x19] = { 'p', 'P' },
    [0x1A] = { '[', '{' }, [0x1B] = { ']', '}' }, [0x1C] = { '\n', '\n' },
    [0x1E] = { 'a', 'A' }, [0x1F] = { 's', 'S' }, [0x20] = { 'd', 'D' }, [0x21] = { 'f', 'F' }, [0x22] = { 'g', 'G' },
    [0x23] = { 'h', 'H' }, [0x24] = { 'j', 'J' }, [0x25] = { 'k', 'K' }, [0x26] = { 'l', 'L' }, [0x27] = { ';', ':' },
    [0x28] = { '\'', '"' }, [0x29] = { '`', '~' }, [0x2B] = { '\\', '|' },
    [0x2C] = { 'z', 'Z' }, [0x2D] = { 'x', 'X' }, [0x2E] = { 'c', 'C' }, [0x2F] = { 'v', 'V' }, [0x30] = { 'b', 'B' },
    [0x31] = { 'n', 'N' }, [0x32] = { 'm', 'M' }, [0x33] = { ',', '<' }, [0x34] = { '.', '>' }, [0x35] = { '/', '?' },
    [0x37] = { '*', '*' }, [0x39] = { ' ', ' ' }, [0x4A] = { '-', '-' }, [0x4E] = { '+', '+' },
};

uint32_t fairy_key_char(uint8_t code, uint8_t mods, bool numlock) {
    bool shift = mods & MOD_SHIFT, caps = mods & MOD_CAPS;
    if (code == KEY_KPENTER) return '\n';
    if (code == KEY_KPSLASH) return '/';
    if (code & KEY_EXT) return 0;
    if (code >= KEY_KP7 && code <= KEY_KPDOT && code != KEY_KPMINUS && code != KEY_KPPLUS) {
        static const char digits[] = { '7', '8', '9', 0, '4', '5', '6', 0, '1', '2', '3', '0', '.' };
        return numlock ? (uint32_t)digits[code - KEY_KP7] : 0;
    }
    if (code >= sizeof g_keymap / sizeof g_keymap[0]) return 0;
    char c = shift ? g_keymap[code].shift : g_keymap[code].base;
    bool letter = g_keymap[code].base >= 'a' && g_keymap[code].base <= 'z';
    if (letter && caps) c = shift ? g_keymap[code].base : g_keymap[code].shift;
    return (uint32_t)(unsigned char)c;
}
