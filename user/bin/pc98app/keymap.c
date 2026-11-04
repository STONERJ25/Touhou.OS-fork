/* keymap: Fairies key codes to PC-98 key numbers (keymap.h). The PC-98 numbers follow the layout of the keyboard as the BIOS
 * of the HLE machine documents it (docs/PC98_BIOS.md: 00h ESC, 01h-0Dh the number row, 0Eh BS, 0Fh TAB, 10h-1Bh Q to [, 1Ch Return,
 * 1Dh-28h A to ], 29h-33h Z to _, 34h space, 35h XFER, 36h ROLL UP, 37h ROLL DOWN, 38h INS, 39h DEL, 3Ah-3Dh up, left, right, down,
 * 3Eh HOME/CLR, 3Fh HELP, 40h-50h the numeric pad, 60h STOP, 61h COPY, 62h-6Bh f.1-f.10, 70h SHIFT, 71h CAPS, 72h KANA, 73h GRPH,
 * 74h CTRL). */
#include "keymap.h"
#include <keys.h>
#include <string.h>

/* The table is indexed by the Fairies code. Entries not listed are 0 in the initialiser, so the table stores "number + 1" and
 * keymap_to_pc98() subtracts it again: a plain zero must mean "no key" and 00h (ESC) is a real PC-98 key. */
#define K(n) ((uint8_t)((n) + 1))

static const uint8_t g_map[256] = {
    [KEY_ESC] = K(0x00),
    [KEY_1] = K(0x01), [KEY_2] = K(0x02), [KEY_3] = K(0x03), [KEY_4] = K(0x04), [KEY_5] = K(0x05),
    [KEY_6] = K(0x06), [KEY_7] = K(0x07), [KEY_8] = K(0x08), [KEY_9] = K(0x09), [KEY_0] = K(0x0A),
    [KEY_MINUS] = K(0x0B),
    [KEY_EQUAL] = K(0x0C),                      /* the '^' key, right of '-' */
    [KEY_BACKSLASH] = K(0x0D),                  /* the '\' (yen) key */
    [KEY_BACKSPACE] = K(0x0E),
    [KEY_TAB] = K(0x0F),
    [KEY_Q] = K(0x10), [KEY_W] = K(0x11), [KEY_E] = K(0x12), [KEY_R] = K(0x13), [KEY_T] = K(0x14),
    [KEY_Y] = K(0x15), [KEY_U] = K(0x16), [KEY_I] = K(0x17), [KEY_O] = K(0x18), [KEY_P] = K(0x19),
    [KEY_LBRACKET] = K(0x1A),                   /* the '@' key, right of P */
    [KEY_RBRACKET] = K(0x1B),                   /* the '[' key */
    [KEY_ENTER] = K(0x1C), [KEY_KPENTER] = K(0x1C),
    [KEY_A] = K(0x1D), [KEY_S] = K(0x1E), [KEY_D] = K(0x1F), [KEY_F] = K(0x20), [KEY_G] = K(0x21),
    [KEY_H] = K(0x22), [KEY_J] = K(0x23), [KEY_K] = K(0x24), [KEY_L] = K(0x25),
    [KEY_SEMICOLON] = K(0x26),
    [KEY_APOSTROPHE] = K(0x27),                 /* the ':' key */
    [KEY_Z] = K(0x29), [KEY_X] = K(0x2A), [KEY_C] = K(0x2B), [KEY_V] = K(0x2C), [KEY_B] = K(0x2D),
    [KEY_N] = K(0x2E), [KEY_M] = K(0x2F),
    [KEY_COMMA] = K(0x30), [KEY_DOT] = K(0x31), [KEY_SLASH] = K(0x32),
    [KEY_SPACE] = K(0x34),
    [KEY_PAGEUP] = K(0x36),                     /* ROLL UP */
    [KEY_PAGEDOWN] = K(0x37),                   /* ROLL DOWN */
    [KEY_INSERT] = K(0x38), [KEY_DELETE] = K(0x39),
    [KEY_UP] = K(0x3A), [KEY_LEFT] = K(0x3B), [KEY_RIGHT] = K(0x3C), [KEY_DOWN] = K(0x3D),
    [KEY_HOME] = K(0x3E),                       /* HOME/CLR */
    [KEY_END] = K(0x3F),                        /* HELP */
    [KEY_KPMINUS] = K(0x40), [KEY_KPSLASH] = K(0x41),
    [KEY_KP7] = K(0x42), [KEY_KP8] = K(0x43), [KEY_KP9] = K(0x44), [KEY_KPSTAR] = K(0x45),
    [KEY_KP4] = K(0x46), [KEY_KP5] = K(0x47), [KEY_KP6] = K(0x48), [KEY_KPPLUS] = K(0x49),
    [KEY_KP1] = K(0x4A), [KEY_KP2] = K(0x4B), [KEY_KP3] = K(0x4C),
    [KEY_KP0] = K(0x4E), [KEY_KPDOT] = K(0x50),
    [KEY_SCROLLLOCK] = K(0x60),                 /* STOP: raises INT 06h in the BIOS and breaks most programs */
    [KEY_F1] = K(0x62), [KEY_F2] = K(0x63), [KEY_F3] = K(0x64), [KEY_F4] = K(0x65), [KEY_F5] = K(0x66),
    [KEY_F6] = K(0x67), [KEY_F7] = K(0x68), [KEY_F8] = K(0x69), [KEY_F9] = K(0x6A), [KEY_F10] = K(0x6B),
    [KEY_LSHIFT] = K(0x70), [KEY_RSHIFT] = K(0x70),
    [KEY_LALT] = K(0x73), [KEY_RALT] = K(0x73),                 /* GRPH */
    [KEY_LCTRL] = K(0x74), [KEY_RCTRL] = K(0x74),
    /* Not mapped: CapsLock and the KANA key are locking keys on the PC-98 (the keyboard reports the lock, not the tap), F11 and F12
     * (F12 is the emulator's menu), the Windows and menu keys, the grave accent, the keypad's = and , keys. */
};

uint8_t keymap_to_pc98(uint8_t fairy_code) {
    uint8_t v = g_map[fairy_code];
    return v ? (uint8_t)(v - 1) : PC98K_NONE;
}

void keys_init(Keys *k) { memset(k, 0, sizeof *k); }

int keys_event(Keys *k, uint8_t fairy_code, int down, KeysSink sink, void *user) {
    uint8_t p = keymap_to_pc98(fairy_code);
    if (p == PC98K_NONE) return 0;
    uint8_t bit = (uint8_t)(1u << (fairy_code & 7)), *held = &k->host_down[fairy_code >> 3];
    if (down == 1) {
        if (*held & bit) return 1;              /* a press the guest already has (a repeat that came as a press) */
        *held |= bit;
        if (k->refs[p]++ == 0 && sink) sink(user, p, 1);
    } else if (down == 0) {
        if (!(*held & bit)) return 1;           /* its press was never seen (it was held before the window had the focus) */
        *held &= (uint8_t)~bit;
        if (k->refs[p] && --k->refs[p] == 0 && sink) sink(user, p, 0);
    }
    return 1;                                   /* down == 2: the host's auto-repeat, dropped on purpose */
}

void keys_release_all(Keys *k, KeysSink sink, void *user) {
    for (unsigned p = 0; p < sizeof k->refs; p++)
        if (k->refs[p] && sink) sink(user, (uint8_t)p, 0);
    memset(k, 0, sizeof *k);
}

int keys_held(const Keys *k) {
    int n = 0;
    for (unsigned p = 0; p < sizeof k->refs; p++) n += k->refs[p] != 0;
    return n;
}
