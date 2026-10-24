/* fairyd: the Fairies window server. Takes over /dev/fb0 and /dev/input/events, listens on a UNIX socket, composites windows. */
#include "fairyd.h"
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>

static volatile int g_term;
static void on_term(int sig) { (void)sig; g_term = 1; }

static void handle_key(uint8_t code, int value) {
    bool down = value != 0;
    switch (code) {
    case KEY_LSHIFT: case KEY_RSHIFT: if (down) S.mods |= MOD_SHIFT; else S.mods &= (uint8_t)~MOD_SHIFT; break;
    case KEY_LCTRL: case KEY_RCTRL: if (down) S.mods |= MOD_CTRL; else S.mods &= (uint8_t)~MOD_CTRL; break;
    case KEY_LALT: case KEY_RALT: if (down) S.mods |= MOD_ALT; else S.mods &= (uint8_t)~MOD_ALT; break;
    case KEY_LSUPER: case KEY_RSUPER: if (down) S.mods |= MOD_SUPER; else S.mods &= (uint8_t)~MOD_SUPER; break;
    case KEY_CAPSLOCK: if (value == 1) S.mods ^= MOD_CAPS; break;
    case KEY_NUMLOCK: if (value == 1) S.numlock = !S.numlock; break;
    default: break;
    }
    if (wm_key_shortcut(code, (uint8_t)value)) return;
    uint32_t ch = value ? fairy_key_char(code, S.mods, S.numlock) : 0;
    send_key_to_focus(code, S.mods, (uint8_t)value, ch);
}

static void process_input(void) {
    struct ginput_event evs[64];
    ssize_t n = read(S.input_fd, evs, sizeof evs);
    if (n <= 0) return;
    int count = (int)(n / (ssize_t)sizeof evs[0]);
    int dx = 0, dy = 0;
    for (int i = 0; i < count; i++) {
        struct ginput_event *e = &evs[i];
        bool flush_motion = e->type != EV_REL || e->code == REL_WHEEL;
        if (flush_motion && (dx || dy)) {
            cursor_move_by(dx, dy);
            wm_pointer_event(S.cursor_x, S.cursor_y, S.buttons, 0, false, 0);
            dx = dy = 0;
        }
        if (e->type == EV_REL) {
            if (e->code == REL_X) dx += e->value;
            else if (e->code == REL_Y) dy += e->value;
            else if (e->code == REL_WHEEL) wm_pointer_event(S.cursor_x, S.cursor_y, S.buttons, 0, false, e->value);
        } else if (e->type == EV_KEY) {
            if (e->code == BTN_LEFT || e->code == BTN_RIGHT || e->code == BTN_MIDDLE) {
                int idx = e->code == BTN_LEFT ? 1 : e->code == BTN_RIGHT ? 2 : 3;
                uint8_t bit = (uint8_t)(1 << (idx - 1));
                uint8_t now = e->value ? (S.buttons | bit) : (S.buttons & (uint8_t)~bit);
                wm_pointer_event(S.cursor_x, S.cursor_y, now, idx, e->value != 0, 0);
            } else if (e->code < 256) {
                handle_key((uint8_t)e->code, e->value);
            }
        }
    }
    if (dx || dy) {
        cursor_move_by(dx, dy);
        wm_pointer_event(S.cursor_x, S.cursor_y, S.buttons, 0, false, 0);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    int fb = open("/dev/fb0", O_RDWR);
    if (fb < 0) { fprintf(stderr, "fairyd: cannot open /dev/fb0: %s\n", strerror(errno)); return 1; }
    struct gfbinfo info;
    if (ioctl(fb, FBIOGET_INFO, &info) < 0) { fprintf(stderr, "fairyd: FBIOGET_INFO failed\n"); return 1; }
    void *map = mmap(NULL, (size_t)info.pitch * info.height, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (map == MAP_FAILED) { fprintf(stderr, "fairyd: cannot map the framebuffer: %s\n", strerror(errno)); return 1; }
    S.fb_fd = fb;

    S.input_fd = open("/dev/input/events", O_RDONLY | O_NONBLOCK);
    if (S.input_fd < 0) { fprintf(stderr, "fairyd: cannot open /dev/input/events: %s\n", strerror(errno)); return 1; }
    int one = 1;
    ioctl(S.input_fd, INPUT_GRAB, &one);
    ioctl(fb, FBIO_GRAPHICS, &one);

    server_init();
    comp_init((int)info.width, (int)info.height, map, (int)info.pitch, (info.flags & FBINFO_SWAP_RB) != 0);
    S.work_x = S.work_y = 0;
    S.work_w = S.screen_w;
    S.work_h = S.screen_h;
    S.numlock = true;
    if (S.scale > 1)
        fprintf(stderr, "fairyd: %dx%d, the fairies are holding the pixels %d by %d (ui scale %d: clients see %dx%d)\n", S.phys_w, S.phys_h,
                S.scale, S.scale, S.scale, S.screen_w, S.screen_h);
    else fprintf(stderr, "fairyd: %dx%d, the fairies are holding the pixels\n", S.screen_w, S.screen_h);

    while (!S.quit && !g_term) {
        struct pollfd pf[2 + 64];
        Client *cl[64];
        int n = 0;
        pf[n].fd = S.input_fd; pf[n].events = POLLIN; n++;
        pf[n].fd = S.listen_fd; pf[n].events = POLLIN; n++;
        for (Client *c = S.clients; c && n < 66; c = c->next) { pf[n].fd = c->fd; pf[n].events = POLLIN; cl[n - 2] = c; n++; }
        for (int i = 0; i < n; i++) pf[i].revents = 0;
        int timeout = S.dirty.w > 0 ? 2 : 100;
        poll(pf, (nfds_t)n, timeout);
        S.now_ms = k_now_ms();

        if (pf[0].revents & POLLIN) process_input();
        if (pf[1].revents & POLLIN) for (int i = 0; i < 8; i++) server_accept();
        for (int i = 2; i < n; i++) if (pf[i].revents & (POLLIN | POLLHUP | POLLERR)) server_read_client(cl[i - 2]);
        for (Client *c = S.clients; c;) {                       /* drop clients that went away */
            Client *next = c->next;
            if (c->dead) server_drop_client(c);
            c = next;
        }
        server_flush_list();
        comp_flush();
        while (waitpid(-1, NULL, WNOHANG) > 0) { }              /* screenshot encoders that have finished */
    }

    ioctl(S.input_fd, INPUT_GRAB, &(int){ 0 });
    ioctl(fb, FBIO_GRAPHICS, &(int){ 0 });
    unlink(FAIRY_SOCKET);
    return 0;
}
