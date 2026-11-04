/*
 * keymap: the keyboard of the PC-98 application. Fairies key codes (shared/keys.h: the PS/2 set-1 scancode, bit 7 set for the
 * extended keys) become PC-98 key numbers (the numbers of the key matrix that the uPD8251 sends and the BIOS keeps in its key
 * status table: 00h ESC, 01h-0Ah the digits, 10h-19h Q to P, 1Ch Return, 29h-2Fh Z to M, 34h space, 3Ah-3Dh the cursor
 * keys, 62h-6Bh f.1 to f.10, 70h SHIFT, 73h GRPH, 74h CTRL ...: docs/PC98_BIOS.md and docs/PC98_APP.md have the table).
 *
 * The mapping is by position on a US keyboard, as a game wants it (Z and X are shoot and bomb on both machines, the arrow keys
 * are the arrow keys). Punctuation lands on the key that sits there on a Japanese keyboard, so a US '[' types '@' in the guest:
 * the letters and digits, which are what the games and most DOS commands use, are the same everywhere.
 *
 * Keys() keeps the host's idea of what is held in step with what the guest was told, so that no key is ever left down in the
 * guest: two host keys that map to one PC-98 key (the two Shift keys) release it only when both are up, a key whose press
 * the guest never saw does not send a release, and keys_release_all() (focus lost, a menu opened, a reset) lets go of
 * everything at once. The host's auto-repeat (down = 2) is ignored: the PC-98 keyboard repeats by itself and the guest's own
 * key handling (the games poll the key status table) decides what a held key means.
 *
 * Pure C with no dependencies, so the host tests (tests/host/test_pc98app.py) compile it as it is.
 */
#ifndef PC98APP_KEYMAP_H
#define PC98APP_KEYMAP_H

#include <stdint.h>

#define PC98K_NONE 0xFF         /* a host key the PC-98 has no counterpart for */

/* The PC-98 key number for a Fairies key code, or PC98K_NONE. */
uint8_t keymap_to_pc98(uint8_t fairy_code);

typedef void (*KeysSink)(void *user, uint8_t pc98_key, int down);

typedef struct {
    uint8_t host_down[32];      /* bitmap by Fairies code: the host key is held (and was taken) */
    uint8_t refs[128];          /* host keys currently holding each PC-98 key down */
} Keys;

void keys_init(Keys *k);
/* One host key event: down = 1 press, 0 release, 2 auto-repeat (ignored). Calls the sink with make (1) and break (0) events for
 * the guest. Returns 1 when the key maps to a PC-98 key (it was the guest's), 0 when it does not (the caller may use it). */
int keys_event(Keys *k, uint8_t fairy_code, int down, KeysSink sink, void *user);
/* Releases every key the guest believes is down. */
void keys_release_all(Keys *k, KeysSink sink, void *user);
/* Number of PC-98 keys held down. */
int keys_held(const Keys *k);

#endif
