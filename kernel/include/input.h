/* Input subsystem: PS/2 keyboard and mouse -> console TTY bytes and/or GUI event queue. */
#ifndef K_INPUT_H
#define K_INPUT_H

#include "types.h"
#include "task.h"
#include "abi.h"
#include "keys.h"

#define INPUT_QUEUE_LEN 512

typedef struct InputQueue {
    struct ginput_event ev[INPUT_QUEUE_LEN];
    u32 head, tail;                 /* head: next slot to write, tail: next to read */
    WaitQueue wq;
    u32 dropped;
} InputQueue;

extern InputQueue g_input;
extern volatile int g_input_readers;    /* open handles on /dev/input/events: events are queued only if > 0 */
extern volatile int g_input_grab;       /* > 0: keyboard goes to the GUI queue, not the console tty */

void input_init(void);
void input_emit(u8 type, u16 code, s32 value);          /* IRQ context */
bool input_pop(struct ginput_event *out);

void keyboard_init(void);               /* sets up the i8042 controller, keyboard and mouse, unmasks IRQ1/IRQ12 */
u8   keyboard_modifiers(void);          /* MOD_* bits */

/* Entry points for non-PS/2 devices (USB HID). They behave exactly like the i8042 path: console tty bytes or GUI events. */
void input_key(u8 code, bool down);                                  /* code: KEY_* from keys.h; repeat is generated here */
void input_mouse(int dx, int dy, int wheel, u8 buttons);             /* dy > 0 moves down; buttons: absolute state */
void input_tick(void);                                               /* 1 kHz, from the timer interrupt (software key repeat) */

/* Mouse state, for in-kernel consumers and the window server */
typedef struct {
    int x, y;                           /* absolute position clamped to the screen */
    u8  buttons;                        /* bit0 left, bit1 right, bit2 middle */
} MouseState;
extern MouseState g_mouse;
void mouse_set_bounds(int w, int h);

#endif
