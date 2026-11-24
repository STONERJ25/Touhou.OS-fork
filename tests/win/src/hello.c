/* W0: the first Windows program on TouhouOS. WriteFile to the standard output handle, then ExitProcess(0). */
#include "win.h"

void start(void) {
    out("Hello from a Windows program on TouhouOS!\n");
    ExitProcess(0);
}
