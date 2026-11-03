/*
 * vncd: the VNC server of the desktop (RFB protocol version 3.8, RFC 6143; the pseudo-encodings follow the community
 * specification of the RFB protocol, rfbproto). See docs/VNC.md for the architecture: the picture comes from the window
 * server's frame service (../fairyd/framepub.h), input goes into the kernel's event queue through /dev/vinput.
 *
 * The files: main.c (options, sockets, the event loop), client.c (one viewer: handshake, messages, updates, input),
 * encode.c (pixel formats and the encodings), region.c (rectangle sets), des.c (DES for VNC authentication), auth.c (the
 * password file and the throttle), keys.c (X11 keysyms to the system's key codes), source.c (the window server's frames),
 * port.c (what differs between TouhouOS and the Linux build of the host tests).
 */
#ifndef VNCD_H
#define VNCD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <z.h>

#define VNC_DEFAULT_PORT     5900
#define VNC_MAX_W            8192
#define VNC_MAX_H            8192

/* ------------------------------------------------------------------------------------------------ rectangles and regions */

typedef struct { int x, y, w, h; } VRect;

/* A set of pixels as non-overlapping rectangles in y-x bands (sorted by y, then x; rectangles of one band share their y range).
 * Every operation produces that form, which is what CopyRect ordering relies on. */
typedef struct {
    VRect *r;
    int n, cap;
} Region;

static inline bool vrect_empty(VRect a) { return a.w <= 0 || a.h <= 0; }
VRect vrect_intersect(VRect a, VRect b);
VRect vrect_union(VRect a, VRect b);

void  region_init(Region *g);
void  region_free(Region *g);
void  region_clear(Region *g);
bool  region_empty(const Region *g);
void  region_set(Region *g, VRect a);
void  region_copy(Region *dst, const Region *src);
void  region_add_rect(Region *g, VRect a);                  /* g |= a */
void  region_union(Region *g, const Region *a);             /* g |= a */
void  region_subtract(Region *g, const Region *a);          /* g -= a */
void  region_subtract_rect(Region *g, VRect a);
void  region_intersect(Region *g, const Region *a);         /* g &= a */
void  region_intersect_rect(Region *g, VRect a);
void  region_translate(Region *g, int dx, int dy);
long  region_area(const Region *g);
VRect region_bbox(const Region *g);
bool  region_contains_point(const Region *g, int x, int y);
void  region_simplify(Region *g, int max_rects);            /* too many rectangles: a superset with fewer (for regions that are "to be sent") */

/* ------------------------------------------------------------------------------------------------ output buffers */

typedef struct {
    uint8_t *d;
    size_t len, off, cap;               /* d[off .. len) is not written yet */
    bool failed;                        /* out of memory: the connection is dropped */
} OutBuf;

void   ob_reserve(OutBuf *b, size_t more);
void   ob_put(OutBuf *b, const void *p, size_t n);
void   ob_u8(OutBuf *b, unsigned v);
void   ob_u16(OutBuf *b, unsigned v);
void   ob_u32(OutBuf *b, uint32_t v);
void   ob_s32(OutBuf *b, int32_t v);
size_t ob_pending(const OutBuf *b);
void   ob_compact(OutBuf *b);
void   ob_free(OutBuf *b);

/* ------------------------------------------------------------------------------------------------ pixels */

typedef struct {
    uint8_t  bpp, depth, big_endian, true_colour;
    uint16_t rmax, gmax, bmax;
    uint8_t  rshift, gshift, bshift;
} PixelFormat;

/* 0x00RRGGBB -> the client's pixel value: the three tables add up to it. */
typedef struct {
    PixelFormat pf;
    uint32_t rt[256], gt[256], bt[256];
    int bytes;                          /* bytes per pixel on the wire */
    int cbytes;                         /* bytes per CPIXEL (ZRLE): 3 when the colours fit in three bytes of a 32-bit pixel */
    int cskip;                          /* which byte of the wire pixel the CPIXEL starts at (0 or 1) */
    bool colour_map;                    /* 8-bit colour map: the server's BGR233 cube was sent with SetColourMapEntries */
} PixConv;

extern const PixelFormat VNC_NATIVE_FORMAT;                 /* 32 bpp, depth 24, little endian, 0x00RRGGBB */

/* "" when the format is acceptable, else what is wrong with it (the connection is then closed). */
const char *pixfmt_check(const PixelFormat *pf);
void pixconv_init(PixConv *c, const PixelFormat *pf);
void pixfmt_parse(PixelFormat *pf, const uint8_t b[16]);
void pixfmt_write(const PixelFormat *pf, uint8_t b[16]);

static inline uint32_t pix_value(const PixConv *c, uint32_t rgb) { return c->rt[(rgb >> 16) & 255] | c->gt[(rgb >> 8) & 255] | c->bt[rgb & 255]; }
void pix_put(const PixConv *c, uint8_t *out, uint32_t value);  /* `bytes` bytes in the client's byte order */
void pix_put_c(const PixConv *c, uint8_t *out, uint32_t value); /* a CPIXEL: `cbytes` bytes */

/* ------------------------------------------------------------------------------------------------ the frame */

typedef struct {
    uint32_t *px;                       /* the last frame the window server published (no pointer in it) */
    int w, h, stride;                   /* the screen in use; stride in pixels */
    int max_w, max_h;
    uint32_t *cur;                      /* the pointer sprite, 0xAARRGGBB, cur_w x cur_h (alpha 0 or 255) */
    int cur_w, cur_h, cur_hotx, cur_hoty;
    int ptr_x, ptr_y;
    uint32_t cursor_serial;
    uint8_t mods, numlock;
    int scale;                          /* the desktop's ui scale (sizes asked for with SetDesktopSize must be multiples of it) */
} Frame;

/* What the encoders read: the frame, with the pointer drawn into it when the viewer cannot draw it itself. */
typedef struct {
    const Frame *f;
    bool overlay;
} PixSrc;

VRect cursor_rect(const Frame *f);                          /* where the sprite covers the frame (may be empty) */
void  fetch_pixels(const PixSrc *s, int x, int y, int w, int h, uint32_t *out);   /* w*h pixels, rows packed */

/* ------------------------------------------------------------------------------------------------ encoders */

enum {
    ENC_RAW = 0, ENC_COPYRECT = 1, ENC_RRE = 2, ENC_HEXTILE = 5, ENC_ZLIB = 6, ENC_TIGHT = 7, ENC_TRLE = 15, ENC_ZRLE = 16,
    ENC_CURSOR = -239, ENC_DESKTOPSIZE = -223, ENC_LASTRECT = -224, ENC_EXTDESKTOPSIZE = -308,
    ENC_COMPRESS_0 = -256, ENC_COMPRESS_9 = -247,
};

typedef struct {
    ZStream z;                          /* ZRLE: one zlib stream for the whole connection */
    bool z_ready;
    int z_level;
    uint8_t *tmp;                       /* scratch: one rectangle's uncompressed ZRLE data */
    size_t tmp_cap;
    uint32_t *tile;                     /* scratch pixels */
    size_t tile_cap;
} EncState;

void enc_state_free(EncState *e);
void enc_rect_header(OutBuf *o, VRect r, int32_t encoding);
void enc_raw(OutBuf *o, const PixConv *c, const PixSrc *s, VRect r, EncState *e);
void enc_hextile(OutBuf *o, const PixConv *c, const PixSrc *s, VRect r, EncState *e);
int  enc_zrle(OutBuf *o, const PixConv *c, const PixSrc *s, VRect r, EncState *e);    /* -1: zlib failed */
void enc_cursor(OutBuf *o, const PixConv *c, const Frame *f);                       /* the cursor pseudo-rectangle, header included */

/* ------------------------------------------------------------------------------------------------ DES (VNC authentication) */

void des_encrypt_block(const uint8_t key[8], const uint8_t in[8], uint8_t out[8]);
/* The VNC response to a challenge: each 8-byte half encrypted with the password (at most 8 bytes, NUL-padded) as the key,
 * with the bits of every key byte in reverse order (the quirk of the original VNC). */
void vnc_auth_response(const char *password, const uint8_t challenge[16], uint8_t out[16]);

/* ------------------------------------------------------------------------------------------------ keys */

/* One key change for the window server: the system's key code (shared/keys.h) and 1 down / 0 up. */
typedef struct { uint8_t code; uint8_t down; } KeyStroke;

/* The key code an X11 keysym stands for, and the shift it wants (-1: none in particular, 0 without, 1 with); 0 when the
 * system's keyboard has no such key. */
int keysym_to_code(uint32_t keysym, int *want_shift, bool *is_letter);

/* ------------------------------------------------------------------------------------------------ the server */

typedef struct Client Client;

typedef struct {
    int port;
    bool listen_all;                    /* --listen-all / vnc.listen=all: every interface, not only 127.0.0.1 */
    bool allow_resize;                  /* --allow-resize / vnc.resize: viewers may change the desktop size (SetDesktopSize) */
    int max_clients, per_address;
    int throttle_fails;                 /* wrong passwords from one address within a minute before it is turned away for a minute (0: never) */
    int fail_delay_ms;                  /* the wait before a wrong password is answered */
    int handshake_ms, stall_ms;
    const char *passwd_path;            /* /etc/vnc.passwd */
    const char *keep_path;              /* the copy on the HUMANVILLAGE volume, NULL: none */
    const char *fairy_socket;           /* /tmp/fairy/fairyd.sock */
    const char *vinput_path;            /* /dev/vinput */
    bool verbose;
} Options;

extern Options g_opt;
extern Frame g_frame;
extern bool g_have_desktop;             /* subscribed to a window server that publishes frames */
extern char g_desktop_name[96];

/* client.c */
Client *client_new(int fd, uint32_t ip, const char *peer);  /* sends the ProtocolVersion; NULL when out of memory */
void    client_set_refusal(Client *c, const char *reason);  /* turn it away after the version exchange, with this reason */
void    client_free(Client *c);
int     client_fd(const Client *c);
bool    client_wants_write(const Client *c);
bool    client_dead(const Client *c);
bool    client_waiting_for_frame(const Client *c);          /* an update request is pending and there is nothing to send */
void    client_on_readable(Client *c);
void    client_on_writable(Client *c);
void    client_tick(Client *c, long now);                   /* timers: handshake timeout, delayed refusal, stalled output */
long    client_next_deadline(const Client *c);
void    client_try_update(Client *c);
void    client_close(Client *c, const char *why);
uint32_t client_ip(const Client *c);
bool    client_can_copy(const Client *c);                   /* a viewer that takes CopyRect (worth looking for scrolls) */
bool    client_authenticated(const Client *c);
bool    client_shared(const Client *c);
void    client_kick_unshared_others(Client *keep);

/* Frame changes, applied to every client (client.c). `mv` (may be empty): the frame's pixels in mv are the previous frame's at
 * mv - (dx, dy), compared pixel by pixel. Called before the pointer fields of g_frame are updated. */
void clients_frame_changed(const Region *damage, VRect mv, int dx, int dy);
void clients_frame_cursor_moved(VRect old_rect, VRect new_rect);
void clients_frame_cursor_shape(void);
void clients_frame_resized(int reason, Client *requester, int status);
void clients_clipboard(const char *latin1, size_t len);
void clients_resize_answer(Client *requester, int status);   /* the answer to one viewer's SetDesktopSize (status 0: no change needed) */

/* client.c: the list of clients */
Client *clients_first(void);
Client *client_next_of(const Client *c);
int     clients_count(void);

/* source.c: the window server */
void source_init(void);
int  source_fd(void);                                       /* -1 while not connected */
void source_poll(long now);                                 /* (re)connect; ask for a frame when somebody waits */
void source_on_readable(void);
long source_next_deadline(void);
void source_set_clipboard(const char *latin1, size_t len);
bool source_set_mode(int w, int h, Client *requester);      /* asks the window server for a new screen size (physical pixels) */
void source_forget_client(Client *c);
void source_want_frame(void);
uint32_t source_fresh_ticket(void);                         /* an update request came: the frame number that answers it */
bool source_has_frame(uint32_t ticket);                     /* that frame (or a later one) has arrived, or there is no window server */
int  source_max_w(void);                                   /* the largest screen the window server can have (0: no server) */
int  source_max_h(void);

/* auth.c */
int  auth_read_password(char *out, size_t cap);             /* 1 a password, 0 none set, -1 the file is not usable (logged) */
int  auth_set_password(const char *pw, char *why, size_t whylen);
bool auth_blocked(uint32_t ip, long now);
void auth_failed(uint32_t ip, long now, const char *peer);
void auth_succeeded(uint32_t ip);

/* port.c: what differs between TouhouOS and the Linux host build */
void vlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
long mono_ms(void);
void random_bytes(void *buf, size_t n);
int  peer_uid(int fd);                                      /* uid of the process at the other end of a UNIX socket, -1 unknown */
void vinput_open(void);
void vinput_key(uint8_t code, int value);                   /* value: 1 down, 0 up, 2 repeat */
void vinput_pointer_abs(int x, int y);
void vinput_button(int which, bool down);                   /* 0 left, 1 middle, 2 right */
void vinput_wheel(int clicks);                              /* positive: up */
void vinput_flush(void);
bool cmdline_word(const char *word);
bool cmdline_value(const char *prefix, char *out, size_t n);
int  keep_file_write(const char *path, const char *data, size_t len);   /* atomic, mode 0600 */
const char *host_name(void);

#endif
