/* Calls a function winrun does not have: winrun names it on standard error and ends the process with 127. */
#include "win.h"

IMPORT void WinrunNeverHasThis(void);

void start(void) {
    out("before the call\n");
    WinrunNeverHasThis();
    out("after the call (must not be printed)\n");
    ExitProcess(0);
}
