/*
 * The picture: vncd is a client of the window server and uses its frame service (../fairyd/framepub.h). fairyd copies what
 * changed into a shared-memory file only when asked and answers with the damaged rectangles; vncd copies those into its own
 * frame (g_frame) and tells every viewer what changed. Windows that only moved come as hints that the moved pixels are the old
 * ones; they become CopyRect, but only after vncd has compared the pixels itself.
 *
 * vncd asks for the next frame only while a viewer is waiting for one, and not more often than every MIN_FRAME_MS, so with nobody
 * watching the window server does nothing but note damage. When the window server goes away (the desktop session ended, the
 * login screen comes) the viewers stay connected and see the last frame; vncd connects again every second, and the new window
 * server's first frame replaces everything (DesktopSize when the size differs).
 *
 * Everything that comes from the socket or the shared memory is checked: the window server is trusted with the picture, not with
 * the memory of a root process (a local user could start a program of their own under the socket's name while no window server
 * runs). The shared-memory file must be in the socket's directory, named frame-*.buf, a plain file of the peer's user.
 */
#include "vncd.h"
#include "../fairyd/framepub.h"
#include <fairy.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#define MIN_FRAME_MS   15
#define RECONNECT_MS   1000
#define MODE_WAIT_MS   3000
#define NOW_WAIT_MS    5000

static struct {
    int fd;
    int uid;                                    /* the window server's user */
    uint8_t in[FAIRY_MSG_MAX * 2];
    size_t in_len;
    uint8_t *shm;
    size_t shm_size;
    const FramePubShm *hdr;
    int max_w, max_h;
    bool subscribed;
    bool requested;                             /* a FM_FRAME_REQUEST is outstanding */
    bool want;                                  /* somebody waits for a frame */
    bool need_full;
    uint32_t sent, received;                    /* requests sent, frames received: they answer each other in order */
    uint32_t fresh_wanted;                      /* a viewer waits for the answer to request number fresh_wanted (FRAMEREQ_NOW) */
    bool outstanding_now;                       /* the outstanding request has FRAMEREQ_NOW */
    long now_since;                             /* since when (it is answered within a round of the window server's loop) */
    long last_frame_ms, next_try_ms;
    uint32_t seq, clip_serial;
    bool clip_known;
    bool said_waiting;
    long mode_deadline;
    Client *mode_requester;
} S = { .fd = -1 };

Frame g_frame;
bool g_have_desktop;

static void send_msg(uint16_t type, const void *payload, size_t plen) {
    if (S.fd < 0) return;
    uint8_t buf[FAIRY_MSG_MAX];
    FairyHdr h = { (uint32_t)(sizeof h + plen), type, 0, 0, 0 };
    memcpy(buf, &h, sizeof h);
    if (plen) memcpy(buf + sizeof h, payload, plen);
    size_t off = 0;
    int spins = 0;
    while (off < h.size) {                          /* small messages on a socket the server reads all the time */
        ssize_t n = write(S.fd, buf + off, h.size - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && (errno == EINTR || (errno == EAGAIN && ++spins < 1000))) { if (errno == EAGAIN) usleep(1000); continue; }
        vlog("the window server does not take messages (%s): starting over", n < 0 ? strerror(errno) : "closed");
        close(S.fd);
        S.fd = -1;
        return;
    }
}

/* The frame vncd keeps: (re)allocated for a new size, black where nothing has come yet. */
static bool frame_resize(int w, int h) {
    if (w == g_frame.w && h == g_frame.h && g_frame.px) return true;
    uint32_t *px = calloc((size_t)w * (size_t)h, 4);
    if (!px) { vlog("out of memory for a %dx%d frame", w, h); return false; }
    free(g_frame.px);
    g_frame.px = px;
    g_frame.w = w;
    g_frame.h = h;
    g_frame.stride = w;
    return true;
}

static void drop_connection(const char *why) {
    if (S.fd >= 0) close(S.fd);
    S.fd = -1;
    if (S.shm) munmap(S.shm, S.shm_size);
    S.shm = NULL;
    S.hdr = NULL;
    S.subscribed = S.requested = false;
    S.sent = S.received;                                        /* the outstanding request will not be answered */
    S.in_len = 0;
    if (g_have_desktop) vlog("the window server went away (%s): viewers keep the last picture until it is back", why);
    g_have_desktop = false;
    S.next_try_ms = mono_ms() + RECONNECT_MS;
    if (S.mode_requester) { clients_resize_answer(S.mode_requester, 2); S.mode_requester = NULL; }
}

void source_init(void) {
    S.fd = -1;
    if (!frame_resize(1024, 768)) exit(1);                      /* until a window server publishes: a black screen of the usual size */
    for (int i = 0; i < 1024 * 768; i++) g_frame.px[i] = 0x0A0F2A;  /* the night blue of the desktop background */
    g_frame.cur = calloc((size_t)FRAMEPUB_CURSOR_MAX * FRAMEPUB_CURSOR_MAX, 4);
    if (!g_frame.cur) exit(1);
    g_frame.cur_w = g_frame.cur_h = 0;
}

int source_fd(void) { return S.fd; }

static void try_connect(long now) {
    S.next_try_ms = now + RECONNECT_MS;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    if (strlen(g_opt.fairy_socket) >= sizeof sa.sun_path) return;
    strcpy(sa.sun_path, g_opt.fairy_socket);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        close(fd);
        if (!S.said_waiting) { vlog("no window server at %s yet: waiting for the desktop", g_opt.fairy_socket); S.said_waiting = true; }
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    S.fd = fd;
    S.uid = peer_uid(fd);
    S.in_len = 0;
    FmHello hello;
    memset(&hello, 0, sizeof hello);
    hello.version = FAIRY_VERSION;
    hello.pid = getpid();
    snprintf(hello.name, sizeof hello.name, "vncd");
    send_msg(FM_HELLO, &hello, sizeof hello);
    FmFrameSubscribe sub = { FRAMEPUB_VERSION };
    send_msg(FM_FRAME_SUBSCRIBE, &sub, sizeof sub);
}

/* The shared memory the window server offers: same directory as the socket, frame-*.buf, a plain file of the server's user. */
static bool map_shm(const FeFrameReady *m) {
    char path[sizeof m->shm + 1];
    memcpy(path, m->shm, sizeof m->shm);
    path[sizeof m->shm] = 0;
    const char *slash = strrchr(g_opt.fairy_socket, '/');
    size_t dirlen = slash ? (size_t)(slash - g_opt.fairy_socket) + 1 : 0;
    const char *base = path + dirlen;
    size_t bl = strlen(base);
    if (strncmp(path, g_opt.fairy_socket, dirlen) || strchr(base, '/') || strncmp(base, "frame-", 6) || bl < 10 || strcmp(base + bl - 4, ".buf")) {
        vlog("the window server named a frame file outside its directory (%.64s): not used", path);
        return false;
    }
    if (m->max_w < 1 || m->max_h < 1 || m->max_w > VNC_MAX_W || m->max_h > VNC_MAX_H) { vlog("the window server offers a %dx%d frame: not used", m->max_w, m->max_h); return false; }
    uint64_t need = (uint64_t)FRAMEPUB_PIXELS_OFF + (uint64_t)m->max_w * m->max_h * 4 + (uint64_t)FRAMEPUB_CURSOR_MAX * FRAMEPUB_CURSOR_MAX * 4;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { vlog("cannot open the frame file %s: %s", path, strerror(errno)); return false; }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || (S.uid >= 0 && (int)st.st_uid != S.uid) || (uint64_t)st.st_size < need || m->shm_size < need ||
        (uint64_t)m->shm_size > (uint64_t)st.st_size) {
        vlog("the frame file %s is not what the window server said (size, owner or kind): not used (mode %o, owner %d but the server is %d, "
             "size %llu, wanted %llu, announced %u)", path, (unsigned)st.st_mode, (int)st.st_uid, S.uid, (unsigned long long)st.st_size,
             (unsigned long long)need, (unsigned)m->shm_size);
        close(fd);
        return false;
    }
    void *p = mmap(NULL, (size_t)need, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { vlog("cannot map the frame file: %s", strerror(errno)); return false; }
    const FramePubShm *h = p;
    if (h->magic != FRAMEPUB_MAGIC || h->version != FRAMEPUB_VERSION || h->pixels_off != FRAMEPUB_PIXELS_OFF || h->stride != (uint32_t)m->max_w ||
        h->max_w != m->max_w || h->max_h != m->max_h || h->cursor_stride != FRAMEPUB_CURSOR_MAX ||
        (uint64_t)h->cursor_off != (uint64_t)FRAMEPUB_PIXELS_OFF + (uint64_t)m->max_w * m->max_h * 4) {
        vlog("the frame file's header does not match: not used");
        munmap(p, (size_t)need);
        return false;
    }
    S.shm = p;
    S.shm_size = (size_t)need;
    S.hdr = h;
    S.max_w = m->max_w;
    S.max_h = m->max_h;
    return true;
}

static const uint32_t *shm_pixels(void) { return (const uint32_t *)(S.shm + FRAMEPUB_PIXELS_OFF); }

/* new[dst] == old[dst - (dx, dy)]? The old pixels are vncd's frame (not yet overwritten), the new ones the shared memory. */
static bool move_holds(VRect dst, int dx, int dy) {
    const uint32_t *nw = shm_pixels();
    for (int y = dst.y; y < dst.y + dst.h; y++)
        if (memcmp(nw + (size_t)y * S.max_w + dst.x, g_frame.px + (size_t)(y - dy) * g_frame.stride + (dst.x - dx), (size_t)dst.w * 4)) return false;
    return true;
}

/* Scrolling: a damaged rectangle whose new rows are its old rows shifted up or down (a terminal, a list). Rows are compared by
 * hash first, then the run found is compared pixel by pixel, so a guess is never sent. Probe rows that occur many times in the old
 * picture (blank lines) are skipped; at most a few shifts are tried. Returns the rows that can be copied and the shift. */
static uint64_t row_hash(const uint32_t *p, int n) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static bool find_scroll(VRect r, VRect *out, int *out_dy) {
    if (r.w < 64 || r.h < 48) return false;
    const uint32_t *nw = shm_pixels();
    uint64_t *ho = malloc((size_t)r.h * 8), *hn = malloc((size_t)r.h * 8);
    if (!ho || !hn) { free(ho); free(hn); return false; }
    for (int i = 0; i < r.h; i++) {
        ho[i] = row_hash(g_frame.px + (size_t)(r.y + i) * g_frame.stride + r.x, r.w);
        hn[i] = row_hash(nw + (size_t)(r.y + i) * S.max_w + r.x, r.w);
    }
    int tried[8], ntried = 0, best_len = 0, best_k0 = 0, best_dy = 0;
    for (int p = 1; p < 16 && ntried < 8; p++) {
        int i = r.h * p / 16;
        int matches = 0, j_found = -1;
        for (int j = 0; j < r.h && matches < 5; j++) if (ho[j] == hn[i]) { matches++; if (j != i) j_found = j; }
        if (matches == 0 || matches >= 5 || j_found < 0) continue;      /* not there, or a row that is everywhere (blank) */
        int dy = i - j_found;
        bool seen = false;
        for (int t = 0; t < ntried; t++) seen |= tried[t] == dy;
        if (seen) continue;
        tried[ntried++] = dy;
        int k0 = i, k1 = i + 1;                                        /* the run of rows around i that moved by dy */
        while (k0 - 1 >= 0 && k0 - 1 - dy >= 0 && k0 - 1 - dy < r.h && hn[k0 - 1] == ho[k0 - 1 - dy]) k0--;
        while (k1 < r.h && k1 - dy >= 0 && k1 - dy < r.h && hn[k1] == ho[k1 - dy]) k1++;
        if (k1 - k0 > best_len) { best_len = k1 - k0; best_k0 = k0; best_dy = dy; }
    }
    free(ho);
    free(hn);
    if (best_len < 32 || best_len < r.h / 4) return false;
    VRect d = { r.x, r.y + best_k0, r.w, best_len };
    if (!move_holds(d, 0, best_dy)) return false;
    *out = d;
    *out_dy = best_dy;
    return true;
}

static bool anybody_copies(void) {
    for (Client *c = clients_first(); c; c = client_next_of(c)) if (client_can_copy(c)) return true;
    return false;
}

static void copy_rect(VRect r) {
    const uint32_t *src = shm_pixels();
    for (int y = r.y; y < r.y + r.h; y++)
        memcpy(g_frame.px + (size_t)y * g_frame.stride + r.x, src + (size_t)y * S.max_w + r.x, (size_t)r.w * 4);
}

static void apply_frame(const FeFrame *m, const FrameRect *rects, const FrameMove *moves) {
    VRect screen = { 0, 0, m->w, m->h };
    bool resized = m->w != g_frame.w || m->h != g_frame.h;
    bool full = (m->flags & (FRAME_F_FULL | FRAME_F_RESIZED)) || resized || S.need_full;
    if (resized && !(m->flags & (FRAME_F_FULL | FRAME_F_RESIZED))) {
        vlog("the window server changed the size without a complete frame: asking for one");
        S.need_full = true;
        return;
    }
    if (resized && !frame_resize(m->w, m->h)) return;
    Region damage;
    region_init(&damage);
    VRect best_move = { 0, 0, 0, 0 };
    int bdx = 0, bdy = 0;
    if (full) {
        copy_rect(screen);
        region_set(&damage, screen);
        S.need_full = false;
    } else {
        for (int i = 0; i < m->nmoves; i++) {                         /* the largest move that really is one */
            VRect d = vrect_intersect((VRect){ moves[i].x, moves[i].y, moves[i].w, moves[i].h }, screen);
            int dx = moves[i].dx, dy = moves[i].dy;
            if (vrect_empty(d) || (!dx && !dy) || dx < -VNC_MAX_W || dx > VNC_MAX_W || dy < -VNC_MAX_H || dy > VNC_MAX_H) continue;
            VRect s = vrect_intersect((VRect){ d.x - dx, d.y - dy, d.w, d.h }, screen);
            if (s.w != d.w || s.h != d.h) continue;                      /* the source must lie on the screen */
            if ((long)d.w * d.h <= (long)best_move.w * best_move.h) continue;
            if (!move_holds(d, dx, dy)) continue;
            best_move = d; bdx = dx; bdy = dy;
        }
        for (int i = 0; i < m->nrects; i++) {
            VRect r = vrect_intersect((VRect){ rects[i].x, rects[i].y, rects[i].w, rects[i].h }, screen);
            if (vrect_empty(r)) continue;
            region_add_rect(&damage, r);
        }
        if (vrect_empty(best_move) && anybody_copies())               /* no window moved: did something scroll? */
            for (int i = 0; i < m->nrects; i++) {                       /* the window server's rectangles (a window's commit), not the
                                                                           bands of the region, which other damage can cut up */
                VRect r = vrect_intersect((VRect){ rects[i].x, rects[i].y, rects[i].w, rects[i].h }, screen), d;
                int dy;
                if ((long)r.w * r.h > (long)best_move.w * best_move.h && find_scroll(r, &d, &dy) &&
                    (long)d.w * d.h > (long)best_move.w * best_move.h) { best_move = d; bdx = 0; bdy = dy; }
            }
        for (int i = 0; i < damage.n; i++) copy_rect(damage.r[i]);
        if (!vrect_empty(best_move)) copy_rect(best_move);            /* (a correct hint lies inside the damage anyway) */
    }
    g_frame.scale = m->scale >= 1 && m->scale <= 8 ? m->scale : 1;
    /* the viewers learn about the pixels first, while g_frame still has the pointer where they last saw it drawn */
    VRect old_cursor = cursor_rect(&g_frame);
    if (!resized) {
        if (S.mode_requester && (m->flags & FRAME_F_RESIZED)) { clients_frame_resized(1, S.mode_requester, 0); S.mode_requester = NULL; }
        else clients_frame_changed(&damage, best_move, bdx, bdy);
    }
    /* the pointer */
    bool shape = m->cursor_serial != g_frame.cursor_serial || (m->flags & FRAME_F_CURSOR) || full;
    int cw = m->cur_w, ch = m->cur_h;
    if (cw < 0 || ch < 0 || cw > FRAMEPUB_CURSOR_MAX || ch > FRAMEPUB_CURSOR_MAX) cw = ch = 0;
    if (shape) {
        const uint32_t *sp = (const uint32_t *)(S.shm + S.hdr->cursor_off);
        for (int y = 0; y < ch; y++) memcpy(g_frame.cur + (size_t)y * cw, sp + (size_t)y * FRAMEPUB_CURSOR_MAX, (size_t)cw * 4);
        g_frame.cur_w = cw;
        g_frame.cur_h = ch;
        g_frame.cur_hotx = m->cur_hotx < 0 || m->cur_hotx >= (cw ? cw : 1) ? 0 : m->cur_hotx;
        g_frame.cur_hoty = m->cur_hoty < 0 || m->cur_hoty >= (ch ? ch : 1) ? 0 : m->cur_hoty;
        g_frame.cursor_serial = m->cursor_serial;
    }
    bool moved = m->ptr_x != g_frame.ptr_x || m->ptr_y != g_frame.ptr_y;
    g_frame.ptr_x = m->ptr_x < 0 ? 0 : m->ptr_x >= m->w ? m->w - 1 : m->ptr_x;
    g_frame.ptr_y = m->ptr_y < 0 ? 0 : m->ptr_y >= m->h ? m->h - 1 : m->ptr_y;
    g_frame.mods = m->mods;
    g_frame.numlock = m->numlock;

    if (resized) {
        vlog("the desktop is %dx%d now", m->w, m->h);
        Client *who = S.mode_requester;
        S.mode_requester = NULL;
        clients_frame_resized(who ? 1 : 0, who, 0);
    } else {
        if (shape) clients_frame_cursor_shape();
        if (shape || moved) clients_frame_cursor_moved(old_cursor, cursor_rect(&g_frame));
    }
    region_free(&damage);
    if (!S.clip_known || m->clip_serial != S.clip_serial) {
        bool first = !S.clip_known;
        S.clip_known = true;
        S.clip_serial = m->clip_serial;
        if (!first) send_msg(FM_CLIPBOARD_GET, NULL, 0);              /* the clipboard changed: fetch it for the viewers */
    }
}

/* UTF-8 (the system's clipboard) to Latin-1 (what RFB's cut text carries): characters beyond Latin-1 become '?'. */
static size_t utf8_to_latin1(const uint8_t *s, size_t n, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && o < cap;) {
        uint32_t c = s[i];
        int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!len || i + (size_t)len > n) { out[o++] = '?'; i++; continue; }
        if (len > 1) {
            c &= 0xFFu >> (len + 1);
            bool ok = true;
            for (int k = 1; k < len; k++) { if ((s[i + k] & 0xC0) != 0x80) ok = false; c = (c << 6) | (s[i + k] & 0x3F); }
            if (!ok) { out[o++] = '?'; i++; continue; }
        }
        out[o++] = c < 0x100 ? (char)c : '?';
        i += (size_t)len;
    }
    return o;
}

static void handle_msg(const FairyHdr *h, const uint8_t *p) {
    size_t plen = h->size - sizeof *h;
    switch (h->type) {
    case FE_WELCOME: break;
    case FE_ERROR: {
        char text[65] = "";
        if (plen >= sizeof(FeError)) { memcpy(text, ((const FeError *)p)->text, 64); text[64] = 0; }
        vlog("the window server refused the frame service: %s", text);
        drop_connection("refused");
        S.next_try_ms = mono_ms() + 10 * RECONNECT_MS;
        break;
    }
    case FE_FRAME_READY:
        if (plen < sizeof(FeFrameReady) || S.subscribed) { drop_connection("a frame offer that makes no sense"); break; }
        if (!map_shm((const FeFrameReady *)p)) { drop_connection("the frame file was not usable"); S.next_try_ms = mono_ms() + 10 * RECONNECT_MS; break; }
        S.subscribed = true;
        S.need_full = true;
        S.said_waiting = false;
        S.clip_known = false;
        g_have_desktop = true;
        vlog("Hatate has the desktop in view: %dx%d frames from the window server", S.max_w, S.max_h);
        source_want_frame();
        break;
    case FE_FRAME: {
        if (!S.subscribed || !S.requested || plen < sizeof(FeFrame)) { drop_connection("a frame nobody asked for"); break; }
        const FeFrame *m = (const FeFrame *)p;
        size_t need = sizeof *m + (size_t)m->nrects * sizeof(FrameRect) + (size_t)m->nmoves * sizeof(FrameMove);
        if (m->nrects > FRAMEPUB_MAX_RECTS || m->nmoves > FRAMEPUB_MAX_MOVES || plen < need || m->w < 1 || m->h < 1 || m->w > S.max_w || m->h > S.max_h) {
            drop_connection("a frame message that does not add up");
            break;
        }
        S.requested = S.outstanding_now = false;
        S.received++;
        S.last_frame_ms = mono_ms();
        S.seq = m->seq;
        const FrameRect *rects = (const FrameRect *)(p + sizeof *m);
        apply_frame(m, rects, (const FrameMove *)(rects + m->nrects));
        break;
    }
    case FE_CLIPBOARD: {
        if (plen < sizeof(uint32_t)) break;
        uint32_t n;
        memcpy(&n, p, 4);
        if (n > plen - 4) n = (uint32_t)(plen - 4);
        char text[1024];
        size_t len = utf8_to_latin1(p + 4, n, text, sizeof text);
        clients_clipboard(text, len);
        break;
    }
    default:
        break;                                                  /* window events and the like: vncd has no windows */
    }
}

void source_on_readable(void) {
    if (S.fd < 0) return;
    if (S.in_len >= sizeof S.in) { drop_connection("a message longer than the protocol allows"); return; }
    ssize_t n = read(S.fd, S.in + S.in_len, sizeof S.in - S.in_len);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) { drop_connection(n == 0 ? "closed" : strerror(errno)); return; }
    if (n < 0) return;
    S.in_len += (size_t)n;
    size_t off = 0;
    while (S.fd >= 0 && S.in_len - off >= sizeof(FairyHdr)) {
        FairyHdr h;
        memcpy(&h, S.in + off, sizeof h);
        if (h.size < sizeof h || h.size > FAIRY_MSG_MAX) { drop_connection("a message of impossible size"); return; }
        if (S.in_len - off < h.size) break;
        handle_msg(&h, S.in + off + sizeof h);
        off += h.size;
    }
    if (S.fd < 0) return;
    if (off) { memmove(S.in, S.in + off, S.in_len - off); S.in_len -= off; }
}

void source_want_frame(void) { S.want = true; }

/* Does anybody want a frame: a viewer is waiting, or one asked for the screen as it is now (*fresh), or the picture is known to be out of date. */
static bool frame_wanted(bool *fresh) {
    bool f = (int32_t)(S.fresh_wanted - S.received) > 0;
    bool want = S.want || S.need_full || f;
    for (Client *c = clients_first(); c && !want; c = client_next_of(c)) want = client_waiting_for_frame(c);
    *fresh = f;
    return want;
}

void source_poll(long now) {
    if (S.fd < 0) {
        if (now >= S.next_try_ms) try_connect(now);
        return;
    }
    if (S.mode_requester && now > S.mode_deadline) {             /* the window server did not change the size in time */
        clients_resize_answer(S.mode_requester, 2);
        S.mode_requester = NULL;
    }
    if (S.requested && S.outstanding_now && now - S.now_since > NOW_WAIT_MS) {     /* viewers must not wait for ever on a server that hangs */
        drop_connection("it did not answer a request for the screen within 5 seconds");
        return;
    }
    if (!S.subscribed || S.requested) return;
    bool fresh;
    bool want = frame_wanted(&fresh);
    if (!want || (!fresh && now - S.last_frame_ms < MIN_FRAME_MS)) return;
    FmFrameRequest rq = { (S.need_full ? FRAMEREQ_FULL : 0) | (fresh ? FRAMEREQ_NOW : 0) };
    send_msg(FM_FRAME_REQUEST, &rq, sizeof rq);
    if (S.fd < 0) return;
    S.requested = true;
    S.outstanding_now = fresh;
    S.now_since = now;
    S.sent++;
    S.want = false;
}

/* A viewer's FramebufferUpdateRequest must be answered from the screen as it is, not from a copy vncd took long ago: the number
 * of the frame that answers it (to wait for with source_has_frame). With a request outstanding, that one is told to answer now
 * (a second FM_FRAME_REQUEST with FRAMEREQ_NOW changes the pending one, it does not queue another); otherwise the next request
 * asks for an answer now. Without a window server there is nothing to wait for. */
uint32_t source_fresh_ticket(void) {
    if (S.fd < 0 || !S.subscribed) return S.received;
    if (S.requested) {
        if (!S.outstanding_now) {
            FmFrameRequest rq = { FRAMEREQ_NOW };
            send_msg(FM_FRAME_REQUEST, &rq, sizeof rq);
            S.outstanding_now = true;
            S.now_since = mono_ms();
        }
        return S.sent;
    }
    uint32_t t = S.sent + 1;
    if ((int32_t)(t - S.fresh_wanted) > 0) S.fresh_wanted = t;
    return t;
}

bool source_has_frame(uint32_t ticket) {
    if (S.fd < 0 || !S.subscribed) return true;
    return (int32_t)(S.received - ticket) >= 0;
}

long source_next_deadline(void) {
    if (S.fd < 0) return S.next_try_ms;
    long d = S.mode_requester ? S.mode_deadline : 0;
    bool fresh;
    if (S.subscribed && !S.requested && frame_wanted(&fresh)) {     /* only when a frame is wanted: with nobody waiting there is nothing to wake up for (the deadline "15 ms after the last frame" is in the past
                                                                      then, and main()'s poll() with a timeout of zero spun at 100% of a core on the Dell for 40 minutes) */
        long t = fresh ? mono_ms() : S.last_frame_ms + MIN_FRAME_MS;
        if (!d || t < d) d = t;
    }
    return d;
}

void source_set_clipboard(const char *latin1, size_t len) {
    if (S.fd < 0) return;
    FmClipboard cb;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {                          /* Latin-1 to UTF-8, as far as the clipboard holds */
        uint8_t c = (uint8_t)latin1[i];
        if (c < 0x80) { if (o + 1 > sizeof cb.data) break; cb.data[o++] = (char)c; }
        else { if (o + 2 > sizeof cb.data) break; cb.data[o++] = (char)(0xC0 | (c >> 6)); cb.data[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    cb.len = (uint32_t)o;
    send_msg(FM_CLIPBOARD_SET, &cb, sizeof cb.len + o);
}

bool source_set_mode(int w, int h, Client *requester) {
    if (S.fd < 0 || !S.subscribed || S.mode_requester) return false;
    int s = g_frame.scale > 0 ? g_frame.scale : 1;
    FmMode m = { w / s, h / s };                                /* the toolkit's protocol speaks logical pixels */
    send_msg(FM_SET_MODE, &m, sizeof m);
    if (S.fd < 0) return false;
    S.mode_requester = requester;
    S.mode_deadline = mono_ms() + MODE_WAIT_MS;
    source_want_frame();
    return true;
}

void source_forget_client(Client *c) { if (S.mode_requester == c) S.mode_requester = NULL; }

int source_max_w(void) { return S.subscribed ? S.max_w : 0; }
int source_max_h(void) { return S.subscribed ? S.max_h : 0; }
