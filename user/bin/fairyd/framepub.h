/*
 * The window server's frame service: fairyd hands a copy of the composed screen to a root process (vncd, the VNC server) through
 * a shared-memory file, with the damage since the last copy, the pointer and hints about windows that only moved.
 *
 * The messages travel on the ordinary window-server socket (/tmp/fairy/fairyd.sock) with the 16-byte FairyHdr of fairy.h. Their
 * numbers (64.. client to server, 164.. server to client) are a range fairy.h does not use, so the toolkit protocol is unchanged
 * and old clients never see them. vncd includes this file as "../fairyd/framepub.h".
 *
 *   client                                          fairyd
 *   FM_HELLO                              ->
 *                                         <-        FE_WELCOME
 *   FM_FRAME_SUBSCRIBE {version}          ->        (only a peer with uid 0; the others get FE_ERROR)
 *                                         <-        FE_FRAME_READY {shm path, sizes}
 *   FM_FRAME_REQUEST {flags}              ->        (one request outstanding at a time)
 *                                         <-        FE_FRAME {damage, moves, pointer, modes}  once something changed (FRAMEREQ_NOW: at once)
 *   FM_FRAME_REQUEST ...
 *
 * Ownership of the shared memory: fairyd writes pixels into it only between a FM_FRAME_REQUEST and the FE_FRAME that answers it;
 * from the FE_FRAME to the next request the subscriber may read it at leisure. So the subscriber never sees a half-written frame
 * and fairyd never waits for anybody: while no request is outstanding it only notes damage (a short rectangle list) and goes on
 * painting. The frame in the shared memory does NOT contain the pointer sprite (the sprite is published beside it, so that a VNC
 * viewer can draw the pointer itself); pointer movements alone cost no pixels.
 *
 * The shared memory file: FRAMEPUB_PIXELS_OFF bytes of FramePubShm header, then max_w * max_h pixels (0x00RRGGBB, stride max_w),
 * then the pointer sprite (FRAMEPUB_CURSOR_MAX x FRAMEPUB_CURSOR_MAX pixels 0xAARRGGBB, alpha 0 or 255, stride FRAMEPUB_CURSOR_MAX).
 * Coordinates are physical pixels (the ui scale is not applied: the screen as the monitor shows it).
 */
#ifndef FAIRYD_FRAMEPUB_H
#define FAIRYD_FRAMEPUB_H

#include <stdint.h>

#define FRAMEPUB_VERSION     1
#define FRAMEPUB_MAGIC       0x504D5246u            /* "FRMP" */
#define FRAMEPUB_PIXELS_OFF  4096u
#define FRAMEPUB_CURSOR_MAX  128
#define FRAMEPUB_MAX_RECTS   32
#define FRAMEPUB_MAX_MOVES   4

enum { FM_FRAME_SUBSCRIBE = 64, FM_FRAME_REQUEST = 65 };
enum { FE_FRAME_READY = 164, FE_FRAME = 165 };

#define FRAMEREQ_FULL   1u                          /* copy the whole screen this time (the first request, or after losing track) */
#define FRAMEREQ_NOW    2u                          /* answer at the end of this round even if nothing changed (a viewer asked for the
                                                       screen as it is: the answer says "your copy is current" or brings what changed).
                                                       Sent while a request is outstanding, it changes that one (no second answer). */

#define FRAME_F_FULL     1u                         /* this frame is complete: every pixel was copied */
#define FRAME_F_RESIZED  2u                         /* the screen size changed (FM_SET_MODE): the frame is complete too */
#define FRAME_F_CURSOR   4u                         /* the sprite in the shared memory is new (cursor_serial changed) */

#pragma pack(push, 1)
typedef struct { uint32_t version; } FmFrameSubscribe;
typedef struct { uint32_t flags; } FmFrameRequest;

typedef struct {
    uint32_t version;
    char     shm[64];                               /* /tmp/fairy/frame-<pid>-<n>.buf, mode 0600, owned by the window server's user */
    uint32_t shm_size;                              /* bytes */
    int32_t  max_w, max_h;                          /* the pixel area (the framebuffer's size); the screen in use may be smaller */
} FeFrameReady;

typedef struct { int32_t x, y, w, h; } FrameRect;
/* A window that only moved: the new frame's pixels in (x, y, w, h) are the previous frame's pixels at (x - dx, y - dy). The
 * subscriber should still compare before trusting it (vncd does). */
typedef struct { int32_t x, y, w, h, dx, dy; } FrameMove;

typedef struct {
    uint32_t seq;
    uint32_t flags;                                 /* FRAME_F_* */
    int32_t  w, h;                                  /* the screen in use, physical pixels */
    int32_t  scale;                                 /* the ui scale (information only) */
    int32_t  ptr_x, ptr_y;                          /* the pointer's hot spot */
    int32_t  cur_w, cur_h, cur_hotx, cur_hoty;      /* the sprite; 0 x 0 when the pointer is hidden */
    uint32_t cursor_serial;
    uint8_t  mods;                                  /* MOD_* of keys.h as the window server sees them (shift, ctrl, caps lock ...) */
    uint8_t  numlock;
    uint16_t pad;
    uint32_t clip_serial;                           /* changes when the clipboard does (FM_CLIPBOARD_GET reads it) */
    uint16_t nrects, nmoves;
    /* followed by nrects FrameRect, then nmoves FrameMove */
} FeFrame;

typedef struct {
    uint32_t magic, version;
    uint32_t pixels_off, stride;                    /* stride in pixels (= max_w) */
    int32_t  max_w, max_h;
    uint32_t cursor_off, cursor_stride;
} FramePubShm;
#pragma pack(pop)

#endif
