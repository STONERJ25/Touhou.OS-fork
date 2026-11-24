/* Imports a function winrun does not have (WinrunNeverHasThis) and never calls it: the program runs to its end. */
#include "win.h"

IMPORT void WinrunNeverHasThis(void);
void *volatile keep;

void start(void) {
    keep = (void *)&WinrunNeverHasThis;
    out("unused import: fine\n");
    ExitProcess(0);
}
