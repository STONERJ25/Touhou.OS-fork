/*
 * The frame service (framepub.h): a copy of the composed screen for one root subscriber, the VNC server.
 *
 * While nobody has a request outstanding the service costs a few rectangle merges per frame (the damage that comp_damage() hears
 * about, in physical pixels). When a request is pending, framepub_flush() at the end of the main loop's round copies the damaged
 * rectangles of the back buffer into the shared memory and answers with FE_FRAME; the window server never waits for the
 * subscriber, and a subscriber that does not ask gets nothing copied.
 *
 * Move hints: a window whose position changed since the last published frame, while nothing else about it did (size, state,
 * focus, title, flags, its pixels: the signature below), and which was unobscured then and is unobscured now (no visible window or
 * drop shadow above it, no Alt+Tab box), shows exactly the old pixels at the new place. The compositor's drawing of a window does
 * not depend on where it is (frame, title gradient and contents are all drawn relative to the window), and the pointer is not in
 * the back buffer. The subscriber compares the pixels before it trusts a hint anyway.
 */
#include "fairyd.h"
#include "framepub.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>

static struct {
    Client *sub;
    uint8_t *shm;
    size_t size;
    char path[64];
    int max_w, max_h;
    bool pending, now, full, resized;
    Rect dmg[FRAMEPUB_MAX_RECTS];               /* physical pixels */
    int ndmg;
    uint32_t seq, cursor_serial, clip_serial;
    int ptr_x, ptr_y;
    int cur_w, cur_h, cur_hotx, cur_hoty;
    bool sent_once;
    int counter;
} P;

bool peer_is_root(const Client *c) {
    struct gpeercred cr;
    return getpeercred(c->fd, &cr) == 0 && cr.uid == 0;
}

static void refuse(Client *c, uint32_t serial, int code, const char *text) {
    FeError e;
    memset(&e, 0, sizeof e);
    e.code = code;
    snprintf(e.text, sizeof e.text, "%s", text);
    send_msg(c, FE_ERROR, 0, serial, &e, sizeof e);
}

static void drop_shm(void) {
    if (P.shm) munmap(P.shm, P.size);
    P.shm = NULL;
    if (P.path[0]) unlink(P.path);
    P.path[0] = 0;
}

static uint32_t win_sig(const Win *w) {
    uint32_t h = 2166136261u;
#define MIX(v) do { h ^= (uint32_t)(v); h *= 16777619u; } while (0)
    MIX(w->cw); MIX(w->ch); MIX(w->state); MIX(w->flags); MIX(S.focus == w); MIX(w->bw); MIX(w->bh); MIX(w->bscale);
    MIX((uintptr_t)w->buf); MIX((uintptr_t)w->buf >> 32); MIX(w->commits); MIX(w->visible);
    for (const char *t = w->title; *t; t++) MIX((unsigned char)*t);
#undef MIX
    return h;
}

/* Nothing that is drawn above window number idx (windows, their drop shadows, the Alt+Tab box) covers its outer rectangle. */
static bool unobscured(int idx) {
    const Win *w = S.wins[idx];
    Rect r = win_outer(w);
    for (int j = idx + 1; j < S.nwins; j++) {
        const Win *v = S.wins[j];
        if (!v->visible) continue;
        Rect o = win_outer(v);
        if (win_has_decor(v) && v->state != FS_MAXIMIZED) { o.w += 6; o.h += 6; }
        if (!rect_empty(rect_intersect(o, r))) return false;
    }
    if (S.switching) {
        Rect box = { (S.screen_w - 360) / 2 - 4, (S.screen_h - (40 + S.switch_n * 26)) / 2 - 4, 376, 54 + S.switch_n * 26 };
        if (!rect_empty(rect_intersect(box, r))) return false;
    }
    return true;
}

static int find_moves(FrameMove *out) {
    int n = 0;
    Rect screen = { 0, 0, S.phys_w, S.phys_h };
    for (int i = 0; i < S.nwins; i++) {
        Win *w = S.wins[i];
        if (!w->visible || !w->pub_known || !w->pub_clear || (w->x == w->pub_x && w->y == w->pub_y)) continue;
        if (win_sig(w) != w->pub_sig || !unobscured(i)) continue;
        Rect o = win_outer(w);
        int s = S.scale, dx = (w->x - w->pub_x) * s, dy = (w->y - w->pub_y) * s;
        Rect d = rect_intersect((Rect){ o.x * s, o.y * s, o.w * s, o.h * s }, screen);
        d = rect_intersect(d, (Rect){ dx, dy, S.phys_w, S.phys_h });     /* where the old place is on the screen too */
        if (rect_empty(d)) continue;
        FrameMove m = { d.x, d.y, d.w, d.h, dx, dy };
        if (n < FRAMEPUB_MAX_MOVES) out[n++] = m;
        else {                                                          /* keep the largest four */
            int k = 0;
            for (int j = 1; j < n; j++) if ((long)out[j].w * out[j].h < (long)out[k].w * out[k].h) k = j;
            if ((long)m.w * m.h > (long)out[k].w * out[k].h) out[k] = m;
        }
    }
    return n;
}

static void note_windows(void) {
    for (int i = 0; i < S.nwins; i++) {
        Win *w = S.wins[i];
        w->pub_known = true;
        w->pub_x = w->x;
        w->pub_y = w->y;
        w->pub_sig = win_sig(w);
        w->pub_clear = w->visible && unobscured(i);
    }
}

void framepub_subscribe(Client *c, uint32_t serial, const void *payload, size_t plen) {
    if (plen < sizeof(FmFrameSubscribe) || ((const FmFrameSubscribe *)payload)->version != FRAMEPUB_VERSION) { refuse(c, serial, 5, "frame service version mismatch"); return; }
    if (!peer_is_root(c)) { refuse(c, serial, 4, "the frame service is for root only"); return; }
    if (P.sub == c) return;
    if (P.sub) { refuse(c, serial, 6, "the frame service has a subscriber already"); return; }
    P.max_w = S.fb_w;
    P.max_h = S.fb_h;
    size_t size = FRAMEPUB_PIXELS_OFF + (size_t)P.max_w * P.max_h * 4 + (size_t)FRAMEPUB_CURSOR_MAX * FRAMEPUB_CURSOR_MAX * 4;
    snprintf(P.path, sizeof P.path, "%s/frame-%d-%d.buf", FAIRY_SHM_DIR, getpid(), ++P.counter);
    int fd = open(P.path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);   /* a name somebody prepared (a link) is not used */
    if (fd < 0) { P.path[0] = 0; refuse(c, serial, 7, "cannot make the frame file"); return; }
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); unlink(P.path); P.path[0] = 0; refuse(c, serial, 7, "no room for the frame file"); return; }
    void *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { unlink(P.path); P.path[0] = 0; refuse(c, serial, 7, "cannot map the frame file"); return; }
    P.shm = m;
    P.size = size;
    FramePubShm *h = m;
    h->magic = FRAMEPUB_MAGIC;
    h->version = FRAMEPUB_VERSION;
    h->pixels_off = FRAMEPUB_PIXELS_OFF;
    h->stride = (uint32_t)P.max_w;
    h->max_w = P.max_w;
    h->max_h = P.max_h;
    h->cursor_off = FRAMEPUB_PIXELS_OFF + (uint32_t)P.max_w * (uint32_t)P.max_h * 4;
    h->cursor_stride = FRAMEPUB_CURSOR_MAX;
    P.sub = c;
    P.pending = P.now = P.resized = false;
    P.full = true;
    P.ndmg = 0;
    P.sent_once = false;
    for (int i = 0; i < S.nwins; i++) S.wins[i]->pub_known = false;
    FeFrameReady r;
    memset(&r, 0, sizeof r);
    r.version = FRAMEPUB_VERSION;
    snprintf(r.shm, sizeof r.shm, "%s", P.path);
    r.shm_size = (uint32_t)size;
    r.max_w = P.max_w;
    r.max_h = P.max_h;
    send_msg(c, FE_FRAME_READY, 0, serial, &r, sizeof r);
    fprintf(stderr, "fairyd: the frame service has a subscriber (%s, pid %d): a %dx%d copy in %s\n", c->name, c->pid, P.max_w, P.max_h, P.path);
}

void framepub_request(Client *c, const void *payload, size_t plen) {
    if (c != P.sub || plen < sizeof(FmFrameRequest)) return;
    uint32_t f = ((const FmFrameRequest *)payload)->flags;
    if (f & FRAMEREQ_FULL) P.full = true;
    if (f & FRAMEREQ_NOW) P.now = true;
    P.pending = true;
}

void framepub_damage(Rect r) {
    if (!P.sub) return;
    int s = S.scale;
    Rect p = rect_intersect((Rect){ r.x * s, r.y * s, r.w * s, r.h * s }, (Rect){ 0, 0, S.phys_w, S.phys_h });
    if (rect_empty(p)) return;
    damage_list_add(P.dmg, &P.ndmg, FRAMEPUB_MAX_RECTS, p);
}

void framepub_resized(void) {
    if (P.sub) P.resized = true;
}

void framepub_client_gone(Client *c) {
    if (c != P.sub) return;
    P.sub = NULL;
    drop_shm();
    fprintf(stderr, "fairyd: the frame service's subscriber went away\n");
}

void framepub_flush(void) {
    if (!P.sub || !P.pending || !P.shm || P.sub->dead) return;
    bool ptr = S.ptr_x != P.ptr_x || S.ptr_y != P.ptr_y;
    bool shape = S.cursor_serial != P.cursor_serial || !P.sent_once;
    bool clip = S.clip_serial != P.clip_serial;
    if (!(P.full || P.resized || P.now || P.ndmg || ptr || shape || clip)) return;
    uint8_t buf[FAIRY_MSG_MAX - sizeof(FairyHdr)];
    FeFrame *m = (FeFrame *)buf;
    memset(m, 0, sizeof *m);
    FrameRect *rects = (FrameRect *)(buf + sizeof *m);
    FrameMove moves[FRAMEPUB_MAX_MOVES];
    uint32_t *px = (uint32_t *)(P.shm + FRAMEPUB_PIXELS_OFF);
    int nr = 0, nm = 0;
    if (P.full || P.resized) {
        for (int y = 0; y < S.phys_h; y++) memcpy(px + (size_t)y * P.max_w, S.back + (size_t)y * S.phys_w, (size_t)S.phys_w * 4);
        m->flags |= FRAME_F_FULL | (P.resized ? FRAME_F_RESIZED : 0);
    } else {
        nm = find_moves(moves);
        Rect screen = { 0, 0, S.phys_w, S.phys_h };
        for (int i = 0; i < P.ndmg; i++) {
            Rect r = rect_intersect(P.dmg[i], screen);
            if (rect_empty(r)) continue;
            for (int y = r.y; y < r.y + r.h; y++)
                memcpy(px + (size_t)y * P.max_w + r.x, S.back + (size_t)y * S.phys_w + r.x, (size_t)r.w * 4);
            rects[nr++] = (FrameRect){ r.x, r.y, r.w, r.h };
        }
    }
    if (shape) {
        uint32_t *cur = (uint32_t *)(P.shm + FRAMEPUB_PIXELS_OFF + (size_t)P.max_w * P.max_h * 4);
        comp_cursor_sprite(cur, FRAMEPUB_CURSOR_MAX, FRAMEPUB_CURSOR_MAX, &P.cur_w, &P.cur_h, &P.cur_hotx, &P.cur_hoty);
        m->flags |= FRAME_F_CURSOR;
    }
    m->seq = ++P.seq;
    m->w = S.phys_w;
    m->h = S.phys_h;
    m->scale = S.scale;
    m->ptr_x = S.ptr_x;
    m->ptr_y = S.ptr_y;
    m->cur_w = P.cur_w;
    m->cur_h = P.cur_h;
    m->cur_hotx = P.cur_hotx;
    m->cur_hoty = P.cur_hoty;
    m->cursor_serial = S.cursor_serial;
    m->mods = S.mods;
    m->numlock = S.numlock;
    m->clip_serial = S.clip_serial;
    m->nrects = (uint16_t)nr;
    m->nmoves = (uint16_t)nm;
    memcpy(rects + nr, moves, (size_t)nm * sizeof moves[0]);
    send_msg(P.sub, FE_FRAME, 0, 0, buf, sizeof *m + (size_t)nr * sizeof(FrameRect) + (size_t)nm * sizeof(FrameMove));
    P.pending = P.now = P.full = P.resized = false;
    P.ndmg = 0;
    P.ptr_x = S.ptr_x;
    P.ptr_y = S.ptr_y;
    P.cursor_serial = S.cursor_serial;
    P.clip_serial = S.clip_serial;
    P.sent_once = true;
    note_windows();
}
