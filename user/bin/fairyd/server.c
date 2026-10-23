/* Clients, the wire protocol, window creation and destruction, task-list updates. */
#include "fairyd.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>

Server S;

/* ---------------------------------------------------------------- output */

void send_msg(Client *c, uint16_t type, uint32_t win, uint32_t serial, const void *payload, size_t plen) {
    if (!c || c->dead) return;
    uint8_t buf[FAIRY_MSG_MAX];
    if (sizeof(FairyHdr) + plen > sizeof buf) return;
    FairyHdr h = { (uint32_t)(sizeof h + plen), type, 0, win, serial };
    memcpy(buf, &h, sizeof h);
    if (plen) memcpy(buf + sizeof h, payload, plen);
    size_t off = 0;
    int stalls = 0;
    while (off < h.size) {
        ssize_t n = write(c->fd, buf + off, h.size - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) {
            if (off == 0) return;                           /* the client is not reading: drop this message, keep the stream intact */
            struct pollfd pf = { c->fd, POLLOUT, 0 };       /* half a message went out: we must finish it or lose the client */
            poll(&pf, 1, 20);
            if (++stalls > 5) { c->dead = true; return; }
            continue;
        }
        c->dead = true;
        return;
    }
}

void server_send_hotkey(uint32_t which) {
    FeHotkey k = { which };
    for (Client *c = S.clients; c; c = c->next) if (c->is_shell) send_msg(c, FE_HOTKEY, 0, 0, &k, sizeof k);
}

bool server_have_shell(void) {
    for (Client *c = S.clients; c; c = c->next) if (c->is_shell && !c->dead) return true;
    return false;
}

void server_mark_list_dirty(void) { S.list_dirty = true; }

static bool listed(const Win *w) { return !(w->flags & (FW_SKIP_TASKS | FW_PANEL | FW_DESKTOP | FW_POPUP)); }

void server_send_window_list(void) {
    FeWindows list;
    memset(&list, 0, sizeof list);
    /* by window id, so task lists keep a stable order */
    Win *sorted[MAX_WINS];
    int n = 0;
    for (int i = 0; i < S.nwins; i++) if (listed(S.wins[i])) sorted[n++] = S.wins[i];
    for (int a = 1; a < n; a++) for (int b = a; b > 0 && sorted[b]->id < sorted[b - 1]->id; b--) { Win *t = sorted[b]; sorted[b] = sorted[b - 1]; sorted[b - 1] = t; }
    for (int i = 0; i < n && list.count < FAIRY_MAX_LIST; i++) {
        Win *w = sorted[i];
        FeWinInfo *e = &list.win[list.count++];
        e->id = w->id;
        e->flags = w->flags;
        e->state = w->visible ? w->state : FS_MINIMIZED;
        e->focused = S.focus == w;
        e->pid = w->client->pid;
        Rect o = win_outer(w);
        e->x = o.x; e->y = o.y; e->w = o.w; e->h = o.h;
        strncpy(e->title, w->title, sizeof e->title - 1);
    }
    size_t len = sizeof list.count + (size_t)list.count * sizeof(FeWinInfo);
    for (Client *c = S.clients; c; c = c->next) if (c->subscribed) send_msg(c, FE_WINDOWS, 0, 0, &list, len);
}

void server_flush_list(void) {
    if (!S.list_dirty) return;
    S.list_dirty = false;
    server_send_window_list();
}

/* ---------------------------------------------------------------- windows */

static int map_buffer(Win *w, const char *path, int bw, int bh) {
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || (size_t)st.st_size < (size_t)bw * (size_t)bh * 4 || bw < 1 || bh < 1 || bw > 8192 || bh > 8192) { close(fd); return -1; }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return -1;
    if (w->buf) munmap(w->buf, w->map_len);
    w->buf = m;
    w->map_len = (size_t)st.st_size;
    w->bw = bw; w->bh = bh; w->bstride = bw;
    return 0;
}

void win_destroy(Win *w) {
    int idx = -1;
    for (int i = 0; i < S.nwins; i++) if (S.wins[i] == w) idx = i;
    if (idx < 0) return;
    comp_damage_win(w);
    memmove(&S.wins[idx], &S.wins[idx + 1], (size_t)(S.nwins - idx - 1) * sizeof(Win *));
    S.nwins--;
    bool had_focus = S.focus == w;
    if (S.lock == w) S.lock = NULL;
    if (S.focus == w) S.focus = NULL;
    if (S.hover == w) S.hover = NULL;
    if (S.grab == w) S.grab = NULL;
    if (S.drag_win == w) { S.drag_win = NULL; S.drag_mode = 0; }
    if (S.last_title_win == w) S.last_title_win = NULL;
    if (w->buf) munmap(w->buf, w->map_len);
    if (w->shm_path[0]) unlink(w->shm_path);
    bool was_panel = (w->flags & FW_PANEL) != 0;
    free(w);
    if (was_panel) work_area_recompute();
    if (had_focus) wm_focus_fallback();
    server_mark_list_dirty();
}

static void destroy_client_windows(Client *c) {
    for (int i = S.nwins - 1; i >= 0; i--)
        if (S.wins[i]->client == c) win_destroy(S.wins[i]);
}

void server_drop_client(Client *c) {
    destroy_client_windows(c);
    for (Client **pp = &S.clients; *pp; pp = &(*pp)->next)
        if (*pp == c) { *pp = c->next; break; }
    close(c->fd);
    free(c);
    server_mark_list_dirty();
}

static void handle_create(Client *c, const FairyHdr *h, const FmCreate *m) {
    if (S.nwins >= MAX_WINS) {
        FeError e = { 1, "too many windows" };
        send_msg(c, FE_ERROR, 0, h->serial, &e, sizeof e);
        return;
    }
    if ((m->flags & FW_LOCK) && S.lock) {                                  /* one lock at a time */
        FeError e = { 3, "the screen is already locked" };
        send_msg(c, FE_ERROR, 0, h->serial, &e, sizeof e);
        return;
    }
    Win *w = calloc(1, sizeof *w);
    if (!w) return;
    w->id = S.next_win++;
    w->client = c;
    w->flags = m->flags;
    if (w->flags & FW_LOCK) w->flags |= FW_FULLSCREEN | FW_NO_DECOR | FW_SKIP_TASKS;
    strncpy(w->title, m->title, sizeof w->title - 1);
    w->cw = m->w; w->ch = m->h;
    w->parent = m->parent;
    w->visible = true;
    w->cursor = FC_ARROW;
    int maxw = S.screen_w, maxh = S.screen_h;
    if (win_has_decor(w)) { maxw -= 2 * BORDER; maxh -= DECOR_T + BORDER; }
    if (w->flags & (FW_FULLSCREEN | FW_DESKTOP)) { w->cw = S.screen_w; w->ch = S.screen_h; }
    if (w->cw > maxw) w->cw = maxw;
    if (w->ch > maxh) w->ch = maxh;
    if (w->cw < 1) w->cw = 1;
    if (w->ch < 1) w->ch = 1;
    char path[64];
    strncpy(path, m->shm, sizeof path - 1);
    path[sizeof path - 1] = 0;
    if (strncmp(path, FAIRY_SHM_DIR "/", strlen(FAIRY_SHM_DIR) + 1) || strstr(path, "..") || map_buffer(w, path, m->w, m->h) < 0) {
        FeError e = { 2, "cannot map the window buffer" };
        send_msg(c, FE_ERROR, 0, h->serial, &e, sizeof e);
        free(w);
        return;
    }
    strncpy(w->shm_path, path, sizeof w->shm_path - 1);
    wm_place_new(w, m->x, m->y);
    win_insert(w);
    if (w->flags & FW_LOCK) S.lock = w;
    if (w->flags & FW_PANEL) work_area_recompute();
    Rect o = win_outer(w);
    FeCreated cr = { w->cw, w->ch, o.x, o.y, o.w, o.h, 0, 0, 0, 0 };
    if (win_has_decor(w)) { cr.decor_l = cr.decor_r = cr.decor_b = BORDER; cr.decor_t = DECOR_T; }
    send_msg(c, FE_CREATED, w->id, h->serial, &cr, sizeof cr);
    comp_damage_win(w);
    if (!(w->flags & (FW_NO_FOCUS | FW_POPUP | FW_PANEL))) { win_raise(w); win_focus(w); }
    server_mark_list_dirty();
}

/* ---------------------------------------------------------------- protocol */

static void handle_msg(Client *c, const FairyHdr *h, const uint8_t *p) {
    size_t plen = h->size - sizeof *h;
    Win *w = h->win ? win_by_id(h->win) : NULL;
    if (w && w->client != c && h->type != FM_ACTIVATE && h->type != FM_REQUEST_CLOSE && !(c->is_shell && h->type == FM_SET_STATE)) w = NULL;   /* only the owner (or the shell) may touch a window */
    switch (h->type) {
    case FM_HELLO: {
        if (plen < sizeof(FmHello)) break;
        const FmHello *m = (const FmHello *)p;
        c->pid = m->pid;
        strncpy(c->name, m->name, sizeof c->name - 1);
        FeWelcome wl = { FAIRY_VERSION, c->id, S.screen_w, S.screen_h, S.work_x, S.work_y, S.work_w, S.work_h };
        send_msg(c, FE_WELCOME, 0, h->serial, &wl, sizeof wl);
        break;
    }
    case FM_CREATE:
        if (plen >= sizeof(FmCreate)) handle_create(c, h, (const FmCreate *)p);
        break;
    case FM_DESTROY:
        if (w) win_destroy(w);
        break;
    case FM_ATTACH: {
        if (!w || plen < sizeof(FmAttach)) break;
        const FmAttach *m = (const FmAttach *)p;
        char path[64];
        strncpy(path, m->shm, sizeof path - 1);
        path[sizeof path - 1] = 0;
        if (strncmp(path, FAIRY_SHM_DIR "/", strlen(FAIRY_SHM_DIR) + 1) || strstr(path, "..")) break;
        char old[64];
        strncpy(old, w->shm_path, sizeof old);
        if (map_buffer(w, path, m->w, m->h) == 0) {
            if (strcmp(old, path)) { unlink(old); strncpy(w->shm_path, path, sizeof w->shm_path - 1); }
            comp_damage(win_content(w));
        }
        break;
    }
    case FM_COMMIT: {
        if (!w || !w->visible || plen < sizeof(FmCommit)) break;
        const FmCommit *m = (const FmCommit *)p;
        Rect c0 = win_content(w);
        Rect d = { c0.x + m->x, c0.y + m->y, m->w, m->h };
        comp_damage(rect_intersect(d, c0));
        break;
    }
    case FM_SET_TITLE:
        if (w && plen >= sizeof(FmTitle)) {
            strncpy(w->title, ((const FmTitle *)p)->title, sizeof w->title - 1);
            Rect o = win_outer(w);
            comp_damage((Rect){ o.x, o.y, o.w, DECOR_T });
            server_mark_list_dirty();
        }
        break;
    case FM_SET_STATE:
        if (w && plen >= sizeof(FmState)) win_set_state(w, ((const FmState *)p)->state);
        break;
    case FM_SET_CURSOR:
        if (w && plen >= sizeof(FmCursor)) {
            w->cursor = ((const FmCursor *)p)->shape;
            if (S.hover == w || S.grab == w) cursor_set_shape(w->cursor);
        }
        break;
    case FM_MOVE:
        if (w && plen >= sizeof(FmMove)) {
            const FmMove *m = (const FmMove *)p;
            win_set_geometry(w, m->x, m->y, m->w > 0 ? m->w : w->cw, m->h > 0 ? m->h : w->ch);
            if (w->flags & FW_PANEL) work_area_recompute();
        }
        break;
    case FM_RAISE:
        if (w) win_raise(w);
        break;
    case FM_ACTIVATE: {
        Win *t = win_by_id(h->win);
        if (t) {
            if (!t->visible) win_set_state(t, FS_NORMAL);
            else { win_raise(t); win_focus(t); }
        }
        break;
    }
    case FM_REQUEST_CLOSE: {
        Win *t = win_by_id(h->win);
        if (t) wm_request_close(t);
        break;
    }
    case FM_SUBSCRIBE:
        c->subscribed = true;
        S.list_dirty = true;
        break;
    case FM_REGISTER_SHELL:
        c->is_shell = true;
        break;
    case FM_SET_STRUT:
        if (w && plen >= sizeof(FmStrut)) {
            const FmStrut *m = (const FmStrut *)p;
            w->strut_edge = (int)m->edge;
            w->strut_size = m->size;
            work_area_recompute();
        }
        break;
    case FM_SET_MINSIZE:
        if (w && plen >= sizeof(FmMinSize)) { w->minw = ((const FmMinSize *)p)->w; w->minh = ((const FmMinSize *)p)->h; }
        break;
    case FM_CLIPBOARD_SET:
        if (plen >= sizeof(uint32_t)) {
            uint32_t n = ((const FmClipboard *)p)->len;
            if (n > sizeof S.clip - 1) n = sizeof S.clip - 1;
            if (n > plen - sizeof(uint32_t)) n = (uint32_t)(plen - sizeof(uint32_t));
            memcpy(S.clip, ((const FmClipboard *)p)->data, n);
            S.clip[n] = 0;
            S.cliplen = n;
        }
        break;
    case FM_CLIPBOARD_GET: {
        FeClipboard cb;
        cb.len = (uint32_t)S.cliplen;
        memcpy(cb.data, S.clip, S.cliplen);
        send_msg(c, FE_CLIPBOARD, 0, h->serial, &cb, sizeof cb.len + S.cliplen);
        break;
    }
    case FM_SCREENSHOT:
        if (plen >= sizeof(FmPath)) {
            char path[96];
            strncpy(path, ((const FmPath *)p)->path, sizeof path - 1);
            path[sizeof path - 1] = 0;
            comp_screenshot(path);
        }
        break;
    case FM_WARP_POINTER:
        if (plen >= sizeof(FmWarp)) cursor_move_to(((const FmWarp *)p)->x, ((const FmWarp *)p)->y);
        break;
    case FM_QUIT_SERVER:
        S.quit = true;
        break;
    default:
        break;
    }
}

void server_read_client(Client *c) {
    ssize_t n = read(c->fd, c->in + c->in_len, sizeof c->in - c->in_len);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) { c->dead = true; return; }
    if (n < 0) return;
    c->in_len += (size_t)n;
    size_t off = 0;
    while (c->in_len - off >= sizeof(FairyHdr)) {
        FairyHdr h;
        memcpy(&h, c->in + off, sizeof h);
        if (h.size < sizeof h || h.size > FAIRY_MSG_MAX) { c->dead = true; return; }
        if (c->in_len - off < h.size) break;
        handle_msg(c, &h, c->in + off + sizeof h);
        off += h.size;
        if (c->dead) return;
    }
    if (off) {
        memmove(c->in, c->in + off, c->in_len - off);
        c->in_len -= off;
    }
}

void server_accept(void) {
    int fd = accept(S.listen_fd, NULL, NULL);
    if (fd < 0) return;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    Client *c = calloc(1, sizeof *c);
    if (!c) { close(fd); return; }
    c->fd = fd;
    c->id = ++S.next_client;
    c->next = S.clients;
    S.clients = c;
}

void server_init(void) {
    S.next_win = 1;
    umask(0);                                               /* the login screen runs as root, the desktop as a resident: both must reach the directory */
    mkdir(FAIRY_SHM_DIR, 0777);
    DIR *d = opendir(FAIRY_SHM_DIR);                        /* clean up after a crashed server */
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char path[128];
            snprintf(path, sizeof path, "%s/%s", FAIRY_SHM_DIR, e->d_name);
            unlink(path);
        }
        closedir(d);
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, FAIRY_SOCKET);
    unlink(FAIRY_SOCKET);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 16) < 0) {
        fprintf(stderr, "fairyd: cannot listen on %s: %s\n", FAIRY_SOCKET, strerror(errno));
        exit(1);
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    S.listen_fd = fd;
}
