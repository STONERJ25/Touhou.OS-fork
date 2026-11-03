/*
 * One viewer: the RFB 3.3/3.7/3.8 handshake (RFC 6143 sections 7.1-7.3), its messages (7.5), the updates it is sent (7.6) and the
 * input it gives the desktop.
 *
 * Everything a viewer sends is network input into a root process, so every message is checked against the specification before
 * it is used: the version string byte for byte, the security type against what was offered, pixel formats (encode.c), the
 * number of encodings, cut-text lengths, the screens of SetDesktopSize. A message type that is not known ends the connection
 * (its length could not be known either). Nothing a viewer sends can make the server allocate more than a fixed amount per
 * viewer, and a viewer that stops reading is dropped after a while instead of making its output grow.
 *
 * Updates (7.5.3): a viewer asks with FramebufferUpdateRequest and is sent what changed in the area it asked about, once there
 * is something. What it lacks is kept as two regions: `mod`, pixels to send, and `copy`, a pending CopyRect (pixels the viewer
 * already has elsewhere, all shifted by the same cdx, cdy). When a window moves (a hint from the window server, verified in
 * source.c), the new region either becomes a copy or is sent as pixels, as follows: a pixel whose old place the viewer has
 * right can be copied from there; a pixel whose old place is itself still to be copied can be copied from where that one is
 * (the two shifts add up); a pixel whose old place the viewer does not have is sent. Only one shift can be pending, so of
 * these kinds the largest wins and the rest is sent as pixels. Sending pixels is always correct, so every doubtful case does.
 */
#include "vncd.h"
#include <keys.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#define IN_CAP          8192
#define OUT_LOW_WATER   (256 * 1024)        /* a new update is built only when less than this is still waiting to go out */
#define CUT_MAX         (1u << 20)          /* longer ClientCutText is taken for hostile */
#define ENCODINGS_MAX   512
#define REGION_RECTS    48
#define MAX_KEYS        32

enum { ST_VERSION, ST_SECTYPE, ST_VNCAUTH, ST_FAIL_WAIT, ST_INIT, ST_NORMAL, ST_CUTTEXT, ST_CLOSING };

struct Client {
    Client *next;
    int fd;
    uint32_t ip;
    char peer[48];
    int state, minor;
    uint8_t in[IN_CAP];
    size_t in_len;
    OutBuf out;
    long t_start, t_fail_reply, t_progress, t_closing;
    int sectype;                            /* what was offered: 1 None, 2 VNC Authentication */
    char refusal[120];
    uint8_t challenge[16];
    char password[16];
    bool shared;
    PixConv pc;
    EncState enc;
    int pref;                               /* ENC_ZRLE, ENC_HEXTILE or ENC_RAW */
    bool can_copy, can_cursor, can_size, can_ext;
    bool send_shape, ext_initial, size_pending;
    int size_reason, size_status;
    Region mod, copy;
    int cdx, cdy;
    bool req;
    VRect req_area;
    uint32_t ticket;                        /* the window server's frame that answers the request (source_fresh_ticket) */
    uint8_t buttons;
    int px, py;
    bool pointer_known;
    struct { uint32_t sym; uint8_t code; } keys[MAX_KEYS];
    int nkeys;
    bool warned_key;
    uint32_t cut_left;
    size_t cut_len;
    char cut[1024];
    bool dead;
    char why[96];
    unsigned long updates;
    uint64_t sent;
    uint64_t encode_us;                     /* time spent building this viewer's updates */
};

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static Client *g_list;
static uint8_t g_down[256];                 /* how many viewers hold each key down */
char g_desktop_name[96];

Client *clients_first(void) { return g_list; }
Client *client_next_of(const Client *c) { return c->next; }
int clients_count(void) { int n = 0; for (Client *c = g_list; c; c = c->next) n++; return n; }
int client_fd(const Client *c) { return c->fd; }
bool client_dead(const Client *c) { return c->dead; }
uint32_t client_ip(const Client *c) { return c->ip; }
bool client_authenticated(const Client *c) { return c->state == ST_NORMAL || c->state == ST_CUTTEXT; }
bool client_shared(const Client *c) { return c->shared; }
bool client_can_copy(const Client *c) { return (c->state == ST_NORMAL || c->state == ST_CUTTEXT) && !c->dead && c->can_copy; }
bool client_wants_write(const Client *c) { return ob_pending(&c->out) > 0; }

static inline unsigned get16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static inline uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static VRect screen_rect(void) { return (VRect){ 0, 0, g_frame.w, g_frame.h }; }

/* Ends the connection at once (hostile or broken input: nothing more is said to the viewer). */
void client_close(Client *c, const char *why) {
    if (c->dead) return;
    c->dead = true;
    snprintf(c->why, sizeof c->why, "%s", why);
}

/* Says the last things (already queued) and then closes. */
static void client_finish(Client *c, const char *why) {
    if (c->state == ST_CLOSING || c->dead) return;
    c->state = ST_CLOSING;
    c->t_closing = mono_ms();
    snprintf(c->why, sizeof c->why, "%s", why);
    if (!ob_pending(&c->out)) c->dead = true;
}

Client *client_new(int fd, uint32_t ip, const char *peer) {
    Client *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->fd = fd;
    c->ip = ip;
    snprintf(c->peer, sizeof c->peer, "%s", peer);
    region_init(&c->mod);
    region_init(&c->copy);
    c->pref = ENC_RAW;
    c->t_start = c->t_progress = mono_ms();
    c->enc.z_level = 1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);       /* key presses and small updates should not wait for company */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);              /* a viewer that does not read must not park the server in write() */
    ob_put(&c->out, "RFB 003.008\n", 12);
    c->next = g_list;
    g_list = c;
    return c;
}

void client_set_refusal(Client *c, const char *reason) { snprintf(c->refusal, sizeof c->refusal, "%s", reason); }

/* ------------------------------------------------------------------------------------------------ input for the desktop */

static void key_value(uint8_t code, int value) {
    if (value == 1) {
        if (g_down[code]++ == 0) vinput_key(code, 1);
        else vinput_key(code, 2);                               /* another viewer holds it already: a repeat for the window server */
    } else if (value == 2) vinput_key(code, 2);
}

static void key_release(uint8_t code) {
    if (g_down[code] && --g_down[code] == 0) vinput_key(code, 0);
}

/* Presses (value 1) or repeats (2) `code`, with shift pressed or lifted around it when the character needs the other state. */
static void key_with_shift(uint8_t code, int want_shift, bool letter, int value) {
    bool shift_now = g_down[KEY_LSHIFT] || g_down[KEY_RSHIFT];
    int need = want_shift;
    if (need >= 0 && letter && (g_frame.mods & MOD_CAPS)) need = !need;    /* Caps Lock on the desktop turns the letters round */
    if (need == 1 && !shift_now) {
        vinput_key(KEY_LSHIFT, 1);
        key_value(code, value);
        vinput_key(KEY_LSHIFT, 0);
    } else if (need == 0 && shift_now) {
        if (g_down[KEY_LSHIFT]) vinput_key(KEY_LSHIFT, 0);
        if (g_down[KEY_RSHIFT]) vinput_key(KEY_RSHIFT, 0);
        key_value(code, value);
        if (g_down[KEY_LSHIFT]) vinput_key(KEY_LSHIFT, 1);
        if (g_down[KEY_RSHIFT]) vinput_key(KEY_RSHIFT, 1);
    } else {
        key_value(code, value);
    }
}

static void key_event(Client *c, bool down, uint32_t sym) {
    int want;
    bool letter;
    int code = keysym_to_code(sym, &want, &letter);
    int k = -1;
    for (int i = 0; i < c->nkeys; i++) if (c->keys[i].sym == sym) { k = i; break; }
    if (!down) {
        if (k < 0 && code) for (int i = 0; i < c->nkeys; i++) if (c->keys[i].code == code) { k = i; break; }   /* released under another keysym */
        if (k < 0) return;
        uint8_t kc = c->keys[k].code;
        c->keys[k] = c->keys[--c->nkeys];
        key_release(kc);
        return;
    }
    if (!code) {
        if (!c->warned_key) { vlog("%s: keysym 0x%x has no key on the system's US keyboard; such keys are dropped", c->peer, (unsigned)sym); c->warned_key = true; }
        return;
    }
    if (k >= 0) { key_with_shift((uint8_t)code, want, letter, 2); return; }   /* the viewer's own key repeat */
    if (c->nkeys == MAX_KEYS) return;                           /* thirty-two keys at once: the rest are ignored */
    c->keys[c->nkeys].sym = sym;
    c->keys[c->nkeys].code = (uint8_t)code;
    c->nkeys++;
    key_with_shift((uint8_t)code, want, letter, 1);
}

static void pointer_event(Client *c, uint8_t mask, int x, int y) {
    if (g_frame.w > 0 && x >= g_frame.w) x = g_frame.w - 1;
    if (g_frame.h > 0 && y >= g_frame.h) y = g_frame.h - 1;
    if (!c->pointer_known || x != c->px || y != c->py) vinput_pointer_abs(x, y);
    for (int i = 0; i < 3; i++)                                 /* RFB buttons 1, 2, 3: left, middle, right */
        if ((mask ^ c->buttons) & (1u << i)) vinput_button(i, (mask >> i) & 1);
    if ((mask & 8) && !(c->buttons & 8)) vinput_wheel(1);       /* buttons 4 and 5 are the wheel: one click per press */
    if ((mask & 16) && !(c->buttons & 16)) vinput_wheel(-1);
    c->buttons = mask;
    c->px = x;
    c->py = y;
    c->pointer_known = true;
}

static void release_everything(Client *c) {
    for (int i = 0; i < c->nkeys; i++) key_release(c->keys[i].code);
    c->nkeys = 0;
    for (int i = 0; i < 3; i++) if (c->buttons & (1u << i)) vinput_button(i, false);
    c->buttons = 0;
    vinput_flush();
}

void client_free(Client *c) {
    for (Client **pp = &g_list; *pp; pp = &(*pp)->next) if (*pp == c) { *pp = c->next; break; }
    release_everything(c);
    source_forget_client(c);
    if (c->state >= ST_NORMAL && c->state != ST_CLOSING)
        vlog("%s: viewer gone (%s); %lu updates, %llu KB sent, %llu ms spent encoding", c->peer, c->why[0] ? c->why : "closed", c->updates,
             (unsigned long long)(c->sent / 1024), (unsigned long long)(c->encode_us / 1000));
    else if (c->why[0]) vlog("%s: %s", c->peer, c->why);
    close(c->fd);
    enc_state_free(&c->enc);
    region_free(&c->mod);
    region_free(&c->copy);
    ob_free(&c->out);
    memset(c->password, 0, sizeof c->password);
    free(c);
}

void client_kick_unshared_others(Client *keep) {
    for (Client *c = g_list; c; c = c->next)
        if (c != keep && client_authenticated(c)) { vlog("%s: a viewer asked for the desktop alone: this one is closed", c->peer); client_close(c, "another viewer asked for exclusive access"); }
}

/* ------------------------------------------------------------------------------------------------ the handshake */

static void send_reason(Client *c, const char *reason) {
    ob_u32(&c->out, (uint32_t)strlen(reason));
    ob_put(&c->out, reason, strlen(reason));
}

static void refuse(Client *c, const char *reason) {
    if (c->minor == 3) ob_u32(&c->out, 0);                       /* 3.3: security type 0 = failed, then the reason */
    else ob_u8(&c->out, 0);                                     /* 3.7, 3.8: no security types, then the reason */
    send_reason(c, reason);
    vlog("%s: turned away: %s", c->peer, reason);
    client_finish(c, "turned away");
}

static void send_challenge(Client *c) {
    random_bytes(c->challenge, sizeof c->challenge);
    ob_put(&c->out, c->challenge, sizeof c->challenge);
    c->state = ST_VNCAUTH;
}

static void start_security(Client *c) {
    if (c->refusal[0]) { refuse(c, c->refusal); return; }
    char pw[16];
    int rc = auth_read_password(pw, sizeof pw);
    if (rc == 1) { c->sectype = 2; memcpy(c->password, pw, sizeof pw); }
    else if (rc == 0 && !g_opt.listen_all) c->sectype = 1;        /* no password, and only this machine (or an ssh tunnel) can reach us */
    else if (rc == 0) { refuse(c, "no VNC password is set (root sets one with vncd -passwd), and without one only 127.0.0.1 is served"); return; }
    else { refuse(c, "the server's password file is not usable (see the system log)"); return; }
    memset(pw, 0, sizeof pw);
    if (c->minor == 3) {                                        /* 3.3: the server decides */
        ob_u32(&c->out, (uint32_t)c->sectype);
        if (c->sectype == 2) send_challenge(c);
        else c->state = ST_INIT;
    } else {
        ob_u8(&c->out, 1);
        ob_u8(&c->out, (unsigned)c->sectype);
        c->state = ST_SECTYPE;
    }
}

static void security_result(Client *c, bool ok, const char *reason) {
    ob_u32(&c->out, ok ? 0 : 1);
    if (!ok && c->minor >= 8) send_reason(c, reason);
}

static void server_init(Client *c) {
    VRect s = screen_rect();
    ob_u16(&c->out, (unsigned)s.w);
    ob_u16(&c->out, (unsigned)s.h);
    uint8_t pf[16];
    pixfmt_write(&VNC_NATIVE_FORMAT, pf);
    ob_put(&c->out, pf, 16);
    ob_u32(&c->out, (uint32_t)strlen(g_desktop_name));
    ob_put(&c->out, g_desktop_name, strlen(g_desktop_name));
    pixconv_init(&c->pc, &VNC_NATIVE_FORMAT);
    region_set(&c->mod, s);                                     /* the first update is the whole screen, whatever is asked */
    c->state = ST_NORMAL;
    vlog("%s: a viewer looks at the desktop (%dx%d, %s, %s)", c->peer, s.w, s.h, c->sectype == 2 ? "VNC password" : "no password",
         c->shared ? "shared" : "alone");
}

/* "RFB 003.008\n": 3.8, 3.7 and 3.3 are what RFC 6143 knows; 3.4 to 3.6 are taken for 3.3 (the specification says so). */
static bool parse_version(Client *c, const uint8_t *v) {
    if (memcmp(v, "RFB ", 4) || v[7] != '.' || v[11] != '\n') return false;
    for (int i = 4; i < 11; i++) if (i != 7 && (v[i] < '0' || v[i] > '9')) return false;
    int major = (v[4] - '0') * 100 + (v[5] - '0') * 10 + (v[6] - '0');
    int minor = (v[8] - '0') * 100 + (v[9] - '0') * 10 + (v[10] - '0');
    if (major != 3 || minor < 3 || minor > 8) return false;
    c->minor = minor == 8 ? 8 : minor == 7 ? 7 : 3;
    return true;
}

/* ------------------------------------------------------------------------------------------------ messages */

static void set_encodings(Client *c, const uint8_t *p, unsigned n) {
    bool had_cursor = c->can_cursor, had_ext = c->can_ext;
    c->pref = -1;
    c->can_copy = c->can_cursor = c->can_size = c->can_ext = false;
    for (unsigned i = 0; i < n; i++) {
        int32_t e = (int32_t)get32(p + 4 * i);
        switch (e) {
        case ENC_RAW: case ENC_HEXTILE: case ENC_ZRLE: if (c->pref < 0) c->pref = e; break;
        case ENC_COPYRECT: c->can_copy = true; break;
        case ENC_CURSOR: c->can_cursor = true; break;
        case ENC_DESKTOPSIZE: c->can_size = true; break;
        case ENC_EXTDESKTOPSIZE: c->can_ext = true; break;
        default:
            if (e >= ENC_COMPRESS_0 && e <= ENC_COMPRESS_9 && !c->enc.z_ready) c->enc.z_level = e - ENC_COMPRESS_0;
            break;                                              /* encodings this server does not speak are simply not used */
        }
    }
    if (c->pref < 0) c->pref = ENC_RAW;                         /* Raw is always allowed */
    if (g_opt.verbose) {                                        /* what a real viewer asks for (vnc.verbose): for finding out why one misbehaves */
        char list[150];
        size_t o = 0;
        for (unsigned i = 0; i < n && o + 12 < sizeof list; i++) o += (size_t)snprintf(list + o, sizeof list - o, "%s%d", i ? "," : "", (int32_t)get32(p + 4 * i));
        vlog("%s: encodings %s%s; sending %s", c->peer, list, o + 12 >= sizeof list ? ",..." : "",
             c->pref == ENC_ZRLE ? "ZRLE" : c->pref == ENC_HEXTILE ? "Hextile" : "Raw");
    }
    if (c->can_cursor && !had_cursor) c->send_shape = true;
    if (!c->can_cursor && had_cursor) region_add_rect(&c->mod, cursor_rect(&g_frame));   /* from now on the pointer is in the pixels */
    if (c->can_ext && !had_ext) c->ext_initial = true;          /* rfbproto: tell the screen layout once the viewer can take it */
    if (!c->can_copy && c->copy.n) { region_union(&c->mod, &c->copy); region_clear(&c->copy); }
}

static void set_pixel_format(Client *c, const uint8_t *p) {
    PixelFormat pf;
    pixfmt_parse(&pf, p);
    const char *why = pixfmt_check(&pf);
    if (why[0]) {
        char text[96];
        snprintf(text, sizeof text, "a pixel format the server cannot serve: %s", why);
        client_close(c, text);
        return;
    }
    pixconv_init(&c->pc, &pf);
    if (g_opt.verbose)
        vlog("%s: pixel format %u bpp, depth %u, %s endian, %s, max %u/%u/%u, shift %u/%u/%u", c->peer, pf.bpp, pf.depth, pf.big_endian ? "big" : "little",
             pf.true_colour ? "true colour" : "colour map", pf.rmax, pf.gmax, pf.bmax, pf.rshift, pf.gshift, pf.bshift);
    if (c->pc.colour_map) {                                     /* SetColourMapEntries: the BGR233 cube the indexes refer to */
        ob_u8(&c->out, 1); ob_u8(&c->out, 0); ob_u16(&c->out, 0); ob_u16(&c->out, 256);
        for (int i = 0; i < 256; i++) {
            ob_u16(&c->out, (unsigned)((i >> 5) * 65535 / 7));
            ob_u16(&c->out, (unsigned)(((i >> 2) & 7) * 65535 / 7));
            ob_u16(&c->out, (unsigned)((i & 3) * 65535 / 3));
        }
    }
    region_set(&c->mod, screen_rect());                         /* what the viewer has is in the old format: everything again */
    region_clear(&c->copy);
    c->send_shape = c->can_cursor;
}

static void update_request(Client *c, bool incremental, int x, int y, int w, int h) {
    VRect r = vrect_intersect((VRect){ x, y, w, h }, screen_rect());     /* an area off the screen (just after a resize) is clipped, not an error */
    if (!incremental && !vrect_empty(r)) {
        region_add_rect(&c->mod, r);
        if (c->copy.n) { Region t; region_init(&t); region_copy(&t, &c->copy); region_intersect_rect(&t, r); region_union(&c->mod, &t);
                         region_subtract(&c->copy, &t); region_free(&t); }
    }
    c->req_area = c->req ? vrect_union(c->req_area, r) : r;
    c->req = true;
    c->ticket = source_fresh_ticket();                          /* answered from the screen as it is now, not from an old copy */
}

static void set_desktop_size(Client *c, unsigned w, unsigned h, unsigned nscreens, const uint8_t *screens) {
    int status = 0;
    if (!g_opt.allow_resize) status = 1;                        /* administratively prohibited (vncd --allow-resize, vnc.resize) */
    else if (nscreens != 1) status = 3;                         /* one screen is all the desktop has */
    else {
        unsigned sx = get16(screens + 4), sy = get16(screens + 6), sw = get16(screens + 8), sh = get16(screens + 10);
        int scale = g_frame.scale > 0 ? g_frame.scale : 1;
        int maxw = source_max_w(), maxh = source_max_h();
        if (sx || sy || sw != w || sh != h) status = 3;
        else if (!maxw || (int)w > maxw || (int)h > maxh || (int)w < 640 * scale || (int)h < 480 * scale || w % (unsigned)scale || h % (unsigned)scale) status = 3;
        else if ((int)w == g_frame.w && (int)h == g_frame.h) { clients_resize_answer(c, 0); return; }
        else if (!source_set_mode((int)w, (int)h, c)) status = 2;
        else { vlog("%s: asks for a %ux%u desktop", c->peer, w, h); return; }    /* the answer comes with the window server's next frame */
    }
    clients_resize_answer(c, status);
}

/* Handles what is in c->in; returns false when it must wait for more. */
static bool handle_one(Client *c) {
    const uint8_t *p = c->in;
    size_t have = c->in_len, used = 0;
    switch (c->state) {
    case ST_VERSION:
        if (have < 12) return false;
        if (!parse_version(c, p)) { client_close(c, "not an RFB 3.x viewer (bad ProtocolVersion)"); return false; }
        used = 12;
        start_security(c);
        break;
    case ST_SECTYPE:
        if (have < 1) return false;
        used = 1;
        if (p[0] != c->sectype) {
            if (c->minor >= 8) { security_result(c, false, "that security type was not offered"); client_finish(c, "chose a security type that was not offered"); }
            else client_close(c, "chose a security type that was not offered");
            break;
        }
        if (c->sectype == 2) send_challenge(c);
        else { if (c->minor >= 8) security_result(c, true, NULL); c->state = ST_INIT; }
        break;
    case ST_VNCAUTH: {
        if (have < 16) return false;
        used = 16;
        uint8_t want[16];
        vnc_auth_response(c->password, c->challenge, want);
        unsigned diff = 0;
        for (int i = 0; i < 16; i++) diff |= want[i] ^ p[i];       /* the whole comparison, always */
        memset(want, 0, sizeof want);
        memset(c->password, 0, sizeof c->password);
        if (!diff) {
            auth_succeeded(c->ip);
            security_result(c, true, NULL);
            c->state = ST_INIT;
        } else {
            vlog("%s: wrong VNC password", c->peer);
            auth_failed(c->ip, mono_ms(), c->peer);
            c->state = ST_FAIL_WAIT;                            /* the answer waits: guessing goes slowly */
            c->t_fail_reply = mono_ms() + g_opt.fail_delay_ms;
        }
        break;
    }
    case ST_INIT:
        if (have < 1) return false;
        used = 1;
        c->shared = p[0] != 0;
        if (!c->shared) client_kick_unshared_others(c);
        server_init(c);
        break;
    case ST_CUTTEXT: {
        if (have < 1) return false;
        size_t take = have < c->cut_left ? have : c->cut_left;
        size_t keep = take;
        if (c->cut_len + keep > sizeof c->cut) keep = sizeof c->cut - c->cut_len;
        memcpy(c->cut + c->cut_len, p, keep);
        c->cut_len += keep;
        c->cut_left -= (uint32_t)take;
        used = take;
        if (!c->cut_left) {
            source_set_clipboard(c->cut, c->cut_len);
            c->state = ST_NORMAL;
        }
        break;
    }
    case ST_NORMAL:
        if (have < 1) return false;
        switch (p[0]) {
        case 0:                                                 /* SetPixelFormat */
            if (have < 20) return false;
            used = 20;
            set_pixel_format(c, p + 4);
            break;
        case 2: {                                               /* SetEncodings */
            if (have < 4) return false;
            unsigned n = get16(p + 2);
            if (n > ENCODINGS_MAX) { client_close(c, "SetEncodings with too many encodings"); return false; }
            if (have < 4 + 4 * (size_t)n) return false;
            used = 4 + 4 * (size_t)n;
            set_encodings(c, p + 4, n);
            break;
        }
        case 3:                                                 /* FramebufferUpdateRequest */
            if (have < 10) return false;
            used = 10;
            update_request(c, p[1] != 0, (int)get16(p + 2), (int)get16(p + 4), (int)get16(p + 6), (int)get16(p + 8));
            break;
        case 4:                                                 /* KeyEvent */
            if (have < 8) return false;
            used = 8;
            key_event(c, p[1] != 0, get32(p + 4));
            break;
        case 5:                                                 /* PointerEvent */
            if (have < 6) return false;
            used = 6;
            pointer_event(c, p[1], (int)get16(p + 2), (int)get16(p + 4));
            break;
        case 6: {                                               /* ClientCutText */
            if (have < 8) return false;
            uint32_t len = get32(p + 4);
            if (len > CUT_MAX) { client_close(c, "ClientCutText longer than a megabyte"); return false; }
            used = 8;
            c->cut_len = 0;
            c->cut_left = len;
            if (len) c->state = ST_CUTTEXT;
            else source_set_clipboard("", 0);
            break;
        }
        case 251: {                                             /* SetDesktopSize (rfbproto, with ExtendedDesktopSize) */
            if (!c->can_ext) { client_close(c, "SetDesktopSize without ExtendedDesktopSize"); return false; }
            if (have < 8) return false;
            unsigned n = p[6];
            if (have < 8 + 16 * (size_t)n) return false;
            used = 8 + 16 * (size_t)n;
            set_desktop_size(c, get16(p + 2), get16(p + 4), n, p + 8);
            break;
        }
        default: {
            char text[64];
            snprintf(text, sizeof text, "unknown message type %u", p[0]);
            client_close(c, text);
            return false;
        }
        }
        break;
    default:
        c->in_len = 0;                                          /* refused or closing: what else comes is not read */
        return false;
    }
    if (used) {
        memmove(c->in, c->in + used, c->in_len - used);
        c->in_len -= used;
    }
    return !c->dead && c->in_len > 0;
}

void client_on_readable(Client *c) {
    if (c->dead) return;
    if (c->in_len == sizeof c->in) { client_close(c, "input that never forms a message"); return; }
    ssize_t n = read(c->fd, c->in + c->in_len, sizeof c->in - c->in_len);
    if (n == 0) { client_close(c, "the viewer closed the connection"); return; }
    if (n < 0) {
        if (errno != EAGAIN && errno != EINTR) client_close(c, strerror(errno));
        return;
    }
    if (c->state == ST_FAIL_WAIT || c->state == ST_CLOSING) return;   /* not listening any more */
    c->in_len += (size_t)n;
    while (handle_one(c)) { }
    vinput_flush();
}

void client_on_writable(Client *c) {
    while (ob_pending(&c->out)) {
        ssize_t n = write(c->fd, c->out.d + c->out.off, ob_pending(&c->out));
        if (n > 0) { c->out.off += (size_t)n; c->sent += (uint64_t)n; c->t_progress = mono_ms(); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        client_close(c, n < 0 ? strerror(errno) : "write failed");
        return;
    }
    if (!ob_pending(&c->out)) {
        c->out.off = c->out.len = 0;
        if (c->state == ST_CLOSING) c->dead = true;
    } else if (c->out.off > (1u << 20)) ob_compact(&c->out);
}

void client_tick(Client *c, long now) {
    if (c->dead) return;
    if (c->out.failed) { client_close(c, "out of memory for the update"); return; }
    if (c->state < ST_NORMAL && c->state != ST_FAIL_WAIT && now - c->t_start > g_opt.handshake_ms) { client_close(c, "the handshake took too long"); return; }
    if (c->state == ST_FAIL_WAIT && now >= c->t_fail_reply) {
        security_result(c, false, "the password was not accepted");
        client_finish(c, "wrong password");
        return;
    }
    if (ob_pending(&c->out) && now - c->t_progress > g_opt.stall_ms) { client_close(c, "the viewer stopped reading"); return; }
    if (c->state == ST_CLOSING && now - c->t_closing > 5000) c->dead = true;
}

long client_next_deadline(const Client *c) {
    long d = 0;
    if (c->state < ST_NORMAL) d = c->t_start + g_opt.handshake_ms;
    if (c->state == ST_FAIL_WAIT) d = c->t_fail_reply;
    if (ob_pending(&c->out)) { long t = c->t_progress + g_opt.stall_ms; if (!d || t < d) d = t; }
    if (c->state == ST_CLOSING) { long t = c->t_closing + 5000; if (!d || t < d) d = t; }
    return d;
}

/* ------------------------------------------------------------------------------------------------ what changed on the screen */

static bool normal(const Client *c) { return (c->state == ST_NORMAL || c->state == ST_CUTTEXT) && !c->dead; }

/* The viewer's pixels that differ from the frame: what is still to be sent, and the pointer it had drawn into its pixels. */
static void viewer_dirty(const Client *c, Region *out) {
    region_copy(out, &c->mod);
    if (!c->can_cursor) region_add_rect(out, cursor_rect(&g_frame));
}

static void apply_change(Client *c, const Region *damage, VRect mv, int dx, int dy) {
    if (vrect_empty(mv) || !c->can_copy) {
        region_union(&c->mod, damage);
        if (!vrect_empty(mv)) region_add_rect(&c->mod, mv);
        if (c->copy.n) region_subtract(&c->copy, &c->mod);       /* a pending copy whose pixels changed is sent as pixels instead */
        region_simplify(&c->mod, REGION_RECTS);
        return;
    }
    Region h, dirty, bad, comp, clean, keep;
    region_init(&h); region_init(&dirty); region_init(&bad); region_init(&comp); region_init(&clean); region_init(&keep);
    region_set(&h, mv);
    viewer_dirty(c, &dirty);
    region_copy(&bad, &dirty);                                   /* the old place is not right in the viewer */
    region_translate(&bad, dx, dy);
    region_intersect(&bad, &h);
    region_copy(&comp, &c->copy);                                /* the old place is itself still to be copied */
    region_translate(&comp, dx, dy);
    region_intersect(&comp, &h);
    region_subtract(&comp, &bad);
    region_copy(&clean, &h);                                     /* the old place is right in the viewer */
    region_subtract(&clean, &bad);
    region_subtract(&clean, &comp);
    region_copy(&keep, &c->copy);                                /* the old copy where nothing changed */
    region_subtract(&keep, &h);
    region_subtract(&keep, damage);
    struct { Region *g; int dx, dy; long area; } k[3] = {
        { &keep, c->cdx, c->cdy, 0 }, { &comp, c->cdx + dx, c->cdy + dy, 0 }, { &clean, dx, dy, 0 },
    };
    for (int i = 0; i < 3; i++)                                  /* kinds with the same shift are one kind */
        for (int j = i + 1; j < 3; j++)
            if (k[i].dx == k[j].dx && k[i].dy == k[j].dy && k[j].g->n) { region_union(k[i].g, k[j].g); region_clear(k[j].g); }
    int best = -1;
    Region *free_ok = NULL;                                      /* shifted back to where they were: the viewer has them already */
    for (int i = 0; i < 3; i++) {
        k[i].area = region_area(k[i].g);
        if (!k[i].area) continue;
        if (!k[i].dx && !k[i].dy) { free_ok = k[i].g; continue; }
        if (best < 0 || k[i].area > k[best].area) best = i;
    }
    region_union(&c->mod, damage);
    region_union(&c->mod, &h);
    region_union(&c->mod, &c->copy);
    if (best >= 0) {
        region_subtract(&c->mod, k[best].g);
        region_copy(&c->copy, k[best].g);
        c->cdx = k[best].dx;
        c->cdy = k[best].dy;
    } else {
        region_clear(&c->copy);
    }
    if (free_ok) region_subtract(&c->mod, free_ok);
    if (c->copy.n > REGION_RECTS) { region_union(&c->mod, &c->copy); region_clear(&c->copy); }
    region_simplify(&c->mod, REGION_RECTS);
    region_free(&h); region_free(&dirty); region_free(&bad); region_free(&comp); region_free(&clean); region_free(&keep);
}

void clients_frame_changed(const Region *damage, VRect mv, int dx, int dy) {
    for (Client *c = g_list; c; c = c->next) if (normal(c)) apply_change(c, damage, mv, dx, dy);
}

void clients_frame_cursor_moved(VRect old_rect, VRect new_rect) {
    for (Client *c = g_list; c; c = c->next) {
        if (!normal(c) || c->can_cursor) continue;
        region_add_rect(&c->mod, old_rect);
        region_add_rect(&c->mod, new_rect);
        if (c->copy.n) region_subtract(&c->copy, &c->mod);
        region_simplify(&c->mod, REGION_RECTS);
    }
}

void clients_frame_cursor_shape(void) {
    for (Client *c = g_list; c; c = c->next) if (normal(c) && c->can_cursor) c->send_shape = true;
}

void clients_frame_resized(int reason, Client *requester, int status) {
    for (Client *c = g_list; c; c = c->next) {
        if (!normal(c)) continue;
        if (!c->can_ext && !c->can_size) { client_close(c, "the viewer cannot follow a change of the desktop size"); continue; }
        c->size_pending = true;
        c->size_reason = c == requester ? 1 : reason ? 2 : 0;
        c->size_status = status;
        region_set(&c->mod, screen_rect());
        region_clear(&c->copy);
        c->send_shape = c->can_cursor;
        c->req_area = vrect_intersect(c->req_area, screen_rect());
    }
}

void clients_resize_answer(Client *requester, int status) {
    for (Client *c = g_list; c; c = c->next) {
        if (c != requester || !normal(c)) continue;
        c->size_pending = true;
        c->size_reason = 1;
        c->size_status = status;
    }
}

void clients_clipboard(const char *latin1, size_t len) {
    for (Client *c = g_list; c; c = c->next) {
        if (!normal(c)) continue;
        ob_u8(&c->out, 3); ob_u8(&c->out, 0); ob_u16(&c->out, 0);
        ob_u32(&c->out, (uint32_t)len);
        ob_put(&c->out, latin1, len);
    }
}

/* ------------------------------------------------------------------------------------------------ sending updates */

static int g_order_dx, g_order_dy;

/* CopyRect rectangles in an order where none overwrites the source of one that comes later (the region is banded, all have
 * the same shift): moving down, the lowest band first; within a band, moving right, the rightmost first. */
static int cmp_copy(const void *a, const void *b) {
    const VRect *p = a, *q = b;
    if (p->y != q->y) return g_order_dy > 0 ? (q->y > p->y) - (q->y < p->y) : (p->y > q->y) - (p->y < q->y);
    return g_order_dx > 0 ? (q->x > p->x) - (q->x < p->x) : (p->x > q->x) - (p->x < q->x);
}

static void send_size(Client *c) {
    VRect s = screen_rect();
    OutBuf *o = &c->out;
    ob_u8(o, 0); ob_u8(o, 0); ob_u16(o, 1);
    if (c->can_ext) {
        int reason = c->size_pending ? c->size_reason : 0, status = c->size_pending ? c->size_status : 0;
        enc_rect_header(o, (VRect){ reason, status, s.w, s.h }, ENC_EXTDESKTOPSIZE);
        ob_u8(o, 1); ob_u8(o, 0); ob_u16(o, 0);                  /* one screen, three bytes of padding */
        ob_u32(o, 0); ob_u16(o, 0); ob_u16(o, 0); ob_u16(o, (unsigned)s.w); ob_u16(o, (unsigned)s.h); ob_u32(o, 0);
    } else {
        enc_rect_header(o, s, ENC_DESKTOPSIZE);
    }
    c->size_pending = c->ext_initial = false;
    c->req = false;
    c->updates++;
}

static bool has_work(const Client *c, VRect area) {
    if (c->size_pending || c->ext_initial || (c->can_cursor && c->send_shape)) return true;
    for (int i = 0; i < c->mod.n; i++) if (!vrect_empty(vrect_intersect(c->mod.r[i], area))) return true;
    for (int i = 0; i < c->copy.n; i++) if (!vrect_empty(vrect_intersect(c->copy.r[i], area))) return true;
    return false;
}

bool client_waiting_for_frame(const Client *c) {
    if (!normal(c) || !c->req || ob_pending(&c->out) > OUT_LOW_WATER) return false;
    return !source_has_frame(c->ticket) || !has_work(c, vrect_intersect(c->req_area, screen_rect()));
}

void client_try_update(Client *c) {
    if (!normal(c) || !c->req || ob_pending(&c->out) > OUT_LOW_WATER) return;
    if (!source_has_frame(c->ticket)) return;                   /* the fresh frame is on its way */
    if (c->size_pending || c->ext_initial) { send_size(c); return; }
    VRect area = vrect_intersect(c->req_area, screen_rect());
    if (!has_work(c, area)) return;
    uint64_t t0 = now_us();
    Region send, cp;
    region_init(&send);
    region_init(&cp);
    region_copy(&send, &c->mod);
    region_intersect_rect(&send, area);
    region_copy(&cp, &c->copy);
    region_intersect_rect(&cp, area);
    if (region_area(&cp) != region_area(&c->copy)) {              /* a copy that reaches outside the asked area becomes pixels for later */
        region_subtract(&c->copy, &cp);
        region_union(&c->mod, &c->copy);
    }
    VRect cur = cursor_rect(&g_frame);
    if (!c->can_cursor && cp.n && !vrect_empty(cur)) {             /* a copy over the drawn pointer: draw the pointer again after it */
        Region t; region_init(&t); region_copy(&t, &cp); region_intersect_rect(&t, cur);
        if (t.n) region_add_rect(&send, vrect_intersect(cur, area));
        region_free(&t);
    }
    bool shape = c->can_cursor && c->send_shape;
    unsigned n = (shape ? 1u : 0u) + (unsigned)cp.n + (unsigned)send.n;
    OutBuf *o = &c->out;
    ob_u8(o, 0); ob_u8(o, 0); ob_u16(o, n);
    if (shape) { enc_cursor(o, &c->pc, &g_frame); c->send_shape = false; }
    if (cp.n) {
        g_order_dx = c->cdx;
        g_order_dy = c->cdy;
        qsort(cp.r, (size_t)cp.n, sizeof cp.r[0], cmp_copy);
        for (int i = 0; i < cp.n; i++) {
            enc_rect_header(o, cp.r[i], ENC_COPYRECT);
            ob_u16(o, (unsigned)(cp.r[i].x - c->cdx));
            ob_u16(o, (unsigned)(cp.r[i].y - c->cdy));
        }
    }
    PixSrc src = { &g_frame, !c->can_cursor };
    for (int i = 0; i < send.n && !o->failed; i++) {
        if (c->pref == ENC_ZRLE) { if (enc_zrle(o, &c->pc, &src, send.r[i], &c->enc) < 0) { client_close(c, "the zlib stream failed"); break; } }
        else if (c->pref == ENC_HEXTILE) enc_hextile(o, &c->pc, &src, send.r[i], &c->enc);
        else enc_raw(o, &c->pc, &src, send.r[i], &c->enc);
    }
    if (o->failed) client_close(c, "out of memory for the update");
    region_subtract_rect(&c->mod, area);
    region_clear(&c->copy);
    region_free(&send);
    region_free(&cp);
    c->req = false;
    c->updates++;
    c->encode_us += now_us() - t0;
}
