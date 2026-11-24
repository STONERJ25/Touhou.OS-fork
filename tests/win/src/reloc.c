/* Absolute addresses in data (a table of strings and of functions: DIR64 base relocations). Built twice: at 0x140000000, where winrun maps it as it is,
 * and at 0x400000, where winrun's own program lives, so that winrun has to move it and apply the relocations. */
#include "win.h"

static const char *const words[] = { "reloc", "table", "ok\n" };
static void say(const char *s) { out(s); }
static void space(const char *s) { (void)s; out(" "); }
static void (*const fns[])(const char *) = { say, space, say, space, say };

extern char __ImageBase;

void start(void) {
    for (int i = 0; i < 5; i++) fns[i](words[(i + 1) / 2]);
    out("loaded at a different base: ");
    out((unsigned long long)&__ImageBase == (unsigned long long)GetModuleHandleA(0) ? "consistent\n" : "INCONSISTENT\n");
    ExitProcess(0);
}
