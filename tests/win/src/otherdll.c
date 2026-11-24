/* Imports from USER32.dll, which winrun does not build in: refused at load time (status 126), nothing runs. */
#include "win.h"

IMPORT int MessageBoxA(void *w, const char *text, const char *caption, unsigned type);

void start(void) {
    out("this must not be printed\n");
    MessageBoxA(0, "x", "y", 0);
    ExitProcess(0);
}
