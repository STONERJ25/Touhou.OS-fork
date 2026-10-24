/* i8042 PS/2 controller: keyboard (scancode set 1 via controller translation) and mouse (3/4-byte packets). */
#include "input.h"
#include "kernel.h"
#include "cpu.h"
#include "irq.h"
#include "tty.h"

#define KBD_DATA   0x60
#define KBD_STATUS 0x64
#define KBD_CMD    0x64

InputQueue g_input;
volatile int g_input_readers;
volatile int g_input_grab;
MouseState g_mouse;

static int g_screen_w = 1024, g_screen_h = 768;
static u8 g_mods;                       /* MOD_* */
static bool g_numlock = true;
static bool g_e0;
static int g_skip;                      /* bytes still to swallow after an E1 (Pause) prefix */
static u8 g_down[32];                   /* pressed-key bitmap, 256 codes */

/* ---------------------------------------------------------------- input queue */

void input_init(void) {
    memset(&g_input, 0, sizeof g_input);
    wq_init(&g_input.wq);
}

void input_emit(u8 type, u16 code, s32 value) {
    if (g_input_readers <= 0) return;
    InputQueue *q = &g_input;
    u32 next = (q->head + 1) % INPUT_QUEUE_LEN;
    if (next == q->tail) {              /* full: drop the oldest */
        q->tail = (q->tail + 1) % INPUT_QUEUE_LEN;
        q->dropped++;
    }
    struct ginput_event *e = &q->ev[q->head];
    e->type = type;
    e->pad = 0;
    e->code = code;
    e->value = value;
    e->time_ms = uptime_ms();
    q->head = next;
    wq_wake_all(&q->wq);
}

bool input_pop(struct ginput_event *out) {
    u64 f = irq_save();
    bool ok = g_input.head != g_input.tail;
    if (ok) {
        *out = g_input.ev[g_input.tail];
        g_input.tail = (g_input.tail + 1) % INPUT_QUEUE_LEN;
    }
    irq_restore(f);
    return ok;
}

/* ---------------------------------------------------------------- keymap (US) */

#define SC(n, a, b) [n] = { a, b }
static const struct { char base, shift; } g_keymap[0x59] = {
    SC(0x01, 27, 27), SC(0x02, '1', '!'), SC(0x03, '2', '@'), SC(0x04, '3', '#'), SC(0x05, '4', '$'),
    SC(0x06, '5', '%'), SC(0x07, '6', '^'), SC(0x08, '7', '&'), SC(0x09, '8', '*'), SC(0x0A, '9', '('),
    SC(0x0B, '0', ')'), SC(0x0C, '-', '_'), SC(0x0D, '=', '+'), SC(0x0E, 127, 127), SC(0x0F, '\t', '\t'),
    SC(0x10, 'q', 'Q'), SC(0x11, 'w', 'W'), SC(0x12, 'e', 'E'), SC(0x13, 'r', 'R'), SC(0x14, 't', 'T'),
    SC(0x15, 'y', 'Y'), SC(0x16, 'u', 'U'), SC(0x17, 'i', 'I'), SC(0x18, 'o', 'O'), SC(0x19, 'p', 'P'),
    SC(0x1A, '[', '{'), SC(0x1B, ']', '}'), SC(0x1C, '\r', '\r'),
    SC(0x1E, 'a', 'A'), SC(0x1F, 's', 'S'), SC(0x20, 'd', 'D'), SC(0x21, 'f', 'F'), SC(0x22, 'g', 'G'),
    SC(0x23, 'h', 'H'), SC(0x24, 'j', 'J'), SC(0x25, 'k', 'K'), SC(0x26, 'l', 'L'), SC(0x27, ';', ':'),
    SC(0x28, '\'', '"'), SC(0x29, '`', '~'), SC(0x2B, '\\', '|'),
    SC(0x2C, 'z', 'Z'), SC(0x2D, 'x', 'X'), SC(0x2E, 'c', 'C'), SC(0x2F, 'v', 'V'), SC(0x30, 'b', 'B'),
    SC(0x31, 'n', 'N'), SC(0x32, 'm', 'M'), SC(0x33, ',', '<'), SC(0x34, '.', '>'), SC(0x35, '/', '?'),
    SC(0x37, '*', '*'), SC(0x39, ' ', ' '), SC(0x4A, '-', '-'), SC(0x4E, '+', '+'),
};

static int put_str(u8 *out, const char *s) {
    int n = 0;
    while (*s) out[n++] = (u8)*s++;
    return n;
}

/* Bytes the console TTY should receive for a key press. Returns the count (0 = nothing to send). */
static int key_to_bytes(u8 code, u8 mods, u8 *out) {
    bool shift = mods & MOD_SHIFT, ctrl = mods & MOD_CTRL, alt = mods & MOD_ALT, caps = mods & MOD_CAPS;
    int n = 0;
    if (alt) out[n++] = 0x1B;

    if (code & KEY_EXT) {
        switch (code) {
        case KEY_UP:       return n + put_str(out + n, "\x1b[A");
        case KEY_DOWN:     return n + put_str(out + n, "\x1b[B");
        case KEY_RIGHT:    return n + put_str(out + n, "\x1b[C");
        case KEY_LEFT:     return n + put_str(out + n, "\x1b[D");
        case KEY_HOME:     return n + put_str(out + n, "\x1b[H");
        case KEY_END:      return n + put_str(out + n, "\x1b[F");
        case KEY_INSERT:   return n + put_str(out + n, "\x1b[2~");
        case KEY_DELETE:   return n + put_str(out + n, "\x1b[3~");
        case KEY_PAGEUP:   return n + put_str(out + n, "\x1b[5~");
        case KEY_PAGEDOWN: return n + put_str(out + n, "\x1b[6~");
        case KEY_KPENTER:  out[n++] = '\r'; return n;
        case KEY_KPSLASH:  out[n++] = '/'; return n;
        default: return 0;
        }
    }

    if (code >= KEY_F1 && code <= KEY_F10) {
        static const char *const f[] = { "\x1bOP", "\x1bOQ", "\x1bOR", "\x1bOS", "\x1b[15~", "\x1b[17~", "\x1b[18~", "\x1b[19~", "\x1b[20~", "\x1b[21~" };
        return put_str(out, f[code - KEY_F1]);
    }
    if (code == KEY_F11) return put_str(out, "\x1b[23~");
    if (code == KEY_F12) return put_str(out, "\x1b[24~");

    if (code >= KEY_KP7 && code <= KEY_KPDOT && code != KEY_KPMINUS && code != KEY_KPPLUS) {
        static const char kp_digit[] = { '7', '8', '9', 0, '4', '5', '6', 0, '1', '2', '3', '0', '.' };
        static const char *const kp_nav[] = { "\x1b[H", "\x1b[A", "\x1b[5~", NULL, "\x1b[D", "\x1b[E", "\x1b[C", NULL,
                                              "\x1b[F", "\x1b[B", "\x1b[6~", "\x1b[2~", "\x1b[3~" };
        int idx = code - KEY_KP7;
        if (g_numlock) { out[n++] = (u8)kp_digit[idx]; return n; }
        return n + put_str(out + n, kp_nav[idx] ? kp_nav[idx] : "");
    }

    if (code >= ARRAY_SIZE(g_keymap)) return 0;
    char c = shift ? g_keymap[code].shift : g_keymap[code].base;
    if (!c) return 0;
    bool letter = g_keymap[code].base >= 'a' && g_keymap[code].base <= 'z';
    if (letter && caps) c = shift ? g_keymap[code].base : g_keymap[code].shift;
    if (ctrl) {
        char b = g_keymap[code].base;
        if (b >= 'a' && b <= 'z') c = (char)(b - 'a' + 1);
        else if (b == '[') c = 0x1B;
        else if (b == '\\') c = 0x1C;
        else if (b == ']') c = 0x1D;
        else if (b == ' ') c = 0;
        else if (b == '2') c = 0;
        else if (b == '/') c = 0x1F;
    }
    out[n++] = (u8)c;
    return n;
}

u8 keyboard_modifiers(void) { return g_mods; }

static void set_mod(u8 bit, bool down) {
    if (down) g_mods |= bit;
    else g_mods &= (u8)~bit;
}

static void key_event(u8 code, bool down, bool sw_repeat);

static void kbd_byte(u8 sc) {
    if (g_skip) { g_skip--; return; }
    if (sc == 0xE0) { g_e0 = true; return; }
    if (sc == 0xE1) { g_skip = 5; return; }         /* Pause/Break: E1 1D 45 E1 9D C5 */
    if (sc == 0xFA || sc == 0xFE) return;           /* ack / resend (0xAA is also the left-shift release code: keep it) */

    bool up = sc & 0x80;
    u8 code = sc & 0x7F;
    if (g_e0) {
        g_e0 = false;
        if (code == 0x2A || code == 0x36) return;   /* fake shifts around Insert/Delete/arrows on old keyboards */
        code |= KEY_EXT;
    }

    key_event(code, !up, false);                    /* PS/2 keyboards repeat in hardware */
}

/* ---------------------------------------------------------------- source independent key path */

static u8  g_rep_code;                  /* key being repeated by software (USB keyboards do not repeat themselves) */
static u64 g_rep_next_ms;

static bool is_modifier_key(u8 code) {
    switch (code) {
    case KEY_LSHIFT: case KEY_RSHIFT: case KEY_LCTRL: case KEY_RCTRL: case KEY_LALT: case KEY_RALT:
    case KEY_LSUPER: case KEY_RSUPER: case KEY_CAPSLOCK: case KEY_NUMLOCK: case KEY_SCROLLLOCK:
        return true;
    default:
        return false;
    }
}

/* A key went down or up. `sw_repeat`: this source needs typematic repeat generated here. */
static void key_event(u8 code, bool down, bool sw_repeat) {
    bool was_down = g_down[code >> 3] & (1 << (code & 7));
    if (!down) g_down[code >> 3] &= (u8)~(1 << (code & 7));
    else g_down[code >> 3] |= (u8)(1 << (code & 7));

    switch (code) {
    case KEY_LSHIFT: case KEY_RSHIFT: set_mod(MOD_SHIFT, down); break;
    case KEY_LCTRL:  case KEY_RCTRL:  set_mod(MOD_CTRL, down); break;
    case KEY_LALT:   case KEY_RALT:   set_mod(MOD_ALT, down); break;
    case KEY_LSUPER: case KEY_RSUPER: set_mod(MOD_SUPER, down); break;
    case KEY_CAPSLOCK: if (down && !was_down) g_mods ^= MOD_CAPS; break;
    case KEY_NUMLOCK:  if (down && !was_down) g_numlock = !g_numlock; break;
    default: break;
    }

    if (down && code == KEY_BACKSPACE && (g_mods & MOD_CTRL) && (g_mods & MOD_ALT) && g_input_grab > 0) {
        input_kill_grabber();                           /* escape hatch when the window server hangs */
        return;
    }

    if (sw_repeat) {
        if (down && !was_down && !is_modifier_key(code)) { g_rep_code = code; g_rep_next_ms = uptime_ms() + 500; }
        else if (!down && code == g_rep_code) g_rep_code = 0;
    }

    int value = !down ? 0 : (was_down ? 2 : 1);
    if (g_input_grab > 0 || g_input_readers > 0) input_emit(EV_KEY, code, value);
    if (g_input_grab > 0 || !down) return;

    u8 bytes[8];
    int n = key_to_bytes(code, g_mods, bytes);
    if (n > 0) tty_receive(tty_console(), bytes, (size_t)n);
}

/* For USB (and any other non-PS/2) keyboards: KEY_* code plus pressed/released. Repeat is synthesised from the timer. */
void input_key(u8 code, bool down) { key_event(code, down, true); }

/* Called from the 1 kHz timer interrupt. */
void input_tick(void) {
    if (!g_rep_code) return;
    u64 now = uptime_ms();
    if (now < g_rep_next_ms) return;
    g_rep_next_ms = now + 33;
    key_event(g_rep_code, true, true);
}

/* ---------------------------------------------------------------- mouse */

static u8 g_mpkt[4];
static int g_mpos;
static bool g_mouse_wheel;

void mouse_set_bounds(int w, int h) {
    g_screen_w = w;
    g_screen_h = h;
    if (g_mouse.x >= w) g_mouse.x = w - 1;
    if (g_mouse.y >= h) g_mouse.y = h - 1;
}

static void mouse_byte(u8 b) {
    if (g_mpos == 0 && !(b & 0x08)) return;     /* out of sync: first byte always has bit 3 set */
    g_mpkt[g_mpos++] = b;
    if (g_mpos < (g_mouse_wheel ? 4 : 3)) return;
    g_mpos = 0;

    u8 flags = g_mpkt[0];
    if (flags & 0xC0) return;                   /* overflow: drop the packet */
    int dx = (int)g_mpkt[1] - ((flags & 0x10) ? 256 : 0);
    int dy = (int)g_mpkt[2] - ((flags & 0x20) ? 256 : 0);
    int wheel = 0;
    if (g_mouse_wheel) {
        wheel = g_mpkt[3] & 0x0F;
        if (wheel & 8) wheel -= 16;
        wheel = -wheel;                         /* PS/2 counts a turn towards the user as positive; the input layer wants up = positive */
    }
    dy = -dy;                                   /* PS/2 reports up as positive */
    input_mouse(dx, dy, wheel, flags & 7);
}

/* Movement since the last report (dy positive = down), wheel (positive = up) and the absolute button state
 * (bit0 left, bit1 right, bit2 middle). Used by the PS/2 decoder above and by USB HID mice. */
void input_mouse(int dx, int dy, int wheel, u8 buttons) {
    g_mouse.x += dx;
    g_mouse.y += dy;
    if (g_mouse.x < 0) g_mouse.x = 0;
    if (g_mouse.y < 0) g_mouse.y = 0;
    if (g_mouse.x >= g_screen_w) g_mouse.x = g_screen_w - 1;
    if (g_mouse.y >= g_screen_h) g_mouse.y = g_screen_h - 1;

    if (g_input_readers > 0) {
        if (dx) input_emit(EV_REL, REL_X, dx);
        if (dy) input_emit(EV_REL, REL_Y, dy);
        if (wheel) input_emit(EV_REL, REL_WHEEL, wheel);
        u8 changed = (buttons ^ g_mouse.buttons) & 7;
        static const u16 btn[3] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
        for (int i = 0; i < 3; i++)
            if (changed & (1 << i)) input_emit(EV_KEY, btn[i], (buttons >> i) & 1);
    }
    g_mouse.buttons = buttons & 7;
}

/* ---------------------------------------------------------------- controller */

/* A real i8042 answers within a millisecond or two. These waits are bounded in TIME (up to 25 ms), not in port reads: on a PC
 * that does have the controller but has no mouse plugged in, every mouse command times out, and a count of 100000 slow I/O
 * reads each adds up to seconds of black screen at boot. */
#define KBD_WAIT_US 25000

static int wait_write(void) {
    for (int i = 0; i < 20; i++) if (!(inb(KBD_STATUS) & 2)) return 0;
    for (u32 t = 0; t < KBD_WAIT_US; t += 50) {
        if (!(inb(KBD_STATUS) & 2)) return 0;
        udelay(50);
    }
    return -1;
}

static int wait_read(void) {
    for (int i = 0; i < 20; i++) if (inb(KBD_STATUS) & 1) return 0;
    for (u32 t = 0; t < KBD_WAIT_US; t += 50) {
        if (inb(KBD_STATUS) & 1) return 0;
        udelay(50);
    }
    return -1;
}

static void ctl_cmd(u8 c) { wait_write(); outb(KBD_CMD, c); }
static void ctl_data(u8 d) { wait_write(); outb(KBD_DATA, d); }
static int  ctl_read(void) { return wait_read() ? -1 : inb(KBD_DATA); }

static int mouse_cmd(u8 c) {
    ctl_cmd(0xD4);
    ctl_data(c);
    return ctl_read();                          /* 0xFA = ack */
}

static void i8042_irq(int irq, void *ctx) {
    (void)irq; (void)ctx;
    for (int guard = 0; guard < 64; guard++) {
        u8 st = inb(KBD_STATUS);
        if (!(st & 1)) break;
        u8 data = inb(KBD_DATA);
        if (st & 0x20) mouse_byte(data);
        else kbd_byte(data);
    }
}

void keyboard_init(void) {
    g_mouse.x = g_screen_w / 2;
    g_mouse.y = g_screen_h / 2;
    if (inb(KBD_STATUS) == 0xFF) {              /* floating bus: no i8042 here (USB-only machine) */
        klog("Satori", "no PS/2 controller found; keyboard and mouse must come from USB");
        return;
    }
    /* A status register that does not look like a controller, or a controller that never answers the first command, means there is
     * nothing to talk to (a PC that dropped the legacy ports, or firmware that emulates them through USB and is not in the mood).
     * Anything else would cost 25 ms per unanswered command, and there are dozens of them below. */
    if (wait_write() < 0) {
        klog("Satori", "the PS/2 controller is stuck busy (status %#x); keyboard and mouse must come from USB", inb(KBD_STATUS));
        return;
    }
    ctl_cmd(0xAD);                              /* disable both ports while configuring */
    ctl_cmd(0xA7);
    for (int i = 0; i < 64 && (inb(KBD_STATUS) & 1); i++) inb(KBD_DATA);  /* flush */

    ctl_cmd(0x20);
    int cfg = ctl_read();
    if (cfg < 0) {
        klog("Satori", "the PS/2 controller does not answer (status %#x); keyboard and mouse must come from USB", inb(KBD_STATUS));
        return;
    }
    cfg |= 0x01 | 0x02 | 0x40;                  /* IRQ1, IRQ12, scancode translation to set 1 */
    cfg &= ~(0x10 | 0x20);                      /* clocks enabled */
    ctl_cmd(0x60);
    ctl_data((u8)cfg);

    ctl_cmd(0xAE);
    ctl_cmd(0xA8);

    ctl_data(0xF4);                             /* keyboard: enable scanning */
    bool have_kbd = ctl_read() == 0xFA;

    /* Mouse: defaults, then the "magic" sample-rate sequence that switches an IntelliMouse into wheel mode. The first command is the
     * probe: no acknowledgement means nothing is plugged in and the rest would only time out one by one. */
    bool have_mouse = mouse_cmd(0xF6) == 0xFA;
    if (have_mouse) {
        mouse_cmd(0xF3); mouse_cmd(200);
        mouse_cmd(0xF3); mouse_cmd(100);
        mouse_cmd(0xF3); mouse_cmd(80);
        if (mouse_cmd(0xF2) == 0xFA) g_mouse_wheel = ctl_read() == 3;
        mouse_cmd(0xF3); mouse_cmd(100);
        mouse_cmd(0xF4);                        /* enable streaming */
    }

    g_mouse.x = g_screen_w / 2;
    g_mouse.y = g_screen_h / 2;

    if (!have_kbd && !have_mouse) {
        ctl_cmd(0xAD);                          /* nothing is attached: leave both ports off so that they cannot interrupt */
        ctl_cmd(0xA7);
        klog("Satori", "a PS/2 controller is there but neither a keyboard nor a mouse answered; USB it is");
        return;
    }
    if (have_kbd) { irq_register(1, i8042_irq, NULL); irq_unmask(1); }
    if (have_mouse) { irq_register(12, i8042_irq, NULL); irq_unmask(12); }
    if (have_kbd) klog("Satori", "keyboard online: reading your mind (and your keystrokes)");
    else klog("Satori", "the PS/2 controller has no keyboard on it");
    if (have_mouse) klog("Koishi", "mouse online%s: movement goes unnoticed", g_mouse_wheel ? " with wheel" : "");
    else klog("Koishi", "the PS/2 controller has no mouse on it");
}
