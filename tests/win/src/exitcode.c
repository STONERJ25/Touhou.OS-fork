/* ExitProcess(42): the status reaches the shell's $?. */
#include "win.h"

void start(void) {
    ExitProcess(42);
}
