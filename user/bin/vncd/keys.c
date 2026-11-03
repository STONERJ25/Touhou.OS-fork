/*
 * X11 keysyms (what RFB's KeyEvent carries, RFC 6143 7.5.4) to the system's key codes (shared/keys.h: PS/2 set 1, bit 7 for the
 * E0-prefixed keys). The window server turns key codes into characters with the US layout (fairy_key_char), so a printable
 * keysym becomes the US key that types it, together with the shift that key needs: client.c then presses or lifts shift for
 * that one key when the viewer's shift state says otherwise (a viewer with a German keyboard sends Shift+'/' as Shift_L and
 * the keysym '/', which on the US layout is the unshifted slash key).
 *
 * Unicode keysyms (0x01000000 + code point) are taken as far as the US layout reaches (ASCII); other characters have no key
 * here and are dropped (client.c says so once per viewer).
 */
#include "vncd.h"
#include <keys.h>

typedef struct { uint8_t code, shift; } KeyChar;

/* ASCII 0x20 .. 0x7E on the US layout */
static const KeyChar g_ascii[95] = {
    { KEY_SPACE, 0 }, { KEY_1, 1 }, { KEY_APOSTROPHE, 1 }, { KEY_3, 1 }, { KEY_4, 1 }, { KEY_5, 1 }, { KEY_7, 1 }, { KEY_APOSTROPHE, 0 },
    { KEY_9, 1 }, { KEY_0, 1 }, { KEY_8, 1 }, { KEY_EQUAL, 1 }, { KEY_COMMA, 0 }, { KEY_MINUS, 0 }, { KEY_DOT, 0 }, { KEY_SLASH, 0 },
    { KEY_0, 0 }, { KEY_1, 0 }, { KEY_2, 0 }, { KEY_3, 0 }, { KEY_4, 0 }, { KEY_5, 0 }, { KEY_6, 0 }, { KEY_7, 0 }, { KEY_8, 0 }, { KEY_9, 0 },
    { KEY_SEMICOLON, 1 }, { KEY_SEMICOLON, 0 }, { KEY_COMMA, 1 }, { KEY_EQUAL, 0 }, { KEY_DOT, 1 }, { KEY_SLASH, 1 }, { KEY_2, 1 },
    { KEY_A, 1 }, { KEY_B, 1 }, { KEY_C, 1 }, { KEY_D, 1 }, { KEY_E, 1 }, { KEY_F, 1 }, { KEY_G, 1 }, { KEY_H, 1 }, { KEY_I, 1 },
    { KEY_J, 1 }, { KEY_K, 1 }, { KEY_L, 1 }, { KEY_M, 1 }, { KEY_N, 1 }, { KEY_O, 1 }, { KEY_P, 1 }, { KEY_Q, 1 }, { KEY_R, 1 },
    { KEY_S, 1 }, { KEY_T, 1 }, { KEY_U, 1 }, { KEY_V, 1 }, { KEY_W, 1 }, { KEY_X, 1 }, { KEY_Y, 1 }, { KEY_Z, 1 },
    { KEY_LBRACKET, 0 }, { KEY_BACKSLASH, 0 }, { KEY_RBRACKET, 0 }, { KEY_6, 1 }, { KEY_MINUS, 1 }, { KEY_GRAVE, 0 },
    { KEY_A, 0 }, { KEY_B, 0 }, { KEY_C, 0 }, { KEY_D, 0 }, { KEY_E, 0 }, { KEY_F, 0 }, { KEY_G, 0 }, { KEY_H, 0 }, { KEY_I, 0 },
    { KEY_J, 0 }, { KEY_K, 0 }, { KEY_L, 0 }, { KEY_M, 0 }, { KEY_N, 0 }, { KEY_O, 0 }, { KEY_P, 0 }, { KEY_Q, 0 }, { KEY_R, 0 },
    { KEY_S, 0 }, { KEY_T, 0 }, { KEY_U, 0 }, { KEY_V, 0 }, { KEY_W, 0 }, { KEY_X, 0 }, { KEY_Y, 0 }, { KEY_Z, 0 },
    { KEY_LBRACKET, 1 }, { KEY_BACKSLASH, 1 }, { KEY_RBRACKET, 1 }, { KEY_GRAVE, 1 },
};

typedef struct { uint16_t sym; uint8_t code; } SymKey;

/* The function keys, the keypad and the modifiers (keysyms 0xFExx and 0xFFxx). */
static const SymKey g_special[] = {
    { 0xFF08, KEY_BACKSPACE }, { 0xFF09, KEY_TAB }, { 0xFE20, KEY_TAB } /* ISO_Left_Tab: Shift+Tab */, { 0xFF0D, KEY_ENTER },
    { 0xFF1B, KEY_ESC }, { 0xFFFF, KEY_DELETE }, { 0xFF63, KEY_INSERT }, { 0xFF67, KEY_MENU },
    { 0xFF50, KEY_HOME }, { 0xFF51, KEY_LEFT }, { 0xFF52, KEY_UP }, { 0xFF53, KEY_RIGHT }, { 0xFF54, KEY_DOWN },
    { 0xFF55, KEY_PAGEUP }, { 0xFF56, KEY_PAGEDOWN }, { 0xFF57, KEY_END },
    { 0xFFBE, KEY_F1 }, { 0xFFBF, KEY_F2 }, { 0xFFC0, KEY_F3 }, { 0xFFC1, KEY_F4 }, { 0xFFC2, KEY_F5 }, { 0xFFC3, KEY_F6 },
    { 0xFFC4, KEY_F7 }, { 0xFFC5, KEY_F8 }, { 0xFFC6, KEY_F9 }, { 0xFFC7, KEY_F10 }, { 0xFFC8, KEY_F11 }, { 0xFFC9, KEY_F12 },
    { 0xFFE1, KEY_LSHIFT }, { 0xFFE2, KEY_RSHIFT }, { 0xFFE3, KEY_LCTRL }, { 0xFFE4, KEY_RCTRL }, { 0xFFE5, KEY_CAPSLOCK },
    { 0xFFE7, KEY_LALT } /* Meta_L */, { 0xFFE8, KEY_RALT } /* Meta_R */, { 0xFFE9, KEY_LALT }, { 0xFFEA, KEY_RALT },
    { 0xFFEB, KEY_LSUPER }, { 0xFFEC, KEY_RSUPER }, { 0xFE03, KEY_RALT } /* ISO_Level3_Shift (AltGr) */, { 0xFF7E, KEY_RALT } /* Mode_switch */,
    { 0xFF7F, KEY_NUMLOCK }, { 0xFF14, KEY_SCROLLLOCK },
    { 0xFF8D, KEY_KPENTER }, { 0xFFAA, KEY_KPSTAR }, { 0xFFAB, KEY_KPPLUS }, { 0xFFAD, KEY_KPMINUS }, { 0xFFAE, KEY_KPDOT },
    { 0xFFAC, KEY_KPDOT } /* KP_Separator */, { 0xFFAF, KEY_KPSLASH },
    { 0xFFB0, KEY_KP0 }, { 0xFFB1, KEY_KP1 }, { 0xFFB2, KEY_KP2 }, { 0xFFB3, KEY_KP3 }, { 0xFFB4, KEY_KP4 },
    { 0xFFB5, KEY_KP5 }, { 0xFFB6, KEY_KP6 }, { 0xFFB7, KEY_KP7 }, { 0xFFB8, KEY_KP8 }, { 0xFFB9, KEY_KP9 },
    /* the keypad with the viewer's Num Lock off: the keys they stand for, whatever the server's Num Lock says */
    { 0xFF95, KEY_HOME }, { 0xFF96, KEY_LEFT }, { 0xFF97, KEY_UP }, { 0xFF98, KEY_RIGHT }, { 0xFF99, KEY_DOWN },
    { 0xFF9A, KEY_PAGEUP }, { 0xFF9B, KEY_PAGEDOWN }, { 0xFF9C, KEY_END }, { 0xFF9E, KEY_INSERT }, { 0xFF9F, KEY_DELETE },
    { 0xFF80, KEY_SPACE } /* KP_Space */, { 0xFF89, KEY_TAB } /* KP_Tab */,
};

int keysym_to_code(uint32_t keysym, int *want_shift, bool *is_letter) {
    *want_shift = -1;
    *is_letter = false;
    uint32_t ch = 0;
    if (keysym >= 0x20 && keysym <= 0x7E) ch = keysym;                         /* Latin-1's ASCII half */
    else if (keysym >= 0x01000020u && keysym <= 0x0100007Eu) ch = keysym - 0x01000000u;   /* Unicode keysyms of ASCII characters */
    if (ch) {
        const KeyChar *k = &g_ascii[ch - 0x20];
        *want_shift = k->shift;
        *is_letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        return k->code;
    }
    if (keysym == 0xFFBD) { *want_shift = 0; return KEY_EQUAL; }              /* KP_Equal: the keypad of the system has none */
    if (keysym >= 0xFE00 && keysym <= 0xFFFF)
        for (size_t i = 0; i < sizeof g_special / sizeof g_special[0]; i++)
            if (g_special[i].sym == keysym) return g_special[i].code;
    return 0;
}
